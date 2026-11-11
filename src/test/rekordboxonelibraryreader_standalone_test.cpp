// Prepared by ChatGPT for human review.
#include <sqlite3.h>

#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>

#include "library/rekordbox/rekordboxonelibraryreader.h"

namespace {
using namespace mixxx::rekordbox;

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path utf8Path(const std::string& value) {
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

std::string sqlFixture;
std::filesystem::path fixtureRoot;
std::size_t fixtureCount = 0;

struct Fixture {
    std::filesystem::path root;
    std::filesystem::path databasePath;
    sqlite3* database = nullptr;
    DatabaseReadContext context;

    Fixture() {
        root = fixtureRoot / std::to_string(++fixtureCount);
        std::filesystem::create_directories(root / "private");
        std::filesystem::create_directories(root / "device/Contents");
        databasePath = root / "private/library.db";
        require(sqlite3_open(databasePath.string().c_str(), &database) ==
                        SQLITE_OK,
                "open fixture");
        sql(sqlFixture);
        context.sourceId = "registered-device-uuid";
        context.generation = 17;
        context.fingerprint = "synthetic-snapshot-sha256-placeholder";
        context.originalDatabasePath = root / "device/PIONEER/rekordbox/exportLibrary.db";
        context.mediaRoot = root / "device";
        context.assetRoot = context.mediaRoot;
        context.accessLease = std::make_shared<int>(7);
        context.cancelled = std::make_shared<std::atomic_bool>(false);
        createFile("Contents/Höfn.wav");
        createFile("Contents/second.wav");
        createFile("PIONEER/Artwork/cover.jpg");
        createFile("PIONEER/USBANLZ/ABC/ANLZ0000.DAT");
    }

    ~Fixture() {
        sqlite3_close(database);
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    void createFile(const std::string& relative) {
        const auto path = root / "device" / utf8Path(relative);
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        file << "synthetic asset placeholder";
    }

    void sql(const std::string& sql) {
        require(sqlite3_exec(
                        database, sql.c_str(), nullptr, nullptr, nullptr) ==
                        SQLITE_OK,
                "fixture SQL failed");
    }

    DatabaseReadResult read() {
        return readOneLibrary(databasePath, "", database, context);
    }
};

int failures = 0;
int tests = 0;

void test(const char* name, const std::function<void()>& run) {
    ++tests;
    try {
        run();
        std::cout << "PASS " << name << '\n';
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
}

void rejected(const std::string& sql, DatabaseOutcome outcome) {
    Fixture fixture;
    fixture.sql(sql);
    const auto result = fixture.read();
    require(result.outcome == outcome && !result.library, "invalid snapshot was published");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: onelibrary-reader-test synthetic.sql writable-directory\n";
        return 2;
    }
    std::ifstream input(argv[1], std::ios::binary);
    sqlFixture.assign(std::istreambuf_iterator<char>(input), {});
    if (sqlFixture.empty()) {
        return 2;
    }
    fixtureRoot = std::filesystem::absolute(argv[2]) /
            ("onelibrary-" +
                    std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch()
                                    .count()));

    test("metadata, Unicode, presence, original key and exact units", [] {
        Fixture fixture;
        const auto result = fixture.read();
        require(result.library && result.outcome == DatabaseOutcome::Warning,
                "valid fixture did not read");
        const auto& tracks = result.library->tracks;
        require(tracks.size() == 2, "track count");
        require(tracks[0].title == "Höfn / 東京" &&
                        tracks[0].artist == "Artist Æ",
                "Unicode metadata");
        require(tracks[0].key == "8A" && tracks[0].bpm == 128.5,
                "key or BPM translated incorrectly");
        require(tracks[0].durationMilliseconds == 161000, "whole-second duration misinterpreted");
        require(!tracks[0].comment && tracks[1].comment == "", "null and empty comments collapsed");
        require(tracks[1].title == "" && tracks[1].artist == "", "explicit empty text collapsed");
        require(tracks[0].label == "" && tracks[0].albumArtist == "",
                "empty joined text collapsed");
        require(tracks[0].colorIndex == 1 && tracks[0].discNumber == 2,
                "raw color or disc missing");
    });
    test("hierarchy, sibling order, empty folders and repeated membership", [] {
        Fixture fixture;
        const auto result = fixture.read();
        require(result.library && result.library->playlists.size() == 3, "playlist count");
        const auto& lists = result.library->playlists;
        require(lists[0].id == "102" && lists[1].id == "100" &&
                        lists[2].id == "101",
                "hierarchy order");
        require(lists[1].kind == DatabasePlaylistKind::Folder &&
                        lists[2].parentId == "100",
                "folder parentage");
        require(lists[0].entries.empty() && lists[2].entries.size() == 3, "membership count");
        require(lists[2].entries[0].sourceTrackId == "7" &&
                        lists[2].entries[1].sourceTrackId == "42" &&
                        lists[2].entries[2].sourceTrackId == "7",
                "repeated membership lost");
        require(lists[2].entries[0].id != lists[2].entries[2].id,
                "membership identities collapsed");
    });
    test("original roots, source generation and lifetime retained", [] {
        Fixture fixture;
        const auto result = fixture.read();
        require(result.library &&
                        result.library->context.sourceId ==
                                fixture.context.sourceId,
                "source identity");
        require(result.library->context.generation == 17 &&
                        result.library->context.accessLease ==
                                fixture.context.accessLease,
                "source generation or lease lost");
        require(!result.library->databaseIdentity,
                "desktop origin substituted for device identity");
        require(result.library->tracks[0].analysisPath ==
                        fixture.context.assetRoot /
                                "PIONEER/USBANLZ/ABC/ANLZ0000.DAT",
                "analysis resolved against private snapshot");
        require(result.library->tracks[0].artworkPath ==
                        fixture.context.assetRoot / "PIONEER/Artwork/cover.jpg",
                "artwork locator");
    });
    test("unqualified performance and opaque native data remain explicit", [] {
        Fixture fixture;
        const auto result = fixture.read();
        require(result.library && !result.library->capabilities.qualifiedPerformanceData &&
                        !result.library->capabilities.canonicalApplication,
                "overstated support");
        require(result.library->annotations.empty(), "opaque cue promoted");
        bool foundCue = false;
        bool foundColor = false;
        for (const auto& record : result.library->opaqueRecords) {
            if (record.table == "cue") {
                foundCue = record.properties.at("inUsec") == "250000";
            }
            if (record.table == "color") {
                foundColor = record.properties.at("name") == "Pink";
            }
        }
        require(foundCue && foundColor, "raw timing or color lookup lost");
    });
    test("empty collection stays a valid immutable result", [] {
        Fixture fixture;
        fixture.sql(
                "DELETE FROM playlist_content; DELETE FROM playlist; DELETE "
                "FROM cue; DELETE FROM content;");
        const auto result = fixture.read();
        require(result.library && result.library->tracks.empty() &&
                        result.library->playlists.empty(),
                "empty collection rejected");
    });
    test("missing media and assets have explicit outcomes", [] {
        Fixture fixture;
        std::filesystem::remove(fixture.context.mediaRoot / utf8Path("Contents/Höfn.wav"));
        std::filesystem::remove(fixture.context.assetRoot / "PIONEER/Artwork/cover.jpg");
        const auto result = fixture.read();
        require(result.library &&
                        result.outcome == DatabaseOutcome::MissingMedia,
                "missing media outcome");
        require(result.library->tracks[0].mediaKind ==
                                DatabaseMediaKind::Missing &&
                        result.issues.size() >= 3,
                "missing asset diagnostics");
    });
    test("unknown schema rejected before dependency accepts it", [] {
        rejected("UPDATE property SET dbVersion='9999';", DatabaseOutcome::Unsupported);
    });
    test("missing table rejected", [] {
        rejected("DROP TABLE cue;", DatabaseOutcome::Unsupported);
    });
    test("dependency schema failures have a specific unsupported outcome", [] {
        rejected("DROP INDEX index_playlist_content_playlist_id;", DatabaseOutcome::Unsupported);
    });
    test("wrong column storage type rejected", [] {
        rejected("UPDATE content SET length='later' WHERE content_id=7;", DatabaseOutcome::Corrupt);
    });
    test("view cannot impersonate a required table", [] {
        rejected(
                "ALTER TABLE cue RENAME TO actual_cue; CREATE VIEW cue AS "
                "SELECT * FROM actual_cue;",
                DatabaseOutcome::Unsupported);
    });
    test("added column cannot impersonate a resolved metadata field", [] {
        rejected("ALTER TABLE content ADD COLUMN resolved_artist TEXT;",
                DatabaseOutcome::Unsupported);
    });
    test("duration overflow rejected", [] {
        rejected(
                "UPDATE content SET length=9223372036854775807 WHERE "
                "content_id=7;",
                DatabaseOutcome::Unsupported);
    });
    test("negative metadata is rejected", [] {
        rejected("UPDATE content SET bitrate=-1 WHERE content_id=7;", DatabaseOutcome::Corrupt);
    });
    test("embedded null metadata is rejected", [] {
        rejected(
                "UPDATE content SET title='title'||char(0)||'hidden' WHERE "
                "content_id=7;",
                DatabaseOutcome::Corrupt);
    });
    test("missing playlist track rejected atomically", [] {
        rejected(
                "UPDATE playlist_content SET content_id=999 WHERE "
                "sequenceNo=1;",
                DatabaseOutcome::Corrupt);
    });
    test("missing metadata lookup does not silently erase a field", [] {
        rejected("DELETE FROM artist WHERE artist_id=1;", DatabaseOutcome::Corrupt);
    });
    test("missing album artist does not silently erase a field", [] {
        rejected("UPDATE album SET artist_id=999;", DatabaseOutcome::Corrupt);
    });
    test("missing color lookup is reported", [] {
        rejected("DELETE FROM color;", DatabaseOutcome::Corrupt);
    });
    test("orphaned playlist rejected atomically", [] {
        rejected(
                "UPDATE playlist SET playlist_id_parent=999 WHERE "
                "playlist_id=101;",
                DatabaseOutcome::Corrupt);
    });
    test("cyclic playlist rejected atomically", [] {
        rejected(
                "UPDATE playlist SET playlist_id_parent=101 WHERE "
                "playlist_id=100;",
                DatabaseOutcome::Corrupt);
    });
    test("folder membership rejected atomically", [] {
        rejected("INSERT INTO playlist_content VALUES(100,7,1);", DatabaseOutcome::Corrupt);
    });
    test("duplicate track identifiers rejected", [] {
        rejected(
                "CREATE TABLE replacement AS SELECT * FROM content; INSERT "
                "INTO replacement SELECT * FROM content; "
                "DROP TABLE content; ALTER TABLE replacement RENAME TO "
                "content;",
                DatabaseOutcome::Corrupt);
    });
    test("unsafe relative media path rejected", [] {
        rejected(
                "UPDATE content SET path='/../outside.wav' WHERE content_id=7;",
                DatabaseOutcome::Corrupt);
    });
    test("symlink escape rejected", [] {
        Fixture fixture;
        std::filesystem::create_directories(fixture.root / "outside");
        std::filesystem::create_directory_symlink(
                fixture.root / "outside", fixture.context.mediaRoot / "escape");
        fixture.sql("UPDATE content SET path='/escape/music.wav' WHERE content_id=7;");
        const auto result = fixture.read();
        require(!result.library && result.outcome == DatabaseOutcome::Corrupt,
                "symlink escape accepted");
    });
    test("source cannot be passed directly to dependency", [] {
        Fixture fixture;
        fixture.context.originalDatabasePath = fixture.databasePath;
        const auto result = fixture.read();
        require(!result.library && result.outcome == DatabaseOutcome::Failed,
                "source path accepted");
    });
    test("hardlink alias of source cannot be passed to dependency", [] {
        Fixture fixture;
        std::filesystem::create_directories(fixture.context.originalDatabasePath.parent_path());
        std::filesystem::create_hard_link(
                fixture.databasePath, fixture.context.originalDatabasePath);
        const auto result = fixture.read();
        require(!result.library && result.outcome == DatabaseOutcome::Failed,
                "source hardlink accepted");
    });
    test("unrelated private connection rejected", [] {
        Fixture fixture;
        const auto result = readOneLibrary(fixture.root / "other.db",
                "",
                fixture.database,
                fixture.context);
        require(!result.library && result.outcome == DatabaseOutcome::Failed,
                "mismatched connection accepted");
    });
    test("cancellation publishes no partial records", [] {
        Fixture fixture;
        fixture.context.cancelled->store(true);
        const auto result = fixture.read();
        require(!result.library && result.outcome == DatabaseOutcome::Cancelled,
                "cancelled result published");
    });
    test("record limits reject excessive schema or data", [] {
        Fixture fixture;
        fixture.context.maximumRecords = 1;
        const auto result = fixture.read();
        require(!result.library &&
                        result.outcome == DatabaseOutcome::Unsupported,
                "record limit ignored");
    });
    test("text limits reject oversized values", [] {
        Fixture fixture;
        fixture.context.maximumStringBytes = 4;
        const auto result = fixture.read();
        require(!result.library &&
                        result.outcome == DatabaseOutcome::Unsupported,
                "text limit ignored");
    });
    test("legacy-only and modern-only devices select their available reader",
            [] {
                require(selectDeviceDatabase(true, false).selected ==
                                DeviceDatabaseChoice::Legacy,
                        "legacy-only selection");
                require(selectDeviceDatabase(false, true).selected ==
                                DeviceDatabaseChoice::OneLibrary,
                        "modern-only selection");
            });
    test("matching and divergent dual devices both require explicit selection", [] {
        const auto result = selectDeviceDatabase(true, true);
        require(result.outcome == DatabaseOutcome::Unsupported &&
                        !result.selected,
                "dual database silently merged");
        require(selectDeviceDatabase(true, true, DeviceDatabaseChoice::Legacy)
                                .selected == DeviceDatabaseChoice::Legacy,
                "explicit legacy");
        require(selectDeviceDatabase(
                        true, true, DeviceDatabaseChoice::OneLibrary)
                                .selected == DeviceDatabaseChoice::OneLibrary,
                "explicit modern");
    });
    test("explicit unavailable format does not fall back silently", [] {
        require(selectDeviceDatabase(
                        true, false, DeviceDatabaseChoice::OneLibrary)
                                .outcome == DatabaseOutcome::Unavailable,
                "silent format fallback");
        require(selectDeviceDatabase(false, false).outcome ==
                        DatabaseOutcome::Unavailable,
                "empty device selection");
    });

    std::error_code error;
    std::filesystem::remove_all(fixtureRoot, error);
    std::cout << tests - failures << '/' << tests << " passed\n";
    return failures ? 1 : 0;
}
// End of code prepared by ChatGPT for human review and submission.
