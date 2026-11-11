// Prepared by ChatGPT for human review.
#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QSet>
#include <functional>
#include <memory>

#include "library/rekordbox/rekordboximportregistry.h"
#include "library/rekordbox/rekordboxxmlparser.h"

namespace mixxx::rekordbox {

// Facts probed from media without adding a Track to the canonical collection.
struct XmlMediaFacts {
    QString location;
    int sampleRate = 0;
    int channels = 0;
    double duration = 0.0;
    int bitrate = 0;
    qint64 fileSize = 0;
    QString fileType;
    // Actual successful metadata probe's UTC source synchronization timestamp.
    // This is the observed file timestamp, never a fabricated import time.
    QDateTime sourceSynchronizedAt;
};

// Hold an exclusive GlobalTrackCacheLocker through planning and commit. The
// callback must refuse already cached targets by ID and location. Release the
// lease before notifying TrackDAO/models after a successful commit.
struct XmlCanonicalAccess {
    std::shared_ptr<const void> lease;
    std::function<bool(qint64, const QString&, QString*)> check;
    bool externalCollectionsAbsent = false;
};

struct XmlAdmissionResult {
    ImportOutcome outcome = ImportOutcome::Failed;
    QVector<ImportIssue> issues;
};

// No canonical mutation: useful when the external model has only a sidecar.
XmlAdmissionResult validateXmlTrackAnnotations(const QByteArray& annotations);
XmlAdmissionResult validateXmlTrackImport(const Library& library, int trackIndex);

// Read-only fast path for unchanged, already applied input. Local canonical
// edits do not force reapplication. New diagnostics/profile/policy/source input
// invalidate this match. This read-only path may return an already cached Track
// without acquiring an import lease or changing any canonical fields.
std::optional<qint64> currentXmlTrackRevision(const ImportSourceRegistry& registry,
        const Library& library,
        int trackIndex,
        const QString& sourceTrackId,
        const ImportTicket& ticket,
        const QString& fingerprint,
        const ImportPolicy& policy,
        QString* error = nullptr);

struct XmlCanonicalTarget {
    qint64 trackId = 0;
    qint64 locationId = 0;
    bool createTrack = false;
    bool createLocation = false;
    XmlMediaFacts media;
    std::optional<qint64> previousSourceSynchronizedMs;
};

// Shared by XML and ANLZ normalization: reserve/recheck media identity without
// a canonical write, then put this admission field first in a TracksOnly plan.
XmlAdmissionResult prepareCanonicalTarget(const QSqlDatabase& database,
        const XmlMediaFacts& media,
        const XmlCanonicalAccess& access,
        XmlCanonicalTarget* target);
ImportField canonicalMediaAdmission(const XmlCanonicalTarget& target,
        const QString& sourceTrackId,
        const QJsonObject& provenance = {});

// Shared metadata payloads for the canonical adapter. Key text is preserved
// verbatim. Artwork requires a local absolute reference; empty means an explicit
// clear. Timestamp conversion reports date-only or sub-second precision loss.
QJsonObject canonicalKeyValue(const QString& text);
std::optional<QJsonObject> canonicalArtworkValue(const QString& reference,
        QVector<ImportIssue>* issues);
std::optional<QJsonValue> canonicalTimestampValue(const QString& text,
        const QString& annotation,
        QVector<ImportIssue>* issues,
        bool allowEmpty = true);

struct XmlPreparedTrack {
    ImportOutcome outcome = ImportOutcome::Failed;
    QVector<ImportIssue> issues;
    std::optional<ImportSnapshot> snapshot;
    XmlCanonicalTarget target;
};

// Resolves existing canonical rows, or reserves new IDs without writing them.
// XML source identity is supplied by the source catalog, never inferred from
// canonical TrackId. This snapshot is TracksOnly and cannot stamp a whole source.
XmlPreparedTrack prepareXmlTrackImport(const QSqlDatabase& database,
        const std::shared_ptr<const Library>& library,
        int trackIndex,
        const QString& sourceTrackId,
        const ImportTicket& ticket,
        const QString& fingerprint,
        const XmlMediaFacts& media,
        const XmlCanonicalAccess& access);

class XmlCanonicalAdapter final : public ImportCanonicalAdapter {
  public:
    XmlCanonicalAdapter(XmlCanonicalTarget target, XmlCanonicalAccess access);
    bool read(const QSqlDatabase& database,
            qint64 canonicalTrackId,
            const QString& targetField,
            QJsonValue* value,
            QString* error) const override;
    bool write(const QSqlDatabase& database,
            qint64 canonicalTrackId,
            const QString& targetField,
            const QJsonValue& value,
            QString* error) override;

  private:
    bool check(const QSqlDatabase& database, qint64 trackId, bool* exists, QString* error) const;
    XmlCanonicalTarget m_target;
    XmlCanonicalAccess m_access;
};

struct XmlTrackImportResult {
    ImportApplyResult application;
    qint64 canonicalTrackId = 0;
    bool createdTrack = false;
};

// GUI-thread, same-connection facade. Callers publish only when committed=true.
XmlTrackImportResult importXmlTrack(ImportSourceRegistry* registry,
        const std::shared_ptr<const Library>& library,
        int trackIndex,
        const QString& sourceTrackId,
        const ImportTicket& ticket,
        const QString& fingerprint,
        const XmlMediaFacts& media,
        const ImportPolicy& policy,
        const XmlCanonicalAccess& access,
        const std::function<bool()>& cancelled = {});

} // namespace mixxx::rekordbox
// End of code prepared by ChatGPT for human review and submission.
