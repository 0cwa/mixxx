#include <gtest/gtest.h>

#include "library/rekordbox/rekordboximport.h"
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
    EXPECT_EQ(QStringLiteral("updated"), first->getLabel());
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

} // namespace
