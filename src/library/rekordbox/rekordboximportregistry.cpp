// Prepared by ChatGPT for human review.
#include "library/rekordbox/rekordboximportregistry.h"

#include <QCryptographicHash>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>
#include <exception>
#include <utility>

namespace mixxx::rekordbox {
namespace {

void setError(QString* error, const QString& message) {
    if (error) {
        *error = message;
    }
}

bool execute(const QSqlDatabase& db,
        const QString& sql,
        const QVariantList& values,
        QString* error) {
    QSqlQuery query(db);
    if (!query.prepare(sql)) {
        setError(error, query.lastError().text());
        return false;
    }
    for (const auto& value : values) {
        query.addBindValue(value);
    }
    if (!query.exec()) {
        setError(error, query.lastError().text());
        return false;
    }
    return true;
}

bool executeOne(const QSqlDatabase& db,
        const QString& sql,
        const QVariantList& values,
        QString* error) {
    QSqlQuery query(db);
    if (!query.prepare(sql)) {
        setError(error, query.lastError().text());
        return false;
    }
    for (const auto& value : values) {
        query.addBindValue(value);
    }
    if (!query.exec() || query.numRowsAffected() != 1) {
        setError(error,
                query.lastError().isValid()
                        ? query.lastError().text()
                        : QStringLiteral("Expected one current source row"));
        return false;
    }
    return true;
}

QString encode(const QJsonValue& value) {
    QJsonArray envelope;
    if (!value.isUndefined()) {
        envelope.append(value);
    }
    return QString::fromUtf8(QJsonDocument(envelope).toJson(QJsonDocument::Compact));
}

bool decode(const QString& text, QJsonValue* value, QString* error) {
    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(text.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isArray() ||
            doc.array().size() > 1) {
        setError(error, QStringLiteral("Invalid persisted value envelope"));
        return false;
    }
    *value = doc.array().isEmpty() ? QJsonValue(QJsonValue::Undefined) : doc.array().first();
    return true;
}

QJsonObject policyObject(const ImportPolicy& policy) {
    return {{QStringLiteral("revision"), policy.revision},
            {QStringLiteral("replace_edited_source_owned"), policy.replaceEditedSourceOwned},
            {QStringLiteral("accept_preserved_conflicts"), policy.acceptPreservedConflicts}};
}

QString policyKey(const ImportPolicy& policy) {
    return QString::fromLatin1(QCryptographicHash::hash(
            QJsonDocument(policyObject(policy)).toJson(QJsonDocument::Compact),
            QCryptographicHash::Sha256)
                    .toHex());
}

QString encodeIssues(const QVector<ImportIssue>& issues) {
    QJsonArray array;
    for (const auto& issue : issues) {
        array.append(QJsonObject{{QStringLiteral("code"), issue.code},
                {QStringLiteral("message"), issue.message},
                {QStringLiteral("track"), issue.sourceTrackId},
                {QStringLiteral("annotation"), issue.annotationId}});
    }
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

QVector<ImportIssue> decodeIssues(const QString& text) {
    QVector<ImportIssue> issues;
    for (const auto& item : QJsonDocument::fromJson(text.toUtf8()).array()) {
        const auto object = item.toObject();
        issues.push_back({object.value(QStringLiteral("code")).toString(),
                object.value(QStringLiteral("message")).toString(),
                object.value(QStringLiteral("track")).toString(),
                object.value(QStringLiteral("annotation")).toString()});
    }
    return issues;
}

QString hashObject(const QJsonObject& object) {
    return QString::fromLatin1(QCryptographicHash::hash(
            QJsonDocument(object).toJson(QJsonDocument::Compact),
            QCryptographicHash::Sha256)
                    .toHex());
}

QJsonObject fieldObject(const ImportField& field) {
    return {{QStringLiteral("track"), field.sourceTrackId},
            {QStringLiteral("annotation"), field.annotationId},
            {QStringLiteral("target"), field.targetField},
            {QStringLiteral("value"), encode(field.value)},
            {QStringLiteral("remove"), field.remove},
            {QStringLiteral("provenance"), field.provenance}};
}

QString snapshotKey(const ImportSnapshot& snapshot) {
    QJsonArray tracks;
    for (const auto& track : snapshot.tracks) {
        tracks.append(QJsonArray{track.sourceTrackId,
                track.canonicalMediaId,
                QString::number(track.canonicalTrackId)});
    }
    QJsonArray playlists;
    for (const auto& playlist : snapshot.playlists) {
        QJsonArray members;
        for (const auto& member : playlist.orderedSourceTrackIds) {
            members.append(member);
        }
        playlists.append(QJsonObject{
                {QStringLiteral("id"), playlist.playlistId},
                {QStringLiteral("parent"),
                        playlist.parentId ? QJsonValue(*playlist.parentId)
                                          : QJsonValue(QJsonValue::Null)},
                {QStringLiteral("name"), playlist.name},
                {QStringLiteral("folder"), playlist.folder},
                {QStringLiteral("members"), members}});
    }
    QJsonArray fields;
    for (const auto& field : snapshot.fields) {
        fields.append(fieldObject(field));
    }
    auto capabilities = snapshot.capabilities;
    capabilities.sort();
    capabilities.removeDuplicates();
    return hashObject({{QStringLiteral("source"), snapshot.ticket.sourceId},
            {QStringLiteral("scope"), static_cast<int>(snapshot.scope)},
            {QStringLiteral("input_key"), snapshot.inputKey},
            {QStringLiteral("fingerprint"), snapshot.fingerprint},
            {QStringLiteral("decoder"), snapshot.decoderRevision},
            {QStringLiteral("format"), snapshot.formatVersion},
            {QStringLiteral("capabilities"), QJsonArray::fromStringList(capabilities)},
            {QStringLiteral("outcome"), static_cast<int>(snapshot.decodeOutcome)},
            {QStringLiteral("issues"), encodeIssues(snapshot.decodeIssues)},
            {QStringLiteral("tracks"), tracks},
            {QStringLiteral("playlists"), playlists},
            {QStringLiteral("fields"), fields}});
}

QString planIntegrityKey(const ImportPlan& plan) {
    QJsonArray actions;
    for (const auto& action : plan.actions) {
        actions.append(QJsonObject{{QStringLiteral("field"), fieldObject(action.field)},
                {QStringLiteral("track"), QString::number(action.canonicalTrackId)},
                {QStringLiteral("kind"), static_cast<int>(action.kind)},
                {QStringLiteral("current"), encode(action.expectedCurrent)},
                {QStringLiteral("applied"), encode(action.lastApplied)},
                {QStringLiteral("owned"), action.sourceOwned},
                {QStringLiteral("owner"), encode(action.expectedOwner)}});
    }
    return hashObject({{QStringLiteral("snapshot"), snapshotKey(plan.snapshot)},
            {QStringLiteral("snapshot_key"), plan.snapshotKey},
            {QStringLiteral("policy"), policyObject(plan.policy)},
            {QStringLiteral("policy_key"), plan.policyKey},
            {QStringLiteral("generation"),
                    QString::number(plan.snapshot.ticket.generation)},
            {QStringLiteral("previous_generation"),
                    QString::number(plan.previousAppliedGeneration)},
            {QStringLiteral("registry_revision"), QString::number(plan.previousRegistryRevision)},
            {QStringLiteral("unchanged"), plan.unchangedRevision},
            {QStringLiteral("actions"), actions},
            {QStringLiteral("losses"), encodeIssues(plan.losses)},
            {QStringLiteral("lease"),
                    QString::number(reinterpret_cast<quintptr>(
                                            plan.snapshot.accessLease.get()),
                            16)}});
}

ImportIssue issue(const QString& code, const QString& message, const ImportField& field = {}) {
    return {code, message, field.sourceTrackId, field.annotationId};
}

bool ownerAt(const QSqlDatabase& database,
        qint64 trackId,
        const QString& targetField,
        QJsonValue* owner,
        QString* error) {
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
            "SELECT source_id,source_track_id,annotation_id,last_applied_json "
            "FROM rekordbox_import_provenance_v1 "
            "WHERE canonical_track_id=? AND target_field=? AND owns=1"));
    query.addBindValue(trackId);
    query.addBindValue(targetField);
    if (!query.exec()) {
        setError(error, query.lastError().text());
        return false;
    }
    *owner = QJsonValue::Undefined;
    if (query.next()) {
        *owner = QJsonArray{query.value(0).toString(),
                query.value(1).toString(),
                query.value(2).toString(),
                query.value(3).toString()};
    }
    return true;
}

bool terminalDecodeFailure(ImportOutcome outcome) {
    return outcome != ImportOutcome::Success && outcome != ImportOutcome::Warning;
}

} // namespace

ImportSourceRegistry::ImportSourceRegistry(ConstructorTag)
        : m_connectionName(QUuid::createUuid().toString(QUuid::WithoutBraces)),
          m_database(QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName)) {
}

std::unique_ptr<ImportSourceRegistry> ImportSourceRegistry::open(
        const QString& databasePath, QString* error) {
    setError(error, {});
    auto registry = std::make_unique<ImportSourceRegistry>(ConstructorTag{});
    registry->m_database.setDatabaseName(databasePath);
    if (!registry->m_database.open()) {
        setError(error, registry->m_database.lastError().text());
        return {};
    }
    if (!registry->initialize(error)) {
        return {};
    }
    return registry;
}

ImportSourceRegistry::ImportSourceRegistry(const QSqlDatabase& database)
        : m_database(database) {
}

std::unique_ptr<ImportSourceRegistry> ImportSourceRegistry::attach(
        const QSqlDatabase& database, QString* error) {
    setError(error, {});
    if (!database.isOpen() || database.driverName() != QLatin1String("QSQLITE")) {
        setError(error, QStringLiteral("An open destination SQLite connection is required"));
        return {};
    }
    auto registry = std::unique_ptr<ImportSourceRegistry>(new ImportSourceRegistry(database));
    if (!registry->initialize(error)) {
        return {};
    }
    return registry;
}

ImportSourceRegistry::~ImportSourceRegistry() {
    if (m_connectionName.isEmpty()) {
        return;
    }
    m_database.close();
    m_database = QSqlDatabase();
    QSqlDatabase::removeDatabase(m_connectionName);
}

bool ImportSourceRegistry::initialize(QString* error) {
    // Do not change the collection connection's foreign-key policy. Registry
    // cleanup is explicit; legacy collection schemas contain historical FKs.
    if (!m_database.transaction()) {
        setError(error, m_database.lastError().text());
        return false;
    }
    const QStringList statements{
            QStringLiteral(
                    "CREATE TABLE IF NOT EXISTS rekordbox_import_sources_v1 ("
                    "source_id TEXT PRIMARY KEY,kind TEXT NOT NULL,locator "
                    "TEXT NOT NULL,"
                    "display_name TEXT NOT NULL,requested_generation INTEGER "
                    "NOT NULL DEFAULT 0,"
                    "applied_generation INTEGER NOT NULL DEFAULT 0,fingerprint "
                    "TEXT,"
                    "snapshot_key TEXT,policy_key TEXT,policy_json "
                    "TEXT,format_version "
                    "TEXT,capabilities_json TEXT,"
                    "losses_json TEXT NOT NULL DEFAULT '[]',registry_revision "
                    "INTEGER NOT NULL DEFAULT 0)"),
            QStringLiteral(
                    "CREATE TABLE IF NOT EXISTS rekordbox_import_tracks_v1 ("
                    "source_id TEXT NOT NULL,source_track_id TEXT NOT NULL,"
                    "canonical_media_id TEXT NOT NULL,canonical_track_id "
                    "INTEGER NOT NULL,"
                    "PRIMARY KEY(source_id,source_track_id),"
                    "FOREIGN KEY(source_id) REFERENCES "
                    "rekordbox_import_sources_v1(source_id) "
                    "ON DELETE CASCADE)"),
            QStringLiteral(
                    "CREATE TABLE IF NOT EXISTS rekordbox_import_playlists_v1 ("
                    "source_id TEXT NOT NULL,playlist_id TEXT NOT "
                    "NULL,parent_id TEXT,"
                    "name TEXT NOT NULL,folder INTEGER NOT NULL,PRIMARY "
                    "KEY(source_id,playlist_id),"
                    "FOREIGN KEY(source_id) REFERENCES "
                    "rekordbox_import_sources_v1(source_id) "
                    "ON DELETE CASCADE,FOREIGN KEY(source_id,parent_id) "
                    "REFERENCES "
                    "rekordbox_import_playlists_v1(source_id,playlist_id) "
                    "DEFERRABLE INITIALLY DEFERRED)"),
            QStringLiteral(
                    "CREATE TABLE IF NOT EXISTS rekordbox_import_members_v1 ("
                    "source_id TEXT NOT NULL,playlist_id TEXT NOT NULL,ordinal "
                    "INTEGER NOT NULL,"
                    "source_track_id TEXT NOT NULL,PRIMARY "
                    "KEY(source_id,playlist_id,ordinal),"
                    "FOREIGN KEY(source_id,playlist_id) REFERENCES "
                    "rekordbox_import_playlists_v1(source_id,playlist_id) ON "
                    "DELETE CASCADE,"
                    "FOREIGN KEY(source_id,source_track_id) REFERENCES "
                    "rekordbox_import_tracks_v1(source_id,source_track_id) ON "
                    "DELETE CASCADE)"),
            QStringLiteral(
                    "CREATE TABLE IF NOT EXISTS rekordbox_import_provenance_v1 "
                    "("
                    "source_id TEXT NOT NULL,source_track_id TEXT NOT NULL,"
                    "annotation_id TEXT NOT NULL,canonical_track_id INTEGER "
                    "NOT NULL,"
                    "target_field TEXT NOT NULL,source_json TEXT NOT NULL,"
                    "last_applied_json TEXT NOT NULL,owns INTEGER NOT "
                    "NULL,details_json TEXT NOT NULL,"
                    "policy_key TEXT NOT NULL,format_version TEXT NOT "
                    "NULL,generation INTEGER NOT NULL,"
                    "PRIMARY KEY(source_id,source_track_id,annotation_id),"
                    "FOREIGN KEY(source_id,source_track_id) REFERENCES "
                    "rekordbox_import_tracks_v1(source_id,source_track_id) ON "
                    "DELETE CASCADE)"),
            QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS "
                           "rekordbox_import_owner_v1 ON "
                           "rekordbox_import_provenance_v1(canonical_track_id,"
                           "target_field) WHERE owns=1"),
            QStringLiteral("CREATE TABLE IF NOT EXISTS "
                           "rekordbox_import_cue_bindings_v1 ("
                           "canonical_track_id INTEGER NOT NULL,target_field "
                           "TEXT NOT NULL,"
                           "cue_id INTEGER NOT NULL,PRIMARY "
                           "KEY(canonical_track_id,target_field))"),
            QStringLiteral("CREATE TABLE IF NOT EXISTS "
                           "rekordbox_import_track_revisions_v1 ("
                           "source_id TEXT NOT NULL,source_track_id TEXT NOT "
                           "NULL,fingerprint TEXT NOT NULL,"
                           "snapshot_key TEXT NOT NULL,policy_json TEXT NOT "
                           "NULL,generation INTEGER NOT NULL,"
                           "losses_json TEXT NOT NULL,input_key TEXT NOT NULL "
                           "DEFAULT '',"
                           "PRIMARY KEY(source_id,source_track_id))")};
    for (const auto& sql : statements) {
        if (!execute(m_database, sql, {}, error)) {
            m_database.rollback();
            return false;
        }
    }
    QSqlQuery columns(m_database);
    if (!columns.exec(QStringLiteral("PRAGMA table_info(rekordbox_import_track_revisions_v1)"))) {
        setError(error, columns.lastError().text());
        m_database.rollback();
        return false;
    }
    bool hasInputKey = false;
    while (columns.next()) {
        hasInputKey |= columns.value(1).toString() == QLatin1String("input_key");
    }
    columns.finish();
    if (!hasInputKey &&
            !execute(m_database,
                    QStringLiteral(
                            "ALTER TABLE rekordbox_import_track_revisions_v1 "
                            "ADD COLUMN input_key TEXT NOT NULL DEFAULT ''"),
                    {},
                    error)) {
        m_database.rollback();
        return false;
    }
    if (!columns.exec(QStringLiteral("PRAGMA table_info(rekordbox_import_sources_v1)"))) {
        setError(error, columns.lastError().text());
        m_database.rollback();
        return false;
    }
    bool hasRegistryRevision = false;
    while (columns.next()) {
        hasRegistryRevision |= columns.value(1).toString() == QLatin1String("registry_revision");
    }
    columns.finish();
    if (!hasRegistryRevision &&
            !execute(m_database,
                    QStringLiteral("ALTER TABLE rekordbox_import_sources_v1 "
                                   "ADD COLUMN registry_revision INTEGER NOT "
                                   "NULL DEFAULT 0"),
                    {},
                    error)) {
        m_database.rollback();
        return false;
    }
    if (!m_database.commit()) {
        setError(error, m_database.lastError().text());
        m_database.rollback();
        return false;
    }
    return true;
}

const QSqlDatabase& ImportSourceRegistry::database() const {
    return m_database;
}

std::optional<QString> ImportSourceRegistry::createSource(const QString& kind,
        const QString& locator,
        const QString& displayName,
        QString* error) {
    setError(error, {});
    if (kind.isEmpty() || locator.isEmpty()) {
        setError(error, QStringLiteral("Source kind and locator are required"));
        return {};
    }
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!execute(m_database,
                QStringLiteral("INSERT INTO "
                               "rekordbox_import_sources_v1(source_id,kind,"
                               "locator,display_name) "
                               "VALUES(?,?,?,?)"),
                {id,
                        kind,
                        locator,
                        displayName.isNull() ? QStringLiteral("")
                                             : displayName},
                error)) {
        return {};
    }
    return id;
}

std::optional<ImportSourceState> ImportSourceRegistry::source(
        const QString& sourceId, QString* error) const {
    setError(error, {});
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
            "SELECT source_id,kind,locator,display_name,requested_generation,"
            "applied_generation,fingerprint,policy_key,snapshot_key,losses_json,registry_revision "
            "FROM rekordbox_import_sources_v1 WHERE source_id=?"));
    query.addBindValue(sourceId);
    if (!query.exec()) {
        setError(error, query.lastError().text());
        return {};
    }
    if (!query.next()) {
        return {};
    }
    return ImportSourceState{query.value(0).toString(),
            query.value(1).toString(),
            query.value(2).toString(),
            query.value(3).toString(),
            query.value(4).toLongLong(),
            query.value(5).toLongLong(),
            query.value(6).toString(),
            query.value(7).toString(),
            query.value(8).toString(),
            decodeIssues(query.value(9).toString()),
            query.value(10).toLongLong()};
}

bool ImportSourceRegistry::relocate(
        const QString& sourceId, const QString& locator, QString* error) {
    setError(error, {});
    if (locator.isEmpty() || !source(sourceId, error)) {
        setError(error, QStringLiteral("Unknown source or empty locator"));
        return false;
    }
    return executeOne(m_database,
            QStringLiteral("UPDATE rekordbox_import_sources_v1 SET locator=? "
                           "WHERE source_id=?"),
            {locator, sourceId},
            error);
}

std::optional<ImportTicket> ImportSourceRegistry::requestGeneration(
        const QString& sourceId, QString* error) {
    setError(error, {});
    if (!m_database.transaction()) {
        setError(error, m_database.lastError().text());
        return {};
    }
    if (!executeOne(m_database,
                QStringLiteral("UPDATE rekordbox_import_sources_v1 SET "
                               "requested_generation=requested_generation+1 "
                               "WHERE source_id=? AND "
                               "requested_generation<9223372036854775807"),
                {sourceId},
                error)) {
        m_database.rollback();
        return {};
    }
    const auto state = source(sourceId, error);
    if (!state || !m_database.commit()) {
        if (error && error->isEmpty()) {
            *error = state ? m_database.lastError().text() : QStringLiteral("Unknown source");
        }
        m_database.rollback();
        return {};
    }
    return ImportTicket{sourceId, state->requestedGeneration};
}

bool ImportSourceRegistry::removeSource(const QString& sourceId, QString* error) {
    setError(error, {});
    if (!m_database.transaction()) {
        setError(error, m_database.lastError().text());
        return false;
    }
    const QStringList tables{QStringLiteral("rekordbox_import_members_v1"),
            QStringLiteral("rekordbox_import_provenance_v1"),
            QStringLiteral("rekordbox_import_track_revisions_v1"),
            QStringLiteral("rekordbox_import_playlists_v1"),
            QStringLiteral("rekordbox_import_tracks_v1")};
    for (const auto& table : tables) {
        if (!execute(m_database,
                    QStringLiteral("DELETE FROM %1 WHERE source_id=?")
                            .arg(table),
                    {sourceId},
                    error)) {
            m_database.rollback();
            return false;
        }
    }
    if (!execute(m_database,
                QStringLiteral("DELETE FROM rekordbox_import_sources_v1 WHERE "
                               "source_id=?"),
                {sourceId},
                error) ||
            !m_database.commit()) {
        if (error && error->isEmpty()) {
            *error = m_database.lastError().text();
        }
        m_database.rollback();
        return false;
    }
    return true;
}

ImportPlanResult ImportSourceRegistry::plan(const ImportSnapshot& snapshot,
        const ImportPolicy& policy,
        const ImportCanonicalAdapter& adapter) const {
    const auto failure = [](ImportOutcome outcome, const QString& code, const QString& text) {
        return ImportPlanResult{outcome, {}, {issue(code, text)}};
    };
    if (snapshot.scope == ImportScope::TracksOnly &&
            (snapshot.tracks.isEmpty() || !snapshot.playlists.isEmpty())) {
        return failure(ImportOutcome::Invalid,
                QStringLiteral("invalid_partial_scope"),
                QStringLiteral("Track-scoped application needs tracks and "
                               "cannot replace playlists"));
    }
    if (terminalDecodeFailure(snapshot.decodeOutcome)) {
        return {snapshot.decodeOutcome, {}, snapshot.decodeIssues};
    }
    if (snapshot.decodeOutcome == ImportOutcome::Warning && snapshot.decodeIssues.isEmpty()) {
        return failure(ImportOutcome::Invalid,
                QStringLiteral("missing_warning_details"),
                QStringLiteral("A warning snapshot must describe its loss or limitation"));
    }
    if (snapshot.fingerprint.isEmpty() || snapshot.decoderRevision.isEmpty() ||
            policy.revision.isEmpty()) {
        return failure(ImportOutcome::Invalid,
                QStringLiteral("missing_revision"),
                QStringLiteral(
                        "Fingerprint, decoder revision and versioned policy are required"));
    }
    QString error;
    const auto state = source(snapshot.ticket.sourceId, &error);
    if (!state) {
        return failure(error.isEmpty() ? ImportOutcome::Unavailable
                                       : ImportOutcome::Failed,
                QStringLiteral("source_unavailable"),
                error.isEmpty()
                        ? QStringLiteral("This source is no longer available. "
                                         "Add or refresh it before retrying.")
                        : error);
    }
    if (snapshot.ticket.generation != state->requestedGeneration ||
            snapshot.ticket.generation <= 0) {
        return failure(ImportOutcome::Cancelled,
                QStringLiteral("stale_generation"),
                QStringLiteral(
                        "A newer request superseded this source snapshot"));
    }
    auto result = std::make_shared<ImportPlan>();
    result->snapshot = snapshot;
    result->policy = policy;
    result->policyKey = policyKey(policy);
    result->snapshotKey = snapshotKey(snapshot);
    result->previousAppliedGeneration = state->appliedGeneration;
    result->previousRegistryRevision = state->registryRevision;
    result->losses = snapshot.decodeIssues;
    QHash<QString, ImportTrackMapping> tracks;
    for (const auto& track : snapshot.tracks) {
        if (track.sourceTrackId.isEmpty() || track.canonicalMediaId.isEmpty() ||
                track.canonicalTrackId <= 0 || tracks.contains(track.sourceTrackId)) {
            return failure(ImportOutcome::Invalid,
                    QStringLiteral("invalid_track_mapping"),
                    QStringLiteral("Source track mappings must be complete and "
                                   "unique"));
        }
        QSqlQuery old(m_database);
        old.prepare(QStringLiteral(
                "SELECT canonical_media_id,canonical_track_id FROM "
                "rekordbox_import_tracks_v1 WHERE source_id=? AND source_track_id=?"));
        old.addBindValue(snapshot.ticket.sourceId);
        old.addBindValue(track.sourceTrackId);
        if (!old.exec()) {
            return failure(ImportOutcome::Failed,
                    QStringLiteral("mapping_read"),
                    old.lastError().text());
        }
        if (old.next() &&
                (old.value(0).toString() != track.canonicalMediaId ||
                        old.value(1).toLongLong() != track.canonicalTrackId)) {
            return failure(ImportOutcome::Unsupported,
                    QStringLiteral("mapping_retarget"),
                    QStringLiteral("Canonical remapping requires a separate "
                                   "reviewed migration"));
        }
        tracks.insert(track.sourceTrackId, track);
    }
    QHash<QString, ImportPlaylist> playlists;
    for (const auto& playlist : snapshot.playlists) {
        if (playlist.playlistId.isEmpty() || playlists.contains(playlist.playlistId) ||
                (playlist.folder && !playlist.orderedSourceTrackIds.isEmpty())) {
            return failure(ImportOutcome::Invalid,
                    QStringLiteral("invalid_playlist"),
                    QStringLiteral("Playlist identity, kind or membership is "
                                   "invalid"));
        }
        for (const auto& track : playlist.orderedSourceTrackIds) {
            if (!tracks.contains(track)) {
                return failure(ImportOutcome::Invalid,
                        QStringLiteral("missing_playlist_track"),
                        QStringLiteral("Playlist membership references an "
                                       "absent source track"));
            }
        }
        playlists.insert(playlist.playlistId, playlist);
    }
    for (const auto& playlist : snapshot.playlists) {
        auto parent = playlist.parentId;
        QSet<QString> visited{playlist.playlistId};
        while (parent) {
            if (visited.contains(*parent) || !playlists.contains(*parent) ||
                    !playlists.value(*parent).folder) {
                return failure(ImportOutcome::Invalid,
                        QStringLiteral("invalid_parent"),
                        QStringLiteral("Playlist parents must form an acyclic "
                                       "folder hierarchy"));
            }
            visited.insert(*parent);
            parent = playlists.value(*parent).parentId;
        }
    }
    QSet<QPair<QString, QString>> annotations;
    QSet<QPair<qint64, QString>> destinations;
    for (const auto& field : snapshot.fields) {
        const auto key = qMakePair(field.sourceTrackId, field.annotationId);
        if (!tracks.contains(field.sourceTrackId) || field.annotationId.isEmpty() ||
                field.targetField.isEmpty() || annotations.contains(key)) {
            return failure(ImportOutcome::Invalid,
                    QStringLiteral("invalid_annotation"),
                    QStringLiteral("Annotation source identity is absent or "
                                   "repeated"));
        }
        annotations.insert(key);
        if (field.value.isUndefined() && !field.remove) {
            continue;
        }
        QJsonValue roundTripped;
        if ((field.remove && !field.value.isUndefined()) ||
                !decode(encode(field.value), &roundTripped, &error) ||
                roundTripped != field.value) {
            return failure(ImportOutcome::Invalid,
                    QStringLiteral("invalid_field_value"),
                    QStringLiteral("Values must be lossless JSON; tombstones "
                                   "cannot also carry a value"));
        }
        const auto trackId = tracks.value(field.sourceTrackId).canonicalTrackId;
        const auto destination = qMakePair(trackId, field.targetField);
        if (destinations.contains(destination)) {
            return failure(ImportOutcome::Invalid,
                    QStringLiteral("duplicate_destination"),
                    QStringLiteral("Multiple annotations target the same "
                                   "canonical field"));
        }
        destinations.insert(destination);
        ImportAction action;
        action.field = field;
        action.canonicalTrackId = trackId;
        QSqlQuery previous(m_database);
        previous.prepare(QStringLiteral(
                "SELECT canonical_track_id,target_field,last_applied_json,owns FROM "
                "rekordbox_import_provenance_v1 WHERE source_id=? AND source_track_id=? "
                "AND annotation_id=?"));
        previous.addBindValue(snapshot.ticket.sourceId);
        previous.addBindValue(field.sourceTrackId);
        previous.addBindValue(field.annotationId);
        if (!previous.exec()) {
            return failure(ImportOutcome::Failed,
                    QStringLiteral("provenance_read"),
                    previous.lastError().text());
        }
        if (previous.next()) {
            if (previous.value(0).toLongLong() != trackId ||
                    previous.value(1).toString() != field.targetField) {
                return failure(ImportOutcome::Unsupported,
                        QStringLiteral("annotation_retarget"),
                        QStringLiteral("Annotation retargeting requires a "
                                       "separate reviewed migration"));
            }
            if (!decode(previous.value(2).toString(), &action.lastApplied, &error)) {
                return failure(ImportOutcome::Failed, QStringLiteral("provenance_corrupt"), error);
            }
            action.sourceOwned = previous.value(3).toBool();
        }
        try {
            if (!adapter.read(m_database,
                        trackId,
                        field.targetField,
                        &action.expectedCurrent,
                        &error) ||
                    !ownerAt(m_database,
                            trackId,
                            field.targetField,
                            &action.expectedOwner,
                            &error)) {
                return failure(ImportOutcome::Failed, QStringLiteral("canonical_read"), error);
            }
        } catch (const std::exception& exception) {
            return failure(ImportOutcome::Failed,
                    QStringLiteral("adapter_exception"),
                    QString::fromUtf8(exception.what()));
        } catch (...) {
            return failure(ImportOutcome::Failed,
                    QStringLiteral("adapter_exception"),
                    QStringLiteral("Unknown planning exception"));
        }
        const auto desired = field.remove ? QJsonValue(QJsonValue::Undefined) : field.value;
        const bool ownedUnedited = action.sourceOwned &&
                action.expectedCurrent == action.lastApplied;
        const bool approvedReplacement = action.sourceOwned && policy.replaceEditedSourceOwned;
        if (ownedUnedited || approvedReplacement) {
            action.kind = desired == action.expectedCurrent ? ImportActionKind::Unchanged
                    : field.remove                          ? ImportActionKind::Remove
                                                            : ImportActionKind::Set;
        } else if (action.sourceOwned) {
            action.kind = ImportActionKind::PreserveConflict;
        } else if (desired == action.expectedCurrent) {
            action.kind = ImportActionKind::Observe;
        } else if (action.expectedCurrent.isUndefined() && action.expectedOwner.isUndefined() &&
                !field.remove) {
            action.kind = ImportActionKind::Set;
        } else {
            action.kind = ImportActionKind::PreserveConflict;
        }
        if (action.kind == ImportActionKind::PreserveConflict) {
            result->losses.push_back(issue(QStringLiteral("preserved_conflict"),
                    QStringLiteral("Local or other-source state was preserved"),
                    field));
        }
        result->actions.push_back(std::move(action));
    }
    if (snapshot.scope == ImportScope::FullSource &&
            state->appliedSnapshotKey == result->snapshotKey &&
            state->appliedPolicy == result->policyKey) {
        result->unchangedRevision = true;
        result->actions.clear();
        result->losses = state->losses;
    }
    result->m_integrityKey = planIntegrityKey(*result);
    return {result->losses.isEmpty() ? ImportOutcome::Success : ImportOutcome::Warning,
            result,
            result->losses};
}

ImportApplyResult ImportSourceRegistry::apply(const ImportPlan& plan,
        ImportCanonicalAdapter* adapter,
        const std::function<bool()>& isCancelled) {
    const auto cancelled = [&isCancelled] { return isCancelled && isCancelled(); };
    const auto cancellationMessage = QStringLiteral(
            "This import was cancelled. No changes from this request were committed.");
    const auto failure = [](ImportOutcome outcome, const QString& code, const QString& text) {
        return ImportApplyResult{outcome, false, 0, {issue(code, text)}};
    };
    if (!adapter || plan.policyKey != policyKey(plan.policy) || plan.m_integrityKey.isEmpty() ||
            plan.m_integrityKey != planIntegrityKey(plan)) {
        return failure(ImportOutcome::Invalid,
                QStringLiteral("invalid_plan"),
                QStringLiteral(
                        "Adapter and an unchanged registry-produced plan are required"));
    }
    if (!plan.losses.isEmpty() && !plan.policy.acceptPreservedConflicts) {
        return {ImportOutcome::Denied, false, 0, plan.losses};
    }
    if (cancelled()) {
        return failure(ImportOutcome::Cancelled, QStringLiteral("cancelled"), cancellationMessage);
    }
    if (!m_database.transaction()) {
        return failure(ImportOutcome::Failed,
                QStringLiteral("begin_transaction"),
                m_database.lastError().text());
    }
    const auto rollback = [&](ImportOutcome outcome, const QString& code, const QString& message) {
        m_database.rollback();
        return failure(outcome, code, message);
    };
    QString error;
    try {
        const auto state = source(plan.snapshot.ticket.sourceId, &error);
        if (!state) {
            return rollback(ImportOutcome::Unavailable,
                    QStringLiteral("source_removed"),
                    error.isEmpty()
                            ? QStringLiteral("This source was removed while "
                                             "the import was pending. Add it "
                                             "again before retrying.")
                            : error);
        }
        if (state->requestedGeneration != plan.snapshot.ticket.generation ||
                state->appliedGeneration != plan.previousAppliedGeneration ||
                state->registryRevision != plan.previousRegistryRevision) {
            return rollback(ImportOutcome::Cancelled,
                    QStringLiteral("stale_generation"),
                    QStringLiteral("The source or import state changed after "
                                   "preview. This request was not committed; "
                                   "retry with the current source."));
        }
        int writes = 0;
        if (!plan.unchangedRevision) {
            for (const auto& action : plan.actions) {
                QJsonValue current;
                QJsonValue owner;
                if (!adapter->read(m_database,
                            action.canonicalTrackId,
                            action.field.targetField,
                            &current,
                            &error) ||
                        !ownerAt(m_database,
                                action.canonicalTrackId,
                                action.field.targetField,
                                &owner,
                                &error)) {
                    return rollback(ImportOutcome::Failed,
                            QStringLiteral("canonical_recheck"),
                            error);
                }
                if (current != action.expectedCurrent || owner != action.expectedOwner) {
                    return rollback(ImportOutcome::Failed,
                            QStringLiteral("plan_changed"),
                            QStringLiteral("Canonical values or ownership "
                                           "changed after preview"));
                }
            }
            for (const auto& mapping : plan.snapshot.tracks) {
                if (!execute(m_database,
                            QStringLiteral(
                                    "INSERT INTO rekordbox_import_tracks_v1 "
                                    "VALUES(?,?,?,?) "
                                    "ON CONFLICT(source_id,source_track_id) DO "
                                    "NOTHING"),
                            {plan.snapshot.ticket.sourceId,
                                    mapping.sourceTrackId,
                                    mapping.canonicalMediaId,
                                    mapping.canonicalTrackId},
                            &error)) {
                    return rollback(ImportOutcome::Failed, QStringLiteral("mapping_write"), error);
                }
            }
            for (const auto& action : plan.actions) {
                if (cancelled()) {
                    return rollback(ImportOutcome::Cancelled,
                            QStringLiteral("cancelled"),
                            cancellationMessage);
                }
                bool owns = action.sourceOwned;
                auto lastApplied = action.lastApplied;
                if (action.kind == ImportActionKind::Set ||
                        action.kind == ImportActionKind::Remove) {
                    const auto value = action.kind == ImportActionKind::Remove
                            ? QJsonValue(QJsonValue::Undefined)
                            : action.field.value;
                    if (!adapter->write(m_database,
                                action.canonicalTrackId,
                                action.field.targetField,
                                value,
                                &error)) {
                        return rollback(ImportOutcome::Failed,
                                QStringLiteral("canonical_write"),
                                error);
                    }
                    ++writes;
                    owns = !value.isUndefined();
                    lastApplied = value;
                } else if (action.kind == ImportActionKind::Unchanged) {
                    owns = !action.field.remove;
                    lastApplied = action.field.remove
                            ? QJsonValue(QJsonValue::Undefined)
                            : action.field.value;
                }
                const QString sql = QStringLiteral(
                        "INSERT INTO rekordbox_import_provenance_v1 "
                        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?) "
                        "ON CONFLICT(source_id,source_track_id,annotation_id) "
                        "DO UPDATE SET "
                        "source_json=excluded.source_json,last_applied_json="
                        "excluded.last_applied_json,"
                        "owns=excluded.owns,details_json=excluded.details_json,"
                        "policy_key=excluded.policy_key,"
                        "format_version=excluded.format_version,generation="
                        "excluded.generation");
                const auto desired = action.field.remove
                        ? QJsonValue(QJsonValue::Undefined)
                        : action.field.value;
                const QString version = plan.snapshot.formatVersion.isNull()
                        ? QStringLiteral("")
                        : plan.snapshot.formatVersion;
                if (!execute(m_database,
                            sql,
                            {plan.snapshot.ticket.sourceId,
                                    action.field.sourceTrackId,
                                    action.field.annotationId,
                                    action.canonicalTrackId,
                                    action.field.targetField,
                                    encode(desired),
                                    encode(lastApplied),
                                    owns,
                                    QString::fromUtf8(QJsonDocument(
                                            action.field.provenance)
                                                    .toJson(QJsonDocument::
                                                                    Compact)),
                                    plan.policyKey,
                                    version,
                                    plan.snapshot.ticket.generation},
                            &error)) {
                    return rollback(ImportOutcome::Failed,
                            QStringLiteral("provenance_write"),
                            error);
                }
            }
            if (plan.snapshot.scope == ImportScope::FullSource) {
                if (!execute(m_database,
                            QStringLiteral(
                                    "DELETE FROM rekordbox_import_members_v1 "
                                    "WHERE source_id=?"),
                            {plan.snapshot.ticket.sourceId},
                            &error) ||
                        !execute(m_database,
                                QStringLiteral("DELETE FROM "
                                               "rekordbox_import_playlists_v1 "
                                               "WHERE source_id=?"),
                                {plan.snapshot.ticket.sourceId},
                                &error)) {
                    return rollback(ImportOutcome::Failed,
                            QStringLiteral("playlist_remove"),
                            error);
                }
                for (const auto& playlist : plan.snapshot.playlists) {
                    if (!execute(m_database,
                                QStringLiteral(
                                        "INSERT INTO rekordbox_import_playlists_v1 "
                                        "VALUES(?,?,?,?,?)"),
                                {plan.snapshot.ticket.sourceId,
                                        playlist.playlistId,
                                        playlist.parentId
                                                ? QVariant(*playlist.parentId)
                                                : QVariant(),
                                        playlist.name.isNull() ? QStringLiteral("")
                                                               : playlist.name,
                                        playlist.folder},
                                &error)) {
                        return rollback(ImportOutcome::Failed,
                                QStringLiteral("playlist_write"),
                                error);
                    }
                    for (qsizetype i = 0; i < playlist.orderedSourceTrackIds.size(); ++i) {
                        if (!execute(m_database,
                                    QStringLiteral("INSERT INTO "
                                                   "rekordbox_import_members_v1 "
                                                   "VALUES(?,?,?,?)"),
                                    {plan.snapshot.ticket.sourceId,
                                            playlist.playlistId,
                                            i,
                                            playlist.orderedSourceTrackIds[i]},
                                    &error)) {
                            return rollback(ImportOutcome::Failed,
                                    QStringLiteral("membership_write"),
                                    error);
                        }
                    }
                }
                QSet<QString> retained;
                for (const auto& mapping : plan.snapshot.tracks) {
                    retained.insert(mapping.sourceTrackId);
                }
                QSqlQuery oldMappings(m_database);
                oldMappings.prepare(QStringLiteral(
                        "SELECT source_track_id FROM rekordbox_import_tracks_v1 "
                        "WHERE source_id=?"));
                oldMappings.addBindValue(plan.snapshot.ticket.sourceId);
                if (!oldMappings.exec()) {
                    return rollback(ImportOutcome::Failed,
                            QStringLiteral("mapping_scan"),
                            oldMappings.lastError().text());
                }
                QStringList removed;
                while (oldMappings.next()) {
                    const auto key = oldMappings.value(0).toString();
                    if (!retained.contains(key)) {
                        removed.push_back(key);
                    }
                }
                oldMappings.finish();
                for (const auto& key : removed) {
                    if (!execute(m_database,
                                QStringLiteral(
                                        "DELETE FROM "
                                        "rekordbox_import_track_revisions_v1 "
                                        "WHERE source_id=? AND "
                                        "source_track_id=?"),
                                {plan.snapshot.ticket.sourceId, key},
                                &error) ||
                            !execute(m_database,
                                    QStringLiteral(
                                            "DELETE FROM "
                                            "rekordbox_import_provenance_v1 "
                                            "WHERE source_id=? AND "
                                            "source_track_id=?"),
                                    {plan.snapshot.ticket.sourceId, key},
                                    &error) ||
                            !execute(m_database,
                                    QStringLiteral("DELETE FROM "
                                                   "rekordbox_import_tracks_v1 "
                                                   "WHERE source_id=? AND "
                                                   "source_track_id=?"),
                                    {plan.snapshot.ticket.sourceId, key},
                                    &error)) {
                        return rollback(ImportOutcome::Failed,
                                QStringLiteral("mapping_remove"),
                                error);
                    }
                }
            }
        }
        if (cancelled()) {
            return rollback(ImportOutcome::Cancelled,
                    QStringLiteral("cancelled"),
                    cancellationMessage);
        }
        if (plan.snapshot.scope == ImportScope::TracksOnly) {
            for (const auto& track : plan.snapshot.tracks) {
                if (!execute(m_database,
                            QStringLiteral(
                                    "INSERT INTO "
                                    "rekordbox_import_track_revisions_v1 "
                                    "(source_id,source_track_id,fingerprint,"
                                    "snapshot_key,policy_json,generation,"
                                    "losses_json,input_key) "
                                    "VALUES(?,?,?,?,?,?,?,?) "
                                    "ON CONFLICT(source_id,source_track_id) DO "
                                    "UPDATE SET "
                                    "fingerprint=excluded.fingerprint,snapshot_"
                                    "key=excluded.snapshot_key,"
                                    "policy_json=excluded.policy_json,"
                                    "generation=excluded.generation,"
                                    "losses_json=excluded.losses_json,input_"
                                    "key=excluded.input_key"),
                            {plan.snapshot.ticket.sourceId,
                                    track.sourceTrackId,
                                    plan.snapshot.fingerprint,
                                    plan.snapshotKey,
                                    QString::fromUtf8(QJsonDocument(
                                            policyObject(plan.policy))
                                                    .toJson(QJsonDocument::
                                                                    Compact)),
                                    plan.snapshot.ticket.generation,
                                    encodeIssues(plan.losses),
                                    plan.snapshot.inputKey.isNull()
                                            ? QStringLiteral("")
                                            : plan.snapshot.inputKey},
                            &error)) {
                    return rollback(ImportOutcome::Failed,
                            QStringLiteral("track_revision_write"),
                            error);
                }
            }
        }
        if (plan.snapshot.scope == ImportScope::FullSource &&
                !executeOne(m_database,
                        QStringLiteral(
                                "UPDATE rekordbox_import_sources_v1 SET "
                                "applied_generation=?,fingerprint=?,snapshot_"
                                "key=?,"
                                "policy_key=?,policy_json=?,format_version=?,"
                                "capabilities_json=?,losses_json=? "
                                "WHERE source_id=? AND requested_generation=?"),
                        {plan.snapshot.ticket.generation,
                                plan.snapshot.fingerprint,
                                plan.snapshotKey,
                                plan.policyKey,
                                QString::fromUtf8(
                                        QJsonDocument(policyObject(plan.policy))
                                                .toJson(QJsonDocument::
                                                                Compact)),
                                plan.snapshot.formatVersion,
                                QString::fromUtf8(QJsonDocument(
                                        QJsonArray::fromStringList(
                                                plan.snapshot.capabilities))
                                                .toJson(QJsonDocument::
                                                                Compact)),
                                encodeIssues(plan.losses),
                                plan.snapshot.ticket.sourceId,
                                plan.snapshot.ticket.generation},
                        &error)) {
            return rollback(ImportOutcome::Failed, QStringLiteral("revision_write"), error);
        }
        if (cancelled()) {
            return rollback(ImportOutcome::Cancelled,
                    QStringLiteral("cancelled"),
                    cancellationMessage);
        }
        if (!executeOne(m_database,
                    QStringLiteral("UPDATE rekordbox_import_sources_v1 SET "
                                   "registry_revision=registry_revision+1 "
                                   "WHERE source_id=? AND registry_revision=? "
                                   "AND registry_revision<9223372036854775807"),
                    {plan.snapshot.ticket.sourceId,
                            plan.previousRegistryRevision},
                    &error)) {
            return rollback(ImportOutcome::Failed,
                    QStringLiteral("registry_revision_write"),
                    error);
        }
        if (!m_database.commit()) {
            return rollback(ImportOutcome::Failed,
                    QStringLiteral("commit_failed"),
                    m_database.lastError().text());
        }
        return {plan.losses.isEmpty() ? ImportOutcome::Success : ImportOutcome::Warning,
                true,
                writes,
                plan.losses};
    } catch (const std::exception& exception) {
        return rollback(ImportOutcome::Failed,
                QStringLiteral("adapter_exception"),
                QString::fromUtf8(exception.what()));
    } catch (...) {
        return rollback(ImportOutcome::Failed,
                QStringLiteral("adapter_exception"),
                QStringLiteral("Unknown application exception"));
    }
}

} // namespace mixxx::rekordbox
// End of code prepared by ChatGPT for human review and submission.
