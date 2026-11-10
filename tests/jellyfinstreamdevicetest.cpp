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

#include "fakejellyfinserver.h"
#include "jellyfinrequest.h"
#include "testsupport.h"

#include <QElapsedTimer>
#include <QFile>
#include <QScopeGuard>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTest>
#include <barrier>
#include <core/network/remotestreamdevice.h>
#include <future>
#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace Fooyin::Jellyfin::Testing {
namespace {
QByteArray readBytes(QIODevice& device, qint64 count)
{
    QByteArray data;
    while(data.size() < count) {
        const QByteArray chunk = device.read(count - data.size());
        if(chunk.isEmpty()) {
            break;
        }
        data.append(chunk);
    }
    return data;
}
} // namespace

class JellyfinStreamDeviceTest : public testing::Test
{
protected:
    std::shared_ptr<PendingRemoteStream> prepare(std::stop_token sourceLifetime)
    {
        return m_remoteIoFixture.remoteIo->prepareStream(
            apiRequest(m_server.url(u"/jf/Audio/track1/stream"_s), m_authorization), sourceLifetime);
    }
    RemoteIoFixture m_remoteIoFixture;
    FakeJellyfinServer m_server;
    const QByteArray m_authorization{authorizationHeader(u"device1"_s, u"synthetic-access-token"_s)};
};

TEST_F(JellyfinStreamDeviceTest, ReusesHostPreparedStreamForAuthenticatedReadAndRangeSeek)
{
    const QByteArray data(512 * 1024, 'a');
    m_server.setHandler([data](const FakeRequest& request) { return FakeJellyfinServer::bytes(request, data, true); });
    const auto pending = prepare({});
    ASSERT_TRUE(pending->waitForReady(5s, {}));
    auto source = pending->takeSource();
    ASSERT_NE(source.device, nullptr);
    EXPECT_FALSE(source.device->isSequential());
    EXPECT_EQ(source.device->size(), data.size());
    EXPECT_EQ(readBytes(*source.device, 16), data.first(16));
    ASSERT_TRUE(source.device->seek(400000));
    EXPECT_EQ(readBytes(*source.device, 32), data.sliced(400000, 32));
    EXPECT_FALSE(pending->takeSource().device);
    for(const auto& request : m_server.requests()) {
        EXPECT_EQ(request.header("authorization"), m_authorization);
        EXPECT_FALSE(request.query.hasQueryItem(u"api_key"_s));
    }
}

TEST_F(JellyfinStreamDeviceTest, RefusesRangeReopenWhenServerIgnoresRange)
{
    m_server.setHandler([](const FakeRequest& request) {
        return FakeJellyfinServer::bytes(request, QByteArray(512 * 1024, 'a'), false);
    });
    const auto pending = prepare({});
    ASSERT_TRUE(pending->waitForReady(5s, {}));
    auto source = pending->takeSource();
    ASSERT_NE(source.device, nullptr);
    EXPECT_FALSE(source.device->seek(400000));
}

TEST_F(JellyfinStreamDeviceTest, DoesNotForwardAuthorizationAcrossOrigins)
{
    FakeJellyfinServer other;
    const QUrl target = other.url(u"/leak"_s);
    m_server.setHandler([target](const FakeRequest&) { return FakeJellyfinServer::redirect(target); });
    const auto pending = prepare({});
    EXPECT_FALSE(pending->waitForReady(5s, {}));
    EXPECT_TRUE(other.requests().empty());
}

TEST_F(JellyfinStreamDeviceTest, PreservesAuthorizationForSameOriginRedirect)
{
    const QUrl target = m_server.url(u"/jf/redirected"_s);
    m_server.setHandler([target](const FakeRequest& request) {
        if(request.path.endsWith("/stream"_L1)) {
            return FakeJellyfinServer::redirect(target);
        }
        return FakeJellyfinServer::bytes(request, "audio", true);
    });
    const auto pending = prepare({});
    ASSERT_TRUE(pending->waitForReady(5s, {}));
    const auto requests = m_server.requestsForPath(u"/jf/redirected"_s);
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(requests.front().header("authorization"), m_authorization);
}

TEST_F(JellyfinStreamDeviceTest, SourceCancellationSurvivesReadyAndTakeAndPreventsReopen)
{
    m_server.setHandler([](const FakeRequest& request) {
        return FakeJellyfinServer::bytes(request, QByteArray(512 * 1024, 'a'), true);
    });
    std::stop_source lifetime;
    const auto pending = prepare(lifetime.get_token());
    ASSERT_TRUE(pending->waitForReady(5s, {}));
    auto source = pending->takeSource();
    ASSERT_NE(source.device, nullptr);
    lifetime.request_stop();
    const auto count = m_server.requests().size();
    EXPECT_FALSE(source.device->seek(400000));
    EXPECT_FALSE(source.device->waitForReadyRead(50));
    source.device->close();
    EXPECT_FALSE(source.device->open(QIODevice::ReadOnly));
    EXPECT_EQ(m_server.requests().size(), count);
}

TEST_F(JellyfinStreamDeviceTest, SourceCancelledBeforePreparationDoesNotSendRequest)
{
    std::stop_source lifetime;
    lifetime.request_stop();
    const auto pending = prepare(lifetime.get_token());
    EXPECT_FALSE(pending->waitForReady(5s, {}));
    EXPECT_TRUE(m_server.requests().empty());
}

TEST_F(JellyfinStreamDeviceTest, SourceCancellationWakesPendingHeaders)
{
    m_server.setHandler([](const FakeRequest&) {
        FakeResponse response = FakeJellyfinServer::text(200, "audio");
        response.delayMs      = 5000;
        return response;
    });
    std::stop_source lifetime;
    const auto pending = prepare(lifetime.get_token());
    ASSERT_TRUE(QTest::qWaitFor([&]() { return !m_server.requests().empty(); }, 1000));
    ASSERT_FALSE(pending->isFinished());
    auto cancellation = std::async(std::launch::async, [&lifetime]() { lifetime.request_stop(); });
    EXPECT_FALSE(pending->waitForReady(5s, {}));
    cancellation.get();
    EXPECT_EQ(pending->errorString(), u"Cancelled"_s);
    EXPECT_FALSE(pending->takeSource().device);
}
TEST_F(JellyfinStreamDeviceTest, ReadySourceCannotBeTransferredAfterLifetimeRevocation)
{
    m_server.setHandler([](const FakeRequest& request) {
        return FakeJellyfinServer::bytes(request, QByteArray(512 * 1024, 'a'), true);
    });
    std::stop_source lifetime;
    const auto pending = prepare(lifetime.get_token());
    ASSERT_TRUE(pending->waitForReady(5s, {}));
    ASSERT_TRUE(pending->isReady());
    lifetime.request_stop();
    EXPECT_FALSE(pending->isReady());
    EXPECT_FALSE(pending->waitForReady(50ms, {}));
    EXPECT_EQ(pending->errorString(), u"Cancelled"_s);
    EXPECT_FALSE(pending->takeSource().device);
}

TEST_F(JellyfinStreamDeviceTest, TakenBufferedSourceDiscardsUnreadBytesAndReportsTerminalCancellation)
{
    m_server.setHandler([](const FakeRequest& request) {
        return FakeJellyfinServer::bytes(request, QByteArray(512 * 1024, 'a'), true);
    });
    std::stop_source lifetime;
    const auto pending = prepare(lifetime.get_token());
    ASSERT_TRUE(pending->waitForReady(5s, {}));
    auto source = pending->takeSource();
    ASSERT_NE(source.device, nullptr);
    ASSERT_EQ(source.device->read(1), "a");
    lifetime.request_stop();
    char byte{};
    EXPECT_EQ(source.device->read(&byte, 1), -1);
    EXPECT_EQ(source.device->errorString(), u"Cancelled"_s);
    EXPECT_FALSE(source.device->open(QIODevice::ReadOnly));
    EXPECT_FALSE(source.device->seek(400000));
}

TEST_F(JellyfinStreamDeviceTest, LifetimeRevocationWakesReadBlockedAfterAuthenticatedHeaders)
{
    m_server.setHandler([](const FakeRequest& request) {
        auto response        = FakeJellyfinServer::bytes(request, QByteArray(512 * 1024, 'a'), true);
        response.bodyDelayMs = 5000;
        return response;
    });
    std::stop_source lifetime;
    const auto pending = prepare(lifetime.get_token());
    ASSERT_TRUE(pending->waitForReady(5s, {}));
    std::promise<void> reading;
    auto read = std::async(std::launch::async, [&]() {
        auto source = pending->takeSource();
        if(!source.device) {
            reading.set_value();
            return std::pair<qint64, QString>{0, {}};
        }
        reading.set_value();
        char byte{};
        const qint64 count = source.device->read(&byte, 1);
        return std::pair{count, source.device->errorString()};
    });
    reading.get_future().wait();
    EXPECT_EQ(read.wait_for(50ms), std::future_status::timeout);
    lifetime.request_stop();
    EXPECT_EQ(read.wait_for(1s), std::future_status::ready);
    const auto result = read.get();
    EXPECT_EQ(result.first, -1);
    EXPECT_EQ(result.second, u"Cancelled"_s);
    ASSERT_FALSE(m_server.requests().empty());
    EXPECT_EQ(m_server.requests().front().header("authorization"), m_authorization);
}

TEST_F(JellyfinStreamDeviceTest, SourceRevocationWakesWaiterEntryWithoutNetworkDispatch)
{
    std::promise<void> entered;
    auto blocked = entered.get_future();
    std::promise<void> release;
    const auto permission = release.get_future().share();
    std::promise<void> exited;
    auto finished = exited.get_future();
    bool released{false};
    const auto releaseNetwork = [&]() {
        if(!released) {
            released = true;
            release.set_value();
            finished.wait();
        }
    };
    ASSERT_TRUE(QMetaObject::invokeMethod(
        m_remoteIoFixture.network.get(),
        [&]() {
            entered.set_value();
            permission.wait();
            exited.set_value();
        },
        Qt::QueuedConnection));
    const auto cleanup = qScopeGuard(releaseNetwork);
    ASSERT_EQ(blocked.wait_for(1s), std::future_status::ready);
    for(int iteration{0}; iteration < 128; ++iteration) {
        std::stop_source lifetime;
        const auto pending = prepare(lifetime.get_token());
        std::barrier waiterEntry{2};
        auto waiter = std::async(std::launch::async, [&]() {
            waiterEntry.arrive_and_wait();
            return pending->waitForReady(10s, {});
        });
        waiterEntry.arrive_and_wait();
        lifetime.request_stop();
        if(waiter.wait_for(250ms) != std::future_status::ready) {
            releaseNetwork();
            ADD_FAILURE() << "Source revocation depended on network dispatch at waiter entry " << iteration;
            EXPECT_FALSE(waiter.get());
            return;
        }
        EXPECT_FALSE(waiter.get());
        EXPECT_FALSE(pending->isReady());
        EXPECT_FALSE(pending->takeSource().device);
        EXPECT_EQ(pending->errorString(), u"Cancelled"_s);
    }
    EXPECT_TRUE(m_server.requests().empty());
}

TEST_F(JellyfinStreamDeviceTest, RejectsUntrustedHttpsCertificateBeforeSendingAuthorization)
{
    QFile certificateFile{QString::fromUtf8(JELLYFIN_TEST_CERT_DIR) + u"/certificate.pem"_s};
    QFile keyFile{QString::fromUtf8(JELLYFIN_TEST_CERT_DIR) + u"/key.pem"_s};
    ASSERT_TRUE(certificateFile.open(QIODevice::ReadOnly));
    ASSERT_TRUE(keyFile.open(QIODevice::ReadOnly));
    FakeJellyfinServer encrypted{QSslCertificate{certificateFile.readAll()}, QSslKey{keyFile.readAll(), QSsl::Rsa}};
    encrypted.setHandler([](const FakeRequest& request) { return FakeJellyfinServer::bytes(request, "audio", true); });
    const auto pending = m_remoteIoFixture.remoteIo->prepareStream(
        apiRequest(encrypted.url(u"/jf/Audio/track1/stream"_s), m_authorization), {});
    EXPECT_FALSE(pending->waitForReady(5s, {}));
    EXPECT_TRUE(encrypted.requests().empty());
}

TEST_F(JellyfinStreamDeviceTest, AcceptsExplicitValidHttpsTrustAndPreservesAuthenticatedRequest)
{
    QFile certificateFile{QString::fromUtf8(JELLYFIN_TEST_CERT_DIR) + u"/certificate.pem"_s};
    QFile keyFile{QString::fromUtf8(JELLYFIN_TEST_CERT_DIR) + u"/key.pem"_s};
    ASSERT_TRUE(certificateFile.open(QIODevice::ReadOnly));
    ASSERT_TRUE(keyFile.open(QIODevice::ReadOnly));
    const QSslCertificate certificate{certificateFile.readAll()};
    FakeJellyfinServer encrypted{certificate, QSslKey{keyFile.readAll(), QSsl::Rsa}};
    encrypted.setHandler([](const FakeRequest& request) { return FakeJellyfinServer::bytes(request, "audio", true); });
    auto request       = apiRequest(encrypted.url(u"/jf/Audio/track1/stream"_s), m_authorization);
    auto configuration = QSslConfiguration::defaultConfiguration();
    configuration.setCaCertificates({certificate});
    configuration.setPeerVerifyMode(QSslSocket::VerifyPeer);
    request.setSslConfiguration(configuration);
    const auto pending = m_remoteIoFixture.remoteIo->prepareStream(request, {});
    ASSERT_TRUE(pending->waitForReady(5s, {}));
    auto source = pending->takeSource();
    ASSERT_NE(source.device, nullptr);
    EXPECT_EQ(readBytes(*source.device, 5), "audio");
    const auto requests = encrypted.requests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(requests.front().header("authorization"), m_authorization);
}

TEST_F(JellyfinStreamDeviceTest, RejectsHttpsDowngradeWithoutForwardingAuthorization)
{
    QFile certificateFile{QString::fromUtf8(JELLYFIN_TEST_CERT_DIR) + u"/certificate.pem"_s};
    QFile keyFile{QString::fromUtf8(JELLYFIN_TEST_CERT_DIR) + u"/key.pem"_s};
    ASSERT_TRUE(certificateFile.open(QIODevice::ReadOnly));
    ASSERT_TRUE(keyFile.open(QIODevice::ReadOnly));
    const QSslCertificate certificate{certificateFile.readAll()};
    FakeJellyfinServer encrypted{certificate, QSslKey{keyFile.readAll(), QSsl::Rsa}};
    const QUrl target = m_server.url(u"/leak"_s);
    encrypted.setHandler([target](const FakeRequest&) { return FakeJellyfinServer::redirect(target); });
    auto request       = apiRequest(encrypted.url(u"/jf/Audio/track1/stream"_s), m_authorization);
    auto configuration = QSslConfiguration::defaultConfiguration();
    configuration.setCaCertificates({certificate});
    configuration.setPeerVerifyMode(QSslSocket::VerifyPeer);
    request.setSslConfiguration(configuration);
    const auto pending = m_remoteIoFixture.remoteIo->prepareStream(request, {});
    EXPECT_FALSE(pending->waitForReady(5s, {}));
    EXPECT_TRUE(m_server.requests().empty());
}
} // namespace Fooyin::Jellyfin::Testing
