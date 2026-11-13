/*
 * fooyin-jellyfin
 * Copyright © 2026, Nikita Butenko
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "jellyfincredentialstore.h"
#include "testsupport.h"

#include <core/accountstore.h>
#include <utils/database/dbconnectionhandler.h>
#include <utils/database/dbconnectionprovider.h>
#include <utils/database/dbquery.h>

#include <QElapsedTimer>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <future>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using namespace Fooyin;
using namespace Fooyin::Jellyfin;

namespace {
int busyTimeout(const DbConnectionPoolPtr& dbPool)
{
    DbQuery query{DbConnectionProvider{dbPool}.db(), u"PRAGMA busy_timeout"_s};
    if(!query.exec() || !query.next()) {
        return -1;
    }
    return query.value(0).toInt();
}

bool setBusyTimeout(const DbConnectionPoolPtr& dbPool, int milliseconds)
{
    DbQuery query{DbConnectionProvider{dbPool}.db(), u"PRAGMA busy_timeout = %1"_s.arg(milliseconds)};
    return query.exec();
}

struct LockedOperation
{
    QString error;
    std::optional<QString> token;
    qint64 elapsed;
    int busyTimeout;
    bool ownsConnection;
};

struct LockedRun
{
    bool locked;
    bool finishedWhileLocked;
    LockedOperation operation;
};

// The lock is always released before joining, so an operation that waits on it cannot hang the test.
template<typename Operation>
LockedRun runWhileLocked(const QSqlDatabase& lockOwner, const QString& lockStatement, Operation operation)
{
    QSqlQuery begin{lockOwner};
    const bool locked              = begin.exec(lockStatement);
    auto worker                    = std::async(std::launch::async, operation);
    const bool finishedWhileLocked = worker.wait_for(5s) == std::future_status::ready;
    QSqlQuery rollback{lockOwner};
    rollback.exec(u"ROLLBACK"_s);
    return {.locked = locked, .finishedWhileLocked = finishedWhileLocked, .operation = worker.get()};
}
} // namespace

TEST(JellyfinCredentialStoreTest, PersistsIsolatedRecordsThroughTheHostAccountStoreAcrossThreads)
{
    Fooyin::Jellyfin::Testing::LogCapture logs;
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    DbConnection::DbParams params;
    params.type = u"QSQLITE"_s;
    params.filePath = directory.filePath(u"fooyin.db"_s);
    const auto dbPool = DbConnectionPool::create(params, u"jellyfin-accounts"_s);
    ASSERT_TRUE(dbPool);
    const DbConnectionHandler connection{dbPool};
    ASSERT_TRUE(connection.hasConnection());
    const DbConnectionProvider provider{dbPool};
    DbQuery createSettings{provider.db(), u"CREATE TABLE Settings (Name TEXT PRIMARY KEY, Value TEXT)"_s};
    ASSERT_TRUE(createSettings.exec());
    AccountCredentialStore store{dbPool};
    EXPECT_FALSE(store.readToken(u"first"_s, {}).token);
    EXPECT_TRUE(store.readToken(u"first"_s, {}).error.isEmpty());
    ASSERT_TRUE(store.writeToken(u"first"_s, u"synthetic-token"_s, {}).isEmpty());
    ASSERT_TRUE(store.writeToken(u"second"_s, u"other-token"_s, {}).isEmpty());
    const AccountStore accountStore{dbPool};
    const auto stored = accountStore.load(u"Jellyfin/first"_s, {u"AccessToken"_s});
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->at(u"AccessToken"_s), u"synthetic-token"_s);
    AccountCredentialStore reopened{dbPool};
    EXPECT_EQ(reopened.readToken(u"first"_s, {}).token, u"synthetic-token"_s);
    auto worker = std::async(std::launch::async, [&]() {
        return store.writeToken(u"first"_s, u"replacement-token"_s, {});
    });
    EXPECT_TRUE(worker.get().isEmpty());
    EXPECT_EQ(store.readToken(u"first"_s, {}).token, u"replacement-token"_s);
    std::stop_source cancellation;
    cancellation.request_stop();
    EXPECT_FALSE(store.readToken(u"first"_s, cancellation.get_token()).error.isEmpty());
    EXPECT_FALSE(store.writeToken(u"first"_s, u"cancelled-token"_s, cancellation.get_token()).isEmpty());
    EXPECT_FALSE(store.removeToken(u"first"_s, cancellation.get_token()).isEmpty());
    EXPECT_EQ(store.readToken(u"first"_s, {}).token, u"replacement-token"_s);
    ASSERT_TRUE(store.removeToken(u"first"_s, {}).isEmpty());
    EXPECT_FALSE(store.readToken(u"first"_s, {}).token);
    EXPECT_TRUE(store.removeToken(u"first"_s, {}).isEmpty());
    EXPECT_EQ(store.readToken(u"second"_s, {}).token, u"other-token"_s);
    DbQuery rejectCredential{provider.db(), u"CREATE TRIGGER reject_credential BEFORE INSERT ON Settings "
                                           "WHEN NEW.Name = 'Accounts/Jellyfin/refused' "
                                           "BEGIN SELECT RAISE(ABORT, 'synthetic refusal'); END"_s};
    ASSERT_TRUE(rejectCredential.exec());
    EXPECT_FALSE(store.writeToken(u"refused"_s, u"must-not-enter-logs"_s, {}).isEmpty());
    EXPECT_FALSE(logs.contains(u"must-not-enter-logs"_s));
    DbQuery dropSettings{provider.db(), u"DROP TABLE Settings"_s};
    ASSERT_TRUE(dropSettings.exec());
    EXPECT_FALSE(store.readToken(u"second"_s, {}).error.isEmpty());
    EXPECT_FALSE(store.writeToken(u"second"_s, u"rejected-token"_s, {}).isEmpty());
    EXPECT_FALSE(store.removeToken(u"second"_s, {}).isEmpty());
    EXPECT_FALSE(logs.contains(u"synthetic-token"_s));
    EXPECT_FALSE(logs.contains(u"replacement-token"_s));
    EXPECT_FALSE(logs.contains(u"other-token"_s));
    EXPECT_FALSE(logs.contains(u"rejected-token"_s));
}

TEST(JellyfinCredentialStoreTest, LockedDatabaseFailsWithoutWaitingAndRestoresTheBusyTimeout)
{
    Fooyin::Jellyfin::Testing::LogCapture logs;
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    DbConnection::DbParams params;
    params.type           = u"QSQLITE"_s;
    params.connectOptions = u"QSQLITE_BUSY_TIMEOUT=60000"_s;
    params.filePath       = directory.filePath(u"fooyin.db"_s);
    const auto dbPool     = DbConnectionPool::create(params, u"jellyfin-locked-accounts"_s);
    ASSERT_TRUE(dbPool);
    const DbConnectionHandler connection{dbPool};
    ASSERT_TRUE(connection.hasConnection());
    const DbConnectionProvider provider{dbPool};
    {
        DbQuery journalMode{provider.db(), u"PRAGMA journal_mode = DELETE"_s};
        ASSERT_TRUE(journalMode.exec());
    }
    DbQuery createSettings{provider.db(), u"CREATE TABLE Settings (Name TEXT PRIMARY KEY, Value TEXT)"_s};
    ASSERT_TRUE(createSettings.exec());
    AccountCredentialStore store{dbPool};
    ASSERT_TRUE(store.writeToken(u"first"_s, u"baseline-token"_s, {}).isEmpty());
    ASSERT_TRUE(setBusyTimeout(dbPool, 45000));

    const QString lockOwnerName{u"jellyfin-lock-owner"_s};
    {
        QSqlDatabase lockOwner = QSqlDatabase::addDatabase(u"QSQLITE"_s, lockOwnerName);
        lockOwner.setDatabaseName(params.filePath);
        ASSERT_TRUE(lockOwner.open());

        const auto borrowedWrite = runWhileLocked(lockOwner, u"BEGIN IMMEDIATE"_s, [&]() {
            const DbConnectionHandler outer{dbPool};
            setBusyTimeout(dbPool, 30000);
            QElapsedTimer timer;
            timer.start();
            const QString error = store.writeToken(u"first"_s, u"locked-token"_s, {});
            return LockedOperation{.error          = error,
                                   .token          = {},
                                   .elapsed        = timer.elapsed(),
                                   .busyTimeout    = busyTimeout(dbPool),
                                   .ownsConnection = outer.hasConnection() && dbPool->hasThreadConnection()};
        });
        EXPECT_TRUE(borrowedWrite.locked);
        EXPECT_TRUE(borrowedWrite.finishedWhileLocked);
        EXPECT_FALSE(borrowedWrite.operation.error.isEmpty());
        EXPECT_LT(borrowedWrite.operation.elapsed, 1000);
        EXPECT_EQ(borrowedWrite.operation.busyTimeout, 30000);
        EXPECT_TRUE(borrowedWrite.operation.ownsConnection);

        const auto borrowedRemove = runWhileLocked(lockOwner, u"BEGIN IMMEDIATE"_s, [&]() {
            const DbConnectionHandler outer{dbPool};
            setBusyTimeout(dbPool, 30000);
            QElapsedTimer timer;
            timer.start();
            const QString error = store.removeToken(u"first"_s, {});
            return LockedOperation{.error          = error,
                                   .token          = {},
                                   .elapsed        = timer.elapsed(),
                                   .busyTimeout    = busyTimeout(dbPool),
                                   .ownsConnection = outer.hasConnection() && dbPool->hasThreadConnection()};
        });
        EXPECT_TRUE(borrowedRemove.locked);
        EXPECT_TRUE(borrowedRemove.finishedWhileLocked);
        EXPECT_FALSE(borrowedRemove.operation.error.isEmpty());
        EXPECT_LT(borrowedRemove.operation.elapsed, 1000);
        EXPECT_EQ(borrowedRemove.operation.busyTimeout, 30000);
        EXPECT_TRUE(borrowedRemove.operation.ownsConnection);

        const auto borrowedRead = runWhileLocked(lockOwner, u"BEGIN EXCLUSIVE"_s, [&]() {
            const DbConnectionHandler outer{dbPool};
            setBusyTimeout(dbPool, 30000);
            QElapsedTimer timer;
            timer.start();
            const auto lookup = store.readToken(u"first"_s, {});
            return LockedOperation{.error          = lookup.error,
                                   .token          = lookup.token,
                                   .elapsed        = timer.elapsed(),
                                   .busyTimeout    = busyTimeout(dbPool),
                                   .ownsConnection = outer.hasConnection() && dbPool->hasThreadConnection()};
        });
        EXPECT_TRUE(borrowedRead.locked);
        EXPECT_TRUE(borrowedRead.finishedWhileLocked);
        EXPECT_FALSE(borrowedRead.operation.error.isEmpty());
        EXPECT_FALSE(borrowedRead.operation.token);
        EXPECT_LT(borrowedRead.operation.elapsed, 1000);
        EXPECT_EQ(borrowedRead.operation.busyTimeout, 30000);
        EXPECT_TRUE(borrowedRead.operation.ownsConnection);

        const auto freshWrite = runWhileLocked(lockOwner, u"BEGIN IMMEDIATE"_s, [&]() {
            QElapsedTimer timer;
            timer.start();
            const QString error = store.writeToken(u"first"_s, u"locked-token"_s, {});
            return LockedOperation{.error          = error,
                                   .token          = {},
                                   .elapsed        = timer.elapsed(),
                                   .busyTimeout    = 0,
                                   .ownsConnection = dbPool->hasThreadConnection()};
        });
        EXPECT_TRUE(freshWrite.locked);
        EXPECT_TRUE(freshWrite.finishedWhileLocked);
        EXPECT_FALSE(freshWrite.operation.error.isEmpty());
        EXPECT_LT(freshWrite.operation.elapsed, 1000);
        EXPECT_FALSE(freshWrite.operation.ownsConnection);

        const auto freshRead = runWhileLocked(lockOwner, u"BEGIN EXCLUSIVE"_s, [&]() {
            QElapsedTimer timer;
            timer.start();
            const auto lookup = store.readToken(u"first"_s, {});
            return LockedOperation{.error          = lookup.error,
                                   .token          = lookup.token,
                                   .elapsed        = timer.elapsed(),
                                   .busyTimeout    = 0,
                                   .ownsConnection = dbPool->hasThreadConnection()};
        });
        EXPECT_TRUE(freshRead.locked);
        EXPECT_TRUE(freshRead.finishedWhileLocked);
        EXPECT_FALSE(freshRead.operation.error.isEmpty());
        EXPECT_FALSE(freshRead.operation.token);
        EXPECT_LT(freshRead.operation.elapsed, 1000);
        EXPECT_FALSE(freshRead.operation.ownsConnection);
        lockOwner.close();
    }
    QSqlDatabase::removeDatabase(lockOwnerName);

    EXPECT_EQ(busyTimeout(dbPool), 45000);
    EXPECT_EQ(store.readToken(u"first"_s, {}).token, u"baseline-token"_s);
    EXPECT_EQ(busyTimeout(dbPool), 45000);

    auto unlocked = std::async(std::launch::async, [&]() {
        const DbConnectionHandler outer{dbPool};
        setBusyTimeout(dbPool, 30000);
        std::stop_source cancellation;
        cancellation.request_stop();
        const bool cancelledWriteFailed
            = !store.writeToken(u"first"_s, u"cancelled-token"_s, cancellation.get_token()).isEmpty();
        const int afterCancelledWrite = busyTimeout(dbPool);
        const bool writeSucceeded     = store.writeToken(u"first"_s, u"unlocked-token"_s, {}).isEmpty();
        const int afterWrite          = busyTimeout(dbPool);
        const auto token              = store.readToken(u"first"_s, {}).token;
        const int afterRead           = busyTimeout(dbPool);
        return std::tuple{cancelledWriteFailed,
                          afterCancelledWrite,
                          writeSucceeded,
                          afterWrite,
                          token,
                          afterRead,
                          outer.hasConnection() && dbPool->hasThreadConnection()};
    });
    const auto [cancelledWriteFailed, afterCancelledWrite, writeSucceeded, afterWrite, token, afterRead, ownsConnection]
        = unlocked.get();
    EXPECT_TRUE(cancelledWriteFailed);
    EXPECT_EQ(afterCancelledWrite, 30000);
    EXPECT_TRUE(writeSucceeded);
    EXPECT_EQ(afterWrite, 30000);
    EXPECT_EQ(token, u"unlocked-token"_s);
    EXPECT_EQ(afterRead, 30000);
    EXPECT_TRUE(ownsConnection);
    EXPECT_EQ(busyTimeout(dbPool), 45000);
    EXPECT_FALSE(logs.contains(u"baseline-token"_s));
    EXPECT_FALSE(logs.contains(u"locked-token"_s));
    EXPECT_FALSE(logs.contains(u"unlocked-token"_s));
}
