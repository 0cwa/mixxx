// Prepared by ChatGPT for human review.
#include <djinterop/djinterop.hpp>
#include <djinterop/onelibrary/v1/library.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#if PROBE_WITH_SYSTEM_SQLITE
#include <sqlite3.h>
#endif

#if PROBE_WITH_QT_SQLITE
#include <QCoreApplication>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVariant>
#endif

namespace {
namespace fs = std::filesystem;
namespace ol = djinterop::onelibrary;
using Manifest = std::map<fs::path, std::string>;
int sChecks = 0;

void check(bool condition, const char* label) {
    if (!condition) {
        throw std::runtime_error(label);
    }
    ++sChecks;
    std::cout << "PASS " << label << '\n';
}

std::string randomToken() {
    std::random_device random;
    std::string token;
    for (int i = 0; i < 8; ++i) {
        token += std::to_string(random());
    }
    return token;
}

class TemporaryDirectory {
  public:
    TemporaryDirectory()
            : m_path(fs::temp_directory_path() / ("mixxx-onelibrary-" + randomToken())) {
        if (!fs::create_directory(m_path)) {
            throw std::runtime_error("temporary directory creation");
        }
        fs::permissions(m_path, fs::perms::owner_all);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        fs::remove_all(m_path, error);
    }

    const fs::path& path() const {
        return m_path;
    }

  private:
    fs::path m_path;
};

Manifest manifest(const fs::path& root) {
    Manifest result;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            std::ifstream file(entry.path(), std::ios::binary);
            if (!file) {
                throw std::runtime_error("manifest file access");
            }
            result.emplace(fs::relative(entry.path(), root),
                    std::string(std::istreambuf_iterator<char>(file), {}));
        } else if (entry.is_directory()) {
            result.emplace(fs::relative(entry.path(), root) / ".", "");
        }
    }
    return result;
}

fs::path writeScript(const fs::path& root,
        const std::vector<std::string>& statements) {
    const auto scripts = root / "scripts";
    fs::create_directories(scripts);
    std::ifstream schema(fs::path(PROBE_SCHEMA_DIRECTORY) / "exportLibrary.db.sql");
    std::ofstream output(scripts / "exportLibrary.db.sql");
    if (!schema || !output) {
        throw std::runtime_error("fixture script access");
    }
    output << schema.rdbuf() << '\n';
    for (const auto& statement : statements) {
        output << statement << '\n';
    }
    output.close();
    if (!output) {
        throw std::runtime_error("fixture script write");
    }
    return scripts;
}

std::vector<std::string> populatedStatements() {
    return {
            "INSERT INTO artist(artist_id,name) VALUES(1,'Artist'),(2,'Composer');",
            "INSERT INTO album(album_id,name) VALUES(1,'Album');",
            "INSERT INTO genre(genre_id,name) VALUES(1,'Genre');",
            "INSERT INTO label(label_id,name) VALUES(1,'Publisher');",
            "INSERT INTO key(key_id,name) VALUES(1,'8A');",
            "INSERT INTO content(content_id,title,artist_id_artist,artist_id_composer,"
            "album_id,genre_id,label_id,key_id,djComment,bpmx100,length,trackNo,releaseYear,"
            "rating,path,fileSize,bitrate,samplingRate,analysisDataFilePath,color_id,"
            "masterDbId) VALUES(10,'\u00c1rbol',1,2,1,1,1,1,'Note',12450,161,2,2024,4,"
            "'/Contents/\u00c1rbol.flac',12345678,1000,48000,"
            "'/PIONEER/USBANLZ/A/ANLZ0000.DAT',8,7654321);",
            "INSERT INTO content(content_id,title,djComment,path,masterDbId) "
            "VALUES(20,'',NULL,'/Contents/missing.wav',7654321),"
            "(30,NULL,'','/Contents/missing.wav',7654321);",
            "INSERT INTO playlist(playlist_id,sequenceNo,name,attribute,playlist_id_parent) "
            "VALUES(101,2,'Second root',0,NULL),(100,1,'Folder',1,0),"
            "(102,1,'Nested',0,100);",
            "INSERT INTO playlist_content(playlist_id,content_id,sequenceNo) "
            "VALUES(102,20,2),(102,10,1),(102,10,3);",
            "UPDATE property SET deviceName='Synthetic fixture',numberOfContents=3;",
    };
}

void testEngine(const fs::path& root) {
    const auto path = root / "Engine Library";
    {
        auto database = djinterop::engine::create_database(path);
        djinterop::track_snapshot snapshot;
        snapshot.title = "Engine smoke test";
        snapshot.relative_path = "Music/example.wav";
        snapshot.bpm = 123.0;
        const auto track = database.create_track(snapshot);
        auto playlist = database.create_root_playlist("Smoke test");
        playlist.add_track_back(track);
    }
    auto reloaded = djinterop::engine::load_database(path);
    const auto tracks = reloaded.tracks();
    check(tracks.size() == 1 && tracks.front().title() == "Engine smoke test",
            "Engine track creation and reload");
    const auto playlists = reloaded.root_playlists();
    check(playlists.size() == 1 && playlists.front().tracks().size() == 1,
            "Engine playlist membership and reload");
}

void testOneLibrary(const fs::path& root) {
    const auto scripts = writeScript(root, populatedStatements());
    const auto device = root / "device";
    const auto key = randomToken();
#if !PROBE_HAS_SQLCIPHER
    bool rejected = false;
    try {
        static_cast<void>(ol::v1::library::create_from_scripts(device, scripts, key));
    } catch (const djinterop::unsupported_database&) {
        rejected = true;
    }
    check(rejected, "crypto-disabled build rejects OneLibrary");
    return;
#else
    {
        auto created = ol::v1::library::create_from_scripts(device, scripts, key);
        check(created.content().all_ids().size() == 3, "encrypted fixture creation");
    }
    const auto before = manifest(device);
    {
        auto library = ol::v1::library::load(device, key);
        check(library.property().get_db_version() == "1000", "schema version access");
        check(library.content().all_ids() == std::vector<int64_t>({10, 20, 30}),
                "ordered source track identifiers");
        const auto row = library.content().get(10);
        check(row.has_value(), "low-level content row access");
        check(row->title == "\u00c1rbol" && row->artist == "Artist" &&
                        row->composer == "Composer" && row->album == "Album" &&
                        row->genre == "Genre" && row->label == "Publisher",
                "Unicode metadata and lookup fields");
        check(row->key == "8A" && row->color_id == 8 && row->comment == "Note",
                "original key notation and color");
        check(row->bpm_x100 == 12450 && row->length == std::chrono::seconds(161),
                "explicit tempo and duration units");
        check(row->rating_stars == 4 && row->track_number == 2 && row->release_year == 2024,
                "rating track number and year");
        check(row->file_size == 12345678 && row->bitrate == 1000 &&
                        row->sampling_rate == 48000,
                "file size bitrate and sample rate");
        check(row->path == "/Contents/\u00c1rbol.flac" &&
                        row->analysis_path == "/PIONEER/USBANLZ/A/ANLZ0000.DAT",
                "original media and analysis paths");
        check(library.playlist().root_ids() == std::vector<int64_t>({100, 101}) &&
                        library.playlist().child_ids(100) == std::vector<int64_t>({102}),
                "ordered playlist hierarchy");
        check(library.playlist().track_ids(102) == std::vector<int64_t>({10, 20, 10}),
                "playlist order preserves repeated membership");
        check(library.content().get(20)->title == std::nullopt &&
                        library.content().get(30)->title == std::nullopt,
                "qualification limitation: empty text collapses to absent");
        auto database = library.database();
        check(database.uuid() == "7654321",
                "qualification limitation: uuid represents originating collection");
        const auto snapshot = database.track_by_id(10)->snapshot();
        check(snapshot.hot_cues.empty() && snapshot.loops.empty() && snapshot.beatgrid.empty(),
                "qualification limitation: performance data needs ANLZ");
        bool rejected = false;
        try {
            database.track_by_id(10)->set_title(std::string("changed"));
        } catch (const djinterop::unsupported_operation&) {
            rejected = true;
        }
        check(rejected, "OneLibrary mutation API is rejected");
    }
    check(manifest(device) == before, "complete non-WAL fixture manifest is unchanged");
    bool wrongKeyRejected = false;
    try {
        static_cast<void>(ol::v1::library::load(device, randomToken()));
    } catch (const djinterop::unsupported_database&) {
        wrongKeyRejected = true;
    }
    check(wrongKeyRejected, "wrong key is rejected without disclosure");

    const auto emptyScripts = writeScript(root / "empty", {});
    const auto empty = ol::v1::library::create_from_scripts(
            root / "empty-device", emptyScripts, key);
    check(empty.content().all_ids().empty() && empty.database().uuid().empty(),
            "empty source needs an external source identity");

    const auto futureScripts = writeScript(root / "future",
            {"UPDATE property SET dbVersion='future-version';"});
    const auto future = ol::v1::library::create_from_scripts(
            root / "future-device", futureScripts, key);
    check(future.property().get_db_version() == "future-version",
            "qualification limitation: caller must gate unsupported schema versions");

    const auto plain = ol::v1::library::create_from_scripts(
            root / "plain-device", scripts, "");
    check(plain.content().all_ids().size() == 3,
            "plaintext private snapshot accepts an empty private key");

    const auto walScripts = writeScript(root / "wal", {"PRAGMA journal_mode=WAL;"});
    const auto walDevice = root / "wal-device";
    {
        static_cast<void>(ol::v1::library::create_from_scripts(walDevice, walScripts, key));
    }
    const auto walDatabase = walDevice / ol::database_relative_path;
    fs::remove(walDatabase.string() + "-wal");
    fs::remove(walDatabase.string() + "-shm");
    const auto walBefore = manifest(walDevice);
    {
        const auto wal = ol::v1::library::load(walDevice, key);
        check(wal.content().all_ids().empty(), "WAL-mode fixture metadata can be read");
        check(manifest(walDevice) != walBefore,
                "qualification limitation: direct WAL open changes source directory");
    }
#endif
}

void testSystemSqlite() {
#if PROBE_WITH_SYSTEM_SQLITE
    sqlite3* database = nullptr;
    check(sqlite3_open(":memory:", &database) == SQLITE_OK, "ordinary SQLite connection");
    const auto result = sqlite3_exec(database,
            "CREATE TABLE coexistence(value);INSERT INTO coexistence VALUES(42);",
            nullptr,
            nullptr,
            nullptr);
    check(result == SQLITE_OK, "ordinary SQLite writes beside encrypted reader");
    check(sqlite3_compileoption_used("HAS_CODEC") == 0,
            "ordinary SQLite symbols do not resolve to SQLCipher");
    sqlite3_close(database);
#else
    std::cout << "SKIP ordinary SQLite coexistence: optional library not linked\n";
#endif
}

void testQtSqlite() {
#if PROBE_WITH_QT_SQLITE
    const auto name = QStringLiteral("rekordbox-dependency-probe");
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        database.setDatabaseName(QStringLiteral(":memory:"));
        check(database.open(), "Qt QSQLITE plugin opens beside encrypted reader");
        QSqlQuery query(database);
        check(query.exec(QStringLiteral("CREATE TABLE probe(value INTEGER)")) &&
                        query.exec(QStringLiteral("INSERT INTO probe VALUES(42)")) &&
                        query.exec(QStringLiteral("SELECT value FROM probe")) &&
                        query.next() && query.value(0).toInt() == 42,
                "Qt QSQLITE write and read coexistence");
    }
    QSqlDatabase::removeDatabase(name);
#else
    std::cout << "SKIP Qt SQLite coexistence: configure PROBE_WITH_QT_SQLITE=ON\n";
#endif
}
} // namespace

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {
    try {
#if PROBE_WITH_QT_SQLITE
        QCoreApplication application(argc, argv);
#endif
        TemporaryDirectory temporary;
        testSystemSqlite();
        testQtSqlite();
        testEngine(temporary.path());
        testOneLibrary(temporary.path());
        testSystemSqlite();
        testQtSqlite();
        std::cout << "Passed " << sChecks << " dependency qualification checks\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Dependency probe failed: " << exception.what() << '\n';
        return 1;
    }
}
// End of ChatGPT-prepared probe.
