#include "preferences/dialog/dlgprefwaveform.h"

#include <gtest/gtest.h>

#include "test/mixxxtest.h"
#include "waveform/renderers/waveformwidgetrenderer.h"
#include "waveform/waveformwidgetfactory.h"

class DlgPrefWaveformTest : public MixxxTest {
  protected:
    void SetUp() override {
        WaveformWidgetFactory::createInstance();
        ASSERT_TRUE(WaveformWidgetFactory::instance()->setConfig(config()));
    }

    void TearDown() override {
        WaveformWidgetFactory::destroy();
    }
};

namespace {
const ConfigKey kMaxZoomOutKey(QStringLiteral("[Waveform]"), QStringLiteral("MaxZoomOut"));
} // namespace

TEST_F(DlgPrefWaveformTest, CueCountdownControlsHaveIndependentDefaultsAndSettings) {
    auto* factory = WaveformWidgetFactory::instance();

    DlgPrefWaveform dialog(nullptr, config(), nullptr);

    EXPECT_EQ(QStringLiteral("Hot Cue markers"), dialog.untilMarkShowHotCuesCheckBox->text());
    EXPECT_EQ(QStringLiteral("Memory Cue markers"),
            dialog.untilMarkShowMemoryCuesCheckBox->text());
    EXPECT_EQ(QStringLiteral("Intro Cue markers"),
            dialog.untilMarkShowIntroCuesCheckBox->text());
    EXPECT_EQ(QStringLiteral("Outro Cue markers"),
            dialog.untilMarkShowOutroCuesCheckBox->text());
    EXPECT_FALSE(dialog.untilMarkShowHotCuesCheckBox->isChecked());
    EXPECT_TRUE(dialog.untilMarkShowMemoryCuesCheckBox->isChecked());
    EXPECT_FALSE(dialog.untilMarkShowIntroCuesCheckBox->isChecked());
    EXPECT_FALSE(dialog.untilMarkShowOutroCuesCheckBox->isChecked());

    dialog.untilMarkShowHotCuesCheckBox->setChecked(true);
    EXPECT_TRUE(factory->getUntilMarkShowHotCues());
    EXPECT_TRUE(factory->getUntilMarkShowMemoryCues());

    dialog.untilMarkShowMemoryCuesCheckBox->setChecked(false);
    EXPECT_TRUE(factory->getUntilMarkShowHotCues());
    EXPECT_FALSE(factory->getUntilMarkShowMemoryCues());
}

TEST_F(DlgPrefWaveformTest, CueCategoriesDoNotDependOnDisplayFormatSelection) {
    auto* factory = WaveformWidgetFactory::instance();

    DlgPrefWaveform dialog(nullptr, config(), nullptr);
    dialog.untilMarkShowBeatsCheckBox->setChecked(false);
    dialog.untilMarkShowTimeCheckBox->setChecked(false);
    dialog.untilMarkShowIntroCuesCheckBox->setChecked(true);
    dialog.untilMarkShowOutroCuesCheckBox->setChecked(true);

    EXPECT_TRUE(factory->getUntilMarkShowIntroCues());
    EXPECT_TRUE(factory->getUntilMarkShowOutroCues());
    EXPECT_FALSE(factory->getUntilMarkShowBeats());
    EXPECT_FALSE(factory->getUntilMarkShowTime());
}

TEST_F(DlgPrefWaveformTest, MaxZoomOutDefaultsToLegacyLimit) {
    auto* factory = WaveformWidgetFactory::instance();
    config()->remove(kMaxZoomOutKey);
    ASSERT_TRUE(factory->setConfig(config()));

    EXPECT_DOUBLE_EQ(10.0, factory->getMaxZoomOut());
    EXPECT_DOUBLE_EQ(10.0, config()->getValue(kMaxZoomOutKey, -1.0));

    WaveformWidgetRenderer renderer;
    renderer.setZoom(11.0);
    EXPECT_DOUBLE_EQ(10.0, renderer.getZoom());
}

TEST_F(DlgPrefWaveformTest, MaxZoomOutInvalidConfigUsesLegacyLimit) {
    auto* factory = WaveformWidgetFactory::instance();
    config()->setValue(kMaxZoomOutKey, QStringLiteral("invalid"));
    ASSERT_TRUE(factory->setConfig(config()));

    EXPECT_DOUBLE_EQ(10.0, factory->getMaxZoomOut());
    EXPECT_DOUBLE_EQ(10.0, config()->getValue(kMaxZoomOutKey, -1.0));

    config()->setValue(kMaxZoomOutKey, -1.0);
    ASSERT_TRUE(factory->setConfig(config()));
    EXPECT_DOUBLE_EQ(10.0, factory->getMaxZoomOut());

    config()->setValue(kMaxZoomOutKey, 1000.0);
    ASSERT_TRUE(factory->setConfig(config()));
    EXPECT_DOUBLE_EQ(100.0, factory->getMaxZoomOut());
}

TEST_F(DlgPrefWaveformTest, MaxZoomOutExpandsWaveformClamp) {
    auto* factory = WaveformWidgetFactory::instance();
    config()->setValue(kMaxZoomOutKey, 20);
    config()->setValue(ConfigKey(QStringLiteral("[Waveform]"), QStringLiteral("DefaultZoom")),
            15);
    ASSERT_TRUE(factory->setConfig(config()));

    EXPECT_DOUBLE_EQ(20.0, factory->getMaxZoomOut());
    EXPECT_DOUBLE_EQ(15, factory->getDefaultZoom());

    WaveformWidgetRenderer renderer;
    renderer.setZoom(20.0);
    EXPECT_DOUBLE_EQ(20.0, renderer.getZoom());
    renderer.setZoom(21.0);
    EXPECT_DOUBLE_EQ(20.0, renderer.getZoom());
}

TEST_F(DlgPrefWaveformTest, MaxZoomOutPreferenceIsWiredToDialog) {
    auto* factory = WaveformWidgetFactory::instance();
    DlgPrefWaveform dialog(nullptr, config(), nullptr);

    EXPECT_EQ(QStringLiteral("Maximum zoom-out level"), dialog.maxZoomOutLabel->text());
    EXPECT_EQ(10, dialog.maxZoomOutComboBox->currentData().toInt());
    EXPECT_EQ(10, dialog.maxZoomOutComboBox->count());
    EXPECT_EQ(10, dialog.defaultZoomComboBox->count());

    dialog.maxZoomOutComboBox->setCurrentIndex(
            dialog.maxZoomOutComboBox->findData(20));

    EXPECT_DOUBLE_EQ(20.0, factory->getMaxZoomOut());
    EXPECT_DOUBLE_EQ(20.0, config()->getValue(kMaxZoomOutKey, -1.0));
    EXPECT_EQ(20, dialog.defaultZoomComboBox->count());

    dialog.slotResetToDefaults();
    EXPECT_DOUBLE_EQ(10.0, factory->getMaxZoomOut());
    EXPECT_DOUBLE_EQ(3.0, factory->getDefaultZoom());
    EXPECT_DOUBLE_EQ(3.0, dialog.defaultZoomComboBox->currentData().toDouble());
}
