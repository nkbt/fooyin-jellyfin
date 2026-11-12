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

#include "testsupport.h"

#include <core/engine/audioloader.h>
#include <core/playlist/playlist.h>
#include <core/playlist/playlisthandler.h>
#include <utils/database/dbconnectionhandler.h>
#include <utils/database/dbconnectionpool.h>
#include <utils/database/dbconnectionprovider.h>
#include <utils/database/dbquery.h>
#include <utils/settings/settingsmanager.h>

#include <QCoreApplication>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;

namespace Fooyin::Jellyfin::Testing {
namespace {
//! Mirrors the host playlist schema because the host database module is not a public API.
bool createPlaylistTables(const DbConnectionPoolPtr& dbPool)
{
    const DbConnectionProvider dbProvider{dbPool};

    DbQuery createPlaylists{dbProvider.db(), u"CREATE TABLE IF NOT EXISTS Playlists ("
                                             "PlaylistID INTEGER PRIMARY KEY AUTOINCREMENT, "
                                             "Name TEXT NOT NULL UNIQUE, "
                                             "PlaylistIndex INTEGER, "
                                             "IsAutoPlaylist INTEGER DEFAULT 0, "
                                             "Query TEXT, "
                                             "SortQuery TEXT, "
                                             "ForceSorted INTEGER DEFAULT 1, "
                                             "ExtraProperties BLOB);"_s};
    if(!createPlaylists.exec()) {
        return false;
    }

    DbQuery createPlaylistTracks{dbProvider.db(), u"CREATE TABLE IF NOT EXISTS PlaylistTracks ("
                                                  "PlaylistID INTEGER NOT NULL, "
                                                  "TrackID INTEGER NOT NULL, "
                                                  "TrackIndex INTEGER NOT NULL);"_s};
    return createPlaylistTracks.exec();
}

DbConnectionPoolPtr createPool(const QString& filePath)
{
    DbConnection::DbParams params;
    params.type     = u"QSQLITE"_s;
    params.filePath = filePath;
    return DbConnectionPool::create(params, u"jellyfin_playlistmirror_test"_s);
}

Track makeTrack(const QString& itemId, int id)
{
    Track track{trackPath(Fooyin::Jellyfin::sourceKey(u"server1"_s, u"user1"_s), itemId)};
    track.setId(id);
    track.setTitle(itemId);
    track.generateHash();
    return track;
}

QStringList titles(const Playlist* playlist)
{
    QStringList result;
    for(const auto& track : playlist->tracks()) {
        result.append(track.title());
    }
    return result;
}
} // namespace

class JellyfinPlaylistMirrorTest : public testing::Test
{
protected:
    JellyfinPlaylistMirrorTest()
        : m_settings{m_dir.filePath(u"settings.ini"_s)}
        , m_dbPool{createPool(m_dir.filePath(u"library.sqlite"_s))}
        , m_dbConnectionHandler{m_dbPool}
        , m_dbInitialised{m_dbConnectionHandler.hasConnection() && createPlaylistTables(m_dbPool)}
        , m_audioLoader{std::make_shared<AudioLoader>()}
        , m_playlistHandler{m_dbPool, m_audioLoader, &m_library, &m_settings}
        , m_mirror{&m_playlistHandler, &m_settings}
    {
        const auto first = m_library.addExternalLibrary(
            libraryUri(Fooyin::Jellyfin::sourceKey(u"server1"_s, u"user1"_s)), u"First"_s);
        m_library.applyExternalLibrarySnapshot(
            *first, {makeTrack(u"track1"_s, 1), makeTrack(u"track2"_s, 2), makeTrack(u"track3"_s, 3)},
            QUuid::createUuid());
        QCoreApplication::processEvents();
        m_library.addExternalLibrary(libraryUri(Fooyin::Jellyfin::sourceKey(u"server1"_s, u"user2"_s)), u"Second"_s);
    }

    QTemporaryDir m_dir;
    SettingsManager m_settings;
    DbConnectionPoolPtr m_dbPool;
    DbConnectionHandler m_dbConnectionHandler;
    bool m_dbInitialised;
    std::shared_ptr<AudioLoader> m_audioLoader;
    StubMusicLibrary m_library;
    PlaylistHandler m_playlistHandler;
    PlaylistMirror m_mirror;
};

TEST_F(JellyfinPlaylistMirrorTest, CreatesSourceOwnedPlaylistsAndReusesThem)
{
    ASSERT_TRUE(m_dbInitialised);
    const QString sourceKey = Fooyin::Jellyfin::sourceKey(u"server1"_s, u"user1"_s);

    m_mirror.apply(sourceKey,
                   {{.playlistId = u"playlist1"_s,
                     .name       = u"Synthetic Mix"_s,
                     .tracks     = {makeTrack(u"track2"_s, 2), makeTrack(u"track1"_s, 1), makeTrack(u"track2"_s, 2)},
                     .playlistItemIds = {u"entry0"_s, u"entry1"_s, u"entry2"_s}}});

    Playlist* mix = m_playlistHandler.playlistByName(u"Synthetic Mix"_s);
    ASSERT_NE(mix, nullptr);
    EXPECT_TRUE(mix->isLocked());
    EXPECT_EQ(titles(mix), (QStringList{u"track2"_s, u"track1"_s, u"track2"_s}));
    const UId mixId = mix->id();

    m_mirror.apply(sourceKey, {{.playlistId      = u"playlist1"_s,
                                .name            = u"Renamed Mix"_s,
                                .tracks          = {makeTrack(u"track3"_s, 3)},
                                .playlistItemIds = {u"entry0"_s}}});

    Playlist* renamed = m_playlistHandler.playlistById(mixId);
    ASSERT_NE(renamed, nullptr);
    EXPECT_EQ(renamed->name(), u"Renamed Mix"_s);
    EXPECT_TRUE(renamed->isLocked());
    EXPECT_EQ(titles(renamed), QStringList{u"track3"_s});
    EXPECT_EQ(m_playlistHandler.playlistByName(u"Synthetic Mix"_s), nullptr);
}

TEST_F(JellyfinPlaylistMirrorTest, RemovesPlaylistsDeletedOnTheServer)
{
    ASSERT_TRUE(m_dbInitialised);
    const QString sourceKey = Fooyin::Jellyfin::sourceKey(u"server1"_s, u"user1"_s);

    m_mirror.apply(sourceKey,
                   {{.playlistId      = u"playlist1"_s,
                     .name            = u"Synthetic Mix"_s,
                     .tracks          = {makeTrack(u"track1"_s, 1)},
                     .playlistItemIds = {u"entry0"_s}},
                    {.playlistId = u"playlist2"_s, .name = u"Synthetic Other"_s, .tracks = {}, .playlistItemIds = {}}});
    const Playlist* local = m_playlistHandler.createPlaylist(u"Local Playlist"_s);
    ASSERT_NE(local, nullptr);

    m_mirror.apply(sourceKey, {{.playlistId      = u"playlist1"_s,
                                .name            = u"Synthetic Mix"_s,
                                .tracks          = {makeTrack(u"track1"_s, 1)},
                                .playlistItemIds = {u"entry0"_s}}});

    EXPECT_NE(m_playlistHandler.playlistByName(u"Synthetic Mix"_s), nullptr);
    EXPECT_EQ(m_playlistHandler.playlistByName(u"Synthetic Other"_s), nullptr);
    EXPECT_NE(m_playlistHandler.playlistByName(u"Local Playlist"_s), nullptr);
}

TEST_F(JellyfinPlaylistMirrorTest, KeepsAccountsSeparate)
{
    ASSERT_TRUE(m_dbInitialised);
    const QString first  = Fooyin::Jellyfin::sourceKey(u"server1"_s, u"user1"_s);
    const QString second = Fooyin::Jellyfin::sourceKey(u"server1"_s, u"user2"_s);

    m_mirror.apply(first,
                   {{.playlistId = u"playlist1"_s, .name = u"First Mix"_s, .tracks = {}, .playlistItemIds = {}}});
    m_mirror.apply(second,
                   {{.playlistId = u"playlist1"_s, .name = u"Second Mix"_s, .tracks = {}, .playlistItemIds = {}}});
    m_mirror.apply(second, {});

    EXPECT_NE(m_playlistHandler.playlistByName(u"First Mix"_s), nullptr);
    EXPECT_EQ(m_playlistHandler.playlistByName(u"Second Mix"_s), nullptr);
}
TEST_F(JellyfinPlaylistMirrorTest, SourceOwnerCannotBeUnlockedOrEditedThroughCoreOperations)
{
    ASSERT_TRUE(m_dbInitialised);
    const QString sourceKey = Fooyin::Jellyfin::sourceKey(u"server1"_s, u"user1"_s);
    m_mirror.apply(sourceKey, {{.playlistId      = u"playlist1"_s,
                                .name            = u"Synthetic Mix"_s,
                                .tracks          = {makeTrack(u"track1"_s, 1)},
                                .playlistItemIds = {u"entry0"_s}}});
    Playlist* playlist = m_playlistHandler.playlistByName(u"Synthetic Mix"_s);
    ASSERT_NE(playlist, nullptr);
    const UId id = playlist->id();
    m_playlistHandler.setPlaylistLocked(id, false);
    EXPECT_TRUE(playlist->isLocked());
    m_playlistHandler.appendToPlaylist(id, {makeTrack(u"track2"_s, 2)});
    m_playlistHandler.replacePlaylistTracks(id, TrackList{});
    m_playlistHandler.removePlaylistTracks(id, {0});
    m_playlistHandler.clearPlaylistTracks(id);
    m_playlistHandler.removePlaylist(id);
    EXPECT_EQ(m_playlistHandler.playlistById(id), playlist);
    EXPECT_EQ(titles(playlist), QStringList{u"track1"_s});
    m_mirror.apply(sourceKey, {{.playlistId      = u"playlist1"_s,
                                .name            = u"Refreshed Mix"_s,
                                .tracks          = {makeTrack(u"track2"_s, 2)},
                                .playlistItemIds = {u"entry0"_s}}});
    EXPECT_EQ(m_playlistHandler.playlistById(id), playlist);
    EXPECT_EQ(titles(playlist), QStringList{u"track2"_s});
}
TEST_F(JellyfinPlaylistMirrorTest, PreservesDistinctServerOccurrenceIdentityWhenRepeatedEntriesReorder)
{
    ASSERT_TRUE(m_dbInitialised);
    const QString sourceKey = Fooyin::Jellyfin::sourceKey(u"server1"_s, u"user1"_s);
    m_mirror.apply(sourceKey, {{.playlistId      = u"playlist1"_s,
                                .name            = u"Repeated"_s,
                                .tracks          = {makeTrack(u"track1"_s, 1), makeTrack(u"track1"_s, 1)},
                                .playlistItemIds = {u"server-entry-a"_s, u"server-entry-b"_s}}});
    Playlist* playlist = m_playlistHandler.playlistByName(u"Repeated"_s);
    ASSERT_NE(playlist, nullptr);
    const auto before = playlist->playlistTracks();
    ASSERT_EQ(before.size(), 2U);
    EXPECT_NE(before.at(0).entryId, before.at(1).entryId);
    m_mirror.apply(sourceKey, {{.playlistId      = u"playlist1"_s,
                                .name            = u"Repeated"_s,
                                .tracks          = {makeTrack(u"track1"_s, 1), makeTrack(u"track1"_s, 1)},
                                .playlistItemIds = {u"server-entry-b"_s, u"server-entry-a"_s}}});
    const auto after = playlist->playlistTracks();
    ASSERT_EQ(after.size(), 2U);
    EXPECT_EQ(after.at(0).entryId, before.at(1).entryId);
    EXPECT_EQ(after.at(1).entryId, before.at(0).entryId);
}
TEST_F(JellyfinPlaylistMirrorTest, ReusesPersistentSourcePlaylistAfterHandlerRestartWhileHidden)
{
    ASSERT_TRUE(m_dbInitialised);
    const QString sourceKey = Fooyin::Jellyfin::sourceKey(u"server1"_s, u"user1"_s);
    const std::vector<MirroredPlaylist> snapshot{
        {.playlistId      = u"playlist1"_s,
         .name            = u"Retained source playlist"_s,
         .tracks          = {makeTrack(u"track1"_s, 1), makeTrack(u"track2"_s, 2), makeTrack(u"track1"_s, 1)},
         .playlistItemIds = {u"entry1"_s, u"entry2"_s, u"entry3"_s}}};
    m_mirror.apply(sourceKey, snapshot);
    Playlist* original = m_playlistHandler.playlistByName(u"Retained source playlist"_s);
    ASSERT_NE(original, nullptr);
    const int nativePlaylistDbId = original->dbId();
    const auto originalEntries   = original->playlistTracks();
    m_playlistHandler.savePlaylists();
    const auto source = m_library.libraryForPath(libraryUri(sourceKey));
    ASSERT_TRUE(source);
    m_library.setExternalLibraryActive(*source, false);
    PlaylistHandler restored{m_dbPool, m_audioLoader, &m_library, &m_settings};
    PlaylistMirror restoredMirror{&restored, &m_settings};
    m_library.loadAllTracks();
    Playlist* beforeRefresh = restored.playlistByDbId(nativePlaylistDbId);
    ASSERT_NE(beforeRefresh, nullptr);
    const auto loadedEntries = beforeRefresh->playlistTracks();
    ASSERT_EQ(loadedEntries.size(), originalEntries.size());
    for(size_t index{0}; index < originalEntries.size(); ++index) {
        EXPECT_EQ(loadedEntries.at(index).track.id(), originalEntries.at(index).track.id());
        EXPECT_EQ(loadedEntries.at(index).entryId, originalEntries.at(index).entryId);
    }
    restoredMirror.apply(sourceKey, snapshot);
    Playlist* retained = restored.playlistByName(u"Retained source playlist"_s);
    ASSERT_NE(retained, nullptr);
    EXPECT_EQ(retained->dbId(), nativePlaylistDbId);
    EXPECT_TRUE(retained->isLocked());
    EXPECT_FALSE(restored.isPlaylistVisible(retained->id()));
    EXPECT_EQ(retained->trackCount(), originalEntries.size());
    auto reordered                    = snapshot;
    reordered.front().playlistItemIds = {u"entry3"_s, u"entry2"_s, u"entry1"_s};
    restoredMirror.apply(sourceKey, reordered);
    const auto reorderedEntries = retained->playlistTracks();
    ASSERT_EQ(reorderedEntries.size(), 3U);
    EXPECT_EQ(reorderedEntries.at(0).entryId, originalEntries.at(2).entryId);
    EXPECT_EQ(reorderedEntries.at(1).entryId, originalEntries.at(1).entryId);
    EXPECT_EQ(reorderedEntries.at(2).entryId, originalEntries.at(0).entryId);
    restoredMirror.apply(sourceKey, {});
    EXPECT_EQ(restored.playlistByDbId(nativePlaylistDbId), nullptr);
}

TEST_F(JellyfinPlaylistMirrorTest, FailedWritesKeepIdentitiesAndRecoverAfterRestartWithoutDuplicates)
{
    ASSERT_TRUE(m_dbInitialised);
    const QString sourceKey = Fooyin::Jellyfin::sourceKey(u"server1"_s, u"user1"_s);
    ASSERT_TRUE(m_mirror.apply(
        sourceKey, {{.playlistId      = u"playlist1"_s,
                     .name            = u"Kept"_s,
                     .tracks          = {makeTrack(u"track1"_s, 1)},
                     .playlistItemIds = {u"entry1"_s}},
                    {.playlistId = u"playlist2"_s, .name = u"Deleted"_s, .tracks = {}, .playlistItemIds = {}}}));
    const int keptDbId    = m_playlistHandler.playlistByName(u"Kept"_s)->dbId();
    const int deletedDbId = m_playlistHandler.playlistByName(u"Deleted"_s)->dbId();
    const std::vector<MirroredPlaylist> snapshot{{.playlistId = u"playlist1"_s,
                                                  .name       = u"Kept"_s,
                                                  .tracks     = {makeTrack(u"track1"_s, 1), makeTrack(u"track2"_s, 2)},
                                                  .playlistItemIds = {u"entry1"_s, u"entry2"_s}},
                                                 {.playlistId      = u"playlist3"_s,
                                                  .name            = u"Added"_s,
                                                  .tracks          = {makeTrack(u"track3"_s, 3)},
                                                  .playlistItemIds = {u"entry3"_s}}};

    const DbConnectionProvider dbProvider{m_dbPool};
    DbQuery failInsert{dbProvider.db(), u"CREATE TRIGGER FailPlaylistTracks BEFORE INSERT ON PlaylistTracks "
                                        u"BEGIN SELECT RAISE(ABORT, 'synthetic failure'); END;"_s};
    ASSERT_TRUE(failInsert.exec());
    DbQuery failDelete{dbProvider.db(), u"CREATE TRIGGER FailPlaylists BEFORE DELETE ON Playlists "
                                        u"BEGIN SELECT RAISE(ABORT, 'synthetic failure'); END;"_s};
    ASSERT_TRUE(failDelete.exec());
    EXPECT_FALSE(m_mirror.apply(sourceKey, snapshot));
    EXPECT_EQ(m_playlistHandler.playlistByName(u"Added"_s), nullptr);
    EXPECT_NE(m_playlistHandler.playlistByDbId(deletedDbId), nullptr);
    DbQuery dropInsert{dbProvider.db(), u"DROP TRIGGER FailPlaylistTracks;"_s};
    ASSERT_TRUE(dropInsert.exec());
    DbQuery dropDelete{dbProvider.db(), u"DROP TRIGGER FailPlaylists;"_s};
    ASSERT_TRUE(dropDelete.exec());

    PlaylistHandler restored{m_dbPool, m_audioLoader, &m_library, &m_settings};
    PlaylistMirror restoredMirror{&restored, &m_settings};
    m_library.loadAllTracks();
    EXPECT_EQ(restored.playlistByName(u"Added"_s), nullptr);
    ASSERT_NE(restored.playlistByDbId(keptDbId), nullptr);
    EXPECT_EQ(titles(restored.playlistByDbId(keptDbId)), QStringList{u"track1"_s});
    EXPECT_TRUE(restoredMirror.apply(sourceKey, snapshot));

    int sourcePlaylists{0};
    for(const Playlist* playlist : restored.playlists()) {
        if(playlist->sourceLibraryPath() == libraryUri(sourceKey)) {
            ++sourcePlaylists;
        }
    }
    EXPECT_EQ(sourcePlaylists, 2);
    EXPECT_EQ(titles(restored.playlistByDbId(keptDbId)), (QStringList{u"track1"_s, u"track2"_s}));
    ASSERT_NE(restored.playlistByName(u"Added"_s), nullptr);
    EXPECT_EQ(titles(restored.playlistByName(u"Added"_s)), QStringList{u"track3"_s});
    EXPECT_EQ(restored.playlistByDbId(deletedDbId), nullptr);
}

TEST_F(JellyfinPlaylistMirrorTest, CorruptMappingCannotTakeOverLocalOrAnotherSourcePlaylist)
{
    ASSERT_TRUE(m_dbInitialised);
    const QString sourceKey = Fooyin::Jellyfin::sourceKey(u"server1"_s, u"user1"_s);
    Playlist* local         = m_playlistHandler.createNewPlaylist(u"Local"_s, {makeTrack(u"track1"_s, 1)});
    ASSERT_NE(local, nullptr);
    const auto other = m_library.libraryForPath(libraryUri(Fooyin::Jellyfin::sourceKey(u"server1"_s, u"user2"_s)));
    ASSERT_TRUE(other);
    Playlist* foreign
        = m_playlistHandler.applyExternalPlaylist(other->path, u"other-source"_s, {}, u"Other source"_s, {});
    ASSERT_NE(foreign, nullptr);
    m_settings.fileSet(u"Jellyfin/Playlists/"_s + sourceKey,
                       QVariantMap{{u"playlist1"_s, QVariantMap{{u"nativePlaylistDbId"_s, local->dbId()}}},
                                   {u"playlist2"_s, QVariantMap{{u"nativePlaylistDbId"_s, foreign->dbId()}}}});
    m_mirror.apply(sourceKey,
                   {{.playlistId = u"playlist1"_s, .name = u"Remote"_s, .tracks = {}, .playlistItemIds = {}}});
    EXPECT_EQ(local->name(), u"Local"_s);
    EXPECT_EQ(local->trackCount(), 1);
    EXPECT_TRUE(local->sourceLibraryPath().isEmpty());
    EXPECT_EQ(foreign->name(), u"Other source"_s);
    EXPECT_EQ(foreign->sourceLibraryPath(), other->path);
    EXPECT_EQ(m_playlistHandler.playlistByDbId(foreign->dbId()), foreign);
    Playlist* remote = m_playlistHandler.playlistByName(u"Remote"_s);
    ASSERT_NE(remote, nullptr);
    EXPECT_NE(remote->dbId(), local->dbId());
    EXPECT_EQ(remote->sourceLibraryPath(), libraryUri(sourceKey));
}

TEST_F(JellyfinPlaylistMirrorTest, LostSettingsKeepDurableIdentityAndProtectOtherPlaylists)
{
    ASSERT_TRUE(m_dbInitialised);
    const QString sourceKey = Fooyin::Jellyfin::sourceKey(u"server1"_s, u"user1"_s);
    Playlist* local         = m_playlistHandler.createNewPlaylist(u"Local"_s, {makeTrack(u"track1"_s, 1)});
    ASSERT_NE(local, nullptr);
    const auto other = m_library.libraryForPath(libraryUri(Fooyin::Jellyfin::sourceKey(u"server1"_s, u"user2"_s)));
    ASSERT_TRUE(other);
    Playlist* foreign = m_playlistHandler.applyExternalPlaylist(other->path, u"playlist2"_s, {}, u"Other source"_s, {});
    ASSERT_NE(foreign, nullptr);
    const int localDbId   = local->dbId();
    const int foreignDbId = foreign->dbId();
    const std::vector<MirroredPlaylist> snapshot{{.playlistId      = u"playlist1"_s,
                                                  .name            = u"Alpha"_s,
                                                  .tracks          = {makeTrack(u"track1"_s, 1)},
                                                  .playlistItemIds = {u"entry1"_s}},
                                                 {.playlistId      = u"playlist2"_s,
                                                  .name            = u"Beta"_s,
                                                  .tracks          = {makeTrack(u"track2"_s, 2)},
                                                  .playlistItemIds = {u"entry2"_s}}};
    ASSERT_TRUE(m_mirror.apply(sourceKey, snapshot));
    const int alphaDbId = m_playlistHandler.playlistByName(u"Alpha"_s)->dbId();
    const int betaDbId  = m_playlistHandler.playlistByName(u"Beta"_s)->dbId();
    m_playlistHandler.savePlaylists();

    SettingsManager lostSettings{m_dir.filePath(u"lost-settings.ini"_s)};
    PlaylistHandler restored{m_dbPool, m_audioLoader, &m_library, &lostSettings};
    PlaylistMirror restoredMirror{&restored, &lostSettings};
    m_library.loadAllTracks();
    EXPECT_TRUE(restoredMirror.apply(sourceKey, snapshot));
    EXPECT_EQ(restored.externalPlaylists(libraryUri(sourceKey)).size(), 2U);
    ASSERT_NE(restored.playlistByDbId(alphaDbId), nullptr);
    EXPECT_EQ(restored.playlistByDbId(alphaDbId)->sourcePlaylistId(), u"playlist1"_s);
    ASSERT_NE(restored.playlistByDbId(betaDbId), nullptr);
    EXPECT_EQ(restored.playlistByDbId(betaDbId)->sourcePlaylistId(), u"playlist2"_s);

    EXPECT_TRUE(restoredMirror.apply(sourceKey, {snapshot.front()}));
    EXPECT_EQ(restored.playlistByDbId(betaDbId), nullptr);
    ASSERT_NE(restored.playlistByDbId(alphaDbId), nullptr);
    EXPECT_EQ(restored.externalPlaylists(libraryUri(sourceKey)).size(), 1U);
    ASSERT_NE(restored.playlistByDbId(localDbId), nullptr);
    EXPECT_TRUE(restored.playlistByDbId(localDbId)->sourceLibraryPath().isEmpty());
    ASSERT_NE(restored.playlistByDbId(foreignDbId), nullptr);
    EXPECT_EQ(restored.playlistByDbId(foreignDbId)->sourceLibraryPath(), other->path);
    EXPECT_FALSE(lostSettings.fileContains(u"Jellyfin/Playlists/"_s + sourceKey));
}

TEST_F(JellyfinPlaylistMirrorTest, LegacyMappingAdoptsOwnedPlaylistsWithoutSourcePlaylistId)
{
    ASSERT_TRUE(m_dbInitialised);
    const QString sourceKey = Fooyin::Jellyfin::sourceKey(u"server1"_s, u"user1"_s);
    ASSERT_TRUE(m_mirror.apply(
        sourceKey, {{.playlistId      = u"playlist1"_s,
                     .name            = u"Legacy"_s,
                     .tracks          = {makeTrack(u"track1"_s, 1)},
                     .playlistItemIds = {u"entry1"_s}},
                    {.playlistId = u"playlist2"_s, .name = u"Legacy stale"_s, .tracks = {}, .playlistItemIds = {}}}));
    Playlist* legacy      = m_playlistHandler.playlistByName(u"Legacy"_s);
    Playlist* legacyStale = m_playlistHandler.playlistByName(u"Legacy stale"_s);
    ASSERT_NE(legacy, nullptr);
    ASSERT_NE(legacyStale, nullptr);
    const int legacyDbId      = legacy->dbId();
    const int legacyStaleDbId = legacyStale->dbId();

    const DbConnectionProvider dbProvider{m_dbPool};
    legacy->removeExtraProperty(u"core/source-playlist-id"_s);
    DbQuery removeLegacyIdentity{dbProvider.db(),
                                 u"UPDATE Playlists SET ExtraProperties = :extraProperties WHERE PlaylistID = :id;"_s};
    removeLegacyIdentity.bindValue(u":extraProperties"_s, legacy->serialiseExtraProperties());
    removeLegacyIdentity.bindValue(u":id"_s, legacyDbId);
    ASSERT_TRUE(removeLegacyIdentity.exec());
    legacyStale->removeExtraProperty(u"core/source-playlist-id"_s);
    DbQuery removeLegacyStaleIdentity{
        dbProvider.db(), u"UPDATE Playlists SET ExtraProperties = :extraProperties WHERE PlaylistID = :id;"_s};
    removeLegacyStaleIdentity.bindValue(u":extraProperties"_s, legacyStale->serialiseExtraProperties());
    removeLegacyStaleIdentity.bindValue(u":id"_s, legacyStaleDbId);
    ASSERT_TRUE(removeLegacyStaleIdentity.exec());
    m_settings.fileSet(
        u"Jellyfin/Playlists/"_s + sourceKey,
        QVariantMap{
            {u"playlist1"_s, QVariantMap{{u"nativePlaylistDbId"_s, legacyDbId}, {u"name"_s, u"Legacy"_s}}},
            {u"playlist2"_s, QVariantMap{{u"nativePlaylistDbId"_s, legacyStaleDbId}, {u"name"_s, u"Legacy stale"_s}}}});

    PlaylistHandler restored{m_dbPool, m_audioLoader, &m_library, &m_settings};
    PlaylistMirror restoredMirror{&restored, &m_settings};
    m_library.loadAllTracks();
    ASSERT_NE(restored.playlistByDbId(legacyDbId), nullptr);
    ASSERT_TRUE(restored.playlistByDbId(legacyDbId)->sourcePlaylistId().isEmpty());
    const std::vector<MirroredPlaylist> snapshot{{.playlistId = u"playlist1"_s,
                                                  .name       = u"Legacy"_s,
                                                  .tracks     = {makeTrack(u"track1"_s, 1), makeTrack(u"track2"_s, 2)},
                                                  .playlistItemIds = {u"entry1"_s, u"entry2"_s}}};
    EXPECT_TRUE(restoredMirror.apply(sourceKey, snapshot));
    ASSERT_NE(restored.playlistByDbId(legacyDbId), nullptr);
    EXPECT_EQ(restored.playlistByDbId(legacyDbId)->sourcePlaylistId(), u"playlist1"_s);
    EXPECT_EQ(titles(restored.playlistByDbId(legacyDbId)), (QStringList{u"track1"_s, u"track2"_s}));
    EXPECT_EQ(restored.playlistByDbId(legacyStaleDbId), nullptr);
    EXPECT_EQ(restored.externalPlaylists(libraryUri(sourceKey)).size(), 1U);
    EXPECT_FALSE(m_settings.fileContains(u"Jellyfin/Playlists/"_s + sourceKey));

    SettingsManager lostSettings{m_dir.filePath(u"lost-settings.ini"_s)};
    PlaylistHandler reconstructed{m_dbPool, m_audioLoader, &m_library, &lostSettings};
    PlaylistMirror reconstructedMirror{&reconstructed, &lostSettings};
    m_library.loadAllTracks();
    EXPECT_TRUE(reconstructedMirror.apply(sourceKey, snapshot));
    EXPECT_EQ(reconstructed.externalPlaylists(libraryUri(sourceKey)).size(), 1U);
    ASSERT_NE(reconstructed.playlistByDbId(legacyDbId), nullptr);
    EXPECT_EQ(titles(reconstructed.playlistByDbId(legacyDbId)), (QStringList{u"track1"_s, u"track2"_s}));
}

TEST_F(JellyfinPlaylistMirrorTest, RepeatedLegacyAdoptionFailuresKeepMappingForRestartRecovery)
{
    ASSERT_TRUE(m_dbInitialised);
    const QString sourceKey = Fooyin::Jellyfin::sourceKey(u"server1"_s, u"user1"_s);
    ASSERT_TRUE(m_mirror.apply(sourceKey, {{.playlistId      = u"playlist1"_s,
                                            .name            = u"Legacy"_s,
                                            .tracks          = {makeTrack(u"track1"_s, 1)},
                                            .playlistItemIds = {u"entry1"_s}}}));
    Playlist* legacy = m_playlistHandler.playlistByName(u"Legacy"_s);
    ASSERT_NE(legacy, nullptr);
    const int legacyDbId = legacy->dbId();
    const DbConnectionProvider dbProvider{m_dbPool};
    legacy->removeExtraProperty(u"core/source-playlist-id"_s);
    DbQuery removeLegacyIdentity{dbProvider.db(),
                                 u"UPDATE Playlists SET ExtraProperties = :extraProperties WHERE PlaylistID = :id;"_s};
    removeLegacyIdentity.bindValue(u":extraProperties"_s, legacy->serialiseExtraProperties());
    removeLegacyIdentity.bindValue(u":id"_s, legacyDbId);
    ASSERT_TRUE(removeLegacyIdentity.exec());
    m_settings.fileSet(
        u"Jellyfin/Playlists/"_s + sourceKey,
        QVariantMap{{u"playlist1"_s, QVariantMap{{u"nativePlaylistDbId"_s, legacyDbId}, {u"name"_s, u"Legacy"_s}}}});
    const std::vector<MirroredPlaylist> snapshot{{.playlistId = u"playlist1"_s,
                                                  .name       = u"Legacy"_s,
                                                  .tracks     = {makeTrack(u"track1"_s, 1), makeTrack(u"track2"_s, 2)},
                                                  .playlistItemIds = {u"entry1"_s, u"entry2"_s}}};

    PlaylistHandler restored{m_dbPool, m_audioLoader, &m_library, &m_settings};
    PlaylistMirror restoredMirror{&restored, &m_settings};
    m_library.loadAllTracks();
    ASSERT_NE(restored.playlistByDbId(legacyDbId), nullptr);
    DbQuery failInsert{dbProvider.db(), u"CREATE TRIGGER FailPlaylistTracks BEFORE INSERT ON PlaylistTracks "
                                        u"BEGIN SELECT RAISE(ABORT, 'synthetic failure'); END;"_s};
    ASSERT_TRUE(failInsert.exec());
    EXPECT_FALSE(restoredMirror.apply(sourceKey, snapshot));
    EXPECT_TRUE(restored.playlistByDbId(legacyDbId)->sourcePlaylistId().isEmpty());
    EXPECT_TRUE(m_settings.fileContains(u"Jellyfin/Playlists/"_s + sourceKey));
    EXPECT_FALSE(restoredMirror.apply(sourceKey, snapshot));
    EXPECT_TRUE(restored.playlistByDbId(legacyDbId)->sourcePlaylistId().isEmpty());
    EXPECT_TRUE(m_settings.fileContains(u"Jellyfin/Playlists/"_s + sourceKey));
    EXPECT_EQ(restored.externalPlaylists(libraryUri(sourceKey)).size(), 1U);
    DbQuery dropInsert{dbProvider.db(), u"DROP TRIGGER FailPlaylistTracks;"_s};
    ASSERT_TRUE(dropInsert.exec());

    SettingsManager reloadedSettings{m_dir.filePath(u"settings.ini"_s)};
    EXPECT_EQ(reloadedSettings.fileValue(u"Jellyfin/Playlists/"_s + sourceKey)
                  .toMap()
                  .value(u"playlist1"_s)
                  .toMap()
                  .value(u"nativePlaylistDbId"_s)
                  .toInt(),
              legacyDbId);
    PlaylistHandler reconstructed{m_dbPool, m_audioLoader, &m_library, &reloadedSettings};
    PlaylistMirror reconstructedMirror{&reconstructed, &reloadedSettings};
    m_library.loadAllTracks();
    ASSERT_NE(reconstructed.playlistByDbId(legacyDbId), nullptr);
    EXPECT_TRUE(reconstructed.playlistByDbId(legacyDbId)->sourcePlaylistId().isEmpty());
    EXPECT_TRUE(reconstructedMirror.apply(sourceKey, snapshot));
    EXPECT_EQ(reconstructed.externalPlaylists(libraryUri(sourceKey)).size(), 1U);
    ASSERT_NE(reconstructed.playlistByDbId(legacyDbId), nullptr);
    EXPECT_EQ(reconstructed.playlistByDbId(legacyDbId)->sourcePlaylistId(), u"playlist1"_s);
    EXPECT_EQ(titles(reconstructed.playlistByDbId(legacyDbId)), (QStringList{u"track1"_s, u"track2"_s}));
    EXPECT_FALSE(reloadedSettings.fileContains(u"Jellyfin/Playlists/"_s + sourceKey));
}

TEST_F(JellyfinPlaylistMirrorTest, FailedLegacyStaleRemovalKeepsMappingForRestartRecovery)
{
    ASSERT_TRUE(m_dbInitialised);
    const QString sourceKey = Fooyin::Jellyfin::sourceKey(u"server1"_s, u"user1"_s);
    Playlist* local         = m_playlistHandler.createNewPlaylist(u"Local"_s, {makeTrack(u"track1"_s, 1)});
    ASSERT_NE(local, nullptr);
    const auto other = m_library.libraryForPath(libraryUri(Fooyin::Jellyfin::sourceKey(u"server1"_s, u"user2"_s)));
    ASSERT_TRUE(other);
    Playlist* foreign = m_playlistHandler.applyExternalPlaylist(other->path, u"playlist1"_s, {}, u"Other source"_s, {});
    ASSERT_NE(foreign, nullptr);
    const int localDbId   = local->dbId();
    const int foreignDbId = foreign->dbId();
    ASSERT_TRUE(m_mirror.apply(sourceKey, {{.playlistId      = u"playlist1"_s,
                                            .name            = u"Legacy"_s,
                                            .tracks          = {makeTrack(u"track1"_s, 1)},
                                            .playlistItemIds = {u"entry1"_s}}}));
    Playlist* legacy = m_playlistHandler.playlistByName(u"Legacy"_s);
    ASSERT_NE(legacy, nullptr);
    const int legacyDbId = legacy->dbId();
    const DbConnectionProvider dbProvider{m_dbPool};
    legacy->removeExtraProperty(u"core/source-playlist-id"_s);
    DbQuery removeLegacyIdentity{dbProvider.db(),
                                 u"UPDATE Playlists SET ExtraProperties = :extraProperties WHERE PlaylistID = :id;"_s};
    removeLegacyIdentity.bindValue(u":extraProperties"_s, legacy->serialiseExtraProperties());
    removeLegacyIdentity.bindValue(u":id"_s, legacyDbId);
    ASSERT_TRUE(removeLegacyIdentity.exec());
    m_settings.fileSet(
        u"Jellyfin/Playlists/"_s + sourceKey,
        QVariantMap{{u"playlist1"_s, QVariantMap{{u"nativePlaylistDbId"_s, legacyDbId}, {u"name"_s, u"Legacy"_s}}}});

    PlaylistHandler restored{m_dbPool, m_audioLoader, &m_library, &m_settings};
    PlaylistMirror restoredMirror{&restored, &m_settings};
    m_library.loadAllTracks();
    ASSERT_NE(restored.playlistByDbId(legacyDbId), nullptr);
    DbQuery failInsert{dbProvider.db(), u"CREATE TRIGGER FailPlaylistTracks BEFORE INSERT ON PlaylistTracks "
                                        u"BEGIN SELECT RAISE(ABORT, 'synthetic failure'); END;"_s};
    ASSERT_TRUE(failInsert.exec());
    EXPECT_FALSE(restoredMirror.apply(sourceKey, {{.playlistId = u"playlist1"_s,
                                                   .name       = u"Legacy"_s,
                                                   .tracks     = {makeTrack(u"track1"_s, 1), makeTrack(u"track2"_s, 2)},
                                                   .playlistItemIds = {u"entry1"_s, u"entry2"_s}}}));
    DbQuery failDelete{dbProvider.db(), u"CREATE TRIGGER FailPlaylists BEFORE DELETE ON Playlists "
                                        u"BEGIN SELECT RAISE(ABORT, 'synthetic failure'); END;"_s};
    ASSERT_TRUE(failDelete.exec());
    EXPECT_FALSE(restoredMirror.apply(sourceKey, {}));
    ASSERT_NE(restored.playlistByDbId(legacyDbId), nullptr);
    EXPECT_TRUE(restored.playlistByDbId(legacyDbId)->sourcePlaylistId().isEmpty());
    EXPECT_TRUE(m_settings.fileContains(u"Jellyfin/Playlists/"_s + sourceKey));
    DbQuery dropInsert{dbProvider.db(), u"DROP TRIGGER FailPlaylistTracks;"_s};
    ASSERT_TRUE(dropInsert.exec());
    DbQuery dropDelete{dbProvider.db(), u"DROP TRIGGER FailPlaylists;"_s};
    ASSERT_TRUE(dropDelete.exec());

    SettingsManager reloadedSettings{m_dir.filePath(u"settings.ini"_s)};
    PlaylistHandler reconstructed{m_dbPool, m_audioLoader, &m_library, &reloadedSettings};
    PlaylistMirror reconstructedMirror{&reconstructed, &reloadedSettings};
    m_library.loadAllTracks();
    ASSERT_NE(reconstructed.playlistByDbId(legacyDbId), nullptr);
    EXPECT_TRUE(reconstructedMirror.apply(sourceKey, {}));
    EXPECT_EQ(reconstructed.playlistByDbId(legacyDbId), nullptr);
    EXPECT_TRUE(reconstructed.externalPlaylists(libraryUri(sourceKey)).empty());
    ASSERT_NE(reconstructed.playlistByDbId(localDbId), nullptr);
    EXPECT_TRUE(reconstructed.playlistByDbId(localDbId)->sourceLibraryPath().isEmpty());
    ASSERT_NE(reconstructed.playlistByDbId(foreignDbId), nullptr);
    EXPECT_EQ(reconstructed.playlistByDbId(foreignDbId)->sourceLibraryPath(), other->path);
    EXPECT_FALSE(reloadedSettings.fileContains(u"Jellyfin/Playlists/"_s + sourceKey));
}

} // namespace Fooyin::Jellyfin::Testing
