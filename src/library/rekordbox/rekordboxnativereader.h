// Prepared by ChatGPT for human review.
#pragma once

#include "library/rekordbox/rekordboxdatabaserecords.h"

struct sqlite3;

namespace mixxx::rekordbox {

/// Reads the independently inspected native DBVersion 6000 schema. The caller
/// supplies a private, already unlocked, stable SQLite snapshot; this reader
/// never opens the user's database or selects a cipher/key profile.
/// assetRoot is the original library directory, containing the share directory.
/// Empty assetRoot defaults to originalDatabasePath.parent_path().
/// sourceId is an explicit stable binding, separate from djmdProperty.DBID.
/// This parser does not establish rekordbox 7 or canonical import support.
DatabaseReadResult readNativeLibrary(
        sqlite3* privateDatabase, const DatabaseReadContext& context);

} // namespace mixxx::rekordbox
// End of code prepared by ChatGPT for human review and submission.
