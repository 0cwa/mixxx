// Prepared by ChatGPT for human review.
#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>
#include <memory>
#include <optional>

namespace mixxx::rekordbox {

enum class ImportOutcome {
    Success,
    Warning,
    Unsupported,
    Invalid,
    Denied,
    Unavailable,
    Cancelled,
    Failed,
};

struct ImportIssue {
    QString code;
    QString message;
    QString sourceTrackId;
    QString annotationId;
};

struct ImportTicket {
    QString sourceId;
    qint64 generation = 0;
};

struct ImportSourceState {
    QString sourceId;
    QString kind;
    QString locator;
    QString displayName;
    qint64 requestedGeneration = 0;
    qint64 appliedGeneration = 0;
    QString appliedFingerprint;
    QString appliedPolicy;
    QString appliedSnapshotKey;
    QVector<ImportIssue> losses;
    qint64 registryRevision = 0;
};

struct ImportTrackMapping {
    QString sourceTrackId;
    QString canonicalMediaId;
    qint64 canonicalTrackId = 0;
};

struct ImportPlaylist {
    QString playlistId;
    std::optional<QString> parentId;
    QString name;
    bool folder = false;
    QVector<QString> orderedSourceTrackIds;
};

struct ImportField {
    QString sourceTrackId;
    QString annotationId;
    QString targetField;
    QJsonValue value = QJsonValue::Undefined;
    bool remove = false;
    QJsonObject provenance;
};

enum class ImportScope { FullSource,
    TracksOnly };

struct ImportSnapshot {
    ImportTicket ticket;
    QString fingerprint;
    QString decoderRevision;
    QString formatVersion;
    QStringList capabilities;
    ImportOutcome decodeOutcome = ImportOutcome::Success;
    QVector<ImportIssue> decodeIssues;
    QVector<ImportTrackMapping> tracks;
    QVector<ImportPlaylist> playlists;
    QVector<ImportField> fields;
    std::shared_ptr<const void> accessLease;
    ImportScope scope = ImportScope::FullSource;
    QString inputKey;
};

struct ImportPolicy {
    QString revision;
    bool replaceEditedSourceOwned = false;
    bool acceptPreservedConflicts = false;
};

class ImportCanonicalAdapter {
  public:
    virtual ~ImportCanonicalAdapter() = default;
    virtual bool read(const QSqlDatabase& database,
            qint64 canonicalTrackId,
            const QString& targetField,
            QJsonValue* value,
            QString* error) const = 0;
    virtual bool write(const QSqlDatabase& database,
            qint64 canonicalTrackId,
            const QString& targetField,
            const QJsonValue& value,
            QString* error) = 0;
};

enum class ImportActionKind { Set,
    Remove,
    Unchanged,
    Observe,
    PreserveConflict };

struct ImportAction {
    ImportField field;
    qint64 canonicalTrackId = 0;
    ImportActionKind kind = ImportActionKind::Observe;
    QJsonValue expectedCurrent = QJsonValue::Undefined;
    QJsonValue lastApplied = QJsonValue::Undefined;
    bool sourceOwned = false;
    QJsonValue expectedOwner = QJsonValue::Undefined;
};

struct ImportPlan {
    ImportSnapshot snapshot;
    ImportPolicy policy;
    QString policyKey;
    QString snapshotKey;
    qint64 previousAppliedGeneration = 0;
    qint64 previousRegistryRevision = 0;
    bool unchangedRevision = false;
    QVector<ImportAction> actions;
    QVector<ImportIssue> losses;

  private:
    QString m_integrityKey;
    friend class ImportSourceRegistry;
};

struct ImportPlanResult {
    ImportOutcome outcome = ImportOutcome::Failed;
    std::shared_ptr<const ImportPlan> plan;
    QVector<ImportIssue> issues;
};

struct ImportApplyResult {
    ImportOutcome outcome = ImportOutcome::Failed;
    bool committed = false;
    int canonicalWrites = 0;
    QVector<ImportIssue> issues;
};

class ImportSourceRegistry final {
  private:
    struct ConstructorTag {};

  public:
    explicit ImportSourceRegistry(ConstructorTag);
    static std::unique_ptr<ImportSourceRegistry> open(
            const QString& databasePath, QString* error);
    // The caller retains connection ownership and must not have an active transaction.
    static std::unique_ptr<ImportSourceRegistry> attach(
            const QSqlDatabase& database, QString* error);
    ~ImportSourceRegistry();
    ImportSourceRegistry(const ImportSourceRegistry&) = delete;
    ImportSourceRegistry& operator=(const ImportSourceRegistry&) = delete;

    std::optional<QString> createSource(const QString& kind,
            const QString& locator,
            const QString& displayName,
            QString* error);
    std::optional<ImportSourceState> source(const QString& sourceId, QString* error) const;
    bool relocate(const QString& sourceId, const QString& locator, QString* error);
    std::optional<ImportTicket> requestGeneration(const QString& sourceId, QString* error);
    bool removeSource(const QString& sourceId, QString* error);

    ImportPlanResult plan(const ImportSnapshot& snapshot,
            const ImportPolicy& policy,
            const ImportCanonicalAdapter& adapter) const;
    ImportApplyResult apply(const ImportPlan& plan,
            ImportCanonicalAdapter* adapter,
            const std::function<bool()>& isCancelled = {});

    const QSqlDatabase& database() const;

  private:
    bool initialize(QString* error);
    explicit ImportSourceRegistry(const QSqlDatabase& database);
    QString m_connectionName;
    QSqlDatabase m_database;
};

} // namespace mixxx::rekordbox
// End of code prepared by ChatGPT for human review and submission.
