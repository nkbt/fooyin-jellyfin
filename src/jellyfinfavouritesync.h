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

#include <core/library/libraryinfo.h>
#include <core/track.h>

#include <QObject>
#include <QPointer>
#include <QUuid>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stop_token>

class QNetworkAccessManager;
class QNetworkReply;

namespace Fooyin {
class MusicLibrary;

namespace Jellyfin {
//! Sends native Love and Unlove edits of the signed-in source to Jellyfin favourites.
class FavouriteSync : public QObject
{
    Q_OBJECT

public:
    FavouriteSync(QNetworkAccessManager* network, MusicLibrary* library, std::shared_ptr<AccessRegistry> access,
                  QObject* parent);
    ~FavouriteSync() override;

    //! Accepts Loved edits of @p library with the current access.
    void activate(const LibraryInfo& library);
    //! Cancels pending edits, restores their confirmed values, reports a failed restore and stops accepting edits.
    void release();

    //! Increases with every edit and server result.
    [[nodiscard]] quint64 sequence() const;
    //! Accepts a recreated @p library and keeps pending edits or edits newer than @p snapshotSequence in @p tracks.
    void reconcileSnapshot(const Access& access, const LibraryInfo& library, TrackList& tracks,
                           quint64 snapshotSequence);

Q_SIGNALS:
    //! A user-facing message without URLs, headers or response bodies.
    void failed(const QString& message);

private:
    struct Edit
    {
        int trackId{-1};
        //! The last value the server reported or the catalog delivered.
        bool confirmed{false};
        bool desired{false};
        bool sent{false};
        quint64 sequence{0};
        QPointer<QNetworkReply> reply;
        std::optional<std::stop_callback<std::function<void()>>> cancellation;
    };

    void handleHandlerChanged();
    //! Cancels and returns pending edits.
    std::map<QString, Edit> takeEdits();
    void handleChangeRequested(const LibraryInfo& library, const TrackList& tracks, Track::Stats stats);
    void send(const QString& itemId, Edit& edit);
    void handleReply(QNetworkReply* reply, const QString& itemId);
    void publishTrack(const Edit& edit);
    void publish(const TrackList& tracks);
    void handlePublished(const QUuid& operation, int libraryId, bool success);

    QNetworkAccessManager* m_network;
    MusicLibrary* m_library;
    std::shared_ptr<AccessRegistry> m_access;
    //! Empty after the source library was removed, until Refresh recreates it.
    std::optional<LibraryInfo> m_sourceLibrary;
    std::optional<Access> m_sourceAccess;
    std::map<QString, Edit> m_edits;
    //! The favourite of every item in the latest catalog, by item ID.
    std::map<QString, bool> m_catalogFavourites;
    std::set<QUuid> m_operations;
    QMetaObject::Connection m_handlerChangedConnection;
    quint64 m_sequence{0};
};
} // namespace Jellyfin
} // namespace Fooyin
