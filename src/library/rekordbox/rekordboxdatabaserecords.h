// Prepared by ChatGPT for human review.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mixxx::rekordbox {

inline constexpr int kDatabaseRecordContractVersion = 1;

enum class DatabaseOutcome {
    Success,
    Warning,
    Unsupported,
    MissingMedia,
    Denied,
    Corrupt,
    Cancelled,
    Unavailable,
    Failed,
};

struct DatabaseIssue {
    std::string code;
    std::string message;
    std::optional<std::string> recordId;
};

using DatabaseProperties = std::map<std::string, std::optional<std::string>>;

struct DatabaseReadContext {
    std::string sourceId;
    std::uint64_t generation = 0;
    std::string fingerprint;
    std::filesystem::path originalDatabasePath;
    std::filesystem::path mediaRoot;
    std::filesystem::path assetRoot;
    std::shared_ptr<const void> accessLease;
    std::shared_ptr<std::atomic_bool> cancelled;
    std::size_t maximumRecords = 1000000;
    std::size_t maximumStringBytes = 1024 * 1024;

    bool isCancelled() const {
        return cancelled && cancelled->load(std::memory_order_relaxed);
    }
};

enum class DatabaseMediaKind {
    Local,
    Missing,
    Streaming,
    Unavailable,
};

struct DatabaseTrack {
    std::string sourceTrackId;
    std::optional<std::string> title;
    std::optional<std::string> artist;
    std::optional<std::string> album;
    std::optional<std::string> albumArtist;
    std::optional<std::string> composer;
    std::optional<std::string> genre;
    std::optional<std::string> comment;
    std::optional<std::string> key;
    std::optional<std::string> label;
    std::optional<std::string> remixer;
    std::optional<std::string> kind;
    std::optional<std::string> dateAdded;
    std::optional<std::string> dateModified;
    std::optional<std::string> lastPlayed;
    std::optional<std::string> grouping;
    std::optional<std::string> mix;
    std::optional<std::int64_t> year;
    std::optional<std::int64_t> durationMilliseconds;
    std::optional<std::int64_t> trackNumber;
    std::optional<std::int64_t> discNumber;
    std::optional<std::int64_t> bitrate;
    std::optional<std::int64_t> sampleRate;
    std::optional<std::int64_t> rating;
    std::optional<std::int64_t> playCount;
    std::optional<double> bpm;
    std::optional<std::int64_t> colorIndex;
    std::optional<std::uint32_t> colorRgb;
    std::optional<std::string> originalMediaPath;
    std::optional<std::string> originalAnalysisPath;
    std::optional<std::string> originalArtworkPath;
    std::optional<std::filesystem::path> mediaPath;
    std::optional<std::filesystem::path> analysisPath;
    std::optional<std::filesystem::path> artworkPath;
    DatabaseMediaKind mediaKind = DatabaseMediaKind::Unavailable;
    DatabaseProperties properties;
};

enum class DatabasePlaylistKind {
    Folder,
    Regular,
    Smart,
    History,
    Unsupported,
};

struct DatabasePlaylistEntry {
    std::optional<std::string> id;
    std::string sourceTrackId;
    std::int64_t position = 0;
    DatabaseProperties properties;
};

struct DatabasePlaylist {
    std::string id;
    std::optional<std::string> parentId;
    std::string name;
    DatabasePlaylistKind kind = DatabasePlaylistKind::Regular;
    std::int64_t position = 0;
    std::vector<DatabasePlaylistEntry> entries;
    DatabaseProperties properties;
};

enum class DatabaseCueRole {
    MemoryCue,
    MemoryLoop,
    HotCue,
    HotLoop,
};

struct DatabaseAnnotation {
    std::string sourceAnnotationId;
    std::string sourceTrackId;
    DatabaseCueRole role = DatabaseCueRole::MemoryCue;
    std::optional<std::uint32_t> hotSlot;
    std::int64_t startMilliseconds = 0;
    std::optional<std::int64_t> endMilliseconds;
    std::optional<std::string> label;
    std::optional<std::int64_t> colorIndex;
    std::optional<std::uint32_t> colorRgb;
    std::optional<bool> activeLoop;
    std::optional<std::int64_t> startFrame150;
    std::optional<std::int64_t> endFrame150;
    DatabaseProperties properties;
};

struct DatabaseCapabilities {
    bool metadata = false;
    bool orderedPlaylists = false;
    bool databaseAnnotations = false;
    bool analysisLocators = false;
    bool artworkLocators = false;
    bool canonicalApplication = false;
    bool qualifiedPerformanceData = false;
    std::vector<std::string> limitations;
};

struct DatabaseOpaqueRecord {
    std::string table;
    DatabaseProperties properties;
};

struct DatabaseLibrary {
    DatabaseReadContext context;
    std::string format;
    std::string schemaProfile;
    std::optional<std::string> databaseIdentity;
    std::optional<std::string> producerVersion;
    DatabaseCapabilities capabilities;
    std::vector<DatabaseTrack> tracks;
    std::vector<DatabasePlaylist> playlists;
    std::vector<DatabaseAnnotation> annotations;
    std::vector<DatabaseOpaqueRecord> opaqueRecords;
    DatabaseProperties properties;
};

struct DatabaseReadResult {
    DatabaseOutcome outcome = DatabaseOutcome::Failed;
    std::vector<DatabaseIssue> issues;
    std::shared_ptr<const DatabaseLibrary> library;
};

} // namespace mixxx::rekordbox
// End of code prepared by ChatGPT for human review and submission.
