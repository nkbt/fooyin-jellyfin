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

#include "jellyfinplaybackreporter.h"
#include "fakejellyfinserver.h"
#include "jellyfinaccess.h"
#include "jellyfintrack.h"

#include <QJsonDocument>
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

Connection connectionFor(const QUrl& serverUrl, const QString& userId)
{
    return {.serverUrl = serverUrl,
            .serverId  = u"server1"_s,
            .userId    = userId,
            .userName  = u"Synthetic User"_s,
            .deviceId  = u"device1"_s};
}

struct Report
{
    QString path;
    QJsonObject body;
};
} // namespace

class JellyfinPlaybackReporterTest : public testing::Test
{
protected:
    JellyfinPlaybackReporterTest()
        : m_access{std::make_shared<AccessRegistry>()}
        , m_reporter{&m_network, m_access, nullptr}
        , m_finished{&m_reporter, &PlaybackReporter::reportFinished}
    {
        auto delayMs = m_delayMs;
        m_server.setHandler([delayMs](const FakeRequest& request) {
            FakeResponse response{.status = 204, .headers = {}, .body = {}, .delayMs = *delayMs};
            if(!request.path.startsWith("/jf/Sessions/Playing"_L1)) {
                response.status = 404;
            }
            return response;
        });
        m_access->setAccess(connectionFor(m_server.url(u"/jf"_s), u"user1"_s), Token);
    }

    [[nodiscard]] Track track(const QString& itemId) const
    {
        Track result{trackPath(m_access->access()->sourceKey, itemId)};
        result.setId(1);
        return result;
    }

    bool waitForReports(int count)
    {
        return QTest::qWaitFor([this, count]() { return m_finished.size() >= count; }, 5000);
    }

    [[nodiscard]] std::vector<Report> reports() const
    {
        std::vector<Report> result;
        for(const auto& request : m_server.requests()) {
            EXPECT_EQ(request.method, "POST");
            EXPECT_TRUE(request.header("authorization").contains(TokenField));
            EXPECT_EQ(request.header("content-type"), "application/json");
            result.push_back({request.path.sliced(4), QJsonDocument::fromJson(request.body).object()});
        }
        return result;
    }

    std::shared_ptr<int> m_delayMs{std::make_shared<int>(0)};
    FakeJellyfinServer m_server;
    std::shared_ptr<AccessRegistry> m_access;
    QNetworkAccessManager m_network;
    PlaybackReporter m_reporter;
    QSignalSpy m_finished;
};

TEST_F(JellyfinPlaybackReporterTest, ReportsConfirmedStartProgressPauseSeekAndStop)
{
    Engine::PlaybackOccurrenceContext context{.track                = track(u"track1"_s),
                                              .playbackOccurrenceId = 1,
                                              .sourceGeneration     = m_access->access()->sourceGeneration,
                                              .seekable             = true};
    m_reporter.occurrenceStarted(context);
    context.positionMs = 5000;
    m_reporter.occurrenceUpdated(context);
    context.positionMs = 10000;
    m_reporter.occurrenceUpdated(context);
    context.paused = true;
    m_reporter.occurrenceUpdated(context);
    context.positionMs = 20000;
    m_reporter.occurrenceUpdated(context);
    context.paused = false;
    m_reporter.occurrenceUpdated(context);
    context.positionMs    = 60000;
    context.seekRequestId = 1;
    m_reporter.occurrenceUpdated(context);
    m_reporter.occurrenceEnded(context);
    ASSERT_TRUE(waitForReports(6));
    const auto sent = reports();
    ASSERT_EQ(sent.size(), 6U);
    EXPECT_EQ(sent.at(0).path, u"Sessions/Playing"_s);
    EXPECT_EQ(sent.at(1).path, u"Sessions/Playing/Progress"_s);
    EXPECT_EQ(sent.at(2).path, u"Sessions/Playing/Progress"_s);
    EXPECT_EQ(sent.at(3).path, u"Sessions/Playing/Progress"_s);
    EXPECT_EQ(sent.at(4).path, u"Sessions/Playing/Progress"_s);
    EXPECT_EQ(sent.at(5).path, u"Sessions/Playing/Stopped"_s);
    const QString session = sent.at(0).body.value(u"PlaySessionId"_s).toString();
    EXPECT_FALSE(session.isEmpty());
    for(const auto& report : sent) {
        EXPECT_EQ(report.body.value(u"ItemId"_s).toString(), u"track1"_s);
        EXPECT_EQ(report.body.value(u"PlaySessionId"_s).toString(), session);
        EXPECT_FALSE(report.body.contains(u"UserId"_s));
        EXPECT_FALSE(report.body.contains(u"EventName"_s));
        EXPECT_FALSE(report.body.contains(u"IsSeeking"_s));
    }
    EXPECT_TRUE(sent.at(2).body.value(u"IsPaused"_s).toBool());
    EXPECT_FALSE(sent.at(3).body.value(u"IsPaused"_s).toBool());
    EXPECT_EQ(sent.at(4).body.value(u"PositionTicks"_s).toInteger(), 600000000);
    EXPECT_EQ(sent.at(5).body.value(u"PositionTicks"_s).toInteger(), 600000000);
}

TEST_F(JellyfinPlaybackReporterTest, RepeatedTrackGetsANewPlaySession)
{
    Engine::PlaybackOccurrenceContext context{.track                = track(u"track1"_s),
                                              .playbackOccurrenceId = 1,
                                              .sourceGeneration     = m_access->access()->sourceGeneration};
    m_reporter.occurrenceStarted(context);
    m_reporter.occurrenceEnded(context);
    context.playbackOccurrenceId = 2;
    m_reporter.occurrenceStarted(context);
    m_reporter.occurrenceStarted(context);
    ASSERT_TRUE(waitForReports(3));
    const auto sent = reports();
    ASSERT_EQ(sent.size(), 3U);
    EXPECT_EQ(sent.at(1).path, u"Sessions/Playing/Stopped"_s);
    EXPECT_NE(sent.at(0).body.value(u"PlaySessionId"_s), sent.at(2).body.value(u"PlaySessionId"_s));
}

TEST_F(JellyfinPlaybackReporterTest, FailureReportsOneFailedStopWithFinalPosition)
{
    Engine::PlaybackOccurrenceContext context{.track                = track(u"track1"_s),
                                              .playbackOccurrenceId = 1,
                                              .sourceGeneration     = m_access->access()->sourceGeneration};
    m_reporter.occurrenceStarted(context);
    context.failed     = true;
    context.positionMs = 12345;
    m_reporter.occurrenceEnded(context);
    m_reporter.occurrenceEnded(context);
    m_reporter.shutdown();
    ASSERT_TRUE(waitForReports(2));
    const auto sent = reports();
    ASSERT_EQ(sent.size(), 2U);
    EXPECT_TRUE(sent.at(1).body.value(u"Failed"_s).toBool());
    EXPECT_EQ(sent.at(1).body.value(u"PositionTicks"_s).toInteger(), 123450000);
}

TEST_F(JellyfinPlaybackReporterTest, UnconfirmedTrackHasNoReports)
{
    Engine::PlaybackOccurrenceContext context{.track                = track(u"track1"_s),
                                              .playbackOccurrenceId = 1,
                                              .sourceGeneration     = m_access->access()->sourceGeneration,
                                              .positionMs           = 20000};
    m_reporter.occurrenceUpdated(context);
    m_reporter.occurrenceEnded(context);
    m_reporter.shutdown();
    QTest::qWait(50);
    EXPECT_TRUE(m_server.requests().empty());
}

TEST_F(JellyfinPlaybackReporterTest, LocalRadioInvalidAndSignedOutTracksAreInertWithAssertionsEnabled)
{
    const quint64 generation = m_access->access()->sourceGeneration;
    Engine::PlaybackOccurrenceContext local{
        .track = Track{u"/music/synthetic.flac"_s}, .playbackOccurrenceId = 1, .sourceGeneration = generation};
    m_reporter.occurrenceStarted(local);
    local.track                = Track{u"https://radio.example.test/stream"_s};
    local.playbackOccurrenceId = 2;
    m_reporter.occurrenceStarted(local);
    local.track                = Track{u"jellyfin://invalid"_s};
    local.playbackOccurrenceId = 3;
    m_reporter.occurrenceStarted(local);
    local.track                = track(u"track1"_s);
    local.playbackOccurrenceId = 4;
    m_access->clearAccess();
    m_reporter.occurrenceStarted(local);
    m_reporter.occurrenceUpdated(local);
    m_reporter.occurrenceEnded(local);
    QTest::qWait(50);
    EXPECT_TRUE(m_server.requests().empty());
}

TEST_F(JellyfinPlaybackReporterTest, SourceReplacementCancelsInFlightAndDropsQueuedReports)
{
    *m_delayMs = 300;
    Engine::PlaybackOccurrenceContext context{.track                = track(u"track1"_s),
                                              .playbackOccurrenceId = 1,
                                              .sourceGeneration     = m_access->access()->sourceGeneration};
    m_reporter.occurrenceStarted(context);
    context.positionMs    = 30000;
    context.seekRequestId = 1;
    m_reporter.occurrenceUpdated(context);
    m_reporter.occurrenceEnded(context);
    ASSERT_TRUE(QTest::qWaitFor([this]() { return !m_server.requests().empty(); }, 5000));
    m_access->setAccess(connectionFor(m_server.url(u"/jf"_s), u"user2"_s), Token);
    ASSERT_TRUE(waitForReports(3));
    EXPECT_EQ(reports().size(), 1U);
    EXPECT_FALSE(m_finished.at(1).at(1).toBool());
    EXPECT_FALSE(m_finished.at(2).at(1).toBool());
}

TEST_F(JellyfinPlaybackReporterTest, LateSourceGenerationCannotBorrowNewTokenOrEndCurrentOccurrence)
{
    Engine::PlaybackOccurrenceContext stale{.track                = track(u"track1"_s),
                                            .playbackOccurrenceId = 1,
                                            .sourceGeneration     = m_access->access()->sourceGeneration};
    m_access->setAccess(connectionFor(m_server.url(u"/jf"_s), u"user1"_s), Token);
    m_reporter.occurrenceStarted(stale);
    Engine::PlaybackOccurrenceContext current{.track                = track(u"track1"_s),
                                              .playbackOccurrenceId = 2,
                                              .sourceGeneration     = m_access->access()->sourceGeneration};
    m_reporter.occurrenceStarted(current);
    stale.playbackOccurrenceId = 3;
    m_reporter.occurrenceStarted(stale);
    m_reporter.occurrenceEnded(stale);
    current.positionMs = 14000;
    m_reporter.occurrenceEnded(current);
    ASSERT_TRUE(waitForReports(2));
    const auto sent = reports();
    ASSERT_EQ(sent.size(), 2U);
    EXPECT_EQ(sent.at(0).path, u"Sessions/Playing"_s);
    EXPECT_EQ(sent.at(1).path, u"Sessions/Playing/Stopped"_s);
    EXPECT_EQ(sent.at(1).body.value(u"PositionTicks"_s).toInteger(), 140000000);
}
} // namespace Fooyin::Jellyfin::Testing
