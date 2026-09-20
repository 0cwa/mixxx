#include "waveform/renderers/waveformmarkset.h"

#include <gtest/gtest.h>

#include <algorithm>

#include "control/controlobject.h"
#include "test/mixxxtest.h"
#include "track/cue.h"
#include "util/color/predefinedcolorpalettes.h"
#include "waveform/waveformwidgetfactory.h"

namespace {

WaveformMarkPointer makeFixedMark(const QString& group,
        const QString& positionControl,
        double samplePosition,
        int hotCue = Cue::kNoHotCue,
        const QString& visibilityControl = {}) {
    auto maybeMark = WaveformMark::create(
            group,
            positionControl,
            visibilityControl,
            QStringLiteral("#ffffff"),
            QStringLiteral("AlignBottom"),
            QString(),
            QString(),
            QString(),
            QColor(),
            0,
            hotCue,
            {});
    EXPECT_TRUE(std::holds_alternative<WaveformMarkPointer>(maybeMark));
    if (!std::holds_alternative<WaveformMarkPointer>(maybeMark)) {
        return {};
    }
    auto pMark = std::get<WaveformMarkPointer>(maybeMark);
    pMark->setSamplePosition(samplePosition);
    return pMark;
}

} // anonymous namespace

TEST(WaveformMarkSetTest, MemoryCueMarksShowUntilNext) {
    WaveformMarkSet marks;
    const WaveformMarkSet::DefaultMarkerStyle defaultMarker{
            QString(),
            QString(),
            QStringLiteral("#ffffff"),
            QStringLiteral("AlignBottom"),
            QString(),
            QString(),
            QString(),
            QString(),
            QString(),
            QColor(),
            1.0f,
            1.0f};
    ASSERT_FALSE(marks.setDefault(QString(), defaultMarker).has_value());

    const auto cuePosition = mixxx::audio::FramePos(100);
    const CuePointer pMemoryCue(new Cue(
            mixxx::CueType::Memory,
            Cue::kNoHotCue,
            cuePosition,
            cuePosition,
            mixxx::PredefinedColorPalettes::kDefaultCueColor));

    marks.syncMemoryCueMarks(
            QString(),
            QList<CuePointer>{pMemoryCue},
            0,
            WaveformSignalColors{});
    marks.update();

    const auto pMark = std::find_if(
            marks.cbegin(),
            marks.cend(),
            [cuePosition](const WaveformMarkPointer& mark) {
                return mark->getSamplePosition() == cuePosition.toEngineSamplePos();
            });
    ASSERT_NE(pMark, marks.cend());
    EXPECT_TRUE((*pMark)->isShowUntilNext());
    EXPECT_EQ(WaveformMark::CountdownCategory::MemoryCue,
            (*pMark)->getCountdownCategory());
}

TEST(WaveformMarkTest, CountdownCategoriesUseMarkerTaxonomy) {
    EXPECT_EQ(WaveformMark::CountdownCategory::HotCue,
            WaveformMark::countdownCategoryForPositionControl({}, 0));
    EXPECT_EQ(WaveformMark::CountdownCategory::MemoryCue,
            WaveformMark::countdownCategoryForPositionControl(
                    QStringLiteral("memory_cue")));
    EXPECT_EQ(WaveformMark::CountdownCategory::IntroCue,
            WaveformMark::countdownCategoryForPositionControl(
                    QStringLiteral("intro_start_position")));
    EXPECT_EQ(WaveformMark::CountdownCategory::IntroCue,
            WaveformMark::countdownCategoryForPositionControl(
                    QStringLiteral("intro_end_position")));
    EXPECT_EQ(WaveformMark::CountdownCategory::OutroCue,
            WaveformMark::countdownCategoryForPositionControl(
                    QStringLiteral("outro_start_position")));
    EXPECT_EQ(WaveformMark::CountdownCategory::OutroCue,
            WaveformMark::countdownCategoryForPositionControl(
                    QStringLiteral("outro_end_position")));
    EXPECT_EQ(WaveformMark::CountdownCategory::None,
            WaveformMark::countdownCategoryForPositionControl(
                    QStringLiteral("cue_point")));
}

TEST(WaveformMarkSetTest, CountdownSelectionHonorsIndependentCategories) {
    constexpr double kNoNextMark = 10000.0;
    WaveformMarkSet marks;
    const auto pHotCueMark = makeFixedMark({}, {}, 300.0, 0);
    ASSERT_TRUE(pHotCueMark);
    EXPECT_TRUE(pHotCueMark->isShowUntilNext());
    marks.addMark(pHotCueMark);
    marks.addMark(makeFixedMark({}, QStringLiteral("memory_cue"), 200.0));
    marks.addMark(makeFixedMark({}, QStringLiteral("intro_start_position"), 400.0));
    marks.addMark(makeFixedMark({}, QStringLiteral("outro_start_position"), 500.0));
    marks.addMark(makeFixedMark({}, QStringLiteral("cue_point"), 100.0));
    marks.update();

    for (int mask = 0; mask < 16; ++mask) {
        const bool showHotCues = mask & 1;
        const bool showMemoryCues = mask & 2;
        const bool showIntroCues = mask & 4;
        const bool showOutroCues = mask & 8;
        double expected = kNoNextMark;
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
                        kNoNextMark,
                        showHotCues,
                        showMemoryCues,
                        showIntroCues,
                        showOutroCues))
                << "category mask " << mask;
    }
}

TEST(WaveformMarkSetTest, CountdownSelectionUsesNearestFutureVisibleMark) {
    constexpr double kNoNextMark = 10000.0;
    constexpr double kPlayPosition = 100.0;
    WaveformMarkSet marks;
    marks.addMark(makeFixedMark({}, QStringLiteral("memory_cue"), 100.5));
    marks.addMark(makeFixedMark({}, QStringLiteral("memory_cue"), 101.0));
    marks.addMark(makeFixedMark({}, QStringLiteral("memory_cue"), 99.0));
    marks.addMark(makeFixedMark({}, QStringLiteral("memory_cue"), 150.0));
    marks.addMark(makeFixedMark({}, QStringLiteral("memory_cue"), Cue::kNoPosition));
    marks.update();

    EXPECT_DOUBLE_EQ(101.0,
            marks.findNextCountdownMarkPosition(
                    kPlayPosition, kNoNextMark, false, true, false, false));
}

class WaveformMarkVisibilityTest : public MixxxTest {};

TEST_F(WaveformMarkVisibilityTest, HiddenIntroOutroMarkersAreNotCountdownTargets) {
    constexpr double kNoNextMark = 10000.0;
    const QString group = QStringLiteral("[WaveformCountdownTest]");
    const ConfigKey visibilityControlKey(group, QStringLiteral("show_intro_outro_cues"));
    ControlObject introPositionControl(
            ConfigKey(group, QStringLiteral("intro_start_position")));
    ControlObject outroPositionControl(
            ConfigKey(group, QStringLiteral("outro_start_position")));
    const QString visibilityControl = QStringLiteral(
            "[WaveformCountdownTest],show_intro_outro_cues");
    ControlObject visibilityControlObject(visibilityControlKey);
    ControlObject::set(visibilityControlKey, 0.0);

    WaveformMarkSet marks;
    marks.addMark(makeFixedMark(group,
            QStringLiteral("intro_start_position"),
            200.0,
            Cue::kNoHotCue,
            visibilityControl));
    marks.addMark(makeFixedMark(group,
            QStringLiteral("outro_start_position"),
            300.0,
            Cue::kNoHotCue,
            visibilityControl));
    marks.update();
    EXPECT_DOUBLE_EQ(kNoNextMark,
            marks.findNextCountdownMarkPosition(
                    0.0, kNoNextMark, false, false, true, false));

    ControlObject::set(visibilityControlKey, 1.0);
    marks.update();
    EXPECT_DOUBLE_EQ(200.0,
            marks.findNextCountdownMarkPosition(
                    0.0, kNoNextMark, false, false, true, false));

    EXPECT_DOUBLE_EQ(300.0,
            marks.findNextCountdownMarkPosition(
                    200.0, kNoNextMark, false, false, false, true));
}

class WaveformCueCountdownConfigTest : public MixxxTest {
  protected:
    void SetUp() override {
        WaveformWidgetFactory::createInstance();
        ASSERT_TRUE(WaveformWidgetFactory::instance()->setConfig(config()));
    }

    void TearDown() override {
        WaveformWidgetFactory::destroy();
    }
};

TEST_F(WaveformCueCountdownConfigTest, DefaultsAreMigratedToWaveformConfig) {
    auto* factory = WaveformWidgetFactory::instance();

    const ConfigKey hotKey("[Waveform]", "cue_countdown_hot_cues");
    const ConfigKey memoryKey("[Waveform]", "cue_countdown_memory_cues");
    const ConfigKey introKey("[Waveform]", "cue_countdown_intro_cues");
    const ConfigKey outroKey("[Waveform]", "cue_countdown_outro_cues");

    EXPECT_TRUE(config()->exists(hotKey));
    EXPECT_TRUE(config()->exists(memoryKey));
    EXPECT_TRUE(config()->exists(introKey));
    EXPECT_TRUE(config()->exists(outroKey));
    EXPECT_FALSE(factory->getUntilMarkShowHotCues());
    EXPECT_TRUE(factory->getUntilMarkShowMemoryCues());
    EXPECT_FALSE(factory->getUntilMarkShowIntroCues());
    EXPECT_FALSE(factory->getUntilMarkShowOutroCues());
}

TEST_F(WaveformCueCountdownConfigTest, MigrationPreservesExistingValuesAndFormats) {
    const ConfigKey hotKey("[Waveform]", "cue_countdown_hot_cues");
    const ConfigKey memoryKey("[Waveform]", "cue_countdown_memory_cues");
    const ConfigKey introKey("[Waveform]", "cue_countdown_intro_cues");
    const ConfigKey outroKey("[Waveform]", "cue_countdown_outro_cues");
    const ConfigKey beatsKey("[Waveform]", "UntilMarkShowBeats");
    const ConfigKey timeKey("[Waveform]", "UntilMarkShowTime");

    config()->setValue(hotKey, false);
    config()->setValue(introKey, true);
    config()->setValue(outroKey, false);
    config()->setValue(beatsKey, false);
    config()->setValue(timeKey, true);

    auto* factory = WaveformWidgetFactory::instance();
    ASSERT_TRUE(factory->setConfig(config()));

    EXPECT_FALSE(factory->getUntilMarkShowHotCues());
    EXPECT_TRUE(factory->getUntilMarkShowMemoryCues());
    EXPECT_TRUE(factory->getUntilMarkShowIntroCues());
    EXPECT_FALSE(factory->getUntilMarkShowOutroCues());
    EXPECT_FALSE(config()->getValue<bool>(beatsKey, true));
    EXPECT_TRUE(config()->getValue<bool>(timeKey, false));
}
