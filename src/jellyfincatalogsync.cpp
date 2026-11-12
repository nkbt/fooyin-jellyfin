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

#include "jellyfincatalogsync.h"

#include "jellyfinfavouritesync.h"
#include "jellyfinrequest.h"

#include <core/library/musiclibrary.h>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>

#include <cmath>
#include <limits>
#include <set>
#include <unordered_map>

Q_LOGGING_CATEGORY(JELLYFIN_SYNC, "fy.jellyfin.sync")

using namespace Qt::StringLiterals;

namespace Fooyin::Jellyfin {
namespace {
constexpr int PageSize = 500;

bool validTotal(const QJsonObject& object)
{
    const QJsonValue value = object.value("TotalRecordCount"_L1);
    const double total     = value.toDouble(-1);
    return value.isDouble() && total >= 0 && total <= std::numeric_limits<int>::max() && std::floor(total) == total;
}

std::optional<QJsonObject> replyObject(QNetworkReply* reply)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(reply->readAll(), &error);
    if(error.error != QJsonParseError::NoError || !document.isObject()) {
        return {};
    }
    return document.object();
}
} // namespace

CatalogSync::CatalogSync(QNetworkAccessManager* network, MusicLibrary* library, std::shared_ptr<AccessRegistry> access,
                         FavouriteSync* favourites, QObject* parent)
    : QObject{parent}
    , m_network{network}
    , m_library{library}
    , m_access{std::move(access)}
    , m_favourites{favourites}
{
    QObject::connect(m_library, &MusicLibrary::externalLibrarySnapshotFinished, this,
                     &CatalogSync::handleSnapshotApplied);
}

void CatalogSync::start(const QString& libraryName)
{
    cancel();

    const auto access = m_access->access();
    if(!access) {
        Q_EMIT finished(false, tr("Sign in to a Jellyfin server first."));
        return;
    }

    m_running           = true;
    m_runAccess         = *access;
    m_favouriteSequence = m_favourites->sequence();
    m_libraryName       = libraryName;
    m_totalTracks       = -1;
    m_items.clear();
    m_playlists.clear();
    m_playlistIndex = 0;
    m_libraryId     = -1;
    requestItemsPage();
}

void CatalogSync::cancel()
{
    ++m_run;
    m_running          = false;
    m_awaitingSnapshot = false;
    if(m_reply) {
        QNetworkReply* reply = m_reply;
        m_reply              = nullptr;
        reply->abort();
    }
}

bool CatalogSync::isRunning() const
{
    return m_running;
}

QNetworkReply* CatalogSync::get(const QString& path, const QUrlQuery& query)
{
    const QUrl url = endpointUrl(m_runAccess.serverUrl, path, query);
    auto* reply    = m_network->get(apiRequest(url, m_runAccess.authorization));
    restrictRedirects(reply, m_runAccess.serverUrl);
    m_reply = reply;
    return reply;
}

bool CatalogSync::acceptReply(QNetworkReply* reply, quint64 run)
{
    reply->deleteLater();
    if(run != m_run || reply != m_reply) {
        return false;
    }
    m_reply = nullptr;
    if(!m_access->isCurrent(m_runAccess.sourceGeneration)) {
        fail(tr("The Jellyfin account changed during synchronisation."));
        return false;
    }
    if(reply->error() != QNetworkReply::NoError) {
        fail(replyError(reply));
        return false;
    }
    return true;
}

void CatalogSync::requestItemsPage()
{
    QUrlQuery query;
    query.addQueryItem(u"userId"_s, m_runAccess.userId);
    query.addQueryItem(u"recursive"_s, u"true"_s);
    query.addQueryItem(u"includeItemTypes"_s, u"Audio"_s);
    query.addQueryItem(u"sortBy"_s, u"SortName"_s);
    query.addQueryItem(u"sortOrder"_s, u"Ascending"_s);
    query.addQueryItem(u"fields"_s, u"Genres,DateCreated"_s);
    query.addQueryItem(u"startIndex"_s, QString::number(m_items.size()));
    query.addQueryItem(u"limit"_s, QString::number(PageSize));
    query.addQueryItem(u"enableTotalRecordCount"_s, u"true"_s);
    query.addQueryItem(u"enableUserData"_s, u"true"_s);

    auto* reply = get(u"Items"_s, query);
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, run = m_run]() {
        if(acceptReply(reply, run)) {
            handleItemsPage(reply);
        }
    });
}

void CatalogSync::handleItemsPage(QNetworkReply* reply)
{
    const auto object = replyObject(reply);
    if(!object || !object->value("Items"_L1).isArray() || !validTotal(*object)) {
        fail(tr("The server returned an invalid catalog page."));
        return;
    }

    const int total = object->value("TotalRecordCount"_L1).toInt();
    if(total < 0 || (m_totalTracks >= 0 && total != m_totalTracks)) {
        fail(tr("The server catalog changed during synchronisation. Refresh again."));
        return;
    }
    m_totalTracks = total;

    const QJsonArray items = object->value("Items"_L1).toArray();
    if(items.isEmpty() && std::cmp_less(m_items.size(), total)) {
        fail(tr("The server returned an incomplete catalog."));
        return;
    }
    for(const auto& value : items) {
        if(!value.toObject().value("UserData"_L1).toObject().value("IsFavorite"_L1).isBool()) {
            fail(tr("The server returned an audio item without valid favorite state."));
            return;
        }
        auto item = catalogItem(value.toObject(), m_runAccess.sourceKey);
        if(!item) {
            fail(tr("The server returned an invalid audio item."));
            return;
        }
        m_items.push_back(std::move(*item));
    }

    Q_EMIT progressChanged(static_cast<int>(m_items.size()), total);

    if(std::cmp_less(m_items.size(), total)) {
        requestItemsPage();
        return;
    }
    if(std::cmp_greater(m_items.size(), total)) {
        fail(tr("The server catalog changed during synchronisation. Refresh again."));
        return;
    }
    requestPlaylists();
}

void CatalogSync::requestPlaylists()
{
    QUrlQuery query;
    query.addQueryItem(u"userId"_s, m_runAccess.userId);
    query.addQueryItem(u"recursive"_s, u"true"_s);
    query.addQueryItem(u"includeItemTypes"_s, u"Playlist"_s);
    query.addQueryItem(u"sortBy"_s, u"SortName"_s);
    query.addQueryItem(u"sortOrder"_s, u"Ascending"_s);
    query.addQueryItem(u"enableTotalRecordCount"_s, u"true"_s);

    auto* reply = get(u"Items"_s, query);
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, run = m_run]() {
        if(acceptReply(reply, run)) {
            handlePlaylists(reply);
        }
    });
}

void CatalogSync::handlePlaylists(QNetworkReply* reply)
{
    const auto object = replyObject(reply);
    if(!object || !object->value("Items"_L1).isArray() || !validTotal(*object)) {
        fail(tr("The server returned an invalid playlist list."));
        return;
    }

    const QJsonArray items = object->value("Items"_L1).toArray();
    if(object->value("TotalRecordCount"_L1).toInt() != items.size()) {
        fail(tr("The server returned an incomplete playlist list."));
        return;
    }
    std::set<QString> playlistIds;
    for(const auto& value : items) {
        const QJsonObject playlist = value.toObject();
        const QString playlistId   = playlist.value("Id"_L1).toString();
        if(!isValidItemId(playlistId) || !playlistIds.emplace(playlistId).second) {
            fail(tr("The server returned an invalid playlist."));
            return;
        }
        const QString mediaType = playlist.value("MediaType"_L1).toString();
        if(!mediaType.isEmpty() && mediaType != "Audio"_L1) {
            continue;
        }
        m_playlists.push_back({.playlistId      = playlistId,
                               .name            = playlist.value("Name"_L1).toString(),
                               .itemIds         = {},
                               .playlistItemIds = {}});
    }

    m_playlistIndex = 0;
    requestPlaylistItems();
}

void CatalogSync::requestPlaylistItems()
{
    if(m_playlistIndex >= m_playlists.size()) {
        applySnapshot();
        return;
    }

    auto& playlist = m_playlists.at(m_playlistIndex);
    if(playlist.itemIds.isEmpty()) {
        m_playlistTotal = -1;
    }

    QUrlQuery query;
    query.addQueryItem(u"userId"_s, m_runAccess.userId);
    query.addQueryItem(u"startIndex"_s, QString::number(playlist.itemIds.size()));
    query.addQueryItem(u"limit"_s, QString::number(PageSize));
    query.addQueryItem(u"enableTotalRecordCount"_s, u"true"_s);

    auto* reply = get(u"Playlists/%1/Items"_s.arg(playlist.playlistId), query);
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, run = m_run]() {
        if(acceptReply(reply, run)) {
            handlePlaylistItems(reply);
        }
    });
}

void CatalogSync::handlePlaylistItems(QNetworkReply* reply)
{
    const auto object = replyObject(reply);
    if(!object || !object->value("Items"_L1).isArray() || !validTotal(*object)) {
        fail(tr("The server returned an invalid playlist page."));
        return;
    }

    auto& playlist  = m_playlists.at(m_playlistIndex);
    const int total = object->value("TotalRecordCount"_L1).toInt();
    if(total < 0 || (m_playlistTotal >= 0 && total != m_playlistTotal)) {
        fail(tr("A server playlist changed during synchronisation. Refresh again."));
        return;
    }
    m_playlistTotal = total;

    const QJsonArray items = object->value("Items"_L1).toArray();
    if(items.isEmpty() && playlist.itemIds.size() < total) {
        fail(tr("The server returned an incomplete playlist."));
        return;
    }
    for(const auto& value : items) {
        const QJsonObject entry      = value.toObject();
        const QString itemId         = entry.value("Id"_L1).toString();
        const QString playlistItemId = entry.value("PlaylistItemId"_L1).toString();
        if(!isValidItemId(itemId) || playlistItemId.isEmpty() || playlist.playlistItemIds.contains(playlistItemId)) {
            fail(tr("The server returned an invalid playlist entry."));
            return;
        }
        playlist.playlistItemIds.append(playlistItemId);
        playlist.itemIds.append(itemId);
    }

    if(playlist.itemIds.size() < total) {
        requestPlaylistItems();
        return;
    }
    if(playlist.itemIds.size() > total) {
        fail(tr("A server playlist changed during synchronisation. Refresh again."));
        return;
    }

    ++m_playlistIndex;
    requestPlaylistItems();
}

void CatalogSync::applySnapshot()
{
    const auto library = m_library->addExternalLibrary(libraryUri(m_runAccess.sourceKey), m_libraryName);
    if(!library) {
        fail(tr("The player could not register the Jellyfin library."));
        return;
    }

    TrackList tracks;
    tracks.reserve(m_items.size());
    std::set<QString> itemIds;
    for(const auto& item : m_items) {
        if(!itemIds.emplace(item.itemId).second) {
            fail(tr("The server catalog changed during synchronisation. Refresh again."));
            return;
        }
        tracks.push_back(item.track);
    }

    for(const auto& playlist : m_playlists) {
        for(const QString& itemId : playlist.itemIds) {
            if(!itemIds.contains(itemId)) {
                fail(tr(
                    "The server returned a playlist entry missing from the catalog. The previous snapshot was kept."));
                return;
            }
        }
    }

    m_favourites->reconcileSnapshot(m_runAccess, tracks, m_favouriteSequence);
    m_library->setExternalLibraryReadOnlyStats(*library, Track::Stat::Loved);
    m_libraryId        = library->id;
    m_awaitingSnapshot = true;
    m_operation        = QUuid::createUuid();
    m_library->applyExternalLibrarySnapshot(*library, tracks, m_operation);
}

void CatalogSync::handleSnapshotApplied(const QUuid& operation, int libraryId, bool success)
{
    if(!m_awaitingSnapshot || operation != m_operation || libraryId != m_libraryId
       || !m_access->isCurrent(m_runAccess.sourceGeneration)) {
        return;
    }
    m_awaitingSnapshot = false;

    if(!success) {
        fail(tr("The player could not store the Jellyfin catalog. The previous catalog was kept."));
        return;
    }

    std::unordered_map<QString, Track> libraryTracks;
    const TrackList allTracks = m_library->tracks();
    for(const auto& track : allTracks) {
        if(track.libraryId() == m_libraryId) {
            libraryTracks.emplace(itemIdFromPath(track.filepath()), track);
        }
    }

    std::unordered_map<QString, CatalogImage> images;
    for(const auto& item : m_items) {
        if(item.image) {
            images.emplace(item.itemId, *item.image);
        }
    }
    m_access->setImages(m_runAccess.sourceKey, std::move(images));

    std::vector<MirroredPlaylist> playlists;
    for(const auto& playlist : m_playlists) {
        MirroredPlaylist mirrored{.playlistId      = playlist.playlistId,
                                  .name            = playlist.name,
                                  .tracks          = {},
                                  .playlistItemIds = playlist.playlistItemIds};
        for(const auto& itemId : playlist.itemIds) {
            if(const auto it = libraryTracks.find(itemId); it != libraryTracks.end()) {
                mirrored.tracks.push_back(it->second);
            }
        }
        playlists.push_back(std::move(mirrored));
    }

    const int count = static_cast<int>(m_items.size());
    m_running       = false;
    m_items.clear();
    m_playlists.clear();

    const quint64 run = m_run;
    Q_EMIT playlistsReady(m_runAccess.sourceKey, playlists);
    if(run != m_run) {
        return;
    }
    Q_EMIT finished(true, count == 1 ? tr("Synchronised 1 track.") : tr("Synchronised %1 tracks.").arg(count));
}

void CatalogSync::fail(const QString& message)
{
    qCInfo(JELLYFIN_SYNC) << "Synchronisation failed:" << message;
    ++m_run;
    m_running          = false;
    m_awaitingSnapshot = false;
    m_items.clear();
    m_playlists.clear();
    Q_EMIT finished(false, message);
}
} // namespace Fooyin::Jellyfin
