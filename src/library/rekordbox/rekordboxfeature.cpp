#include "library/rekordbox/rekordboxfeature.h"

#include <mp3guessenc.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMap>
#include <QMessageBox>
#include <QSet>
#include <QSettings>
#include <QString>
#include <QUrl>
#include <QtDebug>
#include <algorithm>
#include <functional>
#include <utility>

#include "engine/engine.h"
#include "library/dao/trackschema.h"
#include "library/library.h"
#include "library/memorycuepromotionconfig.h"
#include "library/queryutil.h"
#include "library/rekordbox/rekordboxconstants.h"
#include "library/rekordbox/rekordboxxmlparser.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "library/treeitem.h"
#include "moc_rekordboxfeature.cpp"
#include "track/beats.h"
#include "track/cue.h"
#include "track/keyfactory.h"
#include "track/keyutils.h"
#include "track/track.h"
#include "util/color/color.h"
#include "util/db/dbconnectionpooled.h"
#include "util/db/dbconnectionpooler.h"
#include "util/sandbox.h"
#include "waveform/waveform.h"
#include "widget/wlibrary.h"
#include "widget/wlibrarytextbrowser.h"

#define IS_RECORDBOX_DEVICE "::isRecordboxDevice::"
#define IS_NOT_RECORDBOX_DEVICE "::isNotRecordboxDevice::"

namespace {

const QString kRekordboxLibraryTable = QStringLiteral("rekordbox_library");
const QString kRekordboxPlaylistsTable = QStringLiteral("rekordbox_playlists");
const QString kRekordboxPlaylistTracksTable = QStringLiteral("rekordbox_playlist_tracks");

const QString kPdbPath = QStringLiteral("PIONEER/rekordbox/export.pdb");
const QString kPLaylistPathDelimiter = QStringLiteral("-->");
const QString kRekordboxXmlImportLink = QStringLiteral("import-xml");
const QString kRekordboxXmlMetadata = QStringLiteral("xml_metadata");

QString canonicalTrackLocation(const QString& location) {
    if (location.isEmpty()) {
        return {};
    }

    const QUrl locationUrl(location);
    const bool isLocalLocation = locationUrl.scheme().isEmpty() ||
            QDir::isAbsolutePath(location) || locationUrl.isLocalFile();
    if (!isLocalLocation) {
        return location;
    }

    const QString localPath = locationUrl.isLocalFile() ? locationUrl.toLocalFile() : location;
    const QFileInfo fileInfo(localPath);
    const QString canonicalPath = fileInfo.canonicalFilePath();
    if (!canonicalPath.isEmpty()) {
        return QDir::cleanPath(canonicalPath);
    }
    return QDir::cleanPath(fileInfo.absoluteFilePath());
}

int findTrackIdByCanonicalLocation(
        QSqlDatabase& database,
        const QString& location) {
    const QString canonicalLocation = canonicalTrackLocation(location);
    if (canonicalLocation.isEmpty()) {
        return -1;
    }

    QSqlQuery query(database);
    query.prepare(
            "SELECT id, location FROM " + kRekordboxLibraryTable +
            " WHERE location=:location ORDER BY id");
    query.bindValue(":location", location);
    if (!query.exec()) {
        LOG_FAILED_QUERY(query) << "location:" << location;
        return -1;
    }
    if (query.next()) {
        return query.value(0).toInt();
    }

    query.prepare(
            "SELECT id, location FROM " + kRekordboxLibraryTable +
            " WHERE location IS NOT NULL ORDER BY id");
    if (!query.exec()) {
        LOG_FAILED_QUERY(query) << "location:" << location;
        return -1;
    }
    while (query.next()) {
        if (canonicalTrackLocation(query.value(1).toString()) == canonicalLocation) {
            return query.value(0).toInt();
        }
    }
    return -1;
}

bool createLibraryTable(QSqlDatabase& database, const QString& tableName) {
    qDebug() << "Creating Rekordbox library table: " << tableName;

    QSqlQuery query(database);
    query.prepare(
            "CREATE TABLE IF NOT EXISTS " + tableName +
            " ("
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "    rb_id INTEGER,"
            "    artist TEXT,"
            "    title TEXT,"
            "    album TEXT,"
            "    year INTEGER,"
            "    genre TEXT,"
            "    tracknumber TEXT,"
            "    location TEXT UNIQUE,"
            "    comment TEXT,"
            "    duration INTEGER,"
            "    bitrate TEXT,"
            "    bpm FLOAT,"
            "    key TEXT,"
            "    key_id INTEGER,"
            "    rating INTEGER,"
            "    analyze_path TEXT UNIQUE,"
            "    device TEXT,"
            "    color INTEGER,"
            "    coverart TEXT,"
            "    xml_metadata TEXT"
            ");");

    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }

    return true;
}

bool createPlaylistsTable(QSqlDatabase& database, const QString& tableName) {
    qDebug() << "Creating Rekordbox playlists table: " << tableName;

    QSqlQuery query(database);
    query.prepare(
            "CREATE TABLE IF NOT EXISTS " + tableName +
            " ("
            "    id INTEGER PRIMARY KEY,"
            "    name TEXT UNIQUE"
            ");");

    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }

    return true;
}

bool createPlaylistTracksTable(QSqlDatabase& database, const QString& tableName) {
    qDebug() << "Creating Rekordbox playlist tracks table: " << tableName;

    QSqlQuery query(database);
    query.prepare(
            "CREATE TABLE IF NOT EXISTS " + tableName +
            " ("
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "    playlist_id INTEGER REFERENCES rekordbox_playlists(id),"
            "    track_id INTEGER REFERENCES rekordbox_library(id),"
            "    position INTEGER"
            ");");

    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }

    return true;
}

bool dropTable(QSqlDatabase& database, const QString& tableName) {
    qDebug() << "Dropping Rekordbox table: " << tableName;

    QSqlQuery query(database);
    query.prepare("DROP TABLE IF EXISTS " + tableName);

    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }

    return true;
}

// This function is executed in a separate thread other than the main thread
// The returned list owns the pointers, but we can't use a unique_ptr because
// the result is passed by a const reference inside QFuture and than copied
// to the main thread requiring a copy-able object.
QList<TreeItem*> findRekordboxDevices() {
    QThread* thisThread = QThread::currentThread();
    thisThread->setPriority(QThread::LowPriority);

    QList<TreeItem*> foundDevices;

#if defined(__WINDOWS__)
    // Repopulate drive list
    QFileInfoList drives = QDir::drives();
    // show drive letters
    foreach (QFileInfo drive, drives) {
        // Using drive.filePath() instead of drive.canonicalPath() as it
        // freezes interface too much if there is a network share mounted
        // (drive letter assigned) but unavailable
        //
        // drive.canonicalPath() make a system call to the underlying filesystem
        // introducing delay if it is unreadable.
        // drive.filePath() doesn't make any access to the filesystem and consequently
        // shorten the delay

        QFileInfo rbDBFileInfo(drive.filePath() + kPdbPath);

        if (rbDBFileInfo.exists() && rbDBFileInfo.isFile()) {
            QString displayPath = drive.filePath();
            if (displayPath.endsWith("/")) {
                displayPath.chop(1);
            }
            QList<QString> data;
            data << drive.filePath();
            data << IS_RECORDBOX_DEVICE;
            auto* pFoundDevice = new TreeItem(
                    std::move(displayPath),
                    QVariant(data));
            foundDevices << pFoundDevice;
        }
    }
#elif defined(__LINUX__)
    // To get devices on Linux, we look for directories under /media and
    // /run/media/$USER.
    QFileInfoList devices;

    // Add folders under /media to devices.
    devices += QDir(QStringLiteral("/media")).entryInfoList(QDir::AllDirs | QDir::NoDotAndDotDot);

    // Add folders under /media/$USER to devices.
    QDir mediaUserDir(QStringLiteral("/media/") + QString::fromLocal8Bit(qgetenv("USER")));
    devices += mediaUserDir.entryInfoList(
            QDir::AllDirs | QDir::NoDotAndDotDot);

    // Add folders under /run/media/$USER to devices.
    QDir runMediaUserDir(QStringLiteral("/run/media/") + QString::fromLocal8Bit(qgetenv("USER")));
    devices += runMediaUserDir.entryInfoList(
            QDir::AllDirs | QDir::NoDotAndDotDot);

    foreach (QFileInfo device, devices) {
        QFileInfo rbDBFileInfo(device.filePath() + QStringLiteral("/") + kPdbPath);

        if (rbDBFileInfo.exists() && rbDBFileInfo.isFile()) {
            auto* pFoundDevice = new TreeItem(
                    device.fileName(),
                    QVariant(QList<QString>{device.filePath(), IS_RECORDBOX_DEVICE}));
            foundDevices << pFoundDevice;
        }
    }
#else // __APPLE__
    QFileInfoList devices = QDir(QStringLiteral("/Volumes")).entryInfoList(QDir::AllDirs | QDir::NoDotAndDotDot);

    foreach (QFileInfo device, devices) {
        QFileInfo rbDBFileInfo(device.filePath() + QStringLiteral("/") + kPdbPath);

        if (rbDBFileInfo.exists() && rbDBFileInfo.isFile()) {
            QList<QString> data;
            data << device.filePath();
            data << IS_RECORDBOX_DEVICE;
            auto* pFoundDevice = new TreeItem(
                    device.fileName(),
                    QVariant(data));
            foundDevices << pFoundDevice;
        }
    }
#endif

    return foundDevices;
}

QString devicePlaylistPath(const QString& devicePath, std::uint32_t sourceId) {
    return devicePath + kPLaylistPathDelimiter + QString::number(sourceId);
}

QString deviceFilePath(const QString& devicePath, const std::string& sourcePath) {
    if (sourcePath.empty() || sourcePath.find('\0') != std::string::npos) {
        return {};
    }
    QString relative = QDir::fromNativeSeparators(QString::fromStdString(sourcePath));
    while (relative.startsWith(QLatin1Char('/'))) {
        relative.remove(0, 1);
    }
    if (relative.isEmpty()) {
        return {};
    }
    const QString root = QDir::fromNativeSeparators(canonicalTrackLocation(devicePath));
    QFileInfo ancestor(QDir::cleanPath(QDir(root).filePath(relative)));
    QStringList missingComponents;
    while (!ancestor.exists()) {
        if (ancestor.isSymLink()) {
            return {};
        }
        const QString name = ancestor.fileName();
        const QString parent = ancestor.dir().absolutePath();
        if (name.isEmpty() || parent == ancestor.absoluteFilePath()) {
            return {};
        }
        missingComponents.prepend(name);
        ancestor.setFile(parent);
    }
    QString result = QDir::fromNativeSeparators(ancestor.canonicalFilePath());
    if (result.isEmpty()) {
        return {};
    }
    for (const auto& component : std::as_const(missingComponents)) {
        result = QDir(result).filePath(component);
    }
    const QString prefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
    return result.startsWith(prefix) ? result : QString();
}

mixxx::rekordbox::DeviceImportResult parseDeviceDB(
        mixxx::DbConnectionPoolPtr dbConnectionPool,
        const QString& devicePath,
        const QString& device,
        const SecurityTokenPointer& securityToken,
        const std::shared_ptr<std::atomic<bool>>& cancelled) {
    using namespace mixxx::rekordbox;
    DeviceImportResult result;
    result.devicePath = devicePath;
    result.device = device;
    const auto retainedToken = securityToken;
    try {
        PdbDecodeOptions options;
        options.isCancelled = [cancelled] { return cancelled->load(); };
        const QString dbPath = QDir(devicePath).filePath(kPdbPath);
        QFile file(dbPath);
        if (!file.open(QIODevice::ReadOnly) ||
                file.size() > static_cast<qint64>(options.limits.maxFileBytes)) {
            result.diagnostics << QStringLiteral(
                    "Unavailable or oversized DeviceSQL database: %1")
                                          .arg(dbPath);
            return result;
        }
        const QByteArray bytes = file.read(static_cast<qint64>(options.limits.maxFileBytes) + 1);
        if (file.error() != QFileDevice::NoError) {
            result.diagnostics << file.errorString();
            return result;
        }
        const auto decoded = decodePdb({dbPath.toStdString(),
                                               std::string_view(bytes.constData(), bytes.size())},
                options);
        result.sourceFingerprint = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        for (const auto& diagnostic : decoded.diagnostics) {
            result.diagnostics << QString::fromStdString(diagnostic.message);
        }
        if (!decoded.usable() || cancelled->load()) {
            return result;
        }
        result.data = decoded.data;

        const mixxx::DbConnectionPooler dbConnectionPooler(dbConnectionPool);
        QSqlDatabase database = mixxx::DbConnectionPooled(dbConnectionPool);
        if (!database.isOpen()) {
            result.diagnostics << database.lastError().text();
            return result;
        }
        ScopedTransaction transaction(database);
        if (!transaction.active()) {
            return result;
        }
        if (!createLibraryTable(database, kRekordboxLibraryTable) ||
                !createPlaylistsTable(database, kRekordboxPlaylistsTable) ||
                !createPlaylistTracksTable(database, kRekordboxPlaylistTracksTable)) {
            result.diagnostics << QStringLiteral("Could not initialize DeviceSQL staging tables");
            return result;
        }
        auto failQuery = [&result](const QSqlQuery& query) {
            result.diagnostics << query.lastError().text();
        };
        QHash<QString, int> mediaIds;
        QSqlQuery existing(database);
        if (!existing.exec(QStringLiteral("SELECT id, location FROM ") + kRekordboxLibraryTable)) {
            failQuery(existing);
            return result;
        }
        while (existing.next()) {
            mediaIds.insert(
                    canonicalTrackLocation(existing.value(1).toString()),
                    existing.value(0).toInt());
        }
        QSqlQuery insertPlaylist(database);
        insertPlaylist.prepare(QStringLiteral("INSERT INTO ") + kRekordboxPlaylistsTable +
                QStringLiteral(" (name) VALUES (:name)"));
        insertPlaylist.bindValue(QStringLiteral(":name"), devicePath);
        if (!insertPlaylist.exec()) {
            failQuery(insertPlaylist);
            return result;
        }
        const int rootPlaylistId = insertPlaylist.lastInsertId().toInt();
        QSqlQuery insertEntry(database);
        insertEntry.prepare(QStringLiteral("INSERT INTO ") +
                kRekordboxPlaylistTracksTable +
                QStringLiteral(" (playlist_id, track_id, position) VALUES "
                               "(:playlist, :track, :position)"));
        QSqlQuery insertTrack(database);
        insertTrack.prepare(QStringLiteral("INSERT INTO ") +
                kRekordboxLibraryTable +
                QStringLiteral(" (rb_id, artist, title, album, year, genre, "
                               "comment, tracknumber,"
                               " bpm, bitrate, duration, location, rating, "
                               "key, key_id, analyze_path,"
                               " device, color, coverart) VALUES (:rb_id, "
                               ":artist, :title, :album, :year,"
                               " :genre, :comment, :tracknumber, :bpm, "
                               ":bitrate, :duration, :location,"
                               " :rating, :key, :key_id, :analyze_path, "
                               ":device, :color, :coverart)"));
        QHash<std::uint32_t, int> trackIds;
        int position = 0;
        auto optionalText = [](const std::optional<std::string>& text) {
            return text ? QVariant(QString::fromStdString(*text)) : QVariant();
        };
        for (const auto& track : result.data->tracks) {
            if (cancelled->load()) {
                return result;
            }
            const QString location = deviceFilePath(devicePath, track.mediaPathUtf8());
            if (location.isEmpty()) {
                result.diagnostics << QStringLiteral(
                        "Skipped invalid device media path for track %1")
                                              .arg(track.sourceTrackId);
                continue;
            }
            int trackId = mediaIds.value(location, -1);
            if (trackId < 0) {
                const auto key = optionalText(track.keyTextUtf8);
                const QString analysis = deviceFilePath(devicePath, track.analyzePathUtf8());
                insertTrack.bindValue(QStringLiteral(":rb_id"),
                        static_cast<qulonglong>(track.sourceTrackId));
                insertTrack.bindValue(QStringLiteral(":artist"), optionalText(track.artistUtf8));
                insertTrack.bindValue(QStringLiteral(":title"),
                        QString::fromStdString(track.titleUtf8()));
                insertTrack.bindValue(QStringLiteral(":album"), optionalText(track.albumUtf8));
                insertTrack.bindValue(QStringLiteral(":year"), track.year);
                insertTrack.bindValue(QStringLiteral(":genre"), optionalText(track.genreUtf8));
                insertTrack.bindValue(QStringLiteral(":comment"),
                        QString::fromStdString(track.commentUtf8()));
                insertTrack.bindValue(QStringLiteral(":tracknumber"),
                        static_cast<qulonglong>(track.trackNumber));
                insertTrack.bindValue(QStringLiteral(":bpm"), track.tempoHundredths / 100.0);
                insertTrack.bindValue(QStringLiteral(":bitrate"),
                        static_cast<qulonglong>(track.bitrate));
                insertTrack.bindValue(QStringLiteral(":duration"), track.durationSeconds);
                insertTrack.bindValue(QStringLiteral(":location"), location);
                insertTrack.bindValue(QStringLiteral(":rating"), track.rating);
                insertTrack.bindValue(QStringLiteral(":key"), key);
                insertTrack.bindValue(QStringLiteral(":key_id"),
                        static_cast<int>(
                                KeyUtils::guessKeyFromText(key.toString())));
                insertTrack.bindValue(QStringLiteral(":analyze_path"),
                        analysis.isEmpty() ? QVariant() : QVariant(analysis));
                insertTrack.bindValue(QStringLiteral(":device"), device);
                insertTrack.bindValue(QStringLiteral(":color"),
                        mixxx::RgbColor::toQVariant(
                                anlzPaletteColor(track.colorId)));
                insertTrack.bindValue(QStringLiteral(":coverart"),
                        track.artworkPathUtf8
                                ? QVariant(deviceFilePath(
                                          devicePath, *track.artworkPathUtf8))
                                : QVariant());
                if (!insertTrack.exec()) {
                    failQuery(insertTrack);
                    return result;
                }
                trackId = insertTrack.lastInsertId().toInt();
                mediaIds.insert(location, trackId);
            }
            trackIds.insert(track.sourceTrackId, trackId);
            insertEntry.bindValue(QStringLiteral(":playlist"), rootPlaylistId);
            insertEntry.bindValue(QStringLiteral(":track"), trackId);
            insertEntry.bindValue(QStringLiteral(":position"), position++);
            if (!insertEntry.exec()) {
                failQuery(insertEntry);
                return result;
            }
        }
        QHash<std::uint32_t, int> playlistIds;
        for (const auto& playlist : result.data->playlists) {
            if (cancelled->load()) {
                return result;
            }
            insertPlaylist.bindValue(QStringLiteral(":name"),
                    devicePlaylistPath(devicePath, playlist.sourcePlaylistId));
            if (!insertPlaylist.exec()) {
                failQuery(insertPlaylist);
                return result;
            }
            playlistIds.insert(playlist.sourcePlaylistId, insertPlaylist.lastInsertId().toInt());
        }
        for (const auto& entry : result.data->playlistEntries) {
            if (cancelled->load()) {
                return result;
            }
            if (!playlistIds.contains(entry.sourcePlaylistId) ||
                    !trackIds.contains(entry.sourceTrackId)) {
                result.diagnostics << QStringLiteral("Skipped an unresolved playlist entry");
                continue;
            }
            insertEntry.bindValue(QStringLiteral(":playlist"),
                    playlistIds.value(entry.sourcePlaylistId));
            insertEntry.bindValue(QStringLiteral(":track"), trackIds.value(entry.sourceTrackId));
            insertEntry.bindValue(QStringLiteral(":position"),
                    static_cast<qulonglong>(entry.position));
            if (!insertEntry.exec()) {
                failQuery(insertEntry);
                return result;
            }
        }
        if (cancelled->load()) {
            return result;
        }
        result.applied = transaction.commit();
        if (!result.applied) {
            database.rollback();
        }
    } catch (const std::exception& error) {
        result.diagnostics << QString::fromUtf8(error.what());
    } catch (...) {
        result.diagnostics << QStringLiteral("Unexpected DeviceSQL worker failure");
    }
    return result;
}

QString parseXmlLibrary(
        mixxx::DbConnectionPoolPtr dbConnectionPool,
        const QString& xmlPath) {
    const mixxx::DbConnectionPooler dbConnectionPooler(dbConnectionPool);
    QSqlDatabase database = mixxx::DbConnectionPooled(dbConnectionPool);
    VERIFY_OR_DEBUG_ASSERT(database.isOpen()) {
        return {};
    }

    QFile xmlFile(xmlPath);
    if (!xmlFile.open(QIODevice::ReadOnly)) {
        qWarning() << "Could not open Rekordbox XML:" << xmlPath
                   << xmlFile.errorString();
        return {};
    }

    const mixxx::rekordbox::Library library = mixxx::rekordbox::parseXml(&xmlFile);
    if (library.hasErrors()) {
        qWarning() << "Skipping malformed Rekordbox XML:" << xmlPath;
        return {};
    }

    const QString device = QStringLiteral("XML:") + QFileInfo(xmlPath).absoluteFilePath();
    const QString playlistPrefix = device + kPLaylistPathDelimiter;
    ScopedTransaction transaction(database);

    // XML views are temporary external-library data. Re-importing the same file
    // replaces only its temporary view and never deletes Mixxx Collection data.
    QSqlQuery deletePlaylistTracks(database);
    deletePlaylistTracks.prepare(
            "DELETE FROM " + kRekordboxPlaylistTracksTable +
            " WHERE playlist_id IN (SELECT id FROM " + kRekordboxPlaylistsTable +
            " WHERE name=:root OR substr(name, 1, length(:prefix))=:prefix)");
    deletePlaylistTracks.bindValue(":root", device);
    deletePlaylistTracks.bindValue(":prefix", playlistPrefix);
    if (!deletePlaylistTracks.exec()) {
        LOG_FAILED_QUERY(deletePlaylistTracks);
        return {};
    }

    QSqlQuery deletePlaylists(database);
    deletePlaylists.prepare(
            "DELETE FROM " + kRekordboxPlaylistsTable +
            " WHERE name=:root OR substr(name, 1, length(:prefix))=:prefix");
    deletePlaylists.bindValue(":root", device);
    deletePlaylists.bindValue(":prefix", playlistPrefix);
    if (!deletePlaylists.exec()) {
        LOG_FAILED_QUERY(deletePlaylists);
        return {};
    }

    QSqlQuery deleteTracks(database);
    deleteTracks.prepare(
            "DELETE FROM " + kRekordboxLibraryTable +
            " WHERE NOT EXISTS ("
            "SELECT 1 FROM " +
            kRekordboxPlaylistTracksTable +
            " AS playlist_tracks WHERE playlist_tracks.track_id=" +
            kRekordboxLibraryTable + ".id)");
    if (!deleteTracks.exec()) {
        LOG_FAILED_QUERY(deleteTracks);
        return {};
    }

    QSqlQuery insertTrackQuery(database);
    insertTrackQuery.prepare(
            "INSERT INTO " + kRekordboxLibraryTable +
            " (rb_id, artist, title, album, year, genre, comment, tracknumber, bpm, "
            "bitrate, duration, location, rating, key, analyze_path, device, color, coverart, "
            "xml_metadata) "
            "VALUES (:rb_id, :artist, :title, :album, :year, :genre, :comment, :tracknumber, "
            ":bpm, :bitrate, :duration, :location, :rating, :key, :analyze_path, :device, "
            ":color, :coverart, :xml_metadata)");
    QSqlQuery updateXmlMetadataQuery(database);
    updateXmlMetadataQuery.prepare(
            "UPDATE " + kRekordboxLibraryTable +
            " SET xml_metadata=:xml_metadata WHERE id=:id");

    QHash<int, int> trackIds;
    QSet<QString> importedLocations;
    QVector<int> importedTrackIds;
    for (const mixxx::rekordbox::Track& xmlTrack : library.tracks) {
        // parseXml normalizes each location once, before this import loop.
        const QString& location = xmlTrack.location;
        const QString canonicalLocation = canonicalTrackLocation(location);
        if (canonicalLocation.isEmpty() || trackIds.contains(xmlTrack.id) ||
                importedLocations.contains(canonicalLocation)) {
            continue;
        }

        int trackID = findTrackIdByCanonicalLocation(database, location);
        if (trackID < 0) {
            insertTrackQuery.bindValue(":rb_id", xmlTrack.id);
            insertTrackQuery.bindValue(":artist", xmlTrack.artist);
            insertTrackQuery.bindValue(":title", xmlTrack.title);
            insertTrackQuery.bindValue(":album", xmlTrack.album);
            insertTrackQuery.bindValue(":year", xmlTrack.year);
            insertTrackQuery.bindValue(":genre", xmlTrack.genre);
            insertTrackQuery.bindValue(":comment", xmlTrack.comment);
            insertTrackQuery.bindValue(":tracknumber", xmlTrack.trackNumber);
            insertTrackQuery.bindValue(":bpm", xmlTrack.bpm);
            insertTrackQuery.bindValue(":bitrate", xmlTrack.bitrate);
            insertTrackQuery.bindValue(":duration", xmlTrack.durationSeconds);
            insertTrackQuery.bindValue(":location", location);
            insertTrackQuery.bindValue(":rating", qBound(0, xmlTrack.rating, 5));
            insertTrackQuery.bindValue(":key", xmlTrack.key);
            // XML imports do not have a Rekordbox ANLZ path. Bind SQL NULL rather
            // than an empty string so every imported row satisfies the UNIQUE
            // analyze_path constraint.
            insertTrackQuery.bindValue(":analyze_path", QVariant());
            insertTrackQuery.bindValue(":device", device);
            insertTrackQuery.bindValue(":color", QVariant());
            insertTrackQuery.bindValue(":coverart", xmlTrack.artworkReference);
            insertTrackQuery.bindValue(
                    ":xml_metadata",
                    mixxx::rekordbox::serializeTrackAnnotations(xmlTrack));
            if (!insertTrackQuery.exec()) {
                LOG_FAILED_QUERY(insertTrackQuery) << "XML track ID:" << xmlTrack.id;
            }
            trackID = findTrackIdByCanonicalLocation(database, location);
        } else {
            // Keep XML annotations attached to the reused database identity.
            updateXmlMetadataQuery.bindValue(
                    ":xml_metadata",
                    mixxx::rekordbox::serializeTrackAnnotations(xmlTrack));
            updateXmlMetadataQuery.bindValue(":id", trackID);
            if (!updateXmlMetadataQuery.exec()) {
                LOG_FAILED_QUERY(updateXmlMetadataQuery)
                        << "XML track ID:" << xmlTrack.id << "database ID:" << trackID;
                return {};
            }
        }

        if (trackID < 0) {
            qWarning() << "Skipping Rekordbox XML track without a database ID"
                       << "TrackID:" << xmlTrack.id << "location:" << location;
            return {};
        }

        mixxx::rekordbox::recordXmlTrackMapping(
                &trackIds, &importedTrackIds, xmlTrack.id, trackID);
        importedLocations.insert(canonicalLocation);
    }

    auto insertPlaylist = [&](const QString& name) {
        QSqlQuery query(database);
        query.prepare(
                "INSERT INTO " + kRekordboxPlaylistsTable + " (name) VALUES (:name)");
        query.bindValue(":name", name);
        return query.exec();
    };

    if (!insertPlaylist(device)) {
        return {};
    }

    QSqlQuery insertPlaylistTrack(database);
    insertPlaylistTrack.prepare(
            "INSERT INTO " + kRekordboxPlaylistTracksTable +
            " (playlist_id, track_id, position) VALUES (:playlist_id, :track_id, :position)");
    QSqlQuery rootIdQuery(database);
    rootIdQuery.prepare(
            "SELECT id FROM " + kRekordboxPlaylistsTable + " WHERE name=:name");
    rootIdQuery.bindValue(":name", device);
    if (!rootIdQuery.exec() || !rootIdQuery.next()) {
        return {};
    }
    const int rootPlaylistId = rootIdQuery.value(0).toInt();
    int position = 1;
    for (const int rbTrackId : std::as_const(importedTrackIds)) {
        insertPlaylistTrack.bindValue(":playlist_id", rootPlaylistId);
        insertPlaylistTrack.bindValue(":track_id", trackIds.value(rbTrackId));
        insertPlaylistTrack.bindValue(":position", position++);
        if (!insertPlaylistTrack.exec()) {
            LOG_FAILED_QUERY(insertPlaylistTrack);
            return {};
        }
    }

    std::function<bool(const mixxx::rekordbox::Playlist&, const QString&)> addPlaylist;
    addPlaylist = [&](const mixxx::rekordbox::Playlist& playlist,
                          const QString& parentPath) {
        const QString path = parentPath + kPLaylistPathDelimiter + playlist.name;
        if (!insertPlaylist(path)) {
            return false;
        }
        QSqlQuery playlistIdQuery(database);
        playlistIdQuery.prepare(
                "SELECT id FROM " + kRekordboxPlaylistsTable + " WHERE name=:name");
        playlistIdQuery.bindValue(":name", path);
        if (!playlistIdQuery.exec() || !playlistIdQuery.next()) {
            return false;
        }
        const int playlistId = playlistIdQuery.value(0).toInt();
        int playlistPosition = 1;
        for (const int rbTrackId : playlist.trackIds) {
            if (!trackIds.contains(rbTrackId)) {
                continue;
            }
            insertPlaylistTrack.bindValue(":playlist_id", playlistId);
            insertPlaylistTrack.bindValue(":track_id", trackIds.value(rbTrackId));
            insertPlaylistTrack.bindValue(":position", playlistPosition++);
            if (!insertPlaylistTrack.exec()) {
                return false;
            }
        }
        for (const auto& child : playlist.children) {
            if (!addPlaylist(child, path)) {
                return false;
            }
        }
        return true;
    };

    for (const auto& playlist : library.playlists.children) {
        if (!addPlaylist(playlist, device)) {
            return {};
        }
    }

    if (!transaction.commit()) {
        database.rollback();
        return {};
    }
    return device;
}

bool clearDeviceTables(QSqlDatabase& database, const QString& devicePath) {
    ScopedTransaction transaction(database);
    if (!transaction.active()) {
        return false;
    }
    const QString playlistPrefix = devicePath + kPLaylistPathDelimiter;

    QSqlQuery deletePlaylistTracksQuery(database);
    deletePlaylistTracksQuery.prepare(
            "DELETE FROM " + kRekordboxPlaylistTracksTable +
            " WHERE playlist_id IN (SELECT id FROM " + kRekordboxPlaylistsTable +
            " WHERE name=:root OR substr(name, 1, length(:prefix))=:prefix)");
    deletePlaylistTracksQuery.bindValue(":root", devicePath);
    deletePlaylistTracksQuery.bindValue(":prefix", playlistPrefix);
    if (!deletePlaylistTracksQuery.exec()) {
        LOG_FAILED_QUERY(deletePlaylistTracksQuery)
                << "device:" << devicePath;
        return false;
    }

    QSqlQuery deletePlaylistsQuery(database);
    deletePlaylistsQuery.prepare(
            "DELETE FROM " + kRekordboxPlaylistsTable +
            " WHERE name=:root OR substr(name, 1, length(:prefix))=:prefix");
    deletePlaylistsQuery.bindValue(":root", devicePath);
    deletePlaylistsQuery.bindValue(":prefix", playlistPrefix);
    if (!deletePlaylistsQuery.exec()) {
        LOG_FAILED_QUERY(deletePlaylistsQuery)
                << "device:" << devicePath;
        return false;
    }

    QSqlQuery deleteTracksQuery(database);
    deleteTracksQuery.prepare(
            "DELETE FROM " + kRekordboxLibraryTable +
            " WHERE NOT EXISTS ("
            "SELECT 1 FROM " +
            kRekordboxPlaylistTracksTable +
            " AS playlist_tracks WHERE playlist_tracks.track_id=" +
            kRekordboxLibraryTable + ".id)");
    if (!deleteTracksQuery.exec()) {
        LOG_FAILED_QUERY(deleteTracksQuery)
                << "device:" << devicePath;
        return false;
    }

    if (!transaction.commit()) {
        database.rollback();
        return false;
    }
    return true;
}

void setMemoryCue(TrackPointer track,
        mixxx::audio::FramePos startPosition,
        mixxx::audio::FramePos endPosition,
        int,
        const QString& label,
        mixxx::RgbColor::optional_t color) {
    CuePointer pCue;

    const mixxx::CueType type = mixxx::CueType::Memory;

    // ANLZ data can be read more than once while an external track is
    // requested. Match the complete imported record before creating a cue so
    // repeated reads are idempotent without removing user-created cues.
    const QList<CuePointer> cuePoints = track->getCuePoints();
    for (const CuePointer& trackCue : cuePoints) {
        if (trackCue->getType() == type &&
                trackCue->getPosition() == startPosition &&
                trackCue->getEndPosition() == endPosition &&
                trackCue->getLabel() == label &&
                (!color || trackCue->getColor() == *color)) {
            pCue = trackCue;
            break;
        }
    }

    if (pCue) {
        pCue->setStartAndEndPosition(startPosition, endPosition);
    } else {
        pCue = track->createAndAddCue(
                type,
                Cue::kNoHotCue,
                startPosition,
                endPosition);
    }
    pCue->setLabel(label);
    if (color) {
        pCue->setColor(*color);
    }
}

mixxx::RgbColor::optional_t xmlCueColor(const QString& value) {
    const QStringList components = value.split(QLatin1Char(','));
    if (components.size() != 3) {
        return mixxx::RgbColor::nullopt();
    }
    bool ok = false;
    const int red = components[0].toInt(&ok);
    if (!ok) {
        return mixxx::RgbColor::nullopt();
    }
    const int green = components[1].toInt(&ok);
    if (!ok) {
        return mixxx::RgbColor::nullopt();
    }
    const int blue = components[2].toInt(&ok);
    if (!ok || red < 0 || red > 255 || green < 0 || green > 255 || blue < 0 || blue > 255) {
        return mixxx::RgbColor::nullopt();
    }
    return mixxx::RgbColor::optional(qRgb(red, green, blue));
}

void applyXmlMetadata(
        TrackPointer track,
        mixxx::audio::SampleRate sampleRate,
        const QByteArray& serializedMetadata) {
    if (!track || !sampleRate.isValid() || serializedMetadata.isEmpty()) {
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(serializedMetadata, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        qWarning() << "Skipping malformed Rekordbox XML annotation:" << parseError.errorString();
        return;
    }

    const QJsonObject metadata = document.object();
    QVector<mixxx::audio::FramePos> beatPositions;
    const QJsonArray beatgrid = metadata.value(QStringLiteral("beatgrid")).toArray();
    if (!track->isBpmLocked()) {
        for (const QJsonValue& value : beatgrid) {
            const double seconds = value.toObject()
                                           .value(QStringLiteral("position"))
                                           .toDouble(-1.0);
            if (seconds >= 0.0) {
                beatPositions << mixxx::audio::FramePos(seconds * sampleRate.value());
            }
        }
    }

    if (!beatPositions.isEmpty()) {
        std::sort(beatPositions.begin(), beatPositions.end());
        beatPositions.erase(std::unique(beatPositions.begin(), beatPositions.end()),
                beatPositions.end());
        mixxx::BeatsPointer beats;
        if (beatPositions.size() >= 2) {
            beats = mixxx::Beats::fromBeatPositions(sampleRate, beatPositions);
        } else {
            const double bpm = beatgrid.first().toObject().value(QStringLiteral("bpm")).toDouble();
            if (bpm > 0.0) {
                beats = mixxx::Beats::fromConstTempo(
                        sampleRate, beatPositions.front(), mixxx::Bpm(bpm));
            }
        }
        if (beats && track->trySetBeats(beats)) {
            track->setBpmLocked(true);
        }
    }

    const QJsonArray cues = metadata.value(QStringLiteral("cues")).toArray();
    int importedCueIndex = 0;
    for (const QJsonValue& value : cues) {
        const QJsonObject item = value.toObject();
        const double start = item.value(QStringLiteral("start")).toDouble(-1.0);
        if (start < 0.0) {
            continue;
        }
        const double end = item.value(QStringLiteral("end")).toDouble(-1.0);
        const QString type = item.value(QStringLiteral("type")).toString();
        const bool isLoop = type.compare(QStringLiteral("loop"), Qt::CaseInsensitive) == 0 &&
                end > start;
        setMemoryCue(
                track,
                mixxx::audio::FramePos(start * sampleRate.value()),
                isLoop ? mixxx::audio::FramePos(end * sampleRate.value())
                       : mixxx::audio::kInvalidFramePos,
                importedCueIndex++,
                item.value(QStringLiteral("name")).toString(),
                xmlCueColor(item.value(QStringLiteral("color")).toString()));
    }
}

} // anonymous namespace

namespace mixxx::rekordbox {

DeviceImportResult importDeviceDatabase(mixxx::DbConnectionPoolPtr dbConnectionPool,
        const QString& devicePath,
        const QString& device,
        const SecurityTokenPointer& securityToken,
        const std::shared_ptr<std::atomic<bool>>& cancelled) {
    const auto cancellation = cancelled ? cancelled : std::make_shared<std::atomic<bool>>(false);
    return parseDeviceDB(std::move(dbConnectionPool),
            devicePath,
            device,
            securityToken,
            cancellation);
}

bool removeDeviceDatabase(QSqlDatabase& database, const QString& devicePath) {
    return clearDeviceTables(database, devicePath);
}

QString resolveDeviceMediaPath(const QString& devicePath, const std::string& sourcePath) {
    return deviceFilePath(devicePath, sourcePath);
}

void applyXmlTrackAnnotations(
        TrackPointer track,
        audio::SampleRate sampleRate,
        const QByteArray& serializedMetadata) {
    applyXmlMetadata(std::move(track), sampleRate, serializedMetadata);
}

QList<MemoryCueLoop> normalizeMemoryCueLoops(const QList<MemoryCueLoop>& cues) {
    QList<MemoryCueLoop> normalized;
    for (const MemoryCueLoop& cue : cues) {
        if (cue.endPosition.isValid()) {
            bool replaced = false;
            for (MemoryCueLoop& existing : normalized) {
                if (existing.endPosition.isValid() &&
                        existing.startPosition == cue.startPosition &&
                        existing.endPosition == cue.endPosition &&
                        (existing.fromExtendedSection != cue.fromExtendedSection ||
                                (existing.comment == cue.comment && existing.color == cue.color))) {
                    if (cue.fromExtendedSection && !existing.fromExtendedSection) {
                        existing = cue;
                    }
                    replaced = true;
                    break;
                }
            }
            if (replaced) {
                continue;
            }
        } else {
            bool duplicate = false;
            for (const MemoryCueLoop& existing : normalized) {
                if (!existing.endPosition.isValid() &&
                        existing.startPosition == cue.startPosition &&
                        existing.comment == cue.comment &&
                        existing.color == cue.color) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }
        }
        normalized << cue;
    }

    std::stable_sort(normalized.begin(), normalized.end(), memoryCueLoopLess);
    return normalized;
}

bool memoryCueLoopLess(const MemoryCueLoop& a, const MemoryCueLoop& b) {
    if (a.startPosition != b.startPosition) {
        return a.startPosition < b.startPosition;
    }
    if (a.endPosition.isValid() != b.endPosition.isValid()) {
        return !a.endPosition.isValid();
    }
    if (a.endPosition != b.endPosition) {
        return a.endPosition < b.endPosition;
    }
    if (a.comment != b.comment) {
        return a.comment < b.comment;
    }
    if (a.color.has_value() != b.color.has_value()) {
        return a.color.has_value();
    }
    if (a.color && a.color != b.color) {
        return static_cast<RgbColor::code_t>(*a.color) < static_cast<RgbColor::code_t>(*b.color);
    }
    return a.sourceOrder < b.sourceOrder;
}

QString analyzeImportSourceIdentity(
        const QString& trackLocation,
        const QString& analyzePath) {
    if (trackLocation.isEmpty() || analyzePath.isEmpty()) {
        return {};
    }
    return trackLocation + QLatin1Char('\n') + analyzePath;
}

void recordXmlTrackMapping(
        QHash<int, int>* trackIds,
        QVector<int>* importedTrackIds,
        int xmlTrackId,
        int databaseTrackId) {
    VERIFY_OR_DEBUG_ASSERT(trackIds && importedTrackIds && databaseTrackId >= 0) {
        return;
    }
    trackIds->insert(xmlTrackId, databaseTrackId);
    importedTrackIds->push_back(xmlTrackId);
}

} // namespace mixxx::rekordbox

RekordboxPlaylistModel::RekordboxPlaylistModel(QObject* parent,
        TrackCollectionManager* trackCollectionManager,
        QSharedPointer<BaseTrackCache> trackSource,
        UserSettingsPointer pConfig)
        : BaseExternalPlaylistModel(parent,
                  trackCollectionManager,
                  "mixxx.db.model.rekordbox.playlistmodel",
                  kRekordboxPlaylistsTable,
                  kRekordboxPlaylistTracksTable,
                  trackSource),
          m_pConfig(std::move(pConfig)) {
}

void RekordboxPlaylistModel::initSortColumnMapping() {
    // Add a bijective mapping between the SortColumnIds and column indices
    for (int i = 0; i < static_cast<int>(TrackModel::SortColumnId::IdMax); ++i) {
        m_columnIndexBySortColumnId[i] = -1;
    }

    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Artist)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_ARTIST);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Title)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_TITLE);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Album)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_ALBUM);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::AlbumArtist)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_ALBUMARTIST);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Year)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_YEAR);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Genre)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_GENRE);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Composer)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COMPOSER);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Grouping)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_GROUPING);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::TrackNumber)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_TRACKNUMBER);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::FileType)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_FILETYPE);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::NativeLocation)] =
            fieldIndex(ColumnCache::COLUMN_TRACKLOCATIONSTABLE_LOCATION);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Comment)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COMMENT);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Duration)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_DURATION);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::BitRate)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_BITRATE);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Bpm)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_BPM);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::ReplayGain)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_REPLAYGAIN);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::DateTimeAdded)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_DATETIMEADDED);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::TimesPlayed)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_TIMESPLAYED);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::LastPlayedAt)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_LAST_PLAYED_AT);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Rating)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_RATING);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Key)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_KEY);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Preview)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_PREVIEW);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Color)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COLOR);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::CoverArt)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COVERART);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Position)] =
            fieldIndex(ColumnCache::COLUMN_PLAYLISTTRACKSTABLE_POSITION);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::PlaylistDateTimeAdded)] =
            fieldIndex(ColumnCache::COLUMN_PLAYLISTTRACKSTABLE_DATETIMEADDED);

    m_sortColumnIdByColumnIndex.clear();
    for (int i = static_cast<int>(TrackModel::SortColumnId::IdMin);
            i < static_cast<int>(TrackModel::SortColumnId::IdMax);
            ++i) {
        TrackModel::SortColumnId sortColumn = static_cast<TrackModel::SortColumnId>(i);
        m_sortColumnIdByColumnIndex.insert(
                m_columnIndexBySortColumnId[static_cast<int>(sortColumn)],
                sortColumn);
    }
}

TrackPointer RekordboxPlaylistModel::getTrack(const QModelIndex& index) const {
    qDebug() << "RekordboxTrackModel::getTrack";

    TrackPointer track = BaseExternalPlaylistModel::getTrack(index);
    QString location = getFieldVariant(
            index, ColumnCache::COLUMN_TRACKLOCATIONSTABLE_LOCATION)
                               .toString();

    if (!track) {
        return track;
    }

    if (!QFile(location).exists()) {
        return track;
    }

    // The following code accounts for timing offsets required to
    // correctly align timing information (cue points, loops, beatgrids)
    // exported from Rekordbox. This is caused by different MP3
    // decoders treating MP3s encoded in a variety of different cases
    // differently. The mp3guessenc library is used to determine which
    // case the MP3 is classified in. See the following PR for more
    // detailed information:
    // https://github.com/mixxxdj/mixxx/pull/2119

    int timingOffset = 0;

    if (location.endsWith(".mp3", Qt::CaseInsensitive)) {
        int timingShiftCase = mp3guessenc_timing_shift_case(location.toStdString().c_str());

        qDebug() << "Timing shift case:" << timingShiftCase << "for MP3 file:" << location;

        switch (timingShiftCase) {
#ifdef __COREAUDIO__
        case EXIT_CODE_CASE_A:
            timingOffset = 12;
            break;
        case EXIT_CODE_CASE_B:
            timingOffset = 13;
            break;
        case EXIT_CODE_CASE_C:
            timingOffset = 26;
            break;
        case EXIT_CODE_CASE_D:
            timingOffset = 50;
            break;
#elif defined(__MAD__)
        case EXIT_CODE_CASE_A:
        case EXIT_CODE_CASE_D:
            timingOffset = 26;
            break;
#elif defined(__FFMPEG__)
        case EXIT_CODE_CASE_D:
            timingOffset = 26;
            break;
#endif
        }
    }

#ifdef __COREAUDIO__
    if (location.toLower().endsWith(".m4a")) {
        timingOffset = 48;
    }
#endif

    mixxx::audio::SampleRate sampleRate = track->getSampleRate();
    const bool importAnalyze = sampleRate.isValid();
    if (!importAnalyze) {
        qWarning() << "Skipping Rekordbox ANLZ import with invalid sample rate for"
                   << location;
    }

    QString anlzPath =
            getFieldVariant(index, ColumnCache::COLUMN_REKORDBOX_ANALYZE_PATH)
                    .toString();

    const QString analyzeSourceIdentity = mixxx::rekordbox::analyzeImportSourceIdentity(
            location, anlzPath);
    if (importAnalyze) {
        mixxx::rekordbox::applyXmlTrackAnnotations(
                track,
                sampleRate,
                getFieldVariant(index, kRekordboxXmlMetadata).toByteArray());

        if (!anlzPath.isEmpty()) {
            if (m_analyzeImportStates.size() > 1024) {
                for (auto it = m_analyzeImportStates.begin(); it != m_analyzeImportStates.end();) {
                    if (it->track.expired()) {
                        it = m_analyzeImportStates.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            auto& state = m_analyzeImportStates[analyzeSourceIdentity];
            const auto applied = mixxx::rekordbox::importAnlzSources(track,
                    sampleRate,
                    timingOffset,
                    anlzPath,
                    mixxx::library::loadMainCuePromotionPolicy(m_pConfig),
                    &state);
            for (const auto& diagnostic : applied.diagnostics) {
                qWarning() << "Rekordbox analysis:" << diagnostic;
            }
        }
    }

    // Assume that the key of the file the has been analyzed in Recordbox is correct
    // and prevent the AnalyzerKey from re-analyzing.
    // Form 5.4.3 Key format depends on the preferences option:
    // Classic: Abm,B,Ebm,F#,Bbm,Db,Fm,Ab,…,F#m,A,Dbm,E
    // Alphanumeric (Camelot): 1A,1B,2A,2B,3A,3B,4A,4B,…,11A,11B,12A,12B
    // Not reckognized: 1m, 01A
    // Earlier versions allow any format
    // Decision: We normalize the KeyText here to not write garbage to the
    // file metadata and it is unlikely to loose extra info.
    track->setKeys(KeyFactory::makeBasicKeysNormalized(
            getFieldVariant(index, ColumnCache::COLUMN_LIBRARYTABLE_KEY).toString(),
            mixxx::track::io::key::USER));

    track->setColor(mixxx::RgbColor::fromQVariant(
            getFieldVariant(index, ColumnCache::COLUMN_LIBRARYTABLE_COLOR)));

    return track;
}

bool RekordboxPlaylistModel::isColumnHiddenByDefault(int column) {
    if (column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_BITRATE)) {
        return true;
    }
    return BaseSqlTableModel::isColumnHiddenByDefault(column);
}

bool RekordboxPlaylistModel::isColumnInternal(int column) {
    return column == fieldIndex(ColumnCache::COLUMN_REKORDBOX_ANALYZE_PATH) ||
            column == fieldIndex(kRekordboxXmlMetadata) ||
            BaseExternalPlaylistModel::isColumnInternal(column);
}

RekordboxFeature::RekordboxFeature(
        Library* pLibrary,
        UserSettingsPointer pConfig)
        : BaseExternalLibraryFeature(pLibrary, pConfig, QStringLiteral("rekordbox")),
          m_pSidebarModel(make_parented<TreeItemModel>(this)) {
    QString tableName = kRekordboxLibraryTable;
    QString idColumn = LIBRARYTABLE_ID;
    QStringList columns = {
            LIBRARYTABLE_ID,
            LIBRARYTABLE_ARTIST,
            LIBRARYTABLE_TITLE,
            LIBRARYTABLE_ALBUM,
            LIBRARYTABLE_YEAR,
            LIBRARYTABLE_GENRE,
            LIBRARYTABLE_TRACKNUMBER,
            TRACKLOCATIONSTABLE_LOCATION,
            LIBRARYTABLE_COMMENT,
            LIBRARYTABLE_RATING,
            LIBRARYTABLE_DURATION,
            LIBRARYTABLE_BITRATE,
            LIBRARYTABLE_BPM,
            LIBRARYTABLE_KEY,
            LIBRARYTABLE_COLOR,
            LIBRARYTABLE_COVERART,
            REKORDBOX_ANALYZE_PATH,
            kRekordboxXmlMetadata};

    const QStringList searchColumns = {
            LIBRARYTABLE_ARTIST,
            LIBRARYTABLE_TITLE,
            LIBRARYTABLE_ALBUM,
            LIBRARYTABLE_GENRE,
            LIBRARYTABLE_TRACKNUMBER,
            TRACKLOCATIONSTABLE_LOCATION,
            LIBRARYTABLE_COMMENT};

    m_trackSource = QSharedPointer<BaseTrackCache>::create(
            m_pTrackCollection,
            tableName,
            std::move(idColumn),
            std::move(columns),
            std::move(searchColumns),
            false);
    m_pRekordboxPlaylistModel = make_parented<RekordboxPlaylistModel>(
            this, pLibrary->trackCollectionManager(), m_trackSource, m_pConfig);

    m_title = tr("Rekordbox");

    QSqlDatabase database = m_pTrackCollection->database();
    ScopedTransaction transaction(database);
    // Drop any leftover temporary Rekordbox database tables if they exist
    dropTable(database, kRekordboxPlaylistTracksTable);
    dropTable(database, kRekordboxPlaylistsTable);
    dropTable(database, kRekordboxLibraryTable);

    // Create new temporary Rekordbox database tables
    createLibraryTable(database, kRekordboxLibraryTable);
    createPlaylistsTable(database, kRekordboxPlaylistsTable);
    createPlaylistTracksTable(database, kRekordboxPlaylistTracksTable);
    transaction.commit();

    connect(&m_devicesFutureWatcher,
            &QFutureWatcher<QList<TreeItem*>>::finished,
            this,
            &RekordboxFeature::onRekordboxDevicesFound);
    connect(&m_tracksFutureWatcher,
            &QFutureWatcher<QString>::finished,
            this,
            &RekordboxFeature::onTracksFound);
    connect(&m_deviceImportWatcher,
            &QFutureWatcher<mixxx::rekordbox::DeviceImportResult>::finished,
            this,
            &RekordboxFeature::onDeviceTracksFound);
    // initialize the model
    m_pSidebarModel->setRootItem(TreeItem::newRoot(this));
}

RekordboxFeature::~RekordboxFeature() {
    if (m_cancelDeviceImport) {
        m_cancelDeviceImport->store(true);
    }
    m_devicesFuture.waitForFinished();
    m_tracksFuture.waitForFinished();
    m_deviceImportFuture.waitForFinished();

    // Drop temporary Rekordbox database tables on shutdown
    QSqlDatabase database = m_pTrackCollection->database();
    ScopedTransaction transaction(database);
    dropTable(database, kRekordboxPlaylistTracksTable);
    dropTable(database, kRekordboxPlaylistsTable);
    dropTable(database, kRekordboxLibraryTable);
    transaction.commit();
}

void RekordboxFeature::bindLibraryWidget(WLibrary* pLibraryWidget,
        KeyboardEventFilter* keyboard) {
    Q_UNUSED(keyboard);
    parented_ptr<WLibraryTextBrowser> pEdit = make_parented<WLibraryTextBrowser>(pLibraryWidget);
    pEdit->setHtml(formatRootViewHtml());
    pEdit->setOpenLinks(false);
    connect(pEdit, &WLibraryTextBrowser::anchorClicked, this, &RekordboxFeature::htmlLinkClicked);
    pLibraryWidget->registerView("REKORDBOXHOME", pEdit);
}

void RekordboxFeature::htmlLinkClicked(const QUrl& link) {
    if (QString(link.path()) == "refresh") {
        activate();
    } else if (QString(link.path()) == kRekordboxXmlImportLink) {
        if (m_pendingOperation != PendingOperation::None) {
            return;
        }
        m_pendingOperation = PendingOperation::XmlImport;
        const QString xmlPath = QFileDialog::getOpenFileName(
                nullptr,
                tr("Select Rekordbox XML collection"),
                QString(),
                tr("Rekordbox XML (*.xml)"));
        if (xmlPath.isEmpty()) {
            m_pendingOperation = PendingOperation::None;
            return;
        }

        m_tracksFuture = QtConcurrent::run(
                parseXmlLibrary, m_pLibrary->dbConnectionPool(), xmlPath);
        m_tracksFutureWatcher.setFuture(m_tracksFuture);
        m_title = tr("(loading) Rekordbox XML");
        emit featureIsLoading(this, true);
    } else {
        qDebug() << "Unknown link clicked" << link;
    }
}

std::unique_ptr<BaseSqlTableModel>
RekordboxFeature::createPlaylistModelForPlaylist(const QVariant& data) {
    VERIFY_OR_DEBUG_ASSERT(data.canConvert<QVariantList>()) {
        return {};
    }
    QVariantList playlists = data.toList();
    VERIFY_OR_DEBUG_ASSERT(playlists.size() > 0) {
        return {};
    }
    auto pModel = std::make_unique<RekordboxPlaylistModel>(
            this, m_pLibrary->trackCollectionManager(), m_trackSource, m_pConfig);
    pModel->setPlaylist(playlists.at(0).toString());
    return pModel;
}

QVariant RekordboxFeature::title() {
    return m_title;
}

bool RekordboxFeature::isSupported() {
    return true;
}

TreeItemModel* RekordboxFeature::sidebarModel() const {
    return m_pSidebarModel;
}

QString RekordboxFeature::formatRootViewHtml() const {
    QString title = tr("Rekordbox");
    QString summary = tr(
            "Reads databases exported for Pioneer CDJ / XDJ players using "
            "the Rekordbox Export mode.<br/>"
            "Rekordbox can only export to USB or SD devices with a FAT or "
            "HFS file system.<br/>"
            "Mixxx can read a database from any device that contains the "
            "database folders (<tt>PIONEER</tt> and <tt>Contents</tt>).<br/>"
            "Not supported are Rekordbox databases that have been moved to "
            "an external device via<br/>"
            "<i>Preferences > Advanced > Database management</i>.<br/>"
            "<br/>"
            "The following data is read:");

    QStringList items;

    items
            << tr("Folders")
            << tr("Playlists")
            << tr("Beatgrids")
            << tr("Hot cues")
            << tr("Memory cues")
            << tr("Loops")
            << tr("Read-only XML collection import");

    QString html;
    QString refreshLink = tr("Check for attached Rekordbox USB / SD devices (refresh)");
    html.append(QString("<h2>%1</h2>").arg(title));
    html.append(QString("<p>%1</p>").arg(summary));
    html.append(QString("<ul>"));
    for (const auto& item : std::as_const(items)) {
        html.append(QString("<li>%1</li>").arg(item));
    }
    html.append(QString("</ul>"));

    // Colorize links in lighter blue, instead of QT default dark blue.
    // Links are still different from regular text, but readable on dark/light backgrounds.
    // https://github.com/mixxxdj/mixxx/issues/9103
    html.append(QString("<a style=\"color:#0496FF;\" href=\"refresh\">%1</a>")
                    .arg(refreshLink));
    const QString xmlImportLink = tr("Import Rekordbox XML collection (read-only)");
    html.append(QString("<br/><a style=\"color:#0496FF;\" href=\"%1\">%2</a>")
                    .arg(kRekordboxXmlImportLink, xmlImportLink));
    return html;
}

void RekordboxFeature::refreshLibraryModels() {
}

void RekordboxFeature::activate() {
    qDebug() << "RekordboxFeature::activate()";

    if (m_pendingOperation == PendingOperation::Discovery ||
            m_pendingOperation == PendingOperation::XmlImport) {
        return;
    }

    if (m_pendingOperation == PendingOperation::DeviceImport) {
        if (m_cancelDeviceImport) {
            m_cancelDeviceImport->store(true);
        }
        m_refreshAfterDeviceImport = true;
        return;
    }

    m_pendingOperation = PendingOperation::Discovery;
    m_devicesFuture = QtConcurrent::run(findRekordboxDevices);
    m_devicesFutureWatcher.setFuture(m_devicesFuture);
    m_title = tr("(loading) Rekordbox");
    // calls a slot in the sidebar model such that 'Rekordbox (isLoading)' is displayed.
    emit featureIsLoading(this, true);

    emit enableCoverArtDisplay(true);
    emit switchToView("REKORDBOXHOME");
    emit disableSearch();
}

void RekordboxFeature::activateChild(const QModelIndex& index) {
    if (!index.isValid()) {
        return;
    }

    // access underlying TreeItem object
    TreeItem* item = static_cast<TreeItem*>(index.internalPointer());
    if (!(item && item->getData().isValid())) {
        return;
    }

    // TreeItem list data holds 2 values in a QList and have different meanings.
    // If the 2nd QList element IS_RECORDBOX_DEVICE, the 1st element is the
    // filesystem device path, and the parseDeviceDB concurrent thread to parse
    // the Rekcordbox database is initiated. If the 2nd element is
    // IS_NOT_RECORDBOX_DEVICE, the 1st element is the playlist path and it is
    // activated.
    QList<QVariant> data = item->getData().toList();
    if (data.size() < 2) {
        return;
    }
    QString playlist = data[0].toString();
    bool doParseDeviceDB = data[1].toString() == IS_RECORDBOX_DEVICE;

    qDebug() << "RekordboxFeature::activateChild " << item->getLabel()
             << " playlist: " << playlist << " doParseDeviceDB: " << doParseDeviceDB;

    if (doParseDeviceDB) {
        if (m_pendingOperation != PendingOperation::None) {
            return;
        }
        m_pendingOperation = PendingOperation::DeviceImport;
        qDebug() << "Parse Rekordbox Device DB: " << playlist;
        mixxx::FileInfo deviceInfo(playlist);
        if (!Sandbox::askForAccess(&deviceInfo)) {
            m_pendingOperation = PendingOperation::None;
            return;
        }
        const auto token = Sandbox::openSecurityToken(&deviceInfo, true);
        m_cancelDeviceImport = std::make_shared<std::atomic<bool>>(false);
        m_deviceImportFuture = QtConcurrent::run(mixxx::rekordbox::importDeviceDatabase,
                m_pLibrary->dbConnectionPool(),
                playlist,
                item->getLabel(),
                token,
                m_cancelDeviceImport);
        m_deviceImportWatcher.setFuture(m_deviceImportFuture);
        m_title = tr("(loading) Rekordbox");
        emit featureIsLoading(this, true);
    } else {
        qDebug() << "Activate Rekordbox Playlist: " << playlist;
        m_pRekordboxPlaylistModel->setPlaylist(playlist);
        emit showTrackModel(m_pRekordboxPlaylistModel);
    }
}

void RekordboxFeature::onRekordboxDevicesFound() {
    QList<TreeItem*> result;
    try {
        result = m_devicesFuture.result();
    } catch (...) {
        qWarning() << "Rekordbox device discovery failed";
        finishOperation();
        return;
    }
    auto foundDevices = std::vector<std::unique_ptr<TreeItem>>(result.cbegin(), result.cend());

    clearLastRightClickedIndex();

    TreeItem* root = m_pSidebarModel->getRootItem();
    QSqlDatabase database = m_pTrackCollection->database();

    for (int deviceIndex = root->childRows() - 1; deviceIndex >= 0; --deviceIndex) {
        TreeItem* child = root->child(deviceIndex);
        bool removeChild = true;

        for (const auto& pDeviceFound : foundDevices) {
            if (pDeviceFound->getData().toList().value(0) == child->getData().toList().value(0)) {
                removeChild = false;
                break;
            }
        }

        if (removeChild) {
            // Device has since been unmounted, cleanup DB
            if (!mixxx::rekordbox::removeDeviceDatabase(
                        database, child->getData().toList().value(0).toString())) {
                continue;
            }

            m_pSidebarModel->removeRows(deviceIndex, 1);
        }
    }

    std::vector<std::unique_ptr<TreeItem>> childrenToAdd;

    for (auto&& pDeviceFound : foundDevices) {
        bool addNewChild = true;
        for (int deviceIndex = 0; deviceIndex < root->childRows(); deviceIndex++) {
            TreeItem* child = root->child(deviceIndex);

            if (pDeviceFound->getData().toList().value(0) == child->getData().toList().value(0)) {
                // This device already exists in the TreeModel, don't add or parse is again
                addNewChild = false;
            }
        }

        if (addNewChild) {
            childrenToAdd.push_back(std::move(pDeviceFound));
        }
    }

    if (!childrenToAdd.empty()) {
        m_pSidebarModel->insertTreeItemRows(std::move(childrenToAdd), 0);
    }

    // calls a slot in the sidebarmodel such that 'isLoading' is removed from the feature title.
    finishOperation();
}

void RekordboxFeature::onDeviceTracksFound() {
    mixxx::rekordbox::DeviceImportResult result;
    try {
        result = m_deviceImportFuture.result();
    } catch (...) {
        qWarning() << "Rekordbox device worker failed";
    }
    for (const auto& diagnostic : std::as_const(result.diagnostics)) {
        qWarning() << "Rekordbox DeviceSQL:" << diagnostic;
    }
    if (result.applied && result.data) {
        TreeItem* root = m_pSidebarModel->getRootItem();
        for (int row = 0; row < root->childRows(); ++row) {
            TreeItem* item = root->child(row);
            auto data = item->getData().toList();
            if (data.size() < 2 || data[0].toString() != result.devicePath) {
                continue;
            }
            QHash<std::uint32_t, QList<const mixxx::rekordbox::PdbPlaylist*>> children;
            for (const auto& playlist : result.data->playlists) {
                children[playlist.parentPlaylistId].append(&playlist);
            }
            for (auto it = children.begin(); it != children.end(); ++it) {
                std::stable_sort(it->begin(), it->end(), [](const auto* a, const auto* b) {
                    return a->sortOrder < b->sortOrder;
                });
            }
            std::function<std::unique_ptr<TreeItem>(const mixxx::rekordbox::PdbPlaylist*)> makeNode;
            makeNode = [&](const auto* playlist) {
                auto node = std::make_unique<TreeItem>(
                        QString::fromStdString(playlist->nameUtf8),
                        QVariant(QList<QString>{
                                devicePlaylistPath(result.devicePath,
                                        playlist->sourcePlaylistId),
                                IS_NOT_RECORDBOX_DEVICE}));
                std::vector<std::unique_ptr<TreeItem>> descendants;
                for (const auto* child : children.value(playlist->sourcePlaylistId)) {
                    descendants.push_back(makeNode(child));
                }
                node->insertChildren(0, std::move(descendants));
                return node;
            };
            std::vector<std::unique_ptr<TreeItem>> nodes;
            for (const auto* playlist : children.value(0)) {
                nodes.push_back(makeNode(playlist));
            }
            const QModelIndex parentIndex = m_pSidebarModel->index(row, 0);
            m_pSidebarModel->insertTreeItemRows(std::move(nodes), 0, parentIndex);
            data[1] = QVariant(IS_NOT_RECORDBOX_DEVICE);
            item->setData(data);
            m_pSidebarModel->triggerRepaint(parentIndex);
            m_pRekordboxPlaylistModel->setPlaylist(result.devicePath);
            emit showTrackModel(m_pRekordboxPlaylistModel);
            break;
        }
    }
    const bool refresh = m_refreshAfterDeviceImport;
    m_refreshAfterDeviceImport = false;
    finishOperation();
    if (refresh) {
        activate();
    }
}

void RekordboxFeature::onTracksFound() {
    qDebug() << "onTracksFound";
    m_pSidebarModel->triggerRepaint();

    QString devicePlaylist;
    try {
        devicePlaylist = m_tracksFuture.result();
    } catch (const std::exception& e) {
        qWarning() << "Failed to load Rekordbox database:" << e.what();
        finishOperation();
        return;
    } catch (...) {
        qWarning() << "Failed to load Rekordbox XML database";
        finishOperation();
        return;
    }

    qDebug() << "Show Rekordbox Device Playlist: " << devicePlaylist;

    if (devicePlaylist.isEmpty()) {
        finishOperation();
        return;
    }

    m_pRekordboxPlaylistModel->setPlaylist(devicePlaylist);
    emit showTrackModel(m_pRekordboxPlaylistModel);
    finishOperation();
}

void RekordboxFeature::finishOperation() {
    m_pendingOperation = PendingOperation::None;
    m_title = tr("Rekordbox");
    emit featureLoadingFinished(this);
}
