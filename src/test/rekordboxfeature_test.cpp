#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <exception>
#include <limits>
#include <string>
#include <thread>

#include "database/mixxxdb.h"
#include "kaitai/exceptions.h"
#include "library/rekordbox/rekordboximport.h"
#include "library/rekordbox/rekordboxparser_test.h"
#include "library/treeitem.h"
#include "proto/keys.pb.h"
#include "rekordbox_pdb_test_fixtures.h"
#include "rekordbox_test_fixtures.h"
#include "test/mixxxtest.h"
#include "track/beats.h"
#include "track/cue.h"
#include "track/track.h"
#include "util/db/dbconnectionpooled.h"
#include "util/db/dbconnectionpooler.h"

namespace {

using PdbRootValidationError = kaitai::validation_not_equal_error<std::string>;

class RekordboxPdbImportTest : public MixxxTest {
  public:
    RekordboxPdbImportTest()
            : m_mixxxDb(config(), false) {
    }

    mixxx::DbConnectionPoolPtr databasePool() const {
        return m_mixxxDb.connectionPool();
    }

  private:
    MixxxDb m_mixxxDb;
};

QString parseDeviceDBOnThread(
        mixxx::DbConnectionPoolPtr dbConnectionPool,
        const QString& devicePath) {
    QString parseResult;
    std::exception_ptr parserException;
    std::thread parserThread([dbConnectionPool,
                                     devicePath,
                                     &parseResult,
                                     &parserException] {
        try {
            TreeItem deviceItem(
                    QStringLiteral("TEST_DEVICE"),
                    QVariant(QList<QString>{devicePath}));
            parseResult = mixxx::rekordbox::test::parseDeviceDBForTest(
                    dbConnectionPool, &deviceItem);
        } catch (...) {
            parserException = std::current_exception();
        }
    });
    parserThread.join();
    if (parserException) {
        std::rethrow_exception(parserException);
    }
    return parseResult;
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
    EXPECT_EQ(mixxx::CueType::HotCue, first->getType());
    EXPECT_EQ(mixxx::audio::FramePos(30), first->getPosition());
    EXPECT_EQ(QStringLiteral("updated"), first->getLabel());
    EXPECT_EQ(mixxx::RgbColor(0x102030), first->getColor());
    EXPECT_EQ(mixxx::audio::FramePos(20), duplicate->getPosition());
    EXPECT_TRUE(duplicate->getLabel().isEmpty());

    mixxx::rekordbox::importHotCue(
            track,
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

    mixxx::rekordbox::importHotCue(
            track,
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

TEST(RekordboxImportTest, RejectsInvalidHotCueIdBeforeCreation) {
    TrackPointer track = Track::newTemporary();

    mixxx::rekordbox::importHotCue(
            track,
            mixxx::audio::FramePos(10),
            mixxx::audio::kInvalidFramePos,
            -2,
            QStringLiteral("invalid"),
            mixxx::RgbColor::nullopt());

    EXPECT_TRUE(track->getCuePoints().isEmpty());
}

TEST(RekordboxImportTest, CreatesHotCueAtFirstIndex) {
    TrackPointer track = Track::newTemporary();

    mixxx::rekordbox::importHotCue(
            track,
            mixxx::audio::FramePos(10),
            mixxx::audio::kInvalidFramePos,
            0,
            QStringLiteral("first"),
            mixxx::RgbColor::nullopt());

    const auto cue = track->findHotcueByIndex(0);
    ASSERT_TRUE(cue);
    EXPECT_EQ(1, track->getCuePoints().size());
    EXPECT_EQ(mixxx::CueType::HotCue, cue->getType());
    EXPECT_EQ(mixxx::audio::FramePos(10), cue->getPosition());
    EXPECT_EQ(QStringLiteral("first"), cue->getLabel());
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

TEST_F(RekordboxPdbImportTest, PersistsNormalizedKeyId) {
    const auto dbConnectionPool = databasePool();
    {
        mixxx::DbConnectionPooler connectionPooler(dbConnectionPool);
        const auto database = mixxx::DbConnectionPooled(dbConnectionPool);
        ASSERT_TRUE(MixxxDb::initDatabaseSchema(database));

        QSqlQuery setupQuery(database);
        ASSERT_TRUE(setupQuery.exec(
                "CREATE TABLE rekordbox_library ("
                "    id INTEGER PRIMARY KEY AUTOINCREMENT, rb_id INTEGER, artist TEXT,"
                "    title TEXT, album TEXT, year INTEGER, genre TEXT, tracknumber TEXT,"
                "    location TEXT UNIQUE, comment TEXT, duration INTEGER, bitrate TEXT,"
                "    bpm FLOAT, key TEXT, key_id INTEGER, rating INTEGER,"
                "    analyze_path TEXT UNIQUE, device TEXT, color INTEGER)"));
        ASSERT_TRUE(setupQuery.exec(
                "CREATE TABLE rekordbox_playlists ("
                "    id INTEGER PRIMARY KEY, name TEXT UNIQUE)"));
        ASSERT_TRUE(setupQuery.exec(
                "CREATE TABLE rekordbox_playlist_tracks ("
                "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "    playlist_id INTEGER REFERENCES rekordbox_playlists(id),"
                "    track_id INTEGER REFERENCES rekordbox_library(id), position INTEGER)"));
    }

    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());
    const QString devicePath = temporaryDirectory.filePath("device");
    const QString rekordboxPath = QDir(devicePath).filePath("PIONEER/rekordbox");
    ASSERT_TRUE(QDir().mkpath(rekordboxPath));

    const QString pdbPath = QDir(rekordboxPath).filePath("export.pdb");
    QFile pdbFile(pdbPath);
    ASSERT_TRUE(pdbFile.open(QIODevice::WriteOnly));
    const QByteArray fixture = mixxx::rekordbox::test::makePdbKeyImportFixture();
    ASSERT_EQ(fixture.size(), pdbFile.write(fixture));
    pdbFile.close();

    const QString parseResult = parseDeviceDBOnThread(dbConnectionPool, devicePath);

    ASSERT_EQ(devicePath, parseResult);

    {
        mixxx::DbConnectionPooler connectionPooler(dbConnectionPool);
        const auto database = mixxx::DbConnectionPooled(dbConnectionPool);

        QSqlQuery countQuery(database);
        ASSERT_TRUE(countQuery.exec("SELECT COUNT(*) FROM rekordbox_library"));
        ASSERT_TRUE(countQuery.next());
        ASSERT_EQ(1, countQuery.value(0).toInt());

        QSqlQuery resultQuery(database);
        ASSERT_TRUE(resultQuery.exec(
                "SELECT rb_id, device, key, key_id "
                "FROM rekordbox_library WHERE rb_id = 100"));
        ASSERT_TRUE(resultQuery.next());
        EXPECT_EQ(100, resultQuery.value(0).toInt());
        EXPECT_EQ(QStringLiteral("TEST_DEVICE"), resultQuery.value(1).toString());
        EXPECT_EQ(QStringLiteral("C"), resultQuery.value(2).toString());

        const int importedKeyId = resultQuery.value(3).toInt();
        constexpr auto expectedKey = mixxx::track::io::key::C_MAJOR;
        EXPECT_EQ(static_cast<int>(expectedKey), importedKeyId);
        EXPECT_NE(42, importedKeyId);
        EXPECT_FALSE(resultQuery.next());
    }
}

TEST_F(RekordboxPdbImportTest, PropagatesMalformedParserExceptionFromWorker) {
    const auto dbConnectionPool = databasePool();
    {
        mixxx::DbConnectionPooler connectionPooler(dbConnectionPool);
        const auto database = mixxx::DbConnectionPooled(dbConnectionPool);
        ASSERT_TRUE(MixxxDb::initDatabaseSchema(database));

        QSqlQuery setupQuery(database);
        ASSERT_TRUE(setupQuery.exec(
                "CREATE TABLE rekordbox_library ("
                "    id INTEGER PRIMARY KEY AUTOINCREMENT, rb_id INTEGER, artist TEXT,"
                "    title TEXT, album TEXT, year INTEGER, genre TEXT, tracknumber TEXT,"
                "    location TEXT UNIQUE, comment TEXT, duration INTEGER, bitrate TEXT,"
                "    bpm FLOAT, key TEXT, key_id INTEGER, rating INTEGER,"
                "    analyze_path TEXT UNIQUE, device TEXT, color INTEGER)"));
        ASSERT_TRUE(setupQuery.exec(
                "CREATE TABLE rekordbox_playlists ("
                "    id INTEGER PRIMARY KEY, name TEXT UNIQUE)"));
        ASSERT_TRUE(setupQuery.exec(
                "CREATE TABLE rekordbox_playlist_tracks ("
                "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "    playlist_id INTEGER REFERENCES rekordbox_playlists(id),"
                "    track_id INTEGER REFERENCES rekordbox_library(id), position INTEGER)"));
    }

    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());
    const QString devicePath = temporaryDirectory.filePath("device");
    const QString rekordboxPath = QDir(devicePath).filePath("PIONEER/rekordbox");
    ASSERT_TRUE(QDir().mkpath(rekordboxPath));

    const QString pdbPath = QDir(rekordboxPath).filePath("export.pdb");
    QFile pdbFile(pdbPath);
    ASSERT_TRUE(pdbFile.open(QIODevice::WriteOnly));
    QByteArray malformedFixture = mixxx::rekordbox::test::makePdbKeyImportFixture();
    malformedFixture[0x18] = '\x01';
    ASSERT_EQ(malformedFixture.size(), pdbFile.write(malformedFixture));
    pdbFile.close();

    EXPECT_THROW(parseDeviceDBOnThread(dbConnectionPool, devicePath), PdbRootValidationError);
}

TEST(RekordboxImportTest, SkipsInvalidSyntheticHotCueNumbers) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());

    const auto sampleRate = mixxx::audio::SampleRate(48000);
    for (const bool extended : {false, true}) {
        for (const quint32 hotCueNumber : {
                     quint32(0), std::numeric_limits<quint32>::max()}) {
            SCOPED_TRACE(extended ? "extended" : "legacy");
            SCOPED_TRACE(hotCueNumber == 0 ? "zero" : "overflow");

            const QString anlzPath = temporaryDirectory.filePath(
                    QStringLiteral("invalid-%1-%2.dat")
                            .arg(extended ? QStringLiteral("extended")
                                          : QStringLiteral("legacy"))
                            .arg(hotCueNumber));
            QFile anlzFile(anlzPath);
            ASSERT_TRUE(anlzFile.open(QIODevice::WriteOnly));
            const QByteArray fixture = mixxx::rekordbox::test::makeAnlzCueFixture(
                    extended,
                    1,
                    hotCueNumber);
            ASSERT_EQ(fixture.size(), anlzFile.write(fixture));
            anlzFile.close();

            TrackPointer track = Track::newTemporary();
            mixxx::rekordbox::test::readAnalyzeForTest(
                    track, sampleRate, 0, false, anlzPath);

            EXPECT_TRUE(track->getCuePoints().isEmpty());
        }
    }
}

TEST(RekordboxImportTest, PreservesSyntheticMemoryCueZeroNumber) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());

    const auto sampleRate = mixxx::audio::SampleRate(48000);
    for (const bool extended : {false, true}) {
        SCOPED_TRACE(extended ? "extended" : "legacy");

        const QString anlzPath = temporaryDirectory.filePath(
                extended ? QStringLiteral("memory-extended.dat")
                         : QStringLiteral("memory-legacy.dat"));
        QFile anlzFile(anlzPath);
        ASSERT_TRUE(anlzFile.open(QIODevice::WriteOnly));
        const QByteArray fixture = mixxx::rekordbox::test::makeAnlzCueFixture(
                extended,
                0,
                0,
                1,
                true);
        ASSERT_EQ(fixture.size(), anlzFile.write(fixture));
        anlzFile.close();

        TrackPointer track = Track::newTemporary();
        mixxx::rekordbox::test::readAnalyzeForTest(
                track, sampleRate, 0, false, anlzPath);

        const QList<CuePointer> cuePoints = track->getCuePoints();
        ASSERT_EQ(2, cuePoints.size());
        EXPECT_EQ(mixxx::CueType::MainCue, cuePoints.at(0)->getType());
        EXPECT_EQ(Cue::kNoHotCue, cuePoints.at(0)->getHotCue());
        EXPECT_EQ(mixxx::CueType::Loop, cuePoints.at(1)->getType());
        EXPECT_EQ(Cue::kNoHotCue, cuePoints.at(1)->getHotCue());
        EXPECT_EQ(mixxx::audio::FramePos(96000), cuePoints.at(1)->getPosition());
        EXPECT_EQ(mixxx::audio::FramePos(144000), cuePoints.at(1)->getEndPosition());
    }
}

TEST(RekordboxImportTest, PreservesSyntheticHotCueNumberBeyondEight) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());

    const auto sampleRate = mixxx::audio::SampleRate(48000);
    for (const bool extended : {false, true}) {
        SCOPED_TRACE(extended ? "extended" : "legacy");

        const QString anlzPath = temporaryDirectory.filePath(
                extended ? QStringLiteral("hot-extended.dat")
                         : QStringLiteral("hot-legacy.dat"));
        QFile anlzFile(anlzPath);
        ASSERT_TRUE(anlzFile.open(QIODevice::WriteOnly));
        const QByteArray fixture = mixxx::rekordbox::test::makeAnlzCueFixture(
                extended,
                1,
                9);
        ASSERT_EQ(fixture.size(), anlzFile.write(fixture));
        anlzFile.close();

        TrackPointer track = Track::newTemporary();
        mixxx::rekordbox::test::readAnalyzeForTest(
                track, sampleRate, 0, false, anlzPath);

        const QList<CuePointer> cuePoints = track->getCuePoints();
        ASSERT_EQ(1, cuePoints.size());
        EXPECT_EQ(mixxx::CueType::HotCue, cuePoints.at(0)->getType());
        EXPECT_EQ(8, cuePoints.at(0)->getHotCue());
    }
}

TEST(RekordboxImportTest, EmptyAnalyzeFilePreservesTrackState) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());

    const QString anlzPath = temporaryDirectory.filePath("empty.dat");
    QFile anlzFile(anlzPath);
    ASSERT_TRUE(anlzFile.open(QIODevice::WriteOnly));
    ASSERT_EQ(0, anlzFile.size());
    anlzFile.close();

    const auto sampleRate = mixxx::audio::SampleRate(48000);
    for (const bool ignoreCues : {true, false}) {
        SCOPED_TRACE(ignoreCues ? "ignoreCues=true" : "ignoreCues=false");

        TrackPointer seededTrack = Track::newTemporary();
        const auto seededBeats = mixxx::Beats::fromConstTempo(
                sampleRate, mixxx::audio::FramePos(48000), mixxx::Bpm(120.0));
        ASSERT_TRUE(seededBeats);
        ASSERT_TRUE(seededTrack->trySetBeats(seededBeats));
        const CuePointer seededCue = seededTrack->createAndAddCue(
                mixxx::CueType::HotCue,
                1,
                mixxx::audio::FramePos(123),
                mixxx::audio::kInvalidFramePos);

        ASSERT_TRUE(seededCue);

        EXPECT_NO_THROW(mixxx::rekordbox::test::readAnalyzeForTest(
                seededTrack, sampleRate, 0, ignoreCues, anlzPath));

        EXPECT_EQ(seededBeats, seededTrack->getBeats());
        ASSERT_EQ(1, seededTrack->getCuePoints().size());
        EXPECT_EQ(seededCue, seededTrack->getCuePoints().at(0));
        EXPECT_EQ(mixxx::audio::FramePos(123), seededCue->getPosition());

        TrackPointer freshTrack = Track::newTemporary();
        EXPECT_NO_THROW(mixxx::rekordbox::test::readAnalyzeForTest(
                freshTrack, sampleRate, 0, ignoreCues, anlzPath));

        EXPECT_FALSE(freshTrack->getBeats());
        EXPECT_TRUE(freshTrack->getCuePoints().isEmpty());
    }
}

TEST(RekordboxImportTest, PartialAnalyzeFilePreservesTrackState) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());

    const QString anlzPath = temporaryDirectory.filePath("partial.dat");
    QFile anlzFile(anlzPath);
    ASSERT_TRUE(anlzFile.open(QIODevice::WriteOnly));
    QByteArray fixture = mixxx::rekordbox::test::makeAnlzBeatGridFixture();
    fixture.append("PCOB", 4);
    mixxx::rekordbox::test::appendU32Be(&fixture, 12);
    mixxx::rekordbox::test::appendU32Be(&fixture, 24);
    QByteArray declaredFileSize;
    mixxx::rekordbox::test::appendU32Be(
            &declaredFileSize, static_cast<quint32>(fixture.size() + 12));
    fixture.replace(8, 4, declaredFileSize);
    ASSERT_EQ(fixture.size(), anlzFile.write(fixture));
    anlzFile.close();

    const auto sampleRate = mixxx::audio::SampleRate(48000);
    for (const bool ignoreCues : {true, false}) {
        SCOPED_TRACE(ignoreCues ? "ignoreCues=true" : "ignoreCues=false");

        TrackPointer seededTrack = Track::newTemporary();
        const auto seededBeats = mixxx::Beats::fromConstTempo(
                sampleRate, mixxx::audio::FramePos(24000), mixxx::Bpm(90.0));
        ASSERT_TRUE(seededBeats);
        ASSERT_TRUE(seededTrack->trySetBeats(seededBeats));
        const CuePointer seededCue = seededTrack->createAndAddCue(
                mixxx::CueType::HotCue,
                1,
                mixxx::audio::FramePos(123),
                mixxx::audio::kInvalidFramePos);

        ASSERT_TRUE(seededCue);

        EXPECT_NO_THROW(mixxx::rekordbox::test::readAnalyzeForTest(
                seededTrack, sampleRate, 0, ignoreCues, anlzPath));

        EXPECT_EQ(seededBeats, seededTrack->getBeats());
        ASSERT_EQ(1, seededTrack->getCuePoints().size());
        EXPECT_EQ(seededCue, seededTrack->getCuePoints().at(0));
        EXPECT_EQ(mixxx::audio::FramePos(123), seededCue->getPosition());

        TrackPointer freshTrack = Track::newTemporary();
        EXPECT_NO_THROW(mixxx::rekordbox::test::readAnalyzeForTest(
                freshTrack, sampleRate, 0, ignoreCues, anlzPath));

        EXPECT_FALSE(freshTrack->getBeats());
        EXPECT_TRUE(freshTrack->getCuePoints().isEmpty());
    }
}

} // namespace
