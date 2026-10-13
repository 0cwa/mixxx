#pragma once

#include <QString>

#include "audio/types.h"
#include "track/track_decl.h"

#ifdef BUILD_TESTING
namespace mixxx::rekordbox::test {

void readAnalyzeForTest(TrackPointer track,
        mixxx::audio::SampleRate sampleRate,
        int timingOffset,
        bool ignoreCues,
        const QString& anlzPath);

} // namespace mixxx::rekordbox::test
#endif
