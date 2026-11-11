// Prepared by ChatGPT for human review.
#include "library/rekordbox/rekordboxanlzimporter.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <algorithm>
#include <cmath>
#include <limits>

#include "track/beats.h"
#include "track/cue.h"
#include "util/color/predefinedcolorpalettes.h"

namespace mixxx::rekordbox {
namespace {
constexpr qint64 kMaximumFileBytes = 64LL * 1024 * 1024;
const QString kDecoderRevision = QStringLiteral("anlz-normalizer-1.0");

ImportIssue issue(const QString& code, const QString& message, const QString& annotation = {}) {
    return {code, message, {}, annotation};
}

QString hash(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

ImportOutcome outcome(AnlzOutcome value) {
    switch (value) {
    case AnlzOutcome::Success:
        return ImportOutcome::Success;
    case AnlzOutcome::Warning:
        return ImportOutcome::Warning;
    case AnlzOutcome::Unsupported:
        return ImportOutcome::Unsupported;
    case AnlzOutcome::Invalid:
        return ImportOutcome::Invalid;
    case AnlzOutcome::Denied:
        return ImportOutcome::Denied;
    case AnlzOutcome::Unavailable:
        return ImportOutcome::Unavailable;
    case AnlzOutcome::Cancelled:
        return ImportOutcome::Cancelled;
    case AnlzOutcome::Failed:
        return ImportOutcome::Failed;
    }
    return ImportOutcome::Failed;
}

bool run(QSqlQuery* query, const QString& sql, const QVariantList& values, QString* error) {
    bool ok = query->prepare(sql);
    for (const auto& value : values) {
        query->addBindValue(value);
    }
    ok = ok && query->exec();
    if (!ok && error) {
        *error = query->lastError().text();
    }
    return ok;
}

QString fingerprint(const SourceSnapshot& source, const AnlzFiles& files) {
    return hash(QJsonDocument(QJsonArray{source.fingerprint, files.fingerprint})
                    .toJson(QJsonDocument::Compact));
}

QJsonObject pdbRecord(const ExternalTrack& external) {
    const auto& metadata = external.metadata;
    const auto presentString = [](const QString& value) {
        return value.isNull() ? QJsonValue(QJsonValue::Null) : QJsonValue(value);
    };
    return {{QStringLiteral("title"), presentString(metadata.title)},
            {QStringLiteral("artist"), presentString(metadata.artist)},
            {QStringLiteral("album"), presentString(metadata.album)},
            {QStringLiteral("albumArtist"),
                    presentString(metadata.albumArtist)},
            {QStringLiteral("composer"), presentString(metadata.composer)},
            {QStringLiteral("genre"), presentString(metadata.genre)},
            {QStringLiteral("comment"), presentString(metadata.comment)},
            {QStringLiteral("year"), metadata.year},
            {QStringLiteral("trackNumber"), metadata.trackNumber},
            {QStringLiteral("rating"), metadata.rating},
            {QStringLiteral("playCount"), metadata.playCount},
            {QStringLiteral("dateAdded"), presentString(metadata.dateAdded)},
            {QStringLiteral("lastPlayed"), presentString(metadata.lastPlayed)},
            {QStringLiteral("key"), presentString(metadata.key)},
            {QStringLiteral("artworkReference"),
                    presentString(metadata.artworkReference)},
            {QStringLiteral("label"), presentString(metadata.label)},
            {QStringLiteral("remixer"), presentString(metadata.remixer)},
            {QStringLiteral("discNumber"), metadata.discNumber},
            {QStringLiteral("declaredSampleRate"), metadata.sampleRate},
            {QStringLiteral("declaredBpm"), metadata.bpm},
            {QStringLiteral("declaredDuration"), metadata.durationSeconds},
            {QStringLiteral("declaredBitrate"), metadata.bitrate},
            {QStringLiteral("color"),
                    external.color
                            ? QJsonValue(static_cast<int>(*external.color))
                            : QJsonValue(QJsonValue::Null)}};
}

QString inputKey(const SourceSnapshot& source,
        int index,
        const AnlzFiles& files,
        qint64 correction) {
    const auto& track = source.tracks[index];
    QJsonArray diagnostics;
    for (const auto& diagnostic : files.issues) {
        diagnostics.append(QJsonArray{
                diagnostic.code, diagnostic.message, diagnostic.annotationId});
    }
    return hash(QJsonDocument(
            QJsonObject{{QStringLiteral("decoder"), kDecoderRevision},
                    {QStringLiteral("profile"),
                            QStringLiteral("explicit-beats-4-counter-sql-v1")},
                    {QStringLiteral("sourceTrackId"), track.sourceTrackId},
                    {QStringLiteral("location"), track.metadata.location},
                    {QStringLiteral("fingerprint"), fingerprint(source, files)},
                    {QStringLiteral("pdbRecord"), pdbRecord(track)},
                    {QStringLiteral("keyInterpretation"),
                            track.metadata.key.isNull()
                                    ? QJsonValue(QJsonValue::Null)
                                    : QJsonValue(canonicalKeyValue(track.metadata.key))},
                    {QStringLiteral("diagnostics"), diagnostics},
                    {QStringLiteral("correctionMilliseconds"),
                            QString::number(correction)}})
                    .toJson(QJsonDocument::Compact));
}

QString policyJson(const ImportPolicy& policy) {
    return QString::fromUtf8(QJsonDocument(
            QJsonObject{{QStringLiteral("revision"), policy.revision},
                    {QStringLiteral("replace_edited_source_owned"),
                            policy.replaceEditedSourceOwned},
                    {QStringLiteral("accept_preserved_conflicts"),
                            policy.acceptPreservedConflicts}})
                    .toJson(QJsonDocument::Compact));
}

bool validSource(const SourceSnapshot& source, int index, const AnlzFiles& files) {
    if (source.source.kind != SourceKind::Usb || source.source.id.isEmpty() ||
            source.generation <= 0 || source.fingerprint.isEmpty() || index < 0 ||
            index >= source.tracks.size() || !files.dat || files.fingerprint.isEmpty()) {
        return false;
    }
    const auto& track = source.tracks[index];
    if (!track.sourceTrackId.startsWith(QLatin1String("pdb:")) ||
            track.metadata.location.isEmpty()) {
        return false;
    }
    return std::count_if(source.tracks.cbegin(), source.tracks.cend(), [&](const auto& candidate) {
        return candidate.sourceTrackId == track.sourceTrackId;
    }) == 1;
}

QString physicalKey(const QString& file, const AnlzCue& cue) {
    return QStringLiteral("%1/%2/%3/%4")
            .arg(file, QString::fromStdString(cue.sectionTag))
            .arg(cue.key.sectionOrdinal)
            .arg(cue.key.entryOrdinal);
}

QJsonObject sourceRecord(const QString& file, const AnlzCue& cue) {
    QJsonObject record{{QStringLiteral("file"), file},
            {QStringLiteral("section"), QString::fromStdString(cue.sectionTag)},
            {QStringLiteral("sectionOrdinal"),
                    static_cast<double>(cue.key.sectionOrdinal)},
            {QStringLiteral("entryOrdinal"),
                    static_cast<double>(cue.key.entryOrdinal)},
            {QStringLiteral("bank"),
                    cue.bank == AnlzCueBank::Hot ? QStringLiteral("hot")
                                                 : QStringLiteral("memory")},
            {QStringLiteral("sourceType"), cue.sourceType},
            {QStringLiteral("startMilliseconds"),
                    static_cast<double>(cue.start.milliseconds)}};
    if (cue.end)
        record.insert(QStringLiteral("endMilliseconds"),
                static_cast<double>(cue.end->milliseconds));
    if (cue.hotSlot)
        record.insert(QStringLiteral("hotSlot"), static_cast<double>(*cue.hotSlot));
    if (cue.labelUtf8)
        record.insert(QStringLiteral("label"), QString::fromStdString(*cue.labelUtf8));
    if (cue.rgb)
        record.insert(QStringLiteral("rgb"),
                QJsonArray{cue.rgb->red, cue.rgb->green, cue.rgb->blue});
    if (cue.sourceColorId)
        record.insert(QStringLiteral("colorId"), *cue.sourceColorId);
    if (cue.sourceColorCode)
        record.insert(QStringLiteral("colorCode"), *cue.sourceColorCode);
    if (cue.sourceStatus)
        record.insert(QStringLiteral("status"), static_cast<double>(*cue.sourceStatus));
    if (cue.enabled)
        record.insert(QStringLiteral("enabled"), *cue.enabled);
    if (cue.activeLoop)
        record.insert(QStringLiteral("activeLoop"), *cue.activeLoop);
    if (cue.sourceOrderFirst)
        record.insert(QStringLiteral("orderFirst"), *cue.sourceOrderFirst);
    if (cue.sourceOrderLast)
        record.insert(QStringLiteral("orderLast"), *cue.sourceOrderLast);
    if (cue.loopNumerator)
        record.insert(QStringLiteral("loopNumerator"), *cue.loopNumerator);
    if (cue.loopDenominator)
        record.insert(QStringLiteral("loopDenominator"), *cue.loopDenominator);
    return record;
}

struct ResolvedCue {
    AnlzCue cue;
    QString identity;
    QJsonArray records;
};

struct CueSet {
    bool present = false;
    QVector<ResolvedCue> cues;
};

bool sameTimingAndRole(const AnlzCue& left, const AnlzCue& right) {
    return left.bank == right.bank && left.role == right.role &&
            left.sourceType == right.sourceType && left.hotSlot == right.hotSlot &&
            left.start.milliseconds == right.start.milliseconds &&
            left.end.has_value() == right.end.has_value() &&
            (!left.end || left.end->milliseconds == right.end->milliseconds);
}

bool mergeCue(ResolvedCue* left, const ResolvedCue& right) {
    if (!sameTimingAndRole(left->cue, right.cue) ||
            (left->cue.labelUtf8 && right.cue.labelUtf8 &&
                    left->cue.labelUtf8 != right.cue.labelUtf8) ||
            (left->cue.sourceStatus && right.cue.sourceStatus &&
                    left->cue.sourceStatus != right.cue.sourceStatus)) {
        return false;
    }
    if (left->cue.rgb && right.cue.rgb &&
            (left->cue.rgb->red != right.cue.rgb->red ||
                    left->cue.rgb->green != right.cue.rgb->green ||
                    left->cue.rgb->blue != right.cue.rgb->blue)) {
        return false;
    }
    if (!left->cue.labelUtf8)
        left->cue.labelUtf8 = right.cue.labelUtf8;
    if (!left->cue.rgb)
        left->cue.rgb = right.cue.rgb;
    if (!left->cue.sourceStatus)
        left->cue.sourceStatus = right.cue.sourceStatus;
    if (!left->cue.enabled)
        left->cue.enabled = right.cue.enabled;
    if (!left->cue.activeLoop)
        left->cue.activeLoop = right.cue.activeLoop;
    if (!left->cue.sourceColorId)
        left->cue.sourceColorId = right.cue.sourceColorId;
    for (const auto& record : right.records)
        left->records.append(record);
    return true;
}

bool mergeSets(CueSet* left, const CueSet& right, AnlzCueBank bank) {
    if (!right.present)
        return true;
    if (!left->present) {
        *left = right;
        return true;
    }
    if (bank == AnlzCueBank::Memory) {
        if (left->cues.size() != right.cues.size())
            return false;
        for (qsizetype i = 0; i < left->cues.size(); ++i) {
            if (!mergeCue(&left->cues[i], right.cues[i]))
                return false;
        }
        return true;
    }
    for (const auto& cue : right.cues) {
        const auto found = std::find_if(left->cues.begin(),
                left->cues.end(),
                [&](const auto& candidate) {
                    return candidate.cue.hotSlot == cue.cue.hotSlot;
                });
        if (found == left->cues.end()) {
            left->cues.append(cue);
        } else if (!mergeCue(&*found, cue)) {
            return false;
        }
    }
    return true;
}

std::optional<CueSet> fileCues(const DecodedAnlz& decoded, const QString& file, AnlzCueBank bank) {
    CueSet result;
    for (const auto* tag : {"PCOB", "PCO2"}) {
        CueSet representation;
        int lists = 0;
        for (const auto& list : decoded.cueLists) {
            if (list.bank == bank && list.sectionTag == tag)
                ++lists;
        }
        if (lists > 1)
            return {};
        representation.present = lists == 1;
        QSet<quint32> hotSlots;
        for (const auto& cue : decoded.cues) {
            if (cue.bank != bank || cue.sectionTag != tag)
                continue;
            if (bank == AnlzCueBank::Hot && (!cue.hotSlot || hotSlots.contains(*cue.hotSlot)))
                return {};
            if (cue.hotSlot)
                hotSlots.insert(*cue.hotSlot);
            representation.cues.push_back({cue,
                    physicalKey(file, cue),
                    QJsonArray{sourceRecord(file, cue)}});
        }
        if (!mergeSets(&result, representation, bank))
            return {};
    }
    return result;
}

QString memoryTarget(const QString& sourceId, const QString& trackId, const QString& identity) {
    return QStringLiteral("cue:memory:") +
            hash(QJsonDocument(QJsonArray{sourceId, trackId, identity})
                            .toJson(QJsonDocument::Compact));
}

std::optional<QJsonObject> grid(const AnlzFiles& files,
        const XmlMediaFacts& media,
        qint64 correction,
        QJsonObject* provenance,
        QVector<ImportIssue>* issues) {
    const auto& beats = files.dat->beats;
    if (beats.empty())
        return {};
    if (beats.size() > 1000000) {
        issues->append(issue(QStringLiteral("grid_limit"),
                QStringLiteral("PQTZ grid exceeds the supported beat count"),
                QStringLiteral("anlz/grid")));
        return {};
    }
    std::vector<BeatMarker> markers;
    QJsonArray raw;
    std::optional<qint64> previous;
    std::optional<qint64> last;
    const auto phase = beats.front().sourceBeatInBar;
    bool observedWrap = false;
    for (std::size_t i = 0; i < beats.size(); ++i) {
        const auto& beat = beats[i];
        const auto position = anlzTimestampToPcmFrames(beat.time, media.sampleRate, correction);
        if (position && *position > std::numeric_limits<qint32>::max()) {
            issues->append(issue(QStringLiteral("grid_storage_range"),
                    QStringLiteral("PQTZ position exceeds the canonical beat serialization range"),
                    QStringLiteral("anlz/grid")));
            return {};
        }
        if (!position ||
                (previous && *position <= *previous) || phase < 1 || phase > 4 ||
                beat.sourceBeatInBar != (phase - 1 + i) % 4 + 1 ||
                beat.key.sectionOrdinal != beats.front().key.sectionOrdinal) {
            issues->append(issue(QStringLiteral("grid_timing_or_phase"),
                    QStringLiteral("PQTZ times or beat counter cannot be represented without loss"),
                    QStringLiteral("anlz/grid")));
            return {};
        }
        observedWrap = observedWrap ||
                (i > 0 && beats[i - 1].sourceBeatInBar == 4 &&
                        beat.sourceBeatInBar == 1);
        if (i + 1 < beats.size())
            markers.emplace_back(audio::FramePos(static_cast<double>(*position)), 1);
        raw.append(QJsonObject{
                {QStringLiteral("sectionOrdinal"),
                        static_cast<double>(beat.key.sectionOrdinal)},
                {QStringLiteral("entryOrdinal"),
                        static_cast<double>(beat.key.entryOrdinal)},
                {QStringLiteral("milliseconds"),
                        static_cast<double>(beat.time.milliseconds)},
                {QStringLiteral("beatInBar"), beat.sourceBeatInBar},
                {QStringLiteral("tempoHundredths"),
                        beat.sourceTempoHundredths}});
        previous = position;
        last = position;
    }
    if (!observedWrap) {
        issues->append(issue(QStringLiteral("grid_bar_length_unknown"),
                QStringLiteral("This short PQTZ sequence does not establish "
                               "the four-beat counter required by Mixxx"),
                QStringLiteral("anlz/grid")));
        return {};
    }
    const auto normalized =
            Beats::fromBeatMarkers(audio::SampleRate(media.sampleRate),
                    markers,
                    audio::FramePos(static_cast<double>(*last)),
                    Bpm(beats.back().sourceTempoHundredths / 100.0),
                    QStringLiteral("rekordbox-anlz-v1"),
                    (4 - (phase - 1)) % 4);
    if (!normalized)
        return {};
    *provenance = {{QStringLiteral("file"), QStringLiteral("DAT")},
            {QStringLiteral("gridKind"), QStringLiteral("explicit-beats")},
            {QStringLiteral("meterNumerator"), QJsonValue(QJsonValue::Null)},
            {QStringLiteral("meterDenominator"), QJsonValue(QJsonValue::Null)},
            {QStringLiteral("correctionMilliseconds"), QString::number(correction)},
            {QStringLiteral("records"), raw}};
    const auto bpm = normalized->getBpmInRange(audio::kStartFramePos,
            audio::FramePos(std::max(media.duration * media.sampleRate,
                    static_cast<double>(*last + media.sampleRate))));
    return QJsonObject{{QStringLiteral("version"), normalized->getVersion()},
            {QStringLiteral("subVersion"), normalized->getSubVersion()},
            {QStringLiteral("data"), QString::fromLatin1(normalized->toByteArray().toBase64())},
            {QStringLiteral("bpm"), bpm.value()},
            {QStringLiteral("locked"), true}};
}
} // namespace

AnlzFilesResult readAnlzFiles(const QString& baseDatPath, const std::function<bool()>& cancelled) {
    AnlzFilesResult result;
    auto files = std::make_shared<AnlzFiles>();
    const auto fail = [&](ImportOutcome status, const QString& code, const QString& message) {
        result.outcome = status;
        result.issues.append(issue(code, message));
        return result;
    };
    if (baseDatPath.isEmpty() ||
            QFileInfo(baseDatPath)
                            .suffix()
                            .compare(QLatin1String("DAT"),
                                    Qt::CaseInsensitive) != 0) {
        return fail(ImportOutcome::Unavailable,
                QStringLiteral("missing_anlz_path"),
                QStringLiteral("A DAT analysis path is required"));
    }
    const QString extPath = baseDatPath.left(baseDatPath.size() - 3) + QStringLiteral("EXT");
    for (const auto& path : {baseDatPath, extPath}) {
        if (cancelled && cancelled())
            return fail(ImportOutcome::Cancelled,
                    QStringLiteral("cancelled"),
                    QStringLiteral("ANLZ read cancelled"));
        const bool dat = path == baseDatPath;
        if (!dat && !QFileInfo::exists(path))
            continue;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return fail(file.error() == QFileDevice::PermissionsError
                            ? ImportOutcome::Denied
                            : ImportOutcome::Unavailable,
                    QStringLiteral("anlz_open"),
                    file.errorString());
        }
        const auto beforeSize = file.size();
        const auto beforeModified = QFileInfo(file).lastModified();
        if (beforeSize < 0 || beforeSize > kMaximumFileBytes) {
            return fail(ImportOutcome::Unsupported,
                    QStringLiteral("anlz_size"),
                    QStringLiteral(
                            "ANLZ file exceeds the supported byte limit"));
        }
        const auto bytes = file.read(kMaximumFileBytes + 1);
        if (file.error() != QFileDevice::NoError || bytes.size() != beforeSize ||
                file.size() != beforeSize || QFileInfo(file).lastModified() != beforeModified) {
            return fail(ImportOutcome::Unavailable,
                    QStringLiteral("anlz_changed_during_read"),
                    QStringLiteral("ANLZ source changed or could not be read "
                                   "completely"));
        }
        const auto decoded =
                decodeAnlz(std::string(bytes.constData(),
                                   static_cast<std::size_t>(bytes.size())),
                        cancelled);
        for (const auto& diagnostic : decoded.diagnostics) {
            if (diagnostic.severity != AnlzSeverity::Info) {
                result.issues.append(
                        issue(QString::fromStdString(diagnostic.code),
                                QString::fromStdString(diagnostic.message)));
            }
        }
        if (!decoded.snapshot) {
            result.outcome = outcome(decoded.outcome);
            return result;
        }
        if (dat) {
            files->dat = decoded.snapshot;
            files->datHash = hash(bytes);
        } else {
            files->ext = decoded.snapshot;
            files->extHash = hash(bytes);
        }
    }
    files->fingerprint = hash(QJsonDocument(
            QJsonArray{files->datHash,
                    files->ext ? files->extHash : QStringLiteral("absent")})
                    .toJson(QJsonDocument::Compact));
    files->issues = result.issues;
    result.outcome = result.issues.isEmpty() ? ImportOutcome::Success : ImportOutcome::Warning;
    result.snapshot = std::move(files);
    return result;
}

std::optional<qint64> currentAnlzTrackRevision(const ImportSourceRegistry& registry,
        const SourceSnapshot& source,
        int trackIndex,
        const AnlzFiles& files,
        const ImportPolicy& policy,
        qint64 correctionMilliseconds,
        QString* error) {
    if (error)
        error->clear();
    if (!validSource(source, trackIndex, files))
        return {};
    const auto& track = source.tracks[trackIndex];
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
                {source.source.id,
                        track.sourceTrackId,
                        source.generation,
                        fingerprint(source, files),
                        inputKey(source,
                                trackIndex,
                                files,
                                correctionMilliseconds),
                        policyJson(policy),
                        track.metadata.location},
                error)) {
        return {};
    }
    return query.next() ? std::optional<qint64>(query.value(0).toLongLong()) : std::nullopt;
}

AnlzPreparedTrack prepareAnlzTrackImport(const QSqlDatabase& database,
        const std::shared_ptr<const SourceSnapshot>& source,
        int trackIndex,
        const std::shared_ptr<const AnlzFiles>& files,
        const XmlMediaFacts& media,
        const XmlCanonicalAccess& access,
        qint64 correctionMilliseconds,
        const std::function<bool()>& cancelled) {
    AnlzPreparedTrack result;
    const auto fail = [&](ImportOutcome status, const QString& code, const QString& message) {
        result.outcome = status;
        result.snapshot.reset();
        result.issues.append(issue(code, message));
        return result;
    };
    if (!source || !files || !validSource(*source, trackIndex, *files)) {
        return fail(ImportOutcome::Invalid,
                QStringLiteral("anlz_source_identity"),
                QStringLiteral("A current unambiguous USB snapshot and decoded "
                               "DAT are required"));
    }
    if (cancelled && cancelled())
        return fail(ImportOutcome::Cancelled,
                QStringLiteral("cancelled"),
                QStringLiteral("ANLZ import cancelled"));
    const auto& external = source->tracks[trackIndex];
    if (media.location != external.metadata.location) {
        return fail(ImportOutcome::Unavailable,
                QStringLiteral("media_changed"),
                QStringLiteral(
                        "Probed media does not match the selected PDB track"));
    }
    result.issues = files->issues;
    const auto admitted = prepareCanonicalTarget(database, media, access, &result.target);
    if (admitted.outcome != ImportOutcome::Success) {
        result.outcome = admitted.outcome;
        result.issues += admitted.issues;
        return result;
    }
    ImportSnapshot snapshot;
    snapshot.ticket = {source->source.id, source->generation};
    snapshot.fingerprint = fingerprint(*source, *files);
    snapshot.decoderRevision = kDecoderRevision;
    snapshot.formatVersion = QStringLiteral("legacy-anlz-PMAI");
    snapshot.capabilities = {QStringLiteral("track-scoped-sql"),
            QStringLiteral("physical-memory-cues"),
            QStringLiteral("native-hot-slots"),
            QStringLiteral("explicit-beat-timestamps"),
            QStringLiteral("four-beat-counter-unknown-meter"),
            QStringLiteral("no-external-collections")};
    snapshot.scope = ImportScope::TracksOnly;
    snapshot.inputKey = inputKey(*source, trackIndex, *files, correctionMilliseconds);
    struct Lease {
        std::shared_ptr<const SourceSnapshot> source;
        std::shared_ptr<const AnlzFiles> files;
        std::shared_ptr<const void> cache;
    };
    snapshot.accessLease = std::make_shared<const Lease>(Lease{source, files, access.lease});
    snapshot.tracks.push_back({external.sourceTrackId,
            QStringLiteral("mixxx-location:%1").arg(result.target.locationId),
            result.target.trackId});
    const auto& metadata = external.metadata;
    snapshot.fields.append(canonicalMediaAdmission(result.target,
            external.sourceTrackId,
            {{QStringLiteral("pdbFingerprint"), source->fingerprint},
                    {QStringLiteral("datFingerprint"), files->datHash},
                    {QStringLiteral("extFingerprint"), files->extHash},
                    {QStringLiteral("pdbRecord"), pdbRecord(external)}}));
    const auto add = [&](const QString& annotation,
                             const QString& target,
                             const QJsonValue& value,
                             const QJsonObject& provenance = QJsonObject()) {
        snapshot.fields.append({external.sourceTrackId,
                annotation,
                target,
                value,
                false,
                provenance});
    };
    for (const auto& field : QVector<QPair<QString, QString>>{
                 {QStringLiteral("title"), metadata.title},
                 {QStringLiteral("artist"), metadata.artist},
                 {QStringLiteral("album"), metadata.album},
                 {QStringLiteral("genre"), metadata.genre},
                 {QStringLiteral("album_artist"), metadata.albumArtist},
                 {QStringLiteral("composer"), metadata.composer},
                 {QStringLiteral("comment"), metadata.comment}}) {
        if (!field.second.isNull())
            add(QStringLiteral("pdb/") + field.first,
                    QStringLiteral("metadata:") + field.first,
                    field.second);
    }
    if (metadata.year > 0)
        add(QStringLiteral("pdb/year"),
                QStringLiteral("metadata:year"),
                QString::number(metadata.year));
    if (metadata.trackNumber > 0)
        add(QStringLiteral("pdb/tracknumber"),
                QStringLiteral("metadata:tracknumber"),
                QString::number(metadata.trackNumber));
    if (metadata.rating >= 0 && metadata.rating <= 5)
        add(QStringLiteral("pdb/rating"), QStringLiteral("metadata:rating"), metadata.rating);
    if (metadata.playCount >= 0)
        add(QStringLiteral("pdb/playcount"),
                QStringLiteral("metadata:timesplayed"),
                metadata.playCount);
    if (external.color)
        add(QStringLiteral("pdb/color"),
                QStringLiteral("metadata:color"),
                static_cast<int>(*external.color));
    if (!metadata.key.isNull())
        add(QStringLiteral("pdb/key"), QStringLiteral("keys"), canonicalKeyValue(metadata.key));
    if (!metadata.artworkReference.isNull()) {
        if (const auto value = canonicalArtworkValue(metadata.artworkReference, &result.issues)) {
            add(QStringLiteral("pdb/artwork"), QStringLiteral("artwork"), *value);
        }
    }
    for (const auto& field : QVector<QPair<QString, QString>>{
                 {QStringLiteral("datetime_added"), metadata.dateAdded},
                 {QStringLiteral("last_played_at"), metadata.lastPlayed}}) {
        if (!field.second.isNull()) {
            if (const auto value = canonicalTimestampValue(field.second,
                        QStringLiteral("pdb/") + field.first,
                        &result.issues,
                        field.first != QLatin1String("datetime_added"))) {
                add(QStringLiteral("pdb/") + field.first,
                        QStringLiteral("metadata:") + field.first,
                        *value);
            }
        }
    }
    if (!metadata.label.isEmpty() || !metadata.remixer.isEmpty() || metadata.discNumber != 0) {
        result.issues.append(issue(QStringLiteral("pdb_metadata_capability"),
                QStringLiteral(
                        "PDB label, remixer and disc number have no canonical "
                        "fields and remain in source provenance"),
                QStringLiteral("pdb/metadata")));
    }
    QJsonObject gridProvenance;
    if (const auto value = grid(*files,
                media,
                correctionMilliseconds,
                &gridProvenance,
                &result.issues)) {
        add(QStringLiteral("anlz/grid"), QStringLiteral("beats"), *value, gridProvenance);
    }
    QSet<QString> supplied;
    QSet<QString> presentBanks;
    for (const auto bank : {AnlzCueBank::Memory, AnlzCueBank::Hot}) {
        auto combined = fileCues(*files->dat, QStringLiteral("DAT"), bank);
        const auto extra = files->ext
                ? fileCues(*files->ext, QStringLiteral("EXT"), bank)
                : std::optional<CueSet>(CueSet{});
        if (!combined || !extra || !mergeSets(&*combined, *extra, bank)) {
            return fail(ImportOutcome::Unsupported,
                    QStringLiteral("anlz_cue_reconciliation"),
                    QStringLiteral("DAT/EXT cue representations disagree or "
                                   "contain ambiguous physical lists"));
        }
        const QString bankName = bank == AnlzCueBank::Memory
                ? QStringLiteral("memory")
                : QStringLiteral("hot");
        if (combined->present)
            presentBanks.insert(bankName);
        if (combined->cues.size() > 4096) {
            return fail(ImportOutcome::Unsupported,
                    QStringLiteral("anlz_cue_limit"),
                    QStringLiteral(
                            "Cue list exceeds the supported entry count"));
        }
        for (const auto& resolved : combined->cues) {
            if (cancelled && cancelled())
                return fail(ImportOutcome::Cancelled,
                        QStringLiteral("cancelled"),
                        QStringLiteral("ANLZ import cancelled"));
            const auto& cue = resolved.cue;
            const QString annotation =
                    QStringLiteral("anlz/cue/%1/%2")
                            .arg(bankName,
                                    cue.hotSlot ? QString::number(*cue.hotSlot)
                                                : resolved.identity);
            supplied.insert(annotation);
            if ((cue.hotSlot && *cue.hotSlot > 63) || (cue.enabled && !*cue.enabled)) {
                result.issues.append(issue(
                        QStringLiteral("anlz_cue_capability"),
                        QStringLiteral("Disabled cues and hot slots beyond the "
                                       "supported range remain source records"),
                        annotation));
                continue;
            }
            const auto start = anlzTimestampToPcmFrames(
                    cue.start, media.sampleRate, correctionMilliseconds);
            std::optional<std::int64_t> end;
            if (cue.end) {
                end = anlzTimestampToPcmFrames(*cue.end, media.sampleRate, correctionMilliseconds);
            }
            if (!start || *start > 9007199254740991LL ||
                    (cue.end &&
                            (!end || *end <= *start ||
                                    *end > 9007199254740991LL))) {
                result.issues.append(issue(QStringLiteral("anlz_cue_timing"),
                        QStringLiteral("Corrected cue time cannot be "
                                       "represented as nonnegative PCM frames"),
                        annotation));
                continue;
            }
            if (cue.activeLoop && *cue.activeLoop) {
                result.issues.append(issue(
                        QStringLiteral("active_loop_unsupported"),
                        QStringLiteral(
                                "Loop endpoints are preserved; automatic "
                                "active-loop state is not supported"),
                        annotation));
            }
            if (!cue.rgb && cue.sourceColorId && *cue.sourceColorId != 0) {
                result.issues.append(issue(
                        QStringLiteral("indexed_color_unsupported"),
                        QStringLiteral("Indexed cue color is retained without "
                                       "assuming an RGB palette"),
                        annotation));
            }
            const bool loop = cue.role == AnlzCueRole::Loop;
            const int hot = cue.hotSlot ? static_cast<int>(*cue.hotSlot) : -1;
            const auto type = loop ? CueType::Loop : hot >= 0 ? CueType::HotCue
                                                              : CueType::Memory;
            const int color = cue.rgb
                    ? (cue.rgb->red << 16) | (cue.rgb->green << 8) |
                            cue.rgb->blue
                    : static_cast<int>(
                              loop ? PredefinedColorPalettes::kDefaultLoopColor
                                   : PredefinedColorPalettes::kDefaultCueColor);
            add(annotation,
                    hot >= 0 ? QStringLiteral("cue:hot:%1").arg(hot)
                             : memoryTarget(source->source.id,
                                       external.sourceTrackId,
                                       resolved.identity),
                    QJsonObject{
                            {QStringLiteral("type"), static_cast<int>(type)},
                            {QStringLiteral("start"),
                                    static_cast<double>(*start)},
                            {QStringLiteral("end"),
                                    end ? QJsonValue(static_cast<double>(*end))
                                        : QJsonValue(QJsonValue::Null)},
                            {QStringLiteral("hotCue"), hot},
                            {QStringLiteral("label"),
                                    cue.labelUtf8 ? QString::fromStdString(
                                                            *cue.labelUtf8)
                                                  : QString()},
                            {QStringLiteral("color"), color}},
                    {{QStringLiteral("physicalIdentity"), resolved.identity},
                            {QStringLiteral("correctionMilliseconds"),
                                    QString::number(correctionMilliseconds)},
                            {QStringLiteral("records"), resolved.records}});
        }
    }
    QSqlQuery prior(database);
    QString error;
    if (!run(&prior,
                QStringLiteral("SELECT annotation_id,target_field FROM "
                               "rekordbox_import_provenance_v1 WHERE "
                               "source_id=? AND source_track_id=?"),
                {source->source.id, external.sourceTrackId},
                &error)) {
        return fail(ImportOutcome::Failed, QStringLiteral("previous_annotations"), error);
    }
    while (prior.next()) {
        const auto annotation = prior.value(0).toString();
        const auto parts = annotation.split(QLatin1Char('/'));
        if (annotation.startsWith(QLatin1String("anlz/cue/")) && parts.size() >= 4 &&
                presentBanks.contains(parts[2]) && !supplied.contains(annotation)) {
            snapshot.fields.append({external.sourceTrackId,
                    annotation,
                    prior.value(1).toString(),
                    QJsonValue(QJsonValue::Undefined),
                    true,
                    {{QStringLiteral("removedFromDeclaredSourceBank"), true}}});
        }
    }
    for (auto& value : result.issues)
        value.sourceTrackId = external.sourceTrackId;
    snapshot.decodeIssues = result.issues;
    snapshot.decodeOutcome = result.issues.isEmpty() ? ImportOutcome::Success
                                                     : ImportOutcome::Warning;
    result.outcome = snapshot.decodeOutcome;
    result.snapshot = std::move(snapshot);
    return result;
}

XmlTrackImportResult importAnlzTrack(ImportSourceRegistry* registry,
        const std::shared_ptr<const SourceSnapshot>& source,
        int trackIndex,
        const std::shared_ptr<const AnlzFiles>& files,
        const XmlMediaFacts& media,
        const ImportPolicy& policy,
        const XmlCanonicalAccess& access,
        qint64 correctionMilliseconds,
        const std::function<bool()>& cancelled) {
    if (!registry)
        return {{ImportOutcome::Invalid,
                        false,
                        0,
                        {issue(QStringLiteral("missing_registry"),
                                QStringLiteral("No destination registry"))}},
                0,
                false};
    const auto prepared = prepareAnlzTrackImport(registry->database(),
            source,
            trackIndex,
            files,
            media,
            access,
            correctionMilliseconds,
            cancelled);
    if (!prepared.snapshot)
        return {{prepared.outcome, false, 0, prepared.issues}, 0, false};
    XmlCanonicalAdapter adapter(prepared.target, access);
    const auto plan = registry->plan(*prepared.snapshot, policy, adapter);
    if (!plan.plan)
        return {{plan.outcome, false, 0, plan.issues}, prepared.target.trackId, false};
    const auto applied = registry->apply(*plan.plan, &adapter, cancelled);
    return {applied, prepared.target.trackId, applied.committed && prepared.target.createTrack};
}

} // namespace mixxx::rekordbox
// End of code prepared by ChatGPT for human review and submission.
