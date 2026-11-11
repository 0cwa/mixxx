// Prepared by ChatGPT for human review.
#include <sqlite3.h>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

#include "library/rekordbox/rekordboxdatabasesnapshot.h"

namespace {

using namespace mixxx::rekordbox;

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

std::int64_t scalar(sqlite3* database, const char* sql) {
    sqlite3_stmt* raw = nullptr;
    require(sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) == SQLITE_OK,
            "Could not prepare the private snapshot assertion.");
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
    require(sqlite3_step(statement.get()) == SQLITE_ROW,
            "Could not read the private snapshot assertion.");
    return sqlite3_column_int64(statement.get(), 0);
}

bool privateDestinationExists(const std::filesystem::path& root) {
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.is_directory() && std::filesystem::exists(entry.path() / "library.sqlite")) {
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        std::cerr << "Usage: live-wal-driver SOURCE_DATABASE PRIVATE_DIRECTORY [CONTINUE_GATE]\n";
        return 2;
    }
    try {
        DatabaseSnapshotRequest request;
        request.sourcePath = std::filesystem::absolute(argv[1]);
        request.temporaryRoot = std::filesystem::absolute(argv[2]);
        request.profile = DatabaseCipherProfile::PlainSqlite;
        request.timeout = std::chrono::seconds(15);
        request.pagesPerStep = 1;
        const auto gate = argc == 4 ? std::filesystem::absolute(argv[3]) : std::filesystem::path{};
        bool coordinated = false;
        bool gateTimeout = false;
        request.cancelled = [&] {
            if (!gate.empty() && !coordinated && privateDestinationExists(request.temporaryRoot)) {
                // The online path creates its private destination after pinning
                // the source read transaction. Pause at that observable point
                // so the other process can commit before copying completes.
                coordinated = true;
                std::cout << "{\"event\":\"read-pinned\"}" << std::endl;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while (!std::filesystem::exists(gate)) {
                    if (std::chrono::steady_clock::now() >= deadline) {
                        gateTimeout = true;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            return gateTimeout;
        };
        auto result = snapshotDatabase(request);
        if (result.outcome != DatabaseSnapshotOutcome::Success) {
            std::cerr << "Snapshot refusal " << static_cast<int>(result.outcome)
                      << ", SQLite code " << result.databaseCode << ": "
                      << result.diagnostic << '\n';
        }
        require(result.outcome == DatabaseSnapshotOutcome::Success && result.snapshot,
                "A live WAL source did not produce a complete snapshot.");
        require(!result.usedQuiescentCopy,
                "A live writer must use the online snapshot path.");
        require(gate.empty() || coordinated,
                "The test did not overlap a writer commit with an active source read.");
        auto* database = result.snapshot->database();
        const auto count = scalar(database, "SELECT count(*) FROM tracks");
        const auto epoch = scalar(database, "SELECT epoch FROM library_state");
        const auto expected = scalar(database, "SELECT expected_count FROM library_state");
        const auto minimum = scalar(database, "SELECT min(generation) FROM tracks");
        const auto maximum = scalar(database, "SELECT max(generation) FROM tracks");
        require(count == expected && minimum == epoch && maximum == epoch,
                "The snapshot mixed records from different committed generations.");
        const auto fingerprint = result.snapshot->fingerprint();
        const auto privateDatabase = result.snapshot->path();
        result.snapshot.reset();
        require(!std::filesystem::exists(privateDatabase) &&
                        std::filesystem::is_empty(request.temporaryRoot),
                "The cross-process probe leaked a private database.");
#ifdef _WIN32
        const auto processId = _getpid();
#else
        const auto processId = getpid();
#endif
        std::cout << "{\"event\":\"result\",\"count\":" << count
                  << ",\"importer_pid\":" << processId
                  << ",\"epoch\":" << epoch
                  << ",\"expected_count\":" << expected
                  << ",\"minimum_generation\":" << minimum
                  << ",\"maximum_generation\":" << maximum
                  << ",\"coordinated\":" << (coordinated ? "true" : "false")
                  << ",\"used_quiescent_copy\":false,\"cleanup\":true,\"fingerprint\":\""
                  << fingerprint << "\",\"sqlite_version\":\"" << sqlite3_libversion() << "\"}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
// End of code prepared by ChatGPT for human review and submission.
