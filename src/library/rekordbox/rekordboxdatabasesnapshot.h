// Prepared by ChatGPT for human review.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct sqlite3;

namespace mixxx::rekordbox {

enum class DatabaseCipherProfile {
    PlainSqlite,
    OneLibrarySqlCipher4,
    Native6SqlCipher4,
};

enum class DatabaseSnapshotOutcome {
    Success,
    Busy,
    Cancelled,
    Denied,
    Unavailable,
    KeyUnavailable,
    Unsupported,
    Invalid,
    Failed,
};

struct DatabaseSnapshotRequest {
    std::filesystem::path sourcePath;
    std::filesystem::path temporaryRoot;
    DatabaseCipherProfile profile = DatabaseCipherProfile::PlainSqlite;
    std::shared_ptr<const void> accessLease;
    std::function<bool()> cancelled;
    std::function<std::vector<unsigned char>(DatabaseCipherProfile)> keyProvider;
    std::chrono::milliseconds timeout{30000};
    std::uint64_t maximumBytes = 2ULL * 1024 * 1024 * 1024;
    int pagesPerStep = 128;
};

// A completed snapshot owns a private plaintext database and the source access
// lease. Keep this owner alive through metadata and ANLZ/media access. A GUI
// owner must supply the lease; this service never requests platform permission.
class DatabaseSnapshot final {
  public:
    ~DatabaseSnapshot();
    DatabaseSnapshot(const DatabaseSnapshot&) = delete;
    DatabaseSnapshot& operator=(const DatabaseSnapshot&) = delete;

    const std::filesystem::path& path() const;
    const std::string& fingerprint() const;
    sqlite3* database() const;

  private:
    friend struct DatabaseSnapshotFactory;
    DatabaseSnapshot(std::filesystem::path directory,
            std::filesystem::path path,
            std::string fingerprint,
            sqlite3* database,
            std::shared_ptr<const void> accessLease);
    std::filesystem::path m_directory;
    std::filesystem::path m_path;
    std::string m_fingerprint;
    sqlite3* m_database = nullptr;
    std::shared_ptr<const void> m_accessLease;
};

struct DatabaseSnapshotResult {
    DatabaseSnapshotOutcome outcome = DatabaseSnapshotOutcome::Failed;
    std::string diagnostic;
    int databaseCode = 0;
    std::shared_ptr<const DatabaseSnapshot> snapshot = {};
    bool usedQuiescentCopy = false;
};

// Uses the qualified SQLite/SQLCipher built-in unix or win32 VFS. Source files
// are opened without create/write/delete rights, including WAL and SHM. Never
// falls back to immutable=1, an unlocked copy, source checkpointing or recovery.
// If WAL sidecars cannot be read, a raw copy is allowed only while an exclusive
// SQLite main-file lock proves there are no other database users. That fallback
// needs a writable lock handle but never writes through it; read-only sources
// that cannot be captured safely produce a retryable refusal. The cipher
// profiles have separate key-provider calls even where their settings agree.
DatabaseSnapshotResult snapshotDatabase(const DatabaseSnapshotRequest& request);

} // namespace mixxx::rekordbox
// End of code prepared by ChatGPT for human review and submission.
