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
#include "jellyfintrack.h"

#include <core/track.h>

#include <QObject>
#include <QPointer>
#include <QUrlQuery>
#include <QUuid>

#include <memory>
#include <vector>

class QNetworkAccessManager;
class QNetworkReply;

namespace Fooyin {
class MusicLibrary;

namespace Jellyfin {
struct MirroredPlaylist
{
    //! Jellyfin playlist ID.
    QString playlistId;
    QString name;
    TrackList tracks;
    QStringList playlistItemIds;
};

//! Fetches a complete catalog snapshot and applies it to the host library.
class CatalogSync : public QObject
{
    Q_OBJECT

public:
    CatalogSync(QNetworkAccessManager* network, MusicLibrary* library, std::shared_ptr<AccessRegistry> access,
                QObject* parent);

    //! Starts a new synchronisation for the current account, replacing a running one.
    void start(const QString& libraryName);
    void cancel();
    [[nodiscard]] bool isRunning() const;

Q_SIGNALS:
    void progressChanged(int fetchedTracks, int totalTracks);
    //! Emitted after a complete snapshot is stored, with read-only playlists in server order.
    void playlistsReady(const QString& sourceKey, const std::vector<Fooyin::Jellyfin::MirroredPlaylist>& playlists);
    void finished(bool success, const QString& message);

private:
    struct RemotePlaylist
    {
        QString playlistId;
        QString name;
        QStringList itemIds;
        QStringList playlistItemIds;
    };

    void requestItemsPage();
    void handleItemsPage(QNetworkReply* reply);
    void requestPlaylists();
    void handlePlaylists(QNetworkReply* reply);
    void requestPlaylistItems();
    void handlePlaylistItems(QNetworkReply* reply);
    void applySnapshot();
    void handleSnapshotApplied(const QUuid& operation, int libraryId, bool success);
    void fail(const QString& message);
    QNetworkReply* get(const QString& path, const QUrlQuery& query);
    [[nodiscard]] bool acceptReply(QNetworkReply* reply, quint64 run);

    QNetworkAccessManager* m_network;
    MusicLibrary* m_library;
    std::shared_ptr<AccessRegistry> m_access;

    quint64 m_run{0};
    bool m_running{false};
    Access m_runAccess;
    QString m_libraryName;
    QPointer<QNetworkReply> m_reply;

    int m_totalTracks{-1};
    std::vector<CatalogItem> m_items;
    std::vector<RemotePlaylist> m_playlists;
    size_t m_playlistIndex{0};
    int m_playlistTotal{-1};
    int m_libraryId{-1};
    bool m_awaitingSnapshot{false};
    QUuid m_operation;
};
} // namespace Jellyfin
} // namespace Fooyin
