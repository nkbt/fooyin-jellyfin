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

#pragma once

#include "jellyfinaccess.h"

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;

namespace Fooyin {
class MusicLibrary;
class SettingsManager;
struct LibraryInfo;

namespace Jellyfin {
class CatalogSync;
class CredentialStore;
class CredentialStateStore;
class FavouriteSync;

constexpr auto ServerUrlKey = "Jellyfin/ServerUrl";
constexpr auto DeviceIdKey  = "Jellyfin/DeviceId";

//! Owns the configured Jellyfin account, Quick Connect sign-in, its stored token and catalog synchronisation.
class Account : public QObject
{
    Q_OBJECT

public:
    Account(QNetworkAccessManager* network, MusicLibrary* library, SettingsManager* settings,
            std::shared_ptr<CredentialStateStore> credentialStateStore, std::shared_ptr<CredentialStore> credentials,
            std::shared_ptr<AccessRegistry> access, std::chrono::milliseconds quickConnectPollInterval,
            QObject* parent);
    ~Account() override;

    //! Reconnects the stored account and synchronises it.
    void restore();
    //! Starts a new Quick Connect request on the server, replacing any pending request.
    void startQuickConnect(const QString& serverInput);
    //! Stops the pending Quick Connect request locally. The server has no cancellation endpoint.
    void cancelQuickConnect();
    void signOut();
    void refresh();

    [[nodiscard]] std::optional<Connection> connection() const;
    [[nodiscard]] bool isQuickConnectPending() const;
    [[nodiscard]] QString quickConnectCode() const;
    [[nodiscard]] QString status() const;
    [[nodiscard]] CatalogSync* catalogSync() const;
    [[nodiscard]] FavouriteSync* favouriteSync() const;

Q_SIGNALS:
    void statusChanged(const QString& status);
    void connectionChanged();
    void quickConnectChanged();
    void credentialOperationFinished();

private:
    struct QuickConnectAttempt
    {
        quint64 id;
        QUrl serverUrl;
        QString deviceId;
        QString code;
        QString secret;
    };

    void requestEnabled();
    void handleEnabled(QNetworkReply* reply);
    void requestInitiate();
    void handleInitiate(QNetworkReply* reply);
    void requestConnect();
    void handleConnect(QNetworkReply* reply);
    void requestExchange();
    void handleExchange(QNetworkReply* reply);
    void finishQuickConnect(const QString& status);
    [[nodiscard]] bool acceptReply(QNetworkReply* reply, quint64 attemptId) const;
    QNetworkReply* sendQuickConnect(const QString& path, const QUrlQuery& query, const QByteArray& body, bool post);

    void activate(const Connection& connection, const QString& token);
    void invalidateCredentials();
    void removeCredential(const QString& sourceKey, const QString& credentialRecordId);
    bool retainCredentialCleanup(const QString& sourceKey, const QString& credentialRecordId, const QString& error);
    void clearCredentialCleanup(const QString& credentialRecordId);
    bool loadCredentialState();
    bool commitCredentialState(const QJsonObject& nextState);
    void reconcileSourceVisibility();
    void handleRefreshRequested(const LibraryInfo& library);
    void setStatus(const QString& status);
    [[nodiscard]] QString libraryName() const;
    [[nodiscard]] QString deviceId();

    QNetworkAccessManager* m_network;
    MusicLibrary* m_library;
    SettingsManager* m_settings;
    std::shared_ptr<CredentialStateStore> m_credentialStateStore;
    std::shared_ptr<CredentialStore> m_credentials;
    std::shared_ptr<AccessRegistry> m_access;
    FavouriteSync* m_favouriteSync;
    CatalogSync* m_catalogSync;
    QTimer m_pollTimer;
    quint64 m_attemptCount{0};
    QThreadPool m_credentialThreadPool;
    std::shared_ptr<std::atomic<quint64>> m_credentialAttemptGeneration{std::make_shared<std::atomic<quint64>>(0)};
    std::stop_source m_credentialCancellation;
    std::stop_source m_credentialShutdown;
    bool m_shuttingDown{false};
    QJsonObject m_credentialState;
    QString m_credentialStateError;
    QString m_credentialStateCommitError;
    struct CredentialMutationState;
    std::shared_ptr<CredentialMutationState> m_credentialMutations;
    std::map<QString, QString> m_credentialCleanupErrors;
    std::optional<QuickConnectAttempt> m_attempt;
    QPointer<QNetworkReply> m_reply;
    std::optional<Connection> m_connection;
    QString m_status;
};
} // namespace Jellyfin
} // namespace Fooyin
