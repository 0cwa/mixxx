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
bool createDeviceTablesForTest(QSqlDatabase& database);
QString parseDeviceDBForTest(mixxx::DbConnectionPoolPtr pool, TreeItem* item);

// End AI-generated test declarations.

} // namespace mixxx::rekordbox::test
