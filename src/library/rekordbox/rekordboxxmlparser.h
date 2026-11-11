#pragma once

#include <QByteArray>
#include <QIODevice>
#include <QMap>
#include <QString>
#include <QVector>
#include <memory>
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
    std::optional<int> meterNumerator;
    std::optional<int> meterDenominator;
    std::optional<int> beatInBar;
    QMap<QString, QString> attributes;
};

struct CueOrLoop {
    QString name;
    QString type;
    double start = 0.0;
    double end = 0.0;
    bool hasEnd = false;
    std::optional<int> number;
    std::optional<int> hotCue;
    int sourceOrdinal = 0;
    bool supported = true;
    QString color;
    QMap<QString, QString> attributes;
};

struct Track {
    int id = 0;
    bool hasValidId = false;
    int sourceOrdinal = 0;
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
    QString dateModified;
    QString lastPlayed;
    QString grouping;
    QString mix;
    QString color;
    QMap<QString, QString> attributes;
    int year = 0;
    double durationSeconds = 0.0;
    int trackNumber = 0;
    int discNumber = 0;
    int bitrate = 0;
    double sampleRate = 0.0;
    int rating = 0;
    int playCount = 0;
    double bpm = 0.0;
    QVector<Beat> beatgrid;
    QVector<CueOrLoop> cues;
};

struct PlaylistEntry {
    QString key;
    std::optional<int> trackId;
    int ordinal = 0;
    std::optional<int> trackIndex;
};

struct Playlist {
    // Prepared by ChatGPT: These IDs are local to a decoded snapshot. XML has
    // no persistent node ID; the registry must reconcile structural edits.
    // End of ChatGPT-generated note.
    QString id;
    QString parentId;
    QString name;
    int type = 0;
    int keyType = 0;
    QVector<PlaylistEntry> entries;
    QVector<int> trackIds;
    QVector<Playlist> children;
};

struct XmlDecodeResult;

struct Library {
    QString version;
    QString product;
    QString productVersion;
    QString productCompany;
    QVector<Track> tracks;
    Playlist playlists;
    QVector<Diagnostic> diagnostics;

    bool hasErrors() const;
    // Prepared by ChatGPT: decoded snapshots use immutable indexes. Copies
    // whose public vectors are edited automatically use the defensive scan.
    // End of ChatGPT-generated note.
    int sourceIdentityMatches(int trackIndex) const;
    QByteArray diagnosticsFingerprint() const;

  private:
    struct ValidationIndex;
    std::shared_ptr<const ValidationIndex> m_validationIndex;
    void freezeValidationIndex();
    friend XmlDecodeResult decodeXml(QIODevice* device);
};

enum class XmlOutcome {
    Success,
    Warning,
    Unsupported,
    Invalid,
    Denied,
    Unavailable,
    Cancelled,
    Failed
};

struct XmlDecodeResult {
    XmlOutcome outcome;
    std::shared_ptr<const Library> snapshot;
    QVector<Diagnostic> diagnostics;
};

// Prepared by ChatGPT: Publish a frozen record graph only after a valid parse;
// retain diagnostics without partial data on invalid or unsupported input.
// End of ChatGPT-generated interface note.
XmlDecodeResult decodeXml(QIODevice* device);

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
