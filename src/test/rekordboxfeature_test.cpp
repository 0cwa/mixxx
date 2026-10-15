#include <gtest/gtest.h>

#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "library/queryutil.h"
#include "library/rekordbox/rekordboxcolors.h"
#include "library/rekordbox/rekordboximport.h"
#include "library/rekordbox/rekordboxparser_test.h"
#include "rekordbox_test_fixtures.h"
#include "track/cue.h"
#include "track/track.h"

namespace {

QString writeAnlzFixture(
        QTemporaryDir& temporaryDirectory,
        const QString& filename,
        const QByteArray& fixture) {
    const QString path = temporaryDirectory.filePath(filename);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(fixture) != fixture.size()) {
        return {};
    }
    return path;
}

TEST(RekordboxImportTest, RejectsInvalidDatabaseIds) {
    EXPECT_FALSE(mixxx::rekordbox::isValidDatabaseId(-1));
    EXPECT_FALSE(mixxx::rekordbox::isValidDatabaseId(0));
    EXPECT_TRUE(mixxx::rekordbox::isValidDatabaseId(1));
}

TEST(RekordboxImportTest, ColorIDTwoMapsToHistoricalRed) {
    EXPECT_EQ(mixxx::RgbColor(0xF80000),
            mixxx::rekordbox::detail::colorFromID(2));
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

    EXPECT_EQ(mixxx::CueType::Loop, cuePoints.at(2)->getType());
    EXPECT_EQ(Cue::kNoHotCue, cuePoints.at(2)->getHotCue());
    EXPECT_EQ(mixxx::audio::FramePos(100), cuePoints.at(2)->getPosition());
    EXPECT_EQ(mixxx::audio::FramePos(300), cuePoints.at(2)->getEndPosition());
    EXPECT_EQ(QStringLiteral("second loop"), cuePoints.at(2)->getLabel());
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
    EXPECT_EQ(mixxx::CueType::HotCue, first->getType());
    EXPECT_EQ(mixxx::audio::FramePos(30), first->getPosition());
    EXPECT_EQ(mixxx::audio::kInvalidFramePos, first->getEndPosition());
    EXPECT_EQ(QStringLiteral("updated"), first->getLabel());
    EXPECT_EQ(mixxx::audio::FramePos(20), duplicate->getPosition());
    EXPECT_TRUE(duplicate->getLabel().isEmpty());

    mixxx::rekordbox::importHotCue(track,
            mixxx::audio::FramePos(40),
            mixxx::audio::FramePos(60),
            2,
            QStringLiteral("updated loop"),
            mixxx::RgbColor(0x405060));

    const QList<CuePointer> loopCuePoints = track->getCuePoints();
    ASSERT_EQ(2, loopCuePoints.size());
    EXPECT_EQ(first, loopCuePoints.at(0));
    EXPECT_EQ(duplicate, loopCuePoints.at(1));
    EXPECT_EQ(mixxx::CueType::Loop, first->getType());
    EXPECT_EQ(mixxx::audio::FramePos(40), first->getPosition());
    EXPECT_EQ(mixxx::audio::FramePos(60), first->getEndPosition());

    mixxx::rekordbox::importHotCue(track,
            mixxx::audio::FramePos(70),
            mixxx::audio::kInvalidFramePos,
            2,
            QStringLiteral("updated hot cue"),
            mixxx::RgbColor(0x102030));

    const QList<CuePointer> hotCuePoints = track->getCuePoints();
    ASSERT_EQ(2, hotCuePoints.size());
    EXPECT_EQ(first, hotCuePoints.at(0));
    EXPECT_EQ(duplicate, hotCuePoints.at(1));
    EXPECT_EQ(mixxx::CueType::HotCue, first->getType());
    EXPECT_EQ(mixxx::audio::FramePos(70), first->getPosition());
    EXPECT_EQ(mixxx::audio::kInvalidFramePos, first->getEndPosition());
}

TEST(RekordboxImportTest, ImportsLegacyAndExtendedHotCueLoops) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());

    const auto sampleRate = mixxx::audio::SampleRate(48000);
    for (const bool extended : {false, true}) {
        SCOPED_TRACE(extended ? "extended" : "legacy");
        const QString anlzPath = writeAnlzFixture(
                temporaryDirectory,
                extended ? QStringLiteral("hot-loop-extended.dat")
                         : QStringLiteral("hot-loop-legacy.dat"),
                mixxx::rekordbox::test::makeAnlzCueFixture(
                        extended, 1, 1, 2));
        ASSERT_FALSE(anlzPath.isEmpty());

        TrackPointer track = Track::newTemporary();
        mixxx::rekordbox::test::readAnalyzeForTest(
                track, sampleRate, 0, false, anlzPath);

        const CuePointer cue = track->findHotcueByIndex(0);
        ASSERT_TRUE(cue);
        EXPECT_EQ(mixxx::CueType::Loop, cue->getType());
        EXPECT_EQ(0, cue->getHotCue());
        EXPECT_EQ(mixxx::audio::FramePos(48000), cue->getPosition());
        EXPECT_EQ(mixxx::audio::FramePos(96000), cue->getEndPosition());
    }
}

TEST(RekordboxImportTest, AppliesTimingOffsetToHotCueLoopEnd) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());

    const QString anlzPath = writeAnlzFixture(
            temporaryDirectory,
            QStringLiteral("hot-loop-offset.dat"),
            mixxx::rekordbox::test::makeAnlzCueFixture(true, 1, 1, 2));
    ASSERT_FALSE(anlzPath.isEmpty());

    TrackPointer track = Track::newTemporary();
    mixxx::rekordbox::test::readAnalyzeForTest(
            track, mixxx::audio::SampleRate(48000), 100, false, anlzPath);

    const CuePointer cue = track->findHotcueByIndex(0);
    ASSERT_TRUE(cue);
    EXPECT_EQ(mixxx::CueType::Loop, cue->getType());
    EXPECT_EQ(mixxx::audio::FramePos(43200), cue->getPosition());
    EXPECT_EQ(mixxx::audio::FramePos(91200), cue->getEndPosition());
}

TEST(RekordboxImportTest, KeepsLegacyAndExtendedPlainHotCuesUnlooped) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());

    const auto sampleRate = mixxx::audio::SampleRate(48000);
    for (const bool extended : {false, true}) {
        SCOPED_TRACE(extended ? "extended" : "legacy");
        const QString anlzPath = writeAnlzFixture(
                temporaryDirectory,
                extended ? QStringLiteral("hot-cue-extended.dat")
                         : QStringLiteral("hot-cue-legacy.dat"),
                mixxx::rekordbox::test::makeAnlzCueFixture(
                        extended, 1, 1, 1));
        ASSERT_FALSE(anlzPath.isEmpty());

        TrackPointer track = Track::newTemporary();
        mixxx::rekordbox::test::readAnalyzeForTest(
                track, sampleRate, 0, false, anlzPath);

        const CuePointer cue = track->findHotcueByIndex(0);
        ASSERT_TRUE(cue);
        EXPECT_EQ(mixxx::CueType::HotCue, cue->getType());
        EXPECT_EQ(mixxx::audio::FramePos(48000), cue->getPosition());
        EXPECT_EQ(mixxx::audio::kInvalidFramePos, cue->getEndPosition());
    }
}

TEST(RekordboxImportTest, ReimportsHotCueLoopBoundsOverExistingCue) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());

    const QString loopPath = writeAnlzFixture(
            temporaryDirectory,
            QStringLiteral("reimport-loop.dat"),
            mixxx::rekordbox::test::makeAnlzCueFixture(true, 1, 1, 2));
    const QString hotCuePath = writeAnlzFixture(
            temporaryDirectory,
            QStringLiteral("reimport-hot-cue.dat"),
            mixxx::rekordbox::test::makeAnlzCueFixture(true, 1, 1, 1));
    ASSERT_FALSE(loopPath.isEmpty());
    ASSERT_FALSE(hotCuePath.isEmpty());

    TrackPointer track = Track::newTemporary();
    CuePointer existingCue = track->createAndAddCue(
            mixxx::CueType::HotCue,
            0,
            mixxx::audio::FramePos(10),
            mixxx::audio::kInvalidFramePos);

    mixxx::rekordbox::test::readAnalyzeForTest(
            track, mixxx::audio::SampleRate(48000), 0, false, loopPath);

    EXPECT_EQ(existingCue, track->findHotcueByIndex(0));
    EXPECT_EQ(mixxx::CueType::Loop, existingCue->getType());
    EXPECT_EQ(mixxx::audio::FramePos(48000), existingCue->getPosition());
    EXPECT_EQ(mixxx::audio::FramePos(96000), existingCue->getEndPosition());

    mixxx::rekordbox::test::readAnalyzeForTest(
            track, mixxx::audio::SampleRate(48000), 0, false, hotCuePath);

    EXPECT_EQ(existingCue, track->findHotcueByIndex(0));
    EXPECT_EQ(mixxx::CueType::HotCue, existingCue->getType());
    EXPECT_EQ(mixxx::audio::FramePos(48000), existingCue->getPosition());
    EXPECT_EQ(mixxx::audio::kInvalidFramePos, existingCue->getEndPosition());
}

TEST(RekordboxImportTest, SkipsZeroHotCueNumberAndStillUpdatesNonzeroCueType) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());

    const auto sampleRate = mixxx::audio::SampleRate(48000);
    for (const bool extended : {false, true}) {
        SCOPED_TRACE(extended ? "extended" : "legacy");
        TrackPointer track = Track::newTemporary();
        CuePointer mainCue = track->createAndAddCue(
                mixxx::CueType::MainCue,
                Cue::kNoHotCue,
                mixxx::audio::FramePos(10),
                mixxx::audio::kInvalidFramePos);
        mainCue->setLabel(QStringLiteral("main cue"));

        const QString zeroHotCuePath = writeAnlzFixture(
                temporaryDirectory,
                extended ? QStringLiteral("zero-hot-cue-extended.dat")
                         : QStringLiteral("zero-hot-cue-legacy.dat"),
                mixxx::rekordbox::test::makeAnlzCueFixture(extended, 1, 0, 2));
        ASSERT_FALSE(zeroHotCuePath.isEmpty());

        mixxx::rekordbox::test::readAnalyzeForTest(
                track, sampleRate, 0, false, zeroHotCuePath);

        EXPECT_EQ(mixxx::CueType::MainCue, mainCue->getType());
        EXPECT_EQ(Cue::kNoHotCue, mainCue->getHotCue());
        EXPECT_EQ(mixxx::audio::FramePos(10), mainCue->getPosition());
        EXPECT_EQ(mixxx::audio::kInvalidFramePos, mainCue->getEndPosition());
        EXPECT_EQ(QStringLiteral("main cue"), mainCue->getLabel());

        CuePointer existingHotCue = track->createAndAddCue(
                mixxx::CueType::HotCue,
                0,
                mixxx::audio::FramePos(20),
                mixxx::audio::kInvalidFramePos);
        const QString loopPath = writeAnlzFixture(
                temporaryDirectory,
                extended ? QStringLiteral("reimport-loop-extended.dat")
                         : QStringLiteral("reimport-loop-legacy.dat"),
                mixxx::rekordbox::test::makeAnlzCueFixture(extended, 1, 1, 2));
        ASSERT_FALSE(loopPath.isEmpty());

        mixxx::rekordbox::test::readAnalyzeForTest(
                track, sampleRate, 0, false, loopPath);

        EXPECT_EQ(existingHotCue, track->findHotcueByIndex(0));
        EXPECT_EQ(mixxx::CueType::Loop, existingHotCue->getType());
        EXPECT_EQ(mixxx::audio::FramePos(48000), existingHotCue->getPosition());
        EXPECT_EQ(mixxx::audio::FramePos(96000), existingHotCue->getEndPosition());

        const QString hotCuePath = writeAnlzFixture(
                temporaryDirectory,
                extended ? QStringLiteral("reimport-hot-cue-extended.dat")
                         : QStringLiteral("reimport-hot-cue-legacy.dat"),
                mixxx::rekordbox::test::makeAnlzCueFixture(extended, 1, 1, 1));
        ASSERT_FALSE(hotCuePath.isEmpty());

        mixxx::rekordbox::test::readAnalyzeForTest(
                track, sampleRate, 0, false, hotCuePath);

        EXPECT_EQ(existingHotCue, track->findHotcueByIndex(0));
        EXPECT_EQ(mixxx::CueType::HotCue, existingHotCue->getType());
        EXPECT_EQ(mixxx::audio::FramePos(48000), existingHotCue->getPosition());
        EXPECT_EQ(mixxx::audio::kInvalidFramePos, existingHotCue->getEndPosition());
        EXPECT_EQ(mixxx::CueType::MainCue, mainCue->getType());
        EXPECT_EQ(mixxx::audio::FramePos(10), mainCue->getPosition());
    }
}

} // namespace
