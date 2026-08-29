#include "waveform/renderers/waveformmarkset.h"

#include <gtest/gtest.h>

#include <algorithm>

#include "track/cue.h"
#include "util/color/predefinedcolorpalettes.h"

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
}
