// Prepared by ChatGPT for human review.
#include "library/rekordbox/rekordboxxmlimporter.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <cmath>
#include <limits>

#include "library/coverart.h"
#include "library/dao/cuedao.h"
#include "track/beats.h"
#include "track/cue.h"
#include "track/keyfactory.h"
#include "track/keys.h"
#include "util/color/predefinedcolorpalettes.h"
#include "util/db/sqlite.h"
#include "util/fpclassify.h"

namespace mixxx::rekordbox {
namespace {

void setError(QString* error, const QString& text) {
    if (error) {
        *error = text;
    }
}

bool run(QSqlQuery* query, const QString& sql, const QVariantList& values, QString* error) {
    if (!query->prepare(sql)) {
        setError(error, query->lastError().text());
        return false;
    }
    for (const auto& value : values) {
        query->addBindValue(value);
    }
    if (!query->exec()) {
        setError(error, query->lastError().text());
        return false;
    }
    return true;
}

bool writeOne(const QSqlDatabase& db,
        const QString& sql,
        const QVariantList& values,
        QString* error) {
    QSqlQuery query(db);
    if (!run(&query, sql, values, error)) {
        return false;
    }
    if (query.numRowsAffected() != 1) {
        setError(error, QStringLiteral("Expected exactly one canonical row"));
        return false;
    }
    return true;
}

QJsonObject attributes(const QMap<QString, QString>& values) {
    QJsonObject result;
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        result.insert(it.key(), it.value());
    }
    return result;
}

const QMap<QString, QString>& metadataColumns() {
    static const QMap<QString, QString> result{
            {QStringLiteral("Name"), QStringLiteral("title")},
            {QStringLiteral("Artist"), QStringLiteral("artist")},
            {QStringLiteral("Album"), QStringLiteral("album")},
            {QStringLiteral("AlbumArtist"), QStringLiteral("album_artist")},
            {QStringLiteral("Composer"), QStringLiteral("composer")},
            {QStringLiteral("Genre"), QStringLiteral("genre")},
            {QStringLiteral("Comments"), QStringLiteral("comment")},
            {QStringLiteral("Grouping"), QStringLiteral("grouping")},
            {QStringLiteral("Year"), QStringLiteral("year")},
            {QStringLiteral("TrackNumber"), QStringLiteral("tracknumber")},
            {QStringLiteral("Rating"), QStringLiteral("rating")},
            {QStringLiteral("PlayCount"), QStringLiteral("timesplayed")},
            {QStringLiteral("DateAdded"), QStringLiteral("datetime_added")},
            {QStringLiteral("LastPlayed"), QStringLiteral("last_played_at")},
            {QStringLiteral("Colour"), QStringLiteral("color")}};
    return result;
}

ImportIssue loss(const QString& code, const QString& message, const QString& annotation = {}) {
    return {code, message, {}, annotation};
}

bool numericColumn(const QString& column) {
    return column == QLatin1String("rating") ||
            column == QLatin1String("timesplayed") ||
            column == QLatin1String("color");
}

std::optional<QJsonValue> metadataValue(
        const QString& name, const QString& raw, QVector<ImportIssue>* issues) {
    if (name == QLatin1String("Rating") || name == QLatin1String("PlayCount")) {
        bool ok = false;
        const auto value = raw.toLongLong(&ok);
        const bool rating = name == QLatin1String("Rating");
        if (!ok || value < 0 || value > std::numeric_limits<int>::max() ||
                (rating && (value > 255 || value % 51 != 0))) {
            issues->push_back(loss(QStringLiteral("invalid_numeric_metadata"),
                    QStringLiteral("Invalid %1 is retained without replacing "
                                   "its canonical value")
                            .arg(name),
                    name));
            return {};
        }
        return QJsonValue(static_cast<int>(rating ? value / 51 : value));
    }
    if (name == QLatin1String("Colour")) {
        static const QRegularExpression rgb(QStringLiteral("^(?:#|0[xX])([0-9A-Fa-f]{6})$"));
        const auto match = rgb.match(raw);
        if (!match.hasMatch()) {
            issues->push_back(loss(QStringLiteral("unsupported_track_color"),
                    QStringLiteral(
                            "Track Colour needs an unambiguous #RRGGBB or "
                            "0xRRGGBB value; palette numbers are not guessed"),
                    name));
            return {};
        }
        return QJsonValue(match.captured(1).toInt(nullptr, 16));
    }
    if (name == QLatin1String("DateAdded") || name == QLatin1String("LastPlayed")) {
        return canonicalTimestampValue(raw, name, issues, name != QLatin1String("DateAdded"));
    }
    return QJsonValue(raw);
}

QJsonObject keysValue(const Keys& keys) {
    return {{QStringLiteral("version"), keys.getVersion()},
            {QStringLiteral("subVersion"), keys.getSubVersion()},
            {QStringLiteral("data"), QString::fromLatin1(keys.toByteArray().toBase64())},
            {QStringLiteral("text"), keys.getGlobalKeyText()},
            {QStringLiteral("id"), static_cast<int>(keys.getGlobalKey())},
            {QStringLiteral("tuning"), keys.getGlobalTuningFrequencyHz()}};
}

std::optional<QJsonObject> artworkValue(const Track& track, QVector<ImportIssue>* issues) {
    if (!track.attributes.contains(QStringLiteral("Artwork")) &&
            !track.attributes.contains(QStringLiteral("ArtworkPath"))) {
        return {};
    }
    const auto artwork = track.attributes.value(QStringLiteral("Artwork"));
    const auto path = track.attributes.value(QStringLiteral("ArtworkPath"));
    if (!artwork.isEmpty() && !path.isEmpty() && artwork != path) {
        issues->push_back(loss(QStringLiteral("ambiguous_artwork"),
                QStringLiteral("Conflicting artwork references require explicit resolution"),
                QStringLiteral("Artwork")));
        return {};
    }
    const auto raw = artwork.isEmpty() ? path : artwork;
    return canonicalArtworkValue(raw, issues);
}

QJsonObject mediaValue(const XmlCanonicalTarget& target) {
    return {{QStringLiteral("location"), target.media.location},
            {QStringLiteral("sampleRate"), target.media.sampleRate},
            {QStringLiteral("channels"), target.media.channels}};
}

// Decimal source seconds are converted once to rounded PCM frames. Values
// requiring more than nanosecond precision are refused, never channel-scaled.
std::optional<qint64> frameFromDecimal(const QString& text, int rate) {
    static const QRegularExpression pattern(QStringLiteral("^\\+?([0-9]+)(?:\\.([0-9]*))?$"));
    const auto match = pattern.match(text);
    if (!match.hasMatch() || rate <= 0) {
        return {};
    }
    bool ok = false;
    const auto whole = match.captured(1).toLongLong(&ok);
    if (!ok || whole > 9007199254740991LL / rate) {
        return {};
    }
    QString fraction = match.captured(2);
    while (fraction.endsWith(QLatin1Char('0'))) {
        fraction.chop(1);
    }
    if (fraction.size() > 9) {
        return {};
    }
    qint64 denominator = 1;
    for (qsizetype i = 0; i < fraction.size(); ++i) {
        denominator *= 10;
    }
    const qint64 numerator = fraction.isEmpty() ? 0 : fraction.toLongLong();
    const qint64 frames = whole * rate + (numerator * rate + denominator / 2) / denominator;
    return frames <= 9007199254740991LL ? std::optional<qint64>(frames) : std::nullopt;
}

std::optional<qint64> frameFromAttributes(const QMap<QString, QString>& attrs,
        const QString& name,
        int rate) {
    return frameFromDecimal(attrs.value(name), rate);
}

std::optional<QJsonObject> normalizedGrid(const Track& track,
        int rate,
        double duration,
        QVector<ImportIssue>* issues) {
    if (track.beatgrid.isEmpty()) {
        return {};
    }
    std::vector<BeatMarker> markers;
    std::vector<qint64> positions;
    for (const auto& segment : track.beatgrid) {
        const auto position = frameFromAttributes(
                segment.attributes, QStringLiteral("Inizio"), rate);
        if (!position || !util_isfinite(segment.bpm) || segment.bpm <= 0 ||
                segment.meterNumerator != 4 || segment.meterDenominator != 4 ||
                !segment.beatInBar || *segment.beatInBar < 1 || *segment.beatInBar > 4) {
            issues->push_back(loss(QStringLiteral("grid_capability"),
                    QStringLiteral("Grid needs finite tempo, decimal times, "
                                   "explicit 4/4 meter and beat-in-bar"),
                    QStringLiteral("grid")));
            return {};
        }
        if (*position > std::numeric_limits<qint32>::max()) {
            issues->push_back(loss(QStringLiteral("grid_position_range"),
                    QStringLiteral(
                            "Grid anchors exceed the canonical beat "
                            "serialization's signed 32-bit PCM frame range"),
                    QStringLiteral("grid")));
            return {};
        }
        positions.push_back(*position);
    }
    qint64 beatCount = 0;
    const int firstPhase = *track.beatgrid.front().beatInBar - 1;
    for (qsizetype i = 0; i + 1 < track.beatgrid.size(); ++i) {
        const auto& segment = track.beatgrid[i];
        const double count = (positions[i + 1] - positions[i]) * segment.bpm / (60.0 * rate);
        if (!util_isfinite(count) || count < 1 || count > std::numeric_limits<int>::max()) {
            issues->push_back(loss(QStringLiteral("grid_interval"),
                    QStringLiteral("Tempo segment cannot be represented as a "
                                   "bounded beat interval"),
                    QStringLiteral("grid")));
            return {};
        }
        const auto rounded = std::llround(count);
        const double predicted = rounded * 60.0 * rate / segment.bpm;
        if (std::abs(predicted - (positions[i + 1] - positions[i])) > 1.0 ||
                (firstPhase + beatCount + rounded) % 4 != *track.beatgrid[i + 1].beatInBar - 1) {
            issues->push_back(loss(QStringLiteral("grid_phase_discontinuity"),
                    QStringLiteral("Nonintegral beat count or discontinuous "
                                   "phase cannot be applied faithfully"),
                    QStringLiteral("grid")));
            return {};
        }
        markers.emplace_back(audio::FramePos(static_cast<double>(positions[i])),
                static_cast<int>(rounded));
        beatCount += rounded;
    }
    const auto beats = Beats::fromBeatMarkers(audio::SampleRate(rate),
            markers,
            audio::FramePos(static_cast<double>(positions.back())),
            Bpm(track.beatgrid.back().bpm),
            QStringLiteral("rekordbox-xml-v2"),
            (4 - firstPhase) % 4);
    if (!beats) {
        issues->push_back(loss(QStringLiteral("grid_construction"),
                QStringLiteral("Could not construct grid")));
        return {};
    }
    const auto average = beats->getBpmInRange(audio::kStartFramePos,
            audio::FramePos(std::max(duration * rate,
                    static_cast<double>(positions.back() + rate))));
    return QJsonObject{{QStringLiteral("version"), beats->getVersion()},
            {QStringLiteral("subVersion"), beats->getSubVersion()},
            {QStringLiteral("data"), QString::fromLatin1(beats->toByteArray().toBase64())},
            {QStringLiteral("bpm"), average.value()},
            {QStringLiteral("locked"), true}};
}

QString memoryTarget(const QString& sourceId, const QString& sourceTrackId, int ordinal) {
    const QJsonArray key{sourceId, sourceTrackId, ordinal};
    const auto hash = QCryptographicHash::hash(QJsonDocument(key).toJson(QJsonDocument::Compact),
            QCryptographicHash::Sha256)
                              .toHex();
    return QStringLiteral("cue:memory:") + QString::fromLatin1(hash);
}

bool cueRow(const QSqlDatabase& db,
        qint64 trackId,
        const QString& target,
        QSqlQuery* query,
        QString* error) {
    if (target.startsWith(QLatin1String("cue:hot:"))) {
        bool ok = false;
        const int slot = target.mid(8).toInt(&ok);
        if (!ok || slot < 0 || slot > 63) {
            setError(error, QStringLiteral("Invalid hotcue target"));
            return false;
        }
        return run(query,
                QStringLiteral(
                        "SELECT id,type,position,length,hotcue,label,color "
                        "FROM cues "
                        "WHERE track_id=? AND hotcue=? ORDER BY id"),
                {trackId, slot},
                error);
    }
    if (target.startsWith(QLatin1String("cue:memory:"))) {
        Q_UNUSED(db);
        return run(query,
                QStringLiteral(
                        "SELECT "
                        "c.id,c.type,c.position,c.length,c.hotcue,c.label,c."
                        "color "
                        "FROM cues c JOIN rekordbox_import_cue_bindings_v1 b "
                        "ON b.cue_id=c.id "
                        "WHERE b.canonical_track_id=? AND b.target_field=? AND "
                        "c.track_id=?"),
                {trackId, target, trackId},
                error);
    }
    setError(error, QStringLiteral("Unknown canonical cue target"));
    return false;
}

QJsonObject cueValue(const QSqlQuery& query) {
    const double start = audio::FramePos::fromEngineSamplePosMaybeInvalid(
            query.value(2).toDouble())
                                 .value();
    const double length = query.value(3).toDouble() / kEngineChannelOutputCount;
    return {{QStringLiteral("type"), query.value(1).toInt()},
            {QStringLiteral("start"), start},
            {QStringLiteral("end"),
                    length > 0 ? QJsonValue(start + length)
                               : QJsonValue(QJsonValue::Null)},
            {QStringLiteral("hotCue"), query.value(4).toInt()},
            {QStringLiteral("label"), query.value(5).toString()},
            {QStringLiteral("color"),
                    static_cast<int>(
                            RgbColor::validateCode(query.value(6).toUInt()))}};
}

QString inputKey(const Library& library,
        int index,
        const QString& sourceTrackId,
        const QString& fingerprint) {
    const auto& track = library.tracks[index];
    const QJsonObject input{
            {QStringLiteral("decoder"), QStringLiteral("xml-normalizer-2.3")},
            {QStringLiteral("profile"),
                    QStringLiteral("track-sql-4/4-no-external-v1")},
            {QStringLiteral("sourceTrackId"), sourceTrackId},
            {QStringLiteral("fingerprint"), fingerprint},
            {QStringLiteral("format"), library.version},
            {QStringLiteral("product"), library.product},
            {QStringLiteral("productVersion"), library.productVersion},
            {QStringLiteral("location"), track.location},
            {QStringLiteral("annotations"),
                    QString::fromUtf8(serializeTrackAnnotations(track))},
            {QStringLiteral("keyProjection"),
                    track.attributes.contains(QStringLiteral("Tonality"))
                            ? QJsonValue(
                                      canonicalKeyValue(track.attributes.value(
                                              QStringLiteral("Tonality"))))
                            : QJsonValue(QJsonValue::Null)},
            {QStringLiteral("diagnostics"),
                    QString::fromLatin1(
                            library.diagnosticsFingerprint().toHex())}};
    return QString::fromLatin1(QCryptographicHash::hash(
            QJsonDocument(input).toJson(QJsonDocument::Compact),
            QCryptographicHash::Sha256)
                    .toHex());
}

QString policyJson(const ImportPolicy& policy) {
    const QJsonObject object{{QStringLiteral("revision"), policy.revision},
            {QStringLiteral("replace_edited_source_owned"), policy.replaceEditedSourceOwned},
            {QStringLiteral("accept_preserved_conflicts"), policy.acceptPreservedConflicts}};
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

} // namespace

QJsonObject canonicalKeyValue(const QString& text) {
    return keysValue(KeyFactory::makeBasicKeysKeepText(text, mixxx::track::io::key::USER));
}

std::optional<QJsonObject> canonicalArtworkValue(const QString& reference,
        QVector<ImportIssue>* issues) {
    const QString resolved = reference.isEmpty() ? QStringLiteral("")
                                                 : normalizeLocation(reference);
    if (!reference.isEmpty() && (resolved.isEmpty() || !QFileInfo(resolved).isAbsolute())) {
        if (issues) {
            issues->push_back(loss(
                    QStringLiteral("unsupported_artwork_reference"),
                    QStringLiteral(
                            "Relative or remote artwork needs source-specific "
                            "resolution before canonical application"),
                    QStringLiteral("Artwork")));
        }
        return {};
    }
    return QJsonObject{
            {QStringLiteral("source"), CoverInfoRelative::USER_SELECTED},
            {QStringLiteral("type"),
                    reference.isEmpty() ? CoverInfoRelative::NONE
                                        : CoverInfoRelative::FILE},
            {QStringLiteral("location"), resolved}};
}

std::optional<QJsonValue> canonicalTimestampValue(const QString& text,
        const QString& annotation,
        QVector<ImportIssue>* issues,
        bool allowEmpty) {
    if (text.isEmpty()) {
        if (!allowEmpty) {
            if (issues) {
                issues->push_back(loss(
                        QStringLiteral("empty_required_timestamp"),
                        QStringLiteral(
                                "An empty %1 remains source metadata; the "
                                "required canonical timestamp is preserved")
                                .arg(annotation),
                        annotation));
            }
            return {};
        }
        return QJsonValue(QStringLiteral(""));
    }
    const auto addLoss = [&](const QString& code, const QString& message) {
        if (issues) {
            issues->push_back(loss(code, message, annotation));
        }
    };
    const auto date = QDate::fromString(text, Qt::ISODate);
    if (text.size() == 10 && date.isValid()) {
        addLoss(QStringLiteral("date_precision"),
                QStringLiteral("%1 supplies a calendar date only; the "
                               "canonical timestamp uses UTC midnight, "
                               "retaining the original date in provenance")
                        .arg(annotation));
        return QJsonValue(date.toString(QStringLiteral("yyyy-MM-dd")) +
                QStringLiteral(" 00:00:00"));
    }
    static const QRegularExpression zone(QStringLiteral("(?:[zZ]|[+-][0-9]{2}:[0-9]{2})$"));
    const auto timestamp = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!timestamp.isValid() || !zone.match(text).hasMatch()) {
        addLoss(QStringLiteral("unsupported_timestamp"),
                QStringLiteral("%1 requires a valid ISO timestamp with an "
                               "explicit timezone")
                        .arg(annotation));
        return {};
    }
    if (timestamp.time().msec() != 0) {
        addLoss(QStringLiteral("timestamp_precision"),
                QStringLiteral("%1 is stored at the canonical timestamp's "
                               "one-second precision")
                        .arg(annotation));
    }
    return QJsonValue(sqlite::writeGeneratedTimestamp(timestamp).toString());
}

XmlAdmissionResult validateXmlTrackAnnotations(const QByteArray& annotations) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(annotations, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return {ImportOutcome::Invalid,
                {loss(QStringLiteral("invalid_sidecar"), error.errorString())}};
    }
    const auto object = document.object();
    if (object.value(QStringLiteral("schemaVersion")) != 2 ||
            object.value(QStringLiteral("gridKind")) != QLatin1String("tempo-segments")) {
        return {ImportOutcome::Unsupported,
                {loss(QStringLiteral("unsupported_sidecar"),
                        QStringLiteral("This importer requires schema 2 "
                                       "tempo-segment annotations"))}};
    }
    if (!object.value(QStringLiteral("beatgrid")).isArray() ||
            !object.value(QStringLiteral("cues")).isArray() ||
            !object.value(QStringLiteral("fields")).isObject()) {
        return {ImportOutcome::Invalid,
                {loss(QStringLiteral("invalid_sidecar_shape"),
                        QStringLiteral("Annotation arrays and source-field "
                                       "presence are required"))}};
    }
    QVector<ImportIssue> issues;
    for (const auto& value : object.value(QStringLiteral("beatgrid")).toArray()) {
        const auto segment = value.toObject();
        const double position = segment.value(QStringLiteral("position")).toDouble(-1);
        const double bpm = segment.value(QStringLiteral("bpm")).toDouble(-1);
        if (!value.isObject() || !util_isfinite(position) || position < 0 ||
                !util_isfinite(bpm) || bpm <= 0 ||
                !segment.value(QStringLiteral("attributes")).isObject()) {
            return {ImportOutcome::Invalid,
                    {loss(QStringLiteral("invalid_tempo_payload"),
                            QStringLiteral(
                                    "Tempo segments need finite times, "
                                    "positive tempo and source attributes"))}};
        }
        if (segment.value(QStringLiteral("meterNumerator")) != 4 ||
                segment.value(QStringLiteral("meterDenominator")) != 4 ||
                segment.value(QStringLiteral("beatInBar")).toInt() < 1 ||
                segment.value(QStringLiteral("beatInBar")).toInt() > 4) {
            issues.push_back(loss(QStringLiteral("grid_capability"),
                    QStringLiteral("This grid needs an explicit supported meter and phase")));
        }
    }
    for (const auto& value : object.value(QStringLiteral("cues")).toArray()) {
        const auto cue = value.toObject();
        const double start = cue.value(QStringLiteral("start")).toDouble(-1);
        const auto type = cue.value(QStringLiteral("type")).toString();
        if (!value.isObject() || !util_isfinite(start) || start < 0 ||
                !cue.value(QStringLiteral("attributes")).isObject() ||
                !cue.value(QStringLiteral("supported")).isBool()) {
            return {ImportOutcome::Invalid,
                    {loss(QStringLiteral("invalid_cue_payload"),
                            QStringLiteral(
                                    "Cue records need finite times and "
                                    "explicit source support information"))}};
        }
        if (type == QLatin1String("loop") &&
                cue.value(QStringLiteral("end")).toDouble(-1) <= start) {
            return {ImportOutcome::Invalid,
                    {loss(QStringLiteral("invalid_loop_payload"),
                            QStringLiteral("Loop end must follow its start"))}};
        }
        const auto hot = cue.value(QStringLiteral("hotCue"));
        if (!cue.value(QStringLiteral("supported")).toBool() ||
                (type != QLatin1String("cue") &&
                        type != QLatin1String("loop")) ||
                (!hot.isUndefined() &&
                        (!hot.isDouble() || hot.toDouble() != hot.toInt(-1) ||
                                hot.toInt(-1) < 0 || hot.toInt(-1) > 63))) {
            issues.push_back(loss(QStringLiteral("cue_capability"),
                    QStringLiteral("A cue role or hot slot is not supported by "
                                   "this application profile")));
        }
    }
    return {issues.isEmpty() ? ImportOutcome::Success : ImportOutcome::Warning, issues};
}

XmlAdmissionResult validateXmlTrackImport(const Library& library, int trackIndex) {
    if (library.hasErrors() || trackIndex < 0 || trackIndex >= library.tracks.size()) {
        return {ImportOutcome::Invalid,
                {loss(QStringLiteral("invalid_xml_track"),
                        QStringLiteral("A complete valid decoded collection "
                                       "and track index are required"))}};
    }
    const auto& track = library.tracks[trackIndex];
    if (track.location.isEmpty()) {
        return {ImportOutcome::Unavailable,
                {loss(QStringLiteral("unresolved_media"),
                        QStringLiteral("The source track has no resolved media "
                                       "location"))}};
    }
    if (library.sourceIdentityMatches(trackIndex) != 1) {
        return {ImportOutcome::Unsupported,
                {loss(QStringLiteral("ambiguous_source_track"),
                        QStringLiteral("Ambiguous source identity needs an "
                                       "explicit source mapping"))}};
    }
    QVector<ImportIssue> issues;
    const QSet<QString> physical{QStringLiteral("TrackID"),
            QStringLiteral("Location"),
            QStringLiteral("TotalTime"),
            QStringLiteral("BitRate"),
            QStringLiteral("SampleRate"),
            QStringLiteral("Kind"),
            QStringLiteral("Size"),
            QStringLiteral("AverageBpm")};
    for (auto it = track.attributes.cbegin(); it != track.attributes.cend(); ++it) {
        if (!metadataColumns().contains(it.key()) && !physical.contains(it.key()) &&
                it.key() != QLatin1String("Tonality") && it.key() != QLatin1String("Artwork") &&
                it.key() != QLatin1String("ArtworkPath")) {
            issues.push_back(loss(QStringLiteral("unsupported_metadata"),
                    QStringLiteral("Source field %1 remains source metadata "
                                   "and is not applied")
                            .arg(it.key()),
                    it.key()));
        }
    }
    if (track.beatgrid.isEmpty() && track.bpm > 0) {
        issues.push_back(loss(QStringLiteral("bpm_without_grid"),
                QStringLiteral("Average BPM alone does not supply a grid anchor or phase"),
                QStringLiteral("grid")));
    }
    for (const auto& cue : track.cues) {
        if (!cue.supported ||
                (cue.type != QLatin1String("cue") &&
                        cue.type != QLatin1String("loop")) ||
                (cue.hotCue && *cue.hotCue > 63)) {
            issues.push_back(loss(QStringLiteral("unsupported_cue"),
                    QStringLiteral("This cue role or hot slot cannot be "
                                   "applied without losing its meaning"),
                    QString::number(cue.sourceOrdinal)));
        }
        if (!cue.number) {
            issues.push_back(loss(QStringLiteral("cue_bank_unspecified"),
                    QStringLiteral("A cue without Num is retained as a memory "
                                   "annotation; no hot slot is inferred"),
                    QString::number(cue.sourceOrdinal)));
        }
        if (cue.hasEnd && cue.type != QLatin1String("loop")) {
            issues.push_back(loss(QStringLiteral("point_cue_end"),
                    QStringLiteral("End on a point cue remains source metadata and is not applied"),
                    QString::number(cue.sourceOrdinal)));
        }
        const QSet<QString> recognized{QStringLiteral("Name"),
                QStringLiteral("Type"),
                QStringLiteral("Start"),
                QStringLiteral("End"),
                QStringLiteral("Num"),
                QStringLiteral("Red"),
                QStringLiteral("Green"),
                QStringLiteral("Blue")};
        for (auto it = cue.attributes.cbegin(); it != cue.attributes.cend(); ++it) {
            if (!recognized.contains(it.key())) {
                issues.push_back(
                        loss(QStringLiteral("unsupported_cue_attribute"),
                                QStringLiteral("Cue attribute %1 is retained "
                                               "in provenance, not applied")
                                        .arg(it.key()),
                                QString::number(cue.sourceOrdinal)));
            }
        }
    }
    return {issues.isEmpty() ? ImportOutcome::Success : ImportOutcome::Warning, issues};
}

std::optional<qint64> currentXmlTrackRevision(const ImportSourceRegistry& registry,
        const Library& library,
        int trackIndex,
        const QString& sourceTrackId,
        const ImportTicket& ticket,
        const QString& fingerprint,
        const ImportPolicy& policy,
        QString* error) {
    setError(error, {});
    const auto admission = validateXmlTrackImport(library, trackIndex);
    if ((admission.outcome != ImportOutcome::Success &&
                admission.outcome != ImportOutcome::Warning) ||
            sourceTrackId.isEmpty() ||
            sourceTrackId.startsWith(QLatin1String("record:")) ||
            fingerprint.isEmpty()) {
        return {};
    }
    QSqlQuery query(registry.database());
    if (!run(&query,
                QStringLiteral("SELECT m.canonical_track_id FROM "
                               "rekordbox_import_track_revisions_v1 r "
                               "JOIN rekordbox_import_sources_v1 s ON "
                               "s.source_id=r.source_id "
                               "JOIN rekordbox_import_tracks_v1 m ON "
                               "m.source_id=r.source_id AND "
                               "m.source_track_id=r.source_track_id "
                               "JOIN library l ON l.id=m.canonical_track_id "
                               "JOIN track_locations t ON t.id=l.location "
                               "WHERE r.source_id=? AND r.source_track_id=? "
                               "AND s.requested_generation=? "
                               "AND r.fingerprint=? AND r.input_key=? AND "
                               "r.policy_json=? AND t.location=? "
                               "AND COALESCE(l.mixxx_deleted,0)=0 AND l.header_parsed=1"),
                {ticket.sourceId,
                        sourceTrackId,
                        ticket.generation,
                        fingerprint,
                        inputKey(library,
                                trackIndex,
                                sourceTrackId,
                                fingerprint),
                        policyJson(policy),
                        library.tracks[trackIndex].location},
                error)) {
        return {};
    }
    return query.next() ? std::optional<qint64>(query.value(0).toLongLong()) : std::nullopt;
}

XmlAdmissionResult prepareCanonicalTarget(const QSqlDatabase& database,
        const XmlMediaFacts& media,
        const XmlCanonicalAccess& access,
        XmlCanonicalTarget* target) {
    const auto fail = [](ImportOutcome outcome, const QString& code, const QString& message) {
        return XmlAdmissionResult{outcome, {loss(code, message)}};
    };
    if (!target || !access.lease || !access.check || !access.externalCollectionsAbsent) {
        return fail(ImportOutcome::Denied,
                QStringLiteral("canonical_access"),
                QStringLiteral("An exclusive cache lease and an empty "
                               "external-collection set are required"));
    }
    *target = XmlCanonicalTarget{};
    if (!QFileInfo(media.location).isAbsolute() || media.sampleRate <= 0 ||
            media.sampleRate > 768000 || media.channels <= 0 ||
            !util_isfinite(media.duration) || media.duration < 0 ||
            media.bitrate < 0 || media.fileSize < 0 ||
            !media.sourceSynchronizedAt.isValid() ||
            media.sourceSynchronizedAt.timeSpec() != Qt::UTC ||
            media.sourceSynchronizedAt.toMSecsSinceEpoch() <= 0) {
        return fail(ImportOutcome::Unavailable,
                QStringLiteral("media_probe"),
                QStringLiteral("Actual local media facts and a successful UTC "
                               "metadata synchronization timestamp are "
                               "required before canonical admission"));
    }
    QString error;
    QSqlQuery existing(database);
    if (!run(&existing,
                QStringLiteral("SELECT "
                               "l.id,l.location,l.samplerate,l.channels,l."
                               "mixxx_deleted,l.header_parsed,l.source_synchronized_ms "
                               "FROM library l JOIN track_locations t ON "
                               "l.location=t.id WHERE t.location=?"),
                {media.location},
                &error)) {
        return fail(ImportOutcome::Failed, QStringLiteral("media_lookup"), error);
    }
    target->media = media;
    if (existing.next()) {
        target->trackId = existing.value(0).toLongLong();
        target->locationId = existing.value(1).toLongLong();
        if (target->trackId <= 0 || target->trackId > std::numeric_limits<int>::max()) {
            return fail(ImportOutcome::Unsupported,
                    QStringLiteral("canonical_id_range"),
                    QStringLiteral(
                            "Canonical identity is outside the TrackId range"));
        }
        if (existing.value(4).toBool()) {
            return fail(ImportOutcome::Denied,
                    QStringLiteral("hidden_track"),
                    QStringLiteral("The canonical track is hidden; source "
                                   "import does not unhide it"));
        }
        if (!existing.value(5).toBool()) {
            return fail(ImportOutcome::Unavailable,
                    QStringLiteral("canonical_metadata_unparsed"),
                    QStringLiteral("The existing canonical track needs a "
                                   "successful metadata refresh before "
                                   "importing source annotations"));
        }
        if (!existing.value(6).isNull()) {
            bool validStamp = false;
            const auto previous = existing.value(6).toLongLong(&validStamp);
            if (!validStamp || previous <= 0 ||
                    previous < media.sourceSynchronizedAt.toMSecsSinceEpoch()) {
                return fail(ImportOutcome::Unavailable,
                        QStringLiteral("canonical_metadata_outdated"),
                        QStringLiteral("Canonical metadata is older than the "
                                       "observed media; refresh it before "
                                       "importing source annotations"));
            }
            target->previousSourceSynchronizedMs = previous;
        }
        if (existing.value(2).toInt() != media.sampleRate ||
                existing.value(3).toInt() != media.channels) {
            return fail(ImportOutcome::Unavailable,
                    QStringLiteral("media_changed"),
                    QStringLiteral(
                            "Canonical and probed media facts differ; refresh "
                            "the media before importing annotations"));
        }
        if (existing.next()) {
            return fail(ImportOutcome::Invalid,
                    QStringLiteral("ambiguous_canonical_track"),
                    QStringLiteral("Several canonical tracks refer to this "
                                   "media location"));
        }
    } else {
        target->createTrack = true;
        const auto reserve = [&](const QString& table) -> qint64 {
            QSqlQuery query(database);
            if (!run(&query,
                        QStringLiteral("SELECT MAX(n)+1 FROM (SELECT "
                                       "COALESCE(MAX(id),0) AS n FROM %1 "
                                       "UNION ALL SELECT seq AS n FROM "
                                       "sqlite_sequence WHERE name=?)")
                                .arg(table),
                        {table},
                        &error) ||
                    !query.next()) {
                return 0;
            }
            const auto id = query.value(0).toLongLong();
            return id > 0 && id <= std::numeric_limits<int>::max() ? id : 0;
        };
        target->trackId = reserve(QStringLiteral("library"));
        QSqlQuery location(database);
        if (!run(&location,
                    QStringLiteral(
                            "SELECT id FROM track_locations WHERE location=?"),
                    {media.location},
                    &error)) {
            return fail(ImportOutcome::Failed, QStringLiteral("location_lookup"), error);
        }
        if (location.next()) {
            target->locationId = location.value(0).toLongLong();
        } else {
            target->createLocation = true;
            target->locationId = reserve(QStringLiteral("track_locations"));
        }
        if (target->trackId <= 0 || target->locationId <= 0) {
            return fail(ImportOutcome::Failed,
                    QStringLiteral("identity_reservation"),
                    error.isEmpty()
                            ? QStringLiteral("Canonical ID space is exhausted")
                            : error);
        }
    }
    if (!access.check(target->trackId, media.location, &error)) {
        return fail(ImportOutcome::Denied, QStringLiteral("cached_track"), error);
    }
    return {ImportOutcome::Success, {}};
}

ImportField canonicalMediaAdmission(const XmlCanonicalTarget& target,
        const QString& sourceTrackId,
        const QJsonObject& provenance) {
    auto details = provenance;
    details.insert(QStringLiteral("observedSourceSynchronizedMs"),
            QString::number(target.media.sourceSynchronizedAt.toMSecsSinceEpoch()));
    return {sourceTrackId,
            QStringLiteral("media"),
            QStringLiteral("media"),
            mediaValue(target),
            false,
            details};
}

XmlPreparedTrack prepareXmlTrackImport(const QSqlDatabase& database,
        const std::shared_ptr<const Library>& library,
        int trackIndex,
        const QString& sourceTrackId,
        const ImportTicket& ticket,
        const QString& fingerprint,
        const XmlMediaFacts& media,
        const XmlCanonicalAccess& access) {
    XmlPreparedTrack result;
    const auto fail = [&](ImportOutcome outcome, const QString& code, const QString& message) {
        result.outcome = outcome;
        result.snapshot.reset();
        result.issues.push_back(loss(code, message));
        return result;
    };
    if (!library) {
        return fail(ImportOutcome::Invalid,
                QStringLiteral("missing_snapshot"),
                QStringLiteral("No frozen XML snapshot"));
    }
    const auto admission = validateXmlTrackImport(*library, trackIndex);
    result.issues = admission.issues;
    if (admission.outcome != ImportOutcome::Success &&
            admission.outcome != ImportOutcome::Warning) {
        result.outcome = admission.outcome;
        return result;
    }
    if (!access.lease || !access.check || !access.externalCollectionsAbsent) {
        return fail(ImportOutcome::Denied,
                QStringLiteral("canonical_access"),
                QStringLiteral("An exclusive cache lease and an empty "
                               "external-collection set are required"));
    }
    const auto& track = library->tracks[trackIndex];
    if (sourceTrackId.startsWith(QLatin1String("record:"))) {
        return fail(ImportOutcome::Unsupported,
                QStringLiteral("display_only_identity"),
                QStringLiteral("An ambiguous display record cannot own "
                               "canonical annotations"));
    }
    if (sourceTrackId.isEmpty() ||
            ticket.sourceId.isEmpty() || ticket.generation <= 0 || fingerprint.isEmpty()) {
        return fail(ImportOutcome::Invalid,
                QStringLiteral("source_identity"),
                QStringLiteral("An unambiguous registered source track and "
                               "current generation are required"));
    }
    if (media.location != track.location ||
            !QFileInfo(media.location).isAbsolute() || media.sampleRate <= 0 ||
            media.sampleRate > 768000 || media.channels <= 0 ||
            !util_isfinite(media.duration) || media.duration < 0 ||
            media.bitrate < 0 || media.fileSize < 0) {
        return fail(ImportOutcome::Unavailable,
                QStringLiteral("media_probe"),
                QStringLiteral("Actual local media facts must match the "
                               "selected source track"));
    }
    const auto canonicalAdmission = prepareCanonicalTarget(database, media, access, &result.target);
    if (canonicalAdmission.outcome != ImportOutcome::Success) {
        result.outcome = canonicalAdmission.outcome;
        result.issues += canonicalAdmission.issues;
        return result;
    }
    QString error;
    ImportSnapshot snapshot;
    snapshot.ticket = ticket;
    snapshot.fingerprint = fingerprint;
    snapshot.decoderRevision = QStringLiteral("xml-normalizer-2.3");
    snapshot.formatVersion = library->version;
    snapshot.capabilities = {QStringLiteral("xml-schema-2"),
            QStringLiteral("track-scoped-sql"),
            QStringLiteral("memory-and-hot-cues"),
            QStringLiteral("phase-bearing-4/4-grid"),
            QStringLiteral("no-external-collections")};
    snapshot.scope = ImportScope::TracksOnly;
    snapshot.inputKey = inputKey(*library, trackIndex, sourceTrackId, fingerprint);
    struct Lease {
        std::shared_ptr<const Library> library;
        std::shared_ptr<const void> access;
    };
    snapshot.accessLease = std::make_shared<const Lease>(Lease{library, access.lease});
    snapshot.tracks.push_back({sourceTrackId,
            QStringLiteral("mixxx-location:%1").arg(result.target.locationId),
            result.target.trackId});
    const QJsonObject sourceAttributes = attributes(track.attributes);
    const auto add = [&](const QString& annotation,
                             const QString& target,
                             const QJsonValue& value,
                             const QJsonObject& detail = QJsonObject()) {
        snapshot.fields.push_back({sourceTrackId, annotation, target, value, false, detail});
    };
    snapshot.fields.push_back(canonicalMediaAdmission(result.target,
            sourceTrackId,
            {{QStringLiteral("sourceAttributes"), sourceAttributes}}));
    for (auto it = metadataColumns().cbegin(); it != metadataColumns().cend(); ++it) {
        if (!track.attributes.contains(it.key())) {
            continue;
        }
        const auto value = metadataValue(
                it.key(), track.attributes.value(it.key()), &result.issues);
        if (!value) {
            continue;
        }
        add(QStringLiteral("metadata/") + it.key(),
                QStringLiteral("metadata:") + it.value(),
                *value,
                {{QStringLiteral("sourceAttributes"), sourceAttributes}});
    }
    if (track.attributes.contains(QStringLiteral("Tonality"))) {
        add(QStringLiteral("metadata/Tonality"),
                QStringLiteral("keys"),
                canonicalKeyValue(track.attributes.value(QStringLiteral("Tonality"))),
                {{QStringLiteral("sourceAttributes"), sourceAttributes}});
    }
    if (const auto artwork = artworkValue(track, &result.issues)) {
        add(QStringLiteral("metadata/Artwork"),
                QStringLiteral("artwork"),
                *artwork,
                {{QStringLiteral("sourceAttributes"), sourceAttributes}});
    }
    if (const auto grid = normalizedGrid(track, media.sampleRate, media.duration, &result.issues)) {
        add(QStringLiteral("grid"),
                QStringLiteral("beats"),
                *grid,
                {{QStringLiteral("source"),
                        QJsonDocument::fromJson(
                                serializeTrackAnnotations(track))
                                .object()}});
    }
    QSet<QString> suppliedCues;
    for (const auto& cue : track.cues) {
        const QString annotation = QStringLiteral("cue/%1").arg(cue.sourceOrdinal);
        suppliedCues.insert(annotation);
        if (!cue.supported ||
                (cue.type != QLatin1String("cue") &&
                        cue.type != QLatin1String("loop")) ||
                (cue.hotCue && *cue.hotCue > 63)) {
            continue;
        }
        const auto start = frameFromAttributes(
                cue.attributes, QStringLiteral("Start"), media.sampleRate);
        std::optional<qint64> end;
        if (cue.hasEnd) {
            end = frameFromAttributes(cue.attributes, QStringLiteral("End"), media.sampleRate);
        }
        const bool loop = cue.type == QLatin1String("loop");
        if (!start || (cue.hasEnd && !end) || (loop && (!end || *end <= *start))) {
            result.issues.push_back(loss(QStringLiteral("cue_time_precision"),
                    QStringLiteral("Cue times cannot be represented as nonnegative PCM frames"),
                    annotation));
            continue;
        }
        const int hot = cue.hotCue.value_or(-1);
        const auto type = loop ? CueType::Loop : hot >= 0 ? CueType::HotCue
                                                          : CueType::Memory;
        int color = static_cast<int>(loop
                        ? PredefinedColorPalettes::kDefaultLoopColor
                        : PredefinedColorPalettes::kDefaultCueColor);
        if (!cue.color.isEmpty()) {
            const auto rgb = cue.color.split(QLatin1Char(','));
            color = (rgb.value(0).toInt() << 16) | (rgb.value(1).toInt() << 8) |
                    rgb.value(2).toInt();
        }
        const QJsonObject value{
                {QStringLiteral("type"), static_cast<int>(type)},
                {QStringLiteral("start"), static_cast<double>(*start)},
                {QStringLiteral("end"),
                        loop ? QJsonValue(static_cast<double>(*end))
                             : QJsonValue(QJsonValue::Null)},
                {QStringLiteral("hotCue"), hot},
                {QStringLiteral("label"), cue.name},
                {QStringLiteral("color"), color}};
        add(annotation,
                hot >= 0 ? QStringLiteral("cue:hot:%1").arg(hot)
                         : memoryTarget(ticket.sourceId,
                                   sourceTrackId,
                                   cue.sourceOrdinal),
                value,
                {{QStringLiteral("sourceOrdinal"), cue.sourceOrdinal},
                        {QStringLiteral("sourceAttributes"),
                                attributes(cue.attributes)},
                        {QStringLiteral("colorPresent"),
                                !cue.color.isEmpty()}});
    }
    QSqlQuery prior(database);
    if (!run(&prior,
                QStringLiteral("SELECT annotation_id,target_field FROM "
                               "rekordbox_import_provenance_v1 "
                               "WHERE source_id=? AND source_track_id=?"),
                {ticket.sourceId, sourceTrackId},
                &error)) {
        return fail(ImportOutcome::Failed, QStringLiteral("previous_annotations"), error);
    }
    while (prior.next()) {
        const auto annotation = prior.value(0).toString();
        if (annotation.startsWith(QLatin1String("cue/")) && !suppliedCues.contains(annotation)) {
            snapshot.fields.push_back({sourceTrackId,
                    annotation,
                    prior.value(1).toString(),
                    QJsonValue(QJsonValue::Undefined),
                    true,
                    {{QStringLiteral("removedFromSource"), true}}});
        }
    }
    for (const auto& diagnostic : library->diagnostics) {
        result.issues.push_back(loss(QStringLiteral("xml_diagnostic"), diagnostic.message));
    }
    for (auto& item : result.issues) {
        item.sourceTrackId = sourceTrackId;
    }
    snapshot.decodeIssues = result.issues;
    snapshot.decodeOutcome = result.issues.isEmpty() ? ImportOutcome::Success
                                                     : ImportOutcome::Warning;
    result.outcome = snapshot.decodeOutcome;
    result.snapshot = std::move(snapshot);
    return result;
}

XmlCanonicalAdapter::XmlCanonicalAdapter(XmlCanonicalTarget target, XmlCanonicalAccess access)
        : m_target(std::move(target)),
          m_access(std::move(access)) {
}

bool XmlCanonicalAdapter::check(const QSqlDatabase& database,
        qint64 trackId,
        bool* exists,
        QString* error) const {
    if (trackId != m_target.trackId || !m_access.lease || !m_access.check ||
            !m_access.externalCollectionsAbsent ||
            !m_access.check(trackId, m_target.media.location, error)) {
        setError(error, QStringLiteral("Canonical cache access was not admitted"));
        return false;
    }
    QSqlQuery query(database);
    if (!run(&query,
                QStringLiteral("SELECT "
                               "l.location,t.location,l.samplerate,l.channels,"
                               "l.mixxx_deleted,l.header_parsed,l.source_synchronized_ms "
                               "FROM library l LEFT JOIN track_locations t ON "
                               "l.location=t.id WHERE l.id=?"),
                {trackId},
                error)) {
        return false;
    }
    *exists = query.next();
    if (*exists) {
        if (query.value(0).toLongLong() != m_target.locationId ||
                query.value(1).toString() != m_target.media.location ||
                query.value(2).toInt() != m_target.media.sampleRate ||
                query.value(3).toInt() != m_target.media.channels || query.value(4).toBool() ||
                !query.value(5).toBool()) {
            setError(error,
                    QStringLiteral("Canonical media identity or facts changed "
                                   "after admission"));
            return false;
        }
        const auto expectedStamp = m_target.createTrack
                ? std::optional<qint64>(m_target.media.sourceSynchronizedAt.toMSecsSinceEpoch())
                : m_target.previousSourceSynchronizedMs;
        bool validStamp = false;
        const auto currentStamp = query.value(6).toLongLong(&validStamp);
        if (query.value(6).isNull() != !expectedStamp ||
                (expectedStamp && (!validStamp || currentStamp != *expectedStamp))) {
            setError(error,
                    QStringLiteral("Canonical media synchronization state "
                                   "changed after admission"));
            return false;
        }
    } else if (!m_target.createTrack) {
        setError(error, QStringLiteral("Canonical track was removed"));
        return false;
    }
    return true;
}

bool XmlCanonicalAdapter::read(const QSqlDatabase& database,
        qint64 trackId,
        const QString& field,
        QJsonValue* value,
        QString* error) const {
    bool exists = false;
    if (!value || !check(database, trackId, &exists, error)) {
        return false;
    }
    *value = QJsonValue::Undefined;
    if (!exists) {
        return true;
    }
    if (field == QLatin1String("media")) {
        *value = mediaValue(m_target);
        return true;
    }
    QSqlQuery query(database);
    if (field.startsWith(QLatin1String("metadata:"))) {
        const auto column = field.mid(9);
        if (!metadataColumns().values().contains(column) ||
                !run(&query,
                        QStringLiteral("SELECT %1 FROM library WHERE id=?")
                                .arg(column),
                        {trackId},
                        error) ||
                !query.next()) {
            return false;
        }
        if (!query.value(0).isNull()) {
            *value = numericColumn(column)
                    ? QJsonValue(query.value(0).toInt())
                    : QJsonValue(query.value(0).toString());
        }
        return true;
    }
    if (field == QLatin1String("keys")) {
        if (!run(&query,
                    QStringLiteral(
                            "SELECT "
                            "keys_version,keys_sub_version,keys,key,key_id,"
                            "tuning_frequency_hz FROM library WHERE id=?"),
                    {trackId},
                    error) ||
                !query.next()) {
            return false;
        }
        if (!query.value(0).toString().isEmpty() ||
                !query.value(2).toByteArray().isEmpty() ||
                !query.value(3).toString().isEmpty() || query.value(4).toInt() != 0 ||
                query.value(5).toDouble() != 0) {
            *value = QJsonObject{
                    {QStringLiteral("version"), query.value(0).toString()},
                    {QStringLiteral("subVersion"), query.value(1).toString()},
                    {QStringLiteral("data"),
                            QString::fromLatin1(
                                    query.value(2).toByteArray().toBase64())},
                    {QStringLiteral("text"), query.value(3).toString()},
                    {QStringLiteral("id"), query.value(4).toInt()},
                    {QStringLiteral("tuning"), query.value(5).toDouble()}};
        }
        return true;
    }
    if (field == QLatin1String("artwork")) {
        if (!run(&query,
                    QStringLiteral("SELECT "
                                   "coverart_source,coverart_type,coverart_"
                                   "location FROM library WHERE id=?"),
                    {trackId},
                    error) ||
                !query.next()) {
            return false;
        }
        if (query.value(0).toInt() != CoverInfoRelative::UNKNOWN ||
                query.value(1).toInt() != CoverInfoRelative::NONE ||
                !query.value(2).toString().isEmpty()) {
            *value = QJsonObject{{QStringLiteral("source"), query.value(0).toInt()},
                    {QStringLiteral("type"), query.value(1).toInt()},
                    {QStringLiteral("location"), query.value(2).toString()}};
        }
        return true;
    }
    if (field == QLatin1String("beats")) {
        if (!run(&query,
                    QStringLiteral("SELECT "
                                   "beats_version,beats_sub_version,beats,bpm,"
                                   "bpm_lock FROM library WHERE id=?"),
                    {trackId},
                    error) ||
                !query.next()) {
            return false;
        }
        if (!query.value(2).toByteArray().isEmpty() ||
                query.value(3).toDouble() > 0 || query.value(4).toBool()) {
            *value = QJsonObject{
                    {QStringLiteral("version"), query.value(0).toString()},
                    {QStringLiteral("subVersion"), query.value(1).toString()},
                    {QStringLiteral("data"),
                            QString::fromLatin1(
                                    query.value(2).toByteArray().toBase64())},
                    {QStringLiteral("bpm"), query.value(3).toDouble()},
                    {QStringLiteral("locked"), query.value(4).toBool()}};
        }
        return true;
    }
    if (!cueRow(database, trackId, field, &query, error)) {
        return false;
    }
    if (query.next()) {
        *value = cueValue(query);
        if (query.next()) {
            setError(error,
                    QStringLiteral("Several canonical cues occupy this target; "
                                   "explicit repair is required"));
            return false;
        }
    }
    return true;
}

bool XmlCanonicalAdapter::write(const QSqlDatabase& database,
        qint64 trackId,
        const QString& field,
        const QJsonValue& value,
        QString* error) {
    bool exists = false;
    if (!check(database, trackId, &exists, error)) {
        return false;
    }
    if (field == QLatin1String("media")) {
        if (exists || !m_target.createTrack || value != mediaValue(m_target)) {
            setError(error,
                    QStringLiteral("Media admission can only insert its "
                                   "unchanged reserved identity"));
            return false;
        }
        if (m_target.createLocation) {
            const QFileInfo info(m_target.media.location);
            if (!writeOne(database,
                        QStringLiteral(
                                "INSERT INTO track_locations "
                                "(id,location,filename,directory,filesize,fs_"
                                "deleted,needs_verification) "
                                "VALUES(?,?,?,?,?,0,0)"),
                        {m_target.locationId,
                                m_target.media.location,
                                info.fileName(),
                                info.absolutePath(),
                                m_target.media.fileSize},
                        error)) {
                return false;
            }
        } else {
            QSqlQuery location(database);
            if (!run(&location,
                        QStringLiteral("SELECT location FROM track_locations "
                                       "WHERE id=?"),
                        {m_target.locationId},
                        error) ||
                    !location.next() ||
                    location.value(0).toString() != m_target.media.location) {
                setError(error, QStringLiteral("Reserved media location changed"));
                return false;
            }
        }
        QSqlQuery duplicate(database);
        if (!run(&duplicate,
                    QStringLiteral("SELECT id FROM library WHERE location=?"),
                    {m_target.locationId},
                    error)) {
            return false;
        }
        if (duplicate.next()) {
            setError(error,
                    QStringLiteral("Another importer admitted this media; "
                                   "replan with its identity"));
            return false;
        }
        return writeOne(database,
                QStringLiteral(
                        "INSERT INTO library "
                        "(id,location,samplerate,channels,duration,bitrate,"
                        "filetype,header_parsed,mixxx_deleted,played,"
                        "cuepoint,bpm,bpm_lock,artist,title,album,album_artist,"
                        "composer,grouping,genre,comment,year,tracknumber,"
                        "rating,source_synchronized_ms,datetime_added) "
                        "VALUES(?,?,?,?,?,?,?,1,0,0,0,0,0,NULL,NULL,NULL,NULL,"
                        "NULL,NULL,NULL,NULL,NULL,NULL,NULL,?,CURRENT_TIMESTAMP)"),
                {trackId,
                        m_target.locationId,
                        m_target.media.sampleRate,
                        m_target.media.channels,
                        m_target.media.duration,
                        m_target.media.bitrate,
                        m_target.media.fileType,
                        m_target.media.sourceSynchronizedAt.toMSecsSinceEpoch()},
                error);
    }
    if (!exists) {
        setError(error, QStringLiteral("Media admission must precede canonical annotations"));
        return false;
    }
    if (field.startsWith(QLatin1String("metadata:"))) {
        const auto column = field.mid(9);
        const bool numeric = numericColumn(column);
        const double number = value.toDouble(-1);
        const int maximum = column == QLatin1String("rating") ? 5
                : column == QLatin1String("color")
                ? 0xffffff
                : std::numeric_limits<int>::max();
        if (!metadataColumns().values().contains(column) ||
                (column == QLatin1String("datetime_added") &&
                        (value.isUndefined() || value.toString().isEmpty())) ||
                (!value.isUndefined() &&
                        (numeric ? (!value.isDouble() ||
                                           !util_isfinite(number) ||
                                           number < 0 || number > maximum ||
                                           std::floor(number) != number)
                                 : !value.isString()))) {
            setError(error, QStringLiteral("Unsupported metadata type or field"));
            return false;
        }
        if (!value.isUndefined() && !value.toString().isEmpty() &&
                (column == QLatin1String("datetime_added") ||
                        column == QLatin1String("last_played_at"))) {
            const auto parsed = QDateTime::fromString(
                    value.toString(), QStringLiteral("yyyy-MM-dd hh:mm:ss"));
            if (!parsed.isValid() ||
                    parsed.toString(QStringLiteral("yyyy-MM-dd hh:mm:ss")) !=
                            value.toString()) {
                setError(error, QStringLiteral("Invalid canonical UTC timestamp"));
                return false;
            }
        }
        return writeOne(database,
                QStringLiteral("UPDATE library SET %1=? WHERE id=?")
                        .arg(column),
                {value.isUndefined() ? QVariant() : value.toVariant(), trackId},
                error);
    }
    if (field == QLatin1String("keys")) {
        if (value.isUndefined()) {
            return writeOne(database,
                    QStringLiteral("UPDATE library SET "
                                   "key=NULL,keys_version=NULL,keys_sub_"
                                   "version=NULL,keys=NULL,key_id=0,tuning_"
                                   "frequency_hz=0 WHERE id=?"),
                    {trackId},
                    error);
        }
        const auto object = value.toObject();
        const auto decoded = QByteArray::fromBase64Encoding(
                object.value(QStringLiteral("data")).toString().toLatin1(),
                QByteArray::AbortOnBase64DecodingErrors);
        mixxx::track::io::key::KeyMap serialized;
        if (!value.isObject() || !decoded ||
                object.value(QStringLiteral("version")) != QLatin1String(KEY_MAP_VERSION) ||
                !serialized.ParseFromArray(decoded.decoded.constData(), decoded.decoded.size())) {
            setError(error, QStringLiteral("Invalid canonical key serialization"));
            return false;
        }
        auto bytes = decoded.decoded;
        const auto keys = KeyFactory::loadKeysFromByteArray(
                object.value(QStringLiteral("version")).toString(),
                object.value(QStringLiteral("subVersion")).toString(),
                &bytes);
        if (!util_isfinite(keys.getGlobalTuningFrequencyHz()) ||
                keys.getGlobalTuningFrequencyHz() < 0 || keysValue(keys) != object) {
            setError(error, QStringLiteral("Inconsistent canonical key fields"));
            return false;
        }
        return writeOne(database,
                QStringLiteral("UPDATE library SET "
                               "key=?,keys_version=?,keys_sub_version=?,keys=?,"
                               "key_id=?,tuning_frequency_hz=? WHERE id=?"),
                {keys.getGlobalKeyText(),
                        keys.getVersion(),
                        keys.getSubVersion(),
                        keys.toByteArray(),
                        static_cast<int>(keys.getGlobalKey()),
                        keys.getGlobalTuningFrequencyHz(),
                        trackId},
                error);
    }
    if (field == QLatin1String("artwork")) {
        const auto object = value.toObject();
        const auto location = object.value(QStringLiteral("location")).toString();
        const int source = object.value(QStringLiteral("source")).toInt(-1);
        const int type = object.value(QStringLiteral("type")).toInt(-1);
        if (!value.isObject() || source != CoverInfoRelative::USER_SELECTED ||
                !object.value(QStringLiteral("location")).isString() ||
                (type == CoverInfoRelative::NONE
                                ? !location.isEmpty()
                                : (type != CoverInfoRelative::FILE ||
                                          !QFileInfo(location).isAbsolute() ||
                                          normalizeLocation(location) !=
                                                  location))) {
            setError(error, QStringLiteral("Invalid canonical artwork reference"));
            return false;
        }
        return writeOne(database,
                QStringLiteral("UPDATE library SET "
                               "coverart_source=?,coverart_type=?,coverart_"
                               "location=?,coverart_color=NULL,coverart_digest="
                               "NULL,coverart_hash=0 WHERE id=?"),
                {source, type, location, trackId},
                error);
    }
    if (field == QLatin1String("beats")) {
        const auto object = value.toObject();
        const auto bytes = QByteArray::fromBase64(
                object.value(QStringLiteral("data")).toString().toLatin1());
        const auto beats = Beats::fromByteArray(audio::SampleRate(m_target.media.sampleRate),
                object.value(QStringLiteral("version")).toString(),
                object.value(QStringLiteral("subVersion")).toString(),
                bytes);
        if (!beats || !util_isfinite(object.value(QStringLiteral("bpm")).toDouble()) ||
                object.value(QStringLiteral("bpm")).toDouble() <= 0) {
            setError(error, QStringLiteral("Invalid canonical grid serialization"));
            return false;
        }
        return writeOne(database,
                QStringLiteral("UPDATE library SET "
                               "beats_version=?,beats_sub_version=?,beats=?,"
                               "bpm=?,bpm_lock=? WHERE id=?"),
                {beats->getVersion(),
                        beats->getSubVersion(),
                        beats->toByteArray(),
                        object.value(QStringLiteral("bpm")).toDouble(),
                        object.value(QStringLiteral("locked")).toBool(),
                        trackId},
                error);
    }
    QSqlQuery query(database);
    if (!cueRow(database, trackId, field, &query, error)) {
        return false;
    }
    DbId id;
    if (query.next()) {
        id = DbId(query.value(0));
        if (query.next()) {
            setError(error, QStringLiteral("Ambiguous occupied cue target"));
            return false;
        }
    }
    query.finish();
    if (value.isUndefined()) {
        if (!id.isValid()) {
            return true;
        }
        return writeOne(database,
                QStringLiteral("DELETE FROM cues WHERE id=? AND track_id=?"),
                {id.toVariant(), trackId},
                error);
    }
    const auto object = value.toObject();
    const auto type = static_cast<CueType>(object.value(QStringLiteral("type")).toInt());
    const double start = object.value(QStringLiteral("start")).toDouble(-1);
    const double end = object.value(QStringLiteral("end")).isNull()
            ? -1
            : object.value(QStringLiteral("end")).toDouble(-1);
    const int hot = object.value(QStringLiteral("hotCue")).toInt(-1);
    const int color = object.value(QStringLiteral("color")).toInt(-1);
    if (!util_isfinite(start) || start < 0 || !util_isfinite(end) ||
            color < 0 || color > 0xffffff ||
            (type != CueType::Memory && type != CueType::HotCue &&
                    type != CueType::Loop) ||
            (type == CueType::Loop ? end <= start : end != -1) ||
            (field.startsWith(QLatin1String("cue:hot:"))
                            ? hot != field.mid(8).toInt()
                            : hot != -1)) {
        setError(error, QStringLiteral("Invalid canonical cue payload"));
        return false;
    }
    CueDAO dao;
    dao.initialize(database);
    std::optional<DbId> saved;
    if (id.isValid()) {
        Cue cue(id,
                type,
                audio::FramePos(start),
                end >= 0 ? end - start : 0,
                hot,
                object.value(QStringLiteral("label")).toString(),
                RgbColor(static_cast<QRgb>(color)));
        saved = dao.writeCue(TrackId(QVariant(trackId)), cue);
    } else {
        Cue cue(type,
                hot,
                audio::FramePos(start),
                end >= 0 ? audio::FramePos(end) : audio::kInvalidFramePos,
                RgbColor(static_cast<QRgb>(color)));
        cue.setLabel(object.value(QStringLiteral("label")).toString());
        saved = dao.writeCue(TrackId(QVariant(trackId)), cue);
    }
    if (!saved) {
        setError(error, QStringLiteral("Canonical cue write failed"));
        return false;
    }
    if (field.startsWith(QLatin1String("cue:memory:"))) {
        return writeOne(database,
                QStringLiteral("INSERT INTO rekordbox_import_cue_bindings_v1 "
                               "VALUES(?,?,?) "
                               "ON CONFLICT(canonical_track_id,target_field) "
                               "DO UPDATE SET cue_id=excluded.cue_id"),
                {trackId, field, saved->toVariant()},
                error);
    }
    return true;
}

XmlTrackImportResult importXmlTrack(ImportSourceRegistry* registry,
        const std::shared_ptr<const Library>& library,
        int trackIndex,
        const QString& sourceTrackId,
        const ImportTicket& ticket,
        const QString& fingerprint,
        const XmlMediaFacts& media,
        const ImportPolicy& policy,
        const XmlCanonicalAccess& access,
        const std::function<bool()>& cancelled) {
    if (!registry) {
        return {{ImportOutcome::Invalid,
                        false,
                        0,
                        {loss(QStringLiteral("missing_registry"),
                                QStringLiteral("No destination registry"))}},
                0,
                false};
    }
    const auto prepared = prepareXmlTrackImport(registry->database(),
            library,
            trackIndex,
            sourceTrackId,
            ticket,
            fingerprint,
            media,
            access);
    if (!prepared.snapshot) {
        return {{prepared.outcome, false, 0, prepared.issues}, 0, false};
    }
    XmlCanonicalAdapter adapter(prepared.target, access);
    const auto plan = registry->plan(*prepared.snapshot, policy, adapter);
    if (!plan.plan) {
        return {{plan.outcome, false, 0, plan.issues}, prepared.target.trackId, false};
    }
    const auto result = registry->apply(*plan.plan, &adapter, cancelled);
    return {result, prepared.target.trackId, result.committed && prepared.target.createTrack};
}

} // namespace mixxx::rekordbox
// End of code prepared by ChatGPT for human review and submission.
