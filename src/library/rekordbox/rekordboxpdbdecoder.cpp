#include "library/rekordbox/rekordboxpdbdecoder.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "rekordbox_pdb.h"

namespace mixxx::rekordbox {
namespace {

constexpr std::size_t kRootHeaderBytes = 28;
constexpr std::size_t kPageHeaderBytes = 40;
constexpr std::size_t kRowGroupBytes = 36;
constexpr std::size_t kRowsPerGroup = 16;
constexpr std::size_t kMaximumDiagnostics = 64;

struct DecodeFailure {
    PdbStatus status;
    std::size_t byteOffset;
    std::string message;
};

struct RowLayout {
    std::size_t index;
    std::size_t offset;
};

struct PageLayout {
    std::uint32_t index;
    std::uint32_t type;
    std::size_t offset;
    std::size_t heapEnd;
    bool supported;
    std::vector<RowLayout> rows;
};

struct StringLayout {
    std::size_t offset;
    std::size_t size;
    bool utf16;
};

bool isSupportedTable(std::uint32_t type) {
    return type <= 8 || type == 13;
}

std::size_t decodedRecordSize(std::uint32_t type) {
    switch (type) {
    case 0:
        return sizeof(PdbTrack);
    case 7:
        return sizeof(PdbPlaylist);
    case 8:
        return sizeof(PdbPlaylistEntry);
    case 13:
        return sizeof(PdbArtwork);
    default:
        return sizeof(PdbNamedRecord);
    }
}

class Decoder {
  public:
    Decoder(const PdbInput& input, const PdbDecodeOptions& options)
            : m_input(input),
              m_options(options) {
    }

    PdbDecodeResult decode() {
        checkCancelled(0);
        const auto pages = preflight();
        auto data = std::make_shared<PdbData>();
        data->sourceIdentity = m_input.sourceIdentity;
        data->sourceGeneration = readU32(20);
        data->pageSize = m_pageSize;

        // Prepared by ChatGPT.
        // Only the bounded header page is needed by the generated root reader.
        // Each validated page gets an independent reader, so linked pages cannot
        // create recursive ownership or destruction proportional to file size.
        // End of ChatGPT-generated documentation.
        kaitai::kstream rootStream(std::string(m_input.bytes.substr(0, m_pageSize)));
        rekordbox_pdb_t database(&rootStream);
        for (const auto& layout : pages) {
            checkCancelled(layout.offset);
            if (!layout.supported) {
                if (m_options.preserveUnsupportedPages) {
                    data->unsupportedPages.push_back({layout.type,
                            layout.index,
                            std::string(m_input.bytes.substr(layout.offset, m_pageSize))});
                }
                continue;
            }
            if (layout.rows.empty()) {
                continue;
            }
            kaitai::kstream pageStream(
                    std::string(m_input.bytes.substr(layout.offset, m_pageSize)));
            rekordbox_pdb_t::page_t page(&pageStream, nullptr, &database);
            page.heap_pos();
            for (const auto& rowLayout : layout.rows) {
                checkCancelled(rowLayout.offset);
                auto* row = page.row_groups()
                                    ->at(rowLayout.index / kRowsPerGroup)
                                    ->rows()
                                    ->at(rowLayout.index % kRowsPerGroup);
                decodeRow(data.get(), layout, rowLayout, row->body());
            }
        }
        resolveReferences(data.get());
        validatePlaylists(data.get());
        checkCancelled(m_input.bytes.size());
        return {m_hasWarnings ? PdbStatus::Warning : PdbStatus::Success,
                std::move(data),
                std::move(m_diagnostics)};
    }

  private:
    void checkCancelled(std::size_t offset) const {
        if (!m_options.isCancelled) {
            return;
        }
        bool cancelled = false;
        try {
            cancelled = m_options.isCancelled();
        } catch (...) {
            throw DecodeFailure{PdbStatus::Failed, offset, "PDB cancellation callback failed"};
        }
        if (cancelled) {
            throw DecodeFailure{PdbStatus::Cancelled, offset, "PDB decoding cancelled"};
        }
    }

    [[noreturn]] static void invalid(std::size_t offset, const std::string& message) {
        throw DecodeFailure{PdbStatus::Invalid, offset, message};
    }

    void warning(PdbStatus status, std::size_t offset, const std::string& message) {
        m_hasWarnings = true;
        if (m_diagnostics.size() < kMaximumDiagnostics) {
            m_diagnostics.push_back({status, m_input.sourceIdentity, offset, message});
        }
    }

    void requireRange(std::size_t offset, std::size_t size, std::size_t end) const {
        if (offset > end || size > end - offset || end > m_input.bytes.size()) {
            invalid(offset, "PDB field extends beyond its bounded container");
        }
    }

    std::uint8_t readU8(std::size_t offset) const {
        requireRange(offset, 1, m_input.bytes.size());
        return static_cast<std::uint8_t>(m_input.bytes[offset]);
    }

    std::uint16_t readU16(std::size_t offset) const {
        requireRange(offset, 2, m_input.bytes.size());
        return static_cast<std::uint16_t>(readU8(offset)) |
                (static_cast<std::uint16_t>(readU8(offset + 1)) << 8);
    }

    std::uint32_t readU32(std::size_t offset) const {
        requireRange(offset, 4, m_input.bytes.size());
        return static_cast<std::uint32_t>(readU16(offset)) |
                (static_cast<std::uint32_t>(readU16(offset + 2)) << 16);
    }

    StringLayout stringLayout(std::size_t offset, std::size_t end) const {
        requireRange(offset, 1, end);
        const auto marker = readU8(offset);
        if (marker == 0x40 || marker == 0x90) {
            requireRange(offset, 4, end);
            const auto length = readU16(offset + 1);
            if (length < 4) {
                invalid(offset, "PDB long string length is smaller than its header");
            }
            requireRange(offset, length, end);
            const std::size_t bodySize = length - 4;
            if (marker == 0x90 && bodySize % 2 != 0) {
                invalid(offset, "PDB UTF-16 string has an odd byte length");
            }
            return {offset + 4, bodySize, marker == 0x90};
        }
        if ((marker & 1) == 0 || marker < 3) {
            invalid(offset, "PDB short string has an invalid length/encoding marker");
        }
        const std::size_t length = marker >> 1;
        requireRange(offset, length, end);
        return {offset + 1, length - 1, false};
    }

    void addStringBudget(std::size_t bytes, std::size_t offset) {
        if (bytes > m_options.limits.maxDecodedStringBytes ||
                m_stringBytes > m_options.limits.maxDecodedStringBytes - bytes) {
            invalid(offset, "PDB decoded strings exceed the configured limit");
        }
        m_stringBytes += bytes;
    }

    void validateString(std::size_t offset, std::size_t end) {
        const auto layout = stringLayout(offset, end);
        if (layout.size > m_options.limits.maxStringBytes) {
            invalid(offset, "PDB string exceeds the configured limit");
        }
        addStringBudget(layout.utf16 ? (layout.size / 2) * 3 : layout.size, offset);
        if (!layout.utf16) {
            return;
        }
        for (std::size_t i = 0; i < layout.size; i += 2) {
            const auto value = readU16(layout.offset + i);
            if (value >= 0xd800 && value <= 0xdbff) {
                if (i + 2 >= layout.size) {
                    invalid(offset, "PDB UTF-16 string ends with an unpaired surrogate");
                }
                const auto low = readU16(layout.offset + i + 2);
                if (low < 0xdc00 || low > 0xdfff) {
                    invalid(offset, "PDB UTF-16 string has an unpaired high surrogate");
                }
                i += 2;
            } else if (value >= 0xdc00 && value <= 0xdfff) {
                invalid(offset, "PDB UTF-16 string has an unpaired low surrogate");
            }
        }
    }

    std::string decodeString(std::size_t offset, std::size_t end) const {
        const auto layout = stringLayout(offset, end);
        if (!layout.utf16) {
            return std::string(m_input.bytes.substr(layout.offset, layout.size));
        }
        std::string text;
        for (std::size_t i = 0; i < layout.size; i += 2) {
            std::uint32_t value = readU16(layout.offset + i);
            if (value >= 0xd800 && value <= 0xdbff) {
                const auto low = readU16(layout.offset + i + 2);
                value = 0x10000 + ((value - 0xd800) << 10) + (low - 0xdc00);
                i += 2;
            }
            if (value < 0x80) {
                text.push_back(static_cast<char>(value));
            } else if (value < 0x800) {
                text.push_back(static_cast<char>(0xc0 | (value >> 6)));
                text.push_back(static_cast<char>(0x80 | (value & 0x3f)));
            } else if (value < 0x10000) {
                text.push_back(static_cast<char>(0xe0 | (value >> 12)));
                text.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
                text.push_back(static_cast<char>(0x80 | (value & 0x3f)));
            } else {
                text.push_back(static_cast<char>(0xf0 | (value >> 18)));
                text.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3f)));
                text.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
                text.push_back(static_cast<char>(0x80 | (value & 0x3f)));
            }
        }
        return text;
    }

    void validateRow(std::uint32_t type, std::size_t offset, std::size_t end) {
        checkCancelled(offset);
        switch (type) {
        case 0:
            requireRange(offset, 136, end);
            for (std::size_t i = 0; i < 21; ++i) {
                validateString(offset + readU16(offset + 94 + i * 2), end);
            }
            break;
        case 1:
        case 4:
        case 13:
            requireRange(offset, 4, end);
            validateString(offset + 4, end);
            break;
        case 2: {
            requireRange(offset, 10, end);
            std::size_t nameOffset = readU8(offset + 9);
            if (readU16(offset) == 0x64) {
                requireRange(offset, 12, end);
                nameOffset = readU16(offset + 10);
            }
            validateString(offset + nameOffset, end);
            break;
        }
        case 3:
            requireRange(offset, 22, end);
            validateString(offset + readU8(offset + 21), end);
            break;
        case 5:
        case 6:
            requireRange(offset, 8, end);
            validateString(offset + 8, end);
            break;
        case 7:
            requireRange(offset, 20, end);
            validateString(offset + 20, end);
            break;
        case 8:
            requireRange(offset, 12, end);
            break;
        default:
            break;
        }
    }

    std::vector<PageLayout> preflight() {
        if (m_input.bytes.size() > m_options.limits.maxFileBytes) {
            invalid(0, "PDB file exceeds the configured byte limit");
        }
        requireRange(0, kRootHeaderBytes, m_input.bytes.size());
        m_pageSize = readU32(4);
        if (m_pageSize < 128 || m_pageSize > m_options.limits.maxPageBytes) {
            invalid(4, "PDB page size is outside the configured bounds");
        }
        if (m_input.bytes.size() % m_pageSize != 0) {
            invalid(m_input.bytes.size(), "PDB file ends in a truncated page");
        }
        const std::size_t pageCount = m_input.bytes.size() / m_pageSize;
        if (pageCount == 0 || pageCount > m_options.limits.maxPages) {
            invalid(4, "PDB page count is outside the configured bounds");
        }
        const auto tableCount = readU32(8);
        if (tableCount > m_options.limits.maxTables ||
                tableCount > (m_pageSize - kRootHeaderBytes) / 16) {
            invalid(8, "PDB table directory exceeds its page or configured limit");
        }
        if (readU32(24) != 0) {
            invalid(24, "PDB root reserved field is not zero");
        }

        std::vector<PageLayout> pages;
        std::unordered_set<std::uint32_t> visitedPages;
        std::unordered_set<std::uint32_t> reportedUnsupportedTypes;
        std::size_t rowCount = 0;
        std::size_t preservedBytes = 0;
        for (std::size_t tableIndex = 0; tableIndex < tableCount; ++tableIndex) {
            const std::size_t tableOffset = kRootHeaderBytes + tableIndex * 16;
            checkCancelled(tableOffset);
            const auto type = readU32(tableOffset);
            const bool supported = isSupportedTable(type);
            if (!supported && reportedUnsupportedTypes.insert(type).second) {
                const bool history = type == 11 || type == 12 || type == 19;
                warning(PdbStatus::Unsupported,
                        tableOffset,
                        history ? "PDB history tables are not imported"
                                : "PDB table type is not supported");
            }
            auto index = readU32(tableOffset + 8);
            const auto lastIndex = readU32(tableOffset + 12);
            if (index == lastIndex &&
                    (index == 0 || index == std::numeric_limits<std::uint32_t>::max())) {
                continue;
            }
            if (lastIndex == 0 || lastIndex >= pageCount) {
                invalid(tableOffset + 12, "PDB last page reference is out of bounds");
            }
            while (true) {
                checkCancelled(tableOffset);
                if (index == 0 || index >= pageCount) {
                    invalid(tableOffset + 8, "PDB page reference is out of bounds");
                }
                const std::size_t offset = static_cast<std::size_t>(index) * m_pageSize;
                if (readU32(offset) != 0 || readU32(offset + 4) != index) {
                    invalid(offset, "PDB page header does not match its reference");
                }
                if (readU32(offset + 8) != type) {
                    warning(PdbStatus::Warning,
                            offset + 8,
                            "PDB table chain stops at a different page type");
                    break;
                }
                if (!visitedPages.insert(index).second) {
                    invalid(offset, "PDB page chain is cyclic or shared by multiple tables");
                }
                PageLayout page{index, type, offset, offset + m_pageSize, supported, {}};
                if (!supported && m_options.preserveUnsupportedPages) {
                    if (m_pageSize > m_options.limits.maxPreservedBytes ||
                            preservedBytes > m_options.limits.maxPreservedBytes - m_pageSize) {
                        invalid(offset, "PDB unsupported pages exceed the preservation limit");
                    }
                    preservedBytes += m_pageSize;
                }
                if ((readU8(offset + 27) & 0x40) == 0) {
                    const std::size_t allocatedSlots = readU16(offset + 24) & 0x1fff;
                    const std::size_t groups = (allocatedSlots + kRowsPerGroup - 1) / kRowsPerGroup;
                    // Prepared by ChatGPT.
                    // Only allocated offsets consume heap space. Unused offsets
                    // in the final partial group may still contain row data.
                    // End of ChatGPT-generated documentation.
                    const std::size_t indexBytes = allocatedSlots * 2 + groups * 4;
                    if (indexBytes > m_pageSize - kPageHeaderBytes) {
                        invalid(offset + 24, "PDB row index overlaps its page header");
                    }
                    page.heapEnd = offset + m_pageSize - indexBytes;
                    const std::size_t heapBytes = page.heapEnd - offset - kPageHeaderBytes;
                    const std::size_t allocatedBytes = readU16(offset + 28) +
                            static_cast<std::size_t>(readU16(offset + 30));
                    if (allocatedBytes > heapBytes) {
                        invalid(offset + 28, "PDB heap sizes overlap its row index");
                    }
                    for (std::size_t group = 0; group < groups; ++group) {
                        const std::size_t base = offset + m_pageSize - group * kRowGroupBytes;
                        const auto mask = readU16(base - 4);
                        for (std::size_t row = 0; row < kRowsPerGroup; ++row) {
                            if ((mask & (1U << row)) == 0) {
                                continue;
                            }
                            const std::size_t slot = group * kRowsPerGroup + row;
                            if (slot >= allocatedSlots) {
                                invalid(base - 4, "PDB presence mask exceeds the row count");
                            }
                            if (++rowCount > m_options.limits.maxRows) {
                                invalid(base - 4, "PDB row count exceeds the configured limit");
                            }
                            const std::size_t rowOffset = offset + kPageHeaderBytes +
                                    readU16(base - 6 - row * 2);
                            requireRange(rowOffset, 1, page.heapEnd);
                            if (supported) {
                                const auto recordBytes = decodedRecordSize(type);
                                if (recordBytes >
                                                m_options.limits
                                                        .maxDecodedRecordBytes ||
                                        m_recordBytes >
                                                m_options.limits.maxDecodedRecordBytes -
                                                        recordBytes) {
                                    invalid(rowOffset,
                                            "PDB decoded records exceed the "
                                            "configured limit");
                                }
                                m_recordBytes += recordBytes;
                                validateRow(type, rowOffset, page.heapEnd);
                                page.rows.push_back({slot, rowOffset});
                            }
                        }
                    }
                }
                pages.push_back(std::move(page));
                if (index == lastIndex) {
                    break;
                }
                index = readU32(offset + 12);
            }
        }
        return pages;
    }

    void decodeRow(PdbData* data,
            const PageLayout& page,
            const RowLayout& layout,
            kaitai::kstruct* body) const {
        const auto offset = layout.offset;
        const PdbRecordLocation location{page.index, layout.index, offset};
        switch (page.type) {
        case 0: {
            const auto* source = static_cast<rekordbox_pdb_t::track_row_t*>(body);
            PdbTrack track;
            track.sourceTrackId = source->id();
            track.artistId = source->artist_id();
            track.albumId = source->album_id();
            track.genreId = source->genre_id();
            track.keyId = source->key_id();
            track.artworkId = source->artwork_id();
            track.composerId = source->composer_id();
            track.originalArtistId = source->original_artist_id();
            track.labelId = source->label_id();
            track.remixerId = source->remixer_id();
            track.sampleRate = source->sample_rate();
            track.fileSize = source->file_size();
            track.bitrate = source->bitrate();
            track.trackNumber = source->track_number();
            track.tempoHundredths = source->tempo();
            track.discNumber = source->disc_number();
            track.playCount = source->play_count();
            track.year = source->year();
            track.sampleDepth = source->sample_depth();
            track.durationSeconds = source->duration();
            track.colorId = source->color_id();
            track.rating = source->rating();
            track.location = location;
            for (std::size_t i = 0; i < track.stringsUtf8.size(); ++i) {
                track.stringsUtf8[i] = decodeString(offset + source->ofs_strings()->at(i),
                        page.heapEnd);
            }
            data->tracks.push_back(std::move(track));
            break;
        }
        case 1: {
            const auto* source = static_cast<rekordbox_pdb_t::genre_row_t*>(body);
            data->genres.push_back(
                    {source->id(), decodeString(offset + 4, page.heapEnd), {}, location});
            break;
        }
        case 2: {
            auto* source = static_cast<rekordbox_pdb_t::artist_row_t*>(body);
            const auto nameOffset = source->subtype() == 0x64
                    ? source->ofs_name_far()
                    : source->ofs_name_near();
            data->artists.push_back({source->id(),
                    decodeString(offset + nameOffset, page.heapEnd),
                    {},
                    location});
            break;
        }
        case 3: {
            const auto* source = static_cast<rekordbox_pdb_t::album_row_t*>(body);
            data->albums.push_back({source->id(),
                    decodeString(offset + source->ofs_name(), page.heapEnd),
                    source->artist_id(),
                    location});
            break;
        }
        case 4: {
            const auto* source = static_cast<rekordbox_pdb_t::label_row_t*>(body);
            data->labels.push_back(
                    {source->id(), decodeString(offset + 4, page.heapEnd), {}, location});
            break;
        }
        case 5: {
            const auto* source = static_cast<rekordbox_pdb_t::key_row_t*>(body);
            data->keys.push_back(
                    {source->id(), decodeString(offset + 8, page.heapEnd), {}, location});
            break;
        }
        case 6: {
            const auto* source = static_cast<rekordbox_pdb_t::color_row_t*>(body);
            data->colors.push_back(
                    {source->id(), decodeString(offset + 8, page.heapEnd), {}, location});
            break;
        }
        case 7: {
            auto* source = static_cast<rekordbox_pdb_t::playlist_tree_row_t*>(body);
            data->playlists.push_back({source->id(),
                    source->parent_id(),
                    source->sort_order(),
                    source->is_folder(),
                    decodeString(offset + 20, page.heapEnd),
                    location});
            break;
        }
        case 8: {
            const auto* source = static_cast<rekordbox_pdb_t::playlist_entry_row_t*>(body);
            data->playlistEntries.push_back(
                    {source->playlist_id(), source->track_id(), source->entry_index(), location});
            break;
        }
        case 13: {
            const auto* source = static_cast<rekordbox_pdb_t::artwork_row_t*>(body);
            data->artwork.push_back(
                    {source->id(), decodeString(offset + 4, page.heapEnd), location});
            break;
        }
        default:
            break;
        }
    }

    using NameMap = std::unordered_map<std::uint32_t, const std::string*>;

    static NameMap names(const std::vector<PdbNamedRecord>& records) {
        NameMap map;
        for (const auto& record : records) {
            if (!map.emplace(record.sourceId, &record.nameUtf8).second) {
                invalid(record.location.byteOffset, "PDB table contains duplicate source IDs");
            }
        }
        return map;
    }

    std::optional<std::string> lookup(const NameMap& map,
            std::uint32_t id,
            std::size_t offset) {
        const auto found = map.find(id);
        if (found == map.end()) {
            if (id != 0) {
                warning(PdbStatus::Warning, offset, "PDB metadata reference is unavailable");
            }
            return std::nullopt;
        }
        addStringBudget(found->second->size(), offset);
        return *found->second;
    }

    void resolveReferences(PdbData* data) {
        const auto artists = names(data->artists);
        const auto albums = names(data->albums);
        const auto genres = names(data->genres);
        const auto keys = names(data->keys);
        const auto labels = names(data->labels);
        names(data->colors);
        NameMap artwork;
        for (const auto& record : data->artwork) {
            if (!artwork.emplace(record.sourceId, &record.pathUtf8).second) {
                invalid(record.location.byteOffset, "PDB artwork contains duplicate source IDs");
            }
        }
        std::unordered_set<std::uint32_t> trackIds;
        for (auto& track : data->tracks) {
            const auto offset = track.location.byteOffset;
            checkCancelled(offset);
            if (track.sourceTrackId == 0 || !trackIds.insert(track.sourceTrackId).second) {
                invalid(offset, "PDB track source ID is zero or duplicated");
            }
            track.artistUtf8 = lookup(artists, track.artistId, offset);
            track.albumUtf8 = lookup(albums, track.albumId, offset);
            track.genreUtf8 = lookup(genres, track.genreId, offset);
            track.keyTextUtf8 = lookup(keys, track.keyId, offset);
            track.labelUtf8 = lookup(labels, track.labelId, offset);
            track.artworkPathUtf8 = lookup(artwork, track.artworkId, offset);
        }
        for (const auto& entry : data->playlistEntries) {
            if (trackIds.find(entry.sourceTrackId) == trackIds.end()) {
                warning(PdbStatus::Warning,
                        entry.location.byteOffset,
                        "PDB playlist references an unavailable track");
            }
        }
    }

    void validatePlaylists(PdbData* data) {
        std::unordered_map<std::uint32_t, std::size_t> indices;
        for (std::size_t i = 0; i < data->playlists.size(); ++i) {
            const auto& playlist = data->playlists[i];
            if (playlist.sourcePlaylistId == 0 ||
                    !indices.emplace(playlist.sourcePlaylistId, i).second) {
                invalid(playlist.location.byteOffset,
                        "PDB playlist source ID is zero or duplicated");
            }
        }
        std::vector<std::size_t> depths(data->playlists.size(), 0);
        std::vector<bool> visiting(data->playlists.size(), false);
        for (std::size_t i = 0; i < data->playlists.size(); ++i) {
            checkCancelled(data->playlists[i].location.byteOffset);
            if (depths[i] != 0) {
                continue;
            }
            std::vector<std::size_t> path;
            std::size_t current = i;
            std::size_t depth = 0;
            while (true) {
                const auto& playlist = data->playlists[current];
                if (depths[current] != 0) {
                    depth = depths[current];
                    break;
                }
                if (visiting[current]) {
                    invalid(playlist.location.byteOffset, "PDB playlist hierarchy is cyclic");
                }
                visiting[current] = true;
                path.push_back(current);
                if (path.size() > m_options.limits.maxPlaylistDepth) {
                    invalid(playlist.location.byteOffset,
                            "PDB playlist hierarchy exceeds depth limit");
                }
                if (playlist.parentPlaylistId == 0) {
                    break;
                }
                const auto parent = indices.find(playlist.parentPlaylistId);
                if (parent == indices.end()) {
                    warning(PdbStatus::Warning,
                            playlist.location.byteOffset,
                            "PDB playlist parent is unavailable");
                    break;
                }
                if (!data->playlists[parent->second].isFolder) {
                    invalid(playlist.location.byteOffset, "PDB playlist parent is not a folder");
                }
                current = parent->second;
            }
            for (auto node = path.rbegin(); node != path.rend(); ++node) {
                if (++depth > m_options.limits.maxPlaylistDepth) {
                    invalid(data->playlists[*node].location.byteOffset,
                            "PDB playlist hierarchy exceeds depth limit");
                }
                depths[*node] = depth;
                visiting[*node] = false;
            }
        }
        for (const auto& entry : data->playlistEntries) {
            const auto found = indices.find(entry.sourcePlaylistId);
            if (found == indices.end()) {
                warning(PdbStatus::Warning,
                        entry.location.byteOffset,
                        "PDB playlist entry references an unavailable playlist");
            } else if (data->playlists[found->second].isFolder) {
                warning(PdbStatus::Warning,
                        entry.location.byteOffset,
                        "PDB playlist entry references a folder");
            }
        }
        std::stable_sort(data->playlistEntries.begin(),
                data->playlistEntries.end(),
                [](const PdbPlaylistEntry& left, const PdbPlaylistEntry& right) {
                    return std::tie(left.sourcePlaylistId, left.position) <
                            std::tie(right.sourcePlaylistId, right.position);
                });
        std::stable_sort(data->playlists.begin(),
                data->playlists.end(),
                [](const PdbPlaylist& left, const PdbPlaylist& right) {
                    return std::tie(left.parentPlaylistId, left.sortOrder) <
                            std::tie(right.parentPlaylistId, right.sortOrder);
                });
    }

    const PdbInput& m_input;
    const PdbDecodeOptions& m_options;
    std::uint32_t m_pageSize{0};
    std::size_t m_recordBytes{0};
    std::size_t m_stringBytes{0};
    bool m_hasWarnings{false};
    std::vector<PdbDiagnostic> m_diagnostics;
};

} // namespace

PdbDecodeResult decodePdb(const PdbInput& input, const PdbDecodeOptions& options) {
    if (input.availability != PdbStatus::Success) {
        return {input.availability,
                {},
                {{input.availability, input.sourceIdentity, 0, "PDB input is not available"}}};
    }
    try {
        return Decoder(input, options).decode();
    } catch (const DecodeFailure& failure) {
        return {failure.status,
                {},
                {{failure.status, input.sourceIdentity, failure.byteOffset, failure.message}}};
    } catch (const std::bad_alloc&) {
        return {PdbStatus::Failed,
                {},
                {{PdbStatus::Failed, input.sourceIdentity, 0, "PDB allocation failed"}}};
    } catch (const std::exception& error) {
        return {PdbStatus::Invalid,
                {},
                {{PdbStatus::Invalid,
                        input.sourceIdentity,
                        0,
                        std::string("PDB parser failed: ") + error.what()}}};
    } catch (...) {
        return {PdbStatus::Failed,
                {},
                {{PdbStatus::Failed, input.sourceIdentity, 0, "PDB decoding failed"}}};
    }
}

} // namespace mixxx::rekordbox
