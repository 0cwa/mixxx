// Prepared by ChatGPT for human review.
#include "library/rekordbox/rekordboxdatabasesource.h"

#include "library/rekordbox/rekordboxnativereader.h"
#ifndef REKORDBOX_NO_ONELIBRARY
#include "library/rekordbox/rekordboxonelibraryreader.h"
#endif

#include <utility>

namespace mixxx::rekordbox {
namespace {

struct SourceAccess {
    std::shared_ptr<const DatabaseSnapshot> snapshot;
    std::shared_ptr<const void> callerLease;
};

DatabaseReadResult refused(DatabaseOutcome outcome,
        const std::string& code,
        const std::string& message) {
    return {outcome, {{code, message, {}}}, {}};
}

DatabaseOutcome readOutcome(DatabaseSnapshotOutcome outcome) {
    switch (outcome) {
    case DatabaseSnapshotOutcome::Success:
        return DatabaseOutcome::Success;
    case DatabaseSnapshotOutcome::Busy:
    case DatabaseSnapshotOutcome::Unavailable:
    case DatabaseSnapshotOutcome::KeyUnavailable:
        return DatabaseOutcome::Unavailable;
    case DatabaseSnapshotOutcome::Cancelled:
        return DatabaseOutcome::Cancelled;
    case DatabaseSnapshotOutcome::Denied:
        return DatabaseOutcome::Denied;
    case DatabaseSnapshotOutcome::Unsupported:
        return DatabaseOutcome::Unsupported;
    case DatabaseSnapshotOutcome::Invalid:
        return DatabaseOutcome::Corrupt;
    case DatabaseSnapshotOutcome::Failed:
        return DatabaseOutcome::Failed;
    }
    return DatabaseOutcome::Failed;
}

} // namespace

DatabaseReadResult readDatabaseSource(const DatabaseSourceReadRequest& request) {
    if (request.context.sourceId.empty()) {
        return refused(DatabaseOutcome::Failed,
                "source-binding",
                "A durable source binding is required before database "
                "reading.");
    }
    if (request.format != DatabaseSourceFormat::OneLibrary &&
            request.format != DatabaseSourceFormat::Native6) {
        return refused(DatabaseOutcome::Unsupported,
                "source-format",
                "This database format has no qualified reader.");
    }
    if ((request.format == DatabaseSourceFormat::OneLibrary &&
                request.snapshot.profile != DatabaseCipherProfile::OneLibrarySqlCipher4 &&
                request.snapshot.profile != DatabaseCipherProfile::PlainSqlite) ||
            (request.format == DatabaseSourceFormat::Native6 &&
                    request.snapshot.profile != DatabaseCipherProfile::Native6SqlCipher4 &&
                    request.snapshot.profile != DatabaseCipherProfile::PlainSqlite)) {
        return refused(DatabaseOutcome::Unsupported,
                "source-cipher-profile",
                "The source format and selected cipher profile do not match.");
    }
#ifdef REKORDBOX_NO_ONELIBRARY
    if (request.format == DatabaseSourceFormat::OneLibrary) {
        return refused(DatabaseOutcome::Unsupported,
                "onelibrary-disabled",
                "OneLibrary support is unavailable in this build.");
    }
#endif
    try {
        auto snapshotRequest = request.snapshot;
        const auto cancelled = request.context.cancelled;
        const auto priorCancellation = snapshotRequest.cancelled;
        snapshotRequest.cancelled = [cancelled, priorCancellation] {
            return (cancelled && cancelled->load(std::memory_order_relaxed)) ||
                    (priorCancellation && priorCancellation());
        };
        const auto captured = snapshotDatabase(snapshotRequest);
        if (!captured.snapshot) {
            return refused(readOutcome(captured.outcome),
                    captured.outcome == DatabaseSnapshotOutcome::KeyUnavailable
                            ? "snapshot-key-unavailable"
                            : "snapshot-refused",
                    captured.diagnostic);
        }
        auto context = request.context;
        context.fingerprint = captured.snapshot->fingerprint();
        context.originalDatabasePath = std::filesystem::absolute(request.snapshot.sourcePath);
        context.accessLease = std::make_shared<const SourceAccess>(
                SourceAccess{captured.snapshot, request.context.accessLease});
        DatabaseReadResult result;
        if (request.format == DatabaseSourceFormat::Native6) {
            result = readNativeLibrary(captured.snapshot->database(), context);
        } else {
#ifndef REKORDBOX_NO_ONELIBRARY
            result = readOneLibrary(captured.snapshot->path(),
                    "",
                    captured.snapshot->database(),
                    context);
#endif
        }
        if (context.isCancelled() || snapshotRequest.cancelled()) {
            return refused(DatabaseOutcome::Cancelled,
                    "source-cancelled",
                    "Database reading was cancelled before records were "
                    "published.");
        }
        return result;
    } catch (...) {
        return refused(DatabaseOutcome::Failed,
                "source-exception",
                "Database reading failed without publishing partial records.");
    }
}

} // namespace mixxx::rekordbox
// End of code prepared by ChatGPT for human review and submission.
