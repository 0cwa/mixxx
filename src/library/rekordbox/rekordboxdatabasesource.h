// Prepared by ChatGPT for human review.
#pragma once

#include "library/rekordbox/rekordboxdatabaserecords.h"
#include "library/rekordbox/rekordboxdatabasesnapshot.h"

#if defined(_WIN32) && defined(REKORDBOX_DATABASE_BUILD_SHARED)
#define REKORDBOX_DATABASE_API __declspec(dllexport)
#elif defined(_WIN32) && defined(REKORDBOX_DATABASE_USE_SHARED)
#define REKORDBOX_DATABASE_API __declspec(dllimport)
#elif defined(__GNUC__)
#define REKORDBOX_DATABASE_API __attribute__((visibility("default")))
#else
#define REKORDBOX_DATABASE_API
#endif

namespace mixxx::rekordbox {

enum class DatabaseSourceFormat {
    OneLibrary,
    Native6,
};

struct DatabaseSourceReadRequest {
    DatabaseSourceFormat format = DatabaseSourceFormat::OneLibrary;
    DatabaseSnapshotRequest snapshot;
    DatabaseReadContext context;
};

// The application-facing boundary carries immutable records, never a SQLite
// handle. Build it in the isolated shared module so SQLCipher cannot replace
// the system SQLite used by Qt and Mixxx's direct SQLite API calls. GUI source
// selection/publication, generation checks and canonical application remain A's
// responsibility. B retains ANLZ decoding and annotation reconciliation.
REKORDBOX_DATABASE_API DatabaseReadResult readDatabaseSource(
        const DatabaseSourceReadRequest& request);

} // namespace mixxx::rekordbox
#undef REKORDBOX_DATABASE_API
// End of code prepared by ChatGPT for human review and submission.
