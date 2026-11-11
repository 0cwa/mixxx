#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "library/rekordbox/rekordboxfeature.h"
#include "proto/keys.pb.h"
#include "test/mixxxdbtest.h"

namespace mixxx::rekordbox {
namespace {

// Prepared by ChatGPT.
// Synthetic DeviceSQL fixtures follow the checked-in rekordbox_pdb.ksy and the
// decoder regression fixtures. The production worker creates all SQL tables;
// these tests exercise its commit/rollback boundary through separate threads.
// These are not rekordbox exporter or player qualification fixtures.
// End of ChatGPT-generated documentation.
void put16(std::string* bytes, std::size_t offset, std::uint16_t value) {
    bytes->at(offset) = static_cast<char>(value);
    bytes->at(offset + 1) = static_cast<char>(value >> 8);
}

void put32(std::string* bytes, std::size_t offset, std::uint32_t value) {
    put16(bytes, offset, static_cast<std::uint16_t>(value));
    put16(bytes, offset + 2, static_cast<std::uint16_t>(value >> 16));
}

std::string deviceString(const std::string& text) {
    if (text.size() > 126) {
        throw std::logic_error("Synthetic DeviceSQL short string is too long");
    }
    return std::string(1, static_cast<char>((text.size() + 1) * 2 + 1)) + text;
}

std::string trackRow(std::uint32_t sourceId,
        const std::string& title,
        const std::string& filename,
        std::uint32_t keyId = 0,
        std::uint32_t artworkId = 0,
        const std::string& mediaPath = {}) {
    std::string row(136, '\0');
    put16(&row, 0, 0x24);
    put32(&row, 8, 48000);
    put32(&row, 16, 123456);
    put32(&row, 28, artworkId);
    put32(&row, 32, keyId);
    put32(&row, 48, 320000);
    put32(&row, 52, 17);
    put32(&row, 56, 12850);
    put32(&row, 72, sourceId);
    put16(&row, 76, 2);
    put16(&row, 80, 2026);
    put16(&row, 82, 24);
    put16(&row, 84, 180);
    row[89] = 5;
    row += deviceString("");
    for (std::size_t index = 0; index < 21; ++index) {
        put16(&row, 94 + index * 2, 136);
    }
    const auto addString = [&](std::size_t field, const std::string& value) {
        put16(&row, 94 + field * 2, static_cast<std::uint16_t>(row.size()));
        row += deviceString(value);
    };
    addString(14, "PIONEER/USBANLZ/" + std::to_string(sourceId) + "/ANLZ0000.DAT");
    addString(16, "Source annotation");
    addString(17, title);
    addString(19, filename);
    addString(20, mediaPath.empty() ? "Contents/" + filename : mediaPath);
    return row;
}

std::string keyRow(std::uint32_t sourceId, const std::string& key) {
    std::string row(8, '\0');
    put32(&row, 0, sourceId);
    put32(&row, 4, sourceId);
    return row + deviceString(key);
}

std::string artworkRow(std::uint32_t sourceId, const std::string& path) {
    std::string row(4, '\0');
    put32(&row, 0, sourceId);
    return row + deviceString(path);
}

std::string playlistRow(std::uint32_t sourceId,
        std::uint32_t parent,
        std::uint32_t order,
        bool folder,
        const std::string& name) {
    std::string row(20, '\0');
    put32(&row, 0, parent);
    put32(&row, 8, order);
    put32(&row, 12, sourceId);
    put32(&row, 16, folder ? 1 : 0);
    return row + deviceString(name);
}

std::string playlistEntry(
        std::uint32_t playlist, std::uint32_t track, std::uint32_t position) {
    std::string row(12, '\0');
    put32(&row, 0, position);
    put32(&row, 4, track);
    put32(&row, 8, playlist);
    return row;
}

using FixtureTable = std::pair<std::uint32_t, std::vector<std::string>>;

std::string deviceFixture(const std::vector<FixtureTable>& tables) {
    constexpr std::uint32_t kPageSize = 1024;
    const auto pageCount = static_cast<std::uint32_t>(tables.size() + 1);
    if (28 + tables.size() * 16 > kPageSize) {
        throw std::logic_error("Synthetic DeviceSQL table directory is too large");
    }
    std::string bytes(pageCount * kPageSize, '\0');
    put32(&bytes, 4, kPageSize);
    put32(&bytes, 8, static_cast<std::uint32_t>(tables.size()));
    put32(&bytes, 12, pageCount);
    put32(&bytes, 20, 0xf0000001);
    for (std::size_t tableIndex = 0; tableIndex < tables.size(); ++tableIndex) {
        const auto& [type, rows] = tables[tableIndex];
        if (rows.size() > 16) {
            throw std::logic_error("Synthetic DeviceSQL page has too many rows");
        }
        const auto pageIndex = static_cast<std::uint32_t>(tableIndex + 1);
        const auto directory = 28 + 16 * tableIndex;
        put32(&bytes, directory, type);
        put32(&bytes, directory + 8, pageIndex);
        put32(&bytes, directory + 12, pageIndex);
        const auto page = pageIndex * kPageSize;
        put32(&bytes, page + 4, pageIndex);
        put32(&bytes, page + 8, type);
        put32(&bytes, page + 12, pageCount);
        const auto rowCount = static_cast<std::uint32_t>(rows.size());
        const auto packed = rowCount | (rowCount << 13);
        bytes[page + 24] = static_cast<char>(packed);
        bytes[page + 25] = static_cast<char>(packed >> 8);
        bytes[page + 26] = static_cast<char>(packed >> 16);
        bytes[page + 27] = '\x24';
        const std::size_t indexBytes = rows.empty() ? 0 : rows.size() * 2 + 4;
        std::size_t heap = 0;
        for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
            if (40 + heap + rows[rowIndex].size() > kPageSize - indexBytes) {
                throw std::logic_error("Synthetic DeviceSQL page heap is too large");
            }
            bytes.replace(page + 40 + heap, rows[rowIndex].size(), rows[rowIndex]);
            put16(&bytes, page + kPageSize - 6 - rowIndex * 2, static_cast<std::uint16_t>(heap));
            heap += rows[rowIndex].size();
        }
        if (!rows.empty()) {
            put16(&bytes, page + kPageSize - 4, static_cast<std::uint16_t>((1u << rowCount) - 1));
        }
        put16(&bytes, page + 30, static_cast<std::uint16_t>(heap));
        put16(&bytes, page + 28, static_cast<std::uint16_t>(kPageSize - 40 - indexBytes - heap));
    }
    return bytes;
}

constexpr auto kPrimarySourceId = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint32_t kSecondarySourceId = 5000;

std::string collectionFixture() {
    return deviceFixture({
            {0,
                    {trackRow(kPrimarySourceId,
                             "Primary",
                             "primary.wav",
                             42,
                             81),
                            trackRow(kSecondarySourceId,
                                    "Secondary",
                                    "secondary.wav")}},
            {5, {keyRow(42, "C")}},
            {7,
                    {playlistRow(900, 0, 8, true, "Empty folder"),
                            playlistRow(100, 0, 2, true, "Parent"),
                            playlistRow(700, 100, 42, false, "Same label"),
                            playlistRow(701, 100, 43, false, "Same label")}},
            {8,
                    {playlistEntry(700, kPrimarySourceId, 50),
                            playlistEntry(700, kSecondarySourceId, 3),
                            playlistEntry(700, kPrimarySourceId, 20),
                            playlistEntry(700, kPrimarySourceId, 20)}},
            {13, {artworkRow(81, "PIONEER/Artwork/81.jpg")}},
    });
}

bool writeDatabase(const QString& devicePath, const std::string& bytes) {
    const auto directory = QDir(devicePath).filePath(QStringLiteral("PIONEER/rekordbox"));
    if (!QDir().mkpath(directory)) {
        return false;
    }
    QFile file(QDir(directory).filePath(QStringLiteral("export.pdb")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(bytes.data(), static_cast<qint64>(bytes.size())) ==
            static_cast<qint64>(bytes.size()) &&
            file.flush();
}

class RekordboxDeviceImporterTest : public MixxxDbTest {
  protected:
    RekordboxDeviceImporterTest()
            : MixxxDbTest(false) {
    }

    void SetUp() override {
        ASSERT_TRUE(m_device.isValid());
        ASSERT_TRUE(dbConnection().isOpen());
    }

    DeviceImportResult importWorker(
            const QString& devicePath,
            std::shared_ptr<std::atomic<bool>> cancelled = {},
            const QString& deviceName = QStringLiteral("Synthetic USB")) {
        if (!cancelled) {
            cancelled = std::make_shared<std::atomic<bool>>(false);
        }
        DeviceImportResult result;
        std::exception_ptr workerFailure;
        const auto pool = dbConnectionPooler();
        std::thread worker([&] {
            try {
                result = importDeviceDatabase(pool,
                        devicePath,
                        deviceName,
                        SecurityTokenPointer{},
                        cancelled);
            } catch (...) {
                workerFailure = std::current_exception();
            }
        });
        worker.join();
        if (workerFailure) {
            std::rethrow_exception(workerFailure);
        }
        return result;
    }

    bool importSharedMediaSources(const QString& laterDevicePath) {
        if (!writeDatabase(m_device.path(),
                    deviceFixture({
                            {0,
                                    {trackRow(111,
                                            "Earlier metadata",
                                            "shared.wav",
                                            0,
                                            0,
                                            "nested/Contents/shared.wav")}},
                    })) ||
                !writeDatabase(laterDevicePath,
                        deviceFixture({
                                {0,
                                        {trackRow(222,
                                                "Later metadata",
                                                "shared.wav")}},
                        }))) {
            return false;
        }
        const auto earlier = importWorker(m_device.path(), {}, QStringLiteral("Earlier owner"));
        const auto later = importWorker(laterDevicePath, {}, QStringLiteral("Later source"));
        if (!earlier.applied || !later.applied) {
            ADD_FAILURE() << earlier.diagnostics.join(QStringLiteral("; ")).toStdString()
                          << " / " << later.diagnostics.join(QStringLiteral("; ")).toStdString();
            return false;
        }
        return true;
    }

    int rowCount(const QString& table) const {
        const auto database = dbConnection();
        if (!database.tables().contains(table)) {
            return 0;
        }
        QSqlQuery query(database);
        if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM ") + table) || !query.next()) {
            ADD_FAILURE() << query.lastError().text().toStdString();
            return -1;
        }
        return query.value(0).toInt();
    }

    QString playlistIdentity(std::uint32_t sourceId) const {
        return m_device.path() + QStringLiteral("-->") + QString::number(sourceId);
    }

    int playlistId(std::uint32_t sourceId) const {
        QSqlQuery query(dbConnection());
        query.prepare(QStringLiteral("SELECT id FROM rekordbox_playlists WHERE name = :name"));
        query.bindValue(QStringLiteral(":name"), playlistIdentity(sourceId));
        if (!query.exec() || !query.next()) {
            ADD_FAILURE() << query.lastError().text().toStdString();
            return -1;
        }
        return query.value(0).toInt();
    }

    QTemporaryDir m_device;
};

TEST_F(RekordboxDeviceImporterTest, WorkerPreservesSourceIdsAndNormalizesKeyMetadata) {
    ASSERT_TRUE(writeDatabase(m_device.path(), collectionFixture()));
    const auto result = importWorker(m_device.path());
    ASSERT_TRUE(result.applied) << result.diagnostics.join(QStringLiteral("; ")).toStdString();
    ASSERT_TRUE(result.data);
    ASSERT_EQ(result.data->tracks.size(), 2u);
    EXPECT_EQ(result.data->sourceGeneration, 0xf0000001u);
    EXPECT_EQ(result.data->tracks.front().sourceTrackId, kPrimarySourceId);
    EXPECT_EQ(result.data->tracks.front().keyId, 42u);
    EXPECT_EQ(result.data->tracks.front().keyTextUtf8, "C");
    EXPECT_EQ(result.data->tracks.front().artworkPathUtf8, "PIONEER/Artwork/81.jpg");

    QSqlQuery query(dbConnection());
    ASSERT_TRUE(query.exec(QStringLiteral(
            "SELECT id, rb_id, key, key_id, location, analyze_path, coverart, title, bpm "
            "FROM rekordbox_library WHERE title = 'Primary'")))
            << query.lastError().text().toStdString();
    ASSERT_TRUE(query.next());
    EXPECT_GT(query.value(0).toLongLong(), 0);
    EXPECT_LE(query.value(0).toLongLong(), std::numeric_limits<int>::max());
    EXPECT_NE(query.value(0).toULongLong(), static_cast<qulonglong>(kPrimarySourceId));
    EXPECT_EQ(query.value(1).toULongLong(), static_cast<qulonglong>(kPrimarySourceId));
    EXPECT_EQ(query.value(2).toString(), QStringLiteral("C"));
    EXPECT_EQ(query.value(3).toInt(), static_cast<int>(track::io::key::C_MAJOR));
    EXPECT_NE(query.value(3).toInt(), 42);
    EXPECT_EQ(query.value(4).toString(),
            QDir(QDir(m_device.path()).canonicalPath())
                    .filePath(QStringLiteral("Contents/primary.wav")));
    EXPECT_EQ(query.value(5).toString(),
            QDir(QDir(m_device.path()).canonicalPath())
                    .filePath(QStringLiteral(
                            "PIONEER/USBANLZ/4294967295/ANLZ0000.DAT")));
    EXPECT_EQ(query.value(6).toString(),
            QDir(QDir(m_device.path()).canonicalPath())
                    .filePath(QStringLiteral("PIONEER/Artwork/81.jpg")));
    EXPECT_EQ(query.value(7).toString(), QStringLiteral("Primary"));
    EXPECT_DOUBLE_EQ(query.value(8).toDouble(), 128.5);
    EXPECT_FALSE(query.next());
    EXPECT_EQ(rowCount(QStringLiteral("rekordbox_library")), 2);
    EXPECT_FALSE(QFile::exists(QDir(m_device.path())
                    .filePath(QStringLiteral("Contents/primary.wav"))));
}

TEST_F(RekordboxDeviceImporterTest, WorkerMapsRepeatedSparsePlaylistEntriesAndEmptyHierarchy) {
    ASSERT_TRUE(writeDatabase(m_device.path(), collectionFixture()));
    const auto result = importWorker(m_device.path());
    ASSERT_TRUE(result.applied) << result.diagnostics.join(QStringLiteral("; ")).toStdString();
    ASSERT_TRUE(result.data);
    ASSERT_EQ(result.data->playlists.size(), 4u);
    const auto empty = std::find_if(result.data->playlists.begin(),
            result.data->playlists.end(),
            [](const auto& playlist) {
                return playlist.sourcePlaylistId == 900;
            });
    ASSERT_NE(empty, result.data->playlists.end());
    EXPECT_TRUE(empty->isFolder);
    EXPECT_EQ(empty->nameUtf8, "Empty folder");
    EXPECT_EQ(std::count_if(result.data->playlists.begin(),
                      result.data->playlists.end(),
                      [](const auto& playlist) {
                          return playlist.nameUtf8 == "Same label" &&
                                  playlist.parentPlaylistId == 100;
                      }),
            2);

    const auto firstPlaylist = playlistId(700);
    const auto duplicateLabelPlaylist = playlistId(701);
    const auto emptyFolder = playlistId(900);
    EXPECT_GT(firstPlaylist, 0);
    EXPECT_GT(duplicateLabelPlaylist, 0);
    EXPECT_GT(emptyFolder, 0);
    EXPECT_NE(firstPlaylist, duplicateLabelPlaylist);
    EXPECT_NE(firstPlaylist, 700);
    EXPECT_EQ(rowCount(QStringLiteral("rekordbox_playlists")), 5);

    QSqlQuery entries(dbConnection());
    entries.prepare(QStringLiteral(
            "SELECT t.rb_id, e.position, e.track_id, t.id "
            "FROM rekordbox_playlist_tracks e "
            "JOIN rekordbox_library t ON t.id = e.track_id "
            "WHERE e.playlist_id = :playlist ORDER BY e.position, e.id"));
    entries.bindValue(QStringLiteral(":playlist"), firstPlaylist);
    ASSERT_TRUE(entries.exec()) << entries.lastError().text().toStdString();
    const std::vector<std::pair<std::uint32_t, int>> expected{
            {kSecondarySourceId, 3},
            {kPrimarySourceId, 20},
            {kPrimarySourceId, 20},
            {kPrimarySourceId, 50}};
    for (const auto& [sourceId, position] : expected) {
        ASSERT_TRUE(entries.next());
        EXPECT_EQ(entries.value(0).toULongLong(), static_cast<qulonglong>(sourceId));
        EXPECT_EQ(entries.value(1).toInt(), position);
        EXPECT_EQ(entries.value(2).toInt(), entries.value(3).toInt());
        EXPECT_NE(entries.value(2).toULongLong(), static_cast<qulonglong>(sourceId));
    }
    EXPECT_FALSE(entries.next());

    QSqlQuery emptyEntries(dbConnection());
    emptyEntries.prepare(
            QStringLiteral("SELECT COUNT(*) FROM rekordbox_playlist_tracks "
                           "WHERE playlist_id IN (:folder, :playlist)"));
    emptyEntries.bindValue(QStringLiteral(":folder"), emptyFolder);
    emptyEntries.bindValue(QStringLiteral(":playlist"), duplicateLabelPlaylist);
    ASSERT_TRUE(emptyEntries.exec());
    ASSERT_TRUE(emptyEntries.next());
    EXPECT_EQ(emptyEntries.value(0).toInt(), 0);
}

TEST_F(RekordboxDeviceImporterTest, CorruptThenRepairedSourceCanBeRetriedOnAnotherWorker) {
    ASSERT_TRUE(writeDatabase(m_device.path(), "invalid DeviceSQL input"));
    const auto corrupt = importWorker(m_device.path());
    EXPECT_FALSE(corrupt.applied);
    EXPECT_FALSE(corrupt.data);
    EXPECT_FALSE(corrupt.diagnostics.empty());
    EXPECT_EQ(rowCount(QStringLiteral("rekordbox_library")), 0);
    EXPECT_EQ(rowCount(QStringLiteral("rekordbox_playlists")), 0);
    EXPECT_EQ(rowCount(QStringLiteral("rekordbox_playlist_tracks")), 0);

    ASSERT_TRUE(writeDatabase(m_device.path(), collectionFixture()));
    const auto repaired = importWorker(m_device.path());
    ASSERT_TRUE(repaired.applied) << repaired.diagnostics.join(QStringLiteral("; ")).toStdString();
    EXPECT_NE(corrupt.sourceFingerprint, repaired.sourceFingerprint);
    EXPECT_EQ(rowCount(QStringLiteral("rekordbox_library")), 2);
    EXPECT_EQ(rowCount(QStringLiteral("rekordbox_playlists")), 5);
    EXPECT_EQ(rowCount(QStringLiteral("rekordbox_playlist_tracks")), 6);
}

TEST_F(RekordboxDeviceImporterTest, CancelledWorkerCommitsNoRowsAndCanBeRetried) {
    ASSERT_TRUE(writeDatabase(m_device.path(), collectionFixture()));
    const auto cancelled = std::make_shared<std::atomic<bool>>(true);
    const auto stopped = importWorker(m_device.path(), cancelled);
    EXPECT_FALSE(stopped.applied);
    EXPECT_FALSE(stopped.data);
    EXPECT_FALSE(stopped.diagnostics.empty());
    EXPECT_EQ(rowCount(QStringLiteral("rekordbox_library")), 0);
    EXPECT_EQ(rowCount(QStringLiteral("rekordbox_playlists")), 0);
    EXPECT_EQ(rowCount(QStringLiteral("rekordbox_playlist_tracks")), 0);

    cancelled->store(false);
    const auto retried = importWorker(m_device.path(), cancelled);
    ASSERT_TRUE(retried.applied) << retried.diagnostics.join(QStringLiteral("; ")).toStdString();
    EXPECT_EQ(rowCount(QStringLiteral("rekordbox_library")), 2);
    EXPECT_EQ(rowCount(QStringLiteral("rekordbox_playlists")), 5);
    EXPECT_EQ(rowCount(QStringLiteral("rekordbox_playlist_tracks")), 6);
}

TEST_F(RekordboxDeviceImporterTest, LateSqlFailureRollsBackAllDeviceRowsBeforeRetry) {
    ASSERT_TRUE(writeDatabase(m_device.path(), deviceFixture({})));
    const auto initialized = importWorker(m_device.path());
    ASSERT_TRUE(initialized.applied)
            << initialized.diagnostics.join(QStringLiteral("; ")).toStdString();
    ASSERT_EQ(rowCount(QStringLiteral("rekordbox_playlists")), 1);
    QSqlQuery trigger(dbConnection());
    ASSERT_TRUE(trigger.exec(QStringLiteral(
            "CREATE TRIGGER fail_late_device_entry BEFORE INSERT ON rekordbox_playlist_tracks "
            "WHEN NEW.position = 20 "
            "BEGIN SELECT RAISE(ABORT, 'synthetic late transaction failure'); END")))
            << trigger.lastError().text().toStdString();

    QTemporaryDir secondDevice;
    ASSERT_TRUE(secondDevice.isValid());
    ASSERT_TRUE(writeDatabase(secondDevice.path(), collectionFixture()));
    const auto failed = importWorker(secondDevice.path());
    EXPECT_FALSE(failed.applied);
    ASSERT_TRUE(failed.data);
    EXPECT_TRUE(failed.diagnostics.join(QStringLiteral("; "))
                    .contains(QStringLiteral("synthetic late transaction failure")));
    EXPECT_EQ(rowCount(QStringLiteral("rekordbox_library")), 0);
    EXPECT_EQ(rowCount(QStringLiteral("rekordbox_playlists")), 1);
    EXPECT_EQ(rowCount(QStringLiteral("rekordbox_playlist_tracks")), 0);

    ASSERT_TRUE(trigger.exec(QStringLiteral("DROP TRIGGER fail_late_device_entry")));
    const auto retried = importWorker(secondDevice.path());
    ASSERT_TRUE(retried.applied) << retried.diagnostics.join(QStringLiteral("; ")).toStdString();
    EXPECT_EQ(retried.sourceFingerprint, failed.sourceFingerprint);
    EXPECT_EQ(rowCount(QStringLiteral("rekordbox_library")), 2);
    EXPECT_EQ(rowCount(QStringLiteral("rekordbox_playlists")), 6);
    EXPECT_EQ(rowCount(QStringLiteral("rekordbox_playlist_tracks")), 6);
}

TEST_F(RekordboxDeviceImporterTest, RemovalTreatsPercentAndUnderscoreInSourcePathsLiterally) {
    const QString removedPath = m_device.filePath(QStringLiteral("USB_100%"));
    const QString remainingPath = m_device.filePath(QStringLiteral("USBX100extra"));
    ASSERT_TRUE(writeDatabase(removedPath, collectionFixture()));
    ASSERT_TRUE(writeDatabase(remainingPath, collectionFixture()));
    const auto removed = importWorker(removedPath);
    const auto remaining = importWorker(remainingPath);
    ASSERT_TRUE(removed.applied) << removed.diagnostics.join(QStringLiteral("; ")).toStdString();
    ASSERT_TRUE(remaining.applied)
            << remaining.diagnostics.join(QStringLiteral("; ")).toStdString();
    ASSERT_EQ(rowCount(QStringLiteral("rekordbox_library")), 4);
    ASSERT_EQ(rowCount(QStringLiteral("rekordbox_playlists")), 10);
    ASSERT_EQ(rowCount(QStringLiteral("rekordbox_playlist_tracks")), 12);

    auto database = dbConnection();
    ASSERT_TRUE(removeDeviceDatabase(database, removedPath));
    EXPECT_EQ(rowCount(QStringLiteral("rekordbox_library")), 2);
    EXPECT_EQ(rowCount(QStringLiteral("rekordbox_playlists")), 5);
    EXPECT_EQ(rowCount(QStringLiteral("rekordbox_playlist_tracks")), 6);
    QSqlQuery query(database);
    ASSERT_TRUE(query.exec(QStringLiteral("SELECT name FROM rekordbox_playlists ORDER BY name")));
    QStringList actualNames;
    while (query.next()) {
        actualNames.append(query.value(0).toString());
    }
    QStringList expectedNames{remainingPath};
    for (const auto id : {100, 700, 701, 900}) {
        expectedNames.append(remainingPath + QStringLiteral("-->") + QString::number(id));
    }
    expectedNames.sort();
    EXPECT_EQ(actualNames, expectedNames);
}

TEST_F(RekordboxDeviceImporterTest, RemovalPreservesMediaReferencedByAnotherSource) {
    const QString laterDevicePath = m_device.filePath(QStringLiteral("nested"));
    ASSERT_TRUE(importSharedMediaSources(laterDevicePath));
    ASSERT_EQ(rowCount(QStringLiteral("rekordbox_library")), 1);
    ASSERT_EQ(rowCount(QStringLiteral("rekordbox_playlists")), 2);
    ASSERT_EQ(rowCount(QStringLiteral("rekordbox_playlist_tracks")), 2);

    auto database = dbConnection();
    ASSERT_TRUE(removeDeviceDatabase(database, m_device.path()));
    EXPECT_EQ(rowCount(QStringLiteral("rekordbox_library")), 1);
    EXPECT_EQ(rowCount(QStringLiteral("rekordbox_playlists")), 1);
    EXPECT_EQ(rowCount(QStringLiteral("rekordbox_playlist_tracks")), 1);
    QSqlQuery query(database);
    ASSERT_TRUE(query.exec(QStringLiteral(
            "SELECT p.name, t.device, t.location FROM rekordbox_playlist_tracks e "
            "JOIN rekordbox_playlists p ON p.id = e.playlist_id "
            "JOIN rekordbox_library t ON t.id = e.track_id")));
    ASSERT_TRUE(query.next());
    EXPECT_EQ(query.value(0).toString(), laterDevicePath);
    EXPECT_EQ(query.value(1).toString(), QStringLiteral("Earlier owner"));
    EXPECT_EQ(query.value(2).toString(),
            QDir(QDir(laterDevicePath).canonicalPath())
                    .filePath(QStringLiteral("Contents/shared.wav")));
    EXPECT_FALSE(query.next());
}

TEST_F(RekordboxDeviceImporterTest, RemovingFinalSourceDeletesRowsOwnedByAnEarlierSource) {
    const QString laterDevicePath = m_device.filePath(QStringLiteral("nested"));
    ASSERT_TRUE(importSharedMediaSources(laterDevicePath));
    auto database = dbConnection();
    ASSERT_TRUE(removeDeviceDatabase(database, m_device.path()));
    ASSERT_EQ(rowCount(QStringLiteral("rekordbox_library")), 1);
    QSqlQuery owner(database);
    ASSERT_TRUE(owner.exec(QStringLiteral("SELECT device FROM rekordbox_library")));
    ASSERT_TRUE(owner.next());
    ASSERT_EQ(owner.value(0).toString(), QStringLiteral("Earlier owner"));
    owner.finish();

    ASSERT_TRUE(removeDeviceDatabase(database, laterDevicePath));
    EXPECT_EQ(rowCount(QStringLiteral("rekordbox_library")), 0);
    EXPECT_EQ(rowCount(QStringLiteral("rekordbox_playlists")), 0);
    EXPECT_EQ(rowCount(QStringLiteral("rekordbox_playlist_tracks")), 0);
}

TEST_F(RekordboxDeviceImporterTest, RemovedSourceCanReimportChangedMetadataAtTheSamePath) {
    ASSERT_TRUE(writeDatabase(m_device.path(),
            deviceFixture({
                    {0, {trackRow(111, "Before removal", "same.wav")}},
            })));
    const auto original = importWorker(m_device.path());
    ASSERT_TRUE(original.applied) << original.diagnostics.join(QStringLiteral("; ")).toStdString();
    auto database = dbConnection();
    ASSERT_TRUE(removeDeviceDatabase(database, m_device.path()));
    ASSERT_EQ(rowCount(QStringLiteral("rekordbox_library")), 0);
    ASSERT_EQ(rowCount(QStringLiteral("rekordbox_playlists")), 0);
    ASSERT_EQ(rowCount(QStringLiteral("rekordbox_playlist_tracks")), 0);

    ASSERT_TRUE(writeDatabase(m_device.path(),
            deviceFixture({
                    {0, {trackRow(999, "After removal", "same.wav")}},
            })));
    const auto changed = importWorker(m_device.path());
    ASSERT_TRUE(changed.applied) << changed.diagnostics.join(QStringLiteral("; ")).toStdString();
    EXPECT_NE(changed.sourceFingerprint, original.sourceFingerprint);
    EXPECT_EQ(rowCount(QStringLiteral("rekordbox_library")), 1);
    EXPECT_EQ(rowCount(QStringLiteral("rekordbox_playlists")), 1);
    EXPECT_EQ(rowCount(QStringLiteral("rekordbox_playlist_tracks")), 1);
    QSqlQuery query(database);
    ASSERT_TRUE(query.exec(QStringLiteral(
            "SELECT rb_id, title, analyze_path FROM rekordbox_library")));
    ASSERT_TRUE(query.next());
    EXPECT_EQ(query.value(0).toULongLong(), 999u);
    EXPECT_EQ(query.value(1).toString(), QStringLiteral("After removal"));
    EXPECT_EQ(query.value(2).toString(),
            QDir(QDir(m_device.path()).canonicalPath())
                    .filePath(QStringLiteral(
                            "PIONEER/USBANLZ/999/ANLZ0000.DAT")));
    EXPECT_FALSE(query.next());
}

} // namespace
} // namespace mixxx::rekordbox
