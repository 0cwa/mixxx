#pragma once

#include <QString>

#include "audio/types.h"
#include "track/track_decl.h"

namespace mixxx::rekordbox::test {

// Test access to the production ANLZ parser and import path.
void readAnalyzeForTest(
        TrackPointer track,
        mixxx::audio::SampleRate sampleRate,
        int timingOffset,
        bool ignoreCues,
        const QString& anlzPath);

} // namespace mixxx::rekordbox::test
