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

#include "jellyfinplaylistmirror.h"

#include <core/playlist/playlist.h>
#include <core/playlist/playlisthandler.h>
#include <utils/settings/settingsmanager.h>

using namespace Qt::StringLiterals;

namespace Fooyin::Jellyfin {
namespace {
QString settingsKey(const QString& sourceKey)
{
    return u"Jellyfin/Playlists/"_s + sourceKey;
}
} // namespace

PlaylistMirror::PlaylistMirror(PlaylistHandler* playlistHandler, SettingsManager* settings)
    : m_playlistHandler{playlistHandler}
    , m_settings{settings}
{ }

void PlaylistMirror::apply(const QString& sourceKey, const std::vector<MirroredPlaylist>& playlists)
{
    for(const auto& mirrored : playlists) {
        if(std::cmp_not_equal(mirrored.tracks.size(), mirrored.playlistItemIds.size())) {
            return;
        }
    }
    QVariantMap previous = m_settings->fileValue(settingsKey(sourceKey)).toMap();
    QVariantMap current;

    for(const auto& mirrored : playlists) {
        const QVariantMap stored = previous.take(mirrored.playlistId).toMap();
        Playlist* playlist       = m_playlistHandler->playlistByDbId(stored.value(u"nativePlaylistDbId"_s, -1).toInt());
        if(playlist && playlist->sourceLibraryPath() != libraryUri(sourceKey)) {
            playlist = nullptr;
        }

        PlaylistTrackList playlistTracks;
        for(size_t index{0}; index < mirrored.tracks.size(); ++index) {
            const QByteArray identity = sourceKey.toUtf8() + '\n' + mirrored.playlistId.toUtf8() + '\n'
                                      + mirrored.playlistItemIds.at(static_cast<qsizetype>(index)).toUtf8();
            playlistTracks.push_back({.track           = mirrored.tracks.at(index),
                                      .playlistId      = {},
                                      .entryId         = UId{QUuid::createUuidV5(QUuid{}, identity)},
                                      .indexInPlaylist = static_cast<int>(index)});
        }
        playlist = m_playlistHandler->applyExternalPlaylist(libraryUri(sourceKey), playlist ? playlist->id() : UId{},
                                                            mirrored.name, playlistTracks);
        if(!playlist) {
            continue;
        }

        current.insert(mirrored.playlistId,
                       QVariantMap{{u"nativePlaylistDbId"_s, playlist->dbId()}, {u"name"_s, mirrored.name}});
    }

    for(const auto& stale : std::as_const(previous)) {
        const int nativePlaylistDbId = stale.toMap().value(u"nativePlaylistDbId"_s, -1).toInt();
        if(Playlist* playlist = m_playlistHandler->playlistByDbId(nativePlaylistDbId)) {
            m_playlistHandler->removeExternalPlaylist(libraryUri(sourceKey), playlist->id());
        }
    }

    m_settings->fileSet(settingsKey(sourceKey), current);
}
} // namespace Fooyin::Jellyfin
