// Prepared by ChatGPT for human review.
#pragma once

#include <QSqlDatabase>
#include <QSqlDriver>

#ifdef __SQLITE3__
#include <sqlite3.h>
#endif

// Reject an actual SQLite COMMIT without changing the application's schema or
// foreign-key policy. SQLite rolls back the transaction when this hook vetoes it.
// The isolated test connection must not already have an application commit hook.
class SqliteCommitFailure {
  public:
    explicit SqliteCommitFailure(const QSqlDatabase& database) {
#ifdef __SQLITE3__
        auto handle = database.driver()->handle();
        if (handle.typeName() && QByteArray(handle.typeName()) == "sqlite3*") {
            m_connection = *static_cast<sqlite3**>(handle.data());
            sqlite3_commit_hook(m_connection, [](void* context) {
                ++static_cast<SqliteCommitFailure*>(context)->m_attempts;
                return 1; }, this);
        }
#else
        Q_UNUSED(database);
#endif
    }

    ~SqliteCommitFailure() {
        disable();
    }

    SqliteCommitFailure(const SqliteCommitFailure&) = delete;
    SqliteCommitFailure& operator=(const SqliteCommitFailure&) = delete;

    bool isValid() const {
#ifdef __SQLITE3__
        return m_connection != nullptr;
#else
        return false;
#endif
    }

    int attempts() const {
        return m_attempts;
    }

    void disable() {
#ifdef __SQLITE3__
        if (m_connection) {
            sqlite3_commit_hook(m_connection, nullptr, nullptr);
            m_connection = nullptr;
        }
#endif
    }

  private:
#ifdef __SQLITE3__
    sqlite3* m_connection = nullptr;
#endif
    int m_attempts = 0;
};
// End of code prepared by ChatGPT for human review and submission.
