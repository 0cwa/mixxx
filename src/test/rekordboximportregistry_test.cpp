// Prepared by ChatGPT for human review.
#include "library/rekordbox/rekordboximportregistry.h"

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <limits>
#include <stdexcept>

namespace {

using namespace mixxx::rekordbox;

QString json(const QJsonValue& value) {
    return QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact));
}

class SyntheticCanonicalAdapter final : public ImportCanonicalAdapter {
  public:
    int writeAttempts = 0;
    int failAt = -1;
    bool throwOnFailure = false;
    bool throwOnRead = false;

    bool read(const QSqlDatabase& database,
            qint64 trackId,
            const QString& field,
            QJsonValue* value,
            QString* error) const override {
        if (throwOnRead) {
            throw std::runtime_error("Synthetic adapter read exception");
        }
        QSqlQuery query(database);
        query.prepare(QStringLiteral(
                "SELECT value_json FROM canonical_fields "
                "WHERE track_id=? AND field_id=?"));
        query.addBindValue(trackId);
        query.addBindValue(field);
        if (!query.exec()) {
            *error = query.lastError().text();
            return false;
        }
        *value = query.next()
                ? QJsonDocument::fromJson(query.value(0).toByteArray()).array().first()
                : QJsonValue(QJsonValue::Undefined);
        return true;
    }

    bool write(const QSqlDatabase& database,
            qint64 trackId,
            const QString& field,
            const QJsonValue& value,
            QString* error) override {
        ++writeAttempts;
        if (writeAttempts == failAt) {
            if (throwOnFailure) {
                throw std::runtime_error("Synthetic adapter write exception");
            }
            *error = QStringLiteral("Synthetic adapter write failure");
            return false;
        }
        QSqlQuery query(database);
        if (value.isUndefined()) {
            query.prepare(
                    QStringLiteral("DELETE FROM canonical_fields WHERE "
                                   "track_id=? AND field_id=?"));
            query.addBindValue(trackId);
            query.addBindValue(field);
        } else {
            query.prepare(QStringLiteral(
                    "INSERT INTO canonical_fields VALUES(?,?,?) "
                    "ON CONFLICT(track_id,field_id) DO UPDATE SET value_json=excluded.value_json"));
            query.addBindValue(trackId);
            query.addBindValue(field);
            query.addBindValue(json(value));
        }
        if (!query.exec()) {
            *error = query.lastError().text();
            return false;
        }
        return true;
    }
};

class ImportRegistryTest : public testing::Test {
  protected:
    QTemporaryDir directory;
    QString path;
    QString error;
    std::unique_ptr<ImportSourceRegistry> registry;
    SyntheticCanonicalAdapter adapter;
    ImportPolicy preserve{QStringLiteral("synthetic-adapter-v1"), false, false};
    ImportPolicy reviewed{QStringLiteral("synthetic-adapter-v1"), false, true};

    void SetUp() override {
        ASSERT_TRUE(directory.isValid());
        path = directory.filePath(QStringLiteral("collection.sqlite"));
        registry = ImportSourceRegistry::open(path, &error);
        ASSERT_TRUE(registry) << error.toStdString();
        QSqlQuery query(registry->database());
        ASSERT_TRUE(query.exec(QStringLiteral(
                "CREATE TABLE canonical_fields ("
                "track_id INTEGER NOT NULL,field_id TEXT NOT NULL,"
                "value_json TEXT NOT NULL,PRIMARY KEY(track_id,field_id))")));
        ASSERT_TRUE(query.exec(QStringLiteral(
                "CREATE TABLE canonical_tracks (id INTEGER PRIMARY KEY)")));
        ASSERT_TRUE(query.exec(QStringLiteral("INSERT INTO canonical_tracks VALUES(7),(8)")));
    }

    QString addSource(const QString& locator = QStringLiteral("/media/USB")) {
        auto id = registry->createSource(QStringLiteral("synthetic"),
                locator,
                QStringLiteral("Same name %_-->"),
                &error);
        EXPECT_TRUE(id) << error.toStdString();
        return id.value_or(QString());
    }

    ImportSnapshot snapshot(const QString& sourceId,
            const QString& fingerprint = QStringLiteral("revision-1")) {
        auto ticket = registry->requestGeneration(sourceId, &error);
        EXPECT_TRUE(ticket) << error.toStdString();
        ImportSnapshot result;
        result.ticket = ticket.value_or(ImportTicket{});
        result.fingerprint = fingerprint;
        result.decoderRevision = QStringLiteral("synthetic-decoder-v1");
        result.formatVersion = QStringLiteral("fixture-v1");
        result.capabilities = {QStringLiteral("generic-fields")};
        result.tracks = {{QStringLiteral("101"), QStringLiteral("media-a"), 7}};
        return result;
    }

    ImportField field(const QString& annotation,
            const QString& destination,
            const QJsonValue& value,
            bool remove = false) {
        return {QStringLiteral("101"),
                annotation,
                destination,
                value,
                remove,
                {{QStringLiteral("source_ordinal"), 0},
                        {QStringLiteral("correction"),
                                QStringLiteral("none")}}};
    }

    ImportApplyResult apply(const ImportSnapshot& data, const ImportPolicy& policy) {
        const auto planned = registry->plan(data, policy, adapter);
        EXPECT_TRUE(planned.plan);
        if (!planned.plan) {
            return {planned.outcome, false, 0, planned.issues};
        }
        return registry->apply(*planned.plan, &adapter);
    }

    QJsonValue current(const QString& target, qint64 trackId = 7) {
        QJsonValue value;
        EXPECT_TRUE(adapter.read(registry->database(), trackId, target, &value, &error));
        return value;
    }

    void localEdit(const QString& target, const QJsonValue& value, qint64 trackId = 7) {
        ASSERT_TRUE(adapter.write(registry->database(), trackId, target, value, &error));
    }

    qint64 count(const QString& table) {
        QSqlQuery query(registry->database());
        EXPECT_TRUE(query.exec(QStringLiteral("SELECT count(*) FROM ") + table));
        EXPECT_TRUE(query.next());
        return query.value(0).toLongLong();
    }

    ImportSourceState state(const QString& sourceId) {
        const auto result = registry->source(sourceId, &error);
        EXPECT_TRUE(result) << error.toStdString();
        return result.value_or(ImportSourceState{});
    }
};

TEST_F(ImportRegistryTest, IdentitySurvivesRelocationReopenAndDuplicateNames) {
    const auto a = addSource();
    const auto b = addSource();
    ASSERT_NE(a, b);
    ASSERT_TRUE(registry->relocate(a, QStringLiteral("/new/mount"), &error));
    registry.reset();
    registry = ImportSourceRegistry::open(path, &error);
    ASSERT_TRUE(registry) << error.toStdString();
    EXPECT_EQ(state(a).locator, QStringLiteral("/new/mount"));
    EXPECT_EQ(state(b).locator, QStringLiteral("/media/USB"));
    EXPECT_EQ(state(a).displayName, state(b).displayName);
}

TEST_F(ImportRegistryTest, OrderedRepeatedMembershipAndEmptyNodesPersist) {
    const auto id = addSource();
    auto data = snapshot(id);
    data.tracks.push_back({QStringLiteral("202"), QStringLiteral("media-b"), 8});
    data.playlists = {
            {QStringLiteral("root-id"), {}, QStringLiteral("ROOT"), true, {}},
            {QStringLiteral("p-id"),
                    QStringLiteral("root-id"),
                    QStringLiteral("Set %_-->"),
                    false,
                    {QStringLiteral("202"),
                            QStringLiteral("101"),
                            QStringLiteral("202")}},
            {QStringLiteral("empty-p"),
                    QStringLiteral("root-id"),
                    QStringLiteral("Set %_-->"),
                    false,
                    {}},
            {QStringLiteral("empty-f"),
                    QStringLiteral("root-id"),
                    QStringLiteral(""),
                    true,
                    {}}};
    ASSERT_TRUE(apply(data, preserve).committed);
    registry.reset();
    registry = ImportSourceRegistry::open(path, &error);
    ASSERT_TRUE(registry);
    QSqlQuery query(registry->database());
    ASSERT_TRUE(query.exec(
            QStringLiteral("SELECT source_track_id FROM "
                           "rekordbox_import_members_v1 ORDER BY ordinal")));
    QStringList actual;
    while (query.next()) {
        actual << query.value(0).toString();
    }
    EXPECT_EQ(actual,
            (QStringList{QStringLiteral("202"),
                    QStringLiteral("101"),
                    QStringLiteral("202")}));
    EXPECT_EQ(count(QStringLiteral("rekordbox_import_playlists_v1")), 4);
}

TEST_F(ImportRegistryTest, CompositeSourceKeysKeepSharedMediaAnnotationsDistinct) {
    const auto a = addSource();
    const auto b = addSource(QStringLiteral("/xml/collection.xml"));
    auto first = snapshot(a);
    first.fields = {field(QStringLiteral("cue-1"), QStringLiteral("opaque-memory-a"), 192000)};
    ASSERT_TRUE(apply(first, preserve).committed);
    auto second = snapshot(b);
    second.fields = {field(QStringLiteral("cue-1"), QStringLiteral("opaque-memory-b"), 288000)};
    ASSERT_TRUE(apply(second, preserve).committed);
    EXPECT_EQ(count(QStringLiteral("rekordbox_import_tracks_v1")), 2);
    EXPECT_EQ(count(QStringLiteral("rekordbox_import_provenance_v1")), 2);
    EXPECT_EQ(current(QStringLiteral("opaque-memory-a")).toInt(), 192000);
    EXPECT_EQ(current(QStringLiteral("opaque-memory-b")).toInt(), 288000);
}

TEST_F(ImportRegistryTest, UnchangedRevisionAfterReopenPerformsNoCanonicalWrites) {
    const auto id = addSource();
    auto data = snapshot(id);
    data.fields = {field(QStringLiteral("cue"), QStringLiteral("hotcue-0"), 1)};
    ASSERT_TRUE(apply(data, preserve).committed);
    registry.reset();
    registry = ImportSourceRegistry::open(path, &error);
    ASSERT_TRUE(registry);
    data.ticket = *registry->requestGeneration(id, &error);
    adapter.writeAttempts = 0;
    const auto planned = registry->plan(data, preserve, adapter);
    ASSERT_TRUE(planned.plan);
    ASSERT_TRUE(planned.plan->unchangedRevision);
    const auto applied = registry->apply(*planned.plan, &adapter);
    EXPECT_TRUE(applied.committed);
    EXPECT_EQ(applied.canonicalWrites, 0);
    EXPECT_EQ(adapter.writeAttempts, 0);
    EXPECT_EQ(count(QStringLiteral("rekordbox_import_provenance_v1")), 1);
}

TEST_F(ImportRegistryTest, AbsentNullEmptyAndZeroHaveDifferentMeanings) {
    const auto id = addSource();
    localEdit(QStringLiteral("absent"), QStringLiteral("user-owned"));
    auto data = snapshot(id);
    data.fields = {field(QStringLiteral("absent"), QStringLiteral("absent"), QJsonValue::Undefined),
            field(QStringLiteral("null"), QStringLiteral("null"), QJsonValue::Null),
            field(QStringLiteral("empty"), QStringLiteral("empty"), QStringLiteral("")),
            field(QStringLiteral("zero"), QStringLiteral("zero"), 0)};
    const auto result = apply(data, preserve);
    ASSERT_TRUE(result.committed);
    EXPECT_EQ(result.canonicalWrites, 3);
    EXPECT_EQ(current(QStringLiteral("absent")).toString(), QStringLiteral("user-owned"));
    EXPECT_TRUE(current(QStringLiteral("null")).isNull());
    EXPECT_TRUE(current(QStringLiteral("empty")).isString());
    EXPECT_EQ(current(QStringLiteral("empty")).toString(), QStringLiteral(""));
    EXPECT_TRUE(current(QStringLiteral("zero")).isDouble());
    EXPECT_EQ(current(QStringLiteral("zero")).toInt(), 0);
    EXPECT_TRUE(current(QStringLiteral("missing")).isUndefined());
}

TEST_F(ImportRegistryTest, OccupiedNullAndZeroRequireReviewAndRemainUnchanged) {
    const auto id = addSource();
    localEdit(QStringLiteral("a"), QJsonValue::Null);
    localEdit(QStringLiteral("b"), 0);
    auto data = snapshot(id);
    data.fields = {field(QStringLiteral("a"), QStringLiteral("a"), 10),
            field(QStringLiteral("b"), QStringLiteral("b"), 20)};
    const auto denied = apply(data, preserve);
    EXPECT_EQ(denied.outcome, ImportOutcome::Denied);
    EXPECT_FALSE(denied.committed);
    EXPECT_TRUE(state(id).appliedFingerprint.isEmpty());
    const auto accepted = apply(data, reviewed);
    EXPECT_EQ(accepted.outcome, ImportOutcome::Warning);
    EXPECT_TRUE(accepted.committed);
    EXPECT_EQ(accepted.issues.size(), 2);
    EXPECT_EQ(accepted.canonicalWrites, 0);
    EXPECT_TRUE(current(QStringLiteral("a")).isNull());
    EXPECT_EQ(current(QStringLiteral("b")).toInt(), 0);
}

TEST_F(ImportRegistryTest, LocalEditPreservesOriginalAndDesiredProvenanceSeparately) {
    const auto id = addSource();
    auto data = snapshot(id);
    data.fields = {field(QStringLiteral("cue"), QStringLiteral("slot"), 10)};
    ASSERT_TRUE(apply(data, preserve).committed);
    localEdit(QStringLiteral("slot"), 15);
    data = snapshot(id, QStringLiteral("revision-2"));
    data.fields = {field(QStringLiteral("cue"), QStringLiteral("slot"), 20)};
    ASSERT_TRUE(apply(data, reviewed).committed);
    EXPECT_EQ(current(QStringLiteral("slot")).toInt(), 15);
    QSqlQuery query(registry->database());
    ASSERT_TRUE(query.exec(
            QStringLiteral("SELECT source_json,last_applied_json,owns FROM "
                           "rekordbox_import_provenance_v1")));
    ASSERT_TRUE(query.next());
    EXPECT_EQ(query.value(0).toString(), QStringLiteral("[20]"));
    EXPECT_EQ(query.value(1).toString(), QStringLiteral("[10]"));
    EXPECT_TRUE(query.value(2).toBool());
}

TEST_F(ImportRegistryTest, LocalDeletionSurvivesUntilVersionedReplacementIsApproved) {
    const auto id = addSource();
    auto data = snapshot(id);
    data.fields = {field(QStringLiteral("cue"), QStringLiteral("slot"), 10)};
    ASSERT_TRUE(apply(data, preserve).committed);
    localEdit(QStringLiteral("slot"), QJsonValue::Undefined);
    data = snapshot(id, QStringLiteral("revision-2"));
    data.fields = {field(QStringLiteral("cue"), QStringLiteral("slot"), 20)};
    ASSERT_TRUE(apply(data, reviewed).committed);
    EXPECT_TRUE(current(QStringLiteral("slot")).isUndefined());
    data.ticket = *registry->requestGeneration(id, &error);
    ImportPolicy replacement{QStringLiteral("synthetic-adapter-v2"), true, true};
    const auto planned = registry->plan(data, replacement, adapter);
    ASSERT_TRUE(planned.plan);
    EXPECT_FALSE(planned.plan->unchangedRevision);
    const auto applied = registry->apply(*planned.plan, &adapter);
    EXPECT_TRUE(applied.committed);
    EXPECT_EQ(current(QStringLiteral("slot")).toInt(), 20);
}

TEST_F(ImportRegistryTest, OtherSourceSlotConflictKeepsBothAnnotationRecords) {
    const auto a = addSource();
    const auto b = addSource();
    auto first = snapshot(a);
    first.fields = {field(QStringLiteral("cue"), QStringLiteral("hotcue-0"), 10)};
    ASSERT_TRUE(apply(first, preserve).committed);
    auto second = snapshot(b);
    second.fields = {field(QStringLiteral("cue"), QStringLiteral("hotcue-0"), 20)};
    const auto result = apply(second, reviewed);
    ASSERT_TRUE(result.committed);
    EXPECT_EQ(result.outcome, ImportOutcome::Warning);
    EXPECT_EQ(current(QStringLiteral("hotcue-0")).toInt(), 10);
    EXPECT_EQ(count(QStringLiteral("rekordbox_import_provenance_v1")), 2);
    QSqlQuery query(registry->database());
    ASSERT_TRUE(query.exec(QStringLiteral("SELECT sum(owns) FROM rekordbox_import_provenance_v1")));
    ASSERT_TRUE(query.next());
    EXPECT_EQ(query.value(0).toInt(), 1);
}

TEST_F(ImportRegistryTest, AdapterFailureRollsBackThenSameGenerationRetries) {
    const auto id = addSource();
    auto data = snapshot(id);
    data.fields = {field(QStringLiteral("a"), QStringLiteral("a"), 1),
            field(QStringLiteral("b"), QStringLiteral("b"), 2)};
    adapter.failAt = 2;
    const auto failed = apply(data, preserve);
    EXPECT_FALSE(failed.committed);
    EXPECT_EQ(failed.outcome, ImportOutcome::Failed);
    EXPECT_EQ(count(QStringLiteral("canonical_fields")), 0);
    EXPECT_EQ(count(QStringLiteral("rekordbox_import_tracks_v1")), 0);
    EXPECT_EQ(count(QStringLiteral("rekordbox_import_provenance_v1")), 0);
    EXPECT_EQ(state(id).appliedGeneration, 0);
    adapter.failAt = -1;
    EXPECT_TRUE(apply(data, preserve).committed);
    EXPECT_EQ(count(QStringLiteral("canonical_fields")), 2);
}

TEST_F(ImportRegistryTest, AdapterExceptionRollsBack) {
    const auto id = addSource();
    auto data = snapshot(id);
    data.fields = {field(QStringLiteral("a"), QStringLiteral("a"), 1),
            field(QStringLiteral("b"), QStringLiteral("b"), 2)};
    adapter.failAt = 2;
    adapter.throwOnFailure = true;
    const auto result = apply(data, preserve);
    EXPECT_EQ(result.outcome, ImportOutcome::Failed);
    EXPECT_FALSE(result.committed);
    EXPECT_EQ(count(QStringLiteral("canonical_fields")), 0);
    EXPECT_TRUE(state(id).appliedFingerprint.isEmpty());
}

TEST_F(ImportRegistryTest, CancellationBeforeAndDuringApplicationDoesNotAdvanceRevision) {
    const auto id = addSource();
    auto data = snapshot(id);
    data.fields = {field(QStringLiteral("a"), QStringLiteral("a"), 1),
            field(QStringLiteral("b"), QStringLiteral("b"), 2)};
    const auto planned = registry->plan(data, preserve, adapter);
    ASSERT_TRUE(planned.plan);
    const auto before = registry->apply(*planned.plan, &adapter, [] { return true; });
    EXPECT_EQ(before.outcome, ImportOutcome::Cancelled);
    ASSERT_FALSE(before.issues.isEmpty());
    EXPECT_FALSE(before.issues.front().message.trimmed().isEmpty());
    const auto result = registry->apply(*planned.plan, &adapter, [&] {
        return adapter.writeAttempts >= 1;
    });
    EXPECT_EQ(result.outcome, ImportOutcome::Cancelled);
    ASSERT_FALSE(result.issues.isEmpty());
    EXPECT_FALSE(result.issues.front().message.trimmed().isEmpty());
    EXPECT_FALSE(result.committed);
    EXPECT_EQ(count(QStringLiteral("canonical_fields")), 0);
    EXPECT_TRUE(state(id).appliedFingerprint.isEmpty());
}

TEST_F(ImportRegistryTest, InvalidDecodeDoesNotPoisonTheRepairedFingerprint) {
    const auto id = addSource();
    auto data = snapshot(id);
    data.fields = {field(QStringLiteral("cue"), QStringLiteral("slot"), 1)};
    data.decodeOutcome = ImportOutcome::Invalid;
    auto result = registry->plan(data, preserve, adapter);
    EXPECT_EQ(result.outcome, ImportOutcome::Invalid);
    EXPECT_FALSE(result.plan);
    EXPECT_TRUE(state(id).appliedFingerprint.isEmpty());
    data.decodeOutcome = ImportOutcome::Success;
    EXPECT_TRUE(apply(data, preserve).committed);
    EXPECT_EQ(state(id).appliedFingerprint, data.fingerprint);
}

TEST_F(ImportRegistryTest, StaleGenerationCannotPublish) {
    const auto id = addSource();
    auto data = snapshot(id);
    data.fields = {field(QStringLiteral("cue"), QStringLiteral("slot"), 1)};
    const auto planned = registry->plan(data, preserve, adapter);
    ASSERT_TRUE(planned.plan);
    ASSERT_TRUE(registry->requestGeneration(id, &error));
    const auto result = registry->apply(*planned.plan, &adapter);
    EXPECT_EQ(result.outcome, ImportOutcome::Cancelled);
    ASSERT_FALSE(result.issues.isEmpty());
    EXPECT_FALSE(result.issues.front().message.trimmed().isEmpty());
    EXPECT_EQ(count(QStringLiteral("canonical_fields")), 0);
    EXPECT_TRUE(state(id).appliedFingerprint.isEmpty());
}

TEST_F(ImportRegistryTest, CanonicalMutationAfterPreviewRejectsTheWholePlan) {
    const auto id = addSource();
    auto data = snapshot(id);
    data.fields = {field(QStringLiteral("a"), QStringLiteral("a"), 1),
            field(QStringLiteral("b"), QStringLiteral("b"), 2)};
    const auto planned = registry->plan(data, preserve, adapter);
    ASSERT_TRUE(planned.plan);
    localEdit(QStringLiteral("b"), 99);
    const auto result = registry->apply(*planned.plan, &adapter);
    EXPECT_FALSE(result.committed);
    EXPECT_EQ(result.outcome, ImportOutcome::Failed);
    EXPECT_TRUE(current(QStringLiteral("a")).isUndefined());
    EXPECT_EQ(current(QStringLiteral("b")).toInt(), 99);
    EXPECT_TRUE(state(id).appliedFingerprint.isEmpty());
}

TEST_F(ImportRegistryTest, ExplicitTombstoneOnlyRemovesUneditedSourceOwnedValues) {
    const auto id = addSource();
    auto data = snapshot(id);
    data.fields = {field(QStringLiteral("a"), QStringLiteral("a"), 1),
            field(QStringLiteral("b"), QStringLiteral("b"), 2)};
    ASSERT_TRUE(apply(data, preserve).committed);
    localEdit(QStringLiteral("b"), 77);
    data = snapshot(id, QStringLiteral("revision-2"));
    data.fields = {field(QStringLiteral("a"), QStringLiteral("a"), QJsonValue::Undefined, true),
            field(QStringLiteral("b"), QStringLiteral("b"), QJsonValue::Undefined, true)};
    const auto result = apply(data, reviewed);
    ASSERT_TRUE(result.committed);
    EXPECT_TRUE(current(QStringLiteral("a")).isUndefined());
    EXPECT_EQ(current(QStringLiteral("b")).toInt(), 77);
}

TEST_F(ImportRegistryTest, RemovedTracksDetachMappingsWithoutDeletingCanonicalData) {
    const auto id = addSource();
    auto data = snapshot(id);
    data.fields = {field(QStringLiteral("cue"), QStringLiteral("slot"), 1)};
    ASSERT_TRUE(apply(data, preserve).committed);
    data = snapshot(id, QStringLiteral("empty-revision"));
    data.tracks.clear();
    ASSERT_TRUE(apply(data, preserve).committed);
    EXPECT_EQ(count(QStringLiteral("rekordbox_import_tracks_v1")), 0);
    EXPECT_EQ(count(QStringLiteral("rekordbox_import_provenance_v1")), 0);
    EXPECT_EQ(count(QStringLiteral("canonical_tracks")), 2);
    EXPECT_EQ(current(QStringLiteral("slot")).toInt(), 1);
}

TEST_F(ImportRegistryTest, RemovedSourcePreservesCanonicalDataAndOtherSource) {
    const auto a = addSource(QStringLiteral("/media/USB%_-->"));
    const auto b = addSource(QStringLiteral("/media/USBxx"));
    auto first = snapshot(a);
    first.fields = {field(QStringLiteral("cue"), QStringLiteral("a"), 1)};
    ASSERT_TRUE(apply(first, preserve).committed);
    auto second = snapshot(b);
    second.fields = {field(QStringLiteral("cue"), QStringLiteral("b"), 2)};
    ASSERT_TRUE(apply(second, preserve).committed);
    ASSERT_TRUE(registry->removeSource(a, &error));
    EXPECT_FALSE(registry->source(a, &error));
    EXPECT_TRUE(registry->source(b, &error));
    EXPECT_EQ(count(QStringLiteral("rekordbox_import_provenance_v1")), 1);
    EXPECT_EQ(count(QStringLiteral("canonical_fields")), 2);
    EXPECT_EQ(count(QStringLiteral("canonical_tracks")), 2);
}

TEST_F(ImportRegistryTest, SourceRemovalInvalidatesAnInflightPlan) {
    const auto id = addSource();
    const auto data = snapshot(id);
    const auto planned = registry->plan(data, preserve, adapter);
    ASSERT_TRUE(planned.plan);
    ASSERT_TRUE(registry->removeSource(id, &error));
    const auto result = registry->apply(*planned.plan, &adapter);
    EXPECT_EQ(result.outcome, ImportOutcome::Unavailable);
    ASSERT_FALSE(result.issues.isEmpty());
    EXPECT_FALSE(result.issues.front().message.trimmed().isEmpty());
    const auto unavailable = registry->plan(data, preserve, adapter);
    EXPECT_EQ(unavailable.outcome, ImportOutcome::Unavailable);
    ASSERT_FALSE(unavailable.issues.isEmpty());
    EXPECT_FALSE(unavailable.issues.front().message.trimmed().isEmpty());
}

TEST_F(ImportRegistryTest, CyclicParentsAndMissingPlaylistTracksAreRejected) {
    const auto id = addSource();
    auto data = snapshot(id);
    data.playlists = {{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("A"), true, {}},
            {QStringLiteral("b"), QStringLiteral("a"), QStringLiteral("B"), true, {}}};
    EXPECT_EQ(registry->plan(data, preserve, adapter).outcome, ImportOutcome::Invalid);
    data.playlists = {{QStringLiteral("p"),
            {},
            QStringLiteral("P"),
            false,
            {QStringLiteral("missing")}}};
    EXPECT_EQ(registry->plan(data, preserve, adapter).outcome, ImportOutcome::Invalid);
    EXPECT_TRUE(state(id).appliedFingerprint.isEmpty());
}

TEST_F(ImportRegistryTest, CanonicalRemapRequiresASeparateReviewedMigration) {
    const auto id = addSource();
    auto data = snapshot(id);
    ASSERT_TRUE(apply(data, preserve).committed);
    data = snapshot(id, QStringLiteral("revision-2"));
    data.tracks[0].canonicalTrackId = 8;
    EXPECT_EQ(registry->plan(data, preserve, adapter).outcome, ImportOutcome::Unsupported);
    EXPECT_EQ(state(id).appliedFingerprint, QStringLiteral("revision-1"));
}

TEST_F(ImportRegistryTest, GenerationExhaustionCannotReuseARequestIdentity) {
    const auto id = addSource();
    QSqlQuery query(registry->database());
    ASSERT_TRUE(query.exec(QStringLiteral(
            "UPDATE rekordbox_import_sources_v1 "
            "SET requested_generation=9223372036854775807")));
    EXPECT_FALSE(registry->requestGeneration(id, &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST_F(ImportRegistryTest, SnapshotLeaseStaysAliveWithTheImmutablePlan) {
    const auto id = addSource();
    auto data = snapshot(id);
    auto lease = std::make_shared<int>(42);
    std::weak_ptr<int> observed = lease;
    data.accessLease = lease;
    auto planned = registry->plan(data, preserve, adapter);
    ASSERT_TRUE(planned.plan);
    lease.reset();
    data.accessLease.reset();
    EXPECT_FALSE(observed.expired());
    ASSERT_TRUE(registry->apply(*planned.plan, &adapter).committed);
    planned.plan.reset();
    EXPECT_TRUE(observed.expired());
}

TEST_F(ImportRegistryTest, ApprovedMatchingLocalValueBecomesNewLastAppliedProvenance) {
    const auto id = addSource();
    auto data = snapshot(id);
    data.fields = {field(QStringLiteral("cue"), QStringLiteral("slot"), 10)};
    ASSERT_TRUE(apply(data, preserve).committed);
    localEdit(QStringLiteral("slot"), 20);
    data = snapshot(id, QStringLiteral("revision-2"));
    data.fields = {field(QStringLiteral("cue"), QStringLiteral("slot"), 20)};
    ImportPolicy replacement{QStringLiteral("synthetic-adapter-v2"), true, true};
    const auto approved = apply(data, replacement);
    ASSERT_TRUE(approved.committed);
    EXPECT_EQ(approved.canonicalWrites, 0);
    data = snapshot(id, QStringLiteral("revision-3"));
    data.fields = {field(QStringLiteral("cue"), QStringLiteral("slot"), 30)};
    ASSERT_TRUE(apply(data, preserve).committed);
    EXPECT_EQ(current(QStringLiteral("slot")).toInt(), 30);
}

TEST_F(ImportRegistryTest, ReadExceptionReturnsStructuredFailureWithoutMutation) {
    const auto id = addSource();
    auto data = snapshot(id);
    data.fields = {field(QStringLiteral("cue"), QStringLiteral("slot"), 10)};
    adapter.throwOnRead = true;
    const auto planned = registry->plan(data, preserve, adapter);
    EXPECT_EQ(planned.outcome, ImportOutcome::Failed);
    EXPECT_FALSE(planned.plan);
    EXPECT_TRUE(state(id).appliedFingerprint.isEmpty());
}

TEST_F(ImportRegistryTest, NonfiniteJsonAndAmbiguousTombstonesAreRejected) {
    const auto id = addSource();
    auto data = snapshot(id);
    data.fields = {field(QStringLiteral("cue"),
            QStringLiteral("slot"),
            std::numeric_limits<double>::quiet_NaN())};
    EXPECT_EQ(registry->plan(data, preserve, adapter).outcome, ImportOutcome::Invalid);
    data.fields = {field(QStringLiteral("cue"), QStringLiteral("slot"), 0, true)};
    EXPECT_EQ(registry->plan(data, preserve, adapter).outcome, ImportOutcome::Invalid);
    EXPECT_TRUE(state(id).appliedFingerprint.isEmpty());
}

TEST_F(ImportRegistryTest, DecodeWarningRequiresDiagnosticsAndExplicitAcceptance) {
    const auto id = addSource();
    auto data = snapshot(id);
    data.decodeOutcome = ImportOutcome::Warning;
    EXPECT_EQ(registry->plan(data, preserve, adapter).outcome, ImportOutcome::Invalid);
    data.decodeIssues = {{QStringLiteral("synthetic_loss"),
            QStringLiteral("Fixture omitted a color"),
            {},
            {}}};
    EXPECT_EQ(apply(data, preserve).outcome, ImportOutcome::Denied);
    const auto accepted = apply(data, reviewed);
    EXPECT_TRUE(accepted.committed);
    EXPECT_EQ(accepted.outcome, ImportOutcome::Warning);
    EXPECT_EQ(state(id).losses.size(), 1);
}

TEST_F(ImportRegistryTest, SameFingerprintMappingRetargetStillRequiresMigration) {
    const auto id = addSource();
    auto data = snapshot(id);
    ASSERT_TRUE(apply(data, preserve).committed);
    data.ticket = *registry->requestGeneration(id, &error);
    data.tracks[0].canonicalTrackId = 8;
    EXPECT_EQ(registry->plan(data, preserve, adapter).outcome, ImportOutcome::Unsupported);
    EXPECT_EQ(state(id).appliedGeneration, 1);
}

TEST_F(ImportRegistryTest, SameFingerprintNewWarningCannotBeCachedAway) {
    const auto id = addSource();
    auto data = snapshot(id);
    ASSERT_TRUE(apply(data, preserve).committed);
    data.ticket = *registry->requestGeneration(id, &error);
    data.decodeOutcome = ImportOutcome::Warning;
    data.decodeIssues = {{QStringLiteral("new_loss"),
            QStringLiteral("New decoder limitation"),
            {},
            {}}};
    const auto planned = registry->plan(data, preserve, adapter);
    ASSERT_TRUE(planned.plan);
    EXPECT_FALSE(planned.plan->unchangedRevision);
    EXPECT_EQ(planned.outcome, ImportOutcome::Warning);
    EXPECT_EQ(registry->apply(*planned.plan, &adapter).outcome, ImportOutcome::Denied);
    EXPECT_EQ(state(id).appliedGeneration, 1);
}

TEST_F(ImportRegistryTest, DecoderFormatAndCapabilityChangesInvalidateCachedRevision) {
    const auto id = addSource();
    const auto initial = snapshot(id);
    ASSERT_TRUE(apply(initial, preserve).committed);
    for (int variant = 0; variant < 3; ++variant) {
        SCOPED_TRACE(variant);
        auto changed = initial;
        changed.ticket = *registry->requestGeneration(id, &error);
        if (variant == 0) {
            changed.decoderRevision = QStringLiteral("synthetic-decoder-v2");
        } else if (variant == 1) {
            changed.formatVersion = QStringLiteral("fixture-v2");
        } else {
            changed.capabilities << QStringLiteral("new-capability");
        }
        const auto planned = registry->plan(changed, preserve, adapter);
        ASSERT_TRUE(planned.plan);
        EXPECT_FALSE(planned.plan->unchangedRevision);
    }
    auto missing = initial;
    missing.ticket = *registry->requestGeneration(id, &error);
    missing.decoderRevision.clear();
    EXPECT_EQ(registry->plan(missing, preserve, adapter).outcome, ImportOutcome::Invalid);
}

TEST_F(ImportRegistryTest, MutatedPlanCannotTurnAPreservedConflictIntoAnOverwrite) {
    const auto id = addSource();
    localEdit(QStringLiteral("slot"), 99);
    auto data = snapshot(id);
    data.fields = {field(QStringLiteral("cue"), QStringLiteral("slot"), 10)};
    const auto planned = registry->plan(data, preserve, adapter);
    ASSERT_TRUE(planned.plan);
    auto changed = *planned.plan;
    changed.actions[0].kind = ImportActionKind::Set;
    changed.losses.clear();
    const auto applied = registry->apply(changed, &adapter);
    EXPECT_FALSE(applied.committed);
    EXPECT_EQ(applied.outcome, ImportOutcome::Invalid);
    EXPECT_EQ(current(QStringLiteral("slot")).toInt(), 99);
    EXPECT_TRUE(state(id).appliedFingerprint.isEmpty());
}

TEST_F(ImportRegistryTest, SameGenerationPartialPlansCannotRetargetAnInterveningMapping) {
    const auto id = addSource();
    auto first = snapshot(id);
    first.scope = ImportScope::TracksOnly;
    first.fields = {field(QStringLiteral("cue"), QStringLiteral("slot"), 10)};
    auto second = first;
    second.tracks[0].canonicalTrackId = 8;
    second.tracks[0].canonicalMediaId = QStringLiteral("media-b");
    const auto firstPlan = registry->plan(first, preserve, adapter);
    const auto secondPlan = registry->plan(second, preserve, adapter);
    ASSERT_TRUE(firstPlan.plan);
    ASSERT_TRUE(secondPlan.plan);
    ASSERT_TRUE(registry->apply(*firstPlan.plan, &adapter).committed);
    const auto rejected = registry->apply(*secondPlan.plan, &adapter);
    EXPECT_FALSE(rejected.committed);
    EXPECT_EQ(rejected.outcome, ImportOutcome::Cancelled);
    EXPECT_TRUE(current(QStringLiteral("slot"), 8).isUndefined());
    EXPECT_EQ(state(id).appliedGeneration, 0);
    EXPECT_TRUE(state(id).appliedFingerprint.isEmpty());
    EXPECT_EQ(state(id).registryRevision, 1);
}

TEST_F(ImportRegistryTest, SameGenerationPartialPlansCannotRetargetAnInterveningAnnotation) {
    const auto id = addSource();
    auto first = snapshot(id);
    first.scope = ImportScope::TracksOnly;
    first.fields = {field(QStringLiteral("cue"), QStringLiteral("slot-a"), 10)};
    auto second = first;
    second.fields[0].targetField = QStringLiteral("slot-b");
    const auto firstPlan = registry->plan(first, preserve, adapter);
    const auto secondPlan = registry->plan(second, preserve, adapter);
    ASSERT_TRUE(firstPlan.plan);
    ASSERT_TRUE(secondPlan.plan);
    ASSERT_TRUE(registry->apply(*firstPlan.plan, &adapter).committed);
    EXPECT_EQ(registry->apply(*secondPlan.plan, &adapter).outcome, ImportOutcome::Cancelled);
    EXPECT_TRUE(current(QStringLiteral("slot-b")).isUndefined());
}

TEST_F(ImportRegistryTest, RegistryMutationRevisionExhaustionRollsBackPartialWrites) {
    const auto id = addSource();
    auto data = snapshot(id);
    data.scope = ImportScope::TracksOnly;
    data.fields = {field(QStringLiteral("cue"), QStringLiteral("slot"), 10)};
    QSqlQuery query(registry->database());
    ASSERT_TRUE(query.exec(
            QStringLiteral("UPDATE rekordbox_import_sources_v1 SET "
                           "registry_revision=9223372036854775807")));
    const auto applied = apply(data, preserve);
    EXPECT_FALSE(applied.committed);
    ASSERT_EQ(applied.issues.size(), 1);
    EXPECT_EQ(applied.issues[0].code, QStringLiteral("registry_revision_write"));
    EXPECT_TRUE(current(QStringLiteral("slot")).isUndefined());
    EXPECT_EQ(count(QStringLiteral("rekordbox_import_tracks_v1")), 0);
}

} // namespace
// End of code prepared by ChatGPT for human review and submission.
