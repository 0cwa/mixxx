#include "widget/wmemorycuebutton.h"

#include <gtest/gtest.h>

#include <initializer_list>

#include "track/track.h"

namespace {

TrackPointer trackWithMemoryCues(std::initializer_list<SINT> positions) {
    TrackPointer pTrack = Track::newTemporary();
    for (const SINT position : positions) {
        const auto cuePosition = mixxx::audio::FramePos::fromEngineSamplePos(position);
        pTrack->createAndAddCue(
                mixxx::CueType::Memory,
                Cue::kNoHotCue,
                cuePosition,
                cuePosition);
    }
    return pTrack;
}

} // namespace

TEST(WMemoryCueButtonTest, FindsNearestMemoryCueWithinWindow) {
    const auto pTrack = trackWithMemoryCues({70000, 95000});

    const CuePointer pNearestCue = WMemoryCueButton::findNearestMemoryCue(
            pTrack->getCuePoints(), 100000.0);

    ASSERT_TRUE(pNearestCue);
    EXPECT_EQ(95000,
            pNearestCue->getStartAndEndPosition().startPosition.toEngineSamplePos());
}
