#include "waveform/renderers/waveformmarkset.h"

#include <gtest/gtest.h>

#include <algorithm>

#include "control/controlobject.h"
#include "test/mixxxtest.h"

namespace {

WaveformMarkPointer makeMark(const QString& group,
        const QString& positionControl,
        int hotCue = Cue::kNoHotCue) {
    auto maybeMark = WaveformMark::create(
            group,
            positionControl,
            QString(),
            QStringLiteral("#ffffff"),
            QStringLiteral("AlignBottom"),
            QString(),
            QString(),
            QString(),
            QColor(Qt::white),
            0,
            hotCue);
    EXPECT_TRUE(std::holds_alternative<WaveformMarkPointer>(maybeMark));
    if (!std::holds_alternative<WaveformMarkPointer>(maybeMark)) {
        return {};
    }
    return std::get<WaveformMarkPointer>(maybeMark);
}

} // namespace

class WaveformMarkSetTest : public MixxxTest {};

TEST(WaveformMarkTest, CountdownCategoriesUseMarkerControls) {
    EXPECT_EQ(WaveformMark::CountdownCategory::HotCue,
            WaveformMark::countdownCategoryForPositionControl({}, 0));
    EXPECT_EQ(WaveformMark::CountdownCategory::MemoryCue,
            WaveformMark::countdownCategoryForPositionControl(
                    QStringLiteral("cue_point")));
    EXPECT_EQ(WaveformMark::CountdownCategory::IntroCue,
            WaveformMark::countdownCategoryForPositionControl(
                    QStringLiteral("intro_end_position")));
    EXPECT_EQ(WaveformMark::CountdownCategory::OutroCue,
            WaveformMark::countdownCategoryForPositionControl(
                    QStringLiteral("outro_start_position")));
    EXPECT_EQ(WaveformMark::CountdownCategory::None,
            WaveformMark::countdownCategoryForPositionControl(
                    QStringLiteral("loop_start_position")));
}

TEST_F(WaveformMarkSetTest, CountdownSelectionHonorsCategories) {
    constexpr double kDefaultNextMarkPosition = 10000.0;
    const QString group = QStringLiteral("[WaveformCountdownTest]");

    ControlObject hotCuePosition(ConfigKey(group, QStringLiteral("hotcue_1_position")));
    ControlObject hotCueEndPosition(ConfigKey(group, QStringLiteral("hotcue_1_endposition")));
    ControlObject hotCueType(ConfigKey(group, QStringLiteral("hotcue_1_type")));
    ControlObject hotCueStatus(ConfigKey(group, QStringLiteral("hotcue_1_status")));
    ControlObject memoryCuePosition(ConfigKey(group, QStringLiteral("cue_point")));
    ControlObject introCuePosition(ConfigKey(group, QStringLiteral("intro_start_position")));
    ControlObject outroCuePosition(ConfigKey(group, QStringLiteral("outro_end_position")));
    ControlObject unknownPosition(ConfigKey(group, QStringLiteral("loop_start_position")));

    hotCuePosition.set(300.0);
    hotCueEndPosition.set(0.0);
    hotCueType.set(0.0);
    hotCueStatus.set(0.0);
    memoryCuePosition.set(200.0);
    introCuePosition.set(400.0);
    outroCuePosition.set(500.0);
    unknownPosition.set(100.0);

    WaveformMarkSet marks;
    const auto hotCueMark = makeMark(group, QString(), 0);
    const auto memoryCueMark = makeMark(group, QStringLiteral("cue_point"));
    const auto introCueMark = makeMark(group, QStringLiteral("intro_start_position"));
    const auto outroCueMark = makeMark(group, QStringLiteral("outro_end_position"));
    const auto unknownMark = makeMark(group, QStringLiteral("loop_start_position"));
    ASSERT_TRUE(hotCueMark);
    ASSERT_TRUE(memoryCueMark);
    ASSERT_TRUE(introCueMark);
    ASSERT_TRUE(outroCueMark);
    ASSERT_TRUE(unknownMark);
    EXPECT_EQ(WaveformMark::CountdownCategory::HotCue,
            hotCueMark->getCountdownCategory());
    EXPECT_EQ(WaveformMark::CountdownCategory::MemoryCue,
            memoryCueMark->getCountdownCategory());
    EXPECT_EQ(WaveformMark::CountdownCategory::IntroCue,
            introCueMark->getCountdownCategory());
    EXPECT_EQ(WaveformMark::CountdownCategory::OutroCue,
            outroCueMark->getCountdownCategory());
    EXPECT_EQ(WaveformMark::CountdownCategory::None,
            unknownMark->getCountdownCategory());
    marks.addMark(hotCueMark);
    marks.addMark(memoryCueMark);
    marks.addMark(introCueMark);
    marks.addMark(outroCueMark);
    marks.addMark(unknownMark);
    marks.update();

    for (int mask = 0; mask < 16; ++mask) {
        const bool showHotCues = (mask & 1) != 0;
        const bool showMemoryCues = (mask & 2) != 0;
        const bool showIntroCues = (mask & 4) != 0;
        const bool showOutroCues = (mask & 8) != 0;
        double expected = kDefaultNextMarkPosition;
        if (showHotCues) {
            expected = std::min(expected, 300.0);
        }
        if (showMemoryCues) {
            expected = std::min(expected, 200.0);
        }
        if (showIntroCues) {
            expected = std::min(expected, 400.0);
        }
        if (showOutroCues) {
            expected = std::min(expected, 500.0);
        }

        EXPECT_DOUBLE_EQ(expected,
                marks.findNextCountdownMarkPosition(
                        0.0,
                        kDefaultNextMarkPosition,
                        showHotCues,
                        showMemoryCues,
                        showIntroCues,
                        showOutroCues))
                << "category mask " << mask;
    }
}

TEST_F(WaveformMarkSetTest, CountdownSelectionUsesNearestFutureMark) {
    constexpr double kDefaultNextMarkPosition = 10000.0;
    const QString group = QStringLiteral("[WaveformCountdownNearestTest]");

    ControlObject firstPosition(ConfigKey(group, QStringLiteral("intro_end_position")));
    ControlObject secondPosition(ConfigKey(group, QStringLiteral("intro_start_position")));
    ControlObject pastPosition(ConfigKey(group, QStringLiteral("outro_start_position")));
    firstPosition.set(150.0);
    secondPosition.set(101.0);
    pastPosition.set(99.0);

    WaveformMarkSet marks;
    const auto firstMark = makeMark(group, QStringLiteral("intro_end_position"));
    const auto secondMark = makeMark(group, QStringLiteral("intro_start_position"));
    const auto pastMark = makeMark(group, QStringLiteral("outro_start_position"));
    ASSERT_TRUE(firstMark);
    ASSERT_TRUE(secondMark);
    ASSERT_TRUE(pastMark);
    marks.addMark(firstMark);
    marks.addMark(secondMark);
    marks.addMark(pastMark);
    marks.update();

    EXPECT_DOUBLE_EQ(101.0,
            marks.findNextCountdownMarkPosition(
                    100.0, kDefaultNextMarkPosition, false, false, true, false));
    EXPECT_DOUBLE_EQ(150.0,
            marks.findNextCountdownMarkPosition(
                    101.0, kDefaultNextMarkPosition, false, false, true, false));
    EXPECT_DOUBLE_EQ(kDefaultNextMarkPosition,
            marks.findNextCountdownMarkPosition(
                    100.0, 100.0, false, false, true, false));
}
