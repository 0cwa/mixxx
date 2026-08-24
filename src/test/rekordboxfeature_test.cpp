#include <gtest/gtest.h>

#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QTemporaryDir>

#include "library/rekordbox/rekordboximport.h"
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
    EXPECT_EQ(mixxx::audio::FramePos(30), first->getPosition());
    EXPECT_EQ(QStringLiteral("updated"), first->getLabel());
    EXPECT_EQ(mixxx::audio::FramePos(20), duplicate->getPosition());
    EXPECT_TRUE(duplicate->getLabel().isEmpty());
}

} // namespace
