// Prepared by ChatGPT for human review.
#pragma once

#include "library/rekordbox/rekordboxdatabaserecords.h"

struct sqlite3;

namespace mixxx::rekordbox {

enum class DeviceDatabaseChoice {
    RequireExplicit,
    Legacy,
    OneLibrary,
};

struct DeviceDatabaseSelection {
    DatabaseOutcome outcome = DatabaseOutcome::Unavailable;
    std::optional<DeviceDatabaseChoice> selected;
    std::vector<DatabaseIssue> issues;
};

DeviceDatabaseSelection selectDeviceDatabase(bool hasLegacy,
        bool hasOneLibrary,
        DeviceDatabaseChoice choice = DeviceDatabaseChoice::RequireExplicit);

// The file and connection must name the same completed private snapshot.
// No source key is retained in a record. An empty private key reads a plaintext
// snapshot; the source format and cipher profile remain the snapshot's concern.
DatabaseReadResult readOneLibrary(const std::filesystem::path& privateDatabasePath,
        const std::string& privatePassphrase,
        sqlite3* privateDatabase,
        const DatabaseReadContext& context);

} // namespace mixxx::rekordbox
// End of code prepared by ChatGPT for human review and submission.
