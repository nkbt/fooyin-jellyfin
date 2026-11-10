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

#include "jellyfininput.h"
#include "fakejellyfinserver.h"
#include "jellyfinaccess.h"
#include "jellyfintrack.h"
#include "syntheticnetworkaccessmanager.h"
#include "testsupport.h"

#include <core/engine/audioformat.h>
#include <core/engine/audioloader.h>

#include <QJsonObject>

#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QHostAddress>
#include <QScopeGuard>
#include <QTcpServer>
#include <QTcpSocket>
#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace Fooyin::Jellyfin::Testing {
namespace {
const QString Token = u"synthetic-access-token"_s;
const QByteArray TokenField{"Token=\"synthetic-access-token\""};

struct InnerDecoderLog
{
    std::mutex mutex;
    int created{0};
    QByteArray header;
    QByteArray afterSeek;
    bool remoteFieldsPresent{false};
};

//! Stands in for the host FFmpeg decoder and reads the device it receives.
class RecordingDecoder : public AudioDecoder
{
public:
    explicit RecordingDecoder(std::shared_ptr<InnerDecoderLog> log)
        : m_log{std::move(log)}
    { }

    [[nodiscard]] QStringList extensions() const override
    {
        return {u"flac"_s};
    }

    [[nodiscard]] bool isSeekable() const override
    {
        return true;
    }

    std::optional<AudioFormat> init(const AudioSource& source, const Track& /*track*/,
                                    DecoderOptions /*options*/) override
    {
        m_device = source.device;
        if(!m_device) {
            return {};
        }
        const QByteArray header = m_device->read(16);
        const std::scoped_lock lock{m_log->mutex};
        m_log->header              = header;
        m_log->remoteFieldsPresent = source.remoteStreamDevice != nullptr && source.remoteOpenTimeout.count() > 0;
        return AudioFormat{SampleFormat::S16, 44100, 2};
    }

    void stop() override
    {
        m_device = nullptr;
    }

    void seek(uint64_t pos) override
    {
        if(m_device && m_device->seek(static_cast<qint64>(pos) * 100)) {
            const QByteArray data = m_device->read(32);
            const std::scoped_lock lock{m_log->mutex};
            m_log->afterSeek = data;
        }
    }

    AudioBuffer readBuffer(size_t /*bytes*/) override
    {
        return {};
    }

private:
    std::shared_ptr<InnerDecoderLog> m_log;
    QIODevice* m_device{nullptr};
};

class BlockingDecoder : public AudioDecoder
{
public:
    BlockingDecoder(std::shared_ptr<std::promise<void>> entered, std::shared_ptr<std::atomic<int>> destructions)
        : m_entered{std::move(entered)}
        , m_destructions{std::move(destructions)}
    { }
    ~BlockingDecoder() override
    {
        ++*m_destructions;
    }
    QStringList extensions() const override
    {
        return {};
    }
    bool isSeekable() const override
    {
        return false;
    }
    std::optional<AudioFormat> init(const AudioSource&, const Track&, DecoderOptions) override
    {
        m_entered->set_value();
        std::mutex mutex;
        std::condition_variable_any ready;
        std::unique_lock lock{mutex};
        ready.wait(lock, abortToken(), []() { return false; });
        return {};
    }
    void stop() override { }
    void seek(uint64_t) override { }
    AudioBuffer readBuffer(size_t) override
    {
        return {};
    }

private:
    std::shared_ptr<std::promise<void>> m_entered;
    std::shared_ptr<std::atomic<int>> m_destructions;
};

class BorrowedInterruptDecoder final : public BlockingDecoder
{
public:
    BorrowedInterruptDecoder(std::shared_ptr<std::promise<void>> initialised,
                             std::shared_ptr<std::atomic<int>> destructions,
                             std::shared_ptr<std::promise<void>> borrowed, std::shared_future<void> release)
        : BlockingDecoder{std::move(initialised), std::move(destructions)}
        , m_borrowed{std::move(borrowed)}
        , m_release{std::move(release)}
    { }

protected:
    void interruptRead() override
    {
        if(!m_reported.exchange(true)) {
            m_borrowed->set_value();
        }
        m_release.wait();
    }

private:
    std::shared_ptr<std::promise<void>> m_borrowed;
    std::shared_future<void> m_release;
    std::atomic<bool> m_reported{false};
};

class DecoderRaceServer
{
public:
    DecoderRaceServer()
    {
        std::promise<quint16> listening;
        auto ready = listening.get_future();
        m_thread   = std::jthread([this, listening = std::move(listening)](std::stop_token stop) mutable {
            QTcpServer server;
            if(!server.listen(QHostAddress::LocalHost, 0)) {
                listening.set_value(0);
                return;
            }
            listening.set_value(server.serverPort());
            while(!stop.stop_requested()) {
                if(!server.waitForNewConnection(50)) {
                    continue;
                }
                std::unique_ptr<QTcpSocket> socket{server.nextPendingConnection()};
                if(!socket) {
                    continue;
                }
                QByteArray request;
                while(!stop.stop_requested() && !request.contains("\r\n\r\n")) {
                    if(socket->bytesAvailable() == 0 && !socket->waitForReadyRead(50)) {
                        continue;
                    }
                    request += socket->readAll();
                }
                if(stop.stop_requested()) {
                    break;
                }
                {
                    const std::scoped_lock lock{m_mutex};
                    m_request = request;
                }
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: 16\r\nAccept-Ranges: bytes\r\nConnection: "
                              "close\r\n\r\nsynthetic-audio!");
                while(!stop.stop_requested() && socket->bytesToWrite() > 0) {
                    socket->waitForBytesWritten(50);
                }
                socket->disconnectFromHost();
            }
        });
        m_port     = ready.get();
    }
    ~DecoderRaceServer()
    {
        m_thread.request_stop();
        m_thread.join();
    }
    QUrl url() const
    {
        return QUrl{u"http://127.0.0.1:%1/jf"_s.arg(m_port)};
    }
    QByteArray request() const
    {
        const std::scoped_lock lock{m_mutex};
        return m_request;
    }
    quint16 port() const
    {
        return m_port;
    }

private:
    mutable std::mutex m_mutex;
    QByteArray m_request;
    quint16 m_port{0};
    std::jthread m_thread;
};

QByteArray syntheticAudio(qsizetype size)
{
    QByteArray data;
    data.reserve(size);
    for(qsizetype index = 0; index < size; ++index) {
        data.append(static_cast<char>((index * 13 + index / 509) % 256));
    }
    return data;
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

class JellyfinInputTest : public testing::Test
{
protected:
    JellyfinInputTest()
        : m_access{std::make_shared<AccessRegistry>()}
        , m_loader{std::make_shared<AudioLoader>()}
        , m_log{std::make_shared<InnerDecoderLog>()}
        , m_data{syntheticAudio(500 * 1024)}
    {
        const QByteArray data = m_data;
        m_server.setHandler([data](const FakeRequest& request) {
            if(request.path == "/jf/Audio/track1/stream"_L1) {
                return FakeJellyfinServer::bytes(request, data, true);
            }
            if(request.path == "/jf/Items/album1/Images/Primary"_L1
               && request.query.queryItemValue(u"tag"_s) == "tag1"_L1) {
                return FakeJellyfinServer::bytes(request, "synthetic-image", false);
            }
            return FakeJellyfinServer::text(404, "Not found");
        });
        m_access->setAccess(connectionFor(m_server.url(u"/jf"_s), u"user1"_s), Token);
    }

    void addInnerDecoder()
    {
        auto log = m_log;
        m_loader->addDecoder(u"FFmpeg"_s, [log]() {
            {
                const std::scoped_lock lock{log->mutex};
                ++log->created;
            }
            return std::make_unique<RecordingDecoder>(log);
        });
    }

    [[nodiscard]] QString key() const
    {
        return m_access->access()->sourceKey;
    }

    RemoteIoFixture m_remoteIoFixture;
    FakeJellyfinServer m_server;
    std::shared_ptr<AccessRegistry> m_access;
    std::shared_ptr<AudioLoader> m_loader;
    std::shared_ptr<InnerDecoderLog> m_log;
    QByteArray m_data;
};

TEST_F(JellyfinInputTest, StreamsOnDemandWithCurrentCredentials)
{
    addInnerDecoder();
    Decoder decoder{m_access, m_loader, m_remoteIoFixture.remoteIo, 5s};
    EXPECT_EQ(decoder.supportedSchemes(), QStringList{u"jellyfin"_s});

    const QString path = trackPath(key(), u"track1"_s);
    EXPECT_FALSE(path.contains(Token));
    EXPECT_TRUE(m_server.requests().empty());

    AudioSource source;
    source.filepath   = path;
    const auto format = decoder.init(source, Track{path}, AudioDecoder::None);
    ASSERT_TRUE(format.has_value());
    EXPECT_EQ(format->sampleRate(), 44100);
    EXPECT_TRUE(decoder.isSeekable());

    decoder.seek(4000);
    {
        const std::scoped_lock lock{m_log->mutex};
        EXPECT_EQ(m_log->header, m_data.first(16));
        EXPECT_TRUE(m_log->remoteFieldsPresent);
        EXPECT_EQ(m_log->afterSeek, m_data.sliced(400000, 32));
    }
    decoder.stop();

    const auto requests = m_server.requests();
    ASSERT_FALSE(requests.empty());
    for(const auto& request : requests) {
        EXPECT_EQ(request.method, "GET");
        EXPECT_EQ(request.path, u"/jf/Audio/track1/stream"_s);
        EXPECT_EQ(request.query.queryItemValue(u"static"_s), u"true"_s);
        EXPECT_FALSE(request.query.hasQueryItem(u"api_key"_s));
        EXPECT_TRUE(request.header("authorization").contains(TokenField));
        EXPECT_FALSE(request.header("authorization").isEmpty());
    }
}

TEST_F(JellyfinInputTest, SignedOutTrackDoesNotRequestAudio)
{
    addInnerDecoder();
    const int registeredDecoders = m_log->created;
    const QString path           = trackPath(key(), u"track1"_s);
    m_access->clearAccess();

    Decoder decoder{m_access, m_loader, m_remoteIoFixture.remoteIo, 5s};
    AudioSource source;
    source.filepath = path;
    EXPECT_FALSE(decoder.init(source, Track{path}, AudioDecoder::None).has_value());
    EXPECT_TRUE(m_server.requests().empty());
    EXPECT_EQ(m_log->created, registeredDecoders);
}

TEST_F(JellyfinInputTest, TrackOfAnotherAccountDoesNotUseCurrentCredentials)
{
    addInnerDecoder();
    const QString path = trackPath(key(), u"track1"_s);
    m_access->setAccess(connectionFor(m_server.url(u"/jf"_s), u"user2"_s), Token);

    Decoder decoder{m_access, m_loader, m_remoteIoFixture.remoteIo, 5s};
    AudioSource source;
    source.filepath = path;
    EXPECT_FALSE(decoder.init(source, Track{path}, AudioDecoder::None).has_value());
    EXPECT_TRUE(m_server.requests().empty());
}

TEST_F(JellyfinInputTest, MissingHostDecoderDoesNotRequestAudio)
{
    Decoder decoder{m_access, m_loader, m_remoteIoFixture.remoteIo, 5s};
    const QString path = trackPath(key(), u"track1"_s);
    AudioSource source;
    source.filepath = path;
    EXPECT_FALSE(decoder.init(source, Track{path}, AudioDecoder::None).has_value());
    EXPECT_TRUE(m_server.requests().empty());
}

TEST_F(JellyfinInputTest, ReaderSuppliesCoverFromCatalogImage)
{
    const QString path = trackPath(key(), u"track1"_s);
    m_access->setImages(key(), {{u"track1"_s, CatalogImage{.itemId = u"album1"_s, .tag = u"tag1"_s}}});

    Reader reader{m_access, m_remoteIoFixture.remoteIo, 5s};
    AudioSource source;
    source.filepath = path;
    Track track{path};
    EXPECT_FALSE(reader.readTrack(source, track));
    EXPECT_FALSE(reader.canWriteMetaData());
    EXPECT_TRUE(reader.readCover(source, track, Track::Cover::Back).isEmpty());
    EXPECT_TRUE(m_server.requests().empty());

    EXPECT_EQ(reader.readCover(source, track, Track::Cover::Front), QByteArray{"synthetic-image"});
    const auto requests = m_server.requestsForPath(u"/jf/Items/album1/Images/Primary"_s);
    ASSERT_FALSE(requests.empty());
    EXPECT_TRUE(requests.front().header("authorization").contains(TokenField));
    EXPECT_TRUE(m_server.requestsForPath(u"/jf/Audio/track1/stream"_s).empty());
}

TEST_F(JellyfinInputTest, ReaderWithoutImageDoesNotRequest)
{
    const QString path = trackPath(key(), u"track2"_s);
    Reader reader{m_access, m_remoteIoFixture.remoteIo, 5s};
    AudioSource source;
    source.filepath = path;
    EXPECT_TRUE(reader.readCover(source, Track{path}, Track::Cover::Front).isEmpty());
    EXPECT_TRUE(m_server.requests().empty());
}

TEST(JellyfinInputUrlTest, BuildsUrlsBelowBasePath)
{
    const QUrl serverUrl{u"https://media.example.com/jellyfin"_s};
    EXPECT_EQ(streamUrl(serverUrl, u"track1"_s).toString(),
              u"https://media.example.com/jellyfin/Audio/track1/stream?static=true"_s);
    EXPECT_TRUE(streamUrl(serverUrl, u"../track1"_s).isEmpty());
    EXPECT_EQ(imageUrl(serverUrl, {.itemId = u"album1"_s, .tag = u"a b"_s}).toString(QUrl::FullyEncoded),
              u"https://media.example.com/jellyfin/Items/album1/Images/Primary?tag=a%20b"_s);
}
TEST_F(JellyfinInputTest, AbortDuringInnerCreationDoesNotRaceOwnershipOrStartAudio)
{
    auto entered  = std::make_shared<std::promise<void>>();
    auto creation = entered->get_future();
    std::promise<void> release;
    const auto permission = release.get_future().share();
    auto initialisation   = std::make_shared<std::promise<void>>();
    auto destructions     = std::make_shared<std::atomic<int>>(0);
    auto armed            = std::make_shared<std::atomic<bool>>(false);
    m_loader->addDecoder(u"FFmpeg"_s, [entered, permission, initialisation, destructions, armed]() {
        if(armed->load()) {
            entered->set_value();
            permission.wait();
        }
        return std::make_unique<BlockingDecoder>(initialisation, destructions);
    });
    destructions->store(0);
    Decoder decoder{m_access, m_loader, m_remoteIoFixture.remoteIo, 5s};
    AudioSource source;
    source.filepath = trackPath(key(), u"track1"_s);
    armed->store(true);
    auto result = std::async(std::launch::async,
                             [&]() { return decoder.init(source, Track{source.filepath}, AudioDecoder::None); });
    EXPECT_EQ(creation.wait_for(5s), std::future_status::ready);
    QElapsedTimer timer;
    timer.start();
    decoder.requestAbort();
    EXPECT_LT(timer.elapsed(), 500);
    release.set_value();
    ASSERT_EQ(result.wait_for(5s), std::future_status::ready);
    EXPECT_FALSE(result.get());
    EXPECT_EQ(destructions->load(), 1);
    EXPECT_TRUE(m_server.requests().empty());
}

TEST(JellyfinDecoderRace, AbortRemainsSafeAcrossInnerPublicationAndFailureCleanup)
{
    auto access = std::make_shared<AccessRegistry>();
    access->setAccess(connectionFor(QUrl{u"https://synthetic.invalid/jf"_s}, u"user1"_s), Token);
    auto loader   = std::make_shared<AudioLoader>();
    auto entered  = std::make_shared<std::promise<void>>();
    auto creation = entered->get_future();
    std::promise<void> release;
    const auto permission = release.get_future().share();
    auto initialisation   = std::make_shared<std::promise<void>>();
    auto destructions     = std::make_shared<std::atomic<int>>(0);
    auto armed            = std::make_shared<std::atomic<bool>>(false);
    loader->addDecoder(u"FFmpeg"_s, [entered, permission, initialisation, destructions, armed]() {
        if(armed->load()) {
            entered->set_value();
            permission.wait();
        }
        return std::make_unique<BlockingDecoder>(initialisation, destructions);
    });
    destructions->store(0);
    Decoder decoder{access, loader, {}, 5s};
    AudioSource source;
    source.filepath = trackPath(access->access()->sourceKey, u"track1"_s);
    armed->store(true);
    auto result = std::async(std::launch::async,
                             [&]() { return decoder.init(source, Track{source.filepath}, AudioDecoder::None); });
    EXPECT_EQ(creation.wait_for(5s), std::future_status::ready);
    decoder.requestAbort();
    std::atomic<int> abortCalls{0};
    std::jthread aborting{[&](std::stop_token stop) {
        while(!stop.stop_requested()) {
            decoder.requestAbort();
            ++abortCalls;
            std::this_thread::yield();
        }
    }};
    while(abortCalls.load() == 0) {
        std::this_thread::yield();
    }
    release.set_value();
    EXPECT_EQ(result.wait_for(5s), std::future_status::ready);
    EXPECT_FALSE(result.get());
    aborting.request_stop();
    aborting.join();
    EXPECT_GT(abortCalls.load(), 0);
    EXPECT_EQ(destructions->load(), 1);
}

TEST(JellyfinDecoderRace, AuthenticatedNativeStreamInitAbortPreservesInnerLifetime)
{
    DecoderRaceServer server;
    ASSERT_NE(server.port(), 0);
    RemoteIoFixture remoteIo;
    auto access = std::make_shared<AccessRegistry>();
    access->setAccess(connectionFor(server.url(), u"user1"_s), Token);
    auto loader         = std::make_shared<AudioLoader>();
    auto entered        = std::make_shared<std::promise<void>>();
    auto initialisation = entered->get_future();
    auto destructions   = std::make_shared<std::atomic<int>>(0);
    loader->addDecoder(u"FFmpeg"_s,
                       [entered, destructions]() { return std::make_unique<BlockingDecoder>(entered, destructions); });
    destructions->store(0);
    Decoder decoder{access, loader, remoteIo.remoteIo, 5s};
    AudioSource source;
    source.filepath = trackPath(access->access()->sourceKey, u"track1"_s);
    auto result     = std::async(std::launch::async,
                                 [&]() { return decoder.init(source, Track{source.filepath}, AudioDecoder::None); });
    EXPECT_EQ(initialisation.wait_for(5s), std::future_status::ready);
    QElapsedTimer timer;
    timer.start();
    decoder.requestAbort();
    EXPECT_LT(timer.elapsed(), 500);
    EXPECT_EQ(result.wait_for(5s), std::future_status::ready);
    EXPECT_FALSE(result.get());
    EXPECT_EQ(destructions->load(), 1);
    EXPECT_TRUE(server.request().startsWith("GET /jf/Audio/track1/stream?static=true HTTP/1.1"));
    EXPECT_TRUE(server.request().contains(TokenField));
}

TEST(JellyfinDecoderRace, PreparedReplyInitAbortPreservesInnerLifetime)
{
    auto* manager = new SyntheticNetworkAccessManager(nullptr);
    RemoteIoFixture remoteIo{std::shared_ptr<QNetworkAccessManager>{
        manager, [](QNetworkAccessManager* network) { network->deleteLater(); }}};
    auto access = std::make_shared<AccessRegistry>();
    access->setAccess(connectionFor(QUrl{u"https://synthetic.invalid/jf"_s}, u"user1"_s), Token);
    auto loader         = std::make_shared<AudioLoader>();
    auto entered        = std::make_shared<std::promise<void>>();
    auto initialisation = entered->get_future();
    auto destructions   = std::make_shared<std::atomic<int>>(0);
    loader->addDecoder(u"FFmpeg"_s,
                       [entered, destructions]() { return std::make_unique<BlockingDecoder>(entered, destructions); });
    destructions->store(0);
    Decoder decoder{access, loader, remoteIo.remoteIo, 5s};
    AudioSource source;
    source.filepath = trackPath(access->access()->sourceKey, u"track1"_s);
    auto result     = std::async(std::launch::async,
                                 [&]() { return decoder.init(source, Track{source.filepath}, AudioDecoder::None); });
    EXPECT_EQ(initialisation.wait_for(5s), std::future_status::ready);
    decoder.requestAbort();
    EXPECT_EQ(result.wait_for(5s), std::future_status::ready);
    EXPECT_FALSE(result.get());
    EXPECT_EQ(destructions->load(), 1);
    EXPECT_TRUE(manager->streamRequestAuthenticated.load());
}

TEST(JellyfinDecoderRace, BorrowedInterruptOutlivesOuterCleanup)
{
    auto* manager = new SyntheticNetworkAccessManager(nullptr);
    RemoteIoFixture remoteIo{std::shared_ptr<QNetworkAccessManager>{
        manager, [](QNetworkAccessManager* network) { network->deleteLater(); }}};
    auto access = std::make_shared<AccessRegistry>();
    access->setAccess(connectionFor(QUrl{u"https://synthetic.invalid/jf"_s}, u"user1"_s), Token);
    auto loader         = std::make_shared<AudioLoader>();
    auto entered        = std::make_shared<std::promise<void>>();
    auto initialisation = entered->get_future();
    auto borrowed       = std::make_shared<std::promise<void>>();
    auto borrowing      = borrowed->get_future();
    std::promise<void> permission;
    const auto release = permission.get_future().share();
    auto destructions  = std::make_shared<std::atomic<int>>(0);
    loader->addDecoder(u"FFmpeg"_s, [entered, destructions, borrowed, release]() {
        return std::make_unique<BorrowedInterruptDecoder>(entered, destructions, borrowed, release);
    });
    destructions->store(0);
    Decoder decoder{access, loader, remoteIo.remoteIo, 5s};
    AudioSource source;
    source.filepath  = trackPath(access->access()->sourceKey, u"track1"_s);
    auto initialised = std::async(std::launch::async,
                                  [&]() { return decoder.init(source, Track{source.filepath}, AudioDecoder::None); });
    std::future<void> aborted;
    bool released{false};
    const auto releaseBorrow = [&]() {
        if(!released) {
            released = true;
            permission.set_value();
        }
    };
    const auto cleanup = qScopeGuard([&]() {
        releaseBorrow();
        if(aborted.valid()) {
            aborted.wait();
        }
    });
    EXPECT_EQ(initialisation.wait_for(5s), std::future_status::ready);
    aborted = std::async(std::launch::async, [&]() { decoder.requestAbort(); });
    EXPECT_EQ(borrowing.wait_for(5s), std::future_status::ready);
    EXPECT_EQ(initialised.wait_for(5s), std::future_status::ready);
    EXPECT_FALSE(initialised.get());
    EXPECT_EQ(destructions->load(), 0);
    releaseBorrow();
    aborted.get();
    EXPECT_EQ(destructions->load(), 1);
    EXPECT_TRUE(manager->streamRequestAuthenticated.load());
}

TEST_F(JellyfinInputTest, AbortInterruptsInnerInitialisationAndConcurrentFailureCleanup)
{
    auto entered        = std::make_shared<std::promise<void>>();
    auto initialisation = entered->get_future();
    auto destructions   = std::make_shared<std::atomic<int>>(0);
    m_loader->addDecoder(
        u"FFmpeg"_s, [entered, destructions]() { return std::make_unique<BlockingDecoder>(entered, destructions); });
    destructions->store(0);
    Decoder decoder{m_access, m_loader, m_remoteIoFixture.remoteIo, 5s};
    AudioSource source;
    source.filepath = trackPath(key(), u"track1"_s);
    auto result     = std::async(std::launch::async,
                                 [&]() { return decoder.init(source, Track{source.filepath}, AudioDecoder::None); });
    EXPECT_EQ(initialisation.wait_for(5s), std::future_status::ready);
    QElapsedTimer timer;
    timer.start();
    decoder.requestAbort();
    EXPECT_LT(timer.elapsed(), 500);
    ASSERT_EQ(result.wait_for(5s), std::future_status::ready);
    EXPECT_FALSE(result.get());
    EXPECT_EQ(destructions->load(), 1);
}
} // namespace Fooyin::Jellyfin::Testing
