#include "preferences/dialog/dlgprefwaveform.h"

#include <gtest/gtest.h>

#include "test/mixxxtest.h"
#include "waveform/waveformwidgetfactory.h"

class DlgPrefWaveformTest : public MixxxTest {};

TEST_F(DlgPrefWaveformTest, CueCountdownControlsHaveIndependentDefaultsAndSettings) {
    auto* factory = WaveformWidgetFactory::instance();
    ASSERT_TRUE(factory->setConfig(config()));

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
    ASSERT_TRUE(factory->setConfig(config()));

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
