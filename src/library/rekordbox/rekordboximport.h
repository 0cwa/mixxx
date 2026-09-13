#pragma once

#include <QString>

#include "audio/frame.h"
#include "track/track_decl.h"
#include "util/color/rgbcolor.h"

namespace mixxx::rekordbox {

void importMemoryCue(TrackPointer track,
        mixxx::audio::FramePos startPosition,
        mixxx::audio::FramePos endPosition,
        const QString& label,
        mixxx::RgbColor::optional_t color);

void importHotCue(TrackPointer track,
        mixxx::audio::FramePos startPosition,
        mixxx::audio::FramePos endPosition,
        int id,
        const QString& label,
        mixxx::RgbColor::optional_t color);

} // namespace mixxx::rekordbox
