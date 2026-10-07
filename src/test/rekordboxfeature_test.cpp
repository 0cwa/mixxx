#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <exception>
#include <limits>
#include <string>
#include <thread>

#include "library/rekordbox/rekordboximport.h"
#include "library/rekordbox/rekordboxparser_test.h"
#include "library/treeitem.h"
#include "rekordbox_test_fixtures.h"
#include "test/mixxxtest.h"
#include "track/beats.h"
#include "track/cue.h"
#include "track/track.h"

namespace {

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

TEST(RekordboxImportTest, ExistingUnreadableAnalyzeFilePreservesTrackState) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());
    const QString path = temporaryDirectory.filePath("unreadable.dat");
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(4, file.write("PMAI", 4));
    file.close();
    const auto originalPermissions = file.permissions();
    if (!file.setPermissions(QFileDevice::Permissions{})) {
        GTEST_SKIP() << "Local filesystem cannot remove fixture read permissions";
    }
    const auto restorePermissions = qScopeGuard([&file, originalPermissions] {
        EXPECT_TRUE(file.setPermissions(originalPermissions));
    });
    ASSERT_TRUE(file.exists());
    QFile readProbe(path);
    if (readProbe.open(QIODevice::ReadOnly)) {
        GTEST_SKIP() << "Filesystem or current user still permits reading the fixture";
    }

    const auto sampleRate = mixxx::audio::SampleRate(48000);
    for (const bool ignoreCues : {true, false}) {
        SCOPED_TRACE(ignoreCues ? "ignoreCues=true" : "ignoreCues=false");
        TrackPointer track = Track::newTemporary();
        const auto beats = mixxx::Beats::fromConstTempo(
                sampleRate, mixxx::audio::FramePos(24000), mixxx::Bpm(90.0));
        ASSERT_TRUE(beats);
        ASSERT_TRUE(track->trySetBeats(beats));
        const CuePointer cue = track->createAndAddCue(mixxx::CueType::HotCue,
                0,
                mixxx::audio::FramePos(123),
                mixxx::audio::kInvalidFramePos);
        ASSERT_TRUE(cue);
        EXPECT_NO_THROW(mixxx::rekordbox::test::readAnalyzeForTest(
                track, sampleRate, 0, ignoreCues, path));
        EXPECT_EQ(beats, track->getBeats());
        ASSERT_EQ(1, track->getCuePoints().size());
        EXPECT_EQ(cue, track->getCuePoints().at(0));
        EXPECT_EQ(mixxx::audio::FramePos(123), cue->getPosition());
        TrackPointer fresh = Track::newTemporary();
        EXPECT_NO_THROW(mixxx::rekordbox::test::readAnalyzeForTest(
                fresh, sampleRate, 0, ignoreCues, path));
        EXPECT_FALSE(fresh->getBeats());
        EXPECT_TRUE(fresh->getCuePoints().isEmpty());
    }
}

} // namespace
