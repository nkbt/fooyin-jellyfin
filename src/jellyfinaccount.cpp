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

#include "jellyfincatalogsync.h"
#include "jellyfincredentialstatestore.h"
#include "jellyfincredentialstore.h"
#include "jellyfinrequest.h"
#include "jellyfintrack.h"

#include <core/library/libraryinfo.h>
#include <core/library/musiclibrary.h>
#include <utils/settings/settingsmanager.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QUrlQuery>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <mutex>

using namespace Qt::StringLiterals;

namespace Fooyin::Jellyfin {
namespace {
int httpStatus(QNetworkReply* reply)
{
    return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

bool isQuickConnectCode(const QString& code)
{
    static const QRegularExpression pattern{u"^[0-9]{1,16}$"_s};
    return pattern.match(code).hasMatch();
}

bool isQuickConnectSecret(const QString& secret)
{
    static const QRegularExpression pattern{u"^[0-9A-Fa-f]{16,256}$"_s};
    return pattern.match(secret).hasMatch();
}
} // namespace

struct Account::CredentialMutationState
{
    void setAccepted(const QString& credentialRecordId)
    {
        const std::scoped_lock lock{mutex};
        acceptedCredentialRecordId = credentialRecordId;
    }
    bool mayRemove(const QString& credentialRecordId)
    {
        const std::scoped_lock lock{mutex};
        return credentialRecordId != acceptedCredentialRecordId;
    }
    std::mutex mutex;
    QString acceptedCredentialRecordId;
};

Account::Account(QNetworkAccessManager* network, MusicLibrary* library, SettingsManager* settings,
                 std::shared_ptr<CredentialStateStore> credentialStateStore,
                 std::shared_ptr<CredentialStore> credentials, std::shared_ptr<AccessRegistry> access,
                 std::chrono::milliseconds quickConnectPollInterval, QObject* parent)
    : QObject{parent}
    , m_network{network}
    , m_library{library}
    , m_settings{settings}
    , m_credentialStateStore{std::move(credentialStateStore)}
    , m_credentials{std::move(credentials)}
    , m_access{std::move(access)}
    , m_catalogSync{new CatalogSync(network, library, m_access, this)}
    , m_credentialMutations{std::make_shared<CredentialMutationState>()}
{
    m_credentialThreadPool.setMaxThreadCount(1);
    m_pollTimer.setSingleShot(true);
    m_pollTimer.setInterval(quickConnectPollInterval);
    QObject::connect(&m_pollTimer, &QTimer::timeout, this, &Account::requestConnect);

    QObject::connect(m_catalogSync, &CatalogSync::finished, this,
                     [this](bool /*success*/, const QString& message) { setStatus(message); });
    QObject::connect(m_catalogSync, &CatalogSync::progressChanged, this, [this](int fetched, int total) {
        setStatus(tr("Synchronising: %1 of %2 tracks").arg(fetched).arg(total));
    });
    QObject::connect(m_library, &MusicLibrary::externalLibraryRefreshRequested, this, &Account::handleRefreshRequested);
    QObject::connect(m_library, &MusicLibrary::tracksLoaded, this, &Account::reconcileSourceVisibility);
    if(!loadCredentialState()) {
        return;
    }
    reconcileSourceVisibility();
    const QString credentialRecordId
        = m_credentialState.value(u"acceptedConnection"_s).toObject().value(u"credentialRecordId"_s).toString();
    m_credentialMutations->setAccepted(credentialRecordId);
    const auto credentialRecords = m_credentialState.value(u"credentialRecords"_s).toArray();
    for(const auto& record : credentialRecords) {
        const auto credentialRecord             = record.toObject();
        const QString pendingCredentialRecordId = credentialRecord.value(u"credentialRecordId"_s).toString();
        if(!pendingCredentialRecordId.isEmpty() && pendingCredentialRecordId != credentialRecordId) {
            m_credentialCleanupErrors.insert_or_assign(pendingCredentialRecordId,
                                                       tr("A stored Jellyfin session still needs secure cleanup."));
            removeCredential(credentialRecord.value(u"sourceKey"_s).toString(), pendingCredentialRecordId);
        }
    }
    if(!credentialRecordId.isEmpty()) {
        clearCredentialCleanup(credentialRecordId);
    }
}

Account::~Account()
{
    m_shuttingDown = true;
    m_catalogSync->cancel();
    m_access->clearAccess();
    invalidateCredentials();
    m_credentialShutdown.request_stop();
    m_pollTimer.stop();
    if(m_reply) {
        QNetworkReply* reply = m_reply;
        m_reply              = nullptr;
        reply->abort();
    }
    m_credentialThreadPool.waitForDone();
}

void Account::restore()
{
    if(m_shuttingDown || m_attempt || m_connection) {
        return;
    }
    if(!loadCredentialState()) {
        return;
    }
    reconcileSourceVisibility();
    const auto acceptedConnection = m_credentialState.value(u"acceptedConnection"_s).toObject();
    const Connection stored{
        .serverUrl = serverUrlFromInput(acceptedConnection.value(u"serverUrl"_s).toString()),
        .serverId  = acceptedConnection.value(u"serverId"_s).toString(),
        .userId    = acceptedConnection.value(u"userId"_s).toString(),
        .userName  = acceptedConnection.value(u"userName"_s).toString(),
        .deviceId  = acceptedConnection.value(u"deviceId"_s).toString(),
    };
    const QString credentialRecordId = acceptedConnection.value(u"credentialRecordId"_s).toString();
    if(!stored.serverUrl.isValid() || stored.serverId.isEmpty() || stored.userId.isEmpty() || stored.deviceId.isEmpty()
       || credentialRecordId.isEmpty()) {
        setStatus(tr("Not signed in."));
        return;
    }
    m_credentialMutations->setAccepted(credentialRecordId);
    invalidateCredentials();
    const quint64 attemptGeneration = m_credentialAttemptGeneration->load();
    const auto cancellation         = m_credentialCancellation.get_token();
    const auto shutdown             = m_credentialShutdown.get_token();
    setStatus(tr("Restoring the stored Jellyfin session…"));
    QtConcurrent::run(&m_credentialThreadPool, [credentials = m_credentials, credentialRecordId, cancellation,
                                                shutdown]() {
        std::stop_source operation;
        std::stop_callback pairingCancelled{cancellation, [&]() { operation.request_stop(); }};
        std::stop_callback shutdownCancelled{shutdown, [&]() { operation.request_stop(); }};
        return credentials->readToken(credentialRecordId, operation.get_token());
    }).then(this, [this, stored, attemptGeneration, cancellation](const TokenLookup& lookup) {
        if(m_shuttingDown) {
            return;
        }
        const QPointer<Account> account{this};
        Q_EMIT credentialOperationFinished();
        if(!account || m_shuttingDown || cancellation.stop_requested()
           || attemptGeneration != m_credentialAttemptGeneration->load()) {
            return;
        }
        if(!lookup.error.isEmpty()) {
            setStatus(lookup.error);
            return;
        }
        if(!lookup.token || lookup.token->isEmpty()) {
            setStatus(tr("The stored Jellyfin session is missing. Sign in again."));
            return;
        }
        activate(stored, *lookup.token);
    });
}

void Account::startQuickConnect(const QString& serverInput)
{
    if(m_shuttingDown || !m_credentialStateError.isEmpty()) {
        return;
    }
    const QUrl serverUrl = serverUrlFromInput(serverInput);
    if(!serverUrl.isValid()) {
        setStatus(tr("Enter a valid http or https server address."));
        return;
    }

    cancelQuickConnect();
    m_attempt = QuickConnectAttempt{
        .id        = ++m_attemptCount,
        .serverUrl = serverUrl,
        .deviceId  = deviceId(),
        .code      = {},
        .secret    = {},
    };
    Q_EMIT quickConnectChanged();
    setStatus(tr("Contacting the server…"));
    requestEnabled();
}

void Account::reconcileSourceVisibility()
{
    if(!m_credentialStateError.isEmpty()) {
        return;
    }
    const auto acceptedConnection   = m_credentialState.value(u"acceptedConnection"_s).toObject();
    const QString serverId          = acceptedConnection.value(u"serverId"_s).toString();
    const QString userId            = acceptedConnection.value(u"userId"_s).toString();
    const QString acceptedSourceKey = !acceptedConnection.value(u"credentialRecordId"_s).toString().isEmpty()
                                           && isValidItemId(serverId) && isValidItemId(userId)
                                        ? sourceKey(serverId, userId)
                                        : QString{};
    auto sourceKeys                 = m_credentialState.value(u"sourceKeys"_s).toArray();
    if(!acceptedSourceKey.isEmpty() && !sourceKeys.contains(acceptedSourceKey)) {
        sourceKeys.append(acceptedSourceKey);
        auto nextState = m_credentialState;
        nextState.insert(u"sourceKeys"_s, sourceKeys);
        commitCredentialState(nextState);
    }
    static const QRegularExpression sourceKeyPattern{u"^[0-9a-f]{32}$"_s};
    for(const auto& sourceKey : sourceKeys) {
        if(!sourceKeyPattern.match(sourceKey.toString()).hasMatch()) {
            continue;
        }
        if(const auto library = m_library->libraryForPath(libraryUri(sourceKey.toString()))) {
            m_library->setExternalLibraryActive(*library, sourceKey.toString() == acceptedSourceKey);
        }
    }
}

bool Account::loadCredentialState()
{
    const auto loaded      = m_credentialStateStore->load();
    m_credentialStateError = loaded.error;
    if(!loaded.error.isEmpty()) {
        setStatus(loaded.error);
        return false;
    }
    m_credentialState = loaded.state.value_or(QJsonObject{});
    return true;
}

bool Account::commitCredentialState(const QJsonObject& nextState)
{
    if(!m_credentialStateError.isEmpty()) {
        setStatus(m_credentialStateError);
        return false;
    }
    if(nextState == m_credentialState) {
        return true;
    }
    const QString error          = m_credentialStateStore->commit(nextState);
    m_credentialStateCommitError = error;
    if(!error.isEmpty()) {
        setStatus(error);
        return false;
    }
    m_credentialState = nextState;
    return true;
}

bool Account::retainCredentialCleanup(const QString& sourceKey, const QString& credentialRecordId, const QString& error)
{
    auto nextState  = m_credentialState;
    auto sourceKeys = nextState.value(u"sourceKeys"_s).toArray();
    if(!sourceKey.isEmpty() && !sourceKeys.contains(sourceKey)) {
        sourceKeys.append(sourceKey);
        nextState.insert(u"sourceKeys"_s, sourceKeys);
    }
    auto credentialRecords = nextState.value(u"credentialRecords"_s).toArray();
    const bool retained    = std::ranges::any_of(credentialRecords, [&](const QJsonValue& record) {
        return record.toObject().value(u"credentialRecordId"_s).toString() == credentialRecordId;
    });
    if(!retained) {
        credentialRecords.append(
            QJsonObject{{u"sourceKey"_s, sourceKey}, {u"credentialRecordId"_s, credentialRecordId}});
    }
    nextState.insert(u"credentialRecords"_s, credentialRecords);
    if(!commitCredentialState(nextState)) {
        return false;
    }
    if(!error.isEmpty()) {
        m_credentialCleanupErrors.insert_or_assign(credentialRecordId, error);
    }
    Q_EMIT statusChanged(status());
    return true;
}

void Account::clearCredentialCleanup(const QString& credentialRecordId)
{
    auto nextState         = m_credentialState;
    auto credentialRecords = nextState.value(u"credentialRecords"_s).toArray();
    for(qsizetype index = credentialRecords.size(); index > 0; --index) {
        if(credentialRecords.at(index - 1).toObject().value(u"credentialRecordId"_s).toString() == credentialRecordId) {
            credentialRecords.removeAt(index - 1);
        }
    }
    nextState.insert(u"credentialRecords"_s, credentialRecords);
    if(commitCredentialState(nextState)) {
        m_credentialCleanupErrors.erase(credentialRecordId);
    }
    else {
        m_credentialCleanupErrors.insert_or_assign(credentialRecordId,
                                                   tr("Secure cleanup needs a saved acknowledgement."));
    }
}

void Account::removeCredential(const QString& sourceKey, const QString& credentialRecordId)
{
    if(m_shuttingDown || credentialRecordId.isEmpty() || !m_credentialMutations->mayRemove(credentialRecordId)) {
        return;
    }
    if(!retainCredentialCleanup(sourceKey, credentialRecordId, {})) {
        return;
    }
    const auto shutdown = m_credentialShutdown.get_token();
    QtConcurrent::run(&m_credentialThreadPool, [credentials = m_credentials, mutations = m_credentialMutations,
                                                credentialRecordId, shutdown]() {
        if(!mutations->mayRemove(credentialRecordId)) {
            return QString{};
        }
        const QString error = credentials->removeToken(credentialRecordId, shutdown);
        return shutdown.stop_requested()
                 ? tr("Secure session cleanup was interrupted. It will be retried after restart.")
                 : error;
    }).then(this, [this, sourceKey, credentialRecordId](const QString& error) {
        if(m_shuttingDown) {
            return;
        }
        if(error.isEmpty()) {
            clearCredentialCleanup(credentialRecordId);
        }
        else {
            retainCredentialCleanup(sourceKey, credentialRecordId, error);
        }
        Q_EMIT statusChanged(status());
        Q_EMIT credentialOperationFinished();
    });
}

void Account::invalidateCredentials()
{
    ++*m_credentialAttemptGeneration;
    m_credentialCancellation.request_stop();
    m_credentialCancellation = std::stop_source{};
}

void Account::cancelQuickConnect()
{
    invalidateCredentials();
    m_pollTimer.stop();
    if(m_reply) {
        QNetworkReply* reply = m_reply;
        m_reply              = nullptr;
        reply->abort();
    }
    if(!m_attempt) {
        return;
    }
    m_attempt.reset();
    Q_EMIT quickConnectChanged();
    setStatus(tr("Quick Connect was cancelled. If the code was already approved, the server can keep that device "
                 "session until it is removed in Jellyfin."));
}

void Account::requestEnabled()
{
    const quint64 attemptId = m_attempt->id;
    QNetworkReply* reply    = sendQuickConnect(u"QuickConnect/Enabled"_s, {}, {}, false);
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, attemptId]() {
        reply->deleteLater();
        if(acceptReply(reply, attemptId)) {
            m_reply = nullptr;
            handleEnabled(reply);
        }
    });
}

void Account::handleEnabled(QNetworkReply* reply)
{
    if(reply->error() != QNetworkReply::NoError && httpStatus(reply) != 401) {
        finishQuickConnect(replyError(reply));
        return;
    }

    const QJsonDocument document = QJsonDocument::fromJson("[" + reply->readAll() + "]");
    const QJsonValue enabled     = document.array().at(0);
    if(httpStatus(reply) == 401 || (enabled.isBool() && !enabled.toBool())) {
        finishQuickConnect(tr("Quick Connect is disabled on this server. An administrator can enable it in the "
                              "Jellyfin dashboard."));
        return;
    }
    if(!enabled.isBool()) {
        finishQuickConnect(tr("The server returned an invalid Quick Connect response."));
        return;
    }
    requestInitiate();
}

void Account::requestInitiate()
{
    const quint64 attemptId = m_attempt->id;
    QNetworkReply* reply    = sendQuickConnect(u"QuickConnect/Initiate"_s, {}, {}, true);
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, attemptId]() {
        reply->deleteLater();
        if(acceptReply(reply, attemptId)) {
            m_reply = nullptr;
            handleInitiate(reply);
        }
    });
}

void Account::handleInitiate(QNetworkReply* reply)
{
    if(httpStatus(reply) == 401) {
        finishQuickConnect(tr("Quick Connect is disabled on this server. An administrator can enable it in the "
                              "Jellyfin dashboard."));
        return;
    }
    if(reply->error() != QNetworkReply::NoError) {
        finishQuickConnect(replyError(reply));
        return;
    }

    const QJsonObject result = QJsonDocument::fromJson(reply->readAll()).object();
    const QString code       = result.value("Code"_L1).toString();
    const QString secret     = result.value("Secret"_L1).toString();
    if(!isQuickConnectCode(code) || !isQuickConnectSecret(secret)) {
        finishQuickConnect(tr("The server returned an invalid Quick Connect response."));
        return;
    }

    m_attempt->code   = code;
    m_attempt->secret = secret;
    Q_EMIT quickConnectChanged();
    setStatus(tr("Enter code %1 under Quick Connect in a Jellyfin app where you are already signed in.").arg(code));
    m_pollTimer.start();
}

void Account::requestConnect()
{
    if(!m_attempt || m_attempt->secret.isEmpty()) {
        return;
    }
    const quint64 attemptId = m_attempt->id;
    QUrlQuery query;
    query.addQueryItem(u"secret"_s, m_attempt->secret);
    QNetworkReply* reply = sendQuickConnect(u"QuickConnect/Connect"_s, query, {}, false);
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, attemptId]() {
        reply->deleteLater();
        if(acceptReply(reply, attemptId)) {
            m_reply = nullptr;
            handleConnect(reply);
        }
    });
}

void Account::handleConnect(QNetworkReply* reply)
{
    const int status = httpStatus(reply);
    if(status == 404) {
        finishQuickConnect(tr("The Quick Connect code expired. Start Quick Connect again."));
        return;
    }
    if(status == 401) {
        finishQuickConnect(tr("Quick Connect is disabled on this server. An administrator can enable it in the "
                              "Jellyfin dashboard."));
        return;
    }
    if(reply->error() != QNetworkReply::NoError) {
        finishQuickConnect(replyError(reply));
        return;
    }

    const QJsonObject result    = QJsonDocument::fromJson(reply->readAll()).object();
    const QJsonValue isApproved = result.value("Authenticated"_L1);
    if(!isApproved.isBool() || result.value("Secret"_L1).toString() != m_attempt->secret) {
        finishQuickConnect(tr("The server returned an invalid Quick Connect response."));
        return;
    }
    if(!isApproved.toBool()) {
        m_pollTimer.start();
        return;
    }

    setStatus(tr("Quick Connect approved. Signing in…"));
    requestExchange();
}

void Account::requestExchange()
{
    const quint64 attemptId = m_attempt->id;
    const QJsonObject body{{u"Secret"_s, m_attempt->secret}};
    QNetworkReply* reply = sendQuickConnect(u"Users/AuthenticateWithQuickConnect"_s, {},
                                            QJsonDocument{body}.toJson(QJsonDocument::Compact), true);
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, attemptId]() {
        reply->deleteLater();
        if(acceptReply(reply, attemptId)) {
            m_reply = nullptr;
            handleExchange(reply);
        }
    });
}

void Account::handleExchange(QNetworkReply* reply)
{
    const int status = httpStatus(reply);
    if(status == 404) {
        finishQuickConnect(tr("The Quick Connect request expired before sign-in completed. Start Quick Connect "
                              "again."));
        return;
    }
    if(status == 401) {
        finishQuickConnect(tr("Quick Connect is disabled on this server. An administrator can enable it in the "
                              "Jellyfin dashboard."));
        return;
    }
    if(reply->error() != QNetworkReply::NoError) {
        finishQuickConnect(replyError(reply));
        return;
    }

    const QJsonObject result = QJsonDocument::fromJson(reply->readAll()).object();
    const QJsonObject user   = result.value("User"_L1).toObject();
    const QString token      = result.value("AccessToken"_L1).toString();
    const QString userId     = user.value("Id"_L1).toString();
    const QString serverId   = result.value("ServerId"_L1).toString();
    if(token.isEmpty() || !isValidItemId(userId) || !isValidItemId(serverId)) {
        finishQuickConnect(tr("The server returned an invalid sign-in response."));
        return;
    }

    const Connection connection{
        .serverUrl = m_attempt->serverUrl,
        .serverId  = serverId,
        .userId    = userId,
        .userName  = user.value("Name"_L1).toString(),
        .deviceId  = m_attempt->deviceId,
    };

    const QString sourceKey          = Fooyin::Jellyfin::sourceKey(serverId, userId);
    const QString credentialRecordId = sourceKey + u"/"_s + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const quint64 attemptGeneration  = m_credentialAttemptGeneration->load();
    const quint64 attemptId          = m_attempt->id;
    const auto cancellation          = m_credentialCancellation.get_token();
    const auto shutdown              = m_credentialShutdown.get_token();
    if(!retainCredentialCleanup(sourceKey, credentialRecordId, {})) {
        finishQuickConnect(m_status);
        return;
    }
    setStatus(tr("Saving the Jellyfin session securely…"));
    QtConcurrent::run(&m_credentialThreadPool,
                      [credentials = m_credentials, credentialRecordId, token, cancellation, shutdown]() {
                          std::stop_source operation;
                          std::stop_callback pairingCancelled{cancellation, [&]() { operation.request_stop(); }};
                          std::stop_callback shutdownCancelled{shutdown, [&]() { operation.request_stop(); }};
                          return credentials->writeToken(credentialRecordId, token, operation.get_token());
                      })
        .then(this, [this, connection, token, attemptGeneration, attemptId, cancellation, sourceKey,
                     credentialRecordId](const QString& error) {
            if(m_shuttingDown) {
                return;
            }
            const QPointer<Account> account{this};
            Q_EMIT credentialOperationFinished();
            if(!account || m_shuttingDown) {
                return;
            }
            if(cancellation.stop_requested() || attemptGeneration != m_credentialAttemptGeneration->load() || !m_attempt
               || m_attempt->id != attemptId) {
                removeCredential(sourceKey, credentialRecordId);
                return;
            }
            if(!error.isEmpty()) {
                removeCredential(sourceKey, credentialRecordId);
                finishQuickConnect(error);
                return;
            }
            const auto previousConnection            = m_credentialState.value(u"acceptedConnection"_s).toObject();
            const QString previousCredentialRecordId = previousConnection.value(u"credentialRecordId"_s).toString();
            auto nextState                           = m_credentialState;
            nextState.insert(u"acceptedConnection"_s, QJsonObject{{u"serverUrl"_s, connection.serverUrl.toString()},
                                                                  {u"serverId"_s, connection.serverId},
                                                                  {u"userId"_s, connection.userId},
                                                                  {u"userName"_s, connection.userName},
                                                                  {u"deviceId"_s, connection.deviceId},
                                                                  {u"credentialRecordId"_s, credentialRecordId}});
            if(!previousCredentialRecordId.isEmpty()) {
                auto credentialRecords = nextState.value(u"credentialRecords"_s).toArray();
                if(!std::ranges::any_of(credentialRecords, [&](const QJsonValue& record) {
                       return record.toObject().value(u"credentialRecordId"_s).toString() == previousCredentialRecordId;
                   })) {
                    credentialRecords.append(QJsonObject{
                        {u"sourceKey"_s, Fooyin::Jellyfin::sourceKey(previousConnection.value(u"serverId"_s).toString(),
                                                                     previousConnection.value(u"userId"_s).toString())},
                        {u"credentialRecordId"_s, previousCredentialRecordId}});
                }
                nextState.insert(u"credentialRecords"_s, credentialRecords);
            }
            auto credentialRecords = nextState.value(u"credentialRecords"_s).toArray();
            for(qsizetype index = credentialRecords.size(); index > 0; --index) {
                if(credentialRecords.at(index - 1).toObject().value(u"credentialRecordId"_s).toString()
                   == credentialRecordId) {
                    credentialRecords.removeAt(index - 1);
                }
            }
            nextState.insert(u"credentialRecords"_s, credentialRecords);
            if(!commitCredentialState(nextState)) {
                const QString error = m_status;
                removeCredential(sourceKey, credentialRecordId);
                finishQuickConnect(error);
                return;
            }
            m_credentialMutations->setAccepted(credentialRecordId);
            m_credentialCleanupErrors.erase(credentialRecordId);
            reconcileSourceVisibility();
            m_settings->fileSet(ServerUrlKey, connection.serverUrl.toString());
            m_attempt.reset();
            activate(connection, token);
            Q_EMIT quickConnectChanged();
            if(!previousCredentialRecordId.isEmpty()) {
                removeCredential(Fooyin::Jellyfin::sourceKey(previousConnection.value(u"serverId"_s).toString(),
                                                             previousConnection.value(u"userId"_s).toString()),
                                 previousCredentialRecordId);
            }
        });
}

void Account::finishQuickConnect(const QString& status)
{
    m_pollTimer.stop();
    m_attempt.reset();
    Q_EMIT quickConnectChanged();
    setStatus(status);
}

bool Account::acceptReply(QNetworkReply* reply, quint64 attemptId) const
{
    return !m_shuttingDown && reply == m_reply && m_attempt && m_attempt->id == attemptId;
}

QNetworkReply* Account::sendQuickConnect(const QString& path, const QUrlQuery& query, const QByteArray& body, bool post)
{
    QNetworkRequest request
        = apiRequest(endpointUrl(m_attempt->serverUrl, path, query), authorizationHeader(m_attempt->deviceId, {}));
    QNetworkReply* reply = nullptr;
    if(post) {
        request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
        reply = m_network->post(request, body);
    }
    else {
        reply = m_network->get(request);
    }
    restrictRedirects(reply, m_attempt->serverUrl);
    m_reply = reply;
    return reply;
}

void Account::activate(const Connection& connection, const QString& token)
{
    m_catalogSync->cancel();
    if(m_connection
       && sourceKey(m_connection->serverId, m_connection->userId)
              != sourceKey(connection.serverId, connection.userId)) {
        if(const auto previous
           = m_library->libraryForPath(libraryUri(sourceKey(m_connection->serverId, m_connection->userId)))) {
            m_library->setExternalLibraryActive(*previous, false);
        }
    }
    m_connection = connection;
    m_access->setAccess(connection, token);
    const auto sourceGeneration = m_access->access()->sourceGeneration;
    if(const auto library
       = m_library->addExternalLibrary(libraryUri(sourceKey(connection.serverId, connection.userId)), libraryName())) {
        m_library->setExternalLibraryReadOnlyStats(*library, Track::Stat::Loved);
        m_library->setExternalLibraryActive(*library, true);
        std::unordered_map<QString, CatalogImage> images;
        for(const Track& track : m_library->tracks()) {
            if(track.libraryId() == library->id) {
                const QString imageItemId = track.extraProperties().value(u"Jellyfin/ImageItemId"_s);
                const QString imageTag    = track.extraProperties().value(u"Jellyfin/ImageTag"_s);
                if(!imageItemId.isEmpty() && !imageTag.isEmpty()) {
                    images.emplace(itemIdFromPath(track.filepath()),
                                   CatalogImage{.itemId = imageItemId, .tag = imageTag});
                }
            }
        }
        m_access->setImages(sourceKey(connection.serverId, connection.userId), std::move(images));
    }
    Q_EMIT connectionChanged();
    if(!m_shuttingDown && m_access->isCurrent(sourceGeneration)) {
        m_catalogSync->start(libraryName());
    }
}

void Account::signOut()
{
    if(m_shuttingDown) {
        return;
    }
    const auto acceptedConnection    = m_credentialState.value(u"acceptedConnection"_s).toObject();
    const QString credentialRecordId = acceptedConnection.value(u"credentialRecordId"_s).toString();
    const QString previousKey        = credentialRecordId.isEmpty()
                                         ? QString{}
                                         : sourceKey(acceptedConnection.value(u"serverId"_s).toString(),
                                                     acceptedConnection.value(u"userId"_s).toString());
    auto nextState                   = m_credentialState;
    nextState.remove(u"acceptedConnection"_s);
    if(!credentialRecordId.isEmpty()) {
        auto sourceKeys = nextState.value(u"sourceKeys"_s).toArray();
        if(!sourceKeys.contains(previousKey)) {
            sourceKeys.append(previousKey);
        }
        nextState.insert(u"sourceKeys"_s, sourceKeys);
        auto credentialRecords = nextState.value(u"credentialRecords"_s).toArray();
        if(!std::ranges::any_of(credentialRecords, [&](const QJsonValue& record) {
               return record.toObject().value(u"credentialRecordId"_s).toString() == credentialRecordId;
           })) {
            credentialRecords.append(
                QJsonObject{{u"sourceKey"_s, previousKey}, {u"credentialRecordId"_s, credentialRecordId}});
        }
        nextState.insert(u"credentialRecords"_s, credentialRecords);
    }
    if(!commitCredentialState(nextState)) {
        m_credentialStateCommitError = tr("Sign-out failed. The connection remains active.") + u" "_s + m_status;
        setStatus(m_credentialStateCommitError);
        return;
    }
    m_credentialMutations->setAccepted({});
    cancelQuickConnect();
    m_catalogSync->cancel();
    m_access->clearAccess();
    m_connection.reset();
    reconcileSourceVisibility();
    Q_EMIT connectionChanged();
    setStatus(tr("Signed out. The synchronised catalog is hidden until reconnection."));
    removeCredential(previousKey, credentialRecordId);
}

void Account::refresh()
{
    if(!m_connection || !m_access->access()) {
        setStatus(tr("Sign in to a Jellyfin server first."));
        return;
    }
    m_catalogSync->start(libraryName());
}

void Account::handleRefreshRequested(const LibraryInfo& library)
{
    if(m_connection && library.path == libraryUri(sourceKey(m_connection->serverId, m_connection->userId))) {
        refresh();
        return;
    }
    setStatus(tr("The Jellyfin account for library %1 is not signed in.").arg(library.name));
}

std::optional<Connection> Account::connection() const
{
    return m_connection;
}

bool Account::isQuickConnectPending() const
{
    return m_attempt.has_value();
}

QString Account::quickConnectCode() const
{
    return m_attempt ? m_attempt->code : QString{};
}

QString Account::status() const
{
    QStringList messages{m_status};
    if(!m_credentialStateCommitError.isEmpty() && m_credentialStateCommitError != m_status) {
        messages.push_back(m_credentialStateCommitError);
    }
    for(const auto& [credentialRecordId, error] : m_credentialCleanupErrors) {
        messages.push_back(tr("A stored Jellyfin session still needs secure cleanup. %1").arg(error));
    }
    return messages.join(u"\n"_s);
}

CatalogSync* Account::catalogSync() const
{
    return m_catalogSync;
}

void Account::setStatus(const QString& status)
{
    m_status = status;
    Q_EMIT statusChanged(this->status());
}

QString Account::libraryName() const
{
    return tr("Jellyfin: %1").arg(m_connection ? m_connection->userName : QString{});
}

QString Account::deviceId()
{
    const QString acceptedDeviceId
        = m_credentialState.value(u"acceptedConnection"_s).toObject().value(u"deviceId"_s).toString();
    if(!acceptedDeviceId.isEmpty()) {
        return acceptedDeviceId;
    }
    QString id = m_settings->fileValue(DeviceIdKey).toString();
    if(id.isEmpty()) {
        id = QUuid::createUuid().toString(QUuid::Id128);
        m_settings->fileSet(DeviceIdKey, id);
    }
    return id;
}
} // namespace Fooyin::Jellyfin
