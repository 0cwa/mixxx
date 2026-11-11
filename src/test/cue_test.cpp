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

TEST(CueTest, ActiveLoopIntentKeepsUnknownDistinctFromFalse) {
    for (const auto intent : {std::optional<bool>{},
                 std::optional<bool>{false},
                 std::optional<bool>{true}}) {
        CueInfo info(CueType::Memory,
                1000.0,
                2000.0,
                std::nullopt,
                QStringLiteral("memory loop"),
                RgbColor::optional(0x123456));
        info.setActiveLoopIntent(intent);
        const Cue cue(info, audio::SampleRate(48000), true);
        EXPECT_EQ(intent, cue.getActiveLoopIntent());
        EXPECT_EQ(info, cue.getCueInfo(audio::SampleRate(48000)));
    }
    CueInfo unknown;
    CueInfo inactive;
    inactive.setActiveLoopIntent(false);
    EXPECT_NE(unknown, inactive);
}

TEST(CueTest, EditingHotSlotAndActiveLoopIntentMarksDirty) {
    Cue cue(DbId(QVariant(1)),
            CueType::Loop,
            audio::FramePos(48000),
            48000,
            0,
            QStringLiteral("loop"),
            RgbColor(0x123456));
    ASSERT_FALSE(cue.isDirty());
    int updates = 0;
    QObject::connect(&cue, &Cue::updated, [&updates]() {
        ++updates;
    });
    cue.setHotCue(7);
    EXPECT_EQ(7, cue.getHotCue());
    EXPECT_TRUE(cue.isDirty());
    EXPECT_EQ(1, updates);
    cue.setHotCue(7);
    EXPECT_EQ(1, updates);

    cue.setActiveLoopIntent(false);
    EXPECT_EQ(std::optional<bool>(false), cue.getActiveLoopIntent());
    EXPECT_EQ(2, updates);
    cue.setActiveLoopIntent(false);
    EXPECT_EQ(2, updates);
    cue.setActiveLoopIntent(std::nullopt);
    EXPECT_FALSE(cue.getActiveLoopIntent().has_value());
    EXPECT_EQ(3, updates);
}

} // namespace mixxx
