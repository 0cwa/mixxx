// Prepared by ChatGPT for human review.
#include "library/rekordbox/rekordboxxmlimporter.h"

#include <gtest/gtest.h>

#include <QBuffer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUrl>
#include <QXmlStreamReader>

#include "library/dao/cuedao.h"
#include "track/beats.h"
#include "track/keyfactory.h"
#include "util/db/sqlite.h"
#include "util/fpclassify.h"

namespace {
using namespace mixxx;
using namespace mixxx::rekordbox;

class RekordboxXmlImporterTest : public testing::Test {
  protected:
    QTemporaryDir directory;
    QString error;
    std::unique_ptr<ImportSourceRegistry> registry;
    QString source;
    ImportTicket ticket;
    XmlMediaFacts media;
    XmlCanonicalAccess access;
    ImportPolicy policy{QStringLiteral("xml-test-policy-v1"), false, false};
    bool cacheAllowed = true;

    void SetUp() override {
        ASSERT_TRUE(directory.isValid());
        registry = ImportSourceRegistry::open(
                directory.filePath(QStringLiteral("collection.sqlite")),
                &error);
        ASSERT_TRUE(registry) << error.toStdString();
        const auto supplied = qEnvironmentVariable("REKORDBOX_TEST_SCHEMA");
        QFile schema(supplied.isEmpty() ? QStringLiteral(":/schema.xml") : supplied);
        ASSERT_TRUE(schema.open(QIODevice::ReadOnly)) << schema.fileName().toStdString();
        QXmlStreamReader xml(&schema);
        int revisions = 0;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement() && xml.name() == QLatin1String("sql")) {
                const auto sql = xml.readElementText();
                for (const auto& command : sql.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
                    if (!command.trimmed().isEmpty()) {
                        QSqlQuery query(registry->database());
                        ASSERT_TRUE(query.exec(command)) << query.lastError().text().toStdString()
                                                         << " SQL " << command.toStdString();
                    }
                }
                ++revisions;
            }
        }
        ASSERT_FALSE(xml.hasError());
        ASSERT_GT(revisions, 30);
        source = *registry->createSource(QStringLiteral("xml"),
                QStringLiteral("/source.xml"),
                QStringLiteral("Source"),
                &error);
        ticket = *registry->requestGeneration(source, &error);
        media = {directory.filePath(QStringLiteral("media.wav")),
                48000,
                6,
                30,
                1536,
                1000,
                QStringLiteral("wav"),
                QDateTime::fromString(QStringLiteral("2024-01-01T00:00:00Z"), Qt::ISODate)};
        access.lease = std::make_shared<const int>(7);
        access.externalCollectionsAbsent = true;
        access.check = [&](qint64, const QString&, QString* errorText) {
            if (!cacheAllowed && errorText) {
                *errorText = QStringLiteral("Test cached track guard");
            }
            return cacheAllowed;
        };
    }

    std::shared_ptr<const Library> decoded(
            const QString& body = {}, const QString& trackAttrs = {}) {
        QByteArray bytes = (QStringLiteral(
                                    "<DJ_PLAYLISTS "
                                    "Version=\"1.0.0\"><COLLECTION><TRACK "
                                    "TrackID=\"101\" Location=\"") +
                media.location + QStringLiteral("\" Name=\"Source title\" ") +
                trackAttrs + QStringLiteral(">") + body +
                QStringLiteral("</TRACK></COLLECTION><PLAYLISTS><NODE "
                               "Name=\"root\" Type=\"0\">"
                               "<NODE Name=\"repeat\" Type=\"1\" "
                               "KeyType=\"1\"><TRACK Key=\"") +
                media.location + QStringLiteral("\"/><TRACK Key=\"") +
                media.location +
                QStringLiteral("\"/></NODE></NODE></PLAYLISTS></DJ_PLAYLISTS>"))
                                   .toUtf8();
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::ReadOnly);
        const auto result = decodeXml(&buffer);
        EXPECT_TRUE(result.snapshot);
        return result.snapshot;
    }

    XmlTrackImportResult apply(const std::shared_ptr<const Library>& library,
            const QString& fingerprint = QStringLiteral("bytes-v1"),
            const std::function<bool()>& cancelled = {}) {
        return importXmlTrack(registry.get(),
                library,
                0,
                QStringLiteral("id:101"),
                ticket,
                fingerprint,
                media,
                policy,
                access,
                cancelled);
    }

    QVariant scalar(const QString& sql, const QVariantList& values = {}) {
        QSqlQuery query(registry->database());
        EXPECT_TRUE(query.prepare(sql));
        for (const auto& value : values) {
            query.addBindValue(value);
        }
        EXPECT_TRUE(query.exec()) << query.lastError().text().toStdString();
        EXPECT_TRUE(query.next());
        return query.value(0);
    }

    void exec(const QString& sql) {
        QSqlQuery query(registry->database());
        ASSERT_TRUE(query.exec(sql)) << query.lastError().text().toStdString();
    }
};

TEST_F(RekordboxXmlImporterTest, ActualSchemaCueDaoAndGridRoundTripAfterReopen) {
    const auto library = decoded(QStringLiteral(
            "<TEMPO Inizio=\"0\" Bpm=\"120\" Metro=\"4/4\" Battito=\"3\"/>"
            "<TEMPO Inizio=\"2\" Bpm=\"90\" Metro=\"4/4\" Battito=\"3\"/>"
            "<POSITION_MARK Name=\"Memory A\" Type=\"0\" Start=\"0.0005\" "
            "Num=\"-1\"/>"
            "<POSITION_MARK Name=\"Memory B\" Type=\"0\" Start=\"0.0005\" "
            "Num=\"-1\"/>"
            "<POSITION_MARK Name=\"Hot loop\" Type=\"4\" Start=\"0\" End=\"2\" "
            "Num=\"7\" Red=\"0\" Green=\"0\" Blue=\"0\"/>"));
    const auto imported = apply(library);
    ASSERT_TRUE(imported.application.committed);
    EXPECT_TRUE(imported.createdTrack);
    EXPECT_EQ(scalar(QStringLiteral("SELECT COUNT(*) FROM library")).toInt(), 1);
    EXPECT_EQ(scalar(QStringLiteral("SELECT title FROM library")).toString(),
            QStringLiteral("Source title"));
    EXPECT_EQ(scalar(QStringLiteral("SELECT channels FROM library")).toInt(), 6);
    const auto path = registry->database().databaseName();
    registry.reset();
    registry = ImportSourceRegistry::open(path, &error);
    ASSERT_TRUE(registry);
    const auto storedMainCue = scalar(QStringLiteral("SELECT cuepoint FROM library"));
    ASSERT_FALSE(storedMainCue.isNull());
    const auto mainCuePosition =
            audio::FramePos::fromEngineSamplePosMaybeInvalid(storedMainCue.toDouble());
    EXPECT_TRUE(mainCuePosition.isValid());
    EXPECT_EQ(mainCuePosition, audio::kStartFramePos);
    EXPECT_EQ(scalar(QStringLiteral("SELECT COUNT(*) FROM cues WHERE type=?"),
                      {static_cast<int>(CueType::MainCue)})
                      .toInt(),
            0);
    CueDAO cues;
    cues.initialize(registry->database());
    const auto loaded = cues.getCuesForTrack(TrackId(QVariant(imported.canonicalTrackId)));
    ASSERT_EQ(loaded.size(), 3);
    EXPECT_EQ(loaded[0]->getType(), CueType::Memory);
    EXPECT_EQ(loaded[0]->getPosition(), audio::FramePos(24));
    EXPECT_EQ(loaded[1]->getPosition(), audio::FramePos(24));
    EXPECT_NE(loaded[0]->getId(), loaded[1]->getId());
    EXPECT_EQ(loaded[2]->getType(), CueType::Loop);
    EXPECT_EQ(loaded[2]->getHotCue(), 7);
    EXPECT_EQ(loaded[2]->getEndPosition(), audio::FramePos(96000));
    EXPECT_EQ(loaded[2]->getColor(), RgbColor(0));
    const auto beats = Beats::fromByteArray(audio::SampleRate(48000),
            scalar(QStringLiteral("SELECT beats_version FROM library")).toString(),
            scalar(QStringLiteral("SELECT beats_sub_version FROM library")).toString(),
            scalar(QStringLiteral("SELECT beats FROM library")).toByteArray());
    ASSERT_TRUE(beats);
    EXPECT_EQ(beats->getDownbeatsOffset(), 2);
    EXPECT_EQ(beats->findNextBeat(audio::FramePos(24001)), audio::FramePos(48000));
    EXPECT_EQ(beats->findNextBeat(audio::FramePos(96001)), audio::FramePos(128000));
    EXPECT_TRUE(scalar(
            QStringLiteral(
                    "SELECT fingerprint FROM rekordbox_import_sources_v1"))
                    .isNull());
    EXPECT_EQ(scalar(QStringLiteral("SELECT applied_generation FROM "
                                    "rekordbox_import_sources_v1"))
                      .toInt(),
            0);
    EXPECT_EQ(scalar(QStringLiteral("SELECT fingerprint FROM "
                                    "rekordbox_import_track_revisions_v1"))
                      .toString(),
            QStringLiteral("bytes-v1"));
}

TEST_F(RekordboxXmlImporterTest, UnchangedReopenAndSourceRefreshPreserveCueIdentity) {
    const auto library = decoded(QStringLiteral(
            "<POSITION_MARK Type=\"0\" Start=\"1\" Num=\"-1\"/>"));
    const auto first = apply(library);
    ASSERT_TRUE(first.application.committed);
    const auto second = apply(library);
    ASSERT_TRUE(second.application.committed);
    EXPECT_EQ(second.canonicalTrackId, first.canonicalTrackId);
    EXPECT_FALSE(second.createdTrack);
    EXPECT_EQ(second.application.canonicalWrites, 0);
    EXPECT_EQ(scalar(QStringLiteral("SELECT COUNT(*) FROM cues")).toInt(), 1);
    exec(QStringLiteral("UPDATE library SET cuepoint=48000"));
    const auto cueId = scalar(QStringLiteral("SELECT id FROM cues")).toInt();
    const auto changed = decoded(QStringLiteral(
            "<POSITION_MARK Name=\"Updated memory\" Type=\"0\" Start=\"2\" Num=\"-1\"/>"));
    const auto refreshed = apply(changed, QStringLiteral("changed-cue"));
    ASSERT_TRUE(refreshed.application.committed);
    EXPECT_DOUBLE_EQ(scalar(QStringLiteral("SELECT cuepoint FROM library")).toDouble(), 48000);
    EXPECT_EQ(scalar(QStringLiteral("SELECT COUNT(*) FROM cues")).toInt(), 1);
    EXPECT_EQ(scalar(QStringLiteral("SELECT id FROM cues")).toInt(), cueId);
    CueDAO cues;
    cues.initialize(registry->database());
    const auto loaded = cues.getCuesForTrack(TrackId(QVariant(refreshed.canonicalTrackId)));
    ASSERT_EQ(loaded.size(), 1);
    EXPECT_EQ(loaded[0]->getPosition(), audio::FramePos(96000));
    EXPECT_EQ(loaded[0]->getLabel(), QStringLiteral("Updated memory"));
    EXPECT_FALSE(loaded[0]->getEndPosition().isValid());
}

TEST_F(RekordboxXmlImporterTest, PartialApplicationPreservesOtherMappingsAndPlaylists) {
    const auto library = decoded();
    ASSERT_TRUE(apply(library).application.committed);
    const QString escaped = source;
    exec(
            QStringLiteral("INSERT INTO rekordbox_import_tracks_v1 "
                           "VALUES('%1','other','media-other',99)")
                    .arg(escaped));
    exec(
            QStringLiteral("INSERT INTO rekordbox_import_playlists_v1 "
                           "VALUES('%1','opaque',NULL,'Duplicate%%_',0)")
                    .arg(escaped));
    exec(
            QStringLiteral("INSERT INTO rekordbox_import_members_v1 "
                           "VALUES('%1','opaque',0,'other')")
                    .arg(escaped));
    exec(
            QStringLiteral("UPDATE rekordbox_import_sources_v1 SET "
                           "fingerprint='whole-source',applied_generation=1"));
    ASSERT_TRUE(apply(library).application.committed);
    EXPECT_EQ(scalar(QStringLiteral("SELECT COUNT(*) FROM rekordbox_import_tracks_v1")).toInt(), 2);
    EXPECT_EQ(
            scalar(QStringLiteral(
                           "SELECT COUNT(*) FROM rekordbox_import_members_v1"))
                    .toInt(),
            1);
    EXPECT_EQ(scalar(QStringLiteral("SELECT fingerprint FROM "
                                    "rekordbox_import_sources_v1"))
                      .toString(),
            QStringLiteral("whole-source"));
}

TEST_F(RekordboxXmlImporterTest, TwoSourcesShareMediaButPreserveSeparateMemoryIdentity) {
    const auto library = decoded(QStringLiteral(
            "<POSITION_MARK Type=\"0\" Start=\"1\" Num=\"-1\"/>"));
    const auto first = apply(library);
    ASSERT_TRUE(first.application.committed);
    source = *registry->createSource(QStringLiteral("xml"),
            QStringLiteral("/second.xml"),
            QStringLiteral("Source"),
            &error);
    ticket = *registry->requestGeneration(source, &error);
    const auto second = apply(library);
    ASSERT_TRUE(second.application.committed);
    EXPECT_EQ(first.canonicalTrackId, second.canonicalTrackId);
    EXPECT_EQ(scalar(QStringLiteral("SELECT COUNT(*) FROM library")).toInt(), 1);
    EXPECT_EQ(scalar(QStringLiteral("SELECT COUNT(*) FROM cues")).toInt(), 2);
}

TEST_F(RekordboxXmlImporterTest, OccupiedHotSlotRequiresReviewAndNeverOverwritesOtherSource) {
    const auto first =
            decoded(QStringLiteral("<POSITION_MARK Name=\"First\" Type=\"0\" "
                                   "Start=\"1\" Num=\"2\"/>"));
    ASSERT_TRUE(apply(first).application.committed);
    source = *registry->createSource(QStringLiteral("xml"),
            QStringLiteral("/second.xml"),
            QStringLiteral("Second"),
            &error);
    ticket = *registry->requestGeneration(source, &error);
    const auto second =
            decoded(QStringLiteral("<POSITION_MARK Name=\"Second\" Type=\"0\" "
                                   "Start=\"2\" Num=\"2\"/>"));
    EXPECT_EQ(apply(second).application.outcome, ImportOutcome::Denied);
    policy.acceptPreservedConflicts = true;
    ASSERT_TRUE(apply(second).application.committed);
    EXPECT_EQ(scalar(QStringLiteral("SELECT label FROM cues")).toString(), QStringLiteral("First"));
    EXPECT_EQ(scalar(QStringLiteral("SELECT COUNT(*) FROM "
                                    "rekordbox_import_provenance_v1 WHERE "
                                    "annotation_id='cue/0'"))
                      .toInt(),
            2);
}

TEST_F(RekordboxXmlImporterTest, LocalEditsAndDeletionsSurviveSourceRefresh) {
    const auto library =
            decoded(QStringLiteral("<POSITION_MARK Name=\"Original\" "
                                   "Type=\"0\" Start=\"1\" Num=\"-1\"/>"));
    ASSERT_TRUE(apply(library).application.committed);
    exec(QStringLiteral("UPDATE cues SET label='Local'"));
    exec(QStringLiteral("UPDATE library SET title='Local title'"));
    policy.acceptPreservedConflicts = true;
    ASSERT_TRUE(apply(library, QStringLiteral("bytes-v2")).application.committed);
    EXPECT_EQ(scalar(QStringLiteral("SELECT label FROM cues")).toString(), QStringLiteral("Local"));
    exec(QStringLiteral("DELETE FROM cues"));
    ASSERT_TRUE(apply(library, QStringLiteral("bytes-v3")).application.committed);
    EXPECT_EQ(scalar(QStringLiteral("SELECT COUNT(*) FROM cues")).toInt(), 0);
}

TEST_F(RekordboxXmlImporterTest, CanonicalFailureRollsBackMediaCuesAndSuccessfulTrackRevision) {
    exec(
            QStringLiteral("CREATE TRIGGER fail_cue BEFORE INSERT ON cues "
                           "BEGIN SELECT RAISE(ABORT,'fixture failure'); END"));
    const auto library = decoded(QStringLiteral(
            "<POSITION_MARK Type=\"0\" Start=\"1\" Num=\"-1\"/>"));
    const auto failed = apply(library);
    EXPECT_FALSE(failed.application.committed);
    EXPECT_EQ(failed.application.outcome, ImportOutcome::Failed);
    EXPECT_EQ(scalar(QStringLiteral("SELECT COUNT(*) FROM library")).toInt(), 0);
    EXPECT_EQ(scalar(QStringLiteral("SELECT COUNT(*) FROM track_locations")).toInt(), 0);
    EXPECT_EQ(scalar(QStringLiteral("SELECT COUNT(*) FROM "
                                    "rekordbox_import_track_revisions_v1"))
                      .toInt(),
            0);
    exec(QStringLiteral("DROP TRIGGER fail_cue"));
    ASSERT_TRUE(apply(library).application.committed);
}

TEST_F(RekordboxXmlImporterTest, CancellationAndStaleGenerationCannotAdmitCanonicalMedia) {
    const auto library = decoded();
    int calls = 0;
    const auto cancelled = apply(library, QStringLiteral("bytes-v1"), [&] { return ++calls == 3; });
    EXPECT_FALSE(cancelled.application.committed);
    EXPECT_EQ(cancelled.application.outcome, ImportOutcome::Cancelled);
    EXPECT_EQ(scalar(QStringLiteral("SELECT COUNT(*) FROM library")).toInt(), 0);
    ASSERT_TRUE(registry->requestGeneration(source, &error));
    EXPECT_EQ(apply(library).application.outcome, ImportOutcome::Cancelled);
}

TEST_F(RekordboxXmlImporterTest, CacheAndExternalCollectionAdmissionHappenBeforeMutation) {
    const auto library = decoded();
    cacheAllowed = false;
    EXPECT_EQ(apply(library).application.outcome, ImportOutcome::Denied);
    cacheAllowed = true;
    access.externalCollectionsAbsent = false;
    EXPECT_EQ(apply(library).application.outcome, ImportOutcome::Denied);
    EXPECT_EQ(scalar(QStringLiteral("SELECT COUNT(*) FROM library")).toInt(), 0);
}

TEST_F(RekordboxXmlImporterTest, ObservedMediaSyncTimestampIsRequiredAndPersisted) {
    const auto observed = media.sourceSynchronizedAt;
    media.sourceSynchronizedAt = {};
    EXPECT_EQ(apply(decoded()).application.outcome, ImportOutcome::Unavailable);
    EXPECT_EQ(scalar(QStringLiteral("SELECT COUNT(*) FROM library")).toInt(), 0);
    media.sourceSynchronizedAt = observed;
    ASSERT_TRUE(apply(decoded()).application.committed);
    EXPECT_EQ(
            scalar(QStringLiteral("SELECT source_synchronized_ms FROM library"))
                    .toLongLong(),
            observed.toMSecsSinceEpoch());
    EXPECT_TRUE(scalar(QStringLiteral("SELECT header_parsed FROM library")).toBool());
    EXPECT_TRUE(sqlite::readGeneratedTimestamp(
            scalar(QStringLiteral("SELECT datetime_added FROM library")))
                    .isValid());
    const auto emptyDate = decoded({}, QStringLiteral("DateAdded=\"\""));
    EXPECT_EQ(
            apply(emptyDate, QStringLiteral("empty-date")).application.outcome,
            ImportOutcome::Denied);
    policy.acceptPreservedConflicts = true;
    ASSERT_TRUE(apply(emptyDate, QStringLiteral("empty-date")).application.committed);
    EXPECT_TRUE(sqlite::readGeneratedTimestamp(
            scalar(QStringLiteral("SELECT datetime_added FROM library")))
                    .isValid());
}

TEST_F(RekordboxXmlImporterTest, ExistingMediaSyncStatesArePreservedOrRefusedBeforeMutation) {
    const auto library = decoded();
    ASSERT_TRUE(apply(library).application.committed);
    const auto observed = media.sourceSynchronizedAt;
    media.sourceSynchronizedAt = observed.addSecs(1);
    EXPECT_EQ(apply(library, QStringLiteral("outdated")).application.outcome,
            ImportOutcome::Unavailable);
    EXPECT_EQ(
            scalar(QStringLiteral("SELECT source_synchronized_ms FROM library"))
                    .toLongLong(),
            observed.toMSecsSinceEpoch());
    media.sourceSynchronizedAt = observed;
    const auto ahead = observed.addSecs(5).toMSecsSinceEpoch();
    exec(QStringLiteral("UPDATE library SET source_synchronized_ms=%1").arg(ahead));
    ASSERT_TRUE(apply(library, QStringLiteral("ahead")).application.committed);
    EXPECT_EQ(
            scalar(QStringLiteral("SELECT source_synchronized_ms FROM library"))
                    .toLongLong(),
            ahead);
    exec(QStringLiteral("UPDATE library SET source_synchronized_ms=NULL,header_parsed=1"));
    ASSERT_TRUE(apply(library, QStringLiteral("legacy-unknown")).application.committed);
    EXPECT_TRUE(scalar(QStringLiteral("SELECT source_synchronized_ms FROM library")).isNull());
    exec(QStringLiteral("UPDATE library SET header_parsed=0"));
    EXPECT_FALSE(currentXmlTrackRevision(*registry,
            *library,
            0,
            QStringLiteral("id:101"),
            ticket,
            QStringLiteral("legacy-unknown"),
            policy,
            &error));
    EXPECT_EQ(apply(library, QStringLiteral("void")).application.outcome,
            ImportOutcome::Unavailable);
    EXPECT_EQ(scalar(QStringLiteral("SELECT fingerprint FROM "
                                    "rekordbox_import_track_revisions_v1"))
                      .toString(),
            QStringLiteral("legacy-unknown"));
}

TEST_F(RekordboxXmlImporterTest, ChangedMediaSyncStateAfterPreviewRejectsCanonicalWrites) {
    ASSERT_TRUE(apply(decoded()).application.committed);
    const auto changed = decoded(QStringLiteral(
            "<POSITION_MARK Type=\"0\" Start=\"1\" Num=\"-1\"/>"));
    for (const bool alterTimestamp : {false, true}) {
        exec(QStringLiteral(
                "UPDATE library SET header_parsed=1,source_synchronized_ms=%1")
                        .arg(media.sourceSynchronizedAt.toMSecsSinceEpoch()));
        const auto prepared = prepareXmlTrackImport(registry->database(),
                changed,
                0,
                QStringLiteral("id:101"),
                ticket,
                QStringLiteral("pending"),
                media,
                access);
        ASSERT_TRUE(prepared.snapshot);
        XmlCanonicalAdapter adapter(prepared.target, access);
        const auto planned = registry->plan(*prepared.snapshot, policy, adapter);
        ASSERT_TRUE(planned.plan);
        exec(alterTimestamp
                        ? QStringLiteral("UPDATE library SET "
                                         "source_synchronized_ms=source_"
                                         "synchronized_ms+1")
                        : QStringLiteral("UPDATE library SET header_parsed=0"));
        EXPECT_FALSE(registry->apply(*planned.plan, &adapter).committed);
        EXPECT_EQ(scalar(QStringLiteral("SELECT COUNT(*) FROM cues")).toInt(), 0);
        EXPECT_EQ(scalar(QStringLiteral("SELECT fingerprint FROM "
                                        "rekordbox_import_track_revisions_v1"))
                          .toString(),
                QStringLiteral("bytes-v1"));
    }
}

TEST_F(RekordboxXmlImporterTest, NonfiniteMediaAndUnserializableGridFailClosed) {
    for (const double invalidDuration : {util_double_nan(), util_double_infinity()}) {
        media.duration = invalidDuration;
        EXPECT_EQ(apply(decoded()).application.outcome, ImportOutcome::Unavailable);
        EXPECT_EQ(scalar(QStringLiteral("SELECT COUNT(*) FROM library")).toInt(), 0);
    }
    media.duration = 30;
    const auto library =
            decoded(QStringLiteral("<TEMPO Inizio=\"50000\" Bpm=\"120\" "
                                   "Metro=\"4/4\" Battito=\"1\"/>"));
    EXPECT_EQ(apply(library).application.outcome, ImportOutcome::Denied);
    EXPECT_EQ(scalar(QStringLiteral("SELECT COUNT(*) FROM library")).toInt(), 0);
    policy.acceptPreservedConflicts = true;
    ASSERT_TRUE(apply(library).application.committed);
    EXPECT_TRUE(scalar(QStringLiteral("SELECT beats FROM library")).toByteArray().isEmpty());
    EXPECT_TRUE(scalar(QStringLiteral("SELECT losses_json FROM "
                                      "rekordbox_import_track_revisions_v1"))
                    .toString()
                    .contains(QStringLiteral("grid_position_range")));
}

TEST_F(RekordboxXmlImporterTest, CacheRecheckAfterPreviewRejectsApplication) {
    const auto prepared = prepareXmlTrackImport(registry->database(),
            decoded(),
            0,
            QStringLiteral("id:101"),
            ticket,
            QStringLiteral("bytes"),
            media,
            access);
    ASSERT_TRUE(prepared.snapshot);
    XmlCanonicalAdapter adapter(prepared.target, access);
    const auto planned = registry->plan(*prepared.snapshot, policy, adapter);
    ASSERT_TRUE(planned.plan);
    cacheAllowed = false;
    const auto result = registry->apply(*planned.plan, &adapter);
    EXPECT_FALSE(result.committed);
    EXPECT_EQ(scalar(QStringLiteral("SELECT COUNT(*) FROM library")).toInt(), 0);
}

TEST_F(RekordboxXmlImporterTest, UnsupportedTempoPhaseAndMetadataProduceReviewableLosses) {
    const auto library =
            decoded(QStringLiteral("<TEMPO Inizio=\"0\" Bpm=\"120\" "
                                   "Metro=\"3/4\" Battito=\"1\"/>"),
                    QStringLiteral("Label=\"Source label\""));
    EXPECT_EQ(apply(library).application.outcome, ImportOutcome::Denied);
    EXPECT_EQ(scalar(QStringLiteral("SELECT COUNT(*) FROM library")).toInt(), 0);
    policy.acceptPreservedConflicts = true;
    const auto reviewed = apply(library);
    ASSERT_TRUE(reviewed.application.committed);
    EXPECT_EQ(reviewed.application.outcome, ImportOutcome::Warning);
    EXPECT_TRUE(scalar(QStringLiteral("SELECT beats FROM library")).toByteArray().isEmpty());
    EXPECT_FALSE(scalar(QStringLiteral("SELECT losses_json FROM "
                                       "rekordbox_import_track_revisions_v1"))
                    .toString()
                    .isEmpty());
}

TEST_F(RekordboxXmlImporterTest, AbsentEmptyAndZeroSourceFieldsRemainDistinct) {
    const auto library = decoded({}, QStringLiteral("Artist=\"\" Year=\"0\" Rating=\"0\""));
    ASSERT_TRUE(apply(library).application.committed);
    EXPECT_FALSE(scalar(QStringLiteral("SELECT artist FROM library")).isNull());
    EXPECT_TRUE(scalar(QStringLiteral("SELECT artist FROM library")).toString().isEmpty());
    EXPECT_EQ(scalar(QStringLiteral("SELECT year FROM library")).toString(), QStringLiteral("0"));
    EXPECT_EQ(scalar(QStringLiteral("SELECT rating FROM library")).toInt(), 0);
    EXPECT_TRUE(scalar(QStringLiteral("SELECT album FROM library")).isNull());
}

TEST_F(RekordboxXmlImporterTest, ExtendedMetadataRoundTripsThroughActualSchemaAndKeys) {
    const auto artwork = directory.filePath(QStringLiteral("cover image.png"));
    const auto library = decoded({},
            QStringLiteral("AlbumArtist=\"Album artist\" Composer=\"Composer\" "
                           "PlayCount=\"12\" Colour=\"#000000\" "
                           "DateAdded=\"2024-03-05T10:30:00+02:00\" "
                           "LastPlayed=\"2024-03-06T12:34:56Z\" Tonality=\"8A\" "
                           "Artwork=\"%1\"")
                    .arg(QUrl::fromLocalFile(artwork).toString(QUrl::FullyEncoded)));
    const auto imported = apply(library);
    ASSERT_TRUE(imported.application.committed);
    EXPECT_EQ(imported.application.outcome, ImportOutcome::Success);
    const auto path = registry->database().databaseName();
    registry.reset();
    registry = ImportSourceRegistry::open(path, &error);
    ASSERT_TRUE(registry);
    EXPECT_EQ(scalar(QStringLiteral("SELECT album_artist FROM library"))
                      .toString(),
            QStringLiteral("Album artist"));
    EXPECT_EQ(scalar(QStringLiteral("SELECT composer FROM library")).toString(),
            QStringLiteral("Composer"));
    EXPECT_EQ(scalar(QStringLiteral("SELECT timesplayed FROM library")).toInt(), 12);
    EXPECT_FALSE(scalar(QStringLiteral("SELECT color FROM library")).isNull());
    EXPECT_EQ(scalar(QStringLiteral("SELECT color FROM library")).toInt(), 0);
    EXPECT_EQ(scalar(QStringLiteral("SELECT datetime_added FROM library"))
                      .toString(),
            QStringLiteral("2024-03-05 08:30:00"));
    EXPECT_EQ(sqlite::readGeneratedTimestamp(scalar(
                      QStringLiteral("SELECT last_played_at FROM library"))),
            QDateTime::fromString(
                    QStringLiteral("2024-03-06T12:34:56Z"), Qt::ISODate));
    auto bytes = scalar(QStringLiteral("SELECT keys FROM library")).toByteArray();
    const auto keys = KeyFactory::loadKeysFromByteArray(
            scalar(QStringLiteral("SELECT keys_version FROM library")).toString(),
            scalar(QStringLiteral("SELECT keys_sub_version FROM library")).toString(),
            &bytes);
    EXPECT_EQ(keys.getGlobalKeyText(), QStringLiteral("8A"));
    EXPECT_EQ(keys.getGlobalKey(), mixxx::track::io::key::A_MINOR);
    EXPECT_EQ(scalar(QStringLiteral("SELECT key FROM library")).toString(),
            keys.getGlobalKeyText());
    EXPECT_EQ(scalar(QStringLiteral("SELECT key_id FROM library")).toInt(),
            static_cast<int>(keys.getGlobalKey()));
    EXPECT_EQ(scalar(QStringLiteral("SELECT tuning_frequency_hz FROM library"))
                      .toDouble(),
            keys.getGlobalTuningFrequencyHz());
    EXPECT_EQ(scalar(QStringLiteral("SELECT coverart_source FROM library")).toInt(), 2);
    EXPECT_EQ(scalar(QStringLiteral("SELECT coverart_type FROM library")).toInt(), 2);
    EXPECT_EQ(scalar(QStringLiteral("SELECT coverart_location FROM library")).toString(), artwork);
    EXPECT_TRUE(scalar(QStringLiteral("SELECT coverart_color FROM library")).isNull());
    EXPECT_TRUE(scalar(QStringLiteral("SELECT coverart_digest FROM library")).isNull());
    EXPECT_EQ(scalar(QStringLiteral("SELECT coverart_hash FROM library")).toInt(), 0);
}

TEST_F(RekordboxXmlImporterTest, CalendarDatesRequireReviewedPrecisionLossAndKeepRawProvenance) {
    const auto library = decoded({},
            QStringLiteral("DateAdded=\"2024-05-06\" "
                           "LastPlayed=\"2024-05-07T12:30:01.250Z\""));
    const auto refused = apply(library);
    EXPECT_EQ(refused.application.outcome, ImportOutcome::Denied);
    EXPECT_FALSE(refused.application.committed);
    EXPECT_EQ(scalar(QStringLiteral("SELECT COUNT(*) FROM library")).toInt(), 0);
    policy.acceptPreservedConflicts = true;
    const auto reviewed = apply(library);
    ASSERT_TRUE(reviewed.application.committed);
    EXPECT_EQ(reviewed.application.outcome, ImportOutcome::Warning);
    EXPECT_EQ(scalar(QStringLiteral("SELECT datetime_added FROM library"))
                      .toString(),
            QStringLiteral("2024-05-06 00:00:00"));
    EXPECT_EQ(scalar(QStringLiteral("SELECT last_played_at FROM library"))
                      .toString(),
            QStringLiteral("2024-05-07 12:30:01"));
    const auto provenance = scalar(
            QStringLiteral(
                    "SELECT details_json FROM rekordbox_import_provenance_v1 "
                    "WHERE annotation_id='metadata/DateAdded'"))
                                    .toString();
    EXPECT_TRUE(provenance.contains(QStringLiteral("2024-05-06")));
    const auto losses =
            scalar(QStringLiteral("SELECT losses_json FROM "
                                  "rekordbox_import_track_revisions_v1"))
                    .toString();
    EXPECT_TRUE(losses.contains(QStringLiteral("date_precision")));
    EXPECT_TRUE(losses.contains(QStringLiteral("timestamp_precision")));
}

TEST_F(RekordboxXmlImporterTest, ExtendedMetadataPreservesLocalZerosAndExplicitDeletion) {
    const auto original = decoded({},
            QStringLiteral(
                    "PlayCount=\"12\" Colour=\"#0000ff\" Tonality=\"8A\" "
                    "LastPlayed=\"2024-03-06T12:34:56Z\" "
                    "Artwork=\"/original.png\""));
    ASSERT_TRUE(apply(original).application.committed);
    exec(QStringLiteral(
            "UPDATE library SET "
            "timesplayed=0,color=0,key='',keys_version=NULL,keys_sub_version="
            "NULL,keys=NULL,key_id=0,tuning_frequency_hz=0,last_played_at=NULL,"
            "coverart_source=2,coverart_type=0,coverart_location=''"));
    const auto revised = decoded({},
            QStringLiteral(
                    "PlayCount=\"13\" Colour=\"#ff0000\" Tonality=\"9A\" "
                    "LastPlayed=\"2024-03-07T12:34:56Z\" "
                    "Artwork=\"/revised.png\""));
    EXPECT_EQ(apply(revised, QStringLiteral("changed-metadata"))
                      .application.outcome,
            ImportOutcome::Denied);
    policy.acceptPreservedConflicts = true;
    ASSERT_TRUE(apply(revised, QStringLiteral("changed-metadata")).application.committed);
    EXPECT_EQ(scalar(QStringLiteral("SELECT timesplayed FROM library")).toInt(), 0);
    EXPECT_EQ(scalar(QStringLiteral("SELECT color FROM library")).toInt(), 0);
    EXPECT_TRUE(scalar(QStringLiteral("SELECT keys FROM library")).isNull());
    EXPECT_TRUE(scalar(QStringLiteral("SELECT key FROM library")).toString().isEmpty());
    EXPECT_TRUE(scalar(QStringLiteral("SELECT last_played_at FROM library")).isNull());
    EXPECT_EQ(scalar(QStringLiteral("SELECT coverart_type FROM library")).toInt(), 0);
    EXPECT_TRUE(scalar(QStringLiteral("SELECT coverart_location FROM library"))
                    .toString()
                    .isEmpty());
}

TEST_F(RekordboxXmlImporterTest, UnsupportedMetadataIsRetainedWithoutInventedCanonicalMappings) {
    const auto library = decoded({},
            QStringLiteral("Label=\"Label\" Remixer=\"Remixer\" "
                           "DiscNumber=\"2\" DateModified=\"2024-03-07\" "
                           "DateAdded=\"2024-03-07T12:30:00\" Colour=\"3\" "
                           "Artwork=\"relative/image.png\""));
    const auto prepared = prepareXmlTrackImport(registry->database(),
            library,
            0,
            QStringLiteral("id:101"),
            ticket,
            QStringLiteral("bytes"),
            media,
            access);
    ASSERT_TRUE(prepared.snapshot);
    EXPECT_EQ(prepared.outcome, ImportOutcome::Warning);
    QSet<QString> unsupported;
    for (const auto& issue : prepared.issues) {
        unsupported.insert(issue.annotationId);
    }
    for (const auto& field : {QStringLiteral("Label"),
                 QStringLiteral("Remixer"),
                 QStringLiteral("DiscNumber"),
                 QStringLiteral("DateModified"),
                 QStringLiteral("DateAdded"),
                 QStringLiteral("Colour"),
                 QStringLiteral("Artwork")}) {
        EXPECT_TRUE(unsupported.contains(field)) << field.toStdString();
    }
    EXPECT_EQ(apply(library).application.outcome, ImportOutcome::Denied);
    policy.acceptPreservedConflicts = true;
    ASSERT_TRUE(apply(library).application.committed);
    EXPECT_TRUE(scalar(QStringLiteral("SELECT color FROM library")).isNull());
    EXPECT_EQ(scalar(QStringLiteral("SELECT coverart_source FROM library")).toInt(), 0);
    const auto provenance = scalar(
            QStringLiteral(
                    "SELECT details_json FROM rekordbox_import_provenance_v1 "
                    "WHERE annotation_id='media'"))
                                    .toString();
    EXPECT_TRUE(provenance.contains(QStringLiteral("Label")));
    EXPECT_TRUE(provenance.contains(QStringLiteral("Remixer")));
    EXPECT_TRUE(provenance.contains(QStringLiteral("DiscNumber")));
    EXPECT_TRUE(provenance.contains(QStringLiteral("DateModified")));
}

TEST_F(RekordboxXmlImporterTest, InconsistentKeySerializationRollsBackCanonicalAdmission) {
    auto prepared = prepareXmlTrackImport(registry->database(),
            decoded({}, QStringLiteral("Tonality=\"8A\"")),
            0,
            QStringLiteral("id:101"),
            ticket,
            QStringLiteral("bytes"),
            media,
            access);
    ASSERT_TRUE(prepared.snapshot);
    for (auto& field : prepared.snapshot->fields) {
        if (field.targetField == QLatin1String("keys")) {
            auto value = field.value.toObject();
            value.insert(QStringLiteral("id"), static_cast<int>(mixxx::track::io::key::C_MAJOR));
            field.value = value;
        }
    }
    XmlCanonicalAdapter adapter(prepared.target, access);
    const auto planned = registry->plan(*prepared.snapshot, policy, adapter);
    ASSERT_TRUE(planned.plan);
    EXPECT_FALSE(registry->apply(*planned.plan, &adapter).committed);
    EXPECT_EQ(scalar(QStringLiteral("SELECT COUNT(*) FROM library")).toInt(), 0);
    EXPECT_EQ(scalar(QStringLiteral("SELECT COUNT(*) FROM track_locations")).toInt(), 0);
    EXPECT_EQ(scalar(QStringLiteral("SELECT COUNT(*) FROM "
                                    "rekordbox_import_track_revisions_v1"))
                      .toInt(),
            0);
}

TEST_F(RekordboxXmlImporterTest, BorrowedRegistryPreservesConnectionAndForeignKeyPolicy) {
    const auto connection = registry->database();
    EXPECT_EQ(scalar(QStringLiteral("PRAGMA foreign_keys")).toInt(), 0);
    auto attached = ImportSourceRegistry::attach(connection, &error);
    ASSERT_TRUE(attached) << error.toStdString();
    attached.reset();
    EXPECT_TRUE(connection.isOpen());
    EXPECT_EQ(scalar(QStringLiteral("PRAGMA foreign_keys")).toInt(), 0);
}

TEST_F(RekordboxXmlImporterTest, OldOrMalformedSerializedRowsCannotEnterSchemaTwoConsumer) {
    EXPECT_EQ(validateXmlTrackAnnotations(QByteArray("{\"beatgrid\":[]}"))
                      .outcome,
            ImportOutcome::Unsupported);
    EXPECT_EQ(validateXmlTrackAnnotations(QByteArray("<xml>")).outcome, ImportOutcome::Invalid);
    const auto library = decoded();
    EXPECT_EQ(validateXmlTrackAnnotations(
                      serializeTrackAnnotations(library->tracks[0]))
                      .outcome,
            ImportOutcome::Success);
}

TEST_F(RekordboxXmlImporterTest,
        CurrentRevisionAllowsCachedLocalEditsButNotChangedDiagnosticsOrPolicy) {
    const auto library = decoded();
    const auto first = apply(library);
    ASSERT_TRUE(first.application.committed);
    exec(QStringLiteral("UPDATE library SET title='Local edit'"));
    cacheAllowed = false;
    const auto current = currentXmlTrackRevision(*registry,
            *library,
            0,
            QStringLiteral("id:101"),
            ticket,
            QStringLiteral("bytes-v1"),
            policy,
            &error);
    ASSERT_TRUE(current) << error.toStdString();
    EXPECT_EQ(*current, first.canonicalTrackId);
    auto changed = *library;
    changed.diagnostics.push_back({Diagnostic::Severity::Warning,
            QStringLiteral("New limitation"),
            1,
            2});
    EXPECT_FALSE(currentXmlTrackRevision(*registry,
            changed,
            0,
            QStringLiteral("id:101"),
            ticket,
            QStringLiteral("bytes-v1"),
            policy,
            &error));
    auto newPolicy = policy;
    newPolicy.revision = QStringLiteral("new-policy");
    EXPECT_FALSE(currentXmlTrackRevision(*registry,
            *library,
            0,
            QStringLiteral("id:101"),
            ticket,
            QStringLiteral("bytes-v1"),
            newPolicy,
            &error));
    EXPECT_EQ(scalar(QStringLiteral("SELECT title FROM library")).toString(),
            QStringLiteral("Local edit"));
}

TEST_F(RekordboxXmlImporterTest,
        LocationKeyTracksWithoutIdsKeepRepeatedMembershipAndSelectedIdentity) {
    QByteArray xml =
            (QStringLiteral(
                     "<DJ_PLAYLISTS Version=\"1.0.0\"><COLLECTION><TRACK "
                     "Location=\"") +
                    media.location +
                    QStringLiteral("\" Name=\"No ID\"/></COLLECTION><PLAYLISTS>"
                                   "<NODE Type=\"1\" Name=\"Repeat\" "
                                   "KeyType=\"1\"><TRACK Key=\"") +
                    media.location + QStringLiteral("\"/><TRACK Key=\"") +
                    media.location +
                    QStringLiteral("\"/></NODE></PLAYLISTS></DJ_PLAYLISTS>"))
                    .toUtf8();
    QBuffer buffer(&xml);
    buffer.open(QIODevice::ReadOnly);
    const auto library = decodeXml(&buffer).snapshot;
    ASSERT_TRUE(library);
    ASSERT_FALSE(library->tracks[0].hasValidId);
    ASSERT_EQ(library->playlists.children[0].entries.size(), 2);
    ASSERT_EQ(library->playlists.children[0].entries[0].trackIndex, 0);
    ASSERT_EQ(library->playlists.children[0].entries[1].trackIndex, 0);
    const auto result = importXmlTrack(registry.get(),
            library,
            0,
            QStringLiteral("location:opaque-source-key"),
            ticket,
            QStringLiteral("idless-bytes"),
            media,
            policy,
            access);
    ASSERT_TRUE(result.application.committed);
    EXPECT_EQ(scalar(QStringLiteral("SELECT source_track_id FROM "
                                    "rekordbox_import_tracks_v1"))
                      .toString(),
            QStringLiteral("location:opaque-source-key"));
    EXPECT_EQ(library->playlists.children[0].entries.size(), 2);
}

TEST_F(RekordboxXmlImporterTest, RemovingSourceCueRemovesOnlyItsUneditedOwnedCanonicalCue) {
    const auto library = decoded(QStringLiteral(
            "<POSITION_MARK Type=\"0\" Start=\"1\" Num=\"-1\"/>"));
    ASSERT_TRUE(apply(library).application.committed);
    ASSERT_TRUE(apply(decoded(), QStringLiteral("removed-cue")).application.committed);
    EXPECT_EQ(scalar(QStringLiteral("SELECT COUNT(*) FROM cues")).toInt(), 0);
    EXPECT_EQ(scalar(QStringLiteral("SELECT COUNT(*) FROM library")).toInt(), 1);
}

TEST_F(RekordboxXmlImporterTest, ReservedIdentityCollisionRollsBackInsteadOfRetargeting) {
    const auto prepared = prepareXmlTrackImport(registry->database(),
            decoded(),
            0,
            QStringLiteral("id:101"),
            ticket,
            QStringLiteral("bytes"),
            media,
            access);
    ASSERT_TRUE(prepared.snapshot);
    XmlCanonicalAdapter adapter(prepared.target, access);
    const auto plan = registry->plan(*prepared.snapshot, policy, adapter);
    ASSERT_TRUE(plan.plan);
    exec(QStringLiteral("INSERT INTO track_locations(id,location) VALUES(1,'/different.wav')"));
    exec(QStringLiteral(
            "INSERT INTO library(id,location,samplerate,channels) "
            "VALUES(1,1,48000,6)"));
    EXPECT_FALSE(registry->apply(*plan.plan, &adapter).committed);
    EXPECT_EQ(scalar(QStringLiteral("SELECT COUNT(*) FROM library")).toInt(), 1);
    EXPECT_EQ(scalar(QStringLiteral("SELECT COUNT(*) FROM rekordbox_import_tracks_v1")).toInt(), 0);
}

TEST_F(RekordboxXmlImporterTest, DisplayOnlyOrdinalIdentityIsUnsupportedBeforeAdmission) {
    const auto prepared = prepareXmlTrackImport(registry->database(),
            decoded(),
            0,
            QStringLiteral("record:0"),
            ticket,
            QStringLiteral("bytes"),
            media,
            access);
    EXPECT_EQ(prepared.outcome, ImportOutcome::Unsupported);
    EXPECT_FALSE(prepared.snapshot);
    EXPECT_EQ(scalar(QStringLiteral("SELECT COUNT(*) FROM library")).toInt(), 0);
}

} // namespace
// End of code prepared by ChatGPT for human review and submission.
