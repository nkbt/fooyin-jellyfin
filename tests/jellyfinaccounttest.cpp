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

#include "fakejellyfinserver.h"
#include "jellyfinaccess.h"
#include "jellyfincatalogsync.h"
#include "jellyfincredentialstatestore.h"
#include "jellyfinfavouritebanner.h"
#include "jellyfinfavouritesync.h"
#include "jellyfininput.h"
#include "jellyfinpage.h"
#include "jellyfintrack.h"
#include "testsupport.h"

#include <gui/widgets/messagebanner.h>
#include <utils/settings/settingsmanager.h>

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QWidget>

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <future>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace Fooyin::Jellyfin::Testing {
namespace {
const QString Code   = u"123456"_s;
const QString Secret = u"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"_s;
const QString Token  = u"synthetic-access-token"_s;
const QByteArray TokenField{"Token=\"synthetic-access-token\""};
const QString ServerId = u"server1"_s;
const QString UserId   = u"user1"_s;

struct QuickConnectScenario
{
    QByteArray enabledBody{"true"};
    int initiateStatus{200};
    QJsonObject initiateResult{{u"Code"_s, Code}, {u"Secret"_s, Secret}, {u"Authenticated"_s, false}};
    std::atomic<int> pendingPolls{1};
    std::atomic<int> connectStatus{200};
    QByteArray connectErrorBody;
    int exchangeDelayMs{0};
    int favouriteDelayMs{0};
    QJsonObject exchangeResult{
        {u"AccessToken"_s, Token},
        {u"ServerId"_s, ServerId},
        {u"User"_s, QJsonObject{{u"Id"_s, UserId}, {u"Name"_s, u"Synthetic User"_s}}},
        {u"SessionInfo"_s, QJsonObject{}},
    };
};

FakeResponse catalogResponse(const FakeRequest& request)
{
    if(request.query.queryItemValue(u"includeItemTypes"_s) == "Playlist"_L1) {
        return FakeJellyfinServer::json(QJsonObject{{u"Items"_s, QJsonArray{}}, {u"TotalRecordCount"_s, 0}});
    }
    const QJsonObject item{{u"Id"_s, u"track1"_s},
                           {u"Type"_s, u"Audio"_s},
                           {u"Name"_s, u"Synthetic Track"_s},
                           {u"AlbumId"_s, u"album1"_s},
                           {u"AlbumPrimaryImageTag"_s, u"tag1"_s},
                           {u"UserData"_s, QJsonObject{{u"IsFavorite"_s, false}}}};
    return FakeJellyfinServer::json(QJsonObject{{u"Items"_s, QJsonArray{item}}, {u"TotalRecordCount"_s, 1}});
}

FakeJellyfinServer::Handler quickConnectHandler(std::shared_ptr<QuickConnectScenario> scenario)
{
    return [scenario](const FakeRequest& request) -> FakeResponse {
        if(request.path.endsWith("/QuickConnect/Enabled"_L1) && request.method == "GET") {
            return {.status  = 200,
                    .headers = {{"Content-Type", "application/json"}},
                    .body    = scenario->enabledBody,
                    .delayMs = 0};
        }
        if(request.path.endsWith("/QuickConnect/Initiate"_L1) && request.method == "POST") {
            if(scenario->initiateStatus != 200) {
                return FakeJellyfinServer::text(scenario->initiateStatus,
                                                "Quick connect is not enabled on this server");
            }
            return FakeJellyfinServer::json(scenario->initiateResult);
        }
        if(request.path.endsWith("/QuickConnect/Connect"_L1) && request.method == "GET") {
            if(scenario->connectStatus != 200) {
                return FakeJellyfinServer::text(scenario->connectStatus, scenario->connectErrorBody);
            }
            if(request.query.queryItemValue(u"secret"_s) != scenario->initiateResult.value("Secret"_L1).toString()) {
                return FakeJellyfinServer::text(404, "Unknown secret");
            }
            const bool approved = scenario->pendingPolls.fetch_sub(1) <= 0;
            QJsonObject result{scenario->initiateResult};
            result.insert(u"Authenticated"_s, approved);
            return FakeJellyfinServer::json(result);
        }
        if(request.path.endsWith("/Users/AuthenticateWithQuickConnect"_L1) && request.method == "POST") {
            const QJsonObject body = QJsonDocument::fromJson(request.body).object();
            if(body.value("Secret"_L1).toString() != scenario->initiateResult.value("Secret"_L1).toString()) {
                return FakeJellyfinServer::text(404, "Unknown secret");
            }
            FakeResponse response = FakeJellyfinServer::json(scenario->exchangeResult);
            response.delayMs      = scenario->exchangeDelayMs;
            return response;
        }
        if(request.path.endsWith("/Items"_L1) && request.method == "GET") {
            return catalogResponse(request);
        }
        if(request.path.endsWith("/UserFavoriteItems/track1"_L1)) {
            FakeResponse response = FakeJellyfinServer::json(QJsonObject{{u"IsFavorite"_s, request.method == "POST"}});
            response.delayMs      = scenario->favouriteDelayMs;
            return response;
        }
        return FakeJellyfinServer::text(404, "Not found");
    };
}

QPushButton* findButton(const QWidget* widget, const QString& text)
{
    for(auto* button : widget->findChildren<QPushButton*>()) {
        if(button->text() == text) {
            return button;
        }
    }
    return nullptr;
}

std::vector<FakeRequest> favouriteRequests(const FakeJellyfinServer& server)
{
    std::vector<FakeRequest> found;
    for(const auto& request : server.requests()) {
        if(request.path.contains("/UserFavoriteItems/"_L1)) {
            found.push_back(request);
        }
    }
    return found;
}

void editLoved(MusicLibrary& library, const Track& track, bool loved)
{
    Track edited{library.trackForId(track.id())};
    edited.setLoved(loved);
    library.editTrackStats({edited}, Track::Stat::Loved);
}

bool hasLabel(const QWidget* widget, const QString& text)
{
    for(const auto* label : widget->findChildren<QLabel*>()) {
        if(label->text() == text) {
            return true;
        }
    }
    return false;
}

// Set JELLYFIN_SCREENSHOT_DIR to keep a rendering of the synthetic page state for review.
void saveScreenshot(QWidget* widget, const QString& name)
{
    const QString directory = qEnvironmentVariable("JELLYFIN_SCREENSHOT_DIR");
    if(directory.isEmpty()) {
        return;
    }
    widget->resize(640, 360);
    widget->grab().save(directory + u"/"_s + name);
}

int countPath(const FakeJellyfinServer& server, const QString& suffix)
{
    int count{0};
    for(const auto& request : server.requests()) {
        if(request.path.endsWith(suffix)) {
            ++count;
        }
    }
    return count;
}
QJsonObject credentialState(CredentialStateStore& credentialStateStore)
{
    return credentialStateStore.load().state.value_or(QJsonObject{});
}

QString credentialRecordId(CredentialStateStore& credentialStateStore)
{
    return credentialState(credentialStateStore)
        .value(u"acceptedConnection"_s)
        .toObject()
        .value(u"credentialRecordId"_s)
        .toString();
}

bool hasPendingCredentialCleanup(CredentialStateStore& credentialStateStore, const QString& credentialRecordId)
{
    return std::ranges::any_of(
        credentialState(credentialStateStore).value(u"credentialRecords"_s).toArray(), [&](const QJsonValue& record) {
            return record.toObject().value(u"credentialRecordId"_s).toString() == credentialRecordId;
        });
}

QString pendingCredentialRecordId(CredentialStateStore& credentialStateStore, const QString& sourceKey)
{
    const auto credentialRecords = credentialState(credentialStateStore).value(u"credentialRecords"_s).toArray();
    for(qsizetype index = credentialRecords.size(); index > 0; --index) {
        const auto record = credentialRecords.at(index - 1).toObject();
        if(record.value(u"sourceKey"_s).toString() == sourceKey) {
            return record.value(u"credentialRecordId"_s).toString();
        }
    }
    return {};
}

void storeConnection(CredentialStateStore& credentialStateStore, const QUrl& serverUrl,
                     const QString& credentialRecordId, const QString& userId)
{
    EXPECT_TRUE(
        credentialStateStore
            .commit(QJsonObject{{u"acceptedConnection"_s, QJsonObject{{u"serverUrl"_s, serverUrl.toString()},
                                                                      {u"serverId"_s, ServerId},
                                                                      {u"userId"_s, userId},
                                                                      {u"userName"_s, u"Synthetic User"_s},
                                                                      {u"deviceId"_s, u"device1"_s},
                                                                      {u"credentialRecordId"_s, credentialRecordId}}}})
            .isEmpty());
}

class FailingCredentialStateStore : public CredentialStateStore
{
public:
    explicit FailingCredentialStateStore(QString credentialStatePath)
        : file{std::move(credentialStatePath)}
    { }
    CredentialStateLoad load() override
    {
        return file.load();
    }
    QString commit(const QJsonObject& state) override
    {
        const auto previous         = file.load().state.value_or(QJsonObject{});
        const auto accepted         = state.value(u"acceptedConnection"_s).toObject();
        const auto previousAccepted = previous.value(u"acceptedConnection"_s).toObject();
        if((failCandidateIntent && accepted == previousAccepted
            && state.value(u"credentialRecords"_s).toArray().size()
                   > previous.value(u"credentialRecords"_s).toArray().size())
           || (failAcceptance && !accepted.isEmpty() && accepted != previousAccepted)
           || (failSignOut && accepted.isEmpty() && !previousAccepted.isEmpty())
           || (failCleanupClear && accepted == previousAccepted
               && state.value(u"credentialRecords"_s).toArray().size()
                      < previous.value(u"credentialRecords"_s).toArray().size())) {
            ++failedCommits;
            return u"Synthetic metadata commit failure"_s;
        }
        return file.commit(state);
    }
    FileCredentialStateStore file;
    bool failCandidateIntent{false};
    bool failAcceptance{false};
    bool failCleanupClear{false};
    bool failSignOut{false};
    int failedCommits{0};
};

class DelayedCredentialStore : public CredentialStore
{
public:
    TokenLookup readToken(const QString& credentialRecordId, std::stop_token cancellation) override
    {
        const int concurrentCalls = ++activeCalls;
        int previousMax           = maxConcurrentCalls.load();
        while(previousMax < concurrentCalls
              && !maxConcurrentCalls.compare_exchange_weak(previousMax, concurrentCalls)) { }
        const auto finished = qScopeGuard([&]() { --activeCalls; });
        std::unique_lock lock{mutex};
        TokenLookup result;
        if(const auto found = tokens.find(credentialRecordId); found != tokens.end()) {
            result.token = found->second;
        }
        if(delayReads && cancellation.stop_possible()) {
            readEntered = true;
            ready.wait(lock, cancellation, [this]() { return !delayReads; });
        }
        return result;
    }
    QString writeToken(const QString& credentialRecordId, const QString& token, std::stop_token cancellation) override
    {
        const int concurrentCalls = ++activeCalls;
        int previousMax           = maxConcurrentCalls.load();
        while(previousMax < concurrentCalls
              && !maxConcurrentCalls.compare_exchange_weak(previousMax, concurrentCalls)) { }
        const auto finished = qScopeGuard([&]() { --activeCalls; });
        std::unique_lock lock{mutex};
        tokens[credentialRecordId] = token;
        if(delayWrites && cancellation.stop_possible()) {
            writeEntered = true;
            ready.wait(lock, cancellation, [this]() { return !delayWrites; });
        }
        if(writeCompleted) {
            writeCompleted->set_value();
            writeCompleted.reset();
        }
        return {};
    }
    QString removeToken(const QString& credentialRecordId, std::stop_token cancellation) override
    {
        const int concurrentCalls = ++activeCalls;
        int previousMax           = maxConcurrentCalls.load();
        while(previousMax < concurrentCalls
              && !maxConcurrentCalls.compare_exchange_weak(previousMax, concurrentCalls)) { }
        const auto finished = qScopeGuard([&]() { --activeCalls; });
        std::unique_lock lock{mutex};
        ++removalCalls;
        if(delayRemovals) {
            removalEntered = true;
            ready.wait(lock, cancellation, [this]() { return !delayRemovals; });
        }
        if(cancellation.stop_requested()) {
            removalCancelled = true;
            return u"Synthetic cleanup cancelled"_s;
        }
        if(!removalError.isEmpty()) {
            return removalError;
        }
        tokens.erase(credentialRecordId);
        ++removalCompleted;
        return {};
    }
    void releaseRemovals()
    {
        {
            const std::scoped_lock lock{mutex};
            delayRemovals = false;
        }
        ready.notify_all();
    }
    void releaseWrites()
    {
        {
            const std::scoped_lock lock{mutex};
            delayWrites = false;
        }
        ready.notify_all();
    }
    std::future<void> observeWriteCompletion()
    {
        const std::scoped_lock lock{mutex};
        writeCompleted = std::make_shared<std::promise<void>>();
        return writeCompleted->get_future();
    }
    void setRemovalError(const QString& error)
    {
        const std::scoped_lock lock{mutex};
        removalError = error;
    }
    QString token(const QString& credentialRecordId)
    {
        const std::scoped_lock lock{mutex};
        const auto found = tokens.find(credentialRecordId);
        return found == tokens.end() ? QString{} : found->second;
    }
    std::atomic<int> activeCalls{0};
    std::atomic<int> maxConcurrentCalls{0};
    std::atomic<bool> delayReads{false};
    std::atomic<bool> delayWrites{false};
    std::atomic<bool> readEntered{false};
    std::atomic<bool> writeEntered{false};
    std::atomic<bool> delayRemovals{false};
    std::atomic<bool> removalEntered{false};
    std::atomic<bool> removalCancelled{false};
    std::atomic<int> removalCalls{0};
    std::atomic<int> removalCompleted{0};

private:
    std::mutex mutex;
    std::condition_variable_any ready;
    std::map<QString, QString> tokens;
    QString removalError;
    std::shared_ptr<std::promise<void>> writeCompleted;
};
} // namespace

class JellyfinAccountTest : public testing::Test
{
protected:
    JellyfinAccountTest()
        : m_settingsPath{m_dir.filePath(u"settings.ini"_s)}
        , m_settings{m_settingsPath}
        , m_credentialStateStore{std::make_shared<FileCredentialStateStore>(m_dir.filePath(u"credential-state.json"_s))}
        , m_credentials{std::make_shared<MemoryCredentialStore>()}
        , m_access{std::make_shared<AccessRegistry>()}
        , m_account{&m_network, &m_library, &m_settings, m_credentialStateStore, m_credentials, m_access, 20ms, nullptr}
    { }

    [[nodiscard]] QByteArray settingsFile() const
    {
        QFile file{m_settingsPath};
        if(!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        return file.readAll();
    }

    bool waitForIdle()
    {
        return QTest::qWaitFor([this]() { return !m_account.isQuickConnectPending(); }, 5000);
    }

    QTemporaryDir m_dir;
    QString m_settingsPath;
    SettingsManager m_settings;
    StubMusicLibrary m_library;
    std::shared_ptr<FileCredentialStateStore> m_credentialStateStore;
    std::shared_ptr<MemoryCredentialStore> m_credentials;
    std::shared_ptr<AccessRegistry> m_access;
    QNetworkAccessManager m_network;
    FakeJellyfinServer m_server;
    Account m_account;
};

TEST_F(JellyfinAccountTest, SignsInAfterApprovalAndStoresOnlyTheToken)
{
    const LogCapture logs;
    auto scenario          = std::make_shared<QuickConnectScenario>();
    scenario->pendingPolls = 2;
    m_server.setHandler(quickConnectHandler(scenario));

    QStringList codes;
    QObject::connect(&m_account, &Account::quickConnectChanged, &m_account,
                     [this, &codes]() { codes.append(m_account.quickConnectCode()); });

    m_account.startQuickConnect(m_server.url(u"/jf/"_s).toString());
    ASSERT_TRUE(QTest::qWaitFor([this]() { return m_account.connection().has_value(); }, 5000));
    ASSERT_TRUE(QTest::qWaitFor([this]() { return m_library.snapshotRequests == 1; }, 5000));

    EXPECT_TRUE(codes.contains(Code));
    EXPECT_FALSE(m_account.isQuickConnectPending());
    EXPECT_TRUE(m_account.quickConnectCode().isEmpty());

    const auto requests = m_server.requests();
    ASSERT_GE(requests.size(), 6U);
    EXPECT_EQ(requests.at(0).method, "GET");
    EXPECT_EQ(requests.at(0).path, u"/jf/QuickConnect/Enabled"_s);
    EXPECT_EQ(requests.at(1).method, "POST");
    EXPECT_EQ(requests.at(1).path, u"/jf/QuickConnect/Initiate"_s);
    EXPECT_EQ(countPath(m_server, u"/jf/QuickConnect/Connect"_s), 3);

    for(const auto& request : requests) {
        const QByteArray authorization = request.header("authorization");
        EXPECT_TRUE(authorization.startsWith("MediaBrowser "));
        EXPECT_TRUE(authorization.contains("DeviceId=\""));
        EXPECT_TRUE(request.header("x-emby-token").isEmpty());
        EXPECT_FALSE(request.query.hasQueryItem(u"api_key"_s));
        if(request.path.contains("QuickConnect"_L1) || request.path.contains("AuthenticateWithQuickConnect"_L1)) {
            EXPECT_FALSE(authorization.contains("Token="));
        }
        if(request.path.endsWith("/Items"_L1)) {
            EXPECT_TRUE(authorization.contains(TokenField));
            EXPECT_FALSE(request.query.toString().contains(Secret));
        }
    }

    const auto exchange = m_server.requestsForPath(u"/jf/Users/AuthenticateWithQuickConnect"_s);
    ASSERT_EQ(exchange.size(), 1U);
    EXPECT_EQ(exchange.front().method, "POST");
    EXPECT_EQ(QJsonDocument::fromJson(exchange.front().body).object(), (QJsonObject{{u"Secret"_s, Secret}}));
    EXPECT_FALSE(exchange.front().query.hasQueryItem(u"secret"_s));

    ASSERT_EQ(m_credentials->tokens.size(), 1U);
    EXPECT_EQ(m_credentials->tokens.at(credentialRecordId(*m_credentialStateStore)), Token);

    const auto connection = m_account.connection();
    EXPECT_EQ(connection->serverUrl, m_server.url(u"/jf"_s));
    EXPECT_EQ(connection->serverId, ServerId);
    EXPECT_EQ(connection->userId, UserId);
    EXPECT_EQ(connection->userName, u"Synthetic User"_s);

    QFile credentialStateFile{m_dir.filePath(u"credential-state.json"_s)};
    ASSERT_TRUE(credentialStateFile.open(QIODevice::ReadOnly));
    const QByteArray stored = credentialStateFile.readAll();
    EXPECT_TRUE(stored.contains(ServerId.toLatin1()));
    EXPECT_FALSE(settingsFile().contains(ServerId.toLatin1()));
    EXPECT_FALSE(settingsFile().contains(Token.toLatin1()));
    EXPECT_FALSE(settingsFile().contains(Secret.toLatin1()));
    EXPECT_FALSE(stored.contains(Secret.toLatin1()));
    EXPECT_FALSE(stored.contains(Token.toLatin1()));
    EXPECT_FALSE(stored.contains(Code.toLatin1()));

    EXPECT_FALSE(logs.contains(Secret));
    EXPECT_FALSE(logs.contains(Token));
    EXPECT_FALSE(m_account.status().contains(Secret));
}

TEST_F(JellyfinAccountTest, DisabledWhenEnabledReturnsFalse)
{
    auto scenario         = std::make_shared<QuickConnectScenario>();
    scenario->enabledBody = "false";
    m_server.setHandler(quickConnectHandler(scenario));

    m_account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(waitForIdle());

    EXPECT_TRUE(m_account.status().contains("disabled"_L1));
    EXPECT_EQ(countPath(m_server, u"/QuickConnect/Initiate"_s), 0);
    EXPECT_FALSE(m_account.connection().has_value());
}

TEST_F(JellyfinAccountTest, DisabledWhenInitiateReturnsUnauthorised)
{
    auto scenario            = std::make_shared<QuickConnectScenario>();
    scenario->initiateStatus = 401;
    m_server.setHandler(quickConnectHandler(scenario));

    m_account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(waitForIdle());

    EXPECT_TRUE(m_account.status().contains("disabled"_L1));
    EXPECT_EQ(countPath(m_server, u"/QuickConnect/Connect"_s), 0);
}

TEST_F(JellyfinAccountTest, DisabledDuringPollingStopsAttempt)
{
    auto scenario           = std::make_shared<QuickConnectScenario>();
    scenario->connectStatus = 401;
    m_server.setHandler(quickConnectHandler(scenario));

    m_account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(waitForIdle());

    EXPECT_TRUE(m_account.status().contains("disabled"_L1));
    EXPECT_EQ(countPath(m_server, u"/Users/AuthenticateWithQuickConnect"_s), 0);
}

TEST_F(JellyfinAccountTest, ExpiredCodeStopsPolling)
{
    auto scenario          = std::make_shared<QuickConnectScenario>();
    scenario->pendingPolls = 1000;
    m_server.setHandler(quickConnectHandler(scenario));

    m_account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor([this]() { return countPath(m_server, u"/QuickConnect/Connect"_s) >= 2; }, 5000));
    scenario->connectStatus = 404;
    ASSERT_TRUE(waitForIdle());

    const int polls = countPath(m_server, u"/QuickConnect/Connect"_s);
    QTest::qWait(150);
    EXPECT_EQ(countPath(m_server, u"/QuickConnect/Connect"_s), polls);
    EXPECT_TRUE(m_account.status().contains("expired"_L1));
    EXPECT_TRUE(m_credentials->tokens.empty());
}

TEST_F(JellyfinAccountTest, PlainTextServerErrorIsNotShownOrParsed)
{
    auto scenario              = std::make_shared<QuickConnectScenario>();
    scenario->connectStatus    = 500;
    scenario->connectErrorBody = "Error processing request: synthetic-internal-detail";
    m_server.setHandler(quickConnectHandler(scenario));

    m_account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(waitForIdle());

    EXPECT_TRUE(m_account.status().contains("500"_L1));
    EXPECT_FALSE(m_account.status().contains("synthetic-internal-detail"_L1));
    EXPECT_EQ(countPath(m_server, u"/Users/AuthenticateWithQuickConnect"_s), 0);
}

TEST_F(JellyfinAccountTest, InvalidInitiateResponseIsRejected)
{
    auto scenario = std::make_shared<QuickConnectScenario>();
    scenario->initiateResult.insert(u"Secret"_s, u"not hex"_s);
    m_server.setHandler(quickConnectHandler(scenario));

    m_account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(waitForIdle());

    EXPECT_TRUE(m_account.status().contains("invalid"_L1));
    EXPECT_EQ(countPath(m_server, u"/QuickConnect/Connect"_s), 0);
}

TEST_F(JellyfinAccountTest, CancelDuringPollingStopsRequestsWithoutPromisingRevocation)
{
    auto scenario          = std::make_shared<QuickConnectScenario>();
    scenario->pendingPolls = 1000;
    m_server.setHandler(quickConnectHandler(scenario));

    m_account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor([this]() { return countPath(m_server, u"/QuickConnect/Connect"_s) >= 1; }, 5000));
    EXPECT_EQ(m_account.quickConnectCode(), Code);

    m_account.cancelQuickConnect();
    EXPECT_FALSE(m_account.isQuickConnectPending());
    EXPECT_TRUE(m_account.quickConnectCode().isEmpty());
    EXPECT_TRUE(m_account.status().contains("server can keep"_L1));

    QTest::qWait(100);
    const int polls = countPath(m_server, u"/QuickConnect/Connect"_s);
    QTest::qWait(150);
    EXPECT_EQ(countPath(m_server, u"/QuickConnect/Connect"_s), polls);
    EXPECT_FALSE(m_account.connection().has_value());
}

TEST_F(JellyfinAccountTest, CancelDuringExchangeDiscardsTheResult)
{
    auto scenario             = std::make_shared<QuickConnectScenario>();
    scenario->pendingPolls    = 0;
    scenario->exchangeDelayMs = 300;
    m_server.setHandler(quickConnectHandler(scenario));

    m_account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(
        QTest::qWaitFor([this]() { return countPath(m_server, u"/Users/AuthenticateWithQuickConnect"_s) == 1; }, 5000));
    m_account.cancelQuickConnect();
    QTest::qWait(500);

    EXPECT_TRUE(m_credentials->tokens.empty());
    EXPECT_FALSE(m_account.connection().has_value());
    EXPECT_FALSE(m_access->access().has_value());
    EXPECT_EQ(countPath(m_server, u"/Items"_s), 0);
}

TEST_F(JellyfinAccountTest, NewAttemptOnAnotherServerReplacesThePendingAttempt)
{
    auto first          = std::make_shared<QuickConnectScenario>();
    first->pendingPolls = 1000;
    first->initiateResult.insert(u"Code"_s, u"111111"_s);
    first->initiateResult.insert(u"Secret"_s, QString(64, u'a'));
    m_server.setHandler(quickConnectHandler(first));

    FakeJellyfinServer secondServer;
    auto second          = std::make_shared<QuickConnectScenario>();
    second->pendingPolls = 1;
    secondServer.setHandler(quickConnectHandler(second));

    m_account.startQuickConnect(m_server.url(u"/a"_s).toString());
    ASSERT_TRUE(QTest::qWaitFor([this]() { return countPath(m_server, u"/a/QuickConnect/Connect"_s) >= 1; }, 5000));
    EXPECT_EQ(m_account.quickConnectCode(), u"111111"_s);

    m_account.startQuickConnect(secondServer.url(u"/b"_s).toString());
    ASSERT_TRUE(QTest::qWaitFor([this]() { return m_account.connection().has_value(); }, 5000));
    const auto firstRequests = m_server.requests().size();
    QTest::qWait(100);

    EXPECT_EQ(m_server.requests().size(), firstRequests);
    EXPECT_EQ(countPath(m_server, u"/Users/AuthenticateWithQuickConnect"_s), 0);
    EXPECT_EQ(countPath(secondServer, u"/b/Users/AuthenticateWithQuickConnect"_s), 1);
    EXPECT_EQ(m_account.connection()->serverUrl, secondServer.url(u"/b"_s));
    for(const auto& request : secondServer.requests()) {
        EXPECT_NE(request.query.queryItemValue(u"secret"_s), QString(64, u'a'));
    }
}

TEST_F(JellyfinAccountTest, InvalidExchangeResponseIsRejected)
{
    auto scenario          = std::make_shared<QuickConnectScenario>();
    scenario->pendingPolls = 0;
    scenario->exchangeResult.remove(u"AccessToken"_s);
    m_server.setHandler(quickConnectHandler(scenario));

    m_account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(waitForIdle());

    EXPECT_TRUE(m_account.status().contains("invalid"_L1));
    EXPECT_TRUE(m_credentials->tokens.empty());
    EXPECT_FALSE(m_account.connection().has_value());
}

TEST_F(JellyfinAccountTest, InvalidUserIdentityIsRejected)
{
    auto scenario          = std::make_shared<QuickConnectScenario>();
    scenario->pendingPolls = 0;
    scenario->exchangeResult.insert(u"User"_s, QJsonObject{{u"Id"_s, u"../user"_s}});
    m_server.setHandler(quickConnectHandler(scenario));

    m_account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(waitForIdle());

    EXPECT_TRUE(m_credentials->tokens.empty());
    EXPECT_FALSE(m_account.connection().has_value());
}

TEST_F(JellyfinAccountTest, CredentialStoreFailureDoesNotActivate)
{
    auto scenario          = std::make_shared<QuickConnectScenario>();
    scenario->pendingPolls = 0;
    m_server.setHandler(quickConnectHandler(scenario));
    m_credentials->writeError = u"The secure credential store is unavailable."_s;

    m_account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(waitForIdle());

    EXPECT_EQ(m_account.status(), m_credentials->writeError);
    EXPECT_FALSE(m_account.connection().has_value());
    EXPECT_FALSE(m_access->access().has_value());
    EXPECT_TRUE(credentialRecordId(*m_credentialStateStore).isEmpty());
}

TEST_F(JellyfinAccountTest, RedirectToAnotherOriginIsNotFollowed)
{
    FakeJellyfinServer otherServer;
    auto scenario = std::make_shared<QuickConnectScenario>();
    otherServer.setHandler(quickConnectHandler(scenario));
    const QUrl target = otherServer.url(u"/QuickConnect/Enabled"_s);
    m_server.setHandler([target](const FakeRequest& /*request*/) { return FakeJellyfinServer::redirect(target); });

    m_account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(waitForIdle());

    EXPECT_TRUE(otherServer.requests().empty());
    EXPECT_FALSE(m_account.connection().has_value());
}

TEST_F(JellyfinAccountTest, InvalidServerAddressIsRejectedLocally)
{
    m_account.startQuickConnect(u"ftp://jellyfin.example.com"_s);
    EXPECT_FALSE(m_account.isQuickConnectPending());
    EXPECT_TRUE(m_server.requests().empty());
}

TEST_F(JellyfinAccountTest, RestoreUsesStoredTokenAndSynchronises)
{
    m_settings.fileSet(DeviceIdKey, u"uncommitted-preference-device"_s);
    m_server.setHandler(quickConnectHandler(std::make_shared<QuickConnectScenario>()));
    storeConnection(*m_credentialStateStore, m_server.url(u"/jf"_s),
                    sourceKey(ServerId, UserId) + u"/00000000-0000-4000-8000-000000000001"_s, UserId);
    m_credentials->tokens[sourceKey(ServerId, UserId) + u"/00000000-0000-4000-8000-000000000001"_s] = Token;

    m_account.restore();
    ASSERT_TRUE(QTest::qWaitFor([this]() { return m_account.connection().has_value(); }, 5000));
    ASSERT_TRUE(QTest::qWaitFor([this]() { return m_library.snapshotRequests == 1; }, 5000));

    const auto items = m_server.requestsForPath(u"/jf/Items"_s);
    ASSERT_FALSE(items.empty());
    EXPECT_TRUE(items.front().header("authorization").contains(TokenField));
    EXPECT_TRUE(items.front().header("authorization").contains("DeviceId=\"device1\""));
}

TEST_F(JellyfinAccountTest, RestoreWithoutStoredTokenRequiresSignIn)
{
    storeConnection(*m_credentialStateStore, m_server.url({}),
                    sourceKey(ServerId, UserId) + u"/00000000-0000-4000-8000-000000000002"_s, UserId);

    m_account.restore();
    EXPECT_FALSE(m_account.connection().has_value());
    ASSERT_TRUE(QTest::qWaitFor([this]() { return m_account.status().contains("Sign in again"_L1); }, 5000));
    EXPECT_TRUE(m_server.requests().empty());
}

TEST_F(JellyfinAccountTest, SignOutRemovesTokenAndKeepsCatalog)
{
    auto scenario          = std::make_shared<QuickConnectScenario>();
    scenario->pendingPolls = 0;
    m_server.setHandler(quickConnectHandler(scenario));

    m_account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor([this]() { return !m_library.tracks().empty(); }, 5000));

    const auto requestsBefore = m_server.requests().size();
    QSignalSpy credentialFinished{&m_account, &Account::credentialOperationFinished};
    m_account.signOut();
    ASSERT_TRUE(credentialFinished.wait(5000));

    EXPECT_TRUE(m_credentials->tokens.empty());
    EXPECT_FALSE(m_account.connection().has_value());
    EXPECT_FALSE(m_access->access().has_value());
    EXPECT_TRUE(credentialRecordId(*m_credentialStateStore).isEmpty());
    EXPECT_EQ(m_library.tracks().size(), 1U);
    EXPECT_TRUE(m_library.activeLibraryTracks().empty());
    QTest::qWait(50);
    EXPECT_EQ(m_server.requests().size(), requestsBefore);
}

TEST_F(JellyfinAccountTest, LibraryRefreshSynchronisesTheSignedInAccount)
{
    auto scenario          = std::make_shared<QuickConnectScenario>();
    scenario->pendingPolls = 0;
    m_server.setHandler(quickConnectHandler(scenario));

    m_account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor(
        [this]() { return m_library.snapshotRequests == 1 && !m_account.catalogSync()->isRunning(); }, 5000));

    const auto library = m_library.libraries().begin()->second;
    m_library.refresh(library);
    ASSERT_TRUE(QTest::qWaitFor([this]() { return m_library.snapshotRequests == 2; }, 5000));
}

TEST_F(JellyfinAccountTest, OtherProviderRefreshLeavesTheSignedInAccountUnchanged)
{
    auto scenario          = std::make_shared<QuickConnectScenario>();
    scenario->pendingPolls = 0;
    m_server.setHandler(quickConnectHandler(scenario));

    m_account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor(
        [this]() { return m_library.snapshotRequests == 1 && !m_account.catalogSync()->isRunning(); }, 5000));

    const QString statusBefore = m_account.status();
    const auto requestsBefore  = m_server.requests().size();
    QSignalSpy statusChanges{&m_account, &Account::statusChanged};
    const auto other = m_library.addExternalLibrary(u"otherprovider://synthetic-library"_s, u"Other provider"_s);
    ASSERT_TRUE(other.has_value());
    m_library.refresh(*other);
    QTest::qWait(50);
    EXPECT_EQ(statusChanges.count(), 0);
    EXPECT_EQ(m_account.status(), statusBefore);
    EXPECT_EQ(m_server.requests().size(), requestsBefore);
    EXPECT_EQ(m_library.snapshotRequests, 1);
}

TEST_F(JellyfinAccountTest, OtherProviderRefreshLeavesTheSignedOutAccountUnchanged)
{
    const QString statusBefore = m_account.status();
    QSignalSpy statusChanges{&m_account, &Account::statusChanged};
    const auto other = m_library.addExternalLibrary(u"otherprovider://synthetic-library"_s, u"Other provider"_s);
    ASSERT_TRUE(other.has_value());
    m_library.refresh(*other);
    QTest::qWait(50);
    EXPECT_EQ(statusChanges.count(), 0);
    EXPECT_EQ(m_account.status(), statusBefore);
    EXPECT_TRUE(m_server.requests().empty());

    const auto jellyfin = m_library.addExternalLibrary(libraryUri(sourceKey(ServerId, UserId)), u"Jellyfin library"_s);
    ASSERT_TRUE(jellyfin.has_value());
    m_library.refresh(*jellyfin);
    EXPECT_EQ(m_account.status(), u"The Jellyfin account for library Jellyfin library is not signed in."_s);
    EXPECT_TRUE(m_server.requests().empty());
}

TEST_F(JellyfinAccountTest, SettingsPageShowsTheCodeUntilApproval)
{
    auto scenario          = std::make_shared<QuickConnectScenario>();
    scenario->pendingPolls = 1000;
    m_server.setHandler(quickConnectHandler(scenario));

    JellyfinPage page{&m_account, &m_settings, nullptr};
    QWidget* widget = page.widget();
    ASSERT_NE(widget, nullptr);
    page.load();

    auto* start   = findButton(widget, u"Sign in with Quick Connect"_s);
    auto* cancel  = findButton(widget, u"Cancel"_s);
    auto* signOut = findButton(widget, u"Sign out"_s);
    auto* refresh = findButton(widget, u"Refresh library"_s);
    ASSERT_NE(start, nullptr);
    ASSERT_NE(cancel, nullptr);
    ASSERT_NE(signOut, nullptr);
    ASSERT_NE(refresh, nullptr);
    EXPECT_FALSE(start->isEnabled());
    EXPECT_TRUE(hasLabel(widget, u"Not signed in"_s));

    auto* serverUrl = widget->findChild<QLineEdit*>();
    ASSERT_NE(serverUrl, nullptr);
    serverUrl->setText(m_server.url(u"/jellyfin"_s).toString());
    EXPECT_TRUE(start->isEnabled());

    start->click();
    ASSERT_TRUE(QTest::qWaitFor([widget]() { return hasLabel(widget, Code); }, 5000));
    EXPECT_FALSE(start->isEnabled());
    EXPECT_FALSE(serverUrl->isEnabled());
    EXPECT_TRUE(cancel->isEnabled());
    EXPECT_FALSE(signOut->isEnabled());
    EXPECT_FALSE(refresh->isEnabled());
    saveScreenshot(widget, u"settings-quick-connect-pending.png"_s);

    scenario->pendingPolls = 0;
    ASSERT_TRUE(QTest::qWaitFor(
        [this]() { return m_account.connection().has_value() && !m_account.catalogSync()->isRunning(); }, 5000));
    EXPECT_TRUE(
        hasLabel(widget, u"Signed in as Synthetic User on %1"_s.arg(m_server.url(u"/jellyfin"_s).toDisplayString())));
    EXPECT_FALSE(hasLabel(widget, Code));
    EXPECT_FALSE(cancel->isEnabled());
    EXPECT_TRUE(signOut->isEnabled());
    EXPECT_TRUE(refresh->isEnabled());
    saveScreenshot(widget, u"settings-signed-in.png"_s);

    page.finish();
}
TEST_F(JellyfinAccountTest, DelayedSecureWriteCanBeCancelledWithoutActivatingOrOverwritingNextAttempt)
{
    auto credentials                         = std::make_shared<DelayedCredentialStore>();
    const QString previousCredentialRecordId = sourceKey(ServerId, UserId) + u"/00000000-0000-4000-8000-000000000003"_s;
    const QString previous                   = u"synthetic-previous-token"_s;
    ASSERT_TRUE(credentials->writeToken(previousCredentialRecordId, previous, {}).isEmpty());
    storeConnection(*m_credentialStateStore, m_server.url({}), previousCredentialRecordId, UserId);
    credentials->delayWrites = true;
    Account account{&m_network, &m_library, &m_settings, m_credentialStateStore, credentials, m_access, 20ms, nullptr};
    auto scenario          = std::make_shared<QuickConnectScenario>();
    scenario->pendingPolls = 0;
    m_server.setHandler(quickConnectHandler(scenario));
    account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor([&]() { return credentials->writeEntered.load(); }, 5000));
    bool responsive{false};
    QTimer::singleShot(0, &account, [&]() { responsive = true; });
    ASSERT_TRUE(QTest::qWaitFor([&]() { return responsive; }, 1000));
    account.cancelQuickConnect();
    ASSERT_TRUE(QTest::qWaitFor([&]() { return credentials->token(previousCredentialRecordId) == previous; }, 5000));
    EXPECT_FALSE(account.connection());
    EXPECT_FALSE(m_access->access());
    EXPECT_EQ(m_library.snapshotRequests, 0);

    credentials->delayWrites = false;
    auto next                = std::make_shared<QuickConnectScenario>();
    next->pendingPolls       = 0;
    const QString accepted   = u"synthetic-next-token"_s;
    next->exchangeResult.insert(u"AccessToken"_s, accepted);
    m_server.setHandler(quickConnectHandler(next));
    account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor([&]() { return account.connection().has_value(); }, 5000));
    EXPECT_EQ(credentials->token(credentialRecordId(*m_credentialStateStore)), accepted);
    EXPECT_NE(credentialRecordId(*m_credentialStateStore), previousCredentialRecordId);
    ASSERT_TRUE(m_access->access());
    EXPECT_TRUE(m_access->access()->authorization.contains(accepted.toUtf8()));
}

TEST_F(JellyfinAccountTest, DelayedRestoreCannotReplaceAcceptedQuickConnect)
{
    auto credentials                         = std::make_shared<DelayedCredentialStore>();
    const QString previousCredentialRecordId = sourceKey(ServerId, UserId) + u"/00000000-0000-4000-8000-000000000003"_s;
    ASSERT_TRUE(credentials->writeToken(previousCredentialRecordId, u"synthetic-old-token"_s, {}).isEmpty());
    storeConnection(*m_credentialStateStore, m_server.url({}), previousCredentialRecordId, UserId);
    credentials->delayReads = true;
    Account account{&m_network, &m_library, &m_settings, m_credentialStateStore, credentials, m_access, 20ms, nullptr};
    account.restore();
    ASSERT_TRUE(QTest::qWaitFor([&]() { return credentials->readEntered.load(); }, 5000));
    bool responsive{false};
    QTimer::singleShot(0, &account, [&]() { responsive = true; });
    ASSERT_TRUE(QTest::qWaitFor([&]() { return responsive; }, 1000));
    credentials->delayReads = false;
    auto scenario           = std::make_shared<QuickConnectScenario>();
    scenario->pendingPolls  = 0;
    m_server.setHandler(quickConnectHandler(scenario));
    account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor([&]() { return account.connection().has_value(); }, 5000));
    EXPECT_EQ(credentials->token(credentialRecordId(*m_credentialStateStore)), Token);
    ASSERT_TRUE(m_access->access());
    EXPECT_TRUE(m_access->access()->authorization.contains(Token.toUtf8()));
    ASSERT_TRUE(QTest::qWaitFor([this]() { return m_library.snapshotRequests == 1; }, 5000));
}

TEST_F(JellyfinAccountTest, FreshRegistryRestoresRetainedArtworkWhenCatalogRefreshFails)
{
    auto scenario          = std::make_shared<QuickConnectScenario>();
    scenario->pendingPolls = 0;
    m_server.setHandler(quickConnectHandler(scenario));
    m_account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor([this]() { return !m_library.tracks().empty(); }, 5000));
    const Track retained = m_library.tracks().front();
    EXPECT_EQ(retained.extraProperties().value(u"Jellyfin/AlbumId"_s), u"album1"_s);
    EXPECT_EQ(retained.extraProperties().value(u"Jellyfin/ImageItemId"_s), u"album1"_s);
    m_server.clearRequests();
    m_server.setHandler([](const FakeRequest& request) {
        if(request.path.endsWith("/Items/album1/Images/Primary"_L1)) {
            return FakeJellyfinServer::bytes(request, "retained-synthetic-image", false);
        }
        return FakeJellyfinServer::text(503, {});
    });
    auto freshAccess = std::make_shared<AccessRegistry>();
    Account restored{&m_network,    &m_library,  &m_settings, m_credentialStateStore,
                     m_credentials, freshAccess, 20ms,        nullptr};
    QSignalSpy synchronised{restored.catalogSync(), &CatalogSync::finished};
    restored.restore();
    ASSERT_TRUE(QTest::qWaitFor([&]() { return !synchronised.isEmpty(); }, 5000));
    EXPECT_FALSE(synchronised.front().at(0).toBool());
    ASSERT_TRUE(freshAccess->access());
    EXPECT_EQ(m_library.tracks().front().id(), retained.id());
    EXPECT_EQ(m_library.activeLibraryTracks().size(), 1U);
    RemoteIoFixture transport;
    Reader reader{freshAccess, transport.remoteIo, 5s};
    AudioSource source;
    source.filepath = retained.filepath();
    EXPECT_EQ(reader.readCover(source, retained, Track::Cover::Front), "retained-synthetic-image");
    const auto images = m_server.requestsForPath(u"/Items/album1/Images/Primary"_s);
    ASSERT_EQ(images.size(), 1U);
    EXPECT_TRUE(images.front().header("authorization").contains(TokenField));
    for(const auto& request : m_server.requests()) {
        EXPECT_FALSE(request.path.contains("/Audio/"_L1));
    }
}

TEST_F(JellyfinAccountTest, AcceptedAccountSwitchHidesPriorSourceAndSignOutRetainsBothCatalogs)
{
    auto first          = std::make_shared<QuickConnectScenario>();
    first->pendingPolls = 0;
    m_server.setHandler(quickConnectHandler(first));
    m_account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor(
        [&]() { return m_library.tracks().size() == 1 && !m_account.catalogSync()->isRunning(); }, 5000));
    const Track retained = m_library.tracks().front();
    auto second          = std::make_shared<QuickConnectScenario>();
    second->pendingPolls = 1000;
    second->exchangeResult.insert(u"User"_s, QJsonObject{{u"Id"_s, u"user2"_s}, {u"Name"_s, u"Second User"_s}});
    second->exchangeResult.insert(u"AccessToken"_s, u"synthetic-second-token"_s);
    m_server.setHandler(quickConnectHandler(second));
    m_account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor([&]() { return !m_account.quickConnectCode().isEmpty(); }, 5000));
    EXPECT_TRUE(m_library.isTrackSourceActive(retained));
    second->pendingPolls = 0;
    ASSERT_TRUE(QTest::qWaitFor(
        [&]() {
            return m_account.connection() && m_account.connection()->userId == u"user2"_s
                && m_library.tracks().size() == 2 && !m_account.catalogSync()->isRunning();
        },
        5000));
    EXPECT_FALSE(m_library.isTrackSourceActive(retained));
    ASSERT_EQ(m_library.activeLibraryTracks().size(), 1U);
    EXPECT_NE(m_library.activeLibraryTracks().front().filepath(), retained.filepath());
    EXPECT_EQ(m_library.trackForId(retained.id()).filepath(), retained.filepath());
    m_account.signOut();
    EXPECT_TRUE(m_library.activeLibraryTracks().empty());
    EXPECT_EQ(m_library.tracks().size(), 2U);
}

TEST_F(JellyfinAccountTest, SignOutAfterSecureWriteBeforeAcceptanceCannotBeUndoneByStaleRollback)
{
    auto credentials = std::make_shared<DelayedCredentialStore>();
    Account account{&m_network, &m_library, &m_settings, m_credentialStateStore, credentials, m_access, 20ms, nullptr};
    auto initial          = std::make_shared<QuickConnectScenario>();
    initial->pendingPolls = 0;
    m_server.setHandler(quickConnectHandler(initial));
    account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor([&]() { return account.connection() && !account.catalogSync()->isRunning(); }, 5000));
    const QString previousCredentialRecordId = credentialRecordId(*m_credentialStateStore);
    const QString replacement                = u"synthetic-unaccepted-reconnect-token"_s;
    bool signedOut{false};
    QObject::connect(&account, &Account::credentialOperationFinished, &account, [&]() {
        if(signedOut || !account.isQuickConnectPending()
           || credentials->token(pendingCredentialRecordId(*m_credentialStateStore, sourceKey(ServerId, UserId)))
                  != replacement) {
            return;
        }
        signedOut = true;
        account.signOut();
        EXPECT_TRUE(QTest::qWaitFor([&]() { return credentials->token(previousCredentialRecordId).isEmpty(); }, 5000));
    });
    auto reconnect          = std::make_shared<QuickConnectScenario>();
    reconnect->pendingPolls = 0;
    reconnect->exchangeResult.insert(u"AccessToken"_s, replacement);
    m_server.setHandler(quickConnectHandler(reconnect));
    account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor([&]() { return signedOut; }, 5000));
    auto next          = std::make_shared<QuickConnectScenario>();
    next->pendingPolls = 0;
    next->exchangeResult.insert(u"User"_s, QJsonObject{{u"Id"_s, u"user2"_s}, {u"Name"_s, u"Second"_s}});
    m_server.setHandler(quickConnectHandler(next));
    account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(
        QTest::qWaitFor([&]() { return account.connection() && account.connection()->userId == u"user2"_s; }, 5000));
    EXPECT_TRUE(credentials->token(previousCredentialRecordId).isEmpty());
    EXPECT_EQ(credentials->token(credentialRecordId(*m_credentialStateStore)), Token);
}

TEST_F(JellyfinAccountTest, SameSourcePairingCancellationCannotAbandonDelayedSignOutRemoval)
{
    auto credentials = std::make_shared<DelayedCredentialStore>();
    Account account{&m_network, &m_library, &m_settings, m_credentialStateStore, credentials, m_access, 20ms, nullptr};
    const auto release     = qScopeGuard([&]() { credentials->releaseRemovals(); });
    auto scenario          = std::make_shared<QuickConnectScenario>();
    scenario->pendingPolls = 0;
    m_server.setHandler(quickConnectHandler(scenario));
    account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor([&]() { return account.connection().has_value(); }, 5000));
    const QString previousCredentialRecordId = credentialRecordId(*m_credentialStateStore);
    credentials->delayRemovals               = true;
    account.signOut();
    ASSERT_TRUE(QTest::qWaitFor([&]() { return credentials->removalEntered.load(); }, 5000));
    account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor([&]() { return account.status().contains(u"Saving"_s); }, 5000));
    account.cancelQuickConnect();
    EXPECT_FALSE(credentials->removalCancelled);
    credentials->releaseRemovals();
    ASSERT_TRUE(QTest::qWaitFor([&]() { return credentials->removalCompleted.load() >= 1; }, 5000));
    EXPECT_TRUE(credentials->token(previousCredentialRecordId).isEmpty());
    scenario->exchangeResult.insert(u"AccessToken"_s, u"synthetic-later-accepted-token"_s);
    account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor([&]() { return account.connection().has_value(); }, 5000));
    EXPECT_EQ(credentials->token(credentialRecordId(*m_credentialStateStore)), u"synthetic-later-accepted-token"_s);
    EXPECT_NE(credentialRecordId(*m_credentialStateStore), previousCredentialRecordId);
    EXPECT_FALSE(credentials->removalCancelled);
}

TEST_F(JellyfinAccountTest, DifferentSourcePairingCancellationCannotAbandonDelayedSignOutRemoval)
{
    auto credentials = std::make_shared<DelayedCredentialStore>();
    Account account{&m_network, &m_library, &m_settings, m_credentialStateStore, credentials, m_access, 20ms, nullptr};
    const auto release     = qScopeGuard([&]() { credentials->releaseRemovals(); });
    auto scenario          = std::make_shared<QuickConnectScenario>();
    scenario->pendingPolls = 0;
    m_server.setHandler(quickConnectHandler(scenario));
    account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor([&]() { return account.connection().has_value(); }, 5000));
    const QString previousCredentialRecordId = credentialRecordId(*m_credentialStateStore);
    credentials->delayRemovals               = true;
    account.signOut();
    ASSERT_TRUE(QTest::qWaitFor([&]() { return credentials->removalEntered.load(); }, 5000));
    auto second          = std::make_shared<QuickConnectScenario>();
    second->pendingPolls = 0;
    second->exchangeResult.insert(u"User"_s, QJsonObject{{u"Id"_s, u"user2"_s}, {u"Name"_s, u"Second"_s}});
    m_server.setHandler(quickConnectHandler(second));
    account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor([&]() { return account.status().contains(u"Saving"_s); }, 5000));
    account.cancelQuickConnect();
    credentials->releaseRemovals();
    ASSERT_TRUE(QTest::qWaitFor([&]() { return credentials->removalCompleted.load() >= 1; }, 5000));
    EXPECT_TRUE(credentials->token(previousCredentialRecordId).isEmpty());
    EXPECT_TRUE(credentialRecordId(*m_credentialStateStore).isEmpty());
    account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(
        QTest::qWaitFor([&]() { return account.connection() && account.connection()->userId == u"user2"_s; }, 5000));
    EXPECT_EQ(credentials->token(credentialRecordId(*m_credentialStateStore)), Token);
    EXPECT_TRUE(credentials->token(previousCredentialRecordId).isEmpty());
    EXPECT_FALSE(credentials->removalCancelled);
}

TEST_F(JellyfinAccountTest,
       FailedCleanupRemainsReportedAndRetriesAfterAccountReconstructionWithoutDeletingNewCredentials)
{
    auto credentials = std::make_shared<DelayedCredentialStore>();
    Account account{&m_network, &m_library, &m_settings, m_credentialStateStore, credentials, m_access, 20ms, nullptr};
    auto scenario          = std::make_shared<QuickConnectScenario>();
    scenario->pendingPolls = 0;
    m_server.setHandler(quickConnectHandler(scenario));
    account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor([&]() { return account.connection().has_value(); }, 5000));
    const QString previousCredentialRecordId = credentialRecordId(*m_credentialStateStore);
    credentials->setRemovalError(u"Synthetic secure cleanup failure"_s);
    account.signOut();
    ASSERT_TRUE(
        QTest::qWaitFor([&]() { return account.status().contains(u"Synthetic secure cleanup failure"_s); }, 5000));
    EXPECT_TRUE(hasPendingCredentialCleanup(*m_credentialStateStore, previousCredentialRecordId));
    EXPECT_EQ(credentials->token(previousCredentialRecordId), Token);
    auto second          = std::make_shared<QuickConnectScenario>();
    second->pendingPolls = 0;
    second->exchangeResult.insert(u"User"_s, QJsonObject{{u"Id"_s, u"user2"_s}, {u"Name"_s, u"Second"_s}});
    second->exchangeResult.insert(u"AccessToken"_s, u"synthetic-new-account-token"_s);
    m_server.setHandler(quickConnectHandler(second));
    account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor(
        [&]() {
            return account.connection() && account.connection()->userId == u"user2"_s
                && !account.catalogSync()->isRunning();
        },
        5000));
    EXPECT_TRUE(account.status().contains(u"Synthetic secure cleanup failure"_s));
    EXPECT_TRUE(hasPendingCredentialCleanup(*m_credentialStateStore, previousCredentialRecordId));
    credentials->setRemovalError({});
    auto restoredAccess = std::make_shared<AccessRegistry>();
    Account restored{&m_network,  &m_library,     &m_settings, m_credentialStateStore,
                     credentials, restoredAccess, 20ms,        nullptr};
    restored.restore();
    ASSERT_TRUE(QTest::qWaitFor(
        [&]() {
            return restored.connection()
                && !hasPendingCredentialCleanup(*m_credentialStateStore, previousCredentialRecordId);
        },
        5000));
    EXPECT_TRUE(credentials->token(previousCredentialRecordId).isEmpty());
    EXPECT_EQ(credentials->token(credentialRecordId(*m_credentialStateStore)), u"synthetic-new-account-token"_s);
    EXPECT_EQ(restored.connection()->userId, u"user2"_s);
    QFile settingsFile{m_settingsPath};
    ASSERT_TRUE(settingsFile.open(QIODevice::ReadOnly));
    const QByteArray stored = settingsFile.readAll();
    EXPECT_FALSE(stored.contains(Token.toUtf8()));
    EXPECT_FALSE(stored.contains("synthetic-new-account-token"));
}

TEST_F(JellyfinAccountTest, CancellingSourceWhileAnotherSourceStartsStillRollsBackItsUnacceptedWrite)
{
    auto credentials                         = std::make_shared<DelayedCredentialStore>();
    const QString previousCredentialRecordId = sourceKey(ServerId, UserId) + u"/00000000-0000-4000-8000-000000000003"_s;
    ASSERT_TRUE(credentials->writeToken(previousCredentialRecordId, u"synthetic-previous-token"_s, {}).isEmpty());
    storeConnection(*m_credentialStateStore, m_server.url({}), previousCredentialRecordId, UserId);
    credentials->delayWrites = true;
    Account account{&m_network, &m_library, &m_settings, m_credentialStateStore, credentials, m_access, 20ms, nullptr};
    auto scenario          = std::make_shared<QuickConnectScenario>();
    scenario->pendingPolls = 0;
    m_server.setHandler(quickConnectHandler(scenario));
    account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor([&]() { return credentials->writeEntered.load(); }, 5000));
    auto second          = std::make_shared<QuickConnectScenario>();
    second->pendingPolls = 1000;
    second->exchangeResult.insert(u"User"_s, QJsonObject{{u"Id"_s, u"user2"_s}, {u"Name"_s, u"Second"_s}});
    m_server.setHandler(quickConnectHandler(second));
    account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor(
        [&]() { return credentials->token(previousCredentialRecordId) == u"synthetic-previous-token"_s; }, 5000));
    EXPECT_FALSE(account.connection());
    EXPECT_EQ(m_library.snapshotRequests, 0);
    account.cancelQuickConnect();
}

TEST_F(JellyfinAccountTest, AcceptedMetadataIsolatesRestoredSourcesBeforeDelayedCredentialLookup)
{
    const QString aSourceKey = sourceKey(ServerId, UserId);
    const QString bSourceKey = sourceKey(ServerId, u"user2"_s);
    const auto a             = m_library.addExternalLibrary(libraryUri(aSourceKey), u"Prior"_s);
    const auto b             = m_library.addExternalLibrary(libraryUri(bSourceKey), u"Accepted"_s);
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    Track aTrack{libraryUri(aSourceKey) + u"/a"_s};
    Track bTrack{libraryUri(bSourceKey) + u"/b"_s};
    aTrack.setId(1);
    bTrack.setId(2);
    aTrack.setLibraryId(a->id);
    bTrack.setLibraryId(b->id);
    aTrack.setExternalLibraryPath(a->path);
    bTrack.setExternalLibraryPath(b->path);
    QSignalSpy seeded{&m_library, &MusicLibrary::externalLibrarySnapshotFinished};
    m_library.applyExternalLibrarySnapshot(*a, {aTrack}, QUuid::createUuid());
    m_library.applyExternalLibrarySnapshot(*b, {bTrack}, QUuid::createUuid());
    ASSERT_TRUE(QTest::qWaitFor([&]() { return seeded.size() == 2; }, 1000));
    m_library.setExternalLibraryActive(*a, true);
    m_library.setExternalLibraryActive(*b, false);
    auto credentials = std::make_shared<DelayedCredentialStore>();
    const QString previousCredentialRecordId
        = sourceKey(ServerId, u"user2"_s) + u"/00000000-0000-4000-8000-000000000004"_s;
    ASSERT_TRUE(credentials->writeToken(previousCredentialRecordId, u"synthetic-b-token"_s, {}).isEmpty());
    storeConnection(*m_credentialStateStore, m_server.url({}), previousCredentialRecordId, u"user2"_s);
    auto state              = credentialState(*m_credentialStateStore);
    auto acceptedConnection = state.value(u"acceptedConnection"_s).toObject();
    acceptedConnection.insert(u"userId"_s, u"user2"_s);
    state.insert(u"acceptedConnection"_s, acceptedConnection);
    state.insert(u"sourceKeys"_s, QJsonArray{aSourceKey, bSourceKey});
    ASSERT_TRUE(m_credentialStateStore->commit(state).isEmpty());
    credentials->delayReads = true;
    Account restored{&m_network, &m_library, &m_settings, m_credentialStateStore, credentials, m_access, 20ms, nullptr};
    EXPECT_FALSE(m_library.isTrackSourceActive(aTrack));
    EXPECT_TRUE(m_library.isTrackSourceActive(bTrack));
    EXPECT_FALSE(m_access->access());
    restored.restore();
    ASSERT_TRUE(QTest::qWaitFor([&]() { return credentials->readEntered.load(); }, 5000));
    EXPECT_FALSE(restored.connection());
    EXPECT_FALSE(m_access->access());
    ASSERT_EQ(m_library.activeLibraryTracks().size(), 1U);
    EXPECT_EQ(m_library.activeLibraryTracks().front().id(), bTrack.id());
    restored.cancelQuickConnect();
    EXPECT_TRUE(m_library.isTrackSourceActive(bTrack));
    EXPECT_FALSE(m_library.isTrackSourceActive(aTrack));
    state.remove(u"acceptedConnection"_s);
    ASSERT_TRUE(m_credentialStateStore->commit(state).isEmpty());
    Account signedOut{
        &m_network, &m_library, &m_settings, m_credentialStateStore, credentials, std::make_shared<AccessRegistry>(),
        20ms,       nullptr};
    EXPECT_FALSE(m_library.isTrackSourceActive(aTrack));
    EXPECT_FALSE(m_library.isTrackSourceActive(bTrack));
    signedOut.restore();
    EXPECT_FALSE(signedOut.connection());
    EXPECT_TRUE(m_library.activeLibraryTracks().empty());
}

TEST_F(JellyfinAccountTest, DestructionDuringCandidateWriteCancelsProviderAndPreservesAcceptedRecord)
{
    auto credentials                         = std::make_shared<DelayedCredentialStore>();
    const QString previousCredentialRecordId = sourceKey(ServerId, UserId) + u"/00000000-0000-4000-8000-000000000001"_s;
    ASSERT_TRUE(credentials->writeToken(previousCredentialRecordId, u"synthetic-baseline-token"_s, {}).isEmpty());
    storeConnection(*m_credentialStateStore, m_server.url({}), previousCredentialRecordId, UserId);
    credentials->delayWrites = true;
    auto account  = std::make_unique<Account>(&m_network, &m_library, &m_settings, m_credentialStateStore, credentials,
                                              m_access, 20ms, nullptr);
    auto scenario = std::make_shared<QuickConnectScenario>();
    scenario->pendingPolls = 0;
    m_server.setHandler(quickConnectHandler(scenario));
    account->startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor([&]() { return credentials->writeEntered.load(); }, 5000));
    const QString discardedCredentialRecordId
        = pendingCredentialRecordId(*m_credentialStateStore, sourceKey(ServerId, UserId));
    ASSERT_FALSE(discardedCredentialRecordId.isEmpty());
    QElapsedTimer timer;
    timer.start();
    account.reset();
    EXPECT_LT(timer.elapsed(), 1000);
    EXPECT_EQ(credentials->maxConcurrentCalls.load(), 1);
    EXPECT_EQ(credentialRecordId(*m_credentialStateStore), previousCredentialRecordId);
    EXPECT_EQ(credentials->token(previousCredentialRecordId), u"synthetic-baseline-token"_s);
    EXPECT_TRUE(hasPendingCredentialCleanup(*m_credentialStateStore, discardedCredentialRecordId));
    credentials->releaseWrites();
    Account restored{&m_network, &m_library, &m_settings, m_credentialStateStore, credentials, m_access, 20ms, nullptr};
    restored.restore();
    ASSERT_TRUE(QTest::qWaitFor([&]() { return restored.connection().has_value(); }, 5000));
    ASSERT_TRUE(QTest::qWaitFor(
        [&]() { return !hasPendingCredentialCleanup(*m_credentialStateStore, discardedCredentialRecordId); }, 5000));
    EXPECT_TRUE(credentials->token(discardedCredentialRecordId).isEmpty());
    EXPECT_TRUE(m_access->access()->authorization.contains("synthetic-baseline-token"));
}

TEST_F(JellyfinAccountTest, DestructionAfterSecureWriteBeforeAcceptanceCannotActivateOrLoseAcceptedRecord)
{
    auto credentials                         = std::make_shared<DelayedCredentialStore>();
    const QString previousCredentialRecordId = sourceKey(ServerId, UserId) + u"/00000000-0000-4000-8000-000000000001"_s;
    ASSERT_TRUE(credentials->writeToken(previousCredentialRecordId, u"synthetic-baseline-token"_s, {}).isEmpty());
    storeConnection(*m_credentialStateStore, m_server.url({}), previousCredentialRecordId, UserId);
    credentials->delayWrites = true;
    auto completed           = credentials->observeWriteCompletion();
    auto account  = std::make_unique<Account>(&m_network, &m_library, &m_settings, m_credentialStateStore, credentials,
                                              m_access, 20ms, nullptr);
    auto scenario = std::make_shared<QuickConnectScenario>();
    scenario->pendingPolls = 0;
    m_server.setHandler(quickConnectHandler(scenario));
    account->startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor([&]() { return credentials->writeEntered.load(); }, 5000));
    const QString discardedCredentialRecordId
        = pendingCredentialRecordId(*m_credentialStateStore, sourceKey(ServerId, UserId));
    credentials->releaseWrites();
    ASSERT_EQ(completed.wait_for(1s), std::future_status::ready);
    account.reset();
    QTest::qWait(50);
    EXPECT_FALSE(m_access->access());
    EXPECT_EQ(m_library.snapshotRequests, 0);
    EXPECT_EQ(credentialRecordId(*m_credentialStateStore), previousCredentialRecordId);
    EXPECT_EQ(credentials->token(previousCredentialRecordId), u"synthetic-baseline-token"_s);
    EXPECT_TRUE(hasPendingCredentialCleanup(*m_credentialStateStore, discardedCredentialRecordId));
    EXPECT_EQ(credentials->maxConcurrentCalls.load(), 1);
}

TEST_F(JellyfinAccountTest, DestructionCancelsRequiredRemovalAndReconstructionRetriesSpecificRecord)
{
    auto credentials = std::make_shared<DelayedCredentialStore>();
    auto account  = std::make_unique<Account>(&m_network, &m_library, &m_settings, m_credentialStateStore, credentials,
                                              m_access, 20ms, nullptr);
    auto scenario = std::make_shared<QuickConnectScenario>();
    scenario->pendingPolls = 0;
    m_server.setHandler(quickConnectHandler(scenario));
    account->startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor([&]() { return account->connection().has_value(); }, 5000));
    const QString previousCredentialRecordId = credentialRecordId(*m_credentialStateStore);
    credentials->delayRemovals               = true;
    account->signOut();
    ASSERT_TRUE(QTest::qWaitFor([&]() { return credentials->removalEntered.load(); }, 5000));
    QElapsedTimer timer;
    timer.start();
    account.reset();
    EXPECT_LT(timer.elapsed(), 1000);
    EXPECT_TRUE(credentials->removalCancelled);
    EXPECT_EQ(credentials->maxConcurrentCalls.load(), 1);
    EXPECT_TRUE(credentialRecordId(*m_credentialStateStore).isEmpty());
    EXPECT_TRUE(hasPendingCredentialCleanup(*m_credentialStateStore, previousCredentialRecordId));
    credentials->releaseRemovals();
    Account restored{&m_network, &m_library, &m_settings, m_credentialStateStore, credentials, m_access, 20ms, nullptr};
    restored.restore();
    ASSERT_TRUE(QTest::qWaitFor(
        [&]() { return !hasPendingCredentialCleanup(*m_credentialStateStore, previousCredentialRecordId); }, 5000));
    EXPECT_FALSE(restored.connection());
    EXPECT_TRUE(credentials->token(previousCredentialRecordId).isEmpty());
}

TEST_F(JellyfinAccountTest, DestructionCancelsDiscardCleanupWhileAcceptedCredentialStillRestores)
{
    auto credentials                         = std::make_shared<DelayedCredentialStore>();
    const QString previousCredentialRecordId = sourceKey(ServerId, UserId) + u"/00000000-0000-4000-8000-000000000001"_s;
    ASSERT_TRUE(credentials->writeToken(previousCredentialRecordId, u"synthetic-baseline-token"_s, {}).isEmpty());
    storeConnection(*m_credentialStateStore, m_server.url({}), previousCredentialRecordId, UserId);
    credentials->delayWrites = true;
    auto account  = std::make_unique<Account>(&m_network, &m_library, &m_settings, m_credentialStateStore, credentials,
                                              m_access, 20ms, nullptr);
    auto scenario = std::make_shared<QuickConnectScenario>();
    scenario->pendingPolls = 0;
    m_server.setHandler(quickConnectHandler(scenario));
    account->startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor([&]() { return credentials->writeEntered.load(); }, 5000));
    const QString discardedCredentialRecordId
        = pendingCredentialRecordId(*m_credentialStateStore, sourceKey(ServerId, UserId));
    credentials->delayRemovals = true;
    account->cancelQuickConnect();
    ASSERT_TRUE(QTest::qWaitFor([&]() { return credentials->removalEntered.load(); }, 5000));
    QElapsedTimer timer;
    timer.start();
    account.reset();
    EXPECT_LT(timer.elapsed(), 1000);
    EXPECT_TRUE(credentials->removalCancelled);
    EXPECT_EQ(credentials->maxConcurrentCalls.load(), 1);
    EXPECT_EQ(credentialRecordId(*m_credentialStateStore), previousCredentialRecordId);
    EXPECT_TRUE(hasPendingCredentialCleanup(*m_credentialStateStore, discardedCredentialRecordId));
    credentials->releaseRemovals();
    credentials->releaseWrites();
    Account restored{&m_network, &m_library, &m_settings, m_credentialStateStore, credentials, m_access, 20ms, nullptr};
    restored.restore();
    ASSERT_TRUE(QTest::qWaitFor([&]() { return restored.connection().has_value(); }, 5000));
    EXPECT_EQ(credentials->token(previousCredentialRecordId), u"synthetic-baseline-token"_s);
    EXPECT_TRUE(credentials->token(discardedCredentialRecordId).isEmpty());
    EXPECT_TRUE(m_access->access()->authorization.contains("synthetic-baseline-token"));
}

TEST_F(JellyfinAccountTest, ReconstructionNeverCleansAcceptedRecordEvenIfCleanupIntentContainsIt)
{
    auto credentials                         = std::make_shared<DelayedCredentialStore>();
    const QString previousCredentialRecordId = sourceKey(ServerId, UserId) + u"/00000000-0000-4000-8000-000000000001"_s;
    ASSERT_TRUE(credentials->writeToken(previousCredentialRecordId, Token, {}).isEmpty());
    storeConnection(*m_credentialStateStore, m_server.url({}), previousCredentialRecordId, UserId);
    auto state = credentialState(*m_credentialStateStore);
    state.insert(u"credentialRecords"_s,
                 QJsonArray{QJsonObject{{u"sourceKey"_s, sourceKey(ServerId, UserId)},
                                        {u"credentialRecordId"_s, previousCredentialRecordId}}});
    ASSERT_TRUE(m_credentialStateStore->commit(state).isEmpty());
    m_server.setHandler(quickConnectHandler(std::make_shared<QuickConnectScenario>()));
    {
        Account restored{&m_network,  &m_library, &m_settings, m_credentialStateStore,
                         credentials, m_access,   20ms,        nullptr};
        restored.restore();
        ASSERT_TRUE(QTest::qWaitFor([&]() { return restored.connection().has_value(); }, 5000));
        EXPECT_FALSE(hasPendingCredentialCleanup(*m_credentialStateStore, previousCredentialRecordId));
        EXPECT_TRUE(credentialState(*m_credentialStateStore)
                        .value(u"sourceKeys"_s)
                        .toArray()
                        .contains(sourceKey(ServerId, UserId)));
        EXPECT_EQ(credentials->removalCalls.load(), 0);
    }
    EXPECT_EQ(credentialRecordId(*m_credentialStateStore), previousCredentialRecordId);
    EXPECT_EQ(credentials->token(previousCredentialRecordId), Token);
    EXPECT_EQ(credentials->removalCalls.load(), 0);
}

TEST_F(JellyfinAccountTest, FailedSignOutCommitPreservesActiveAccessCatalogAndCredential)
{
    const QString credentialStatePath        = m_dir.filePath(u"checked-session.json"_s);
    auto credentialStateStore                = std::make_shared<FailingCredentialStateStore>(credentialStatePath);
    auto credentials                         = std::make_shared<DelayedCredentialStore>();
    const QString previousCredentialRecordId = sourceKey(ServerId, UserId) + u"/00000000-0000-4000-8000-000000000001"_s;
    ASSERT_TRUE(credentials->writeToken(previousCredentialRecordId, Token, {}).isEmpty());
    storeConnection(*credentialStateStore, m_server.url({}), previousCredentialRecordId, UserId);
    auto scenario = std::make_shared<QuickConnectScenario>();
    m_server.setHandler(quickConnectHandler(scenario));
    Account account{&m_network, &m_library, &m_settings, credentialStateStore, credentials, m_access, 20ms, nullptr};
    account.restore();
    ASSERT_TRUE(QTest::qWaitFor(
        [&]() {
            return account.connection() && m_library.snapshotRequests == 1 && !account.catalogSync()->isRunning();
        },
        5000));
    ASSERT_FALSE(m_library.tracks().empty());
    const Track track = m_library.tracks().front();
    const auto access = m_access->access();
    ASSERT_TRUE(access);
    QFile beforeFile{credentialStatePath};
    ASSERT_TRUE(beforeFile.open(QIODevice::ReadOnly));
    const QByteArray before = beforeFile.readAll();
    beforeFile.close();
    m_server.setHandler([handler = quickConnectHandler(scenario)](const FakeRequest& request) {
        auto response = handler(request);
        if(request.path.endsWith(u"/Items"_s)) {
            response.delayMs = 100;
        }
        return response;
    });
    JellyfinPage page{&account, &m_settings, nullptr};
    QWidget* widget = page.widget();
    ASSERT_NE(widget, nullptr);
    page.load();
    account.refresh();
    ASSERT_TRUE(account.catalogSync()->isRunning());
    credentialStateStore->failSignOut = true;
    QSignalSpy changed{&account, &Account::connectionChanged};
    account.signOut();
    EXPECT_EQ(credentialStateStore->failedCommits, 1);
    EXPECT_TRUE(account.connection());
    EXPECT_TRUE(account.catalogSync()->isRunning());
    EXPECT_TRUE(m_library.isTrackSourceActive(track));
    EXPECT_EQ(m_library.activeLibraryTracks().size(), 1U);
    ASSERT_TRUE(m_access->access());
    EXPECT_EQ(m_access->access()->sourceGeneration, access->sourceGeneration);
    EXPECT_EQ(m_access->access()->authorization, access->authorization);
    EXPECT_FALSE(access->sourceLifetime.stop_requested());
    EXPECT_EQ(credentials->token(previousCredentialRecordId), Token);
    EXPECT_EQ(credentials->removalCalls.load(), 0);
    EXPECT_EQ(changed.size(), 0);
    EXPECT_TRUE(account.status().contains(u"Sign-out failed"_s));
    EXPECT_TRUE(hasLabel(widget, account.status()));
    ASSERT_TRUE(beforeFile.open(QIODevice::ReadOnly));
    EXPECT_EQ(beforeFile.readAll(), before);
    FileCredentialStateStore fresh{credentialStatePath};
    EXPECT_EQ(credentialRecordId(fresh), previousCredentialRecordId);
    EXPECT_FALSE(hasPendingCredentialCleanup(fresh, previousCredentialRecordId));
    ASSERT_TRUE(QTest::qWaitFor([&]() { return !account.catalogSync()->isRunning(); }, 5000));
    EXPECT_TRUE(m_library.isTrackSourceActive(track));
    EXPECT_FALSE(access->sourceLifetime.stop_requested());
    EXPECT_TRUE(account.status().contains(u"Sign-out failed"_s));
    EXPECT_TRUE(hasLabel(widget, account.status()));
    auto* signOut = findButton(widget, u"Sign out"_s);
    auto* refresh = findButton(widget, u"Refresh library"_s);
    ASSERT_NE(signOut, nullptr);
    ASSERT_NE(refresh, nullptr);
    EXPECT_TRUE(signOut->isEnabled());
    EXPECT_TRUE(refresh->isEnabled());
    saveScreenshot(widget, u"settings-signout-failed.png"_s);
    page.finish();
}

TEST_F(JellyfinAccountTest, SignOutCommitsExactCleanupBeforeDisconnectAndReconstructsWithoutAcceptedAccount)
{
    const QString credentialStatePath        = m_dir.filePath(u"checked-session.json"_s);
    auto credentialStateStore                = std::make_shared<FileCredentialStateStore>(credentialStatePath);
    auto credentials                         = std::make_shared<DelayedCredentialStore>();
    const QString previousCredentialRecordId = sourceKey(ServerId, UserId) + u"/00000000-0000-4000-8000-000000000001"_s;
    ASSERT_TRUE(credentials->writeToken(previousCredentialRecordId, Token, {}).isEmpty());
    storeConnection(*credentialStateStore, m_server.url({}), previousCredentialRecordId, UserId);
    m_server.setHandler(quickConnectHandler(std::make_shared<QuickConnectScenario>()));
    auto account = std::make_unique<Account>(&m_network, &m_library, &m_settings, credentialStateStore, credentials,
                                             m_access, 20ms, nullptr);
    account->restore();
    ASSERT_TRUE(QTest::qWaitFor(
        [&]() {
            return account->connection() && m_library.snapshotRequests == 1 && !account->catalogSync()->isRunning();
        },
        5000));
    ASSERT_FALSE(m_library.tracks().empty());
    const Track track = m_library.tracks().front();
    const auto access = m_access->access();
    ASSERT_TRUE(access);
    credentials->delayRemovals = true;
    bool disconnectSawCommittedCleanup{false};
    QObject::connect(account.get(), &Account::connectionChanged, account.get(), [&]() {
        FileCredentialStateStore fresh{credentialStatePath};
        disconnectSawCommittedCleanup
            = credentialRecordId(fresh).isEmpty() && hasPendingCredentialCleanup(fresh, previousCredentialRecordId);
    });
    account->signOut();
    EXPECT_TRUE(disconnectSawCommittedCleanup);
    EXPECT_FALSE(account->connection());
    EXPECT_FALSE(m_access->access());
    EXPECT_TRUE(access->sourceLifetime.stop_requested());
    EXPECT_FALSE(m_library.isTrackSourceActive(track));
    ASSERT_TRUE(QTest::qWaitFor([&]() { return credentials->removalEntered.load(); }, 5000));
    FileCredentialStateStore fresh{credentialStatePath};
    EXPECT_TRUE(credentialRecordId(fresh).isEmpty());
    EXPECT_TRUE(hasPendingCredentialCleanup(fresh, previousCredentialRecordId));
    account.reset();
    EXPECT_EQ(credentials->activeCalls.load(), 0);
    credentials->releaseRemovals();
    auto freshStore = std::make_shared<FileCredentialStateStore>(credentialStatePath);
    Account reconstructed{&m_network, &m_library, &m_settings, freshStore, credentials, m_access, 20ms, nullptr};
    reconstructed.restore();
    EXPECT_FALSE(reconstructed.connection());
    EXPECT_FALSE(m_access->access());
    EXPECT_FALSE(m_library.isTrackSourceActive(track));
    ASSERT_TRUE(
        QTest::qWaitFor([&]() { return !hasPendingCredentialCleanup(fresh, previousCredentialRecordId); }, 5000));
    EXPECT_TRUE(credentials->token(previousCredentialRecordId).isEmpty());
    EXPECT_TRUE(credentialRecordId(fresh).isEmpty());
    EXPECT_EQ(credentials->maxConcurrentCalls.load(), 1);
}

TEST_F(JellyfinAccountTest, FailedCandidateIntentDoesNotWriteSecureRecord)
{
    const QString credentialStatePath        = m_dir.filePath(u"checked-session.json"_s);
    auto credentialStateStore                = std::make_shared<FailingCredentialStateStore>(credentialStatePath);
    const QString previousCredentialRecordId = sourceKey(ServerId, UserId) + u"/00000000-0000-4000-8000-000000000001"_s;
    storeConnection(*credentialStateStore, m_server.url({}), previousCredentialRecordId, UserId);
    auto credentials = std::make_shared<DelayedCredentialStore>();
    ASSERT_TRUE(credentials->writeToken(previousCredentialRecordId, u"synthetic-baseline-token"_s, {}).isEmpty());
    credentials->writeEntered = false;
    Account account{&m_network, &m_library, &m_settings, credentialStateStore, credentials, m_access, 20ms, nullptr};
    auto scenario          = std::make_shared<QuickConnectScenario>();
    scenario->pendingPolls = 0;
    m_server.setHandler(quickConnectHandler(scenario));
    account.restore();
    ASSERT_TRUE(QTest::qWaitFor([&]() { return account.connection() && m_library.snapshotRequests == 1; }, 5000));
    const auto access = m_access->access();
    ASSERT_TRUE(access);
    credentialStateStore->failCandidateIntent = true;
    account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor([&]() { return !account.isQuickConnectPending(); }, 5000));
    EXPECT_EQ(credentialStateStore->failedCommits, 1);
    EXPECT_TRUE(account.connection());
    EXPECT_FALSE(credentials->writeEntered.load());
    EXPECT_EQ(credentials->removalCalls.load(), 0);
    EXPECT_EQ(credentials->token(previousCredentialRecordId), u"synthetic-baseline-token"_s);
    ASSERT_TRUE(m_access->access());
    EXPECT_EQ(m_access->access()->sourceGeneration, access->sourceGeneration);
    EXPECT_FALSE(access->sourceLifetime.stop_requested());
    EXPECT_EQ(m_library.snapshotRequests, 1);
    FileCredentialStateStore reconstructed{credentialStatePath};
    EXPECT_EQ(credentialRecordId(reconstructed), previousCredentialRecordId);
    EXPECT_TRUE(credentialState(reconstructed).value(u"credentialRecords"_s).toArray().isEmpty());
    EXPECT_TRUE(account.status().contains(u"metadata commit failure"_s));
}

TEST_F(JellyfinAccountTest, FailedAcceptanceKeepsPriorRecordAndCommittedCandidateCleanup)
{
    const QString credentialStatePath        = m_dir.filePath(u"checked-session.json"_s);
    auto credentialStateStore                = std::make_shared<FailingCredentialStateStore>(credentialStatePath);
    const QString previousCredentialRecordId = sourceKey(ServerId, UserId) + u"/00000000-0000-4000-8000-000000000001"_s;
    storeConnection(*credentialStateStore, m_server.url({}), previousCredentialRecordId, UserId);
    auto credentials = std::make_shared<DelayedCredentialStore>();
    ASSERT_TRUE(credentials->writeToken(previousCredentialRecordId, u"synthetic-baseline-token"_s, {}).isEmpty());
    credentials->delayRemovals           = true;
    credentialStateStore->failAcceptance = true;
    Account account{&m_network, &m_library, &m_settings, credentialStateStore, credentials, m_access, 20ms, nullptr};
    auto scenario          = std::make_shared<QuickConnectScenario>();
    scenario->pendingPolls = 0;
    m_server.setHandler(quickConnectHandler(scenario));
    account.restore();
    ASSERT_TRUE(QTest::qWaitFor([&]() { return account.connection() && m_library.snapshotRequests == 1; }, 5000));
    const auto access = m_access->access();
    ASSERT_TRUE(access);
    account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor([&]() { return !account.isQuickConnectPending(); }, 5000));
    ASSERT_EQ(credentialStateStore->failedCommits, 1);
    EXPECT_TRUE(account.connection());
    ASSERT_TRUE(m_access->access());
    EXPECT_EQ(m_access->access()->sourceGeneration, access->sourceGeneration);
    EXPECT_EQ(m_access->access()->authorization, access->authorization);
    EXPECT_FALSE(access->sourceLifetime.stop_requested());
    FileCredentialStateStore reconstructed{credentialStatePath};
    EXPECT_EQ(credentialRecordId(reconstructed), previousCredentialRecordId);
    const QString discardedCredentialRecordId = pendingCredentialRecordId(reconstructed, sourceKey(ServerId, UserId));
    ASSERT_FALSE(discardedCredentialRecordId.isEmpty());
    EXPECT_NE(discardedCredentialRecordId, previousCredentialRecordId);
    EXPECT_EQ(credentials->token(previousCredentialRecordId), u"synthetic-baseline-token"_s);
    EXPECT_EQ(m_library.snapshotRequests, 1);
    credentials->releaseRemovals();
    ASSERT_TRUE(QTest::qWaitFor(
        [&]() { return !hasPendingCredentialCleanup(reconstructed, discardedCredentialRecordId); }, 5000));
    EXPECT_EQ(credentialRecordId(reconstructed), previousCredentialRecordId);
    EXPECT_EQ(credentials->token(previousCredentialRecordId), u"synthetic-baseline-token"_s);
}

TEST_F(JellyfinAccountTest, FailedCleanupAcknowledgementRemainsDurableAndProtectsNewAcceptedRecord)
{
    const QString credentialStatePath        = m_dir.filePath(u"checked-session.json"_s);
    auto credentialStateStore                = std::make_shared<FailingCredentialStateStore>(credentialStatePath);
    const QString previousCredentialRecordId = sourceKey(ServerId, UserId) + u"/00000000-0000-4000-8000-000000000001"_s;
    storeConnection(*credentialStateStore, m_server.url({}), previousCredentialRecordId, UserId);
    m_credentials->tokens[previousCredentialRecordId] = u"synthetic-baseline-token"_s;
    credentialStateStore->failCleanupClear            = true;
    Account account{&m_network, &m_library, &m_settings, credentialStateStore, m_credentials, m_access, 20ms, nullptr};
    auto scenario          = std::make_shared<QuickConnectScenario>();
    scenario->pendingPolls = 0;
    m_server.setHandler(quickConnectHandler(scenario));
    account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor(
        [&]() { return account.connection().has_value() && credentialStateStore->failedCommits == 1; }, 5000));
    FileCredentialStateStore reconstructed{credentialStatePath};
    const QString acceptedCredentialRecordId = credentialRecordId(reconstructed);
    EXPECT_NE(acceptedCredentialRecordId, previousCredentialRecordId);
    EXPECT_EQ(m_credentials->tokens.at(acceptedCredentialRecordId), Token);
    EXPECT_FALSE(m_credentials->tokens.contains(previousCredentialRecordId));
    EXPECT_TRUE(hasPendingCredentialCleanup(reconstructed, previousCredentialRecordId));
    EXPECT_TRUE(account.status().contains(u"needs a saved acknowledgement"_s));
    auto freshStore  = std::make_shared<FileCredentialStateStore>(credentialStatePath);
    auto freshAccess = std::make_shared<AccessRegistry>();
    Account restored{&m_network, &m_library, &m_settings, freshStore, m_credentials, freshAccess, 20ms, nullptr};
    restored.restore();
    ASSERT_TRUE(QTest::qWaitFor(
        [&]() {
            return restored.connection().has_value()
                && !hasPendingCredentialCleanup(reconstructed, previousCredentialRecordId);
        },
        5000));
    EXPECT_EQ(credentialRecordId(reconstructed), acceptedCredentialRecordId);
    EXPECT_EQ(m_credentials->tokens.at(acceptedCredentialRecordId), Token);
    EXPECT_EQ(restored.connection()->deviceId, u"device1"_s);
}

TEST_F(JellyfinAccountTest, MalformedMetadataDoesNotBecomeEmptyAuthoritativeState)
{
    const QString credentialStatePath = m_dir.filePath(u"malformed-session.json"_s);
    QFile file{credentialStatePath};
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write("{malformed-state"), 16);
    file.close();
    auto credentialStateStore = std::make_shared<FileCredentialStateStore>(credentialStatePath);
    auto credentials          = std::make_shared<DelayedCredentialStore>();
    Account account{&m_network, &m_library, &m_settings, credentialStateStore, credentials, m_access, 20ms, nullptr};
    EXPECT_TRUE(account.status().contains(u"Cannot read Jellyfin session metadata"_s));
    account.restore();
    account.startQuickConnect(m_server.url({}).toString());
    EXPECT_FALSE(account.isQuickConnectPending());
    EXPECT_FALSE(account.connection());
    EXPECT_FALSE(m_access->access());
    EXPECT_FALSE(credentials->readEntered.load());
    EXPECT_FALSE(credentials->writeEntered.load());
    EXPECT_EQ(credentials->removalCalls.load(), 0);
    EXPECT_TRUE(m_server.requests().empty());
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(file.readAll(), QByteArray{"{malformed-state"});
}

TEST_F(JellyfinAccountTest, AccountSwitchRestoresAPendingFavouriteAndEditsWithTheNewAccount)
{
    auto first              = std::make_shared<QuickConnectScenario>();
    first->pendingPolls     = 0;
    first->favouriteDelayMs = 800;
    m_server.setHandler(quickConnectHandler(first));
    m_account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor(
        [&]() { return m_library.tracks().size() == 1 && !m_account.catalogSync()->isRunning(); }, 5000));
    const Track firstTrack = m_library.tracks().front();
    QSignalSpy failed{m_account.favouriteSync(), &FavouriteSync::failed};

    editLoved(m_library, firstTrack, true);
    EXPECT_TRUE(m_library.trackForId(firstTrack.id()).isLoved());
    ASSERT_TRUE(QTest::qWaitFor([&]() { return favouriteRequests(m_server).size() == 1; }, 5000));

    auto second          = std::make_shared<QuickConnectScenario>();
    second->pendingPolls = 0;
    second->exchangeResult.insert(u"User"_s, QJsonObject{{u"Id"_s, u"user2"_s}, {u"Name"_s, u"Second User"_s}});
    second->exchangeResult.insert(u"AccessToken"_s, u"synthetic-second-token"_s);
    m_server.setHandler(quickConnectHandler(second));
    m_account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor(
        [&]() {
            return m_account.connection() && m_account.connection()->userId == u"user2"_s
                && m_library.tracks().size() == 2 && !m_account.catalogSync()->isRunning();
        },
        5000));
    EXPECT_FALSE(m_library.trackForId(firstTrack.id()).isLoved());
    EXPECT_FALSE(m_library.canEditTrackStats(m_library.trackForId(firstTrack.id()), Track::Stat::Loved));
    QTest::qWait(1000);
    EXPECT_FALSE(m_library.trackForId(firstTrack.id()).isLoved());

    Track secondTrack;
    for(const Track& track : m_library.tracks()) {
        if(track.libraryId() != firstTrack.libraryId()) {
            secondTrack = track;
        }
    }
    ASSERT_TRUE(m_library.canEditTrackStats(secondTrack, Track::Stat::Loved));
    editLoved(m_library, secondTrack, true);
    ASSERT_TRUE(QTest::qWaitFor([&]() { return favouriteRequests(m_server).size() == 2; }, 5000));
    const FakeRequest request = favouriteRequests(m_server).back();
    EXPECT_EQ(request.query.queryItemValue(u"userId"_s), u"user2"_s);
    EXPECT_TRUE(request.header("authorization").contains("Token=\"synthetic-second-token\""));
    ASSERT_TRUE(QTest::qWaitFor([&]() { return m_library.trackForId(secondTrack.id()).isLoved(); }, 5000));
    EXPECT_TRUE(failed.isEmpty());
}

TEST_F(JellyfinAccountTest, SignOutRestoresAPendingFavouriteAndStopsEditing)
{
    auto scenario              = std::make_shared<QuickConnectScenario>();
    scenario->pendingPolls     = 0;
    scenario->favouriteDelayMs = 800;
    m_server.setHandler(quickConnectHandler(scenario));
    m_account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor(
        [&]() { return m_library.tracks().size() == 1 && !m_account.catalogSync()->isRunning(); }, 5000));
    const Track track = m_library.tracks().front();
    QSignalSpy failed{m_account.favouriteSync(), &FavouriteSync::failed};

    editLoved(m_library, track, true);
    ASSERT_TRUE(QTest::qWaitFor([&]() { return favouriteRequests(m_server).size() == 1; }, 5000));
    m_account.signOut();
    ASSERT_FALSE(m_account.connection());
    EXPECT_FALSE(m_library.trackForId(track.id()).isLoved());
    EXPECT_FALSE(m_library.canEditTrackStats(m_library.trackForId(track.id()), Track::Stat::Loved));

    QTest::qWait(1000);
    EXPECT_FALSE(m_library.trackForId(track.id()).isLoved());
    EXPECT_EQ(favouriteRequests(m_server).size(), 1U);
    EXPECT_TRUE(failed.isEmpty());
}

TEST_F(JellyfinAccountTest, SignOutReportsAFailedFavouriteRollbackInTheBanner)
{
    auto scenario              = std::make_shared<QuickConnectScenario>();
    scenario->pendingPolls     = 0;
    scenario->favouriteDelayMs = 800;
    m_server.setHandler(quickConnectHandler(scenario));
    m_account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor(
        [&]() { return m_library.tracks().size() == 1 && !m_account.catalogSync()->isRunning(); }, 5000));
    const Track track = m_library.tracks().front();
    QWidget window;
    window.show();
    connectFavouriteBanner(m_account.favouriteSync(), &window);
    QSignalSpy failed{m_account.favouriteSync(), &FavouriteSync::failed};
    const LogCapture log;

    editLoved(m_library, track, true);
    ASSERT_TRUE(QTest::qWaitFor([&]() { return favouriteRequests(m_server).size() == 1; }, 5000));
    m_library.failTrackStats = true;
    m_account.signOut();
    ASSERT_FALSE(m_account.connection());
    EXPECT_FALSE(m_library.trackForId(track.id()).isLoved());

    MessageBanner* banner{nullptr};
    ASSERT_TRUE(QTest::qWaitFor(
        [&]() {
            banner = window.findChild<MessageBanner*>();
            return banner && banner->isVisible();
        },
        5000));
    EXPECT_EQ(banner->text(), u"The player could not save the Jellyfin favourite change."_s);
    EXPECT_FALSE(m_library.trackForId(track.id()).isLoved());
    EXPECT_TRUE(log.contains(u"The player could not store a favourite change"_s));
    const QString logged = log.messages().join(u'\n');
    EXPECT_FALSE(logged.contains(m_server.url({}).authority()));
    EXPECT_FALSE(logged.contains("UserFavoriteItems"_L1));

    QTest::qWait(1000);
    EXPECT_EQ(failed.size(), 1);
    EXPECT_FALSE(m_library.trackForId(track.id()).isLoved());
}

TEST_F(JellyfinAccountTest, AccountSwitchReportsAFailedFavouriteRollbackInTheBanner)
{
    auto first              = std::make_shared<QuickConnectScenario>();
    first->pendingPolls     = 0;
    first->favouriteDelayMs = 800;
    m_server.setHandler(quickConnectHandler(first));
    m_account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor(
        [&]() { return m_library.tracks().size() == 1 && !m_account.catalogSync()->isRunning(); }, 5000));
    const Track firstTrack = m_library.tracks().front();
    QWidget window;
    window.show();
    connectFavouriteBanner(m_account.favouriteSync(), &window);
    QSignalSpy failed{m_account.favouriteSync(), &FavouriteSync::failed};
    const LogCapture log;

    editLoved(m_library, firstTrack, true);
    ASSERT_TRUE(QTest::qWaitFor([&]() { return favouriteRequests(m_server).size() == 1; }, 5000));

    auto second          = std::make_shared<QuickConnectScenario>();
    second->pendingPolls = 0;
    second->exchangeResult.insert(u"User"_s, QJsonObject{{u"Id"_s, u"user2"_s}, {u"Name"_s, u"Second User"_s}});
    second->exchangeResult.insert(u"AccessToken"_s, u"synthetic-second-token"_s);
    m_server.setHandler(quickConnectHandler(second));
    m_library.failTrackStats = true;
    m_account.startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(QTest::qWaitFor(
        [&]() { return m_account.connection() && m_account.connection()->userId == u"user2"_s; }, 5000));
    m_library.failTrackStats = false;
    EXPECT_FALSE(m_library.trackForId(firstTrack.id()).isLoved());

    MessageBanner* banner{nullptr};
    ASSERT_TRUE(QTest::qWaitFor(
        [&]() {
            banner = window.findChild<MessageBanner*>();
            return banner && banner->isVisible();
        },
        5000));
    EXPECT_EQ(banner->text(), u"The player could not save the Jellyfin favourite change."_s);
    ASSERT_TRUE(QTest::qWaitFor(
        [&]() { return m_library.tracks().size() == 2 && !m_account.catalogSync()->isRunning(); }, 5000));
    EXPECT_FALSE(m_library.trackForId(firstTrack.id()).isLoved());
    EXPECT_TRUE(log.contains(u"The player could not store a favourite change"_s));
    const QString logged = log.messages().join(u'\n');
    EXPECT_FALSE(logged.contains("synthetic-second-token"_L1));
    EXPECT_FALSE(logged.contains(m_server.url({}).authority()));
    EXPECT_FALSE(logged.contains("UserFavoriteItems"_L1));

    QTest::qWait(1000);
    EXPECT_EQ(failed.size(), 1);
    EXPECT_FALSE(m_library.trackForId(firstTrack.id()).isLoved());
}

TEST_F(JellyfinAccountTest, DestructionRestoresAPendingFavouriteAndClearsTheHandler)
{
    StubMusicLibrary library;
    auto account  = std::make_unique<Account>(&m_network, &library, &m_settings, m_credentialStateStore, m_credentials,
                                              m_access, 20ms, nullptr);
    auto scenario = std::make_shared<QuickConnectScenario>();
    scenario->pendingPolls     = 0;
    scenario->favouriteDelayMs = 800;
    m_server.setHandler(quickConnectHandler(scenario));
    account->startQuickConnect(m_server.url({}).toString());
    ASSERT_TRUE(
        QTest::qWaitFor([&]() { return library.tracks().size() == 1 && !account->catalogSync()->isRunning(); }, 5000));
    const Track track = library.tracks().front();

    editLoved(library, track, true);
    ASSERT_TRUE(QTest::qWaitFor([&]() { return favouriteRequests(m_server).size() == 1; }, 5000));
    account.reset();
    EXPECT_FALSE(library.trackForId(track.id()).isLoved());
    EXPECT_FALSE(library.canEditTrackStats(library.trackForId(track.id()), Track::Stat::Loved));

    QTest::qWait(1000);
    EXPECT_FALSE(library.trackForId(track.id()).isLoved());
}

} // namespace Fooyin::Jellyfin::Testing
