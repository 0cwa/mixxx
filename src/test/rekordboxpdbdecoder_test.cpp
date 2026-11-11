#include "library/rekordbox/rekordboxpdbdecoder.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#ifndef MIXXX_PDB_DECODER_STANDALONE
#include <gtest/gtest.h>
#endif

namespace {

using namespace mixxx::rekordbox;

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error(std::string(expression) + " at line " + std::to_string(line));
    }
}

#define PDB_REQUIRE(condition) require(static_cast<bool>(condition), #condition, __LINE__)

#ifdef MIXXX_PDB_DECODER_STANDALONE
using Test = std::pair<const char*, void (*)()>;
std::vector<Test>& tests() {
    static std::vector<Test> registered;
    return registered;
}

struct RegisterTest {
    RegisterTest(const char* name, void (*function)()) {
        tests().emplace_back(name, function);
    }
};

#define PDB_TEST(name)                              \
    void name();                                    \
    const RegisterTest register##name(#name, name); \
    void name()
#else
#define PDB_TEST(name) TEST(RekordboxPdbDecoderTest, name)
#endif

// Prepared by ChatGPT.
// These fixtures are synthetic DeviceSQL pages authored from the checked-in
// rekordbox_pdb.ksy layout. They are not exporter or player compatibility
// evidence. Invalid variants change declared pointers, counts, strings, and
// graph links before entering the production byte decoder.
// End of ChatGPT-generated documentation.
void put16(std::string* bytes, std::size_t offset, std::uint16_t value) {
    bytes->at(offset) = static_cast<char>(value);
    bytes->at(offset + 1) = static_cast<char>(value >> 8);
}

void put32(std::string* bytes, std::size_t offset, std::uint32_t value) {
    put16(bytes, offset, static_cast<std::uint16_t>(value));
    put16(bytes, offset + 2, static_cast<std::uint16_t>(value >> 16));
}

std::string ascii(const std::string& text) {
    PDB_REQUIRE(text.size() <= 126);
    return std::string(1, static_cast<char>((text.size() + 1) * 2 + 1)) + text;
}

std::string longString(const std::string& bytes, bool utf16) {
    std::string result(4, '\0');
    result[0] = utf16 ? '\x90' : '\x40';
    put16(&result, 1, static_cast<std::uint16_t>(bytes.size() + 4));
    result += bytes;
    return result;
}

std::string trackRow(std::uint32_t id, const std::string& title = "Track") {
    std::string row(136, '\0');
    put16(&row, 0, 0x24);
    put32(&row, 8, 48000);
    put32(&row, 16, 123456);
    put32(&row, 48, 320000);
    put32(&row, 52, 17);
    put32(&row, 56, 12850);
    put32(&row, 72, id);
    put16(&row, 76, 2);
    put16(&row, 78, 6);
    put16(&row, 80, 2026);
    put16(&row, 82, 24);
    put16(&row, 84, 180);
    row[88] = 2;
    row[89] = 5;
    row += ascii("");
    for (std::size_t i = 0; i < 21; ++i) {
        put16(&row, 94 + i * 2, 136);
    }
    const auto addString = [&](std::size_t field, const std::string& value) {
        put16(&row, 94 + field * 2, static_cast<std::uint16_t>(row.size()));
        row += ascii(value);
    };
    addString(17, title);
    addString(16, "Annotation");
    addString(14, "PIONEER/USBANLZ/A/ANLZ0000.DAT");
    addString(19, "music.wav");
    addString(20, "Contents/music.wav");
    return row;
}

std::string nameRow(std::uint32_t type,
        std::uint32_t id,
        const std::string& encodedName) {
    std::size_t fixed = 4;
    if (type == 2) {
        fixed = 10;
    } else if (type == 3) {
        fixed = 22;
    } else if (type == 5 || type == 6) {
        fixed = 8;
    }
    std::string row(fixed, '\0');
    if (type == 2) {
        put16(&row, 0, 0x60);
        put32(&row, 4, id);
        row[9] = static_cast<char>(fixed);
    } else if (type == 3) {
        put16(&row, 0, 0x80);
        put32(&row, 8, 7);
        put32(&row, 12, id);
        row[21] = static_cast<char>(fixed);
    } else if (type == 6) {
        put16(&row, 5, static_cast<std::uint16_t>(id));
    } else {
        put32(&row, 0, id);
        if (type == 5) {
            put32(&row, 4, id);
        }
    }
    row += encodedName;
    return row;
}

std::string playlistRow(std::uint32_t id,
        std::uint32_t parent,
        std::uint32_t order,
        bool folder,
        const std::string& name) {
    std::string row(20, '\0');
    put32(&row, 0, parent);
    put32(&row, 8, order);
    put32(&row, 12, id);
    put32(&row, 16, folder ? 1 : 0);
    return row + ascii(name);
}

std::string entryRow(std::uint32_t playlist, std::uint32_t track, std::uint32_t position) {
    std::string row(12, '\0');
    put32(&row, 0, position);
    put32(&row, 4, track);
    put32(&row, 8, playlist);
    return row;
}

class Fixture {
  public:
    static constexpr std::uint32_t kPageSize = 1024;

    void page(std::uint32_t type, std::vector<std::string> rows, bool dataPage = true) {
        m_pages.push_back({type, std::move(rows), dataPage});
    }

    std::string bytes() const {
        const auto count = static_cast<std::uint32_t>(m_pages.size() + 1);
        std::string bytes(count * kPageSize, '\0');
        put32(&bytes, 4, kPageSize);
        put32(&bytes, 12, count);
        put32(&bytes, 20, 0xf0000001);
        std::map<std::uint32_t, std::vector<std::uint32_t>> tables;
        for (std::size_t i = 0; i < m_pages.size(); ++i) {
            tables[m_pages[i].type].push_back(static_cast<std::uint32_t>(i + 1));
        }
        put32(&bytes, 8, static_cast<std::uint32_t>(tables.size()));
        std::size_t tableOffset = 28;
        for (const auto& [type, indices] : tables) {
            put32(&bytes, tableOffset, type);
            put32(&bytes, tableOffset + 8, indices.front());
            put32(&bytes, tableOffset + 12, indices.back());
            tableOffset += 16;
            PDB_REQUIRE(tableOffset <= kPageSize);
            for (std::size_t i = 0; i < indices.size(); ++i) {
                const auto index = indices[i];
                const auto& page = m_pages[index - 1];
                const std::size_t offset = index * kPageSize;
                put32(&bytes, offset + 4, index);
                put32(&bytes, offset + 8, type);
                put32(&bytes, offset + 12, i + 1 < indices.size() ? indices[i + 1] : count);
                const auto rows = static_cast<std::uint32_t>(page.rows.size());
                const auto packed = rows | (rows << 13);
                bytes[offset + 24] = static_cast<char>(packed);
                bytes[offset + 25] = static_cast<char>(packed >> 8);
                bytes[offset + 26] = static_cast<char>(packed >> 16);
                bytes[offset + 27] = page.dataPage ? '\x24' : '\x64';
                const auto groups = (rows + 15) / 16;
                const auto indexBytes = rows * 2 + groups * 4;
                std::size_t heap = 0;
                for (std::size_t row = 0; row < page.rows.size(); ++row) {
                    PDB_REQUIRE(40 + heap + page.rows[row].size() <= kPageSize - indexBytes);
                    bytes.replace(offset + 40 + heap, page.rows[row].size(), page.rows[row]);
                    const auto group = row / 16;
                    const auto slot = row % 16;
                    const auto base = offset + kPageSize - group * 36;
                    put16(&bytes, base - 6 - slot * 2, static_cast<std::uint16_t>(heap));
                    const auto groupCount = std::min<std::size_t>(
                            16, page.rows.size() - group * 16);
                    put16(&bytes, base - 4, static_cast<std::uint16_t>((1U << groupCount) - 1));
                    heap += page.rows[row].size();
                }
                put16(&bytes, offset + 30, static_cast<std::uint16_t>(heap));
                put16(&bytes,
                        offset + 28,
                        static_cast<std::uint16_t>(kPageSize - 40 - indexBytes - heap));
            }
        }
        return bytes;
    }

  private:
    struct Page {
        std::uint32_t type;
        std::vector<std::string> rows;
        bool dataPage;
    };
    std::vector<Page> m_pages;
};

PdbDecodeResult decode(const std::string& bytes, const PdbDecodeOptions& options = {}) {
    return decodePdb({"fixture-device", bytes}, options);
}

std::string simpleFixture() {
    Fixture fixture;
    fixture.page(0, {trackRow(100)});
    return fixture.bytes();
}

void invalidResult(const std::string& bytes, const PdbDecodeOptions& options = {}) {
    const auto result = decode(bytes, options);
    PDB_REQUIRE(result.status == PdbStatus::Invalid);
    PDB_REQUIRE(!result.usable());
    PDB_REQUIRE(!result.data);
    PDB_REQUIRE(!result.diagnostics.empty());
    PDB_REQUIRE(result.diagnostics.front().sourceIdentity == "fixture-device");
}

PDB_TEST(ReadsTrackMetadataAndIndependentSourceIds) {
    Fixture fixture;
    auto row = trackRow(0xf1234567);
    put32(&row, 68, 7);
    put32(&row, 64, 9);
    put32(&row, 60, 11);
    put32(&row, 32, 42);
    put32(&row, 28, 81);
    put32(&row, 40, 17);
    fixture.page(0, {row});
    fixture.page(1, {nameRow(1, 11, ascii("House"))});
    fixture.page(2, {nameRow(2, 7, ascii("Artist"))});
    fixture.page(3, {nameRow(3, 9, ascii("Album"))});
    fixture.page(4, {nameRow(4, 17, ascii("Label"))});
    fixture.page(5, {nameRow(5, 42, ascii("C"))});
    fixture.page(6, {nameRow(6, 2, ascii("Red"))});
    fixture.page(13, {nameRow(13, 81, ascii("PIONEER/Artwork/81.jpg"))});
    const auto result = decode(fixture.bytes());
    PDB_REQUIRE(result.status == PdbStatus::Success);
    PDB_REQUIRE(result.data->sourceGeneration == 0xf0000001);
    PDB_REQUIRE(result.data->sourceIdentity == "fixture-device");
    PDB_REQUIRE(result.data->tracks.size() == 1);
    const auto& track = result.data->tracks.front();
    PDB_REQUIRE(track.sourceTrackId == 0xf1234567);
    PDB_REQUIRE(track.keyId == 42);
    PDB_REQUIRE(track.keyTextUtf8 == "C");
    PDB_REQUIRE(track.artistUtf8 == "Artist");
    PDB_REQUIRE(track.albumUtf8 == "Album");
    PDB_REQUIRE(track.genreUtf8 == "House");
    PDB_REQUIRE(track.labelUtf8 == "Label");
    PDB_REQUIRE(track.artworkPathUtf8 == "PIONEER/Artwork/81.jpg");
    PDB_REQUIRE(track.sampleRate == 48000);
    PDB_REQUIRE(track.tempoHundredths == 12850);
    PDB_REQUIRE(track.bitrate == 320000);
    PDB_REQUIRE(track.year == 2026);
    PDB_REQUIRE(track.discNumber == 2);
    PDB_REQUIRE(track.playCount == 6);
    PDB_REQUIRE(track.sampleDepth == 24);
    PDB_REQUIRE(track.durationSeconds == 180);
    PDB_REQUIRE(track.rating == 5);
    PDB_REQUIRE(track.colorId == 2);
    PDB_REQUIRE(track.titleUtf8() == "Track");
    PDB_REQUIRE(track.commentUtf8() == "Annotation");
    PDB_REQUIRE(track.mediaPathUtf8() == "Contents/music.wav");
    PDB_REQUIRE(track.analyzePathUtf8() == "PIONEER/USBANLZ/A/ANLZ0000.DAT");
}

PDB_TEST(PreservesSparseOrderRepeatedMembershipAndEmptyFolders) {
    Fixture fixture;
    fixture.page(0, {trackRow(100), trackRow(200)});
    fixture.page(7,
            {playlistRow(1, 0, 8, true, "Empty"),
                    playlistRow(2, 0, 2, true, "Folder"),
                    playlistRow(3, 2, 42, false, "Playlist")});
    fixture.page(8,
            {entryRow(3, 100, 50),
                    entryRow(3, 200, 3),
                    entryRow(3, 100, 20),
                    entryRow(3, 100, 20)});
    const auto result = decode(fixture.bytes());
    PDB_REQUIRE(result.status == PdbStatus::Success);
    PDB_REQUIRE(result.data->playlists.size() == 3);
    PDB_REQUIRE(result.data->playlists[0].sourcePlaylistId == 2);
    PDB_REQUIRE(result.data->playlists[1].nameUtf8 == "Empty");
    const auto& entries = result.data->playlistEntries;
    PDB_REQUIRE(entries.size() == 4);
    PDB_REQUIRE(entries[0].position == 3);
    PDB_REQUIRE(entries[0].sourceTrackId == 200);
    PDB_REQUIRE(entries[1].position == 20);
    PDB_REQUIRE(entries[2].position == 20);
    PDB_REQUIRE(entries[3].position == 50);
    PDB_REQUIRE(entries[1].sourceTrackId == 100 && entries[2].sourceTrackId == 100);
    PDB_REQUIRE(entries[1].location.byteOffset < entries[2].location.byteOffset);
}

PDB_TEST(PreservesDifferentTrackIdsWithTheSameMediaPath) {
    Fixture fixture;
    fixture.page(0, {trackRow(100), trackRow(200)});
    const auto result = decode(fixture.bytes());
    PDB_REQUIRE(result.status == PdbStatus::Success);
    PDB_REQUIRE(result.data->tracks.size() == 2);
    PDB_REQUIRE(result.data->tracks[0].mediaPathUtf8() == result.data->tracks[1].mediaPathUtf8());
}

PDB_TEST(ReadsMultipleRowIndexGroups) {
    Fixture fixture;
    fixture.page(0, {trackRow(100)});
    fixture.page(7, {playlistRow(3, 0, 0, false, "Playlist")});
    std::vector<std::string> rows;
    for (std::uint32_t i = 0; i < 33; ++i) {
        rows.push_back(entryRow(3, 100, 100 - i));
    }
    fixture.page(8, std::move(rows));
    const auto result = decode(fixture.bytes());
    PDB_REQUIRE(result.status == PdbStatus::Success);
    PDB_REQUIRE(result.data->playlistEntries.size() == 33);
    PDB_REQUIRE(result.data->playlistEntries.front().position == 68);
    PDB_REQUIRE(result.data->playlistEntries.back().position == 100);
}

PDB_TEST(DistinguishesMissingAndExplicitlyEmptyReferenceText) {
    Fixture fixture;
    auto first = trackRow(1);
    auto second = trackRow(2);
    put32(&first, 32, 42);
    put32(&second, 32, 43);
    fixture.page(0, {first, second});
    fixture.page(5, {nameRow(5, 42, ascii(""))});
    const auto result = decode(fixture.bytes());
    PDB_REQUIRE(result.status == PdbStatus::Warning);
    PDB_REQUIRE(result.data->tracks[0].keyTextUtf8.has_value());
    PDB_REQUIRE(result.data->tracks[0].keyTextUtf8->empty());
    PDB_REQUIRE(!result.data->tracks[1].keyTextUtf8.has_value());
}

PDB_TEST(DecodesLongAsciiAndUtf16SurrogatePairs) {
    Fixture fixture;
    fixture.page(1, {nameRow(1, 1, longString(std::string(200, 'a'), false))});
    fixture.page(2, {nameRow(2, 2, longString(std::string("\xe9\0\x3c\xd8\xb5\xdf", 6), true))});
    const auto result = decode(fixture.bytes());
    PDB_REQUIRE(result.status == PdbStatus::Success);
    PDB_REQUIRE(result.data->genres[0].nameUtf8 == std::string(200, 'a'));
    PDB_REQUIRE(result.data->artists[0].nameUtf8 == "\xc3\xa9\xf0\x9f\x8e\xb5");
}

PDB_TEST(ReadsFarArtistNameOffset) {
    std::string row(300, '\0');
    put16(&row, 0, 0x64);
    put32(&row, 4, 1);
    put16(&row, 10, 300);
    row += ascii("Far name");
    Fixture fixture;
    fixture.page(2, {row});
    const auto result = decode(fixture.bytes());
    PDB_REQUIRE(result.status == PdbStatus::Success);
    PDB_REQUIRE(result.data->artists[0].nameUtf8 == "Far name");
}

PDB_TEST(RejectsAllTruncationsWithoutPublishingPartialData) {
    const auto bytes = simpleFixture();
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        invalidResult(bytes.substr(0, size));
    }
}

PDB_TEST(RejectsOversizedRootCountsAndPageSizes) {
    auto bytes = simpleFixture();
    put32(&bytes, 8, 0xffffffff);
    invalidResult(bytes);
    bytes = simpleFixture();
    put32(&bytes, 4, 0);
    invalidResult(bytes);
    bytes = simpleFixture();
    put32(&bytes, 4, 0xffffffff);
    invalidResult(bytes);
}

PDB_TEST(RejectsRootAndPageReservedFieldCorruption) {
    auto bytes = simpleFixture();
    bytes[24] = 1;
    invalidResult(bytes);
    bytes = simpleFixture();
    bytes[Fixture::kPageSize] = 1;
    invalidResult(bytes);
}

PDB_TEST(RejectsOutOfRangePageReferences) {
    auto bytes = simpleFixture();
    put32(&bytes, 36, 0xffffffff);
    invalidResult(bytes);
    bytes = simpleFixture();
    put32(&bytes, 40, 0xffffffff);
    invalidResult(bytes);
}

PDB_TEST(RejectsPageCyclesBeforeGeneratedReaderTraversal) {
    Fixture fixture;
    fixture.page(0, {trackRow(1)});
    fixture.page(0, {trackRow(2)});
    auto bytes = fixture.bytes();
    put32(&bytes, Fixture::kPageSize + 12, 1);
    invalidResult(bytes);
}

PDB_TEST(RejectsPageIndexAndRowIndexMismatch) {
    auto bytes = simpleFixture();
    put32(&bytes, Fixture::kPageSize + 4, 2);
    invalidResult(bytes);
    bytes = simpleFixture();
    put16(&bytes, Fixture::kPageSize + 24, 8191);
    invalidResult(bytes);
    bytes = simpleFixture();
    put16(&bytes, bytes.size() - 4, 2);
    invalidResult(bytes);
}

PDB_TEST(RejectsRowAndStringOffsetsOutsideTheHeap) {
    auto bytes = simpleFixture();
    put16(&bytes, bytes.size() - 6, 0xffff);
    invalidResult(bytes);
    bytes = simpleFixture();
    put16(&bytes, Fixture::kPageSize + 40 + 94 + 17 * 2, 0xffff);
    invalidResult(bytes);
    bytes = simpleFixture();
    put16(&bytes, Fixture::kPageSize + 28, 0xffff);
    invalidResult(bytes);
}

PDB_TEST(ReservesOnlyAllocatedOffsetsInPartialRowGroups) {
    for (const std::size_t count : {1, 15, 16, 17, 31, 32, 33}) {
        Fixture fixture;
        std::vector<std::string> rows;
        for (std::size_t i = 0; i < count; ++i) {
            rows.push_back(nameRow(1, static_cast<std::uint32_t>(i + 1), ascii("Genre")));
        }
        fixture.page(1, std::move(rows));
        const auto result = decode(fixture.bytes());
        PDB_REQUIRE(result.status == PdbStatus::Success);
        PDB_REQUIRE(result.data->genres.size() == count);
    }
}

PDB_TEST(AcceptsRowsEndingAtThePartialRowIndexBoundary) {
    Fixture fixture;
    const std::string name(Fixture::kPageSize - 40 - 6 - 8, 'G');
    fixture.page(1, {nameRow(1, 7, longString(name, false))});
    const auto result = decode(fixture.bytes());
    PDB_REQUIRE(result.status == PdbStatus::Success);
    PDB_REQUIRE(result.data->genres.size() == 1);
    PDB_REQUIRE(result.data->genres[0].nameUtf8 == name);
}

PDB_TEST(RejectsStringsOverlappingTheAllocatedPartialRowIndex) {
    Fixture fixture;
    const std::string name(Fixture::kPageSize - 40 - 6 - 8, 'G');
    fixture.page(1, {nameRow(1, 7, longString(name, false))});
    auto bytes = fixture.bytes();
    put16(&bytes, Fixture::kPageSize + 40 + 4 + 1, static_cast<std::uint16_t>(name.size() + 5));
    invalidResult(bytes);
}

PDB_TEST(IgnoresDeletedRowsAndNonDataPages) {
    auto bytes = simpleFixture();
    put16(&bytes, bytes.size() - 4, 0);
    bytes[Fixture::kPageSize + 40 + 136] = 0;
    auto result = decode(bytes);
    PDB_REQUIRE(result.status == PdbStatus::Success);
    PDB_REQUIRE(result.data->tracks.empty());
    bytes = simpleFixture();
    bytes[Fixture::kPageSize + 27] = '\x64';
    put16(&bytes, Fixture::kPageSize + 24, 8191);
    result = decode(bytes);
    PDB_REQUIRE(result.status == PdbStatus::Success);
    PDB_REQUIRE(result.data->tracks.empty());
}

PDB_TEST(RejectsMalformedStringLengthsBeforeGeneratedStringConstruction) {
    for (const auto& text : {std::string("\x40\0\0\0", 4),
                 std::string("\x40\xff\xff\0", 4),
                 std::string("\x90\x05\0\0x", 5),
                 std::string("\x01", 1),
                 std::string("\x04xx", 3)}) {
        Fixture fixture;
        fixture.page(1, {nameRow(1, 1, text)});
        invalidResult(fixture.bytes());
    }
}

PDB_TEST(RejectsUnpairedUtf16Surrogates) {
    for (const auto& text : {std::string("\x00\xd8", 2),
                 std::string("\x00\xdc", 2),
                 std::string("\x00\xd8"
                             "a\0",
                         4)}) {
        Fixture fixture;
        fixture.page(1, {nameRow(1, 1, longString(text, true))});
        invalidResult(fixture.bytes());
    }
}

PDB_TEST(ReportsHistoryAndPreservesUnsupportedPageBytes) {
    Fixture fixture;
    fixture.page(0, {trackRow(100)});
    fixture.page(11, {nameRow(1, 1, ascii("History"))});
    fixture.page(0xffffffff, {std::string(1, '\xff')});
    const auto bytes = fixture.bytes();
    const auto result = decode(bytes);
    PDB_REQUIRE(result.status == PdbStatus::Warning);
    PDB_REQUIRE(result.usable());
    PDB_REQUIRE(result.data->unsupportedPages.size() == 2);
    PDB_REQUIRE(result.diagnostics[0].status == PdbStatus::Unsupported);
    PDB_REQUIRE(result.data->unsupportedPages[0].bytes ==
            bytes.substr(Fixture::kPageSize * 2, Fixture::kPageSize));
    PdbDecodeOptions options;
    options.preserveUnsupportedPages = false;
    const auto withoutRaw = decode(bytes, options);
    PDB_REQUIRE(withoutRaw.status == PdbStatus::Warning);
    PDB_REQUIRE(withoutRaw.data->unsupportedPages.empty());
}

PDB_TEST(RejectsDuplicateTrackAndDictionaryIds) {
    Fixture fixture;
    fixture.page(0, {trackRow(100), trackRow(100)});
    invalidResult(fixture.bytes());
    Fixture names;
    names.page(5, {nameRow(5, 42, ascii("C")), nameRow(5, 42, ascii("D"))});
    invalidResult(names.bytes());
}

PDB_TEST(RejectsPlaylistCyclesAndInvalidParents) {
    Fixture fixture;
    fixture.page(7,
            {playlistRow(1, 2, 0, true, "A"), playlistRow(2, 1, 1, true, "B")});
    invalidResult(fixture.bytes());
    Fixture invalidParent;
    invalidParent.page(7,
            {playlistRow(1, 0, 0, false, "A"), playlistRow(2, 1, 1, false, "B")});
    invalidResult(invalidParent.bytes());
}

PDB_TEST(RejectsExcessivePlaylistDepthAndDuplicateIds) {
    Fixture fixture;
    fixture.page(7,
            {playlistRow(1, 0, 0, true, "A"),
                    playlistRow(2, 1, 1, true, "B"),
                    playlistRow(3, 2, 1, false, "C")});
    PdbDecodeOptions options;
    options.limits.maxPlaylistDepth = 2;
    invalidResult(fixture.bytes(), options);
    Fixture duplicate;
    duplicate.page(7,
            {playlistRow(1, 0, 0, true, "A"), playlistRow(1, 0, 1, true, "B")});
    invalidResult(duplicate.bytes());
}

PDB_TEST(PreservesUnresolvedPlaylistReferencesWithDiagnostics) {
    Fixture fixture;
    fixture.page(7, {playlistRow(1, 999, 0, true, "Orphan")});
    fixture.page(8, {entryRow(2, 123, 7)});
    const auto result = decode(fixture.bytes());
    PDB_REQUIRE(result.status == PdbStatus::Warning);
    PDB_REQUIRE(result.data->playlists[0].parentPlaylistId == 999);
    PDB_REQUIRE(result.data->playlistEntries[0].sourceTrackId == 123);
    PDB_REQUIRE(result.data->playlistEntries[0].sourcePlaylistId == 2);
}

PDB_TEST(EnforcesFileTablePageRowAndStringLimits) {
    const auto bytes = simpleFixture();
    PdbDecodeOptions options;
    options.limits.maxFileBytes = bytes.size() - 1;
    invalidResult(bytes, options);
    options = {};
    options.limits.maxTables = 0;
    invalidResult(bytes, options);
    options = {};
    options.limits.maxPages = 1;
    invalidResult(bytes, options);
    options = {};
    options.limits.maxRows = 0;
    invalidResult(bytes, options);
    options = {};
    options.limits.maxDecodedRecordBytes = sizeof(PdbTrack) - 1;
    invalidResult(bytes, options);
    options = {};
    options.limits.maxStringBytes = 3;
    invalidResult(bytes, options);
    options = {};
    options.limits.maxDecodedStringBytes = 5;
    invalidResult(bytes, options);
}

PDB_TEST(EnforcesUnsupportedPagePreservationLimit) {
    Fixture fixture;
    fixture.page(11, {nameRow(1, 1, ascii("History"))});
    PdbDecodeOptions options;
    options.limits.maxPreservedBytes = Fixture::kPageSize - 1;
    invalidResult(fixture.bytes(), options);
}

PDB_TEST(CancelsBeforeAndDuringDecodingWithoutPartialResults) {
    const auto bytes = simpleFixture();
    PdbDecodeOptions options;
    options.isCancelled = [] { return true; };
    auto result = decode(bytes, options);
    PDB_REQUIRE(result.status == PdbStatus::Cancelled && !result.data);
    std::size_t calls = 0;
    options.isCancelled = [&calls] { return ++calls >= 6; };
    result = decode(bytes, options);
    PDB_REQUIRE(result.status == PdbStatus::Cancelled && !result.data);
    PDB_REQUIRE(calls == 6);
}

PDB_TEST(ConvertsCallbackExceptionsToFailedOutcome) {
    PdbDecodeOptions options;
    options.isCancelled = []() -> bool { throw std::runtime_error("worker exception"); };
    const auto result = decode(simpleFixture(), options);
    PDB_REQUIRE(result.status == PdbStatus::Failed);
    PDB_REQUIRE(!result.data);
}

PDB_TEST(PreservesAvailabilityOutcomes) {
    for (const auto status : {PdbStatus::Unavailable,
                 PdbStatus::Denied,
                 PdbStatus::Cancelled,
                 PdbStatus::Failed,
                 PdbStatus::Unsupported}) {
        const auto result = decodePdb({"missing-device", {}, status});
        PDB_REQUIRE(result.status == status);
        PDB_REQUIRE(!result.usable());
        PDB_REQUIRE(!result.data);
    }
}

PDB_TEST(OwnsImmutableResultsAfterSourceBytesChange) {
    static_assert(std::is_same_v<decltype(PdbDecodeResult{}.data), std::shared_ptr<const PdbData>>);
    auto bytes = simpleFixture();
    const auto result = decode(bytes);
    bytes.assign(bytes.size(), '\xff');
    PDB_REQUIRE(result.data->tracks[0].titleUtf8() == "Track");
    PDB_REQUIRE(result.data->tracks[0].sourceTrackId == 100);
}

PDB_TEST(HandlesLongPageChainsWithoutRecursiveOwnership) {
    Fixture fixture;
    for (std::size_t i = 0; i < 1024; ++i) {
        fixture.page(0, {}, false);
    }
    fixture.page(0, {trackRow(100)});
    const auto result = decode(fixture.bytes());
    PDB_REQUIRE(result.status == PdbStatus::Success);
    PDB_REQUIRE(result.data->tracks.size() == 1);
}

PDB_TEST(AcceptsAnEmptyDatabaseAndEmptyTableSentinels) {
    Fixture fixture;
    auto bytes = fixture.bytes();
    auto result = decode(bytes);
    PDB_REQUIRE(result.status == PdbStatus::Success && result.data->tracks.empty());
    put32(&bytes, 8, 1);
    result = decode(bytes);
    PDB_REQUIRE(result.status == PdbStatus::Success && result.data->tracks.empty());
    put32(&bytes, 36, 0xffffffff);
    put32(&bytes, 40, 0xffffffff);
    result = decode(bytes);
    PDB_REQUIRE(result.status == PdbStatus::Success && result.data->tracks.empty());
}

PDB_TEST(AcceptsEmptyIndexAndDataPagesWithoutFollowingTheLastPageSentinel) {
    for (const bool dataPage : {false, true}) {
        Fixture fixture;
        fixture.page(0, {}, dataPage);
        auto bytes = fixture.bytes();
        put32(&bytes, Fixture::kPageSize + 12, 0xffffffff);
        const auto result = decode(bytes);
        PDB_REQUIRE(result.status == PdbStatus::Success);
        PDB_REQUIRE(result.data->tracks.empty());
    }
}

#ifdef MIXXX_PDB_DECODER_STANDALONE
void benchmark(std::size_t count) {
    Fixture fixture;
    std::vector<std::string> rows;
    for (std::size_t i = 0; i < count; ++i) {
        rows.push_back(trackRow(static_cast<std::uint32_t>(i + 1)));
        if (rows.size() == 4) {
            fixture.page(0, std::move(rows));
            rows.clear();
        }
    }
    if (!rows.empty()) {
        fixture.page(0, std::move(rows));
    }
    const auto bytes = fixture.bytes();
    const auto start = std::chrono::steady_clock::now();
    const auto result = decode(bytes);
    const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start);
    PDB_REQUIRE(result.status == PdbStatus::Success);
    PDB_REQUIRE(result.data->tracks.size() == count);
    std::cout << "Synthetic PDB metadata: " << count << " tracks, " << bytes.size()
              << " bytes, " << elapsed.count() << " ms\n";
}
#endif

} // namespace

#ifdef MIXXX_PDB_DECODER_STANDALONE
int main(int argc, char** argv) {
    std::size_t passed = 0;
    for (const auto& [name, test] : tests()) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
            ++passed;
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << passed << '/' << tests().size() << " PDB decoder tests passed\n";
    if (passed != tests().size() || tests().empty()) {
        return 1;
    }
    if (argc > 1 && std::string(argv[1]) == "--benchmark") {
        benchmark(10000);
        benchmark(100000);
    }
    return 0;
}
#endif
