#include <gtest/gtest.h>

#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "library/queryutil.h"
#include "library/rekordbox/rekordboximport.h"
#include "library/rekordbox/rekordboxparser_test.h"
#include "rekordbox_test_fixtures.h"
#include "track/beats.h"
#include "track/cue.h"
#include "track/track.h"

namespace {

TEST(RekordboxImportTest, RejectsInvalidDatabaseIds) {
    EXPECT_FALSE(mixxx::rekordbox::isValidDatabaseId(-1));
    EXPECT_FALSE(mixxx::rekordbox::isValidDatabaseId(0));
    EXPECT_TRUE(mixxx::rekordbox::isValidDatabaseId(1));
}

TEST(RekordboxImportTest, RequiresWritableDestinationDatabase) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());

    const QString databasePath = temporaryDirectory.filePath("mixxxdb.sqlite");
    const QString connectionName = QStringLiteral("rekordbox-import-test");
    bool permissionCheckSkipped = false;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", connectionName);
        database.setDatabaseName(databasePath);
        ASSERT_TRUE(database.open());
        EXPECT_TRUE(mixxx::rekordbox::isWritableDatabase(database));

        database.close();
        ASSERT_TRUE(QFile::setPermissions(
                databasePath,
                QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther));
        ASSERT_TRUE(database.open());
        permissionCheckSkipped = QFileInfo(databasePath).isWritable();
        if (!permissionCheckSkipped) {
            EXPECT_FALSE(mixxx::rekordbox::isWritableDatabase(database));
        }

        database.close();
    }
    QSqlDatabase::removeDatabase(connectionName);
    if (permissionCheckSkipped) {
        GTEST_SKIP() << "The test runner can write files without write permission bits";
    }
}

TEST(RekordboxImportTest, SkipsDanglingPlaylistTrackReferences) {
    const QString connectionName = QStringLiteral("rekordbox-playlist-import-test");
    {
        QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", connectionName);
        database.setDatabaseName(QStringLiteral(":memory:"));
        ASSERT_TRUE(database.open());

        QSqlQuery setupQuery(database);
        ASSERT_TRUE(setupQuery.exec(
                "CREATE TABLE rekordbox_library (id INTEGER, rb_id INTEGER, device TEXT)"));
        ASSERT_TRUE(setupQuery.exec(
                "CREATE TABLE rekordbox_playlist_tracks "
                "(playlist_id INTEGER, track_id INTEGER, position INTEGER)"));
        ASSERT_TRUE(setupQuery.exec(
                "INSERT INTO rekordbox_library (id, rb_id, device) "
                "VALUES (7, 100, 'USB'), (0, 101, 'USB'), (8, 102, 'USB')"));

        ASSERT_TRUE(database.transaction());
        const QMap<uint32_t, uint32_t> playlistTracks{
                {1, 100},
                {2, 999},
                {3, 102},
                {4, 101}};
        ASSERT_TRUE(mixxx::rekordbox::importPlaylistTracks(
                database, 42, playlistTracks, QStringLiteral("USB")));

        QSqlQuery resultQuery(database);
        ASSERT_TRUE(resultQuery.exec(
                "SELECT playlist_id, track_id, position "
                "FROM rekordbox_playlist_tracks ORDER BY position"));
        ASSERT_TRUE(resultQuery.next());
        EXPECT_EQ(42, resultQuery.value(0).toInt());
        EXPECT_EQ(7, resultQuery.value(1).toInt());
        EXPECT_EQ(1, resultQuery.value(2).toInt());
        ASSERT_TRUE(resultQuery.next());
        EXPECT_EQ(42, resultQuery.value(0).toInt());
        EXPECT_EQ(8, resultQuery.value(1).toInt());
        EXPECT_EQ(2, resultQuery.value(2).toInt());
        EXPECT_FALSE(resultQuery.next());

        QSqlQuery invalidRelationQuery(database);
        ASSERT_TRUE(invalidRelationQuery.exec(
                "SELECT COUNT(*) FROM rekordbox_playlist_tracks "
                "WHERE track_id <= 0"));
        ASSERT_TRUE(invalidRelationQuery.next());
        EXPECT_EQ(0, invalidRelationQuery.value(0).toInt());
        ASSERT_TRUE(database.commit());

        database.close();
    }
    QSqlDatabase::removeDatabase(connectionName);
}

TEST(RekordboxImportTest, RollsBackFatalPlaylistInsertErrors) {
    const QString connectionName = QStringLiteral("rekordbox-playlist-rollback-test");
    {
        QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", connectionName);
        database.setDatabaseName(QStringLiteral(":memory:"));
        ASSERT_TRUE(database.open());

        QSqlQuery setupQuery(database);
        ASSERT_TRUE(setupQuery.exec(
                "CREATE TABLE rekordbox_library (id INTEGER, rb_id INTEGER, device TEXT)"));
        ASSERT_TRUE(setupQuery.exec(
                "CREATE TABLE rekordbox_playlist_tracks "
                "(playlist_id INTEGER, track_id INTEGER CHECK(track_id <> 8), position INTEGER)"));
        ASSERT_TRUE(setupQuery.exec(
                "INSERT INTO rekordbox_library (id, rb_id, device) "
                "VALUES (7, 100, 'USB'), (8, 102, 'USB')"));

        {
            ScopedTransaction transaction(database);
            ASSERT_TRUE(transaction.active());
            const QMap<uint32_t, uint32_t> playlistTracks{
                    {1, 100},
                    {2, 102}};
            EXPECT_FALSE(mixxx::rekordbox::importPlaylistTracks(
                    database, 42, playlistTracks, QStringLiteral("USB")));
        }

        QSqlQuery resultQuery(database);
        ASSERT_TRUE(resultQuery.exec(
                "SELECT COUNT(*) FROM rekordbox_playlist_tracks"));
        ASSERT_TRUE(resultQuery.next());
        EXPECT_EQ(0, resultQuery.value(0).toInt());

        database.close();
    }
    QSqlDatabase::removeDatabase(connectionName);
}

TEST(RekordboxImportTest, PreservesMemoryLoopBoundsAndCueOrder) {
    TrackPointer track = Track::newTemporary();
    CuePointer hotCue = track->createAndAddCue(
            mixxx::CueType::HotCue,
            1,
            mixxx::audio::FramePos(10),
            mixxx::audio::kInvalidFramePos);

    mixxx::rekordbox::importMemoryCue(track,
            mixxx::audio::FramePos(100),
            mixxx::audio::FramePos(200),
            QStringLiteral("first loop"),
            mixxx::RgbColor(0x102030));
    mixxx::rekordbox::importMemoryCue(track,
            mixxx::audio::FramePos(100),
            mixxx::audio::FramePos(300),
            QStringLiteral("second loop"),
            mixxx::RgbColor(0x405060));

    const QList<CuePointer> cuePoints = track->getCuePoints();
    ASSERT_EQ(3, cuePoints.size());
    EXPECT_EQ(hotCue, cuePoints.at(0));
    EXPECT_EQ(mixxx::audio::FramePos(10), hotCue->getPosition());

    EXPECT_EQ(mixxx::CueType::Loop, cuePoints.at(1)->getType());
    EXPECT_EQ(Cue::kNoHotCue, cuePoints.at(1)->getHotCue());
    EXPECT_EQ(mixxx::audio::FramePos(100), cuePoints.at(1)->getPosition());
    EXPECT_EQ(mixxx::audio::FramePos(200), cuePoints.at(1)->getEndPosition());
    EXPECT_EQ(QStringLiteral("first loop"), cuePoints.at(1)->getLabel());
    EXPECT_EQ(mixxx::RgbColor(0x102030), cuePoints.at(1)->getColor());

    EXPECT_EQ(mixxx::CueType::Loop, cuePoints.at(2)->getType());
    EXPECT_EQ(Cue::kNoHotCue, cuePoints.at(2)->getHotCue());
    EXPECT_EQ(mixxx::audio::FramePos(100), cuePoints.at(2)->getPosition());
    EXPECT_EQ(mixxx::audio::FramePos(300), cuePoints.at(2)->getEndPosition());
    EXPECT_EQ(QStringLiteral("second loop"), cuePoints.at(2)->getLabel());
    EXPECT_EQ(mixxx::RgbColor(0x405060), cuePoints.at(2)->getColor());
}

TEST(RekordboxImportTest, UpdatesFirstMatchingHotCueWithoutReordering) {
    TrackPointer track = Track::newTemporary();
    CuePointer first = track->createAndAddCue(
            mixxx::CueType::HotCue,
            2,
            mixxx::audio::FramePos(10),
            mixxx::audio::kInvalidFramePos);
    CuePointer duplicate = track->createAndAddCue(
            mixxx::CueType::HotCue,
            2,
            mixxx::audio::FramePos(20),
            mixxx::audio::kInvalidFramePos);

    mixxx::rekordbox::importHotCue(track,
            mixxx::audio::FramePos(30),
            mixxx::audio::kInvalidFramePos,
            2,
            QStringLiteral("updated"),
            mixxx::RgbColor(0x102030));

    const QList<CuePointer> cuePoints = track->getCuePoints();
    ASSERT_EQ(2, cuePoints.size());
    EXPECT_EQ(first, cuePoints.at(0));
    EXPECT_EQ(duplicate, cuePoints.at(1));
    EXPECT_EQ(mixxx::audio::FramePos(30), first->getPosition());
    EXPECT_EQ(QStringLiteral("updated"), first->getLabel());
    EXPECT_EQ(mixxx::RgbColor(0x102030), first->getColor());
    EXPECT_EQ(mixxx::audio::FramePos(20), duplicate->getPosition());
    EXPECT_TRUE(duplicate->getLabel().isEmpty());
}

TEST(RekordboxImportTest, ReadsSyntheticBeatGridThroughProductionPath) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());

    const QString anlzPath = temporaryDirectory.filePath("synthetic.dat");
    QFile anlzFile(anlzPath);
    ASSERT_TRUE(anlzFile.open(QIODevice::WriteOnly));
    const QByteArray fixture = mixxx::rekordbox::test::makeAnlzBeatGridFixture();
    ASSERT_EQ(fixture.size(), anlzFile.write(fixture));
    anlzFile.close();

    TrackPointer track = Track::newTemporary();
    const auto sampleRate = mixxx::audio::SampleRate(48000);
    mixxx::rekordbox::test::readAnalyzeForTest(
            track, sampleRate, 0, true, anlzPath);

    const auto beats = track->getBeats();
    ASSERT_TRUE(beats);
    EXPECT_EQ(sampleRate, beats->getSampleRate());
    EXPECT_EQ(mixxx::audio::FramePos(48000),
            beats->findNextBeat(mixxx::audio::FramePos(48000)));
    EXPECT_EQ(mixxx::audio::FramePos(72000),
            beats->findNextBeat(mixxx::audio::FramePos(48001)));
    EXPECT_DOUBLE_EQ(120.0,
            beats->getBpmInRange(
                         mixxx::audio::FramePos(48000),
                         mixxx::audio::FramePos(72000))
                    .value());
}

TEST(RekordboxImportTest, SkipsTruncatedSyntheticBeatGrid) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());

    const QString anlzPath = temporaryDirectory.filePath("truncated.dat");
    QFile anlzFile(anlzPath);
    ASSERT_TRUE(anlzFile.open(QIODevice::WriteOnly));
    QByteArray fixture = mixxx::rekordbox::test::makeAnlzBeatGridFixture();
    fixture.chop(1);
    ASSERT_EQ(fixture.size(), anlzFile.write(fixture));
    anlzFile.close();

    const auto sampleRate = mixxx::audio::SampleRate(48000);
    for (const bool ignoreCues : {true, false}) {
        SCOPED_TRACE(ignoreCues ? "ignoreCues=true" : "ignoreCues=false");

        TrackPointer seededTrack = Track::newTemporary();
        const QVector<mixxx::audio::FramePos> seedPositions{
                mixxx::audio::FramePos(48000),
                mixxx::audio::FramePos(72000)};
        const auto seededBeats = mixxx::Beats::fromBeatPositions(sampleRate, seedPositions);
        ASSERT_TRUE(seededBeats);
        ASSERT_TRUE(seededTrack->trySetBeats(seededBeats));
        const CuePointer seededCue = seededTrack->createAndAddCue(
                mixxx::CueType::HotCue,
                1,
                mixxx::audio::FramePos(123),
                mixxx::audio::kInvalidFramePos);

        mixxx::rekordbox::test::readAnalyzeForTest(
                seededTrack, sampleRate, 0, ignoreCues, anlzPath);

        EXPECT_EQ(seededBeats, seededTrack->getBeats());
        ASSERT_EQ(1, seededTrack->getCuePoints().size());
        EXPECT_EQ(seededCue, seededTrack->getCuePoints().at(0));

        TrackPointer freshTrack = Track::newTemporary();
        mixxx::rekordbox::test::readAnalyzeForTest(
                freshTrack, sampleRate, 0, ignoreCues, anlzPath);

        EXPECT_FALSE(freshTrack->getBeats());
    }
}

} // namespace
