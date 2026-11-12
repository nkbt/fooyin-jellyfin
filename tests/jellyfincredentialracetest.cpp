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

#include "jellyfinaccount.h"
#include "jellyfincredentialstatestore.h"
#include "jellyfincredentialstore.h"
#include "jellyfintrack.h"
#include "syntheticnetworkaccessmanager.h"
#include "testsupport.h"

#include <utils/database/dbconnectionhandler.h>
#include <utils/database/dbconnectionprovider.h>
#include <utils/database/dbquery.h>

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTest>

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <mutex>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
namespace Fooyin::Jellyfin::Testing {
namespace {
class CancellationCredentialStore final : public CredentialStore
{
public:
    TokenLookup readToken(const QString& credentialRecordId, std::stop_token) override
    {
        const std::scoped_lock lock{mutex};
        const auto found = tokens.find(credentialRecordId);
        return found == tokens.end() ? TokenLookup{} : TokenLookup{.token = found->second, .error = {}};
    }
    QString writeToken(const QString& credentialRecordId, const QString& token, std::stop_token cancellation) override
    {
        const int concurrent = ++activeCalls;
        int previous         = maxConcurrentCalls.load();
        while(previous < concurrent && !maxConcurrentCalls.compare_exchange_weak(previous, concurrent)) { }
        const auto finished = qScopeGuard([&]() { --activeCalls; });
        std::unique_lock lock{mutex};
        tokens[credentialRecordId] = token;
        writing                    = true;
        condition.wait(lock, cancellation, []() { return false; });
        return u"Synthetic write cancellation"_s;
    }
    QString removeToken(const QString&, std::stop_token cancellation) override
    {
        const int concurrent = ++activeCalls;
        int previous         = maxConcurrentCalls.load();
        while(previous < concurrent && !maxConcurrentCalls.compare_exchange_weak(previous, concurrent)) { }
        const auto finished = qScopeGuard([&]() { --activeCalls; });
        std::unique_lock lock{mutex};
        removing = true;
        condition.wait(lock, cancellation, []() { return false; });
        return u"Synthetic removal cancellation"_s;
    }
    void seed(const QString& credentialRecordId, const QString& token)
    {
        const std::scoped_lock lock{mutex};
        tokens[credentialRecordId] = token;
    }
    QString token(const QString& credentialRecordId)
    {
        const std::scoped_lock lock{mutex};
        const auto found = tokens.find(credentialRecordId);
        return found == tokens.end() ? QString{} : found->second;
    }
    std::atomic<bool> writing{false};
    std::atomic<bool> removing{false};
    std::atomic<int> activeCalls{0};
    std::atomic<int> maxConcurrentCalls{0};

private:
    std::mutex mutex;
    std::condition_variable_any condition;
    std::map<QString, QString> tokens;
};

// Signals entry before forwarding to the real account database backend.
class EntrySignallingCredentialStore final : public CredentialStore
{
public:
    explicit EntrySignallingCredentialStore(std::shared_ptr<CredentialStore> backend)
        : m_backend{std::move(backend)}
    { }
    TokenLookup readToken(const QString& credentialRecordId, std::stop_token cancellation) override
    {
        return m_backend->readToken(credentialRecordId, cancellation);
    }
    QString writeToken(const QString& credentialRecordId, const QString& token, std::stop_token cancellation) override
    {
        writing = true;
        return m_backend->writeToken(credentialRecordId, token, cancellation);
    }
    QString removeToken(const QString& credentialRecordId, std::stop_token cancellation) override
    {
        removing = true;
        return m_backend->removeToken(credentialRecordId, cancellation);
    }
    std::atomic<bool> writing{false};
    std::atomic<bool> removing{false};

private:
    std::shared_ptr<CredentialStore> m_backend;
};

// Fresh connections wait ten seconds for a lock unless the credential store disables waiting.
DbConnectionPoolPtr createAccountDatabase(const QString& filePath)
{
    DbConnection::DbParams params;
    params.type           = u"QSQLITE"_s;
    params.connectOptions = u"QSQLITE_BUSY_TIMEOUT=10000"_s;
    params.filePath       = filePath;
    auto dbPool           = DbConnectionPool::create(params, u"jellyfin-race-accounts"_s);
    const DbConnectionHandler connection{dbPool};
    const DbConnectionProvider provider{dbPool};
    DbQuery journalMode{provider.db(), u"PRAGMA journal_mode = DELETE"_s};
    DbQuery createSettings{provider.db(), u"CREATE TABLE Settings (Name TEXT PRIMARY KEY, Value TEXT)"_s};
    if(!journalMode.exec() || !createSettings.exec()) {
        return {};
    }
    return dbPool;
}

void seedAcceptedConnection(CredentialStateStore& credentialStateStore, const QString& credentialRecordId)
{
    ASSERT_TRUE(credentialStateStore
                    .commit(QJsonObject{
                        {u"acceptedConnection"_s, QJsonObject{{u"serverUrl"_s, u"https://synthetic.invalid/jf"_s},
                                                              {u"serverId"_s, u"server1"_s},
                                                              {u"userId"_s, u"user1"_s},
                                                              {u"userName"_s, u"Synthetic"_s},
                                                              {u"deviceId"_s, u"device1"_s},
                                                              {u"credentialRecordId"_s, credentialRecordId}}},
                        {u"sourceKeys"_s, QJsonArray{sourceKey(u"server1"_s, u"user1"_s)}}})
                    .isEmpty());
}
} // namespace

TEST(JellyfinCredentialRace, ShutdownCancelsCandidateWriteAndPreservesAcceptedRecord)
{
    QTemporaryDir directory;
    SettingsManager settings{directory.filePath(u"settings.ini"_s)};
    auto credentialStateStore
        = std::make_shared<FileCredentialStateStore>(directory.filePath(u"credential-state.json"_s));
    StubMusicLibrary library;
    SyntheticNetworkAccessManager network{nullptr};
    auto credentials = std::make_shared<CancellationCredentialStore>();
    const QString previousCredentialRecordId
        = sourceKey(u"server1"_s, u"user1"_s) + u"/00000000-0000-4000-8000-000000000001"_s;
    credentials->seed(previousCredentialRecordId, u"synthetic-baseline-token"_s);
    seedAcceptedConnection(*credentialStateStore, previousCredentialRecordId);
    auto account = std::make_unique<Account>(&network, &library, &settings, credentialStateStore, credentials,
                                             std::make_shared<AccessRegistry>(), 1ms, nullptr);
    account->startQuickConnect(u"https://synthetic.invalid/jf"_s);
    ASSERT_TRUE(QTest::qWaitFor([&]() { return credentials->writing.load(); }, 5000))
        << account->status().toStdString();
    QElapsedTimer timer;
    timer.start();
    account.reset();
    EXPECT_LT(timer.elapsed(), 1000);
    EXPECT_EQ(credentials->activeCalls.load(), 0);
    EXPECT_EQ(credentials->maxConcurrentCalls.load(), 1);
    EXPECT_EQ(credentials->token(previousCredentialRecordId), u"synthetic-baseline-token"_s);
    const auto state
        = FileCredentialStateStore{directory.filePath(u"credential-state.json"_s)}.load().state.value_or(QJsonObject{});
    EXPECT_EQ(state.value(u"acceptedConnection"_s).toObject().value(u"credentialRecordId"_s).toString(),
              previousCredentialRecordId);
    ASSERT_EQ(state.value(u"credentialRecords"_s).toArray().size(), 1);
    EXPECT_NE(
        state.value(u"credentialRecords"_s).toArray().first().toObject().value(u"credentialRecordId"_s).toString(),
        previousCredentialRecordId);
    EXPECT_EQ(library.snapshotRequests, 0);
}

TEST(JellyfinCredentialRace, ShutdownCancelsAuthoritativeRemovalWithoutParallelCleanup)
{
    QTemporaryDir directory;
    SettingsManager settings{directory.filePath(u"settings.ini"_s)};
    auto credentialStateStore
        = std::make_shared<FileCredentialStateStore>(directory.filePath(u"credential-state.json"_s));
    StubMusicLibrary library;
    SyntheticNetworkAccessManager network{nullptr};
    auto credentials = std::make_shared<CancellationCredentialStore>();
    const QString previousCredentialRecordId
        = sourceKey(u"server1"_s, u"user1"_s) + u"/00000000-0000-4000-8000-000000000001"_s;
    credentials->seed(previousCredentialRecordId, u"synthetic-baseline-token"_s);
    seedAcceptedConnection(*credentialStateStore, previousCredentialRecordId);
    auto account = std::make_unique<Account>(&network, &library, &settings, credentialStateStore, credentials,
                                             std::make_shared<AccessRegistry>(), 1ms, nullptr);
    account->signOut();
    ASSERT_TRUE(QTest::qWaitFor([&]() { return credentials->removing.load(); }, 5000));
    QElapsedTimer timer;
    timer.start();
    account.reset();
    EXPECT_LT(timer.elapsed(), 1000);
    EXPECT_EQ(credentials->activeCalls.load(), 0);
    EXPECT_EQ(credentials->maxConcurrentCalls.load(), 1);
    const auto state
        = FileCredentialStateStore{directory.filePath(u"credential-state.json"_s)}.load().state.value_or(QJsonObject{});
    EXPECT_TRUE(state.value(u"acceptedConnection"_s).toObject().isEmpty());
    ASSERT_EQ(state.value(u"credentialRecords"_s).toArray().size(), 1);
    EXPECT_EQ(
        state.value(u"credentialRecords"_s).toArray().first().toObject().value(u"credentialRecordId"_s).toString(),
        previousCredentialRecordId);
}
TEST(JellyfinCredentialRace, ShutdownDuringLockedCandidateWriteDoesNotWaitAndCleansUpAfterRestart)
{
    QTemporaryDir directory;
    SettingsManager settings{directory.filePath(u"settings.ini"_s)};
    auto credentialStateStore
        = std::make_shared<FileCredentialStateStore>(directory.filePath(u"credential-state.json"_s));
    StubMusicLibrary library;
    SyntheticNetworkAccessManager network{nullptr};
    const QString databasePath = directory.filePath(u"fooyin.db"_s);
    const auto dbPool          = createAccountDatabase(databasePath);
    ASSERT_TRUE(dbPool);
    auto backend = std::make_shared<AccountCredentialStore>(dbPool);
    const QString previousCredentialRecordId
        = sourceKey(u"server1"_s, u"user1"_s) + u"/00000000-0000-4000-8000-000000000001"_s;
    ASSERT_TRUE(backend->writeToken(previousCredentialRecordId, u"synthetic-baseline-token"_s, {}).isEmpty());
    seedAcceptedConnection(*credentialStateStore, previousCredentialRecordId);

    const QString lockOwnerName{u"jellyfin-race-lock-owner"_s};
    {
        QSqlDatabase lockOwner = QSqlDatabase::addDatabase(u"QSQLITE"_s, lockOwnerName);
        lockOwner.setDatabaseName(databasePath);
        ASSERT_TRUE(lockOwner.open());
        QSqlQuery lock{lockOwner};
        ASSERT_TRUE(lock.exec(u"BEGIN IMMEDIATE"_s));
        auto releaseLock = qScopeGuard([&]() { QSqlQuery{lockOwner}.exec(u"ROLLBACK"_s); });
        auto credentials = std::make_shared<EntrySignallingCredentialStore>(backend);
        auto account     = std::make_unique<Account>(&network, &library, &settings, credentialStateStore, credentials,
                                                     std::make_shared<AccessRegistry>(), 1ms, nullptr);
        account->startQuickConnect(u"https://synthetic.invalid/jf"_s);
        ASSERT_TRUE(QTest::qWaitFor([&]() { return credentials->writing.load(); }, 5000))
            << account->status().toStdString();
        QElapsedTimer timer;
        timer.start();
        account.reset();
        EXPECT_LT(timer.elapsed(), 1000);
        releaseLock.dismiss();
        EXPECT_TRUE(QSqlQuery{lockOwner}.exec(u"ROLLBACK"_s));
    }
    QSqlDatabase::removeDatabase(lockOwnerName);

    auto state
        = FileCredentialStateStore{directory.filePath(u"credential-state.json"_s)}.load().state.value_or(QJsonObject{});
    EXPECT_EQ(state.value(u"acceptedConnection"_s).toObject().value(u"credentialRecordId"_s).toString(),
              previousCredentialRecordId);
    ASSERT_EQ(state.value(u"credentialRecords"_s).toArray().size(), 1);
    const QString candidateCredentialRecordId
        = state.value(u"credentialRecords"_s).toArray().first().toObject().value(u"credentialRecordId"_s).toString();
    EXPECT_NE(candidateCredentialRecordId, previousCredentialRecordId);
    EXPECT_EQ(backend->readToken(previousCredentialRecordId, {}).token, u"synthetic-baseline-token"_s);
    const auto candidate = backend->readToken(candidateCredentialRecordId, {});
    EXPECT_TRUE(candidate.error.isEmpty());
    EXPECT_FALSE(candidate.token);

    auto restarted = std::make_unique<Account>(&network, &library, &settings, credentialStateStore, backend,
                                               std::make_shared<AccessRegistry>(), 1ms, nullptr);
    ASSERT_TRUE(QTest::qWaitFor(
        [&]() {
            return FileCredentialStateStore{directory.filePath(u"credential-state.json"_s)}
                .load()
                .state.value_or(QJsonObject{})
                .value(u"credentialRecords"_s)
                .toArray()
                .isEmpty();
        },
        5000))
        << restarted->status().toStdString();
    restarted.reset();
    state
        = FileCredentialStateStore{directory.filePath(u"credential-state.json"_s)}.load().state.value_or(QJsonObject{});
    EXPECT_EQ(state.value(u"acceptedConnection"_s).toObject().value(u"credentialRecordId"_s).toString(),
              previousCredentialRecordId);
    EXPECT_EQ(backend->readToken(previousCredentialRecordId, {}).token, u"synthetic-baseline-token"_s);
    EXPECT_FALSE(backend->readToken(candidateCredentialRecordId, {}).token);
}

TEST(JellyfinCredentialRace, ShutdownDuringLockedRemovalDoesNotWaitAndRetainsCleanupUntilRestart)
{
    QTemporaryDir directory;
    SettingsManager settings{directory.filePath(u"settings.ini"_s)};
    auto credentialStateStore
        = std::make_shared<FileCredentialStateStore>(directory.filePath(u"credential-state.json"_s));
    StubMusicLibrary library;
    SyntheticNetworkAccessManager network{nullptr};
    const QString databasePath = directory.filePath(u"fooyin.db"_s);
    const auto dbPool          = createAccountDatabase(databasePath);
    ASSERT_TRUE(dbPool);
    auto backend = std::make_shared<AccountCredentialStore>(dbPool);
    const QString previousCredentialRecordId
        = sourceKey(u"server1"_s, u"user1"_s) + u"/00000000-0000-4000-8000-000000000001"_s;
    ASSERT_TRUE(backend->writeToken(previousCredentialRecordId, u"synthetic-baseline-token"_s, {}).isEmpty());
    seedAcceptedConnection(*credentialStateStore, previousCredentialRecordId);

    const QString lockOwnerName{u"jellyfin-race-lock-owner"_s};
    {
        QSqlDatabase lockOwner = QSqlDatabase::addDatabase(u"QSQLITE"_s, lockOwnerName);
        lockOwner.setDatabaseName(databasePath);
        ASSERT_TRUE(lockOwner.open());
        QSqlQuery lock{lockOwner};
        ASSERT_TRUE(lock.exec(u"BEGIN IMMEDIATE"_s));
        auto releaseLock = qScopeGuard([&]() { QSqlQuery{lockOwner}.exec(u"ROLLBACK"_s); });
        auto credentials = std::make_shared<EntrySignallingCredentialStore>(backend);
        auto account     = std::make_unique<Account>(&network, &library, &settings, credentialStateStore, credentials,
                                                     std::make_shared<AccessRegistry>(), 1ms, nullptr);
        account->signOut();
        ASSERT_TRUE(QTest::qWaitFor([&]() { return credentials->removing.load(); }, 5000));
        QElapsedTimer timer;
        timer.start();
        account.reset();
        EXPECT_LT(timer.elapsed(), 1000);
        releaseLock.dismiss();
        EXPECT_TRUE(QSqlQuery{lockOwner}.exec(u"ROLLBACK"_s));
    }
    QSqlDatabase::removeDatabase(lockOwnerName);

    auto state
        = FileCredentialStateStore{directory.filePath(u"credential-state.json"_s)}.load().state.value_or(QJsonObject{});
    EXPECT_TRUE(state.value(u"acceptedConnection"_s).toObject().isEmpty());
    ASSERT_EQ(state.value(u"credentialRecords"_s).toArray().size(), 1);
    EXPECT_EQ(
        state.value(u"credentialRecords"_s).toArray().first().toObject().value(u"credentialRecordId"_s).toString(),
        previousCredentialRecordId);
    EXPECT_EQ(backend->readToken(previousCredentialRecordId, {}).token, u"synthetic-baseline-token"_s);

    auto restarted = std::make_unique<Account>(&network, &library, &settings, credentialStateStore, backend,
                                               std::make_shared<AccessRegistry>(), 1ms, nullptr);
    ASSERT_TRUE(QTest::qWaitFor(
        [&]() {
            return FileCredentialStateStore{directory.filePath(u"credential-state.json"_s)}
                .load()
                .state.value_or(QJsonObject{})
                .value(u"credentialRecords"_s)
                .toArray()
                .isEmpty();
        },
        5000))
        << restarted->status().toStdString();
    restarted.reset();
    const auto removed = backend->readToken(previousCredentialRecordId, {});
    EXPECT_TRUE(removed.error.isEmpty());
    EXPECT_FALSE(removed.token);
}
} // namespace Fooyin::Jellyfin::Testing
