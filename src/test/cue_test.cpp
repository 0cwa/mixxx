#include "track/cue.h"

#include <gtest/gtest.h>

#include "engine/engine.h"
#include "test/mixxxtest.h"
#include "util/color/color.h"
#include "util/color/predefinedcolorpalettes.h"

namespace mixxx {

TEST(CueTest, NewCueIsDirty) {
    const auto cue = Cue(
            mixxx::CueType::HotCue,
            1,
            mixxx::audio::kStartFramePos,
            mixxx::audio::kInvalidFramePos,
            mixxx::PredefinedColorPalettes::kDefaultCueColor);
    EXPECT_TRUE(cue.isDirty());
}

TEST(CueTest, DefaultCueInfoToCueRoundtrip) {
    const CueInfo cueInfo1;
    const Cue cueObject(
            cueInfo1,
            audio::SampleRate(44100),
            true);
    auto cueInfo2 = cueObject.getCueInfo(
            audio::SampleRate(44100));
    cueInfo2.setColor(std::nullopt);
    EXPECT_EQ(cueInfo1, cueInfo2);
}

TEST(CueTest, ConvertCueInfoToCueRoundtrip) {
    // Due to rounding errors this test may fail if the
    // cue position/sample conversions don't always result
    // in integer numbers.
    const auto cueInfo1 = CueInfo(
            CueType::HotCue,
            std::make_optional(1.0 * 44100 * mixxx::kEngineChannelOutputCount),
            std::nullopt,
            std::make_optional(3),
            QStringLiteral("label"),
            RgbColor::optional(0xABCDEF));
    const Cue cueObject(
            cueInfo1,
            audio::SampleRate(44100),
            true);
    EXPECT_TRUE(cueObject.isDirty());
    const auto cueInfo2 = cueObject.getCueInfo(
            audio::SampleRate(44100));
    EXPECT_EQ(cueInfo1, cueInfo2);
}

// Prepared by ChatGPT for human review.
TEST(CueTest, ChangingLoadedHotCueIndexMarksDirtyAndEmits) {
    Cue cue(DbId(QVariant(7)),
            CueType::HotCue,
            audio::FramePos(48000),
            0,
            0,
            QString(),
            RgbColor(0xabcdef));
    int updates = 0;
    QObject::connect(&cue, &Cue::updated, [&updates] { ++updates; });
    ASSERT_FALSE(cue.isDirty());

    cue.setHotCue(3);

    EXPECT_EQ(3, cue.getHotCue());
    EXPECT_TRUE(cue.isDirty());
    EXPECT_EQ(1, updates);
}

TEST(CueTest, UnchangedLoadedHotCueIndexStaysClean) {
    Cue cue(DbId(QVariant(7)),
            CueType::HotCue,
            audio::FramePos(48000),
            0,
            0,
            QString(),
            RgbColor(0xabcdef));
    int updates = 0;
    QObject::connect(&cue, &Cue::updated, [&updates] { ++updates; });
    ASSERT_FALSE(cue.isDirty());

    cue.setHotCue(0);

    EXPECT_FALSE(cue.isDirty());
    EXPECT_EQ(0, updates);
}
// End of ChatGPT-prepared tests for human review.

} // namespace mixxx
