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

bool PlaylistMirror::apply(const QString& sourceKey, const std::vector<MirroredPlaylist>& playlists)
{
    for(const auto& mirrored : playlists) {
        if(std::cmp_not_equal(mirrored.tracks.size(), mirrored.playlistItemIds.size())) {
            return false;
        }
    }
    const QString sourceLibraryPath = libraryUri(sourceKey);
    QVariantMap legacyEntries       = m_settings->fileValue(settingsKey(sourceKey)).toMap();
    QVariantMap pendingLegacyEntries;
    QStringList serverPlaylistIds;
    bool complete{true};

    for(const auto& mirrored : playlists) {
        serverPlaylistIds.push_back(mirrored.playlistId);
        const QVariantMap legacyEntry = legacyEntries.take(mirrored.playlistId).toMap();
        const Playlist* legacy
            = m_playlistHandler->playlistByDbId(legacyEntry.value(u"nativePlaylistDbId"_s, -1).toInt());
        const UId adoptedId
            = legacy && legacy->sourceLibraryPath() == sourceLibraryPath && legacy->sourcePlaylistId().isEmpty()
                ? legacy->id()
                : UId{};

        PlaylistTrackList playlistTracks;
        for(size_t index{0}; index < mirrored.tracks.size(); ++index) {
            const QByteArray identity = sourceKey.toUtf8() + '\n' + mirrored.playlistId.toUtf8() + '\n'
                                      + mirrored.playlistItemIds.at(static_cast<qsizetype>(index)).toUtf8();
            playlistTracks.push_back({.track           = mirrored.tracks.at(index),
                                      .playlistId      = {},
                                      .entryId         = UId{QUuid::createUuidV5(QUuid{}, identity)},
                                      .indexInPlaylist = static_cast<int>(index)});
        }
        if(!m_playlistHandler->applyExternalPlaylist(sourceLibraryPath, mirrored.playlistId, adoptedId, mirrored.name,
                                                     playlistTracks)) {
            complete = false;
            if(adoptedId.isValid()) {
                pendingLegacyEntries.insert(mirrored.playlistId, legacyEntry);
            }
        }
    }

    std::vector<UId> staleIds;
    for(const Playlist* playlist : m_playlistHandler->externalPlaylists(sourceLibraryPath)) {
        if(!playlist->sourcePlaylistId().isEmpty() && !serverPlaylistIds.contains(playlist->sourcePlaylistId())) {
            staleIds.push_back(playlist->id());
        }
    }
    for(const UId& staleId : staleIds) {
        if(!m_playlistHandler->removeExternalPlaylist(sourceLibraryPath, staleId)) {
            complete = false;
        }
    }

    for(const auto& [playlistId, legacyEntry] : std::as_const(legacyEntries).asKeyValueRange()) {
        const Playlist* legacy
            = m_playlistHandler->playlistByDbId(legacyEntry.toMap().value(u"nativePlaylistDbId"_s, -1).toInt());
        if(legacy && legacy->sourceLibraryPath() == sourceLibraryPath && legacy->sourcePlaylistId().isEmpty()
           && !m_playlistHandler->removeExternalPlaylist(sourceLibraryPath, legacy->id())) {
            complete = false;
            pendingLegacyEntries.insert(playlistId, legacyEntry);
        }
    }

    if(pendingLegacyEntries.isEmpty()) {
        m_settings->fileRemove(settingsKey(sourceKey));
    }
    else {
        m_settings->fileSet(settingsKey(sourceKey), pendingLegacyEntries);
    }
    return complete;
}
} // namespace Fooyin::Jellyfin
