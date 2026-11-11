// Prepared by ChatGPT for human review.
#include "library/rekordbox/rekordboxdatabasesnapshot.h"

#include <openssl/evp.h>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace mixxx::rekordbox {
namespace {
namespace fs = std::filesystem;

constexpr char kReadVfsName[] = "mixxx-rekordbox-source-readonly-v1";

struct ReadFile {
    sqlite3_file file;
    sqlite3_file* underlying;
};

sqlite3_file* underlying(sqlite3_file* file) {
    return reinterpret_cast<ReadFile*>(file)->underlying;
}

int readClose(sqlite3_file* file) {
    auto* actual = underlying(file);
    const int rc = actual->pMethods->xClose(actual);
    sqlite3_free(actual);
    return rc;
}

int readRead(sqlite3_file* file, void* data, int bytes, sqlite3_int64 offset) {
    auto* actual = underlying(file);
    return actual->pMethods->xRead(actual, data, bytes, offset);
}

int refuseWrite(sqlite3_file*, const void*, int, sqlite3_int64) {
    return SQLITE_READONLY;
}

int refuseTruncate(sqlite3_file*, sqlite3_int64) {
    return SQLITE_READONLY;
}

int refuseSync(sqlite3_file*, int) {
    return SQLITE_READONLY;
}

int readSize(sqlite3_file* file, sqlite3_int64* size) {
    auto* actual = underlying(file);
    return actual->pMethods->xFileSize(actual, size);
}

int readLock(sqlite3_file* file, int lock) {
    auto* actual = underlying(file);
    return actual->pMethods->xLock(actual, lock);
}

int readUnlock(sqlite3_file* file, int lock) {
    auto* actual = underlying(file);
    return actual->pMethods->xUnlock(actual, lock);
}

int readReserved(sqlite3_file* file, int* reserved) {
    auto* actual = underlying(file);
    return actual->pMethods->xCheckReservedLock(actual, reserved);
}

int readControl(sqlite3_file* file, int operation, void* argument) {
    if (operation == SQLITE_FCNTL_SIZE_HINT ||
            operation == SQLITE_FCNTL_CHUNK_SIZE ||
            operation == SQLITE_FCNTL_PERSIST_WAL) {
        return SQLITE_NOTFOUND;
    }
    auto* actual = underlying(file);
    return actual->pMethods->xFileControl(actual, operation, argument);
}

int readSector(sqlite3_file* file) {
    auto* actual = underlying(file);
    return actual->pMethods->xSectorSize(actual);
}

int readCharacteristics(sqlite3_file* file) {
    auto* actual = underlying(file);
    return actual->pMethods->xDeviceCharacteristics(actual);
}

int readShmMap(sqlite3_file* file,
        int region,
        int size,
        int,
        void volatile** memory) {
    auto* actual = underlying(file);
    if (actual->pMethods->iVersion < 2 || !actual->pMethods->xShmMap) {
        *memory = nullptr;
        return SQLITE_IOERR_SHMMAP;
    }
    // The URI prevents SHM creation. Disallow growth and mark even a mapping
    // reused from another connection as read-only so SQLite writes no marks.
    const int rc = actual->pMethods->xShmMap(actual, region, size, 0, memory);
    return rc == SQLITE_OK ? SQLITE_READONLY : rc;
}

int readShmLock(sqlite3_file* file, int offset, int count, int flags) {
    auto* actual = underlying(file);
    return actual->pMethods->xShmLock(actual, offset, count, flags);
}

void readShmBarrier(sqlite3_file* file) {
    auto* actual = underlying(file);
    actual->pMethods->xShmBarrier(actual);
}

int readShmUnmap(sqlite3_file* file, int) {
    auto* actual = underlying(file);
    return actual->pMethods->xShmUnmap(actual, 0);
}

int readFetch(sqlite3_file*, sqlite3_int64, int, void** memory) {
    *memory = nullptr;
    return SQLITE_OK;
}

int readUnfetch(sqlite3_file*, sqlite3_int64, void*) {
    return SQLITE_OK;
}

const sqlite3_io_methods kReadMethods{3,
        readClose,
        readRead,
        refuseWrite,
        refuseTruncate,
        refuseSync,
        readSize,
        readLock,
        readUnlock,
        readReserved,
        readControl,
        readSector,
        readCharacteristics,
        readShmMap,
        readShmLock,
        readShmBarrier,
        readShmUnmap,
        readFetch,
        readUnfetch};

int readOpen(sqlite3_vfs* vfs,
        const char* name,
        sqlite3_file* file,
        int flags,
        int* outputFlags) {
    if (!name || !(flags & (SQLITE_OPEN_MAIN_DB | SQLITE_OPEN_WAL | SQLITE_OPEN_MAIN_JOURNAL))) {
        return SQLITE_CANTOPEN;
    }
    if ((flags & SQLITE_OPEN_MAIN_DB) &&
            !sqlite3_uri_boolean(name, "readonly_shm", 0)) {
        return SQLITE_CANTOPEN;
    }
    auto* base = static_cast<sqlite3_vfs*>(vfs->pAppData);
    auto* wrapper = reinterpret_cast<ReadFile*>(file);
    wrapper->underlying = static_cast<sqlite3_file*>(sqlite3_malloc(base->szOsFile));
    if (!wrapper->underlying) {
        return SQLITE_NOMEM;
    }
    std::memset(wrapper->underlying, 0, static_cast<std::size_t>(base->szOsFile));
    flags &= ~(SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_DELETEONCLOSE);
    flags |= SQLITE_OPEN_READONLY;
    const int rc = base->xOpen(base, name, wrapper->underlying, flags, outputFlags);
    if (rc != SQLITE_OK) {
        if (wrapper->underlying->pMethods) {
            wrapper->underlying->pMethods->xClose(wrapper->underlying);
        }
        sqlite3_free(wrapper->underlying);
        wrapper->underlying = nullptr;
        return rc;
    }
    if (outputFlags) {
        *outputFlags = flags;
    }
    file->pMethods = &kReadMethods;
    return SQLITE_OK;
}

int refuseDelete(sqlite3_vfs*, const char*, int) {
    return SQLITE_READONLY;
}

struct QualifiedVfs {
    sqlite3_vfs wrapper{};
    const char* baseName = nullptr;
    bool ready = false;

    QualifiedVfs() {
        // A silently interposed SQLite may lack the qualified SHM behavior.
        if (std::strcmp(sqlite3_sourceid(), SQLITE_SOURCE_ID) != 0 ||
                sqlite3_libversion_number() < 3022000) {
            return;
        }
#ifdef _WIN32
        baseName = "win32";
#else
        baseName = "unix";
#endif
        auto* base = sqlite3_vfs_find(baseName);
        if (!base || base->iVersion < 2) {
            return;
        }
        wrapper = *base;
        wrapper.zName = kReadVfsName;
        wrapper.pNext = nullptr;
        wrapper.szOsFile = sizeof(ReadFile);
        wrapper.pAppData = base;
        wrapper.xOpen = readOpen;
        wrapper.xDelete = refuseDelete;
        ready = sqlite3_vfs_register(&wrapper, 0) == SQLITE_OK;
    }
};

const QualifiedVfs& qualifiedVfs() {
    static const QualifiedVfs vfs;
    return vfs;
}

std::string utf8Path(const fs::path& path) {
    const auto value = path.generic_u8string();
    return {value.begin(), value.end()};
}

std::string fileUri(const fs::path& path) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result = "file:";
    for (unsigned char value : utf8Path(path)) {
        if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
                (value >= '0' && value <= '9') || value == '/' ||
                value == ':' || value == '-' || value == '_' || value == '.') {
            result += static_cast<char>(value);
        } else {
            result += '%';
            result += hex[value >> 4];
            result += hex[value & 15];
        }
    }
    return result;
}

struct Connection {
    sqlite3* value = nullptr;
    ~Connection() {
        sqlite3_close_v2(value);
    }
};

struct Statement {
    sqlite3_stmt* value = nullptr;
    ~Statement() {
        sqlite3_finalize(value);
    }
};

struct PrivateDirectory {
    fs::path path;
    ~PrivateDirectory() {
        if (!path.empty()) {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    }
};

struct Secret {
    std::vector<unsigned char> bytes;
    ~Secret() {
        volatile unsigned char* data = bytes.data();
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            data[i] = 0;
        }
    }
};

struct Operation {
    const DatabaseSnapshotRequest& request;
    std::chrono::steady_clock::time_point deadline;

    bool cancelled() const {
        try {
            return request.cancelled && request.cancelled();
        } catch (...) {
            return true;
        }
    }
    bool expired() const {
        return std::chrono::steady_clock::now() >= deadline;
    }
    bool stop() const {
        return cancelled() || expired();
    }
};

int busy(void* context, int) {
    auto* operation = static_cast<Operation*>(context);
    if (operation->stop()) {
        return 0;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return !operation->stop();
}

int progress(void* context) {
    return static_cast<Operation*>(context)->stop();
}

int execute(sqlite3* database, const char* sql) {
    return sqlite3_exec(database, sql, nullptr, nullptr, nullptr);
}

int integer(sqlite3* database, const char* sql, sqlite3_int64* value) {
    Statement statement;
    int rc = sqlite3_prepare_v2(database, sql, -1, &statement.value, nullptr);
    if (rc != SQLITE_OK) {
        return rc;
    }
    rc = sqlite3_step(statement.value);
    if (rc == SQLITE_ROW) {
        *value = sqlite3_column_int64(statement.value, 0);
        return SQLITE_OK;
    }
    return rc;
}

DatabaseSnapshotResult failure(int code, const Operation& operation) {
    DatabaseSnapshotResult result;
    result.databaseCode = code;
    if (operation.cancelled()) {
        result.outcome = DatabaseSnapshotOutcome::Cancelled;
        result.diagnostic = "Database snapshot was cancelled.";
    } else if (operation.expired() || (code & 255) == SQLITE_BUSY ||
            (code & 255) == SQLITE_LOCKED || (code & 255) == SQLITE_READONLY) {
        result.outcome = DatabaseSnapshotOutcome::Busy;
        result.diagnostic =
                "A non-mutating snapshot is not available. Retry after the "
                "library has finished saving; do not remove its WAL or SHM "
                "files.";
    } else if ((code & 255) == SQLITE_CANTOPEN || (code & 255) == SQLITE_IOERR) {
        result.outcome = DatabaseSnapshotOutcome::Unavailable;
        result.diagnostic =
                "The database or required journal is unavailable. Reconnect "
                "the source and retry with access to its complete directory.";
    } else if ((code & 255) == SQLITE_PERM || (code & 255) == SQLITE_AUTH) {
        result.outcome = DatabaseSnapshotOutcome::Denied;
        result.diagnostic = "Database access was denied.";
    } else if ((code & 255) == SQLITE_NOTADB || (code & 255) == SQLITE_CORRUPT ||
            (code & 255) == SQLITE_ERROR) {
        result.outcome = DatabaseSnapshotOutcome::Invalid;
        result.diagnostic =
                "The database could not be read with the selected profile. "
                "Verify its key, cipher profile and integrity.";
    } else {
        result.outcome = DatabaseSnapshotOutcome::Failed;
        result.diagnostic = "The private database snapshot could not be completed.";
    }
    return result;
}

bool isInside(const fs::path& path, const fs::path& parent) {
    const auto relative = path.lexically_relative(parent);
    return !relative.empty() && *relative.begin() != "..";
}

fs::path makePrivateDirectory(const fs::path& root) {
    std::random_device random;
    for (int attempt = 0; attempt < 10; ++attempt) {
        std::ostringstream name;
        name << "rekordbox-snapshot-" << std::hex;
        for (int i = 0; i < 4; ++i) {
            name << std::setw(8) << std::setfill('0') << random();
        }
        const auto candidate = root / name.str();
        if (fs::create_directory(candidate)) {
            std::error_code error;
            fs::permissions(candidate, fs::perms::owner_all, fs::perm_options::replace, error);
            if (error) {
                fs::remove(candidate);
                throw fs::filesystem_error("Private directory permissions", candidate, error);
            }
            return candidate;
        }
    }
    throw std::runtime_error("Private directory creation failed");
}

std::string fingerprint(const fs::path& path, const Operation& operation) {
    std::ifstream input(path, std::ios::binary);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
            EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!input || !context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        return {};
    }
    std::array<char, 64 * 1024> data;
    while (input && !operation.stop()) {
        input.read(data.data(), data.size());
        if (input.gcount() && EVP_DigestUpdate(context.get(), data.data(), input.gcount()) != 1) {
            return {};
        }
    }
    if (!input.eof() || operation.stop()) {
        return {};
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest;
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &length) != 1) {
        return {};
    }
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < length; ++i) {
        result << std::setw(2) << static_cast<unsigned int>(digest[i]);
    }
    return result.str();
}

int backupPlain(sqlite3* source, const fs::path& target, Operation* operation) {
    Connection destination;
    int rc = sqlite3_open_v2(utf8Path(target).c_str(),
            &destination.value,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
            qualifiedVfs().baseName);
    if (rc != SQLITE_OK) {
        return rc;
    }
    sqlite3_busy_handler(destination.value, busy, operation);
    auto* backup = sqlite3_backup_init(destination.value, "main", source, "main");
    if (!backup) {
        return sqlite3_extended_errcode(destination.value);
    }
    do {
        if (operation->stop()) {
            rc = SQLITE_INTERRUPT;
            break;
        }
        rc = sqlite3_backup_step(backup, operation->request.pagesPerStep);
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    } while (rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED);
    const int finished = sqlite3_backup_finish(backup);
    if (rc != SQLITE_DONE) {
        return rc;
    }
    if (finished != SQLITE_OK) {
        return finished;
    }
    return execute(destination.value, "PRAGMA journal_mode=DELETE");
}

int exportCipher(sqlite3* source, const fs::path& target, std::uint64_t maximumBytes) {
    // SQLCipher deliberately rejects its backup API for encrypted databases.
    // Export is performed inside the already established source read transaction.
    Statement attach;
    int rc = sqlite3_prepare_v2(source,
            "ATTACH DATABASE ?1 AS mixxx_snapshot KEY ''",
            -1,
            &attach.value,
            nullptr);
    const auto uri = fileUri(target) + "?mode=rwc&vfs=" + qualifiedVfs().baseName;
    if (rc == SQLITE_OK) {
        rc = sqlite3_bind_text(attach.value, 1, uri.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(attach.value);
    }
    if (rc != SQLITE_DONE) {
        return rc;
    }
    sqlite3_int64 pageSize = 0;
    rc = integer(source, "PRAGMA mixxx_snapshot.page_size", &pageSize);
    if (rc != SQLITE_OK || pageSize < 512) {
        return rc == SQLITE_OK ? SQLITE_CORRUPT : rc;
    }
    const std::string pageLimit = "PRAGMA mixxx_snapshot.max_page_count=" +
            std::to_string(std::max<std::uint64_t>(1, maximumBytes / pageSize));
    rc = execute(source, pageLimit.c_str());
    if (rc != SQLITE_OK) {
        return rc;
    }
    rc = execute(source, "SELECT sqlcipher_export('mixxx_snapshot')");
    for (const auto* pragma : {"user_version", "application_id"}) {
        sqlite3_int64 value = 0;
        const std::string read = std::string("PRAGMA main.") + pragma;
        if (rc == SQLITE_OK) {
            rc = integer(source, read.c_str(), &value);
        }
        if (rc == SQLITE_OK) {
            const std::string write = std::string("PRAGMA mixxx_snapshot.") +
                    pragma + "=" + std::to_string(value);
            rc = execute(source, write.c_str());
        }
    }
    return rc;
}

} // namespace

struct DatabaseSnapshotFactory {
    static std::shared_ptr<const DatabaseSnapshot> create(fs::path directory,
            fs::path path,
            std::string hash,
            sqlite3* database,
            std::shared_ptr<const void> accessLease) {
        return std::shared_ptr<const DatabaseSnapshot>(
                new DatabaseSnapshot(std::move(directory),
                        std::move(path),
                        std::move(hash),
                        database,
                        std::move(accessLease)));
    }
};

DatabaseSnapshot::DatabaseSnapshot(fs::path directory,
        fs::path path,
        std::string hash,
        sqlite3* database,
        std::shared_ptr<const void> accessLease)
        : m_directory(std::move(directory)),
          m_path(std::move(path)),
          m_fingerprint(std::move(hash)),
          m_database(database),
          m_accessLease(std::move(accessLease)) {
}

DatabaseSnapshot::~DatabaseSnapshot() {
    sqlite3_close_v2(m_database);
    std::error_code ignored;
    fs::remove_all(m_directory, ignored);
}

const fs::path& DatabaseSnapshot::path() const {
    return m_path;
}

const std::string& DatabaseSnapshot::fingerprint() const {
    return m_fingerprint;
}

sqlite3* DatabaseSnapshot::database() const {
    return m_database;
}

static DatabaseSnapshotResult snapshotOnline(const DatabaseSnapshotRequest& request) {
    Operation operation{request, std::chrono::steady_clock::now() + request.timeout};
    if (operation.stop()) {
        return failure(SQLITE_INTERRUPT, operation);
    }
    if (!qualifiedVfs().ready) {
        return {DatabaseSnapshotOutcome::Unsupported,
                "This SQLite runtime has not been qualified for non-mutating snapshots."};
    }
    if (request.timeout <= std::chrono::milliseconds::zero() ||
            request.pagesPerStep < 1 || request.maximumBytes < 512) {
        return {DatabaseSnapshotOutcome::Invalid, "Invalid database snapshot limits."};
    }
    try {
        std::error_code error;
        if (!fs::is_regular_file(request.sourcePath, error)) {
            return {error == std::errc::permission_denied ? DatabaseSnapshotOutcome::Denied
                                                          : DatabaseSnapshotOutcome::Unavailable,
                    "Select an accessible library database file."};
        }
        const auto sourcePath = fs::canonical(request.sourcePath);
        const auto temporaryRoot = fs::canonical(request.temporaryRoot);
        if (!fs::is_directory(temporaryRoot) || isInside(temporaryRoot, sourcePath.parent_path())) {
            return {DatabaseSnapshotOutcome::Invalid,
                    "Snapshot storage must be outside the source library directory."};
        }
        if (fs::file_size(sourcePath) > request.maximumBytes) {
            return {DatabaseSnapshotOutcome::Unsupported,
                    "The database exceeds the configured snapshot size limit."};
        }
        const bool encrypted = request.profile != DatabaseCipherProfile::PlainSqlite;
        Secret key;
        if (encrypted) {
#ifdef SQLITE_HAS_CODEC
            if (request.keyProvider) {
                key.bytes = request.keyProvider(request.profile);
            }
            if (key.bytes.empty() ||
                    key.bytes.size() >
                            static_cast<std::size_t>(
                                    std::numeric_limits<int>::max())) {
                return {DatabaseSnapshotOutcome::KeyUnavailable,
                        "No key is available for the selected database cipher profile."};
            }
#else
            return {DatabaseSnapshotOutcome::Unsupported,
                    "SQLCipher support is unavailable in this build."};
#endif
        }
        PrivateDirectory directory{makePrivateDirectory(temporaryRoot)};
        const auto target = directory.path / "library.sqlite";
        Connection source;
        const auto uri = fileUri(sourcePath) + "?mode=ro&readonly_shm=1";
        int rc = sqlite3_open_v2(uri.c_str(),
                &source.value,
                SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI |
                        SQLITE_OPEN_NOMUTEX,
                kReadVfsName);
        if (rc != SQLITE_OK) {
            const int systemError = source.value ? sqlite3_system_errno(source.value) : 0;
            if (systemError == EACCES || systemError == EPERM) {
                return {DatabaseSnapshotOutcome::Denied, "Reading the source database was denied."};
            }
            return failure(rc, operation);
        }
        sqlite3_extended_result_codes(source.value, 1);
        sqlite3_busy_handler(source.value, busy, &operation);
        sqlite3_progress_handler(source.value, 1000, progress, &operation);
        sqlite3_limit(source.value,
                SQLITE_LIMIT_LENGTH,
                static_cast<int>(std::min<std::uint64_t>(
                        request.maximumBytes, 64 * 1024 * 1024)));
        sqlite3_db_config(source.value, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
        rc = execute(source.value, "PRAGMA temp_store=MEMORY; PRAGMA trusted_schema=OFF");
#ifdef SQLITE_HAS_CODEC
        if (rc == SQLITE_OK && encrypted) {
            rc = sqlite3_key(source.value, key.bytes.data(), static_cast<int>(key.bytes.size()));
            if (rc == SQLITE_OK) {
                switch (request.profile) {
                case DatabaseCipherProfile::OneLibrarySqlCipher4:
                    rc = execute(source.value, "PRAGMA cipher_compatibility=4");
                    break;
                case DatabaseCipherProfile::Native6SqlCipher4:
                    rc = execute(source.value, "PRAGMA cipher_compatibility=4");
                    break;
                default:
                    rc = SQLITE_MISUSE;
                }
            }
        }
#endif
        if (rc == SQLITE_OK) {
            rc = execute(source.value, "BEGIN; SELECT count(*) FROM sqlite_schema");
        }
        sqlite3_int64 pages = 0;
        sqlite3_int64 pageSize = 0;
        if (rc == SQLITE_OK) {
            rc = integer(source.value, "PRAGMA page_count", &pages);
        }
        if (rc == SQLITE_OK) {
            rc = integer(source.value, "PRAGMA page_size", &pageSize);
        }
        if (rc != SQLITE_OK) {
            return failure(rc, operation);
        }
        if (pageSize < 512 || pages < 0 ||
                static_cast<std::uint64_t>(pages) > request.maximumBytes /
                                static_cast<std::uint64_t>(pageSize)) {
            return {DatabaseSnapshotOutcome::Unsupported,
                    "The committed database exceeds the configured snapshot "
                    "size limit."};
        }
        if (encrypted) {
            rc = exportCipher(source.value, target, request.maximumBytes);
        } else {
            rc = backupPlain(source.value, target, &operation);
        }
        int moved = 0;
        const int movedResult = sqlite3_file_control(
                source.value, "main", SQLITE_FCNTL_HAS_MOVED, &moved);
        if (moved || (movedResult != SQLITE_OK && movedResult != SQLITE_NOTFOUND)) {
            return {DatabaseSnapshotOutcome::Unavailable,
                    "The source was replaced or removed during the snapshot."};
        }
        if (rc == SQLITE_OK && !operation.stop()) {
            rc = execute(source.value, "COMMIT");
        }
        if (rc != SQLITE_OK || operation.stop()) {
            return failure(rc == SQLITE_OK ? SQLITE_INTERRUPT : rc, operation);
        }
        // Closing only the protected source handle never checkpoints or removes
        // source sidecars. The attached destination, if any, is private.
        rc = sqlite3_close(source.value);
        if (rc != SQLITE_OK) {
            return failure(rc, operation);
        }
        source.value = nullptr;
        if (!fs::exists(sourcePath)) {
            return {DatabaseSnapshotOutcome::Unavailable,
                    "The source was removed during the snapshot."};
        }
        if (fs::file_size(target) > request.maximumBytes) {
            return {DatabaseSnapshotOutcome::Unsupported,
                    "The snapshot exceeds the configured size limit."};
        }
        const auto hash = fingerprint(target, operation);
        if (hash.empty()) {
            return failure(SQLITE_IOERR, operation);
        }
        Connection completed;
        const auto completedUri = fileUri(target) + "?mode=ro&readonly_shm=1";
        rc = sqlite3_open_v2(completedUri.c_str(),
                &completed.value,
                SQLITE_OPEN_READONLY | SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX,
                kReadVfsName);
        if (rc == SQLITE_OK) {
            rc = execute(completed.value, "PRAGMA query_only=ON; PRAGMA trusted_schema=OFF");
        }
        if (rc != SQLITE_OK) {
            return failure(rc, operation);
        }
        auto snapshot = DatabaseSnapshotFactory::create(directory.path,
                target,
                hash,
                completed.value,
                request.accessLease);
        completed.value = nullptr;
        directory.path.clear();
        return {DatabaseSnapshotOutcome::Success, {}, SQLITE_OK, std::move(snapshot)};
    } catch (const fs::filesystem_error& error) {
        if (error.code() == std::errc::permission_denied) {
            return {DatabaseSnapshotOutcome::Denied,
                    "Access to the database or private snapshot storage was "
                    "denied."};
        }
        return failure(SQLITE_IOERR, operation);
    } catch (...) {
        return failure(SQLITE_ERROR, operation);
    }
}

namespace {

struct QuiescentFile {
    sqlite3_file* file = nullptr;
    sqlite3_filename filename = nullptr;
    ~QuiescentFile() {
        if (file) {
            if (file->pMethods) {
                file->pMethods->xUnlock(file, SQLITE_LOCK_NONE);
                file->pMethods->xClose(file);
            }
            sqlite3_free(file);
        }
        sqlite3_free_filename(filename);
    }
};

int copyLockedSource(const fs::path& sourcePath,
        const fs::path& target,
        Operation* operation) {
    auto* vfs = sqlite3_vfs_find(qualifiedVfs().baseName);
    QuiescentFile source;
    source.file = static_cast<sqlite3_file*>(sqlite3_malloc(vfs->szOsFile));
    if (!source.file) {
        return SQLITE_NOMEM;
    }
    std::memset(source.file, 0, static_cast<std::size_t>(vfs->szOsFile));
    const auto path = utf8Path(sourcePath);
    const auto journal = path + "-journal";
    const auto wal = path + "-wal";
    source.filename = sqlite3_create_filename(
            path.c_str(), journal.c_str(), wal.c_str(), 0, nullptr);
    if (!source.filename) {
        return SQLITE_NOMEM;
    }
    int flags = 0;
    int rc = vfs->xOpen(vfs,
            source.filename,
            source.file,
            SQLITE_OPEN_MAIN_DB | SQLITE_OPEN_READWRITE,
            &flags);
    if (rc != SQLITE_OK) {
        return rc;
    }
    int moved = 0;
    rc = source.file->pMethods->xFileControl(source.file, SQLITE_FCNTL_HAS_MOVED, &moved);
    if ((rc != SQLITE_OK && rc != SQLITE_NOTFOUND) || moved) {
        return SQLITE_IOERR;
    }
    if (!(flags & SQLITE_OPEN_READWRITE)) {
        return SQLITE_READONLY;
    }
    // WAL connections hold SHARED on the main file for their whole lifetime.
    // EXCLUSIVE therefore excludes WAL writers/checkpointers as well as rollback
    // writers. Only xRead/xFileSize/xLock/xUnlock/xClose are called on this handle.
    do {
        if (operation->stop()) {
            return SQLITE_INTERRUPT;
        }
        rc = source.file->pMethods->xLock(source.file, SQLITE_LOCK_SHARED);
        if (rc == SQLITE_OK) {
            rc = source.file->pMethods->xLock(source.file, SQLITE_LOCK_EXCLUSIVE);
        }
        if (rc == SQLITE_BUSY) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    } while (rc == SQLITE_BUSY);
    if (rc != SQLITE_OK) {
        return rc;
    }
    sqlite3_int64 size = 0;
    rc = source.file->pMethods->xFileSize(source.file, &size);
    if (rc != SQLITE_OK || size < 0 ||
            static_cast<std::uint64_t>(size) > operation->request.maximumBytes) {
        return rc == SQLITE_OK ? SQLITE_TOOBIG : rc;
    }
    std::ofstream output(target, std::ios::binary | std::ios::trunc);
    if (!output) {
        return SQLITE_IOERR_WRITE;
    }
    std::array<char, 64 * 1024> buffer;
    for (sqlite3_int64 offset = 0; offset < size;) {
        if (operation->stop()) {
            return SQLITE_INTERRUPT;
        }
        const int amount = static_cast<int>(std::min<sqlite3_int64>(buffer.size(), size - offset));
        rc = source.file->pMethods->xRead(source.file, buffer.data(), amount, offset);
        if (rc != SQLITE_OK) {
            return rc;
        }
        output.write(buffer.data(), amount);
        if (!output) {
            return SQLITE_IOERR_WRITE;
        }
        offset += amount;
    }
    output.close();
    if (!output) {
        return SQLITE_IOERR_WRITE;
    }
    // SHM is an index, rebuilt only in private storage. WAL carries committed
    // data; a hot rollback journal, if present, must travel with the main file.
    for (const auto* suffix : {"-wal", "-journal"}) {
        auto companion = sourcePath;
        companion += suffix;
        if (fs::exists(companion)) {
            if (fs::is_symlink(companion) || !fs::is_regular_file(companion)) {
                return SQLITE_IOERR;
            }
            const auto companionSize = fs::file_size(companion);
            const auto modified = fs::last_write_time(companion);
            if (companionSize > operation->request.maximumBytes) {
                return SQLITE_TOOBIG;
            }
            auto copiedCompanion = target;
            copiedCompanion += suffix;
            std::ifstream input(companion, std::ios::binary);
            std::ofstream copied(copiedCompanion, std::ios::binary | std::ios::trunc);
            if (!input || !copied) {
                return SQLITE_IOERR;
            }
            while (input && !operation->stop()) {
                input.read(buffer.data(), buffer.size());
                copied.write(buffer.data(), input.gcount());
                if (!copied) {
                    return SQLITE_IOERR_WRITE;
                }
            }
            copied.close();
            if (operation->stop()) {
                return SQLITE_INTERRUPT;
            }
            if (!input.eof() || !copied || !fs::exists(companion) ||
                    fs::is_symlink(companion) || fs::file_size(companion) != companionSize ||
                    fs::last_write_time(companion) != modified ||
                    fs::file_size(copiedCompanion) != companionSize) {
                return SQLITE_IOERR;
            }
        }
    }
    moved = 0;
    rc = source.file->pMethods->xFileControl(source.file, SQLITE_FCNTL_HAS_MOVED, &moved);
    if ((rc != SQLITE_OK && rc != SQLITE_NOTFOUND) || moved) {
        return SQLITE_IOERR;
    }
    return operation->stop() ? SQLITE_INTERRUPT : SQLITE_OK;
}

DatabaseSnapshotResult snapshotQuiescent(const DatabaseSnapshotRequest& request) {
    Operation operation{request, std::chrono::steady_clock::now() + request.timeout};
    try {
        const auto sourcePath = fs::canonical(request.sourcePath);
        const auto temporaryRoot = fs::canonical(request.temporaryRoot);
        if (!fs::is_regular_file(sourcePath) ||
                fs::file_size(sourcePath) > request.maximumBytes ||
                isInside(temporaryRoot, sourcePath.parent_path())) {
            return {DatabaseSnapshotOutcome::Invalid,
                    "The source or private storage changed before quiescent capture."};
        }
        PrivateDirectory directory{makePrivateDirectory(temporaryRoot)};
        const auto captured = directory.path / "captured.db";
        int rc = copyLockedSource(sourcePath, captured, &operation);
        if (rc != SQLITE_OK) {
            return failure(rc, operation);
        }
        Connection recovery;
        rc = sqlite3_open_v2(utf8Path(captured).c_str(),
                &recovery.value,
                SQLITE_OPEN_READWRITE,
                qualifiedVfs().baseName);
        if (rc == SQLITE_OK) {
            sqlite3_progress_handler(recovery.value, 1000, progress, &operation);
            sqlite3_busy_handler(recovery.value, busy, &operation);
            rc = execute(recovery.value, "PRAGMA temp_store=MEMORY; PRAGMA trusted_schema=OFF");
        }
#ifdef SQLITE_HAS_CODEC
        Secret key;
        if (rc == SQLITE_OK && request.profile != DatabaseCipherProfile::PlainSqlite) {
            if (request.keyProvider) {
                key.bytes = request.keyProvider(request.profile);
            }
            if (key.bytes.empty() ||
                    key.bytes.size() >
                            static_cast<std::size_t>(
                                    std::numeric_limits<int>::max())) {
                return {DatabaseSnapshotOutcome::KeyUnavailable,
                        "No key is available for the selected database cipher profile."};
            }
            rc = sqlite3_key(recovery.value, key.bytes.data(), static_cast<int>(key.bytes.size()));
            if (rc == SQLITE_OK) {
                rc = execute(recovery.value, "PRAGMA cipher_compatibility=4");
            }
        }
#endif
        if (rc == SQLITE_OK) {
            rc = execute(recovery.value,
                    "SELECT count(*) FROM sqlite_schema; PRAGMA "
                    "journal_mode=DELETE");
        }
        if (rc != SQLITE_OK) {
            return failure(rc, operation);
        }
        rc = sqlite3_close(recovery.value);
        if (rc != SQLITE_OK) {
            return failure(rc, operation);
        }
        recovery.value = nullptr;
        if (!fs::exists(sourcePath)) {
            return {DatabaseSnapshotOutcome::Unavailable,
                    "The source was removed during the snapshot."};
        }
        auto privateRequest = request;
        privateRequest.sourcePath = captured;
        privateRequest.timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
                operation.deadline - std::chrono::steady_clock::now());
        auto result = snapshotOnline(privateRequest);
        result.usedQuiescentCopy = result.snapshot != nullptr;
        return result;
    } catch (const fs::filesystem_error&) {
        return failure(SQLITE_IOERR, operation);
    } catch (...) {
        return failure(SQLITE_ERROR, operation);
    }
}

} // namespace

DatabaseSnapshotResult snapshotDatabase(const DatabaseSnapshotRequest& request) {
#ifdef _WIN32
    (void)request;
    return {DatabaseSnapshotOutcome::Unsupported,
            "Windows snapshot storage requires a qualified private-directory ACL implementation."};
#else
    struct stat original{};
    const bool identified = ::stat(request.sourcePath.c_str(), &original) == 0;
#endif
    const auto started = std::chrono::steady_clock::now();
    auto result = snapshotOnline(request);
    const int primaryCode = result.databaseCode & 255;
    const bool mayCapture =
            (result.outcome == DatabaseSnapshotOutcome::Unavailable &&
                    primaryCode == SQLITE_CANTOPEN) ||
            (result.outcome == DatabaseSnapshotOutcome::Busy &&
                    primaryCode == SQLITE_READONLY);
    if (mayCapture) {
        const auto remaining = request.timeout -
                std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started);
        std::error_code error;
        if (remaining > std::chrono::milliseconds::zero() &&
                fs::is_regular_file(request.sourcePath, error)) {
            auto fallback = request;
            fallback.timeout = remaining;
            result = snapshotQuiescent(fallback);
        }
    }
#ifndef _WIN32
    struct stat current{};
    if (identified &&
            (::stat(request.sourcePath.c_str(), &current) != 0 ||
                    current.st_dev != original.st_dev ||
                    current.st_ino != original.st_ino)) {
        return {DatabaseSnapshotOutcome::Unavailable,
                "The source was replaced or removed during the snapshot."};
    }
#endif
    return result;
}

} // namespace mixxx::rekordbox
// End of code prepared by ChatGPT for human review and submission.
