#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mixxx::rekordbox {

enum class PdbStatus {
    Success,
    Warning,
    Unsupported,
    Invalid,
    Unavailable,
    Denied,
    Cancelled,
    Failed,
};

struct PdbRecordLocation {
    std::uint32_t pageIndex{0};
    std::size_t rowIndex{0};
    std::size_t byteOffset{0};
};

struct PdbNamedRecord {
    std::uint32_t sourceId{0};
    std::string nameUtf8;
    std::optional<std::uint32_t> artistId;
    PdbRecordLocation location;
};

struct PdbArtwork {
    std::uint32_t sourceId{0};
    std::string pathUtf8;
    PdbRecordLocation location;
};

struct PdbTrack {
    std::uint32_t sourceTrackId{0};
    std::uint32_t artistId{0};
    std::uint32_t albumId{0};
    std::uint32_t genreId{0};
    std::uint32_t keyId{0};
    std::uint32_t artworkId{0};
    std::uint32_t composerId{0};
    std::uint32_t originalArtistId{0};
    std::uint32_t labelId{0};
    std::uint32_t remixerId{0};
    std::uint32_t sampleRate{0};
    std::uint32_t fileSize{0};
    std::uint32_t bitrate{0};
    std::uint32_t trackNumber{0};
    std::uint32_t tempoHundredths{0};
    std::uint16_t discNumber{0};
    std::uint16_t playCount{0};
    std::uint16_t year{0};
    std::uint16_t sampleDepth{0};
    std::uint16_t durationSeconds{0};
    std::uint8_t colorId{0};
    std::uint8_t rating{0};
    std::array<std::string, 21> stringsUtf8;
    std::optional<std::string> artistUtf8;
    std::optional<std::string> albumUtf8;
    std::optional<std::string> genreUtf8;
    std::optional<std::string> keyTextUtf8;
    std::optional<std::string> labelUtf8;
    std::optional<std::string> artworkPathUtf8;
    PdbRecordLocation location;

    const std::string& titleUtf8() const {
        return stringsUtf8[17];
    }
    const std::string& commentUtf8() const {
        return stringsUtf8[16];
    }
    const std::string& analyzePathUtf8() const {
        return stringsUtf8[14];
    }
    const std::string& mediaPathUtf8() const {
        return stringsUtf8[20];
    }
    const std::string& filenameUtf8() const {
        return stringsUtf8[19];
    }
};

struct PdbPlaylist {
    std::uint32_t sourcePlaylistId{0};
    std::uint32_t parentPlaylistId{0};
    std::uint32_t sortOrder{0};
    bool isFolder{false};
    std::string nameUtf8;
    PdbRecordLocation location;
};

struct PdbPlaylistEntry {
    std::uint32_t sourcePlaylistId{0};
    std::uint32_t sourceTrackId{0};
    std::uint32_t position{0};
    PdbRecordLocation location;
};

struct PdbRawPage {
    std::uint32_t pageType{0};
    std::uint32_t pageIndex{0};
    std::string bytes;
};

struct PdbData {
    std::string sourceIdentity;
    std::uint32_t sourceGeneration{0};
    std::uint32_t pageSize{0};
    std::vector<PdbTrack> tracks;
    std::vector<PdbNamedRecord> artists;
    std::vector<PdbNamedRecord> albums;
    std::vector<PdbNamedRecord> genres;
    std::vector<PdbNamedRecord> keys;
    std::vector<PdbNamedRecord> labels;
    std::vector<PdbNamedRecord> colors;
    std::vector<PdbArtwork> artwork;
    std::vector<PdbPlaylist> playlists;
    std::vector<PdbPlaylistEntry> playlistEntries;
    std::vector<PdbRawPage> unsupportedPages;
};

struct PdbDiagnostic {
    PdbStatus status{PdbStatus::Warning};
    std::string sourceIdentity;
    std::size_t byteOffset{0};
    std::string message;
};

struct PdbInput {
    std::string sourceIdentity;
    std::string_view bytes;
    PdbStatus availability{PdbStatus::Success};
};

struct PdbDecodeLimits {
    std::size_t maxFileBytes{512 * 1024 * 1024};
    std::size_t maxPageBytes{65536};
    std::size_t maxTables{256};
    std::size_t maxPages{1048576};
    std::size_t maxRows{2000000};
    std::size_t maxPlaylistDepth{128};
    std::size_t maxStringBytes{65536};
    std::size_t maxDecodedRecordBytes{256 * 1024 * 1024};
    std::size_t maxDecodedStringBytes{256 * 1024 * 1024};
    std::size_t maxPreservedBytes{64 * 1024 * 1024};
};

struct PdbDecodeOptions {
    PdbDecodeLimits limits;
    std::function<bool()> isCancelled;
    bool preserveUnsupportedPages{true};
};

struct PdbDecodeResult {
    PdbStatus status{PdbStatus::Failed};
    std::shared_ptr<const PdbData> data;
    std::vector<PdbDiagnostic> diagnostics;

    bool usable() const {
        return data && (status == PdbStatus::Success || status == PdbStatus::Warning);
    }
};

// Prepared by ChatGPT.
// Decodes caller-owned bounded bytes without filesystem, SQL, Track, or UI
// mutation. Source IDs and database sequence are independent of Mixxx TrackId,
// media identity, and an applied-generation fingerprint. Referenced missing
// text stays absent; a source string that is explicitly empty stays present.
// Playlist entries retain repeated membership and their original positions.
// History and unknown table pages have explicit unsupported diagnostics.
// End of ChatGPT-generated documentation.
PdbDecodeResult decodePdb(
        const PdbInput& input, const PdbDecodeOptions& options = {});

} // namespace mixxx::rekordbox
