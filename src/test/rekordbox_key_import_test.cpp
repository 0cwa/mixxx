#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <exception>
#include <memory>
#include <thread>

#include "control/controlobject.h"
#include "library/basetrackcache.h"
#include "library/dateformatbroadcaster.h"
#include "library/library_prefs.h"
#include "library/rekordbox/rekordboxfeature.h"
#include "library/rekordbox/rekordboxparser_test.h"
#include "library/trackcollectionmanager.h"
#include "library/treeitem.h"
#include "rekordbox_pdb_test_fixtures.h"
#include "test/mixxxdbtest.h"
#include "track/track.h"

namespace {

// AI-generated public PDB import/model regression tests begin.
// The synthetic schema-derived bytes contain no private media or USB export.
class RekordboxKeyImportTest : public MixxxDbTest {
  protected:
    RekordboxKeyImportTest()
            : keyNotation(mixxx::library::prefs::kKeyNotationConfigKey) {
    }

    void SetUp() override {
        auto database = dbConnection();
        ASSERT_TRUE(MixxxDb::initDatabaseSchema(database));
        ASSERT_TRUE(mixxx::rekordbox::test::createDeviceTablesForTest(database));
        ASSERT_TRUE(directory.isValid());
        keyNotation.set(static_cast<double>(KeyUtils::KeyNotation::Traditional));
    }

    QString importDevice(const QString& name, const QByteArray& key) {
        const QString path = directory.filePath(name);
        const QString pdbDirectory = QDir(path).filePath("PIONEER/rekordbox");
        EXPECT_TRUE(QDir().mkpath(pdbDirectory));
        auto bytes = mixxx::rekordbox::test::makePdbKeyImportFixture();
        mixxx::rekordbox::test::setShortAscii(&bytes,
                mixxx::rekordbox::test::kPdbPageSize + 0x28 + 8,
                key);
        QFile file(QDir(pdbDirectory).filePath("export.pdb"));
        EXPECT_TRUE(file.open(QIODevice::WriteOnly));
        EXPECT_EQ(bytes.size(), file.write(bytes));
        file.close();

        QString result;
        std::exception_ptr failure;
        const auto pool = dbConnectionPooler();
        std::thread worker([&] {
            try {
                TreeItem item(name, QVariant(QList<QString>{path}));
                result = mixxx::rekordbox::test::parseDeviceDBForTest(pool, &item);
            } catch (...) {
                failure = std::current_exception();
            }
        });
        worker.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
        EXPECT_EQ(path, result);
        return result;
    }

    QTemporaryDir directory;
    ControlObject keyNotation;
};

TEST_F(RekordboxKeyImportTest, ProductionSchemaAndImportNormalizeForeignKeyId) {
    ASSERT_NO_THROW(importDevice(QStringLiteral("public-C"), QByteArrayLiteral("C")));
    QSqlQuery query(dbConnection());
    ASSERT_TRUE(query.exec("SELECT rb_id, key, key_id FROM rekordbox_library"));
    ASSERT_TRUE(query.next());
    EXPECT_EQ(100, query.value(0).toInt());
    EXPECT_EQ(QStringLiteral("C"), query.value(1).toString());
    EXPECT_EQ(static_cast<int>(mixxx::track::io::key::C_MAJOR), query.value(2).toInt());
    EXPECT_NE(42, query.value(2).toInt());
    EXPECT_FALSE(query.next());
}

TEST_F(RekordboxKeyImportTest, ImportedKeysSortInBothDirectionsThroughRealPlaylistModel) {
    DateFormatChangedBroadcaster::createInstance();
    const auto destroyDateFormatBroadcaster = qScopeGuard([] {
        DateFormatChangedBroadcaster::destroy();
    });
    ASSERT_NO_THROW(importDevice(QStringLiteral("public-D"), QByteArrayLiteral("D")));
    ASSERT_NO_THROW(importDevice(QStringLiteral("public-G"), QByteArrayLiteral("G")));
    ASSERT_NO_THROW(importDevice(QStringLiteral("public-C"), QByteArrayLiteral("C")));
    QSqlQuery query(dbConnection());
    ASSERT_TRUE(query.exec("INSERT INTO rekordbox_playlists (id,name) VALUES (100,'public-sort')"));
    ASSERT_TRUE(query.exec(
            "INSERT INTO rekordbox_playlist_tracks (playlist_id,track_id,position) "
            "SELECT 100,id,id FROM rekordbox_library"));

    auto manager = std::make_unique<TrackCollectionManager>(
            nullptr, config(), dbConnectionPooler(), [](Track* track) { delete track; });
    auto cache = QSharedPointer<BaseTrackCache>::create(
            manager->internalCollection(),
            QStringLiteral("rekordbox_library"),
            QStringLiteral("id"),
            QStringList{"id",
                    "artist",
                    "title",
                    "album",
                    "year",
                    "genre",
                    "tracknumber",
                    "location",
                    "comment",
                    "rating",
                    "duration",
                    "bitrate",
                    "bpm",
                    "key",
                    "color",
                    "analyze_path"},
            QStringList{},
            false);
    RekordboxPlaylistModel model(nullptr, manager.get(), cache);
    model.setPlaylist(QStringLiteral("public-sort"));
    const int keyColumn = model.fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_KEY);
    ASSERT_GE(keyColumn, 0);
    EXPECT_EQ(keyColumn, model.columnIndexFromSortColumnId(TrackModel::SortColumnId::Key));

    model.sort(keyColumn, Qt::AscendingOrder);
    model.select();
    ASSERT_EQ(3, model.rowCount());
    EXPECT_EQ(QStringLiteral("C"), model.index(0, keyColumn).data().toString());
    EXPECT_EQ(QStringLiteral("G"), model.index(1, keyColumn).data().toString());
    EXPECT_EQ(QStringLiteral("D"), model.index(2, keyColumn).data().toString());

    model.sort(keyColumn, Qt::DescendingOrder);
    model.select();
    ASSERT_EQ(3, model.rowCount());
    EXPECT_EQ(QStringLiteral("D"), model.index(0, keyColumn).data().toString());
    EXPECT_EQ(QStringLiteral("G"), model.index(1, keyColumn).data().toString());
    EXPECT_EQ(QStringLiteral("C"), model.index(2, keyColumn).data().toString());
}
// End AI-generated public PDB import/model regression tests.

} // namespace
