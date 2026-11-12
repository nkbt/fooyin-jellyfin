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

#include "jellyfinfavouritesync.h"

#include "jellyfinrequest.h"
#include "jellyfintrack.h"

#include <core/library/musiclibrary.h>

#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QUrlQuery>

Q_LOGGING_CATEGORY(JELLYFIN_FAVOURITES, "fy.jellyfin.favourites")

using namespace Qt::StringLiterals;

namespace Fooyin::Jellyfin {
FavouriteSync::FavouriteSync(QNetworkAccessManager* network, MusicLibrary* library,
                             std::shared_ptr<AccessRegistry> access, QObject* parent)
    : QObject{parent}
    , m_network{network}
    , m_library{library}
    , m_access{std::move(access)}
{
    QObject::connect(m_library, &MusicLibrary::externalTrackStatsChangeRequested, this,
                     &FavouriteSync::handleChangeRequested);
    QObject::connect(m_library, &MusicLibrary::externalTrackStatsFinished, this, &FavouriteSync::handlePublished);
    m_handlerChangedConnection = QObject::connect(m_library, &MusicLibrary::externalLibraryStatsHandlerChanged, this,
                                                  &FavouriteSync::handleHandlerChanged);
}

FavouriteSync::~FavouriteSync()
{
    // The library reports the removed handler while QObject destroys this object.
    QObject::disconnect(m_handlerChangedConnection);
}

void FavouriteSync::activate(const LibraryInfo& library)
{
    release();

    m_sourceAccess = m_access->access();
    if(!m_sourceAccess || library.path != libraryUri(m_sourceAccess->sourceKey)) {
        m_sourceAccess.reset();
        return;
    }
    m_sourceLibrary = library;
    m_library->setExternalLibraryStatsHandler(library, Track::Stat::Loved, this);
}

void FavouriteSync::release()
{
    if(!m_sourceAccess) {
        return;
    }

    const std::map<QString, Edit> edits = takeEdits();
    if(m_sourceLibrary) {
        TrackList restored;
        for(const auto& [itemId, edit] : edits) {
            if(edit.desired != edit.confirmed) {
                Track track{m_library->trackForId(edit.trackId)};
                if(track.isValid()) {
                    track.setLoved(edit.confirmed);
                    restored.push_back(track);
                }
            }
        }
        publish(restored);
        m_library->setExternalLibraryStatsHandler(*m_sourceLibrary, Track::Stat::None, nullptr);
    }
    m_sourceLibrary.reset();
    m_sourceAccess.reset();
}

quint64 FavouriteSync::sequence() const
{
    return m_sequence;
}

void FavouriteSync::reconcileSnapshot(const Access& access, const LibraryInfo& library, TrackList& tracks,
                                      quint64 snapshotSequence)
{
    if(!m_sourceAccess || access.sourceGeneration != m_sourceAccess->sourceGeneration
       || library.path != libraryUri(m_sourceAccess->sourceKey)) {
        return;
    }

    if(!m_sourceLibrary) {
        // Refresh recreated the removed source library.
        m_sourceLibrary = library;
        m_library->setExternalLibraryStatsHandler(library, Track::Stat::Loved, this);
    }

    m_catalogFavourites.clear();
    m_catalogSequence = snapshotSequence;
    for(Track& track : tracks) {
        const QString itemId = itemIdFromPath(track.filepath());
        m_catalogFavourites.insert_or_assign(itemId, track.isLoved());
        const auto it = m_edits.find(itemId);
        if(it == m_edits.end()) {
            continue;
        }
        Edit& edit = it->second;
        // Only a server result after the catalog started is newer; a later choice is not a server value.
        if(edit.confirmedSequence <= snapshotSequence) {
            edit.confirmed         = track.isLoved();
            edit.confirmedSequence = snapshotSequence;
        }
        if(edit.reply || edit.sequence > snapshotSequence) {
            track.setLoved(edit.desired);
        }
        else {
            m_edits.erase(it);
        }
    }
}

void FavouriteSync::handleHandlerChanged()
{
    if(!m_sourceLibrary) {
        return;
    }
    const auto library = m_library->libraryInfo(m_sourceLibrary->id);
    if(library && library->path == m_sourceLibrary->path) {
        return;
    }
    // The removed library took its tracks, so pending edits have nothing to restore.
    takeEdits();
    m_sourceLibrary.reset();
}

std::map<QString, FavouriteSync::Edit> FavouriteSync::takeEdits()
{
    std::map<QString, Edit> edits;
    edits.swap(m_edits);
    m_catalogFavourites.clear();
    ++m_sequence;
    for(auto& [itemId, edit] : edits) {
        if(QNetworkReply* reply = edit.reply) {
            edit.reply = nullptr;
            edit.cancellation.reset();
            reply->abort();
        }
    }
    return edits;
}

void FavouriteSync::handleChangeRequested(const LibraryInfo& library, const TrackList& tracks, Track::Stats stats)
{
    if(!m_sourceLibrary || !m_sourceAccess || library.id != m_sourceLibrary->id || library.path != m_sourceLibrary->path
       || !stats.testFlag(Track::Stat::Loved) || !m_access->isCurrent(m_sourceAccess->sourceGeneration)) {
        return;
    }

    TrackList published;
    for(const Track& track : tracks) {
        const QString itemId = itemIdFromPath(track.filepath());
        if(itemId.isEmpty() || sourceKeyFromPath(track.filepath()) != m_sourceAccess->sourceKey) {
            continue;
        }

        const bool shown    = m_library->trackForId(track.id()).isLoved();
        auto [it, inserted] = m_edits.try_emplace(itemId);
        Edit& edit          = it->second;
        if(inserted) {
            // The library shows an older value until a submitted catalog is stored.
            const auto catalog     = m_catalogFavourites.find(itemId);
            edit.confirmed         = catalog != m_catalogFavourites.end() ? catalog->second : shown;
            edit.confirmedSequence = m_catalogSequence;
            edit.desired           = edit.confirmed;
        }
        edit.trackId = track.id();
        if(track.isLoved() == edit.desired && shown == edit.desired) {
            continue;
        }

        edit.sequence = ++m_sequence;
        published.push_back(track);
        if(track.isLoved() == edit.desired) {
            continue;
        }
        edit.desired = track.isLoved();
        if(!edit.reply) {
            send(itemId, edit);
        }
    }

    publish(published);
}

void FavouriteSync::send(const QString& itemId, Edit& edit)
{
    edit.sent = edit.desired;

    QUrlQuery query;
    query.addQueryItem(u"userId"_s, m_sourceAccess->userId);
    const QNetworkRequest request = apiRequest(
        endpointUrl(m_sourceAccess->serverUrl, u"UserFavoriteItems/"_s + itemId, query), m_sourceAccess->authorization);
    QNetworkReply* reply = edit.sent ? m_network->post(request, QByteArray{}) : m_network->deleteResource(request);
    restrictRedirects(reply, m_sourceAccess->serverUrl);

    edit.reply = reply;
    edit.cancellation.emplace(m_sourceAccess->sourceLifetime, [reply]() {
        QMetaObject::invokeMethod(reply, &QNetworkReply::abort, Qt::QueuedConnection);
    });
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, itemId]() { handleReply(reply, itemId); });
}

void FavouriteSync::handleReply(QNetworkReply* reply, const QString& itemId)
{
    reply->deleteLater();

    const auto it = m_edits.find(itemId);
    if(it == m_edits.end() || it->second.reply != reply || !m_sourceAccess
       || !m_access->isCurrent(m_sourceAccess->sourceGeneration)) {
        return;
    }
    Edit& edit = it->second;
    edit.reply = nullptr;
    edit.cancellation.reset();
    edit.sequence = ++m_sequence;

    QString error;
    std::optional<bool> favourite;
    if(reply->error() != QNetworkReply::NoError) {
        error = replyError(reply);
    }
    else {
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        const QJsonValue value       = document.object().value("IsFavorite"_L1);
        if(document.isObject() && value.isBool()) {
            favourite = value.toBool();
        }
        else {
            error = tr("The server returned an invalid favourite response.");
        }
    }

    if(favourite) {
        edit.confirmed         = *favourite;
        edit.confirmedSequence = edit.sequence;
    }

    if(edit.desired != edit.sent) {
        // A newer choice replaced this request, so its own result decides what the user sees.
        send(itemId, edit);
        return;
    }

    if(favourite) {
        if(edit.desired != *favourite) {
            edit.desired = *favourite;
            publishTrack(edit);
        }
        return;
    }

    qCWarning(JELLYFIN_FAVOURITES) << "Favourite change failed:" << error;
    if(edit.desired != edit.confirmed) {
        edit.desired = edit.confirmed;
        publishTrack(edit);
    }
    Q_EMIT failed(tr("Could not update the Jellyfin favourite: %1").arg(error));
}

void FavouriteSync::publishTrack(const Edit& edit)
{
    Track track{m_library->trackForId(edit.trackId)};
    if(!track.isValid()) {
        return;
    }
    track.setLoved(edit.desired);
    publish({track});
}

void FavouriteSync::publish(const TrackList& tracks)
{
    if(tracks.empty()) {
        return;
    }
    const QUuid operation = QUuid::createUuid();
    // The library can report a rejection before applyExternalTrackStats() returns.
    m_operations.insert(operation);
    m_library->applyExternalTrackStats(*m_sourceLibrary, tracks, Track::Stat::Loved, operation);
}

void FavouriteSync::handlePublished(const QUuid& operation, int /*libraryId*/, bool success)
{
    if(m_operations.erase(operation) == 0 || success) {
        return;
    }
    qCWarning(JELLYFIN_FAVOURITES) << "The player could not store a favourite change";
    Q_EMIT failed(tr("The player could not save the Jellyfin favourite change."));
}
} // namespace Fooyin::Jellyfin
