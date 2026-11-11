// Prepared by ChatGPT for human review.
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>

#include "library/rekordbox/rekordboxnativereader.h"

namespace {

using namespace mixxx::rekordbox;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void execute(sqlite3* database, const std::string& sql) {
    require(sqlite3_exec(database, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK,
            "Fixture SQL failed");
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "Fixture file unavailable");
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

const DatabaseTrack& track(const DatabaseLibrary& library, std::string_view id) {
    const auto found = std::find_if(library.tracks.begin(),
            library.tracks.end(),
            [&](const auto& item) { return item.sourceTrackId == id; });
    require(found != library.tracks.end(), "Expected track missing");
    return *found;
}

const DatabasePlaylist& playlist(const DatabaseLibrary& library, std::string_view id) {
    const auto found = std::find_if(library.playlists.begin(),
            library.playlists.end(),
            [&](const auto& item) { return item.id == id; });
    require(found != library.playlists.end(), "Expected playlist missing");
    return *found;
}

const DatabaseAnnotation& annotation(const DatabaseLibrary& library, std::string_view id) {
    const auto found = std::find_if(library.annotations.begin(),
            library.annotations.end(),
            [&](const auto& item) { return item.sourceAnnotationId == id; });
    require(found != library.annotations.end(), "Expected annotation missing");
    return *found;
}

bool hasIssue(const DatabaseReadResult& result, const std::string& code) {
    return std::any_of(result.issues.begin(),
            result.issues.end(),
            [&](const auto& issue) { return issue.code == code; });
}

class Fixture {
  public:
    explicit Fixture(const std::string& sql) {
        sqlite3* raw = nullptr;
        require(sqlite3_open(":memory:", &raw) == SQLITE_OK, "Private database unavailable");
        database.reset(raw);
        execute(database.get(), sql);
        const auto serial = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() / ("mixxx-native-" + std::to_string(serial));
        require(std::filesystem::create_directory(root), "Private test directory unavailable");
        context.sourceId = "binding-independent-of-mount";
        context.generation = 7;
        context.fingerprint = "private-snapshot-fingerprint";
        context.originalDatabasePath = root / "library" / "master.db";
        context.assetRoot = root / "library";
        context.mediaRoot = root / "media";
        context.cancelled = std::make_shared<std::atomic_bool>(false);
        context.accessLease = std::make_shared<const std::string>("test-access-lease");
        put(context.mediaRoot / std::filesystem::path(u8"música/日本語.wav"));
        put(context.assetRoot / "share/PIONEER/USBANLZ/test/ANLZ0000.DAT");
        put(context.assetRoot / "artwork/cover.png");
    }

    ~Fixture() {
        database.reset();
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    DatabaseReadResult read() const {
        return readNativeLibrary(database.get(), context);
    }

    static void put(const std::filesystem::path& path) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        file << "Synthetic path existence placeholder; not audio or ANLZ.";
        require(file.good(), "Cannot create test placeholder");
    }

    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database{nullptr, sqlite3_close};
    DatabaseReadContext context;
    std::filesystem::path root;
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: rekordbox-native-reader-test synthetic-schema.sql "
                     "[independent-master.db]\n";
        return 2;
    }
    int passed = 0;
    int failed = 0;
    const auto sql = readFile(argv[1]);
    const auto test = [&](const std::string& name, const std::function<void(Fixture&)>& check) {
        try {
            Fixture fixture(sql);
            check(fixture);
            ++passed;
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    };
    const auto rejects = [&](const std::string& name,
                                 const std::string& change,
                                 DatabaseOutcome expected,
                                 const std::string& code) {
        test(name, [&](Fixture& fixture) {
            execute(fixture.database.get(), change);
            const auto result = fixture.read();
            require(result.outcome == expected && !result.library && hasIssue(result, code),
                    "Expected atomic refusal with a structured outcome");
        });
    };

    test("metadata and opaque IDs", [](Fixture& fixture) {
        const auto result = fixture.read();
        require(result.library != nullptr, "No library");
        require(result.outcome == DatabaseOutcome::MissingMedia, "Missing media outcome lost");
        const auto& library = *result.library;
        require(library.tracks.size() == 4, "Deleted track not excluded");
        const auto& item = track(library, "7");
        require(item.title == "Ångström 日本語" &&
                        item.artist == "Synthetic Artist" &&
                        item.album == "Synthetic Album" &&
                        item.albumArtist == "Synthetic Artist" &&
                        item.composer == "Synthetic Artist" &&
                        item.remixer == "Synthetic Artist" &&
                        item.genre == "Synthetic Genre" &&
                        item.label == "Synthetic Label" &&
                        item.key == "F# minor",
                "Metadata relationship mapping failed");
        require(item.bpm == 128.5 && item.durationMilliseconds == 10000 &&
                        item.sampleRate == 48000 && item.rating == 5 &&
                        item.playCount == 12 && item.trackNumber == 2 &&
                        item.discNumber == 1,
                "Numeric units failed");
        require(track(library, "4294967296").sourceTrackId == "4294967296", "Track ID narrowed");
        require(item.properties.at("ColorName") == "Green" && !item.colorRgb &&
                        item.colorIndex == 5,
                "Unqualified indexed color was converted to RGB");
    });
    test("absent and explicit empty values", [](Fixture& fixture) {
        const auto result = fixture.read();
        require(result.library != nullptr, "No library");
        const auto& missing = track(*result.library, "4294967296");
        require(!missing.title && missing.artist == "",
                "NULL and explicit empty metadata collapsed");
        require(missing.properties.contains("Title") &&
                        !missing.properties.at("Title"),
                "SQL NULL not preserved");
        require(!missing.properties.contains("unknown-column"), "Absent column invented");
        require(track(*result.library, "empty").title == "", "Explicit empty title lost");
    });
    test("ordered repeated memberships and history namespace", [](Fixture& fixture) {
        const auto result = fixture.read();
        require(result.library != nullptr, "No library");
        const auto& list = playlist(*result.library, "playlist:7");
        require(list.parentId == "playlist:folder" && list.entries.size() == 3,
                "Hierarchy or membership missing");
        require(list.entries[0].sourceTrackId == "7" &&
                        list.entries[1].sourceTrackId == "4294967296" &&
                        list.entries[2].sourceTrackId == "7" &&
                        list.entries[0].id == "membership-a" &&
                        list.entries[2].position == 3,
                "Repeated or ordered membership changed");
        const auto& history = playlist(*result.library, "history:7");
        require(history.kind == DatabasePlaylistKind::History && history.entries.size() == 2 &&
                        history.entries[0].id == "history-a",
                "History conflicted with regular playlist IDs");
        require(playlist(*result.library, "playlist:smart")
                                        .properties.at("SmartList") ==
                                "<NODE/>" &&
                        hasIssue(result, "native.smart_playlist") &&
                        hasIssue(result, "native.playlist_type"),
                "Advanced playlist concepts silently discarded");
    });
    test("cue roles units colors labels and active loops", [](Fixture& fixture) {
        const auto result = fixture.read();
        require(result.library != nullptr, "No library");
        require(result.library->annotations.size() == 5, "Annotation count changed");
        const auto& hot = annotation(*result.library, "hot-loop");
        require(hot.role == DatabaseCueRole::HotLoop && hot.hotSlot == 0 &&
                        hot.startMilliseconds == 2000 &&
                        hot.endMilliseconds == 4000 &&
                        hot.startFrame150 == 300 && hot.endFrame150 == 600 &&
                        hot.activeLoop == true && hot.label == "" &&
                        hot.colorIndex == 0 && !hot.colorRgb,
                "Hot loop semantics changed");
        const auto& memoryA = annotation(*result.library, "memory-a");
        const auto& memoryB = annotation(*result.library, "memory-b");
        require(memoryA.role == DatabaseCueRole::MemoryCue &&
                        !memoryA.hotSlot && !memoryA.endMilliseconds &&
                        memoryA.startMilliseconds ==
                                memoryB.startMilliseconds &&
                        !memoryB.label && !memoryB.colorIndex &&
                        memoryB.properties.at("Color") == "-1",
                "Independent memory records or absent label/color lost");
        const auto& hotPoint = annotation(*result.library, "hot-point");
        require(hotPoint.startMilliseconds == 2501 &&
                        hotPoint.startFrame150 == 375 && !hotPoint.activeLoop,
                "Millisecond time was replaced by frame or PCM assumptions");
        require(annotation(*result.library, "memory-loop").role == DatabaseCueRole::MemoryLoop,
                "Memory loop role changed");
    });
    test("original roots and raw paths", [](Fixture& fixture) {
        const auto result = fixture.read();
        require(result.library != nullptr, "No library");
        const auto& item = track(*result.library, "7");
        require(item.originalAnalysisPath ==
                                "/PIONEER/USBANLZ/test/ANLZ0000.DAT" &&
                        item.analysisPath ==
                                fixture.context.assetRoot /
                                        "share/PIONEER/USBANLZ/test/"
                                        "ANLZ0000.DAT" &&
                        item.artworkPath ==
                                fixture.context.assetRoot /
                                        "artwork/cover.png" &&
                        item.mediaKind == DatabaseMediaKind::Local,
                "Snapshot path incorrectly substituted for original "
                "asset/media roots");
    });
    test("streaming missing and unavailable media", [](Fixture& fixture) {
        const auto result = fixture.read();
        require(result.library != nullptr, "No library");
        require(track(*result.library, "stream").mediaKind ==
                                DatabaseMediaKind::Streaming &&
                        !track(*result.library, "stream").mediaPath &&
                        track(*result.library, "empty").mediaKind ==
                                DatabaseMediaKind::Unavailable &&
                        track(*result.library, "4294967296").mediaKind ==
                                DatabaseMediaKind::Missing,
                "Media classes collapsed");
    });
    test("identity generation and access lease", [](Fixture& fixture) {
        std::weak_ptr<const void> lease = fixture.context.accessLease;
        const auto result = fixture.read();
        require(result.library != nullptr, "No library");
        fixture.context.accessLease.reset();
        require(!lease.expired() &&
                        result.library->context.sourceId ==
                                "binding-independent-of-mount" &&
                        result.library->databaseIdentity ==
                                "fixture-native-dbid" &&
                        result.library->context.generation == 7 &&
                        result.library->context.fingerprint ==
                                "private-snapshot-fingerprint",
                "Stable source binding or access lifetime lost");
        fixture.context.originalDatabasePath = fixture.root / "moved/master.db";
        const auto moved = fixture.read();
        require(moved.library &&
                        moved.library->databaseIdentity ==
                                result.library->databaseIdentity &&
                        moved.library->context.sourceId ==
                                result.library->context.sourceId &&
                        track(*moved.library, "7").sourceTrackId ==
                                track(*result.library, "7").sourceTrackId,
                "Moving a source changed its identity");
    });
    test("capabilities stay below M2 acceptance", [](Fixture& fixture) {
        const auto result = fixture.read();
        require(result.library != nullptr, "No library");
        require(result.library->schemaProfile == "native-dbversion-6000-v1" &&
                        !result.library->producerVersion &&
                        result.library->capabilities.databaseAnnotations &&
                        !result.library->capabilities
                                .qualifiedPerformanceData &&
                        !result.library->capabilities.canonicalApplication,
                "Parser success incorrectly advertised application or version "
                "compatibility");
    });
    test("missing assets retain original references", [](Fixture& fixture) {
        std::filesystem::remove_all(fixture.context.assetRoot);
        const auto result = fixture.read();
        require(result.library && hasIssue(result, "native.missing_asset") &&
                        track(*result.library, "7").originalAnalysisPath.has_value(),
                "Missing assets silently discarded");
    });
    test("asset traversal blocked", [](Fixture& fixture) {
        execute(fixture.database.get(),
                "UPDATE djmdContent SET AnalysisDataPath='/../outside.DAT' "
                "WHERE ID='7'");
        const auto result = fixture.read();
        require(result.library && hasIssue(result, "native.asset_path") &&
                        !track(*result.library, "7").analysisPath,
                "Escaping asset path resolved");
    });
    test("opaque unqualified cue fields retained", [](Fixture& fixture) {
        execute(fixture.database.get(), "UPDATE djmdCue SET CueMicrosec=1234 WHERE ID='hot-loop'");
        const auto result = fixture.read();
        require(result.library &&
                        annotation(*result.library, "hot-loop")
                                        .properties.at("CueMicrosec") ==
                                "1234" &&
                        annotation(*result.library, "hot-loop")
                                        .startMilliseconds == 2000,
                "Unqualified timing field discarded or substituted");
    });
    test("asset symlink escape blocked", [](Fixture& fixture) {
        const auto outside = fixture.root / "outside";
        Fixture::put(outside / "private.DAT");
        std::filesystem::create_directory_symlink(
                outside, fixture.context.assetRoot / "share" / "escape");
        execute(fixture.database.get(),
                "UPDATE djmdContent SET AnalysisDataPath='/escape/private.DAT' "
                "WHERE ID='7'");
        const auto result = fixture.read();
        require(result.library && hasIssue(result, "native.asset_path") &&
                        !track(*result.library, "7").analysisPath,
                "Native asset symlink escaped its allowed root");
    });
    test("reader executes no writes", [](Fixture& fixture) {
        const auto before = sqlite3_total_changes64(fixture.database.get());
        const auto result = fixture.read();
        require(result.library && sqlite3_total_changes64(fixture.database.get()) == before,
                "Parser changed its private database");
    });
    test("advanced native concepts retain opaque provenance", [](Fixture& fixture) {
        const auto result = fixture.read();
        require(result.library && result.library->opaqueRecords.size() == 8,
                "Raw lookup or ancillary records missing");
        const auto censor = std::find_if(result.library->opaqueRecords.begin(),
                result.library->opaqueRecords.end(),
                [](const auto& record) { return record.table == "djmdActiveCensor"; });
        require(censor != result.library->opaqueRecords.end() &&
                        censor->properties.at("ParameterList") == "opaque-effect-parameters" &&
                        hasIssue(result, "native.ancillary_records"),
                "An unsupported advanced native concept was silently discarded");
    });
    test("empty native collection", [](Fixture& fixture) {
        execute(fixture.database.get(),
                "DELETE FROM djmdContent; DELETE FROM djmdSongPlaylist; DELETE "
                "FROM djmdPlaylist; DELETE FROM djmdSongHistory; DELETE FROM "
                "djmdHistory; DELETE FROM djmdCue; DELETE FROM djmdActiveCensor");
        const auto result = fixture.read();
        require(result.outcome == DatabaseOutcome::Success && result.library &&
                        result.library->tracks.empty() &&
                        result.library->playlists.empty() &&
                        result.library->annotations.empty(),
                "Empty native collection did not produce a complete empty "
                "record set");
    });

    rejects("unknown database version",
            "UPDATE djmdProperty SET DBVersion='7000'",
            DatabaseOutcome::Unsupported,
            "native.schema_version");
    rejects("missing schema column",
            "ALTER TABLE djmdContent RENAME COLUMN BPM TO unknown_tempo",
            DatabaseOutcome::Unsupported,
            "native.schema_columns");
    rejects("views cannot impersonate profile tables",
            "ALTER TABLE djmdContent RENAME TO saved_content; CREATE VIEW "
            "djmdContent AS SELECT * FROM saved_content",
            DatabaseOutcome::Unsupported,
            "native.schema_table");
    rejects("duplicate track ID",
            "INSERT INTO djmdContent SELECT * FROM djmdContent WHERE ID='7'",
            DatabaseOutcome::Corrupt,
            "native.duplicate_id");
    rejects("duplicate membership ID",
            "INSERT INTO djmdSongPlaylist SELECT * FROM djmdSongPlaylist WHERE "
            "ID='membership-a'",
            DatabaseOutcome::Corrupt,
            "native.membership_reference");
    rejects("ambiguous membership order",
            "UPDATE djmdSongPlaylist SET TrackNo=1 WHERE ID='membership-b'",
            DatabaseOutcome::Corrupt,
            "native.ambiguous_order");
    rejects("ambiguous sibling order",
            "UPDATE djmdPlaylist SET Seq=1 WHERE ID='smart'",
            DatabaseOutcome::Corrupt,
            "native.ambiguous_hierarchy_order");
    rejects("orphan track membership",
            "UPDATE djmdSongPlaylist SET ContentID='absent' WHERE "
            "ID='membership-a'",
            DatabaseOutcome::Corrupt,
            "native.membership_reference");
    rejects("playlist cycle",
            "UPDATE djmdPlaylist SET ParentID='folder', Seq=99 WHERE "
            "ID='folder'",
            DatabaseOutcome::Corrupt,
            "native.playlist_hierarchy");
    rejects("playlist parent is regular",
            "UPDATE djmdPlaylist SET ParentID='7' WHERE ID='smart'",
            DatabaseOutcome::Corrupt,
            "native.playlist_hierarchy");
    rejects("missing cue track",
            "UPDATE djmdCue SET ContentID='absent' WHERE ID='memory-a'",
            DatabaseOutcome::Corrupt,
            "native.annotation_reference");
    rejects("duplicate hot slot",
            "UPDATE djmdCue SET Kind=1 WHERE ID='hot-point'",
            DatabaseOutcome::Corrupt,
            "native.duplicate_hot_slot");
    rejects("reversed loop interval",
            "UPDATE djmdCue SET OutMsec=1999 WHERE ID='hot-loop'",
            DatabaseOutcome::Corrupt,
            "native.annotation_timing");
    rejects("negative cue start",
            "UPDATE djmdCue SET InMsec=-1 WHERE ID='memory-a'",
            DatabaseOutcome::Corrupt,
            "native.annotation_timing");
    rejects("alternate cue authority unknown",
            "INSERT INTO contentCue VALUES ('alternate', '7', "
            "'{unknown-cues}', 0)",
            DatabaseOutcome::Unsupported,
            "native.alternate_cues");
    rejects("invalid active loop",
            "UPDATE djmdCue SET ActiveLoop=1 WHERE ID='memory-a'",
            DatabaseOutcome::Unsupported,
            "native.active_loop");
    rejects("missing metadata lookup",
            "UPDATE djmdContent SET ArtistID='absent' WHERE ID='7'",
            DatabaseOutcome::Corrupt,
            "native.missing_reference");
    rejects("multiple database identities",
            "INSERT INTO djmdProperty SELECT * FROM djmdProperty",
            DatabaseOutcome::Corrupt,
            "native.property_count");
    rejects("duration multiplication overflow",
            "UPDATE djmdContent SET Length=9223372036854776 WHERE ID='7'",
            DatabaseOutcome::Corrupt,
            "native.track_timing");
    rejects("invalid integer text",
            "UPDATE djmdContent SET BPM='128x' WHERE ID='7'",
            DatabaseOutcome::Corrupt,
            "native.invalid_integer");
    rejects("negative sample rate",
            "UPDATE djmdContent SET SampleRate=-1 WHERE ID='7'",
            DatabaseOutcome::Corrupt,
            "native.metadata_range");
    rejects("embedded null text",
            "UPDATE djmdContent SET Title='before'||char(0)||'after' WHERE "
            "ID='7'",
            DatabaseOutcome::Corrupt,
            "native.text_nul");
    test("pre-cancelled operation", [](Fixture& fixture) {
        fixture.context.cancelled->store(true);
        const auto result = fixture.read();
        require(result.outcome == DatabaseOutcome::Cancelled && !result.library,
                "Cancelled read published records");
    });
    test("cancellation during SQLite scan", [](Fixture& fixture) {
        sqlite3_progress_handler(
                fixture.database.get(),
                50,
                [](void* cancelled) {
                    static_cast<std::atomic_bool*>(cancelled)->store(true);
                    return 0;
                },
                fixture.context.cancelled.get());
        const auto result = fixture.read();
        sqlite3_progress_handler(fixture.database.get(), 0, nullptr, nullptr);
        require(result.outcome == DatabaseOutcome::Cancelled && !result.library,
                "Mid-read cancellation published partial records");
    });
    test("bounded record count", [](Fixture& fixture) {
        fixture.context.maximumRecords = 10;
        const auto result = fixture.read();
        require(result.outcome == DatabaseOutcome::Unsupported &&
                        !result.library &&
                        hasIssue(result, "native.record_limit"),
                "Configured record bound ignored");
    });
    test("bounded value bytes", [](Fixture& fixture) {
        fixture.context.maximumStringBytes = 1024;
        execute(fixture.database.get(),
                "UPDATE djmdContent SET Title=hex(zeroblob(1024)) WHERE "
                "ID='7'");
        const auto result = fixture.read();
        require(result.outcome == DatabaseOutcome::Unsupported &&
                        !result.library &&
                        hasIssue(result, "native.string_limit"),
                "Configured string bound ignored");
    });
    test("missing source binding", [](Fixture& fixture) {
        fixture.context.sourceId.clear();
        const auto result = fixture.read();
        require(result.outcome == DatabaseOutcome::Failed && !result.library &&
                        hasIssue(result, "native.request"),
                "Reader invented a path-based stable identity");
    });
    test("missing private database", [](Fixture& fixture) {
        const auto result = readNativeLibrary(nullptr, fixture.context);
        require(result.outcome == DatabaseOutcome::Failed && !result.library,
                "Null database accepted");
    });
    test("missing fingerprint", [](Fixture& fixture) {
        fixture.context.fingerprint.clear();
        const auto result = fixture.read();
        require(result.outcome == DatabaseOutcome::Failed && !result.library &&
                        hasIssue(result, "native.request"),
                "Reader published records without a snapshot fingerprint");
    });
    test("missing original database path", [](Fixture& fixture) {
        fixture.context.originalDatabasePath = "relative/master.db";
        const auto result = fixture.read();
        require(result.outcome == DatabaseOutcome::Failed && !result.library &&
                        hasIssue(result, "native.request"),
                "Reader published records without an absolute original source "
                "path");
    });
    if (argc == 3) {
        test("independent pyrekordbox native DBVersion 6000 fixture", [&](Fixture& fixture) {
            fixture.database.reset();
            const auto copy = fixture.root / "private-independent.db";
            std::filesystem::copy_file(argv[2], copy);
            const auto locator = "file:" + copy.generic_string() + "?immutable=1";
            sqlite3* raw = nullptr;
            require(sqlite3_open_v2(locator.c_str(),
                            &raw,
                            SQLITE_OPEN_READONLY | SQLITE_OPEN_URI,
                            nullptr) == SQLITE_OK,
                    "Independent private fixture open failed");
            fixture.database.reset(raw);
            const auto result = fixture.read();
            require(result.library &&
                            result.library->databaseIdentity == "3546525630" &&
                            result.library->tracks.size() == 6,
                    "Independent library identity/count mismatch");
            const auto& first = track(*result.library, "178162577");
            const auto& second = track(*result.library, "66382436");
            require(first.title == "Demo Track 1" &&
                            first.artist == "Loopmasters" &&
                            first.bpm == 128.0 &&
                            first.durationMilliseconds == 172000 &&
                            second.title == "Demo Track 2" &&
                            second.bpm == 120.0 &&
                            second.durationMilliseconds == 128000,
                    "Independent fixture metadata expectations failed");
            require(result.library->playlists.size() == 2 && result.library->annotations.empty() &&
                            playlist(*result.library, "playlist:2602250856").entries.empty(),
                    "Independent fixture falsely supplied membership/cue evidence");
        });
    }
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
// End of code prepared by ChatGPT for human review and submission.
