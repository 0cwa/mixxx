#pragma once

#include <QSqlDatabase>
#include <QString>
#include <string>

#include "audio/types.h"
#include "track/track_decl.h"
#include "util/db/dbconnectionpool.h"

class TreeItem;

namespace mixxx::rekordbox::test {

#if defined(BUILD_TESTING)

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

// AI-generated combined test accessors begin; original declarations retained.
bool createDeviceTablesForTest(QSqlDatabase& database);
QString nullPdbStringForTest();
QString textFromPdbStringForTest(const std::string& bytes);
QString utf16BeTextForTest(const std::string& bytes);
// End AI-generated combined test accessors.

#endif

} // namespace mixxx::rekordbox::test
