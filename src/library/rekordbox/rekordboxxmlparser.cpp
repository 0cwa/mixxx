#include "library/rekordbox/rekordboxxmlparser.h"

#include <QDir>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QUrl>
#include <QXmlStreamReader>
#include <utility>

namespace mixxx::rekordbox {
namespace {

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
    if (!value.isEmpty() && !ok) {
        diagnostic(library,
                Diagnostic::Severity::Warning,
                QStringLiteral("Invalid number in %1: %2").arg(field, value),
                xml);
    }
    return ok ? result : 0.0;
}

void parseTrackChild(QXmlStreamReader* xml, Track* track, Library* library) {
    const auto name = xml->name();
    const auto attributes = xml->attributes();
    if (name == QLatin1String("TEMPO")) {
        track->beatgrid.push_back({real(attribute(attributes, "Inizio"),
                                           QStringLiteral("TEMPO/Inizio"),
                                           library,
                                           *xml),
                real(attribute(attributes, "Bpm"),
                        QStringLiteral("TEMPO/Bpm"),
                        library,
                        *xml)});
    } else if (name == QLatin1String("POSITION_MARK")) {
        CueOrLoop cue;
        cue.name = attribute(attributes, "Name");
        cue.type = attribute(attributes, "Type");
        cue.start = real(attribute(attributes, "Start"),
                QStringLiteral("POSITION_MARK/Start"),
                library,
                *xml);
        cue.end = real(attribute(attributes, "End"),
                QStringLiteral("POSITION_MARK/End"),
                library,
                *xml);
        cue.beatNumber = integer(attribute(attributes, "Num"),
                QStringLiteral("POSITION_MARK/Num"),
                library,
                *xml);
        const QString red = attribute(attributes, "Red");
        const QString green = attribute(attributes, "Green");
        const QString blue = attribute(attributes, "Blue");
        if (!red.isEmpty() || !green.isEmpty() || !blue.isEmpty()) {
            cue.color = red + QLatin1Char(',') + green + QLatin1Char(',') + blue;
        }
        track->cues.push_back(std::move(cue));
    }
    xml->skipCurrentElement();
}

Track parseTrack(QXmlStreamReader* xml, Library* library) {
    const auto attributes = xml->attributes();
    Track track;
    track.id = integer(attribute(attributes, "TrackID"), QStringLiteral("TrackID"), library, *xml);
    track.title = attribute(attributes, "Name");
    track.artist = attribute(attributes, "Artist");
    track.album = attribute(attributes, "Album");
    track.albumArtist = attribute(attributes, "AlbumArtist");
    track.composer = attribute(attributes, "Composer");
    track.genre = attribute(attributes, "Genre");
    track.comment = attribute(attributes, "Comments");
    track.location = normalizeLocation(attribute(attributes, "Location"));
    track.artworkReference = attribute(attributes, "Artwork");
    if (track.artworkReference.isEmpty()) {
        track.artworkReference = attribute(attributes, "ArtworkPath");
    }
    track.key = attribute(attributes, "Tonality");
    track.label = attribute(attributes, "Label");
    track.remixer = attribute(attributes, "Remixer");
    track.kind = attribute(attributes, "Kind");
    track.dateAdded = attribute(attributes, "DateAdded");
    track.year = integer(attribute(attributes, "Year"),
            QStringLiteral("Year"),
            library,
            *xml);
    track.durationSeconds = integer(attribute(attributes, "TotalTime"),
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
    track.sampleRate = integer(attribute(attributes, "SampleRate"),
            QStringLiteral("SampleRate"),
            library,
            *xml);
    track.rating = integer(attribute(attributes, "Rating"),
            QStringLiteral("Rating"),
            library,
            *xml);
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

Playlist parsePlaylist(QXmlStreamReader* xml, Library* library) {
    const auto attributes = xml->attributes();
    Playlist playlist;
    playlist.name = attribute(attributes, "Name");
    playlist.type = integer(attribute(attributes, "Type"),
            QStringLiteral("NODE/Type"),
            library,
            *xml);
    while (xml->readNextStartElement()) {
        if (xml->name() == QLatin1String("PLAYLIST")) {
            const int id = integer(attribute(xml->attributes(), "Key"),
                    QStringLiteral("PLAYLIST/Key"),
                    library,
                    *xml);
            playlist.trackIds.push_back(id);
            xml->skipCurrentElement();
        } else if (xml->name() == QLatin1String("NODE")) {
            playlist.children.push_back(parsePlaylist(xml, library));
        } else {
            xml->skipCurrentElement();
        }
    }
    return playlist;
}

} // namespace

QString normalizeLocation(const QString& location) {
    if (location.isEmpty()) {
        return location;
    }
    const QUrl locationUrl(location);
    const QString host = locationUrl.host();
    if (locationUrl.scheme().compare(QStringLiteral("file"), Qt::CaseInsensitive) == 0 &&
            host.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0) {
        QUrl localUrl = locationUrl;
        localUrl.setHost(QString());
        return QDir::cleanPath(localUrl.toLocalFile());
    }
    if (locationUrl.isLocalFile() && host.isEmpty()) {
        return QDir::cleanPath(locationUrl.toLocalFile());
    }
    if (locationUrl.scheme().isEmpty() || QDir::isAbsolutePath(location)) {
        return QDir::cleanPath(location);
    }
    return location;
}

QByteArray serializeTrackAnnotations(const Track& track) {
    QJsonObject metadata;
    QJsonArray beatgrid;
    for (const Beat& beat : track.beatgrid) {
        QJsonObject item;
        item.insert(QStringLiteral("position"), beat.position);
        item.insert(QStringLiteral("bpm"), beat.bpm);
        beatgrid.append(item);
    }
    metadata.insert(QStringLiteral("beatgrid"), beatgrid);

    QJsonArray cues;
    for (const CueOrLoop& cue : track.cues) {
        QJsonObject item;
        item.insert(QStringLiteral("name"), cue.name);
        item.insert(QStringLiteral("type"), cue.type);
        item.insert(QStringLiteral("start"), cue.start);
        item.insert(QStringLiteral("end"), cue.end);
        item.insert(QStringLiteral("color"), cue.color);
        cues.append(item);
    }
    metadata.insert(QStringLiteral("cues"), cues);
    return QJsonDocument(metadata).toJson(QJsonDocument::Compact);
}

bool Library::hasErrors() const {
    for (const auto& item : diagnostics) {
        if (item.severity == Diagnostic::Severity::Error) {
            return true;
        }
    }
    return false;
}

Library parseXml(QIODevice* device) {
    Library library;
    if (!device) {
        library.diagnostics.push_back(
                {Diagnostic::Severity::Error, QStringLiteral("No XML device supplied"), 0, 0});
        return library;
    }
    QXmlStreamReader xml(device);
    bool foundRoot = false;
    while (xml.readNextStartElement()) {
        if (xml.name() == QLatin1String("DJ_PLAYLISTS")) {
            foundRoot = true;
            library.version = attribute(xml.attributes(), "Version");
            library.product = attribute(xml.attributes(), "Product");
            while (xml.readNextStartElement()) {
                if (xml.name() == QLatin1String("COLLECTION")) {
                    while (xml.readNextStartElement()) {
                        if (xml.name() == QLatin1String("TRACK")) {
                            library.tracks.push_back(parseTrack(&xml, &library));
                        } else {
                            xml.skipCurrentElement();
                        }
                    }
                } else if (xml.name() == QLatin1String("PLAYLISTS")) {
                    while (xml.readNextStartElement()) {
                        if (xml.name() == QLatin1String("NODE")) {
                            library.playlists.children.push_back(parsePlaylist(&xml, &library));
                        } else {
                            xml.skipCurrentElement();
                        }
                    }
                } else {
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
    QSet<QString> locations;
    for (const Track& track : std::as_const(library.tracks)) {
        ++trackIdCounts[track.id];
        const QString normalizedLocation = normalizeLocation(track.location);
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
    return library;
}

} // namespace mixxx::rekordbox
