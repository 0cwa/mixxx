#pragma once

#include <QSqlDatabase>

namespace mixxx::rekordbox {

bool isWritableDatabase(const QSqlDatabase& database);

constexpr bool isValidDatabaseId(int id) {
    return id > 0;
}

} // namespace mixxx::rekordbox
