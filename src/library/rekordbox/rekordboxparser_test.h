#pragma once

#include <QSqlDatabase>
#include <QString>
#include <string>

#include "audio/types.h"
#include "track/track_decl.h"
#include "util/db/dbconnectionpool.h"

class TreeItem;

namespace mixxx::rekordbox::test {

// AI-generated test declarations begin; compiled only with BUILD_TESTING.
void readAnalyzeForTest(TrackPointer track,
        mixxx::audio::SampleRate sampleRate,
        int timingOffset,
        bool ignoreCues,
        const QString& path);
QString nullPdbStringForTest();
QString textFromPdbStringForTest(const std::string& bytes);
QString utf16BeTextForTest(const std::string& bytes);

// End AI-generated test declarations.

} // namespace mixxx::rekordbox::test
