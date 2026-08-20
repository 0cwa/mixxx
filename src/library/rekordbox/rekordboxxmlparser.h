#pragma once

#include <QByteArray>
#include <QIODevice>
#include <QString>
#include <QVector>
#include <optional>

namespace mixxx::rekordbox {

struct Diagnostic {
    enum class Severity { Warning,
        Error };
    Severity severity = Severity::Warning;
    QString message;
    qint64 line = 0;
    qint64 column = 0;
};

struct Beat {
    double position = 0.0;
    double bpm = 0.0;
};

struct CueOrLoop {
    QString name;
    QString type;
    double start = 0.0;
    double end = 0.0;
    int beatNumber = 0;
    QString color;
};

struct Track {
    int id = 0;
    QString title;
    QString artist;
    QString album;
    QString albumArtist;
    QString composer;
    QString genre;
    QString comment;
    QString location;
    QString artworkReference;
    QString key;
    QString label;
    QString remixer;
    QString kind;
    QString dateAdded;
    int year = 0;
    int durationSeconds = 0;
    int trackNumber = 0;
    int discNumber = 0;
    int bitrate = 0;
    int sampleRate = 0;
    int rating = 0;
    int playCount = 0;
    double bpm = 0.0;
    QVector<Beat> beatgrid;
    QVector<CueOrLoop> cues;
};

struct Playlist {
    QString name;
    int type = 0;
    QVector<int> trackIds;
    QVector<Playlist> children;
};

struct Library {
    QString version;
    QString product;
    QVector<Track> tracks;
    Playlist playlists;
    QVector<Diagnostic> diagnostics;

    bool hasErrors() const;
};

/// Parses the documented Rekordbox XML V1 read-only interchange fields.
/// The returned collection is independent of Mixxx Track/DAO objects.
Library parseXml(QIODevice* device);

/// Normalizes a Rekordbox XML location for use by Mixxx's external track model.
/// Localhost file URLs are converted to native local paths; remote file URLs
/// and other non-local locations are preserved.
QString normalizeLocation(const QString& location);

/// Serializes only the per-track timing annotations needed by the temporary
/// external-library model. The source XML remains untouched.
QByteArray serializeTrackAnnotations(const Track& track);

} // namespace mixxx::rekordbox
