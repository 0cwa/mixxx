// Prepared by ChatGPT for human review.
#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "library/rekordbox/rekordboxdatabasesnapshot.h"

namespace {
namespace fs = std::filesystem;
using namespace mixxx::rekordbox;

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

void execute(sqlite3* database, const std::string& sql) {
    if (sqlite3_exec(database, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Fixture SQL failed");
    }
}

std::int64_t scalar(sqlite3* database, const char* sql) {
    sqlite3_stmt* statement = nullptr;
    require(sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) == SQLITE_OK,
            "Scalar prepare failed");
    const int rc = sqlite3_step(statement);
    const auto value = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    require(rc == SQLITE_ROW, "Scalar read failed");
    return value;
}

struct Database {
    sqlite3* value = nullptr;
    explicit Database(const fs::path& path, bool encrypted = false) {
        require(sqlite3_open(path.string().c_str(), &value) == SQLITE_OK, "Fixture open failed");
#ifdef SQLITE_HAS_CODEC
        if (encrypted) {
            // A synthetic per-test credential, unrelated to vendor keys.
            require(sqlite3_key(value, "synthetic-fixture-only", 22) == SQLITE_OK,
                    "Fixture key setup failed");
        }
#else
        require(!encrypted, "Encrypted fixture requires SQLCipher");
#endif
    }
    ~Database() {
        sqlite3_close_v2(value);
    }
};

struct Entry {
    std::string bytes;
    fs::file_time_type modified;
    fs::perms permissions;
    bool operator==(const Entry&) const = default;
};

std::map<std::string, Entry> manifest(const fs::path& directory) {
    std::map<std::string, Entry> result;
    for (const auto& entry : fs::recursive_directory_iterator(directory)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::ifstream input(entry.path(), std::ios::binary);
        result.emplace(entry.path().lexically_relative(directory).generic_string(),
                Entry{{std::istreambuf_iterator<char>(input), {}},
                        entry.last_write_time(),
                        entry.status().permissions()});
    }
    return result;
}

struct Fixture {
    fs::path root;
    fs::path sourceDirectory;
    fs::path temporaryDirectory;
    fs::path databasePath;
    explicit Fixture(const fs::path& base, const std::string& name)
            : root(base / name),
              sourceDirectory(root / "source"),
              temporaryDirectory(root / "private"),
              databasePath(sourceDirectory / "library.db") {
        fs::create_directories(sourceDirectory);
        fs::create_directories(temporaryDirectory);
    }
    DatabaseSnapshotRequest request(bool encrypted = false) const {
        DatabaseSnapshotRequest result;
        result.sourcePath = databasePath;
        result.temporaryRoot = temporaryDirectory;
        result.profile = encrypted ? DatabaseCipherProfile::Native6SqlCipher4
                                   : DatabaseCipherProfile::PlainSqlite;
        result.timeout = std::chrono::seconds(5);
        result.keyProvider = [](DatabaseCipherProfile) {
            const std::string key("synthetic-fixture-only", 22);
            return std::vector<unsigned char>(key.begin(), key.end());
        };
        return result;
    }
};

void createRows(sqlite3* database, int count = 1) {
    execute(database,
            "CREATE TABLE tracks(id INTEGER PRIMARY KEY, name TEXT, empty "
            "TEXT, absent TEXT)");
    execute(database,
            "WITH RECURSIVE numbers(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM "
            "numbers WHERE x<" +
                    std::to_string(count) +
                    ") INSERT INTO tracks SELECT x, printf('%01000d', x), '', "
                    "NULL FROM numbers");
}

void checkSnapshot(const DatabaseSnapshotResult& result, int count = 1) {
    if (result.outcome != DatabaseSnapshotOutcome::Success) {
        std::cerr << "snapshot refusal " << static_cast<int>(result.outcome)
                  << ", SQLite code " << result.databaseCode << ": " << result.diagnostic << '\n';
    }
    require(result.outcome == DatabaseSnapshotOutcome::Success && result.snapshot,
            "Expected a complete snapshot");
    require(scalar(result.snapshot->database(), "SELECT count(*) FROM tracks") == count,
            "Snapshot has wrong committed rows");
    require(scalar(result.snapshot->database(),
                    "SELECT count(*) FROM tracks WHERE empty='' AND absent IS NULL") == count,
            "Null and empty field distinctions were lost");
    require(result.snapshot->fingerprint().size() == 64, "Missing SHA-256 fingerprint");
    require(sqlite3_exec(result.snapshot->database(),
                    "DELETE FROM tracks",
                    nullptr,
                    nullptr,
                    nullptr) != SQLITE_OK,
            "Published snapshot database must reject writes");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: rekordbox-database-snapshot-test TEMPORARY_DIRECTORY\n";
        return 2;
    }
    const fs::path base = fs::absolute(argv[1]);
    fs::create_directories(base);
    int passed = 0;
    int failed = 0;
    const auto test = [&](const char* name, const std::function<void(Fixture&)>& run) {
        Fixture fixture(base, name);
        try {
            run(fixture);
            require(fs::is_empty(fixture.temporaryDirectory),
                    "Private snapshots leaked after operation");
            std::cout << "PASS " << name << '\n';
            ++passed;
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
            ++failed;
        }
        fs::permissions(fixture.sourceDirectory, fs::perms::owner_all, fs::perm_options::add);
        for (const auto& entry : fs::directory_iterator(fixture.sourceDirectory)) {
            fs::permissions(entry.path(), fs::perms::owner_all, fs::perm_options::add);
        }
        fs::remove_all(fixture.root);
    };

    test("plain_closed_directory_integrity", [](Fixture& fixture) {
        {
            Database source(fixture.databasePath);
            createRows(source.value);
        }
        const auto before = manifest(fixture.sourceDirectory);
        {
            const auto result = snapshotDatabase(fixture.request());
            checkSnapshot(result);
            require(manifest(fixture.sourceDirectory) == before,
                    "Source files changed during read");
        }
        require(manifest(fixture.sourceDirectory) == before, "Source files changed during cleanup");
    });

    test("committed_wal_only_rows", [](Fixture& fixture) {
        Database writer(fixture.databasePath);
        execute(writer.value, "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0");
        createRows(writer.value, 7);
        require(fs::file_size(fixture.databasePath.string() + "-wal") > 0, "WAL fixture missing");
        const auto before = manifest(fixture.sourceDirectory);
        {
            const auto result = snapshotDatabase(fixture.request());
            checkSnapshot(result, 7);
        }
        require(manifest(fixture.sourceDirectory) == before, "Main/WAL/SHM changed");
    });

    test("wal_uncommitted_rows_excluded", [](Fixture& fixture) {
        Database writer(fixture.databasePath);
        execute(writer.value, "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0");
        createRows(writer.value);
        execute(writer.value, "BEGIN; INSERT INTO tracks VALUES(99, 'pending', '', NULL)");
        const auto before = manifest(fixture.sourceDirectory);
        {
            const auto result = snapshotDatabase(fixture.request());
            checkSnapshot(result);
        }
        require(manifest(fixture.sourceDirectory) == before, "Uncommitted source was mutated");
        execute(writer.value, "ROLLBACK");
    });

    test("concurrent_commit_fixed_view", [](Fixture& fixture) {
        Database writer(fixture.databasePath);
        execute(writer.value, "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0");
        createRows(writer.value, 400);
        auto request = fixture.request();
        request.pagesPerStep = 1;
        int calls = 0;
        bool committed = false;
        request.cancelled = [&] {
            if (++calls == 4) {
                execute(writer.value, "INSERT INTO tracks VALUES(9999, 'new commit', '', NULL)");
                committed = true;
            }
            return false;
        };
        const auto result = snapshotDatabase(request);
        checkSnapshot(result, 400);
        require(committed, "Concurrent writer did not execute");
        require(scalar(writer.value, "SELECT count(*) FROM tracks") == 401,
                "Writer commit missing");
    });

    test("readonly_source_directory", [](Fixture& fixture) {
        {
            Database source(fixture.databasePath);
            createRows(source.value);
        }
        fs::permissions(fixture.databasePath, fs::perms::owner_read, fs::perm_options::replace);
        fs::permissions(fixture.sourceDirectory,
                fs::perms::owner_read | fs::perms::owner_exec,
                fs::perm_options::replace);
        const auto before = manifest(fixture.sourceDirectory);
        {
            const auto result = snapshotDatabase(fixture.request());
            checkSnapshot(result);
        }
        require(manifest(fixture.sourceDirectory) == before, "Read-only source changed");
    });

    test("closed_wal_without_sidecars", [](Fixture& fixture) {
        {
            Database source(fixture.databasePath);
            execute(source.value, "PRAGMA journal_mode=WAL");
            createRows(source.value);
        }
        require(!fs::exists(fixture.databasePath.string() + "-wal"), "Fixture did not close WAL");
        const auto before = manifest(fixture.sourceDirectory);
        {
            const auto result = snapshotDatabase(fixture.request());
            checkSnapshot(result);
            require(result.usedQuiescentCopy, "Closed WAL must use verified quiescent capture");
        }
        require(manifest(fixture.sourceDirectory) == before, "Absent sidecars were created");
    });

    test("orphan_wal_locked_copy", [](Fixture& fixture) {
        const auto other = fixture.root / "writer.db";
        Database writer(other);
        execute(writer.value, "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0");
        createRows(writer.value);
        fs::copy_file(other, fixture.databasePath);
        fs::copy_file(other.string() + "-wal", fixture.databasePath.string() + "-wal");
        const auto before = manifest(fixture.sourceDirectory);
        const auto result = snapshotDatabase(fixture.request());
        checkSnapshot(result);
        require(result.usedQuiescentCopy, "Orphan WAL must use verified quiescent capture");
        require(manifest(fixture.sourceDirectory) == before, "Missing SHM was created");
    });

    test("cancellation_before_read", [](Fixture& fixture) {
        auto request = fixture.request();
        request.cancelled = [] { return true; };
        const auto result = snapshotDatabase(request);
        require(result.outcome == DatabaseSnapshotOutcome::Cancelled && !result.snapshot,
                "Pre-cancelled request was not cancelled");
    });

    test("cancellation_during_backup", [](Fixture& fixture) {
        {
            Database source(fixture.databasePath);
            createRows(source.value, 400);
        }
        const auto before = manifest(fixture.sourceDirectory);
        auto request = fixture.request();
        request.pagesPerStep = 1;
        int calls = 0;
        request.cancelled = [&] { return ++calls > 5; };
        const auto result = snapshotDatabase(request);
        require(result.outcome == DatabaseSnapshotOutcome::Cancelled && !result.snapshot,
                "Cancelled backup published partial data");
        require(manifest(fixture.sourceDirectory) == before, "Cancelled backup mutated source");
    });

    test("busy_deadline", [](Fixture& fixture) {
        Database writer(fixture.databasePath);
        createRows(writer.value);
        execute(writer.value, "BEGIN EXCLUSIVE");
        auto request = fixture.request();
        request.timeout = std::chrono::milliseconds(60);
        const auto start = std::chrono::steady_clock::now();
        const auto result = snapshotDatabase(request);
        require(result.outcome == DatabaseSnapshotOutcome::Busy && !result.snapshot,
                "Locked source must return bounded busy result");
        require(std::chrono::steady_clock::now() - start < std::chrono::seconds(2),
                "Busy timeout was unbounded");
        execute(writer.value, "ROLLBACK");
    });

    test("missing_source", [](Fixture& fixture) {
        const auto result = snapshotDatabase(fixture.request());
        require(result.outcome == DatabaseSnapshotOutcome::Unavailable && !result.snapshot,
                "Missing source was not reported");
    });

    test("corrupt_source", [](Fixture& fixture) {
        std::ofstream(fixture.databasePath) << "this is not a database";
        const auto before = manifest(fixture.sourceDirectory);
        const auto result = snapshotDatabase(fixture.request());
        require(result.outcome == DatabaseSnapshotOutcome::Invalid && !result.snapshot,
                "Corruption was not reported");
        require(manifest(fixture.sourceDirectory) == before, "Corrupt source was changed");
    });

    test("size_limit", [](Fixture& fixture) {
        {
            Database source(fixture.databasePath);
            createRows(source.value, 10);
        }
        auto request = fixture.request();
        request.maximumBytes = 512;
        const auto result = snapshotDatabase(request);
        require(result.outcome == DatabaseSnapshotOutcome::Unsupported && !result.snapshot,
                "Size limit was ignored");
    });

    test("no_temporary_files_in_source", [](Fixture& fixture) {
        {
            Database source(fixture.databasePath);
            createRows(source.value);
        }
        auto request = fixture.request();
        request.temporaryRoot = fixture.sourceDirectory;
        const auto before = manifest(fixture.sourceDirectory);
        const auto result = snapshotDatabase(request);
        require(result.outcome == DatabaseSnapshotOutcome::Invalid && !result.snapshot,
                "Snapshot storage inside source was accepted");
        require(manifest(fixture.sourceDirectory) == before, "Source storage was modified");
    });

    test("access_lease_and_cleanup", [](Fixture& fixture) {
        {
            Database source(fixture.databasePath);
            createRows(source.value);
        }
        auto lease = std::make_shared<int>(7);
        const std::weak_ptr<int> weak = lease;
        auto request = fixture.request();
        request.accessLease = lease;
        auto result = snapshotDatabase(request);
        checkSnapshot(result);
        const auto snapshotPath = result.snapshot->path();
        lease.reset();
        request.accessLease.reset();
        require(!weak.expired(), "Snapshot dropped the source access lease");
        result.snapshot.reset();
        require(weak.expired(), "Source access lease leaked");
        require(!fs::exists(snapshotPath), "Private database was not removed");
    });

    test("special_filename_uri", [](Fixture& fixture) {
        fixture.databasePath = fixture.sourceDirectory / fs::path(u8"músiq?#%.db");
        {
            Database source(fixture.databasePath);
            createRows(source.value);
        }
        checkSnapshot(snapshotDatabase(fixture.request()));
    });

    test("source_removed_during_read", [](Fixture& fixture) {
        {
            Database source(fixture.databasePath);
            createRows(source.value, 400);
        }
        auto request = fixture.request();
        request.pagesPerStep = 1;
        int calls = 0;
        request.cancelled = [&] {
            if (++calls == 5) {
                fs::rename(fixture.databasePath, fixture.databasePath.string() + ".removed");
            }
            return false;
        };
        const auto result = snapshotDatabase(request);
        require(result.outcome == DatabaseSnapshotOutcome::Unavailable && !result.snapshot,
                "Removed source published a snapshot");
    });

    test("source_replaced_during_read", [](Fixture& fixture) {
        {
            Database source(fixture.databasePath);
            createRows(source.value, 400);
        }
        auto request = fixture.request();
        request.pagesPerStep = 1;
        int calls = 0;
        request.cancelled = [&] {
            if (++calls == 5) {
                fs::rename(fixture.databasePath, fixture.databasePath.string() + ".old");
                std::ofstream(fixture.databasePath) << "replacement";
            }
            return false;
        };
        const auto result = snapshotDatabase(request);
        require(result.outcome != DatabaseSnapshotOutcome::Success && !result.snapshot,
                "Replaced source published stale snapshot");
    });

#ifdef SQLITE_HAS_CODEC
    test("encrypted_closed_source", [](Fixture& fixture) {
        {
            Database source(fixture.databasePath, true);
            createRows(source.value, 4);
            execute(source.value, "PRAGMA user_version=321; PRAGMA application_id=12345");
        }
        const auto before = manifest(fixture.sourceDirectory);
        {
            const auto result = snapshotDatabase(fixture.request(true));
            checkSnapshot(result, 4);
            require(scalar(result.snapshot->database(), "PRAGMA user_version") == 321,
                    "Cipher export lost user_version");
            require(scalar(result.snapshot->database(), "PRAGMA application_id") == 12345,
                    "Cipher export lost application_id");
        }
        require(manifest(fixture.sourceDirectory) == before, "Encrypted source was changed");
    });

    test("encrypted_closed_wal", [](Fixture& fixture) {
        {
            Database source(fixture.databasePath, true);
            execute(source.value, "PRAGMA journal_mode=WAL");
            createRows(source.value, 4);
        }
        const auto before = manifest(fixture.sourceDirectory);
        {
            const auto result = snapshotDatabase(fixture.request(true));
            checkSnapshot(result, 4);
            require(result.usedQuiescentCopy, "Encrypted closed WAL missed locked copy");
        }
        require(manifest(fixture.sourceDirectory) == before, "Encrypted closed WAL source changed");
    });

    test("encrypted_committed_wal", [](Fixture& fixture) {
        Database writer(fixture.databasePath, true);
        execute(writer.value, "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0");
        createRows(writer.value, 8);
        const auto before = manifest(fixture.sourceDirectory);
        {
            const auto result = snapshotDatabase(fixture.request(true));
            checkSnapshot(result, 8);
        }
        require(manifest(fixture.sourceDirectory) == before, "Encrypted WAL/SHM changed");
    });

    test("wrong_key_is_private_failure", [](Fixture& fixture) {
        {
            Database source(fixture.databasePath, true);
            createRows(source.value);
        }
        const auto before = manifest(fixture.sourceDirectory);
        auto request = fixture.request(true);
        request.keyProvider = [](DatabaseCipherProfile) {
            return std::vector<unsigned char>{'w', 'r', 'o', 'n', 'g'};
        };
        const auto result = snapshotDatabase(request);
        require(result.outcome == DatabaseSnapshotOutcome::Invalid && !result.snapshot,
                "Wrong key was accepted");
        require(result.diagnostic.find("wrong") == std::string::npos,
                "Credential contents leaked in a diagnostic");
        require(manifest(fixture.sourceDirectory) == before, "Wrong-key source changed");
    });

    test("missing_key_and_separate_profile", [](Fixture& fixture) {
        {
            Database source(fixture.databasePath, true);
            createRows(source.value);
        }
        auto request = fixture.request(true);
        request.profile = DatabaseCipherProfile::OneLibrarySqlCipher4;
        bool requested = false;
        request.keyProvider = [&](DatabaseCipherProfile profile) {
            require(profile == DatabaseCipherProfile::OneLibrarySqlCipher4,
                    "OneLibrary and native profiles were conflated");
            requested = true;
            return std::vector<unsigned char>{};
        };
        const auto result = snapshotDatabase(request);
        require(requested &&
                        result.outcome ==
                                DatabaseSnapshotOutcome::KeyUnavailable &&
                        !result.snapshot,
                "Missing key was not actionable");
    });

    test("encrypted_export_cancellation", [](Fixture& fixture) {
        {
            Database source(fixture.databasePath, true);
            createRows(source.value, 3000);
        }
        const auto before = manifest(fixture.sourceDirectory);
        auto request = fixture.request(true);
        int calls = 0;
        request.cancelled = [&] { return ++calls > 4; };
        const auto result = snapshotDatabase(request);
        require(result.outcome == DatabaseSnapshotOutcome::Cancelled && !result.snapshot,
                "Encrypted cancellation published partial data");
        require(manifest(fixture.sourceDirectory) == before,
                "Cancelled export changed encrypted source");
    });
#else
    test("cipher_feature_disabled", [](Fixture& fixture) {
        {
            Database source(fixture.databasePath);
            createRows(source.value);
        }
        const auto result = snapshotDatabase(fixture.request(true));
        require(result.outcome == DatabaseSnapshotOutcome::Unsupported && !result.snapshot,
                "Build without SQLCipher did not refuse encrypted input");
    });
#endif

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
// End of code prepared by ChatGPT for human review and submission.
