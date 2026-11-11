// Prepared by ChatGPT for human review.
#pragma once

#include "library/rekordbox/rekordboxanlzdecoder.h"
#include "library/rekordbox/rekordboxsource.h"
#include "library/rekordbox/rekordboxxmlimporter.h"

namespace mixxx::rekordbox {

struct AnlzFiles {
    std::shared_ptr<const DecodedAnlz> dat;
    std::shared_ptr<const DecodedAnlz> ext;
    QString datHash;
    QString extHash;
    QString fingerprint;
    QVector<ImportIssue> issues;
};

struct AnlzFilesResult {
    ImportOutcome outcome = ImportOutcome::Failed;
    std::shared_ptr<const AnlzFiles> snapshot;
    QVector<ImportIssue> issues;
};

// Reads the required DAT and optional sibling EXT without modifying either.
// A present but undecodable file invalidates the complete pair.
AnlzFilesResult readAnlzFiles(const QString& baseDatPath,
        const std::function<bool()>& cancelled = {});

// Read-only repeat-load path. No cache lease or canonical writes are needed.
std::optional<qint64> currentAnlzTrackRevision(const ImportSourceRegistry& registry,
        const SourceSnapshot& source,
        int trackIndex,
        const AnlzFiles& files,
        const ImportPolicy& policy,
        qint64 correctionMilliseconds = 0,
        QString* error = nullptr);

struct AnlzPreparedTrack {
    ImportOutcome outcome = ImportOutcome::Failed;
    QVector<ImportIssue> issues;
    std::optional<ImportSnapshot> snapshot;
    XmlCanonicalTarget target;
};

// Read-only preview. Pass snapshot to registry.plan with XmlCanonicalAdapter.
// Retain the same guard object and sealed plan through consent, but release the
// actual cache mutex before running a dialog. Reacquire it before apply; the
// guard's check callback must refuse access while unlocked. Registry apply
// rechecks expected canonical values, ownership, source generation and policy.
AnlzPreparedTrack prepareAnlzTrackImport(const QSqlDatabase& database,
        const std::shared_ptr<const SourceSnapshot>& source,
        int trackIndex,
        const std::shared_ptr<const AnlzFiles>& files,
        const XmlMediaFacts& media,
        const XmlCanonicalAccess& access,
        qint64 correctionMilliseconds = 0,
        const std::function<bool()>& cancelled = {});

// The caller holds the same exclusive cache lease used by XML import. A positive
// correction is subtracted exactly once, after source record reconciliation.
// The result publishes neither Track objects nor signals. Notify models only
// after application.committed and after releasing the lease.
XmlTrackImportResult importAnlzTrack(ImportSourceRegistry* registry,
        const std::shared_ptr<const SourceSnapshot>& source,
        int trackIndex,
        const std::shared_ptr<const AnlzFiles>& files,
        const XmlMediaFacts& media,
        const ImportPolicy& policy,
        const XmlCanonicalAccess& access,
        qint64 correctionMilliseconds = 0,
        const std::function<bool()>& cancelled = {});

} // namespace mixxx::rekordbox
// End of code prepared by ChatGPT for human review and submission.
