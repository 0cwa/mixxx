// Prepared by ChatGPT for human review.
#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

#include "library/rekordbox/rekordboxanlzdecoder.h"
#include "library/rekordbox/rekordboxdatabasesource.h"

#ifdef REKORDBOX_DATABASE_QT_CONSUMER
#include <QCoreApplication>
#include <QSqlDatabase>
#include <QSqlDriver>
#include <QSqlQuery>
#include <QVariant>
#endif

namespace {
namespace fs = std::filesystem;
using namespace mixxx::rekordbox;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string readFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "Fixture file is unavailable");
    return {std::istreambuf_iterator<char>(input), {}};
}

void put(const fs::path& path, const std::string& bytes = "synthetic asset") {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << bytes;
    require(output.good(), "Fixture asset creation failed");
}

void execute(sqlite3* database, const std::string& sql) {
    require(sqlite3_exec(database, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK,
            "Fixture SQL failed");
}

std::map<fs::path, std::string> manifest(const fs::path& root) {
    std::map<fs::path, std::string> result;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            result.emplace(entry.path().lexically_relative(root), readFile(entry.path()));
        } else {
            result.emplace(entry.path().lexically_relative(root), "directory");
        }
    }
    return result;
}

struct Fixture {
    fs::path root;
    fs::path source;
    fs::path temporary;
    DatabaseSourceReadRequest request;

    Fixture(const fs::path& base,
            const std::string& name,
            DatabaseSourceFormat format,
            const std::string& sql,
            const std::string& analysis,
            bool encrypted = false,
            bool wal = false)
            : root(base / name),
              source(root / "source"),
              temporary(root / "private") {
        fs::create_directories(source);
        fs::create_directories(temporary);
        request.format = format;
        request.snapshot.sourcePath = source /
                (format == DatabaseSourceFormat::OneLibrary ? "exportLibrary.db"
                                                            : "master.db");
        request.snapshot.temporaryRoot = temporary;
        request.snapshot.profile = encrypted
                ? (format == DatabaseSourceFormat::OneLibrary
                                  ? DatabaseCipherProfile::OneLibrarySqlCipher4
                                  : DatabaseCipherProfile::Native6SqlCipher4)
                : DatabaseCipherProfile::PlainSqlite;
        request.snapshot.keyProvider = [](DatabaseCipherProfile) {
            const std::string key = "synthetic-pipeline-key";
            return std::vector<unsigned char>(key.begin(), key.end());
        };
        request.context.sourceId = "source-binding-independent-of-mount";
        request.context.generation = 17;
        request.context.originalDatabasePath = request.snapshot.sourcePath;
        request.context.mediaRoot = source;
        request.context.assetRoot = source;
        request.context.cancelled = std::make_shared<std::atomic_bool>(false);
        sqlite3* database = nullptr;
        require(sqlite3_open(request.snapshot.sourcePath.string().c_str(), &database) == SQLITE_OK,
                "Fixture database open failed");
#if defined(SQLITE_HAS_CODEC) && !defined(REKORDBOX_DATABASE_SHARED_CONSUMER)
        if (encrypted) {
            const auto key = request.snapshot.keyProvider(request.snapshot.profile);
            require(sqlite3_key(database, key.data(), static_cast<int>(key.size())) == SQLITE_OK,
                    "Fixture cipher setup failed");
        }
#else
        require(!encrypted, "Encrypted fixture is unavailable in ordinary SQLite consumer");
#endif
        if (wal) {
            execute(database, "PRAGMA journal_mode=WAL");
        }
        execute(database, sql);
        require(sqlite3_close(database) == SQLITE_OK, "Fixture database close failed");
        put(source / fs::path(u8"Contents/Höfn.wav"));
        put(source / "Contents/second.wav");
        put(source / "PIONEER/Artwork/cover.jpg");
        put(source / "PIONEER/USBANLZ/ABC/ANLZ0000.DAT", analysis);
        put(source / fs::path(u8"música/日本語.wav"));
        put(source / "share/PIONEER/USBANLZ/test/ANLZ0000.DAT", analysis);
        put(source / "artwork/cover.png");
    }
    ~Fixture() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};

const DatabaseTrack& track(const DatabaseReadResult& result, std::string_view id) {
    require(result.library != nullptr, "Pipeline did not publish a complete record set");
    const auto found = std::find_if(result.library->tracks.begin(),
            result.library->tracks.end(),
            [&](const auto& item) { return item.sourceTrackId == id; });
    require(found != result.library->tracks.end(), "Expected source track is absent");
    return *found;
}

void checkSharedBoundary() {
#ifdef REKORDBOX_DATABASE_SHARED_CONSUMER
    require(sqlite3_compileoption_used("HAS_CODEC") == 0,
            "The application SQLite C API was replaced by SQLCipher");
    sqlite3* database = nullptr;
    require(sqlite3_open(":memory:", &database) == SQLITE_OK, "Consumer SQLite open failed");
    require(sqlite3_create_function(
                    database,
                    "consumer_check",
                    0,
                    SQLITE_UTF8,
                    nullptr,
                    [](sqlite3_context* context, int, sqlite3_value**) {
                        sqlite3_result_int(context, 42);
                    },
                    nullptr,
                    nullptr) == SQLITE_OK,
            "Consumer SQLite function registration crossed runtimes");
    sqlite3_stmt* statement = nullptr;
    require(sqlite3_prepare_v2(database,
                    "SELECT consumer_check()",
                    -1,
                    &statement,
                    nullptr) == SQLITE_OK,
            "Consumer function query failed");
    require(sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == 42,
            "Consumer SQLite function returned the wrong value");
    sqlite3_finalize(statement);
    sqlite3_close(database);
#endif
}

#ifdef REKORDBOX_DATABASE_QT_CONSUMER
void checkQtBoundary() {
    auto database = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), QStringLiteral("reader-consumer"));
    database.setDatabaseName(QStringLiteral(":memory:"));
    require(database.open(), "Qt SQLite connection could not open");
    auto native = database.driver()->handle();
    require(native.isValid() && std::string(native.typeName()) == "sqlite3*",
            "Qt did not expose a SQLite handle");
    auto* connection = *static_cast<sqlite3**>(native.data());
    require(sqlite3_create_function(
                    connection,
                    "consumer_check",
                    0,
                    SQLITE_UTF8,
                    nullptr,
                    [](sqlite3_context* context, int, sqlite3_value**) {
                        sqlite3_result_int(context, 42);
                    },
                    nullptr,
                    nullptr) == SQLITE_OK,
            "Qt handle crossed the wrong SQLite runtime during function "
            "registration");
    require(sqlite3_create_collation(connection,
                    "consumer_order",
                    SQLITE_UTF8,
                    nullptr,
                    [](void*,
                            int leftBytes,
                            const void* left,
                            int rightBytes,
                            const void* right) {
                        return std::string(
                                static_cast<const char*>(left), leftBytes)
                                .compare(std::string(
                                        static_cast<const char*>(right),
                                        rightBytes));
                    }) == SQLITE_OK,
            "Qt handle crossed the wrong SQLite runtime during collation "
            "registration");
    {
        QSqlQuery query(database);
        require(query.exec(QStringLiteral("SELECT consumer_check(), 'a' < 'b' "
                                          "COLLATE consumer_order")) &&
                        query.next() && query.value(0).toInt() == 42 &&
                        query.value(1).toInt() == 1,
                "Qt function/collation lookup failed after library reading");
    }
    database.close();
    database = {};
    QSqlDatabase::removeDatabase(QStringLiteral("reader-consumer"));
}
#endif

} // namespace

int main(int argc, char** argv) {
#ifdef REKORDBOX_DATABASE_QT_CONSUMER
    QCoreApplication application(argc, argv);
#endif
    if (argc != 5) {
        std::cerr << "Usage: database-source-test onelibrary.sql native.sql "
                     "analysis.DAT temporary-directory\n";
        return 2;
    }
    const std::string modernSql = readFile(argv[1]);
    const std::string nativeSql = readFile(argv[2]);
    const std::string analysis = readFile(argv[3]);
    const auto base = fs::absolute(argv[4]);
    fs::create_directories(base);
    int passed = 0;
    int failed = 0;
    const auto test = [&](const char* name, const std::function<void()>& run) {
        try {
            checkSharedBoundary();
#ifdef REKORDBOX_DATABASE_QT_CONSUMER
            checkQtBoundary();
#endif
            run();
            checkSharedBoundary();
#ifdef REKORDBOX_DATABASE_QT_CONSUMER
            checkQtBoundary();
#endif
            std::cout << "PASS " << name << '\n';
            ++passed;
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
            ++failed;
        }
    };

    const auto exercise = [&](const char* name,
                                  DatabaseSourceFormat format,
                                  bool encrypted,
                                  bool wal) {
        Fixture fixture(base,
                name,
                format,
                format == DatabaseSourceFormat::OneLibrary ? modernSql
                                                           : nativeSql,
                analysis,
                encrypted,
                wal);
        const auto before = manifest(fixture.source);
        {
            const auto result = readDatabaseSource(fixture.request);
            const auto& expected = track(result, "7");
            require(result.library->context.sourceId == fixture.request.context.sourceId &&
                            result.library->context.generation == 17 &&
                            result.library->context.fingerprint.size() == 64,
                    "Source identity, generation or fingerprint was lost");
            require(expected.bpm == 128.5 && expected.analysisPath &&
                            expected.analysisPath->string().find(fixture.source.string()) == 0,
                    "Metadata or original analysis root was lost");
            require(!result.library->capabilities.canonicalApplication &&
                            !result.library->capabilities.qualifiedPerformanceData,
                    "Read success was advertised as canonical or performance acceptance");
            const auto decoded = decodeAnlz(readFile(*expected.analysisPath));
            require(decoded.snapshot && !decoded.snapshot->beats.empty() &&
                            decoded.snapshot->beats.front().time.milliseconds == 26,
                    "Analysis locator did not reach the shared ANLZ decoder");
            if (format == DatabaseSourceFormat::OneLibrary) {
                require(expected.key == "8A" && result.library->tracks.size() == 2,
                        "OneLibrary record mapping changed");
                const auto repeated =
                        std::find_if(result.library->playlists.begin(),
                                result.library->playlists.end(),
                                [](const auto& playlist) {
                                    return playlist.id == "101";
                                });
                require(repeated != result.library->playlists.end() &&
                                repeated->entries.size() == 3 &&
                                repeated->entries[0].sourceTrackId == "7" &&
                                repeated->entries[2].sourceTrackId == "7",
                        "Repeated playlist membership was lost across the "
                        "pipeline");
            } else {
                require(expected.key == "F# minor" && result.library->tracks.size() == 4 &&
                                result.library->annotations.size() == 5,
                        "Native record mapping changed");
                const auto memory =
                        std::find_if(result.library->annotations.begin(),
                                result.library->annotations.end(),
                                [](const auto& cue) {
                                    return cue.sourceAnnotationId == "memory-a";
                                });
                require(memory != result.library->annotations.end() &&
                                memory->startMilliseconds == 1500 &&
                                memory->startFrame150 == 225,
                        "Database timing was silently replaced by disagreeing "
                        "ANLZ data");
            }
        }
        require(fs::is_empty(fixture.temporary), "Source result leaked private plaintext files");
        require(manifest(fixture.source) == before,
                "Source directory changed during the joined read");
    };

#ifndef REKORDBOX_NO_ONELIBRARY
    test("onelibrary_plain_pipeline", [&] {
        exercise(
                "modern-plain", DatabaseSourceFormat::OneLibrary, false, false);
    });
    test("onelibrary_closed_wal_pipeline", [&] {
        exercise("modern-wal", DatabaseSourceFormat::OneLibrary, false, true);
    });
#else
    test("onelibrary_feature_disabled", [&] {
        Fixture fixture(base,
                "modern-disabled",
                DatabaseSourceFormat::OneLibrary,
                modernSql,
                analysis);
        const auto result = readDatabaseSource(fixture.request);
        require(result.outcome == DatabaseOutcome::Unsupported &&
                        !result.library && fs::is_empty(fixture.temporary),
                "Disabled OneLibrary feature accepted a request");
    });
#endif
    test("native_plain_pipeline", [&] {
        exercise("native-plain", DatabaseSourceFormat::Native6, false, false);
    });
    test("native_closed_wal_pipeline", [&] {
        exercise("native-wal", DatabaseSourceFormat::Native6, false, true);
    });

    test("retained_access_and_explicit_binding", [&] {
        Fixture fixture(base, "lease", DatabaseSourceFormat::Native6, nativeSql, analysis);
        auto sourceLease = std::make_shared<int>(1);
        auto contextLease = std::make_shared<int>(2);
        const std::weak_ptr<int> sourceWeak = sourceLease;
        const std::weak_ptr<int> contextWeak = contextLease;
        fixture.request.snapshot.accessLease = sourceLease;
        fixture.request.context.accessLease = contextLease;
        auto result = readDatabaseSource(fixture.request);
        track(result, "7");
        sourceLease.reset();
        contextLease.reset();
        fixture.request.snapshot.accessLease.reset();
        fixture.request.context.accessLease.reset();
        require(!sourceWeak.expired() && !contextWeak.expired(),
                "A source access lease expired before asset decoding");
        result.library.reset();
        require(sourceWeak.expired() && contextWeak.expired() && fs::is_empty(fixture.temporary),
                "Snapshot or access lifetime leaked after record release");
    });

    test("remount_and_overlapping_source_ids", [&] {
        Fixture first(base, "binding-a", DatabaseSourceFormat::Native6, nativeSql, analysis);
        Fixture second(base, "binding-b", DatabaseSourceFormat::Native6, nativeSql, analysis);
        const auto before = readDatabaseSource(first.request);
        second.request.context.sourceId = "independent-device";
        const auto other = readDatabaseSource(second.request);
        require(track(before, "7").sourceTrackId == track(other, "7").sourceTrackId &&
                        before.library->context.sourceId != other.library->context.sourceId,
                "Overlapping IDs from distinct sources were conflated");
        const auto moved = first.root / "remounted";
        fs::rename(first.source, moved);
        first.request.snapshot.sourcePath = moved / "master.db";
        first.request.context.mediaRoot = moved;
        first.request.context.assetRoot = moved;
        const auto after = readDatabaseSource(first.request);
        require(track(after, "7").sourceTrackId == track(before, "7").sourceTrackId &&
                        after.library->context.sourceId == before.library->context.sourceId &&
                        after.library->context.fingerprint == before.library->context.fingerprint,
                "Explicit source binding changed across a remount");
    });

    test("profile_mismatch_refused_before_key_access", [&] {
        Fixture fixture(base, "profile", DatabaseSourceFormat::OneLibrary, modernSql, analysis);
        fixture.request.snapshot.profile = DatabaseCipherProfile::Native6SqlCipher4;
        bool requestedKey = false;
        fixture.request.snapshot.keyProvider = [&](DatabaseCipherProfile) {
            requestedKey = true;
            return std::vector<unsigned char>{};
        };
        const auto result = readDatabaseSource(fixture.request);
        require(result.outcome == DatabaseOutcome::Unsupported && !result.library && !requestedKey,
                "Mismatched cipher profile reached a reader or key provider");
    });

    test("cancelled_request_has_no_partial_records", [&] {
        Fixture fixture(base, "cancelled", DatabaseSourceFormat::Native6, nativeSql, analysis);
        fixture.request.context.cancelled->store(true);
        const auto result = readDatabaseSource(fixture.request);
        require(result.outcome == DatabaseOutcome::Cancelled &&
                        !result.library && fs::is_empty(fixture.temporary),
                "Cancellation published records or retained temporary files");
    });

    test("unsupported_schema_has_no_partial_records", [&] {
        Fixture fixture(base,
                "unsupported",
                DatabaseSourceFormat::Native6,
                nativeSql + "UPDATE djmdProperty SET DBVersion='future';",
                analysis);
        const auto result = readDatabaseSource(fixture.request);
        require(result.outcome == DatabaseOutcome::Unsupported &&
                        !result.library && fs::is_empty(fixture.temporary),
                "Unsupported schema published a partial collection");
    });

#if defined(SQLITE_HAS_CODEC) && !defined(REKORDBOX_DATABASE_SHARED_CONSUMER)
    test("onelibrary_encrypted_pipeline", [&] {
        exercise("modern-encrypted",
                DatabaseSourceFormat::OneLibrary,
                true,
                false);
    });
    test("native_encrypted_pipeline", [&] {
        exercise(
                "native-encrypted", DatabaseSourceFormat::Native6, true, false);
    });
    test("onelibrary_encrypted_closed_wal_pipeline", [&] {
        exercise("modern-encrypted-wal",
                DatabaseSourceFormat::OneLibrary,
                true,
                true);
    });
    test("native_encrypted_closed_wal_pipeline", [&] {
        exercise("native-encrypted-wal",
                DatabaseSourceFormat::Native6,
                true,
                true);
    });

    test("key_unavailable_is_actionable", [&] {
        Fixture fixture(base, "no-key", DatabaseSourceFormat::Native6, nativeSql, analysis, true);
        fixture.request.snapshot.keyProvider = {};
        const auto result = readDatabaseSource(fixture.request);
        require(result.outcome == DatabaseOutcome::Unavailable &&
                        !result.library && !result.issues.empty() &&
                        result.issues.front().code ==
                                "snapshot-key-unavailable",
                "Unavailable key was not distinguished from schema support");
    });
#endif

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
// End of code prepared by ChatGPT for human review and submission.
