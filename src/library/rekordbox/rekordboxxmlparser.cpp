#include "library/rekordbox/rekordboxxmlparser.h"

#include <QCryptographicHash>
#include <QDir>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QUrl>
#include <QXmlStreamReader>
#include <cmath>
#include <utility>

#include "util/fpclassify.h"

namespace mixxx::rekordbox {
namespace {

// Prepared by ChatGPT for human review. Hash every diagnostic dimension so a
// newly decoded warning cannot be hidden by an unchanged raw source fingerprint.
QByteArray diagnosticFingerprint(const QVector<Diagnostic>& diagnostics) {
    QJsonArray values;
    for (const auto& item : diagnostics) {
        values.append(QJsonArray{static_cast<int>(item.severity),
                item.message,
                QString::number(item.line),
                QString::number(item.column)});
    }
    return QCryptographicHash::hash(QJsonDocument(values).toJson(QJsonDocument::Compact),
            QCryptographicHash::Sha256);
}
// End of ChatGPT-generated helper.

void diagnostic(Library* library,
        Diagnostic::Severity severity,
        const QString& message,
        const QXmlStreamReader& xml) {
    library->diagnostics.push_back(
            {severity, message, xml.lineNumber(), xml.columnNumber()});
}

QString attribute(const QXmlStreamAttributes& attributes, const char* name) {
    return attributes.value(QLatin1String(name)).toString();
}

QMap<QString, QString> attributeMap(const QXmlStreamAttributes& attributes) {
    QMap<QString, QString> result;
    for (const auto& item : attributes) {
        result.insert(item.qualifiedName().toString(), item.value().toString());
    }
    return result;
}

int integer(const QString& value,
        const QString& field,
        Library* library,
        const QXmlStreamReader& xml) {
    bool ok = false;
    const int result = value.toInt(&ok);
    if (!value.isEmpty() && !ok) {
        diagnostic(library,
                Diagnostic::Severity::Warning,
                QStringLiteral("Invalid integer in %1: %2").arg(field, value),
                xml);
    }
    return ok ? result : 0;
}

double real(const QString& value,
        const QString& field,
        Library* library,
        const QXmlStreamReader& xml) {
    bool ok = false;
    const double result = value.toDouble(&ok);
    if (!value.isEmpty() && (!ok || !util_isfinite(result))) {
        diagnostic(library,
                Diagnostic::Severity::Warning,
                QStringLiteral("Invalid number in %1: %2").arg(field, value),
                xml);
    }
    return ok && util_isfinite(result) ? result : 0.0;
}

void parseTrackChild(QXmlStreamReader* xml, Track* track, Library* library) {
    if (!xml->namespaceUri().isEmpty()) {
        diagnostic(library,
                Diagnostic::Severity::Warning,
                QStringLiteral("Unsupported namespaced TRACK child: %1")
                        .arg(xml->qualifiedName().toString()),
                *xml);
        xml->skipCurrentElement();
        return;
    }
    const auto name = xml->name();
    const auto attributes = xml->attributes();
    if (name == QLatin1String("TEMPO")) {
        Beat segment;
        segment.attributes = attributeMap(attributes);
        bool positionOk = false;
        bool bpmOk = false;
        segment.position = attribute(attributes, "Inizio").toDouble(&positionOk);
        segment.bpm = attribute(attributes, "Bpm").toDouble(&bpmOk);
        if (!positionOk || !bpmOk || !util_isfinite(segment.position) ||
                !util_isfinite(segment.bpm) || segment.position < 0.0 || segment.bpm <= 0.0) {
            diagnostic(library,
                    Diagnostic::Severity::Error,
                    QStringLiteral("Invalid TEMPO/Inizio or TEMPO/Bpm"),
                    *xml);
        } else {
            const QString meter = attribute(attributes, "Metro");
            if (attributes.hasAttribute(QLatin1String("Metro"))) {
                const auto parts = meter.split(QLatin1Char('/'));
                bool numeratorOk = false;
                bool denominatorOk = false;
                const int numerator = parts.value(0).toInt(&numeratorOk);
                const int denominator = parts.value(1).toInt(&denominatorOk);
                if (parts.size() != 2 || !numeratorOk || !denominatorOk ||
                        numerator <= 0 || denominator <= 0) {
                    diagnostic(library,
                            Diagnostic::Severity::Error,
                            QStringLiteral("Invalid TEMPO/Metro: %1")
                                    .arg(meter),
                            *xml);
                } else {
                    segment.meterNumerator = numerator;
                    segment.meterDenominator = denominator;
                }
            }
            const QString beatInBar = attribute(attributes, "Battito");
            if (attributes.hasAttribute(QLatin1String("Battito"))) {
                bool ok = false;
                const int beat = beatInBar.toInt(&ok);
                if (!ok || beat < 1 ||
                        (segment.meterNumerator && beat > *segment.meterNumerator)) {
                    diagnostic(library,
                            Diagnostic::Severity::Error,
                            QStringLiteral("Invalid TEMPO/Battito: %1")
                                    .arg(beatInBar),
                            *xml);
                } else {
                    segment.beatInBar = beat;
                }
            }
            if (!track->beatgrid.isEmpty() && segment.position <= track->beatgrid.back().position) {
                diagnostic(library,
                        Diagnostic::Severity::Error,
                        QStringLiteral("TEMPO anchors must be in strictly "
                                       "increasing order"),
                        *xml);
            }
            track->beatgrid.push_back(std::move(segment));
        }
    } else if (name == QLatin1String("POSITION_MARK")) {
        CueOrLoop cue;
        cue.attributes = attributeMap(attributes);
        cue.sourceOrdinal = track->cues.size();
        cue.name = attribute(attributes, "Name");
        bool typeOk = false;
        const int type = attribute(attributes, "Type").toInt(&typeOk);
        const QStringList types = {QStringLiteral("cue"),
                QStringLiteral("fade-in"),
                QStringLiteral("fade-out"),
                QStringLiteral("load"),
                QStringLiteral("loop")};
        if (!typeOk || type < 0 || type >= types.size()) {
            cue.type = QStringLiteral("unsupported");
            cue.supported = false;
            diagnostic(library,
                    Diagnostic::Severity::Warning,
                    QStringLiteral("Unsupported POSITION_MARK/Type: %1")
                            .arg(attribute(attributes, "Type")),
                    *xml);
        } else {
            cue.type = types[type];
        }
        bool startOk = false;
        cue.start = attribute(attributes, "Start").toDouble(&startOk);
        if (!startOk || !util_isfinite(cue.start) || cue.start < 0.0) {
            cue.supported = false;
            diagnostic(library,
                    Diagnostic::Severity::Error,
                    QStringLiteral("Invalid POSITION_MARK/Start"),
                    *xml);
        }
        cue.hasEnd = attributes.hasAttribute(QLatin1String("End"));
        if (cue.hasEnd) {
            bool endOk = false;
            cue.end = attribute(attributes, "End").toDouble(&endOk);
            if (!endOk || !util_isfinite(cue.end) || cue.end < 0.0) {
                cue.supported = false;
                diagnostic(library,
                        Diagnostic::Severity::Error,
                        QStringLiteral("Invalid POSITION_MARK/End"),
                        *xml);
            }
        }
        if (cue.type == QLatin1String("loop") && (!cue.hasEnd || cue.end <= cue.start)) {
            cue.supported = false;
            diagnostic(library,
                    Diagnostic::Severity::Error,
                    QStringLiteral("Loop End must be later than Start"),
                    *xml);
        }
        if (attributes.hasAttribute(QLatin1String("Num"))) {
            bool numberOk = false;
            const int number = attribute(attributes, "Num").toInt(&numberOk);
            if (!numberOk || number < -1) {
                cue.supported = false;
                diagnostic(library,
                        Diagnostic::Severity::Error,
                        QStringLiteral("Invalid POSITION_MARK/Num"),
                        *xml);
            } else {
                cue.number = number;
                if (number >= 0) {
                    cue.hotCue = number;
                }
            }
        }
        const QString red = attribute(attributes, "Red");
        const QString green = attribute(attributes, "Green");
        const QString blue = attribute(attributes, "Blue");
        if (attributes.hasAttribute(QLatin1String("Red")) ||
                attributes.hasAttribute(QLatin1String("Green")) ||
                attributes.hasAttribute(QLatin1String("Blue"))) {
            bool redOk = false;
            bool greenOk = false;
            bool blueOk = false;
            const int r = red.toInt(&redOk);
            const int g = green.toInt(&greenOk);
            const int b = blue.toInt(&blueOk);
            if (redOk && greenOk && blueOk && r >= 0 && r <= 255 &&
                    g >= 0 && g <= 255 && b >= 0 && b <= 255) {
                cue.color = QStringLiteral("%1,%2,%3").arg(r).arg(g).arg(b);
            } else {
                diagnostic(library,
                        Diagnostic::Severity::Warning,
                        QStringLiteral("Invalid POSITION_MARK RGB extension"),
                        *xml);
            }
        }
        track->cues.push_back(std::move(cue));
    } else {
        diagnostic(library,
                Diagnostic::Severity::Warning,
                QStringLiteral("Unsupported TRACK child: %1")
                        .arg(name.toString()),
                *xml);
    }
    xml->skipCurrentElement();
}

Track parseTrack(QXmlStreamReader* xml, Library* library) {
    const auto attributes = xml->attributes();
    Track track;
    track.attributes = attributeMap(attributes);
    track.sourceOrdinal = library->tracks.size();
    track.id = attribute(attributes, "TrackID").toInt(&track.hasValidId);
    if (attributes.hasAttribute(QLatin1String("TrackID")) && !track.hasValidId) {
        diagnostic(library,
                Diagnostic::Severity::Error,
                QStringLiteral("Invalid TrackID: %1")
                        .arg(attribute(attributes, "TrackID")),
                *xml);
    }
    track.title = attribute(attributes, "Name");
    track.artist = attribute(attributes, "Artist");
    track.album = attribute(attributes, "Album");
    track.albumArtist = attribute(attributes, "AlbumArtist");
    track.composer = attribute(attributes, "Composer");
    track.genre = attribute(attributes, "Genre");
    track.comment = attribute(attributes, "Comments");
    track.location = normalizeLocation(attribute(attributes, "Location"));
    if (attributes.hasAttribute(QLatin1String("Location")) &&
            !attribute(attributes, "Location").isEmpty() && track.location.isEmpty()) {
        diagnostic(library,
                Diagnostic::Severity::Warning,
                QStringLiteral("Invalid or unsupported file URI; media remains "
                               "unresolved"),
                *xml);
    }
    track.artworkReference = attribute(attributes, "Artwork");
    if (track.artworkReference.isEmpty()) {
        track.artworkReference = attribute(attributes, "ArtworkPath");
    }
    track.key = attribute(attributes, "Tonality");
    track.label = attribute(attributes, "Label");
    track.remixer = attribute(attributes, "Remixer");
    track.kind = attribute(attributes, "Kind");
    track.dateAdded = attribute(attributes, "DateAdded");
    track.dateModified = attribute(attributes, "DateModified");
    track.lastPlayed = attribute(attributes, "LastPlayed");
    track.grouping = attribute(attributes, "Grouping");
    track.mix = attribute(attributes, "Mix");
    track.color = attribute(attributes, "Colour");
    track.year = integer(attribute(attributes, "Year"),
            QStringLiteral("Year"),
            library,
            *xml);
    track.durationSeconds = real(attribute(attributes, "TotalTime"),
            QStringLiteral("TotalTime"),
            library,
            *xml);
    track.trackNumber = integer(attribute(attributes, "TrackNumber"),
            QStringLiteral("TrackNumber"),
            library,
            *xml);
    track.discNumber = integer(attribute(attributes, "DiscNumber"),
            QStringLiteral("DiscNumber"),
            library,
            *xml);
    track.bitrate = integer(attribute(attributes, "BitRate"),
            QStringLiteral("BitRate"),
            library,
            *xml);
    track.sampleRate = real(attribute(attributes, "SampleRate"),
            QStringLiteral("SampleRate"),
            library,
            *xml);
    track.rating = integer(attribute(attributes, "Rating"),
            QStringLiteral("Rating"),
            library,
            *xml);
    if (track.rating < 0 || track.rating > 255 || track.rating % 51 != 0) {
        diagnostic(library,
                Diagnostic::Severity::Warning,
                QStringLiteral(
                        "Rating must be one of 0, 51, 102, 153, 204, 255"),
                *xml);
        track.rating = 0;
    } else {
        track.rating /= 51;
    }
    track.playCount = integer(attribute(attributes, "PlayCount"),
            QStringLiteral("PlayCount"),
            library,
            *xml);
    track.bpm = real(attribute(attributes, "AverageBpm"),
            QStringLiteral("AverageBpm"),
            library,
            *xml);

    while (xml->readNextStartElement()) {
        parseTrackChild(xml, &track, library);
    }
    return track;
}

Playlist parsePlaylist(QXmlStreamReader* xml,
        Library* library,
        const QString& parentId,
        int* nextId) {
    const auto attributes = xml->attributes();
    Playlist playlist;
    playlist.id = QStringLiteral("xml-node-%1").arg((*nextId)++);
    playlist.parentId = parentId;
    playlist.name = attribute(attributes, "Name");
    bool typeOk = false;
    playlist.type = attribute(attributes, "Type").toInt(&typeOk);
    if (!typeOk) {
        playlist.type = -1;
    }
    if (attributes.hasAttribute(QLatin1String("KeyType"))) {
        bool keyTypeOk = false;
        playlist.keyType = attribute(attributes, "KeyType").toInt(&keyTypeOk);
        if (!keyTypeOk) {
            playlist.keyType = -1;
        }
    }
    if (playlist.type != 0 && playlist.type != 1) {
        diagnostic(library,
                Diagnostic::Severity::Warning,
                QStringLiteral("Unsupported NODE/Type: %1").arg(playlist.type),
                *xml);
    }
    if (playlist.keyType != 0 && playlist.keyType != 1) {
        diagnostic(library,
                Diagnostic::Severity::Warning,
                QStringLiteral("Unsupported NODE/KeyType: %1")
                        .arg(playlist.keyType),
                *xml);
    }
    while (xml->readNextStartElement()) {
        if (!xml->namespaceUri().isEmpty()) {
            diagnostic(library,
                    Diagnostic::Severity::Warning,
                    QStringLiteral("Unsupported namespaced NODE child: %1")
                            .arg(xml->qualifiedName().toString()),
                    *xml);
            xml->skipCurrentElement();
        } else if (xml->name() == QLatin1String("TRACK")) {
            playlist.entries.push_back({attribute(xml->attributes(), "Key"),
                    std::nullopt,
                    static_cast<int>(playlist.entries.size()),
                    std::nullopt});
            xml->skipCurrentElement();
        } else if (xml->name() == QLatin1String("NODE")) {
            playlist.children.push_back(parsePlaylist(xml, library, playlist.id, nextId));
        } else {
            diagnostic(library,
                    Diagnostic::Severity::Warning,
                    QStringLiteral("Unsupported NODE child: %1")
                            .arg(xml->name().toString()),
                    *xml);
            xml->skipCurrentElement();
        }
    }
    return playlist;
}

void resolvePlaylist(Playlist* playlist,
        const QHash<int, int>& ids,
        const QHash<int, int>& idIndices,
        const QHash<QString, QVector<int>>& locations,
        Library* library) {
    for (auto& entry : playlist->entries) {
        if (playlist->type == 1 && playlist->keyType == 0) {
            bool ok = false;
            const int id = entry.key.toInt(&ok);
            if (ok && ids.value(id) == 1) {
                entry.trackId = id;
                entry.trackIndex = idIndices.value(id);
            }
        } else if (playlist->type == 1 && playlist->keyType == 1) {
            const auto candidates = locations.value(normalizeLocation(entry.key));
            if (candidates.size() == 1) {
                const int index = candidates.front();
                const auto& track = library->tracks[index];
                entry.trackIndex = index;
                if (track.hasValidId && ids.value(track.id) == 1) {
                    entry.trackId = track.id;
                }
            }
        }
        if (entry.trackId) {
            playlist->trackIds.push_back(*entry.trackId);
        }
        if (!entry.trackIndex) {
            library->diagnostics.push_back({Diagnostic::Severity::Warning,
                    QStringLiteral("Unresolved or ambiguous playlist Key: %1").arg(entry.key),
                    0,
                    0});
        }
    }
    for (auto& child : playlist->children) {
        resolvePlaylist(&child, ids, idIndices, locations, library);
    }
}

} // namespace

QString normalizeLocation(const QString& location) {
    if (location.isEmpty()) {
        return location;
    }
    if (QDir::isAbsolutePath(location)) {
        return QDir::cleanPath(location);
    }
    const QUrl locationUrl(location, QUrl::StrictMode);
    const QString host = locationUrl.host();
    const bool fileScheme = location.startsWith(QLatin1String("file:"), Qt::CaseInsensitive);
    if (fileScheme) {
        if (!locationUrl.isValid() || locationUrl.hasQuery() || locationUrl.hasFragment()) {
            return {};
        }
        const QByteArray pathBytes = QByteArray::fromPercentEncoding(
                locationUrl.path(QUrl::FullyEncoded).toUtf8());
        if (pathBytes.contains('\0') || QString::fromUtf8(pathBytes).toUtf8() != pathBytes) {
            return {};
        }
    }
    if (fileScheme &&
            host.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0) {
        QUrl localUrl = locationUrl;
        localUrl.setHost(QString());
        const QString path = localUrl.toLocalFile();
        return !path.isEmpty() && QDir::isAbsolutePath(path) ? QDir::cleanPath(path) : QString();
    }
    if (locationUrl.isLocalFile() && host.isEmpty()) {
        const QString path = locationUrl.toLocalFile();
        return !path.isEmpty() && QDir::isAbsolutePath(path) ? QDir::cleanPath(path) : QString();
    }
    if (locationUrl.scheme().isEmpty() || QDir::isAbsolutePath(location)) {
        return QDir::cleanPath(location);
    }
    return location;
}

QByteArray serializeTrackAnnotations(const Track& track) {
    QJsonObject metadata;
    metadata.insert(QStringLiteral("schemaVersion"), 2);
    metadata.insert(QStringLiteral("gridKind"), QStringLiteral("tempo-segments"));
    QJsonObject fields;
    for (auto it = track.attributes.cbegin(); it != track.attributes.cend(); ++it) {
        fields.insert(it.key(), it.value());
    }
    metadata.insert(QStringLiteral("fields"), fields);
    QJsonArray beatgrid;
    for (const Beat& beat : track.beatgrid) {
        QJsonObject item;
        item.insert(QStringLiteral("position"), beat.position);
        item.insert(QStringLiteral("bpm"), beat.bpm);
        if (beat.meterNumerator) {
            item.insert(QStringLiteral("meterNumerator"), *beat.meterNumerator);
            item.insert(QStringLiteral("meterDenominator"), *beat.meterDenominator);
        }
        if (beat.beatInBar) {
            item.insert(QStringLiteral("beatInBar"), *beat.beatInBar);
        }
        QJsonObject attributes;
        for (auto it = beat.attributes.cbegin(); it != beat.attributes.cend(); ++it) {
            attributes.insert(it.key(), it.value());
        }
        item.insert(QStringLiteral("attributes"), attributes);
        beatgrid.append(item);
    }
    metadata.insert(QStringLiteral("beatgrid"), beatgrid);

    QJsonArray cues;
    for (const CueOrLoop& cue : track.cues) {
        QJsonObject item;
        item.insert(QStringLiteral("name"), cue.name);
        item.insert(QStringLiteral("type"), cue.type);
        item.insert(QStringLiteral("sourceType"), cue.attributes.value(QStringLiteral("Type")));
        item.insert(QStringLiteral("sourceOrdinal"), cue.sourceOrdinal);
        item.insert(QStringLiteral("supported"), cue.supported);
        item.insert(QStringLiteral("start"), cue.start);
        if (cue.hasEnd) {
            item.insert(QStringLiteral("end"), cue.end);
        }
        if (cue.number) {
            item.insert(QStringLiteral("number"), *cue.number);
        }
        if (cue.hotCue) {
            item.insert(QStringLiteral("hotCue"), *cue.hotCue);
        }
        if (!cue.color.isEmpty()) {
            item.insert(QStringLiteral("color"), cue.color);
        }
        QJsonObject attributes;
        for (auto it = cue.attributes.cbegin(); it != cue.attributes.cend(); ++it) {
            attributes.insert(it.key(), it.value());
        }
        item.insert(QStringLiteral("attributes"), attributes);
        cues.append(item);
    }
    metadata.insert(QStringLiteral("cues"), cues);
    return QJsonDocument(metadata).toJson(QJsonDocument::Compact);
}

// Prepared by ChatGPT: retain Qt's implicitly shared vectors as the index's
// identity guard. Mutating a copied DTO detaches its storage, so stale counts or
// diagnostic hashes cannot be reused. Frozen snapshots keep constant-time access.
struct Library::ValidationIndex {
    QVector<Track> tracks;
    QVector<Diagnostic> diagnostics;
    QHash<int, int> ids;
    QHash<QString, int> locations;
    QByteArray diagnosticHash;
    bool hasErrors = false;
};

void Library::freezeValidationIndex() {
    auto index = std::make_shared<ValidationIndex>();
    index->tracks = tracks;
    index->diagnostics = diagnostics;
    for (const auto& track : std::as_const(tracks)) {
        if (track.hasValidId) {
            ++index->ids[track.id];
        }
        ++index->locations[track.location];
    }
    for (const auto& item : std::as_const(diagnostics)) {
        index->hasErrors |= item.severity == Diagnostic::Severity::Error;
    }
    index->diagnosticHash = diagnosticFingerprint(diagnostics);
    m_validationIndex = std::move(index);
}

int Library::sourceIdentityMatches(int trackIndex) const {
    if (trackIndex < 0 || trackIndex >= tracks.size()) {
        return 0;
    }
    const auto& track = tracks[trackIndex];
    if (m_validationIndex && tracks.size() == m_validationIndex->tracks.size() &&
            tracks.constData() == m_validationIndex->tracks.constData()) {
        return track.hasValidId
                ? m_validationIndex->ids.value(track.id)
                : m_validationIndex->locations.value(track.location);
    }
    int matches = 0;
    for (const auto& other : tracks) {
        if (track.hasValidId ? (other.hasValidId && other.id == track.id)
                             : other.location == track.location) {
            ++matches;
        }
    }
    return matches;
}

QByteArray Library::diagnosticsFingerprint() const {
    if (m_validationIndex && diagnostics.size() == m_validationIndex->diagnostics.size() &&
            diagnostics.constData() == m_validationIndex->diagnostics.constData()) {
        return m_validationIndex->diagnosticHash;
    }
    return diagnosticFingerprint(diagnostics);
}

bool Library::hasErrors() const {
    if (m_validationIndex && diagnostics.size() == m_validationIndex->diagnostics.size() &&
            diagnostics.constData() == m_validationIndex->diagnostics.constData()) {
        return m_validationIndex->hasErrors;
    }
    for (const auto& item : diagnostics) {
        if (item.severity == Diagnostic::Severity::Error) {
            return true;
        }
    }
    return false;
}
// End of ChatGPT-generated validation index.

Library parseXml(QIODevice* device) {
    Library library;
    if (!device) {
        library.diagnostics.push_back(
                {Diagnostic::Severity::Error, QStringLiteral("No XML device supplied"), 0, 0});
        return library;
    }
    QXmlStreamReader xml(device);
    bool foundRoot = false;
    int nextPlaylistId = 0;
    if (xml.readNextStartElement()) {
        if (xml.namespaceUri().isEmpty() && xml.name() == QLatin1String("DJ_PLAYLISTS")) {
            foundRoot = true;
            library.version = attribute(xml.attributes(), "Version");
            if (!library.version.isEmpty() && library.version != QLatin1String("1.0.0")) {
                diagnostic(&library,
                        Diagnostic::Severity::Error,
                        QStringLiteral("Unsupported DJ_PLAYLISTS Version: %1")
                                .arg(library.version),
                        xml);
            }
            library.product = attribute(xml.attributes(), "Product");
            while (xml.readNextStartElement()) {
                if (!xml.namespaceUri().isEmpty()) {
                    diagnostic(&library,
                            Diagnostic::Severity::Warning,
                            QStringLiteral(
                                    "Unsupported namespaced document child: %1")
                                    .arg(xml.qualifiedName().toString()),
                            xml);
                    xml.skipCurrentElement();
                } else if (xml.name() == QLatin1String("PRODUCT")) {
                    library.product = attribute(xml.attributes(), "Name");
                    library.productVersion = attribute(xml.attributes(), "Version");
                    library.productCompany = attribute(xml.attributes(), "Company");
                    xml.skipCurrentElement();
                } else if (xml.name() == QLatin1String("COLLECTION")) {
                    while (xml.readNextStartElement()) {
                        if (xml.namespaceUri().isEmpty() && xml.name() == QLatin1String("TRACK")) {
                            library.tracks.push_back(parseTrack(&xml, &library));
                        } else {
                            diagnostic(&library,
                                    Diagnostic::Severity::Warning,
                                    QStringLiteral(
                                            "Unsupported COLLECTION child: %1")
                                            .arg(xml.qualifiedName()
                                                            .toString()),
                                    xml);
                            xml.skipCurrentElement();
                        }
                    }
                } else if (xml.name() == QLatin1String("PLAYLISTS")) {
                    while (xml.readNextStartElement()) {
                        if (xml.namespaceUri().isEmpty() && xml.name() == QLatin1String("NODE")) {
                            library.playlists.children.push_back(parsePlaylist(
                                    &xml, &library, QString(), &nextPlaylistId));
                        } else {
                            diagnostic(&library,
                                    Diagnostic::Severity::Warning,
                                    QStringLiteral(
                                            "Unsupported PLAYLISTS child: %1")
                                            .arg(xml.qualifiedName()
                                                            .toString()),
                                    xml);
                            xml.skipCurrentElement();
                        }
                    }
                } else {
                    diagnostic(&library,
                            Diagnostic::Severity::Warning,
                            QStringLiteral("Unsupported document child: %1")
                                    .arg(xml.name().toString()),
                            xml);
                    xml.skipCurrentElement();
                }
            }
        } else {
            diagnostic(&library,
                    Diagnostic::Severity::Warning,
                    QStringLiteral("Unexpected root element: %1")
                            .arg(xml.name().toString()),
                    xml);
            xml.skipCurrentElement();
        }
    }
    while (!xml.atEnd()) {
        xml.readNext();
    }
    if (!foundRoot && !xml.hasError()) {
        diagnostic(&library,
                Diagnostic::Severity::Error,
                QStringLiteral("Missing DJ_PLAYLISTS root element"),
                xml);
    }
    if (xml.hasError()) {
        diagnostic(&library, Diagnostic::Severity::Error, xml.errorString(), xml);
    }

    QHash<int, int> trackIdCounts;
    QHash<int, int> trackIdIndices;
    QHash<QString, QVector<int>> locationTrackIds;
    QSet<QString> locations;
    for (const Track& track : std::as_const(library.tracks)) {
        if (track.hasValidId) {
            ++trackIdCounts[track.id];
            trackIdIndices.insert(track.id, track.sourceOrdinal);
        }
        const QString normalizedLocation = normalizeLocation(track.location);
        if (!normalizedLocation.isEmpty()) {
            locationTrackIds[normalizedLocation].push_back(track.sourceOrdinal);
        }
        if (normalizedLocation.isEmpty()) {
            library.diagnostics.push_back({Diagnostic::Severity::Warning,
                    QStringLiteral("Track %1 has no Location and will be "
                                   "skipped during import")
                            .arg(track.id),
                    0,
                    0});
        } else if (locations.contains(normalizedLocation)) {
            library.diagnostics.push_back(
                    {Diagnostic::Severity::Warning,
                            QStringLiteral("Duplicate track Location: %1").arg(normalizedLocation),
                            0,
                            0});
        } else {
            locations.insert(normalizedLocation);
        }
    }
    for (auto it = trackIdCounts.cbegin(); it != trackIdCounts.cend(); ++it) {
        if (it.value() > 1) {
            library.diagnostics.push_back(
                    {Diagnostic::Severity::Warning,
                            QStringLiteral("Duplicate TrackID: %1").arg(it.key()),
                            0,
                            0});
        }
    }
    resolvePlaylist(&library.playlists, trackIdCounts, trackIdIndices, locationTrackIds, &library);
    return library;
}

XmlDecodeResult decodeXml(QIODevice* device) {
    Library library = parseXml(device);
    XmlDecodeResult result{XmlOutcome::Success, {}, library.diagnostics};
    if (!library.version.isEmpty() && library.version != QLatin1String("1.0.0")) {
        result.outcome = XmlOutcome::Unsupported;
    } else if (library.hasErrors()) {
        result.outcome = XmlOutcome::Invalid;
    } else {
        if (!library.diagnostics.isEmpty()) {
            result.outcome = XmlOutcome::Warning;
        }
        library.freezeValidationIndex();
        result.snapshot = std::make_shared<const Library>(std::move(library));
    }
    return result;
}

} // namespace mixxx::rekordbox
