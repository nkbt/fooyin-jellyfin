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

#include "fakejellyfinserver.h"
#include "jellyfinaccess.h"
#include "jellyfincatalogsync.h"
#include "jellyfintrack.h"
#include "testsupport.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QTest>

#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <set>

using namespace Qt::StringLiterals;

namespace Fooyin::Jellyfin::Testing {
namespace {
const QString Token = u"synthetic-access-token"_s;
const QByteArray TokenField{"Token=\"synthetic-access-token\""};
const QByteArray BodySentinel{"synthetic-body-sentinel"};

enum class FavouriteReply : int
{
    Echo,
    Opposite,
    NotJson,
    MissingField,
    WrongType,
    Redirect,
};

struct FavouriteServer
{
    std::mutex mutex;
    std::map<QString, bool> favourites{{u"track0"_s, true}, {u"track1"_s, false}, {u"track2"_s, false}};
    std::set<QString> failingItems;
    std::atomic<int> status{200};
    std::atomic<int> failNextRequests{0};
    std::atomic<int> delayMs{0};
    std::atomic<int> catalogDelayMs{0};
    //! False keeps the catalog value until a later request, like a server that applies a change late.
    std::atomic<bool> storeFavourites{true};
    std::atomic<FavouriteReply> reply{FavouriteReply::Echo};
    QUrl redirectTarget{u"http://127.0.0.2:9/jf/UserFavoriteItems/track1"_s};

    void setFavourite(const QString& itemId, bool favourite)
    {
        const std::scoped_lock lock{mutex};
        favourites[itemId] = favourite;
    }
    bool favourite(const QString& itemId)
    {
        const std::scoped_lock lock{mutex};
        return favourites[itemId];
    }
};

FakeResponse favouriteResponse(FavouriteServer& server, const FakeRequest& request)
{
    const QString itemId = request.path.section(u'/', -1);
    bool failing{false};
    {
        const std::scoped_lock lock{server.mutex};
        failing = server.failingItems.contains(itemId);
    }
    if(server.failNextRequests.fetch_sub(1) > 0 || failing) {
        FakeResponse response = FakeJellyfinServer::text(500, BodySentinel);
        response.delayMs      = server.delayMs;
        return response;
    }
    if(server.status != 200) {
        FakeResponse response = FakeJellyfinServer::text(server.status, BodySentinel);
        response.delayMs      = server.delayMs;
        return response;
    }

    const bool requested = request.method == "POST";
    if(server.storeFavourites) {
        server.setFavourite(itemId, requested);
    }
    FakeResponse response;
    switch(server.reply.load()) {
        case FavouriteReply::Echo:
            response = FakeJellyfinServer::json(
                QJsonObject{{u"IsFavorite"_s, requested}, {u"Key"_s, QString::fromLatin1(BodySentinel)}});
            break;
        case FavouriteReply::Opposite:
            server.setFavourite(itemId, !requested);
            response = FakeJellyfinServer::json(QJsonObject{{u"IsFavorite"_s, !requested}});
            break;
        case FavouriteReply::NotJson:
            response = FakeJellyfinServer::text(200, BodySentinel);
            break;
        case FavouriteReply::MissingField:
            response
                = FakeJellyfinServer::json(QJsonObject{{u"UserData"_s, QJsonObject{{u"IsFavorite"_s, requested}}}});
            break;
        case FavouriteReply::WrongType:
            response = FakeJellyfinServer::json(QJsonObject{{u"IsFavorite"_s, u"true"_s}});
            break;
        case FavouriteReply::Redirect:
            response = FakeJellyfinServer::redirect(server.redirectTarget);
            break;
    }
    response.delayMs = server.delayMs;
    return response;
}

FakeJellyfinServer::Handler favouriteHandler(std::shared_ptr<FavouriteServer> server)
{
    return [server](const FakeRequest& request) -> FakeResponse {
        if(request.path.contains("/UserFavoriteItems/"_L1)) {
            return favouriteResponse(*server, request);
        }
        if(request.method != "GET" || !request.path.endsWith("/Items"_L1)) {
            return FakeJellyfinServer::text(404, "Not found");
        }
        if(request.query.queryItemValue(u"includeItemTypes"_s) == "Playlist"_L1) {
            return FakeJellyfinServer::json(QJsonObject{{u"Items"_s, QJsonArray{}}, {u"TotalRecordCount"_s, 0}});
        }
        QJsonArray items;
        for(int index{0}; index < 3; ++index) {
            const QString itemId = u"track%1"_s.arg(index);
            items.append(QJsonObject{
                {u"Id"_s, itemId},
                {u"Type"_s, u"Audio"_s},
                {u"Name"_s, u"Synthetic Track %1"_s.arg(index)},
                {u"UserData"_s, QJsonObject{{u"IsFavorite"_s, server->favourite(itemId)}}},
            });
        }
        FakeResponse response = FakeJellyfinServer::json(QJsonObject{{u"Items"_s, items}, {u"TotalRecordCount"_s, 3}});
        response.delayMs      = server->catalogDelayMs;
        return response;
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
} // namespace

class JellyfinFavouriteSyncTest : public testing::Test
{
protected:
    JellyfinFavouriteSyncTest()
        : m_access{std::make_shared<AccessRegistry>()}
        , m_favourites{&m_network, &m_library, m_access, nullptr}
        , m_sync{&m_network, &m_library, m_access, &m_favourites, nullptr}
        , m_server{std::make_shared<FavouriteServer>()}
    {
        m_fake.setHandler(favouriteHandler(m_server));
        m_access->setAccess(connectionFor(m_fake.url(u"/jf"_s), u"user1"_s), Token);
    }

    void SetUp() override
    {
        ASSERT_TRUE(runSync());
        m_libraryInfo = m_library.libraries().begin()->second;
        m_favourites.activate(m_libraryInfo);
        m_fake.clearRequests();
    }

    bool runSync()
    {
        QSignalSpy finished{&m_sync, &CatalogSync::finished};
        m_sync.start(u"Jellyfin: Synthetic User"_s);
        return finished.wait(10000) && finished.first().at(0).toBool();
    }

    [[nodiscard]] Track track(const QString& itemId) const
    {
        for(const Track& track : m_library.tracks()) {
            if(itemIdFromPath(track.filepath()) == itemId) {
                return track;
            }
        }
        return {};
    }

    [[nodiscard]] bool loved(const QString& itemId) const
    {
        return track(itemId).isLoved();
    }

    void edit(const QString& itemId, bool loved)
    {
        Track edited{track(itemId)};
        edited.setLoved(loved);
        m_library.editTrackStats({edited}, Track::Stat::Loved);
    }

    [[nodiscard]] std::vector<FakeRequest> favouriteRequests() const
    {
        std::vector<FakeRequest> found;
        for(const auto& request : m_fake.requests()) {
            if(request.path.contains("/UserFavoriteItems/"_L1)) {
                found.push_back(request);
            }
        }
        return found;
    }

    bool waitForRequests(size_t count)
    {
        return QTest::qWaitFor([&]() { return favouriteRequests().size() >= count; }, 5000);
    }

    void expectInvalidResponseRestores(FavouriteReply reply)
    {
        const LogCapture log;
        QSignalSpy failed{&m_favourites, &FavouriteSync::failed};
        m_server->reply = reply;

        edit(u"track0"_s, false);
        EXPECT_FALSE(loved(u"track0"_s));
        ASSERT_TRUE(failed.wait(5000));
        EXPECT_TRUE(loved(u"track0"_s));
        EXPECT_TRUE(failed.first().at(0).toString().contains("invalid favourite response"_L1));
        EXPECT_TRUE(log.contains(u"Favourite change failed"_s));
        EXPECT_FALSE(log.contains(QString::fromLatin1(BodySentinel)));
    }

    //! Waits until every request reached the client and was handled.
    void settle()
    {
        QTest::qWait(m_server->delayMs + 300);
    }

    StubMusicLibrary m_library;
    std::shared_ptr<AccessRegistry> m_access;
    QNetworkAccessManager m_network;
    FavouriteSync m_favourites;
    CatalogSync m_sync;
    std::shared_ptr<FavouriteServer> m_server;
    FakeJellyfinServer m_fake;
    LibraryInfo m_libraryInfo;
};

TEST_F(JellyfinFavouriteSyncTest, LoveAndUnloveSendTheFavouriteEndpointWithAnEmptyBody)
{
    QSignalSpy failed{&m_favourites, &FavouriteSync::failed};
    EXPECT_TRUE(m_library.canEditTrackStats(track(u"track1"_s), Track::Stat::Loved));

    edit(u"track1"_s, true);
    EXPECT_TRUE(loved(u"track1"_s));
    ASSERT_TRUE(waitForRequests(1));
    settle();

    edit(u"track1"_s, false);
    EXPECT_FALSE(loved(u"track1"_s));
    ASSERT_TRUE(waitForRequests(2));
    settle();

    const auto requests = favouriteRequests();
    ASSERT_EQ(requests.size(), 2U);
    EXPECT_EQ(requests.at(0).method, "POST");
    EXPECT_EQ(requests.at(1).method, "DELETE");
    for(const auto& request : requests) {
        EXPECT_EQ(request.path, u"/jf/UserFavoriteItems/track1"_s);
        EXPECT_EQ(request.query.query(), u"userId=user1"_s);
        EXPECT_TRUE(request.header("authorization").contains(TokenField));
        EXPECT_TRUE(request.body.isEmpty());
    }
    EXPECT_FALSE(loved(u"track1"_s));
    EXPECT_FALSE(m_server->favourite(u"track1"_s));
    EXPECT_TRUE(failed.isEmpty());
}

TEST_F(JellyfinFavouriteSyncTest, ReturnedFavouriteValueIsConfirmed)
{
    QSignalSpy failed{&m_favourites, &FavouriteSync::failed};
    m_server->reply = FavouriteReply::Opposite;

    edit(u"track1"_s, true);
    EXPECT_TRUE(loved(u"track1"_s));
    ASSERT_TRUE(waitForRequests(1));
    ASSERT_TRUE(QTest::qWaitFor([&]() { return !loved(u"track1"_s); }, 5000));
    EXPECT_TRUE(failed.isEmpty());

    m_server->reply = FavouriteReply::Echo;
    edit(u"track1"_s, true);
    ASSERT_TRUE(waitForRequests(2));
    settle();
    EXPECT_TRUE(loved(u"track1"_s));
    EXPECT_EQ(favouriteRequests().back().method, "POST");
}

TEST_F(JellyfinFavouriteSyncTest, NonJsonResponseRestoresTheConfirmedValue)
{
    expectInvalidResponseRestores(FavouriteReply::NotJson);
}

TEST_F(JellyfinFavouriteSyncTest, WrappedFavouriteResponseRestoresTheConfirmedValue)
{
    expectInvalidResponseRestores(FavouriteReply::MissingField);
}

TEST_F(JellyfinFavouriteSyncTest, NonBooleanFavouriteResponseRestoresTheConfirmedValue)
{
    expectInvalidResponseRestores(FavouriteReply::WrongType);
}

TEST_F(JellyfinFavouriteSyncTest, HttpFailureRestoresAndReportsWithoutSensitiveDetails)
{
    const LogCapture log;
    QSignalSpy failed{&m_favourites, &FavouriteSync::failed};
    m_server->status = 500;

    edit(u"track1"_s, true);
    EXPECT_TRUE(loved(u"track1"_s));
    ASSERT_TRUE(failed.wait(5000));
    EXPECT_FALSE(loved(u"track1"_s));

    const QString message = failed.first().at(0).toString();
    EXPECT_TRUE(message.contains("HTTP status 500"_L1));
    const QString logged = log.messages().join(u'\n');
    EXPECT_FALSE(message.contains(QString::fromLatin1(BodySentinel)));
    EXPECT_FALSE(message.contains(Token));
    EXPECT_FALSE(message.contains(m_fake.url({}).authority()));
    EXPECT_FALSE(message.contains("UserFavoriteItems"_L1));
    EXPECT_FALSE(logged.contains(QString::fromLatin1(BodySentinel)));
    EXPECT_FALSE(logged.contains(Token));
    EXPECT_FALSE(logged.contains(m_fake.url({}).authority()));
    EXPECT_FALSE(logged.contains("UserFavoriteItems"_L1));

    m_server->status = 401;
    edit(u"track1"_s, true);
    ASSERT_TRUE(failed.wait(5000));
    EXPECT_FALSE(loved(u"track1"_s));
    EXPECT_TRUE(failed.last().at(0).toString().contains("rejected the credentials"_L1));
}

TEST_F(JellyfinFavouriteSyncTest, TransportFailureRestoresTheConfirmedValue)
{
    QSignalSpy failed{&m_favourites, &FavouriteSync::failed};
    m_access->setAccess(connectionFor(QUrl{u"http://127.0.0.1:1/jf"_s}, u"user1"_s), Token);
    m_favourites.activate(m_libraryInfo);

    edit(u"track1"_s, true);
    EXPECT_TRUE(loved(u"track1"_s));
    ASSERT_TRUE(failed.wait(10000));
    EXPECT_FALSE(loved(u"track1"_s));
    EXPECT_TRUE(failed.first().at(0).toString().contains("could not be reached"_L1));
}

TEST_F(JellyfinFavouriteSyncTest, RedirectToAnotherOriginFails)
{
    QSignalSpy failed{&m_favourites, &FavouriteSync::failed};
    m_server->reply = FavouriteReply::Redirect;

    edit(u"track1"_s, true);
    ASSERT_TRUE(failed.wait(5000));
    EXPECT_FALSE(loved(u"track1"_s));
    EXPECT_TRUE(failed.first().at(0).toString().contains("redirected to another origin"_L1));
    EXPECT_FALSE(failed.first().at(0).toString().contains("127.0.0.2"_L1));
}

TEST_F(JellyfinFavouriteSyncTest, RapidOppositeClicksKeepOneOrderedRequestPerItem)
{
    QSignalSpy failed{&m_favourites, &FavouriteSync::failed};
    m_server->delayMs = 300;

    edit(u"track1"_s, true);
    edit(u"track1"_s, false);
    edit(u"track1"_s, true);
    edit(u"track1"_s, false);
    EXPECT_FALSE(loved(u"track1"_s));
    ASSERT_TRUE(waitForRequests(1));
    QTest::qWait(150);
    EXPECT_EQ(favouriteRequests().size(), 1U);

    ASSERT_TRUE(waitForRequests(2));
    settle();
    const auto requests = favouriteRequests();
    ASSERT_EQ(requests.size(), 2U);
    EXPECT_EQ(requests.at(0).method, "POST");
    EXPECT_EQ(requests.at(1).method, "DELETE");
    EXPECT_FALSE(loved(u"track1"_s));
    EXPECT_FALSE(m_server->favourite(u"track1"_s));
    EXPECT_TRUE(failed.isEmpty());
}

TEST_F(JellyfinFavouriteSyncTest, ReturningToTheSentChoiceNeedsNoFurtherRequest)
{
    m_server->delayMs = 300;

    edit(u"track1"_s, true);
    edit(u"track1"_s, false);
    edit(u"track1"_s, true);
    ASSERT_TRUE(waitForRequests(1));
    settle();
    EXPECT_EQ(favouriteRequests().size(), 1U);
    EXPECT_TRUE(loved(u"track1"_s));
    EXPECT_TRUE(m_server->favourite(u"track1"_s));
}

TEST_F(JellyfinFavouriteSyncTest, StaleFailureCannotOverrideANewerChoice)
{
    QSignalSpy failed{&m_favourites, &FavouriteSync::failed};
    m_server->delayMs          = 300;
    m_server->failNextRequests = 1;

    edit(u"track1"_s, true);
    ASSERT_TRUE(waitForRequests(1));
    edit(u"track1"_s, false);
    ASSERT_TRUE(waitForRequests(2));
    settle();

    EXPECT_FALSE(loved(u"track1"_s));
    EXPECT_EQ(favouriteRequests().back().method, "DELETE");
    EXPECT_TRUE(failed.isEmpty());
}

TEST_F(JellyfinFavouriteSyncTest, ItemsAreIndependent)
{
    QSignalSpy failed{&m_favourites, &FavouriteSync::failed};
    m_server->delayMs = 300;
    {
        const std::scoped_lock lock{m_server->mutex};
        m_server->failingItems.insert(u"track2"_s);
    }

    edit(u"track1"_s, true);
    edit(u"track2"_s, true);
    ASSERT_TRUE(waitForRequests(2));
    settle();

    EXPECT_TRUE(loved(u"track1"_s));
    EXPECT_FALSE(loved(u"track2"_s));
    EXPECT_EQ(failed.size(), 1);
}

TEST_F(JellyfinFavouriteSyncTest, BatchEditPublishesEveryTrackAtOnce)
{
    Track first{track(u"track1"_s)};
    Track second{track(u"track2"_s)};
    first.setLoved(true);
    second.setLoved(true);
    const size_t publications = m_library.trackStatsPublications.size();

    m_library.editTrackStats({first, second}, Track::Stat::Loved);
    EXPECT_TRUE(loved(u"track1"_s));
    EXPECT_TRUE(loved(u"track2"_s));
    ASSERT_EQ(m_library.trackStatsPublications.size(), publications + 1);
    EXPECT_EQ(m_library.trackStatsPublications.back().size(), 2U);
    ASSERT_TRUE(waitForRequests(2));
    settle();
    EXPECT_TRUE(m_server->favourite(u"track1"_s));
    EXPECT_TRUE(m_server->favourite(u"track2"_s));
}

TEST_F(JellyfinFavouriteSyncTest, LocalStorageFailureAfterServerSuccessKeepsTheValue)
{
    QSignalSpy failed{&m_favourites, &FavouriteSync::failed};
    m_library.failTrackStats = true;

    edit(u"track1"_s, true);
    EXPECT_TRUE(loved(u"track1"_s));
    ASSERT_TRUE(waitForRequests(1));
    settle();

    EXPECT_TRUE(loved(u"track1"_s));
    EXPECT_TRUE(m_server->favourite(u"track1"_s));
    ASSERT_EQ(failed.size(), 1);
    EXPECT_TRUE(failed.first().at(0).toString().contains("could not save"_L1));
    EXPECT_EQ(m_library.trackStatsPublications.size(), 1U);
}

TEST_F(JellyfinFavouriteSyncTest, ServerCorrectionWhoseStorageFailsStaysVisible)
{
    QSignalSpy failed{&m_favourites, &FavouriteSync::failed};
    m_server->reply   = FavouriteReply::Opposite;
    m_server->delayMs = 200;

    edit(u"track1"_s, true);
    ASSERT_TRUE(waitForRequests(1));
    QTest::qWait(50);
    m_library.failTrackStats = true;
    settle();

    EXPECT_FALSE(loved(u"track1"_s));
    ASSERT_EQ(failed.size(), 1);
    EXPECT_TRUE(failed.first().at(0).toString().contains("could not save"_L1));
    EXPECT_EQ(m_library.trackStatsPublications.size(), 2U);
}

TEST_F(JellyfinFavouriteSyncTest, RollbackWhoseStorageFailsStaysVisibleWithoutRetry)
{
    QSignalSpy failed{&m_favourites, &FavouriteSync::failed};
    m_server->status  = 500;
    m_server->delayMs = 200;

    edit(u"track1"_s, true);
    ASSERT_TRUE(waitForRequests(1));
    QTest::qWait(50);
    m_library.failTrackStats = true;
    settle();
    QTest::qWait(300);

    EXPECT_FALSE(loved(u"track1"_s));
    ASSERT_EQ(failed.size(), 2);
    EXPECT_TRUE(failed.at(0).at(0).toString().contains("HTTP status 500"_L1));
    EXPECT_TRUE(failed.at(1).at(0).toString().contains("could not save"_L1));
    EXPECT_EQ(m_library.trackStatsPublications.size(), 2U);
    EXPECT_EQ(favouriteRequests().size(), 1U);
}

TEST_F(JellyfinFavouriteSyncTest, CatalogRefreshKeepsAPendingEdit)
{
    m_server->storeFavourites = false;
    m_server->delayMs         = 800;

    edit(u"track1"_s, true);
    ASSERT_TRUE(waitForRequests(1));
    ASSERT_TRUE(runSync());
    EXPECT_FALSE(m_server->favourite(u"track1"_s));
    EXPECT_TRUE(loved(u"track1"_s));

    settle();
    EXPECT_TRUE(loved(u"track1"_s));
}

TEST_F(JellyfinFavouriteSyncTest, CatalogFetchedBeforeAnEditCannotOverwriteIt)
{
    m_server->catalogDelayMs = 600;
    QSignalSpy finished{&m_sync, &CatalogSync::finished};

    m_sync.start(u"Jellyfin: Synthetic User"_s);
    ASSERT_TRUE(QTest::qWaitFor([&]() { return !m_fake.requestsForPath(u"/jf/Items"_s).empty(); }, 5000));
    edit(u"track1"_s, true);
    ASSERT_TRUE(waitForRequests(1));
    settle();
    EXPECT_TRUE(finished.isEmpty());

    ASSERT_TRUE(finished.wait(10000));
    ASSERT_TRUE(finished.first().at(0).toBool());
    EXPECT_TRUE(loved(u"track1"_s));
}

TEST_F(JellyfinFavouriteSyncTest, SubmittedCatalogIsTheRollbackValueBeforeItIsStored)
{
    QSignalSpy failed{&m_favourites, &FavouriteSync::failed};
    QSignalSpy finished{&m_sync, &CatalogSync::finished};
    m_server->setFavourite(u"track0"_s, false);
    m_server->failNextRequests = 1;
    m_library.snapshotDelayMs  = 1500;
    const int snapshots        = m_library.snapshotRequests;

    m_sync.start(u"Jellyfin: Synthetic User"_s);
    ASSERT_TRUE(QTest::qWaitFor([&]() { return m_library.snapshotRequests > snapshots; }, 5000));
    ASSERT_TRUE(loved(u"track0"_s));
    edit(u"track0"_s, true);
    ASSERT_TRUE(failed.wait(5000));
    EXPECT_TRUE(finished.isEmpty());
    EXPECT_FALSE(loved(u"track0"_s));
    EXPECT_EQ(favouriteRequests().front().method, "POST");

    ASSERT_TRUE(finished.wait(10000));
    ASSERT_TRUE(finished.first().at(0).toBool());
    EXPECT_FALSE(loved(u"track0"_s));
    EXPECT_EQ(favouriteRequests().size(), 1U);
}

TEST_F(JellyfinFavouriteSyncTest, ChoiceMatchingASubmittedCatalogShowsAtOnceWithoutARequest)
{
    QSignalSpy failed{&m_favourites, &FavouriteSync::failed};
    QSignalSpy finished{&m_sync, &CatalogSync::finished};
    m_server->setFavourite(u"track0"_s, false);
    m_server->failNextRequests = 1;
    m_library.snapshotDelayMs  = 1500;
    const int snapshots        = m_library.snapshotRequests;

    m_sync.start(u"Jellyfin: Synthetic User"_s);
    ASSERT_TRUE(QTest::qWaitFor([&]() { return m_library.snapshotRequests > snapshots; }, 5000));
    ASSERT_TRUE(loved(u"track0"_s));
    edit(u"track0"_s, false);
    EXPECT_FALSE(loved(u"track0"_s));
    settle();
    EXPECT_TRUE(finished.isEmpty());
    EXPECT_FALSE(loved(u"track0"_s));

    ASSERT_TRUE(finished.wait(10000));
    ASSERT_TRUE(finished.first().at(0).toBool());
    EXPECT_FALSE(loved(u"track0"_s));
    EXPECT_TRUE(favouriteRequests().empty());
    EXPECT_TRUE(failed.isEmpty());
}

TEST_F(JellyfinFavouriteSyncTest, RefreshConfirmingAPendingEditIsTheRollbackValue)
{
    QSignalSpy failed{&m_favourites, &FavouriteSync::failed};
    m_server->failingItems = {u"track1"_s};
    m_server->delayMs      = 800;

    edit(u"track1"_s, true);
    ASSERT_TRUE(waitForRequests(1));
    // The server applied the change but its reply fails.
    m_server->setFavourite(u"track1"_s, true);
    ASSERT_TRUE(runSync());
    EXPECT_TRUE(loved(u"track1"_s));
    EXPECT_TRUE(failed.isEmpty());
    const size_t publications = m_library.trackStatsPublications.size();

    ASSERT_TRUE(failed.wait(5000));
    settle();
    EXPECT_TRUE(loved(u"track1"_s));
    EXPECT_EQ(m_library.trackStatsPublications.size(), publications);
    EXPECT_EQ(failed.size(), 1);
    EXPECT_EQ(favouriteRequests().size(), 1U);
}

TEST_F(JellyfinFavouriteSyncTest, CatalogFetchedBeforeAServerResultCannotReplaceTheRollbackValue)
{
    QSignalSpy failed{&m_favourites, &FavouriteSync::failed};
    QSignalSpy finished{&m_sync, &CatalogSync::finished};
    m_server->catalogDelayMs = 600;

    m_sync.start(u"Jellyfin: Synthetic User"_s);
    ASSERT_TRUE(QTest::qWaitFor([&]() { return !m_fake.requestsForPath(u"/jf/Items"_s).empty(); }, 5000));
    edit(u"track1"_s, true);
    ASSERT_TRUE(waitForRequests(1));
    settle();
    ASSERT_TRUE(finished.wait(10000));
    ASSERT_TRUE(finished.first().at(0).toBool());
    EXPECT_TRUE(loved(u"track1"_s));

    m_server->status = 500;
    edit(u"track1"_s, false);
    EXPECT_FALSE(loved(u"track1"_s));
    ASSERT_TRUE(failed.wait(5000));
    settle();
    EXPECT_TRUE(loved(u"track1"_s));
    EXPECT_EQ(failed.size(), 1);
    EXPECT_EQ(favouriteRequests().size(), 2U);
}

TEST_F(JellyfinFavouriteSyncTest, CatalogConfirmsAPendingEditWithoutReplacingANewerChoice)
{
    QSignalSpy failed{&m_favourites, &FavouriteSync::failed};
    QSignalSpy finished{&m_sync, &CatalogSync::finished};
    m_server->setFavourite(u"track0"_s, false);
    m_server->failingItems   = {u"track0"_s};
    m_server->catalogDelayMs = 300;
    m_server->delayMs        = 800;

    m_sync.start(u"Jellyfin: Synthetic User"_s);
    ASSERT_TRUE(QTest::qWaitFor([&]() { return !m_fake.requestsForPath(u"/jf/Items"_s).empty(); }, 5000));
    edit(u"track0"_s, false);
    ASSERT_TRUE(waitForRequests(1));
    edit(u"track0"_s, true);
    ASSERT_TRUE(finished.wait(10000));
    ASSERT_TRUE(finished.first().at(0).toBool());
    EXPECT_TRUE(loved(u"track0"_s));
    EXPECT_TRUE(failed.isEmpty());

    ASSERT_TRUE(failed.wait(5000));
    settle();
    EXPECT_FALSE(loved(u"track0"_s));
    EXPECT_EQ(failed.size(), 1);
    ASSERT_EQ(favouriteRequests().size(), 2U);
    EXPECT_EQ(favouriteRequests().front().method, "DELETE");
    EXPECT_EQ(favouriteRequests().back().method, "POST");
}

TEST_F(JellyfinFavouriteSyncTest, RepeatedRefreshKeepsOneHandlerRegistration)
{
    QSignalSpy handlerChanged{&m_library, &MusicLibrary::externalLibraryStatsHandlerChanged};

    ASSERT_TRUE(runSync());
    ASSERT_TRUE(runSync());
    EXPECT_TRUE(handlerChanged.isEmpty());
    EXPECT_TRUE(m_library.canEditTrackStats(track(u"track1"_s), Track::Stat::Loved));
}

TEST_F(JellyfinFavouriteSyncTest, RefreshRestoresEditingOfARecreatedLibrary)
{
    QSignalSpy failed{&m_favourites, &FavouriteSync::failed};
    QSignalSpy handlerChanged{&m_library, &MusicLibrary::externalLibraryStatsHandlerChanged};
    m_library.removeExternalLibrary(m_libraryInfo.id);
    ASSERT_FALSE(track(u"track1"_s).isValid());
    EXPECT_EQ(handlerChanged.size(), 1);

    ASSERT_TRUE(runSync());
    const Track recreated = track(u"track1"_s);
    ASSERT_TRUE(recreated.isValid());
    EXPECT_EQ(recreated.libraryId(), m_libraryInfo.id);
    EXPECT_EQ(handlerChanged.size(), 2);
    EXPECT_TRUE(m_library.canEditTrackStats(recreated, Track::Stat::Loved));

    edit(u"track1"_s, true);
    EXPECT_TRUE(loved(u"track1"_s));
    ASSERT_TRUE(waitForRequests(1));
    settle();
    const FakeRequest request = favouriteRequests().front();
    EXPECT_EQ(request.method, "POST");
    EXPECT_EQ(request.query.query(), u"userId=user1"_s);
    EXPECT_TRUE(request.header("authorization").contains(TokenField));
    EXPECT_TRUE(loved(u"track1"_s));
    EXPECT_TRUE(failed.isEmpty());
}

TEST_F(JellyfinFavouriteSyncTest, RemovalDuringADelayedEditIgnoresItsReplyAfterRecreation)
{
    QSignalSpy failed{&m_favourites, &FavouriteSync::failed};
    m_server->storeFavourites = false;
    m_server->delayMs         = 800;

    edit(u"track1"_s, true);
    ASSERT_TRUE(waitForRequests(1));
    const size_t publications = m_library.trackStatsPublications.size();
    m_library.removeExternalLibrary(m_libraryInfo.id);
    EXPECT_EQ(m_library.trackStatsPublications.size(), publications);

    m_server->delayMs = 0;
    ASSERT_TRUE(runSync());
    EXPECT_FALSE(loved(u"track1"_s));
    QTest::qWait(1100);
    EXPECT_FALSE(loved(u"track1"_s));
    EXPECT_EQ(m_library.trackStatsPublications.size(), publications);
    EXPECT_EQ(favouriteRequests().size(), 1U);
    EXPECT_TRUE(failed.isEmpty());

    m_server->storeFavourites = true;
    edit(u"track1"_s, true);
    EXPECT_TRUE(loved(u"track1"_s));
    ASSERT_TRUE(waitForRequests(2));
    settle();
    EXPECT_EQ(favouriteRequests().back().method, "POST");
    EXPECT_TRUE(loved(u"track1"_s));
    EXPECT_TRUE(failed.isEmpty());
}

TEST_F(JellyfinFavouriteSyncTest, RemovalWithoutRecreationStopsEditing)
{
    QSignalSpy failed{&m_favourites, &FavouriteSync::failed};
    m_server->delayMs   = 600;
    const Track removed = track(u"track1"_s);

    edit(u"track1"_s, true);
    ASSERT_TRUE(waitForRequests(1));
    m_library.removeExternalLibrary(m_libraryInfo.id);
    settle();
    EXPECT_FALSE(m_library.trackForId(removed.id()).isValid());
    EXPECT_TRUE(failed.isEmpty());

    m_favourites.release();
    QTest::qWait(100);
    EXPECT_TRUE(failed.isEmpty());
    EXPECT_EQ(favouriteRequests().size(), 1U);
}

TEST_F(JellyfinFavouriteSyncTest, CatalogRefreshAdoptsServerChangesAfterSettledEdits)
{
    edit(u"track1"_s, true);
    ASSERT_TRUE(waitForRequests(1));
    settle();
    ASSERT_TRUE(loved(u"track1"_s));

    m_server->setFavourite(u"track1"_s, false);
    ASSERT_TRUE(runSync());
    EXPECT_FALSE(loved(u"track1"_s));

    edit(u"track1"_s, true);
    ASSERT_TRUE(waitForRequests(2));
    settle();
    EXPECT_EQ(favouriteRequests().back().method, "POST");
    EXPECT_TRUE(loved(u"track1"_s));
}

TEST_F(JellyfinFavouriteSyncTest, ForeignAndLocalTracksAreIgnored)
{
    const auto foreign = m_library.addExternalLibrary(libraryUri(u"othersource"_s), u"Other"_s);
    ASSERT_TRUE(foreign);
    m_library.setExternalLibraryReadOnlyStats(*foreign, Track::Stat::Loved);
    QSignalSpy applied{&m_library, &MusicLibrary::externalLibrarySnapshotFinished};
    m_library.applyExternalLibrarySnapshot(*foreign, {Track{trackPath(u"othersource"_s, u"track1"_s)}},
                                           QUuid::createUuid());
    ASSERT_TRUE(applied.wait(5000));
    Track foreignTrack;
    for(const Track& candidate : m_library.tracks()) {
        if(candidate.libraryId() == foreign->id) {
            foreignTrack = candidate;
        }
    }
    ASSERT_TRUE(foreignTrack.isValid());
    EXPECT_FALSE(m_library.canEditTrackStats(foreignTrack, Track::Stat::Loved));

    const size_t publications = m_library.trackStatsPublications.size();
    foreignTrack.setLoved(true);
    Q_EMIT m_library.externalTrackStatsChangeRequested(*foreign, {foreignTrack}, Track::Stat::Loved);

    Track local{u"/music/synthetic.flac"_s};
    local.setLoved(true);
    Q_EMIT m_library.externalTrackStatsChangeRequested(m_libraryInfo, {local}, Track::Stat::Loved);

    Track forged{trackPath(u"othersource"_s, u"track1"_s)};
    forged.setId(track(u"track1"_s).id());
    forged.setLoved(true);
    Q_EMIT m_library.externalTrackStatsChangeRequested(m_libraryInfo, {forged}, Track::Stat::Loved);

    QTest::qWait(200);
    EXPECT_TRUE(favouriteRequests().empty());
    EXPECT_EQ(m_library.trackStatsPublications.size(), publications);
    EXPECT_FALSE(loved(u"track1"_s));
}

TEST_F(JellyfinFavouriteSyncTest, ReleaseCancelsPendingEditsAndStopsEditing)
{
    QSignalSpy failed{&m_favourites, &FavouriteSync::failed};
    m_server->delayMs = 600;

    edit(u"track1"_s, true);
    ASSERT_TRUE(waitForRequests(1));
    m_favourites.release();
    EXPECT_FALSE(loved(u"track1"_s));
    EXPECT_FALSE(m_library.canEditTrackStats(track(u"track1"_s), Track::Stat::Loved));

    edit(u"track1"_s, true);
    settle();
    EXPECT_FALSE(loved(u"track1"_s));
    EXPECT_EQ(favouriteRequests().size(), 1U);
    EXPECT_TRUE(failed.isEmpty());
}

TEST_F(JellyfinFavouriteSyncTest, AccountChangeAbortsTheRequestAndIgnoresLaterEdits)
{
    QSignalSpy failed{&m_favourites, &FavouriteSync::failed};
    m_server->delayMs = 600;

    edit(u"track1"_s, true);
    ASSERT_TRUE(waitForRequests(1));
    m_access->setAccess(connectionFor(m_fake.url(u"/jf"_s), u"user2"_s), u"synthetic-second-token"_s);
    edit(u"track2"_s, true);
    settle();

    EXPECT_EQ(favouriteRequests().size(), 1U);
    EXPECT_FALSE(loved(u"track2"_s));
    EXPECT_TRUE(failed.isEmpty());

    m_favourites.release();
    EXPECT_FALSE(loved(u"track1"_s));
}
} // namespace Fooyin::Jellyfin::Testing
