#include <gtest/gtest.h>

#include <QSqlDatabase>
#include <QSqlDriver>
#include <QSqlError>
#include <QSqlQuery>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

#ifdef __SQLITE3__
#include <sqlite3.h>
#endif // __SQLITE3__

#include "library/dao/settingsdao.h"
#include "test/mixxxdbtest.h"
#include "util/db/dbconnectionpooled.h"
#include "util/db/dbconnectionpooler.h"

class DbConnectionPoolTest : public MixxxTest {};

namespace {

#ifdef __SQLITE3__
struct ContenderState {
    std::condition_variable condition;
    std::mutex mutex;
    bool ready{false};
    bool busyObserved{false};
    bool holderReleased{false};
    bool retryAllowed{false};
    bool retryIssued{false};
    bool finished{false};
    bool succeeded{false};
    int busyTimeout{0};
    int busyCallbackCalls{0};
    QString error;
    std::chrono::steady_clock::time_point writeStartedAt;
    std::chrono::steady_clock::time_point busyObservedAt;
    std::chrono::steady_clock::time_point holderReleasedAt;
    std::chrono::steady_clock::time_point finishedAt;
};

void finishContender(
        ContenderState* pState,
        bool succeeded,
        const QString& error) {
    std::lock_guard lock(pState->mutex);
    pState->succeeded = succeeded;
    pState->error = error;
    pState->finished = true;
    pState->finishedAt = std::chrono::steady_clock::now();
    pState->condition.notify_one();
}

int sqliteBusyHandler(void* pContext, int) {
    auto* pState = static_cast<ContenderState*>(pContext);
    std::unique_lock lock(pState->mutex);
    ++pState->busyCallbackCalls;
    if (!pState->busyObserved) {
        pState->busyObserved = true;
        pState->busyObservedAt = std::chrono::steady_clock::now();
        pState->condition.notify_one();
    }

    pState->condition.wait(
            lock,
            [pState]() {
                return pState->holderReleased;
            });

    if (!pState->retryAllowed || pState->retryIssued) {
        return 0;
    }
    pState->retryIssued = true;
    return 1;
}
#endif // __SQLITE3__

} // anonymous namespace

TEST_F(DbConnectionPoolTest, MoveSemantics) {
    mixxx::DbConnectionPooler p1(MixxxDb(config()).connectionPool());
    ASSERT_TRUE(p1.isPooling());

    // Move construction
    mixxx::DbConnectionPooler p2(std::move(p1));
    EXPECT_FALSE(p1.isPooling());
    EXPECT_TRUE(p2.isPooling());

    // Move assignment
    p1 = std::move(p2);
    EXPECT_TRUE(p1.isPooling());
    EXPECT_FALSE(p2.isPooling());
}

TEST_F(DbConnectionPoolTest, ConfiguresSqliteBusyTimeout) {
    const MixxxDb mixxxDb(config(), true);
    const mixxx::DbConnectionPooler pooler(mixxxDb.connectionPool());
    ASSERT_TRUE(pooler.isPooling());

    const QSqlDatabase database = mixxx::DbConnectionPooled(pooler);
    ASSERT_TRUE(database.isOpen());

    QSqlQuery query(database);
    ASSERT_TRUE(query.exec(QStringLiteral("PRAGMA busy_timeout")));
    ASSERT_TRUE(query.next());
    EXPECT_EQ(5000, query.value(0).toInt());
}

TEST_F(DbConnectionPoolTest, ContendedWriteSucceedsBeforeBusyTimeout) {
#ifndef __SQLITE3__
    GTEST_SKIP() << "SQLite3 C API is unavailable (__SQLITE3__ is not defined)";
#else
    // Shared-cache in-memory databases report SQLITE_LOCKED for table
    // contention, which a busy timeout cannot retry. The production database
    // is file-backed and reports SQLITE_BUSY for this contention.
    const MixxxDb mixxxDb(config());
    const auto dbConnectionPool = mixxxDb.connectionPool();
    const mixxx::DbConnectionPooler holderPooler(dbConnectionPool);
    ASSERT_TRUE(holderPooler.isPooling());

    QSqlDatabase holderDatabase = mixxx::DbConnectionPooled(holderPooler);
    ASSERT_TRUE(holderDatabase.isOpen());

    QSqlQuery setupQuery(holderDatabase);
    ASSERT_TRUE(setupQuery.exec(
            QStringLiteral("CREATE TABLE contention_test (value INTEGER)")));

    ASSERT_TRUE(holderDatabase.transaction());
    QSqlQuery holderQuery(holderDatabase);
    ASSERT_TRUE(holderQuery.exec(
            QStringLiteral("INSERT INTO contention_test VALUES (1)")));

    const auto pContenderState = std::make_shared<ContenderState>();
    std::thread contender([dbConnectionPool, pContenderState]() {
        const mixxx::DbConnectionPooler contenderPooler(dbConnectionPool);
        const QSqlDatabase contenderDatabase =
                mixxx::DbConnectionPooled(contenderPooler);

        if (!contenderDatabase.isOpen()) {
            finishContender(
                    pContenderState.get(),
                    false,
                    QStringLiteral("Contender database is not open"));
            return;
        }

        QSqlQuery timeoutQuery(contenderDatabase);
        if (!timeoutQuery.exec(QStringLiteral("PRAGMA busy_timeout")) ||
                !timeoutQuery.next()) {
            finishContender(
                    pContenderState.get(),
                    false,
                    timeoutQuery.lastError().text());
            return;
        }

        const int busyTimeout = timeoutQuery.value(0).toInt();
        QSqlQuery contenderQuery(contenderDatabase);
        if (!contenderQuery.prepare(
                    QStringLiteral("INSERT INTO contention_test VALUES (2)"))) {
            finishContender(
                    pContenderState.get(),
                    false,
                    contenderQuery.lastError().text());
            return;
        }

        QVariant handleVariant = contenderDatabase.driver()->handle();
        if (!handleVariant.isValid() ||
                std::strcmp(handleVariant.typeName(), "sqlite3*") != 0) {
            finishContender(
                    pContenderState.get(),
                    false,
                    QStringLiteral("Contender SQLite handle is unavailable"));
            return;
        }
        sqlite3* handle = *static_cast<sqlite3**>(handleVariant.data());
        if (handle == nullptr ||
                sqlite3_busy_handler(handle, sqliteBusyHandler, pContenderState.get()) !=
                        SQLITE_OK) {
            finishContender(
                    pContenderState.get(),
                    false,
                    QStringLiteral("Could not install SQLite busy handler"));
            return;
        }

        {
            std::lock_guard lock(pContenderState->mutex);
            pContenderState->busyTimeout = busyTimeout;
            pContenderState->ready = true;
            pContenderState->condition.notify_one();
            pContenderState->writeStartedAt = std::chrono::steady_clock::now();
        }

        const bool succeeded = contenderQuery.exec();
        finishContender(
                pContenderState.get(),
                succeeded,
                contenderQuery.lastError().text());
    });

    bool contenderReady = false;
    {
        std::unique_lock lock(pContenderState->mutex);
        contenderReady = pContenderState->condition.wait_for(
                lock,
                std::chrono::seconds(1),
                [pContenderState]() {
                    return pContenderState->ready;
                });
    }

    bool busyObserved = false;
    if (contenderReady) {
        std::unique_lock lock(pContenderState->mutex);
        busyObserved = pContenderState->condition.wait_for(
                lock,
                std::chrono::seconds(1),
                [pContenderState]() {
                    return pContenderState->busyObserved ||
                            pContenderState->finished;
                });
        busyObserved = busyObserved && pContenderState->busyObserved;
    }

    bool holderCommitted = false;
    if (contenderReady && busyObserved) {
        holderCommitted = holderDatabase.commit();
        if (!holderCommitted) {
            holderDatabase.rollback();
        }
    } else {
        holderDatabase.rollback();
    }

    {
        std::lock_guard lock(pContenderState->mutex);
        pContenderState->holderReleased = true;
        pContenderState->retryAllowed = holderCommitted;
        pContenderState->holderReleasedAt = std::chrono::steady_clock::now();
        pContenderState->condition.notify_all();
    }

    bool contenderFinished = false;
    {
        std::unique_lock lock(pContenderState->mutex);
        contenderFinished = pContenderState->condition.wait_for(
                lock,
                std::chrono::seconds(6),
                [pContenderState]() {
                    return pContenderState->finished;
                });
    }

    contender.join();

    ASSERT_TRUE(contenderReady);
    ASSERT_TRUE(busyObserved);
    ASSERT_TRUE(holderCommitted);
    ASSERT_TRUE(contenderFinished);
    EXPECT_EQ(5000, pContenderState->busyTimeout);
    EXPECT_TRUE(pContenderState->succeeded)
            << qPrintable(pContenderState->error);
    EXPECT_GT(pContenderState->busyCallbackCalls, 0);
    EXPECT_LE(pContenderState->writeStartedAt, pContenderState->busyObservedAt);
    EXPECT_LE(pContenderState->busyObservedAt, pContenderState->holderReleasedAt);
    EXPECT_LE(pContenderState->holderReleasedAt, pContenderState->finishedAt);
    const auto writeDuration =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                    pContenderState->finishedAt - pContenderState->writeStartedAt);
    EXPECT_LT(writeDuration.count(), 5000)
            << "write elapsed " << writeDuration.count()
            << " ms; busy callbacks " << pContenderState->busyCallbackCalls;

    QSqlQuery countQuery(holderDatabase);
    ASSERT_TRUE(countQuery.exec(
            QStringLiteral("SELECT COUNT(*) FROM contention_test")));
    ASSERT_TRUE(countQuery.next());
    EXPECT_EQ(2, countQuery.value(0).toInt());
#endif // __SQLITE3__
}
