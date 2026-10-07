#pragma once

#include <QString>

#include "audio/types.h"
#include "track/track_decl.h"
#include "util/db/dbconnectionpool.h"

class TreeItem;

namespace mixxx::rekordbox::test {

// Test accessors for the production parser entry points. These intentionally
// preserve the file, Kaitai, and import paths used by RekordboxFeature. The
// readAnalyze seam does not cover getTrack's DAT/EXT selection.
void readAnalyzeForTest(
        TrackPointer track,
        mixxx::audio::SampleRate sampleRate,
        int timingOffset,
        bool ignoreCues,
        const QString& anlzPath);

QString parseDeviceDBForTest(
        mixxx::DbConnectionPoolPtr dbConnectionPool,
        TreeItem* deviceItem);

} // namespace mixxx::rekordbox::test
