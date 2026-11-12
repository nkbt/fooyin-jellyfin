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

#include "fakejellyfinserver.h"
#include "jellyfinaccess.h"
#include "jellyfintrack.h"
#include "testsupport.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QTest>

#include <gtest/gtest.h>

#include <atomic>

using namespace Qt::StringLiterals;

namespace Fooyin::Jellyfin::Testing {
namespace {
const QString Token = u"synthetic-access-token"_s;
const QByteArray TokenField{"Token=\"synthetic-access-token\""};

QJsonObject audioItem(int index, bool isFavorite)
{
    return {
        {u"Id"_s, u"track%1"_s.arg(index)},
        {u"Type"_s, u"Audio"_s},
        {u"Name"_s, u"Synthetic Track %1"_s.arg(index)},
        {u"Album"_s, u"Synthetic Album"_s},
        {u"AlbumId"_s, u"album1"_s},
        {u"AlbumPrimaryImageTag"_s, u"tag1"_s},
        {u"RunTimeTicks"_s, 1800000000},
        {u"UserData"_s, QJsonObject{{u"IsFavorite"_s, isFavorite}}},
    };
}

struct CatalogScenario
{
    std::atomic<int> totalTracks{3};
    std::atomic<int> changedTotalAfterFirstPage{-1};
    std::atomic<int> failItemsStatus{0};
    std::atomic<int> itemsDelayMs{0};
    QJsonArray playlists{
        QJsonObject{{u"Id"_s, u"playlist1"_s}, {u"Name"_s, u"Synthetic Mix"_s}, {u"MediaType"_s, u"Audio"_s}},
        QJsonObject{{u"Id"_s, u"video1"_s}, {u"Name"_s, u"Synthetic Videos"_s}, {u"MediaType"_s, u"Video"_s}}};
    QStringList playlistItems{u"track2"_s, u"track0"_s, u"track2"_s};
};

FakeResponse itemsPage(const CatalogScenario& scenario, const FakeRequest& request)
{
    const int start = request.query.queryItemValue(u"startIndex"_s).toInt();
    const int limit = request.query.queryItemValue(u"limit"_s).toInt();
    int total       = scenario.totalTracks;
    if(scenario.changedTotalAfterFirstPage >= 0 && start > 0) {
        total = scenario.changedTotalAfterFirstPage;
    }
    QJsonArray items;
    for(int index = start; index < std::min(total, start + limit); ++index) {
        items.append(audioItem(index, index % 2 == 0));
    }
    FakeResponse response = FakeJellyfinServer::json(QJsonObject{{u"Items"_s, items}, {u"TotalRecordCount"_s, total}});
    response.delayMs      = scenario.itemsDelayMs;
    return response;
}

FakeJellyfinServer::Handler catalogHandler(std::shared_ptr<CatalogScenario> scenario)
{
    return [scenario](const FakeRequest& request) -> FakeResponse {
        if(request.method != "GET") {
            return FakeJellyfinServer::text(405, "Method not allowed");
        }
        if(request.path.endsWith("/Playlists/playlist1/Items"_L1)) {
            const int start = request.query.queryItemValue(u"startIndex"_s).toInt();
            QJsonArray items;
            for(int index = start; index < std::min<int>(scenario->playlistItems.size(), start + 2); ++index) {
                items.append(QJsonObject{{u"Id"_s, scenario->playlistItems.at(index)},
                                         {u"PlaylistItemId"_s, u"entry%1"_s.arg(index)},
                                         {u"Type"_s, u"Audio"_s}});
            }
            return FakeJellyfinServer::json(
                QJsonObject{{u"Items"_s, items}, {u"TotalRecordCount"_s, scenario->playlistItems.size()}});
        }
        if(!request.path.endsWith("/Items"_L1)) {
            return FakeJellyfinServer::text(404, "Not found");
        }
        if(request.query.queryItemValue(u"includeItemTypes"_s) == "Playlist"_L1) {
            return FakeJellyfinServer::json(
                QJsonObject{{u"Items"_s, scenario->playlists}, {u"TotalRecordCount"_s, scenario->playlists.size()}});
        }
        if(scenario->failItemsStatus != 0) {
            return FakeJellyfinServer::text(scenario->failItemsStatus, "Synthetic failure");
        }
        return itemsPage(*scenario, request);
    };
}

Connection connectionFor(const QUrl& serverUrl, const QString& userId)
{
    return {.serverUrl = serverUrl,
            .serverId  = u"server1"_s,
            .userId    = userId,
            .userName  = u"Synthetic User"_s,
            .deviceId  = u"device1"_s};
}

QStringList titles(const TrackList& tracks)
{
    QStringList result;
    for(const auto& track : tracks) {
        result.append(track.title());
    }
    return result;
}
} // namespace

class JellyfinCatalogSyncTest : public testing::Test
{
protected:
    JellyfinCatalogSyncTest()
        : m_access{std::make_shared<AccessRegistry>()}
        , m_sync{&m_network, &m_library, m_access, nullptr}
        , m_scenario{std::make_shared<CatalogScenario>()}
    {
        m_server.setHandler(catalogHandler(m_scenario));
        m_access->setAccess(connectionFor(m_server.url(u"/jf"_s), u"user1"_s), Token);
    }

    bool runSync(QList<QVariantList>* results)
    {
        QSignalSpy finished{&m_sync, &CatalogSync::finished};
        m_sync.start(u"Jellyfin: Synthetic User"_s);
        if(finished.isEmpty() && !finished.wait(10000)) {
            return false;
        }
        if(results) {
            *results = finished;
        }
        return finished.first().at(0).toBool();
    }

    [[nodiscard]] QString sourceKey() const
    {
        return m_access->access()->sourceKey;
    }

    StubMusicLibrary m_library;
    std::shared_ptr<AccessRegistry> m_access;
    QNetworkAccessManager m_network;
    FakeJellyfinServer m_server;
    CatalogSync m_sync;
    std::shared_ptr<CatalogScenario> m_scenario;
};

TEST_F(JellyfinCatalogSyncTest, FetchesAllPagesWithMetadataOnly)
{
    m_scenario->totalTracks = 1001;
    QSignalSpy progress{&m_sync, &CatalogSync::progressChanged};

    ASSERT_TRUE(runSync(nullptr));

    QStringList startIndexes;
    for(const auto& request : m_server.requests()) {
        EXPECT_EQ(request.method, "GET");
        EXPECT_FALSE(request.path.contains("/Audio/"_L1));
        EXPECT_FALSE(request.path.contains("/Images/"_L1));
        EXPECT_FALSE(request.path.contains("/Download"_L1));
        EXPECT_TRUE(request.path.startsWith("/jf/"_L1));
        EXPECT_TRUE(request.header("authorization").contains(TokenField));
        if(request.query.queryItemValue(u"includeItemTypes"_s) == "Audio"_L1) {
            startIndexes.append(request.query.queryItemValue(u"startIndex"_s));
            EXPECT_EQ(request.query.queryItemValue(u"limit"_s), u"500"_s);
            EXPECT_EQ(request.query.queryItemValue(u"userId"_s), u"user1"_s);
            EXPECT_EQ(request.query.queryItemValue(u"recursive"_s), u"true"_s);
        }
    }
    EXPECT_EQ(startIndexes, (QStringList{u"0"_s, u"500"_s, u"1000"_s}));
    ASSERT_EQ(progress.size(), 3);
    EXPECT_EQ(progress.last().at(0).toInt(), 1001);

    const auto libraries = m_library.libraries();
    ASSERT_EQ(libraries.size(), 1U);
    const LibraryInfo library = libraries.begin()->second;
    EXPECT_EQ(library.path, libraryUri(sourceKey()));
    EXPECT_EQ(library.name, u"Jellyfin: Synthetic User"_s);
    EXPECT_FALSE(library.path.contains(Token));

    const TrackList tracks = m_library.tracksForLibrary(library.id);
    ASSERT_EQ(tracks.size(), 1001U);
    for(const auto& track : tracks) {
        EXPECT_TRUE(track.filepath().startsWith("jellyfin://"_L1));
        EXPECT_FALSE(track.filepath().contains(Token));
        EXPECT_FALSE(track.filepath().contains("127.0.0.1"_L1));
        EXPECT_EQ(sourceKeyFromPath(track.filepath()), sourceKey());
        EXPECT_EQ(track.duration(), 180000U);
    }
}

TEST_F(JellyfinCatalogSyncTest, ImportsLovedAndReadOnlyPlaylistsWithImages)
{
    QSignalSpy playlistsReady{&m_sync, &CatalogSync::playlistsReady};
    ASSERT_TRUE(runSync(nullptr));
    ASSERT_EQ(playlistsReady.size(), 1);
    EXPECT_EQ(playlistsReady.first().at(0).toString(), sourceKey());

    const auto playlists = playlistsReady.first().at(1).value<std::vector<MirroredPlaylist>>();
    ASSERT_EQ(playlists.size(), 1U);
    EXPECT_EQ(playlists.at(0).playlistId, u"playlist1"_s);
    EXPECT_EQ(playlists.at(0).name, u"Synthetic Mix"_s);
    EXPECT_EQ(playlists.at(0).playlistItemIds, (QStringList{u"entry0"_s, u"entry1"_s, u"entry2"_s}));
    EXPECT_EQ(titles(playlists.at(0).tracks),
              (QStringList{u"Synthetic Track 2"_s, u"Synthetic Track 0"_s, u"Synthetic Track 2"_s}));
    const auto tracks = m_library.libraryTracks();
    ASSERT_EQ(tracks.size(), 3U);
    EXPECT_TRUE(tracks.at(0).isLoved());
    EXPECT_FALSE(tracks.at(1).isLoved());
    EXPECT_TRUE(tracks.at(2).isLoved());
    for(const auto& track : playlists.at(0).tracks) {
        EXPECT_GT(track.id(), 0);
    }

    EXPECT_TRUE(m_server.requestsForPath(u"/jf/Playlists/video1/Items"_s).empty());
    EXPECT_EQ(m_server.requestsForPath(u"/jf/Playlists/playlist1/Items"_s).size(), 2U);
    for(const auto& request : m_server.requests()) {
        EXPECT_EQ(request.method, "GET");
    }

    const auto image = m_access->image(sourceKey(), u"track1"_s);
    ASSERT_TRUE(image.has_value());
    EXPECT_EQ(image->itemId, u"album1"_s);
    EXPECT_EQ(image->tag, u"tag1"_s);
}

TEST_F(JellyfinCatalogSyncTest, KeepsTrackIdentityAcrossRepeatedSyncs)
{
    ASSERT_TRUE(runSync(nullptr));
    const int libraryId     = m_library.libraries().begin()->first;
    const TrackList initial = m_library.tracksForLibrary(libraryId);

    m_scenario->totalTracks = 4;
    ASSERT_TRUE(runSync(nullptr));
    const TrackList updated = m_library.tracksForLibrary(libraryId);

    ASSERT_EQ(m_library.libraries().size(), 1U);
    ASSERT_EQ(updated.size(), 4U);
    for(const auto& before : initial) {
        const auto it = std::ranges::find(updated, before.filepath(), &Track::filepath);
        ASSERT_NE(it, updated.end());
        EXPECT_EQ(it->id(), before.id());
    }
}

TEST_F(JellyfinCatalogSyncTest, RemovalsApplyOnlyAfterACompleteSnapshot)
{
    ASSERT_TRUE(runSync(nullptr));
    const int libraryId = m_library.libraries().begin()->first;

    m_scenario->totalTracks     = 1;
    m_scenario->failItemsStatus = 500;
    QList<QVariantList> results;
    EXPECT_FALSE(runSync(&results));
    EXPECT_TRUE(results.first().at(1).toString().contains("500"_L1));
    EXPECT_EQ(m_library.tracksForLibrary(libraryId).size(), 3U);
    EXPECT_EQ(m_library.snapshotRequests, 1);

    m_scenario->failItemsStatus = 0;
    m_scenario->playlistItems   = {u"track0"_s};
    ASSERT_TRUE(runSync(nullptr));
    EXPECT_EQ(m_library.tracksForLibrary(libraryId).size(), 1U);
}

TEST_F(JellyfinCatalogSyncTest, TotalChangeDuringPaginationKeepsPreviousCatalog)
{
    ASSERT_TRUE(runSync(nullptr));
    const int libraryId = m_library.libraries().begin()->first;

    m_scenario->totalTracks                = 600;
    m_scenario->changedTotalAfterFirstPage = 599;
    QList<QVariantList> results;
    EXPECT_FALSE(runSync(&results));
    EXPECT_TRUE(results.first().at(1).toString().contains("changed"_L1));
    EXPECT_EQ(m_library.tracksForLibrary(libraryId).size(), 3U);
}

TEST_F(JellyfinCatalogSyncTest, UnreachableServerKeepsPreviousCatalog)
{
    ASSERT_TRUE(runSync(nullptr));
    const int libraryId = m_library.libraries().begin()->first;

    QUrl offline = m_server.url(u"/jf"_s);
    offline.setPort(1);
    m_access->setAccess(connectionFor(offline, u"user1"_s), Token);
    EXPECT_FALSE(runSync(nullptr));
    EXPECT_EQ(m_library.tracksForLibrary(libraryId).size(), 3U);
}

TEST_F(JellyfinCatalogSyncTest, HostSnapshotFailureKeepsPreviousCatalog)
{
    ASSERT_TRUE(runSync(nullptr));
    const int libraryId = m_library.libraries().begin()->first;

    m_scenario->totalTracks = 1;
    m_library.failSnapshots = true;
    QSignalSpy playlistsReady{&m_sync, &CatalogSync::playlistsReady};
    EXPECT_FALSE(runSync(nullptr));
    EXPECT_EQ(m_library.tracksForLibrary(libraryId).size(), 3U);
    EXPECT_TRUE(playlistsReady.isEmpty());
}

TEST_F(JellyfinCatalogSyncTest, PlaylistStorageFailureReportsIncompleteSynchronisation)
{
    QObject::connect(&m_sync, &CatalogSync::playlistsReady, &m_sync,
                     [this]() { m_sync.fail(u"Synthetic playlist failure"_s); });
    QList<QVariantList> results;
    EXPECT_FALSE(runSync(&results));
    ASSERT_EQ(results.size(), 1);
    EXPECT_EQ(results.first().at(1).toString(), u"Synthetic playlist failure"_s);
    EXPECT_FALSE(m_sync.isRunning());
}

TEST_F(JellyfinCatalogSyncTest, InvalidPageIsRejected)
{
    m_server.setHandler([](const FakeRequest& /*request*/) {
        return FakeJellyfinServer::json(
            QJsonObject{{u"Items"_s, QJsonArray{QJsonObject{{u"Type"_s, u"Audio"_s}}}}, {u"TotalRecordCount"_s, 1}});
    });
    EXPECT_FALSE(runSync(nullptr));
    EXPECT_TRUE(m_library.libraries().empty());
}

TEST_F(JellyfinCatalogSyncTest, AccountChangeDuringSyncDiscardsTheRun)
{
    m_scenario->itemsDelayMs = 200;
    QSignalSpy finished{&m_sync, &CatalogSync::finished};
    m_sync.start(u"Jellyfin: Synthetic User"_s);
    ASSERT_TRUE(QTest::qWaitFor([this]() { return !m_server.requests().empty(); }, 5000));

    m_access->setAccess(connectionFor(m_server.url(u"/jf"_s), u"user2"_s), Token);
    ASSERT_TRUE(finished.wait(5000));
    EXPECT_FALSE(finished.first().at(0).toBool());
    EXPECT_EQ(m_library.snapshotRequests, 0);
    EXPECT_TRUE(m_library.libraries().empty());
}

TEST_F(JellyfinCatalogSyncTest, CancelStopsTheRunWithoutApplying)
{
    m_scenario->itemsDelayMs = 200;
    QSignalSpy finished{&m_sync, &CatalogSync::finished};
    m_sync.start(u"Jellyfin: Synthetic User"_s);
    ASSERT_TRUE(QTest::qWaitFor([this]() { return !m_server.requests().empty(); }, 5000));

    m_sync.cancel();
    EXPECT_FALSE(m_sync.isRunning());
    QTest::qWait(400);
    EXPECT_TRUE(finished.isEmpty());
    EXPECT_EQ(m_library.snapshotRequests, 0);
}

TEST_F(JellyfinCatalogSyncTest, AccountsUseSeparateLibraries)
{
    ASSERT_TRUE(runSync(nullptr));
    const QString firstSourceKey = sourceKey();

    m_access->setAccess(connectionFor(m_server.url(u"/jf"_s), u"user2"_s), Token);
    ASSERT_TRUE(runSync(nullptr));
    const QString secondSourceKey = sourceKey();

    EXPECT_NE(firstSourceKey, secondSourceKey);
    const auto libraries = m_library.libraries();
    ASSERT_EQ(libraries.size(), 2U);
    for(const auto& [id, library] : libraries) {
        for(const auto& track : m_library.tracksForLibrary(id)) {
            EXPECT_EQ(libraryUri(sourceKeyFromPath(track.filepath())), library.path);
        }
    }
    EXPECT_FALSE(m_access->image(firstSourceKey, u"track1"_s).has_value());
    EXPECT_TRUE(m_access->image(secondSourceKey, u"track1"_s).has_value());
}

TEST_F(JellyfinCatalogSyncTest, StartWithoutAccessFails)
{
    m_access->clearAccess();
    QList<QVariantList> results;
    EXPECT_FALSE(runSync(&results));
    EXPECT_TRUE(m_server.requests().empty());
}
TEST_F(JellyfinCatalogSyncTest, MissingPlaylistTotalRetainsPreviousSnapshot)
{
    ASSERT_TRUE(runSync(nullptr));
    const auto previous = titles(m_library.libraryTracks());
    const auto handler  = catalogHandler(m_scenario);
    m_server.setHandler([handler](const FakeRequest& request) {
        return request.query.queryItemValue(u"includeItemTypes"_s) == "Playlist"_L1
                 ? FakeJellyfinServer::json(QJsonObject{{u"Items"_s, QJsonArray{}}})
                 : handler(request);
    });
    QSignalSpy mirrors{&m_sync, &CatalogSync::playlistsReady};
    EXPECT_FALSE(runSync(nullptr));
    EXPECT_EQ(titles(m_library.libraryTracks()), previous);
    EXPECT_TRUE(mirrors.isEmpty());
}

TEST_F(JellyfinCatalogSyncTest, FractionalPlaylistTotalIsRejected)
{
    const auto handler = catalogHandler(m_scenario);
    m_server.setHandler([handler](const FakeRequest& request) {
        return request.query.queryItemValue(u"includeItemTypes"_s) == "Playlist"_L1
                 ? FakeJellyfinServer::json(QJsonObject{{u"Items"_s, QJsonArray{}}, {u"TotalRecordCount"_s, 0.5}})
                 : handler(request);
    });
    EXPECT_FALSE(runSync(nullptr));
    EXPECT_EQ(m_library.snapshotRequests, 0);
}

TEST_F(JellyfinCatalogSyncTest, WrongTypePlaylistTotalIsRejected)
{
    const auto handler = catalogHandler(m_scenario);
    m_server.setHandler([handler](const FakeRequest& request) {
        return request.query.queryItemValue(u"includeItemTypes"_s) == "Playlist"_L1
                 ? FakeJellyfinServer::json(QJsonObject{{u"Items"_s, QJsonArray{}}, {u"TotalRecordCount"_s, u"0"_s}})
                 : handler(request);
    });
    EXPECT_FALSE(runSync(nullptr));
    EXPECT_EQ(m_library.snapshotRequests, 0);
}

TEST_F(JellyfinCatalogSyncTest, DuplicatePlaylistIdentityRetainsPreviousSnapshot)
{
    ASSERT_TRUE(runSync(nullptr));
    const auto previous = titles(m_library.libraryTracks());
    m_scenario->playlists.append(m_scenario->playlists.first());
    EXPECT_FALSE(runSync(nullptr));
    EXPECT_EQ(titles(m_library.libraryTracks()), previous);
    EXPECT_EQ(m_library.snapshotRequests, 1);
}

TEST_F(JellyfinCatalogSyncTest, UnresolvedPlaylistOccurrenceRetainsPreviousSnapshot)
{
    ASSERT_TRUE(runSync(nullptr));
    const auto previous = titles(m_library.libraryTracks());
    m_scenario->playlistItems.insert(1, u"missing-item"_s);
    EXPECT_FALSE(runSync(nullptr));
    EXPECT_EQ(titles(m_library.libraryTracks()), previous);
    EXPECT_EQ(m_library.snapshotRequests, 1);
}

TEST_F(JellyfinCatalogSyncTest, OldSuccessAndFailureCannotCompleteNewSnapshotForSameSource)
{
    ASSERT_TRUE(runSync(nullptr));
    m_library.snapshotDelayMs = 200;
    m_sync.start(u"First refresh"_s);
    ASSERT_TRUE(QTest::qWaitFor([this]() { return m_library.snapshotRequests == 2; }, 5000));
    const QUuid previousOperation = m_library.lastSnapshotOperation;
    const int libraryId           = m_library.libraries().begin()->first;
    m_library.snapshotDelayMs     = 400;
    QSignalSpy finished{&m_sync, &CatalogSync::finished};
    QSignalSpy mirrors{&m_sync, &CatalogSync::playlistsReady};
    m_sync.start(u"Second refresh"_s);
    ASSERT_TRUE(QTest::qWaitFor([this]() { return m_library.snapshotRequests == 3; }, 5000));
    EXPECT_NE(previousOperation, m_library.lastSnapshotOperation);
    Q_EMIT m_library.externalLibrarySnapshotFinished(previousOperation, libraryId, true);
    Q_EMIT m_library.externalLibrarySnapshotFinished(previousOperation, libraryId, false);
    EXPECT_TRUE(finished.isEmpty());
    EXPECT_TRUE(mirrors.isEmpty());
    EXPECT_TRUE(m_sync.isRunning());
    ASSERT_TRUE(finished.wait(5000));
    ASSERT_EQ(finished.size(), 1);
    EXPECT_TRUE(finished.first().at(0).toBool());
    EXPECT_EQ(mirrors.size(), 1);
}
} // namespace Fooyin::Jellyfin::Testing
