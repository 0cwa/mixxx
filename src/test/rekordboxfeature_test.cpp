#include <gtest/gtest.h>

#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QTemporaryDir>

#include "library/rekordbox/rekordboximport.h"

namespace {

TEST(RekordboxImportTest, RejectsInvalidDatabaseIds) {
    EXPECT_FALSE(mixxx::rekordbox::isValidDatabaseId(-1));
    EXPECT_FALSE(mixxx::rekordbox::isValidDatabaseId(0));
    EXPECT_TRUE(mixxx::rekordbox::isValidDatabaseId(1));
}

TEST(RekordboxImportTest, RequiresWritableDestinationDatabase) {
    QTemporaryDir temporaryDirectory;
    ASSERT_TRUE(temporaryDirectory.isValid());

    const QString databasePath = temporaryDirectory.filePath("mixxxdb.sqlite");
    const QString connectionName = QStringLiteral("rekordbox-import-test");
    bool permissionCheckSkipped = false;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", connectionName);
        database.setDatabaseName(databasePath);
        ASSERT_TRUE(database.open());
        EXPECT_TRUE(mixxx::rekordbox::isWritableDatabase(database));

        database.close();
        ASSERT_TRUE(QFile::setPermissions(
                databasePath,
                QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther));
        ASSERT_TRUE(database.open());
        permissionCheckSkipped = QFileInfo(databasePath).isWritable();
        if (!permissionCheckSkipped) {
            EXPECT_FALSE(mixxx::rekordbox::isWritableDatabase(database));
        }

        database.close();
    }
    QSqlDatabase::removeDatabase(connectionName);
    if (permissionCheckSkipped) {
        GTEST_SKIP() << "The test runner can write files without write permission bits";
    }
}

} // namespace
