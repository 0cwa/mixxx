#pragma once

#include <QMap>
#include <QSqlDatabase>
#include <QString>

namespace mixxx::rekordbox {

bool isWritableDatabase(const QSqlDatabase& database);

bool importPlaylistTracks(QSqlDatabase& database,
        int playlistID,
        const QMap<uint32_t, uint32_t>& playlistTracks,
        const QString& device);

constexpr bool isValidDatabaseId(int id) {
    return id > 0;
}

} // namespace mixxx::rekordbox
