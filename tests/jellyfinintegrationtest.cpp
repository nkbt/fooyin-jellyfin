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
#include "jellyfinaccess.h"
#include "jellyfincatalogsync.h"
#include "jellyfininput.h"
#include "jellyfinplaybackreporter.h"

#include "core/application.h"
#include "core/engine/input/ffmpeg/ffmpeginput.h"
#include "core/plugins/pluginmanager.h"
#include "gui/filters/filteritem.h"
#include "gui/guiapplication.h"
#include "gui/librarytree/librarytreeitem.h"
#include "gui/playlist/playlistcontroller.h"
#include <core/coresettings.h>
#include <core/engine/audioloader.h>
#include <core/engine/enginecontroller.h>
#include <core/library/musiclibrary.h>
#include <core/player/playercontroller.h>
#include <core/playlist/playlisthandler.h>
#include <core/scripting/scriptregistry.h>
#include <gui/fywidget.h>
#include <gui/guiconstants.h>
#include <gui/layout/fylayout.h>
#include <gui/layout/layoutprovider.h>
#include <gui/playlist/currentplaylistcontroller.h>
#include <gui/playlist/playlisteditcontroller.h>
#include <gui/widgetprovider.h>
#include <gui/widgets/expandedtreeview.h>
#include <utils/actions/actionmanager.h>
#include <utils/actions/command.h>
#include <utils/settings/settingsmanager.h>

#include <QAbstractItemModelTester>
#include <QAbstractItemView>
#include <QAbstractProxyModel>
#include <QAction>
#include <QApplication>
#include <QDataStream>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QTabBar>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <QTimer>
#include <QTreeView>
#include <QtConcurrent/QtConcurrentRun>

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <mutex>
#include <thread>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace Fooyin::Jellyfin::Testing {
namespace {
QByteArray syntheticWave()
{
    QByteArray bytes;
    QDataStream stream{&bytes, QIODevice::WriteOnly};
    stream.setByteOrder(QDataStream::LittleEndian);
    constexpr quint32 frames = 44100 * 30;
    constexpr quint32 length = frames * 4;
    stream.writeRawData("RIFF", 4);
    stream << quint32{36 + length};
    stream.writeRawData("WAVEfmt ", 8);
    stream << quint32{16} << quint16{1} << quint16{2} << quint32{44100} << quint32{44100 * 4} << quint16{4}
           << quint16{16};
    stream.writeRawData("data", 4);
    stream << length;
    bytes.append(QByteArray(length, '\x11'));
    return bytes;
}

class NativeWorkerBarrier
{
public:
    NativeWorkerBarrier()
        : previousMaxThreadCount{QThreadPool::globalInstance()->maxThreadCount()}
    {
        QThreadPool::globalInstance()->waitForDone();
        QThreadPool::globalInstance()->setMaxThreadCount(1);
        std::promise<void> entered;
        auto ready = entered.get_future();
        blocker    = QtConcurrent::run([this, &entered]() {
            entered.set_value();
            std::unique_lock lock{mutex};
            condition.wait(lock, [this]() { return released; });
        });
        ready.wait();
    }
    ~NativeWorkerBarrier()
    {
        release();
        blocker.waitForFinished();
        QThreadPool::globalInstance()->setMaxThreadCount(previousMaxThreadCount);
    }
    void release()
    {
        {
            const std::scoped_lock lock{mutex};
            released = true;
        }
        condition.notify_all();
    }

private:
    int previousMaxThreadCount;
    bool released{false};
    std::mutex mutex;
    std::condition_variable condition;
    QFuture<void> blocker;
};

class SyntheticOutput final : public AudioOutput
{
public:
    bool init(const AudioFormat& format) override
    {
        m_format      = format;
        m_initialised = format.isValid();
        return m_initialised;
    }
    void uninit() override
    {
        m_initialised = false;
    }
    void reset() override { }
    void drain() override { }
    void start() override { }
    bool initialised() const override
    {
        return m_initialised;
    }
    QString device() const override
    {
        return u"default"_s;
    }
    OutputState currentState() override
    {
        return {.freeFrames = 16, .queuedFrames = 0, .delay = 0};
    }
    int bufferSize() const override
    {
        return 256;
    }
    OutputDevices getAllDevices(bool) override
    {
        return {};
    }
    int write(std::span<const std::byte>, int frameCount) override
    {
        std::this_thread::sleep_for(1ms);
        return std::min(frameCount, 16);
    }
    void setPaused(bool) override { }
    void setDevice(const QString&) override { }
    AudioFormat format() const override
    {
        return m_format;
    }

private:
    AudioFormat m_format;
    bool m_initialised{false};
};

QJsonObject item(const QString& itemId)
{
    return {{u"Id"_s, itemId},
            {u"Type"_s, u"Audio"_s},
            {u"Name"_s, u"Synthetic %1"_s.arg(itemId)},
            {u"Artists"_s, QJsonArray{u"Synthetic Artist"_s}},
            {u"Album"_s, u"Synthetic Album"_s},
            {u"Genres"_s, QJsonArray{u"Synthetic Genre"_s}},
            {u"RunTimeTicks"_s, 300000000},
            {u"UserData"_s, QJsonObject{{u"IsFavorite"_s, false}}}};
}
} // namespace

TEST(JellyfinNativeIntegration, CatalogAndRealDecoderControllerUseConfirmedOccurrences)
{
    Application application;
    application.settingsManager()->set<Settings::Core::FirstRun>(false);
    application.pluginManager()->findPlugins(
        {QFileInfo(QString::fromUtf8(JELLYFIN_HOST_CORE_LIBRARY)).dir().filePath(u"plugins"_s)});
    ASSERT_FALSE(application.pluginManager()->allPluginInfo().empty());
    GuiApplication gui{&application};
    QStringList unexpectedDialogs;
    bool expectingBrokenTrack{false};
    QTimer dialogTimer;
    QObject::connect(&dialogTimer, &QTimer::timeout, &gui, [&]() {
        for(QWidget* widget : QApplication::topLevelWidgets()) {
            if(auto* message = qobject_cast<QMessageBox*>(widget)) {
                if(!expectingBrokenTrack) {
                    unexpectedDialogs.push_back(message->text() + u": "_s + message->informativeText());
                }
                std::fprintf(stderr, "native fixture dialog: %s\n", qPrintable(message->text()));
                message->accept();
            }
        }
    });
    dialogTimer.start(10);
    gui.startup();
    auto nativeViews       = std::make_unique<QWidget>();
    auto* nativeLayout     = new QHBoxLayout(nativeViews.get());
    FyWidget* nativeTree   = gui.widgetProvider()->createWidget(u"LibraryTree"_s);
    FyWidget* nativeFilter = gui.widgetProvider()->createWidget(u"LibraryFilter"_s);
    const auto cleanupGui  = qScopeGuard([&]() {
        EXPECT_TRUE(unexpectedDialogs.empty()) << unexpectedDialogs.join(u"; "_s).toStdString();
        nativeViews.reset();
        dialogTimer.stop();
        gui.shutdown();
    });
    ASSERT_NE(nativeTree, nullptr);
    ASSERT_NE(nativeFilter, nullptr);
    nativeLayout->addWidget(nativeTree);
    nativeLayout->addWidget(nativeFilter);
    nativeViews->resize(1024, 640);
    nativeViews->show();
    application.settingsManager()->set<Settings::Core::Internal::EngineFading>(false);
    application.settingsManager()->set<Settings::Core::Internal::EngineCrossfading>(false);
    application.settingsManager()->set<Settings::Core::GaplessPlayback>(false);
    application.engine()->addOutput(u"Synthetic"_s, []() { return std::make_unique<SyntheticOutput>(); });
    application.settingsManager()->set<Settings::Core::AudioOutput>(u"Synthetic|default"_s);
    application.audioLoader()->addDecoder(u"FFmpeg"_s, []() { return std::make_unique<FFmpegDecoder>(); });
    FakeJellyfinServer server;
    const QByteArray wave  = syntheticWave();
    auto rejectFirstReport = std::make_shared<std::atomic<bool>>(true);
    server.setHandler([wave, rejectFirstReport](const FakeRequest& request) {
        if(request.path == "/jf/Items"_L1) {
            const QJsonArray items = request.query.queryItemValue(u"includeItemTypes"_s) == "Audio"_L1
                                       ? QJsonArray{item(u"track1"_s), item(u"track2"_s), item(u"broken"_s)}
                                       : QJsonArray{};
            return FakeJellyfinServer::json(QJsonObject{{u"Items"_s, items}, {u"TotalRecordCount"_s, items.size()}});
        }
        if(request.path.startsWith("/jf/Sessions/Playing"_L1)) {
            if(request.path == "/jf/Sessions/Playing"_L1 && rejectFirstReport->exchange(false)) {
                return FakeJellyfinServer::text(500, {});
            }
            return FakeJellyfinServer::text(204, {});
        }
        if(request.path == "/jf/Audio/broken/stream"_L1) {
            return FakeJellyfinServer::text(500, {});
        }
        if(request.path.startsWith("/jf/Audio/"_L1)) {
            return FakeJellyfinServer::bytes(request, wave, true);
        }
        return FakeJellyfinServer::text(404, {});
    });
    auto access = std::make_shared<AccessRegistry>();
    access->setAccess({.serverUrl = server.url(u"/jf"_s),
                       .serverId  = u"server"_s,
                       .userId    = u"account"_s,
                       .userName  = u"Synthetic"_s,
                       .deviceId  = u"device"_s},
                      u"synthetic-token"_s);
    application.audioLoader()->addDecoder(
        u"Jellyfin"_s,
        [access, loader = application.audioLoader(), remoteIo = application.remoteIoService()]() {
            return std::make_unique<Decoder>(access, loader, remoteIo, 5s);
        },
        0);
    QNetworkAccessManager network;
    CatalogSync sync{&network, application.library(), access, nullptr};
    QSignalSpy synchronised{&sync, &CatalogSync::finished};
    sync.start(u"Synthetic source"_s);
    ASSERT_TRUE(QTest::qWaitFor([&]() { return !synchronised.isEmpty(); }, 10000));
    ASSERT_TRUE(synchronised.front().at(0).toBool());
    ASSERT_EQ(application.library()->visibleLibraryTracks().size(), 3U);
    auto* treeView = nativeTree->findChild<QTreeView*>();
    ASSERT_NE(treeView, nullptr);
    const auto treeHasTracks = [&]() {
        for(int row{0}; treeView->model() && row < treeView->model()->rowCount(); ++row) {
            if(treeView->model()->index(row, 0).data(LibraryTreeItem::TrackCount).toInt() > 0) {
                return true;
            }
        }
        return false;
    };
    const auto filterHasTracks = [&]() {
        const auto views = nativeFilter->findChildren<QAbstractItemView*>();
        for(const auto* view : views) {
            for(int row{0}; view->model() && row < view->model()->rowCount(); ++row) {
                if(!view->model()->index(row, 0).data(Filters::FilterItem::TrackIdsRole).value<TrackIds>().empty()) {
                    return true;
                }
            }
        }
        return false;
    };
    ASSERT_TRUE(QTest::qWaitFor([&]() { return treeHasTracks() && filterHasTracks(); }, 10000));
    treeView->expandAll();
    const QString screenshotDirectory = qEnvironmentVariable("JELLYFIN_SCREENSHOT_DIR");
    if(!screenshotDirectory.isEmpty()) {
        ASSERT_TRUE(nativeViews->grab().save(screenshotDirectory + u"/native-library-filter-wide.png"_s));
        nativeViews->resize(480, 640);
        ASSERT_TRUE(nativeViews->grab().save(screenshotDirectory + u"/native-library-filter-narrow.png"_s));
        nativeViews->resize(1024, 640);
    }
    EXPECT_TRUE(server.requestsForPath(u"/jf/Audio/track1/stream"_s).empty());
    Track track1;
    Track track2;
    Track broken;
    for(const Track& track : application.library()->libraryTracks()) {
        if(itemIdFromPath(track.filepath()) == "track1"_L1) {
            track1 = track;
        }
        if(itemIdFromPath(track.filepath()) == "track2"_L1) {
            track2 = track;
        }
        if(itemIdFromPath(track.filepath()) == "broken"_L1) {
            broken = track;
        }
    }
    ASSERT_GE(track1.id(), 0);
    ASSERT_GE(track2.id(), 0);
    ASSERT_GE(broken.id(), 0);
    const auto sourceLibrary = application.library()->libraryForPath(track1.filepath());
    ASSERT_TRUE(sourceLibrary);
    Playlist* mirror = application.playlistHandler()->applyExternalPlaylist(
        sourceLibrary->path, u"native-source-playlist"_s, {}, u"Native read-only source playlist"_s,
        PlaylistTrack::fromTracks({track1, track2}, {}));
    ASSERT_NE(mirror, nullptr);
    CurrentPlaylistController* playlistController  = gui.playlistController();
    PlaylistEditController* playlistEditController = gui.playlistController();
    ASSERT_NE(playlistController, nullptr);
    ASSERT_NE(playlistEditController, nullptr);
    playlistController->changeCurrentPlaylist(mirror->id());
    EXPECT_EQ(playlistController->currentPlaylist(), mirror);
    EXPECT_FALSE(playlistEditController->insertPlaylistItems(mirror->id(), 0, {track1}));
    EXPECT_FALSE(playlistEditController->replacePlaylistItem(mirror->id(), 0, {track2}));
    EXPECT_FALSE(playlistEditController->removePlaylistItems(mirror->id(), {0}));
    EXPECT_FALSE(playlistEditController->movePlaylistItems(mirror->id(), {0}, 1));
    EXPECT_FALSE(playlistEditController->reorderPlaylistItems(mirror->id(), {1, 0}));
    EXPECT_FALSE(playlistEditController->clearPlaylist(mirror->id()));
    EXPECT_EQ(mirror->trackCount(), 2);
    PlaybackReporter reporter{&network, access, nullptr};
    QSignalSpy reportFinished{&reporter, &PlaybackReporter::reportFinished};
    QObject::connect(application.engine(), &EngineController::playbackOccurrenceStarted, &reporter,
                     &PlaybackReporter::occurrenceStarted);
    QObject::connect(application.engine(), &EngineController::playbackOccurrenceUpdated, &reporter,
                     &PlaybackReporter::occurrenceUpdated);
    QObject::connect(application.engine(), &EngineController::playbackOccurrenceEnded, &reporter,
                     &PlaybackReporter::occurrenceEnded);
    QSignalSpy starts{application.engine(), &EngineController::playbackOccurrenceStarted};
    QSignalSpy stops{application.engine(), &EngineController::playbackOccurrenceEnded};
    PlayerController* player = application.playerController();
    Playlist* first          = application.playlistHandler()->createNewPlaylist(u"First"_s, {track1});
    std::fputs("native fixture: play first\n", stderr);
    player->startPlayback(first);
    ASSERT_TRUE(QTest::qWaitFor([&]() { return starts.size() == 1; }, 10000));
    const auto confirmed = starts.front().at(0).value<Engine::PlaybackOccurrenceContext>();
    EXPECT_EQ(confirmed.sourceGeneration, access->access()->sourceGeneration);
    EXPECT_EQ(confirmed.track.filepath(), track1.filepath());
    ASSERT_TRUE(QTest::qWaitFor([&]() { return server.requestsForPath(u"/jf/Sessions/Playing"_s).size() == 1; }, 5000));
    ASSERT_TRUE(QTest::qWaitFor([&]() { return !reportFinished.isEmpty(); }, 5000));
    EXPECT_FALSE(reportFinished.front().at(1).toBool());
    EXPECT_EQ(application.engine()->engineState(), Engine::PlaybackState::Playing);
    std::fputs("native fixture: pause and seek\n", stderr);
    player->pause();
    ASSERT_TRUE(
        QTest::qWaitFor([&]() { return application.engine()->engineState() == Engine::PlaybackState::Paused; }, 5000));
    player->seek(4000);
    ASSERT_TRUE(QTest::qWaitFor([&]() { return player->currentPosition() >= 4000; }, 5000));
    player->play();
    ASSERT_TRUE(
        QTest::qWaitFor([&]() { return application.engine()->engineState() == Engine::PlaybackState::Playing; }, 5000));
    EXPECT_EQ(starts.size(), 1);
    player->stop();
    ASSERT_TRUE(QTest::qWaitFor([&]() { return stops.size() == 1; }, 5000));
    EXPECT_GE(stops.front().front().value<Engine::PlaybackOccurrenceContext>().positionMs, 4000U);
    ASSERT_TRUE(
        QTest::qWaitFor([&]() { return server.requestsForPath(u"/jf/Sessions/Playing/Stopped"_s).size() == 1; }, 5000));
    const auto progress = server.requestsForPath(u"/jf/Sessions/Playing/Progress"_s);
    EXPECT_TRUE(std::ranges::any_of(progress, [](const FakeRequest& request) {
        return QJsonDocument::fromJson(request.body).object().value(u"IsPaused"_s).toBool();
    }));
    EXPECT_TRUE(std::ranges::any_of(progress, [](const FakeRequest& request) {
        return QJsonDocument::fromJson(request.body).object().value(u"PositionTicks"_s).toInteger() >= 40000000;
    }));
    const auto firstStopped
        = QJsonDocument::fromJson(server.requestsForPath(u"/jf/Sessions/Playing/Stopped"_s).front().body).object();
    EXPECT_GE(firstStopped.value(u"PositionTicks"_s).toInteger(), 40000000);
    const auto firstStarted
        = QJsonDocument::fromJson(server.requestsForPath(u"/jf/Sessions/Playing"_s).front().body).object();
    EXPECT_TRUE(firstStarted.value(u"CanSeek"_s).toBool());
    EXPECT_EQ(firstStarted.value(u"PlaySessionId"_s), firstStopped.value(u"PlaySessionId"_s));
    Playlist* second = application.playlistHandler()->createNewPlaylist(u"Second"_s, {track2});
    std::fputs("native fixture: play second\n", stderr);
    player->startPlayback(second);
    ASSERT_TRUE(QTest::qWaitFor([&]() { return starts.size() == 2; }, 10000));
    const auto secondContext = starts.back().front().value<Engine::PlaybackOccurrenceContext>();
    EXPECT_EQ(secondContext.track.id(), track2.id());
    player->seek(6000);
    ASSERT_TRUE(QTest::qWaitFor([&]() { return player->currentPosition() >= 6000; }, 5000));
    ASSERT_TRUE(QTest::qWaitFor([&]() { return server.requestsForPath(u"/jf/Sessions/Playing"_s).size() == 2; }, 5000));
    Playlist* failed = application.playlistHandler()->createNewPlaylist(u"Failed"_s, {broken});
    QSignalSpy statuses{application.engine(), &EngineController::trackStatusContextChanged};
    std::fputs("native fixture: expected broken source\n", stderr);
    expectingBrokenTrack = true;
    player->startPlayback(failed);
    ASSERT_TRUE(QTest::qWaitFor(
        [&]() {
            return std::ranges::any_of(statuses, [](const auto& values) {
                return values.front().template value<Engine::TrackStatusContext>().status
                    == Engine::TrackStatus::Invalid;
            });
        },
        10000));
    expectingBrokenTrack = false;
    EXPECT_EQ(starts.size(), 2);
    ASSERT_TRUE(QTest::qWaitFor([&]() { return stops.size() == 2; }, 5000));
    EXPECT_EQ(stops.back().front().value<Engine::PlaybackOccurrenceContext>().track.id(), track2.id());
    EXPECT_EQ(stops.back().front().value<Engine::PlaybackOccurrenceContext>().playbackOccurrenceId,
              secondContext.playbackOccurrenceId);
    ASSERT_TRUE(
        QTest::qWaitFor([&]() { return server.requestsForPath(u"/jf/Sessions/Playing/Stopped"_s).size() == 2; }, 5000));
    const auto secondStopped
        = QJsonDocument::fromJson(server.requestsForPath(u"/jf/Sessions/Playing/Stopped"_s).back().body).object();
    EXPECT_EQ(secondStopped.value(u"ItemId"_s).toString(), u"track2"_s);
    EXPECT_GE(secondStopped.value(u"PositionTicks"_s).toInteger(), 60000000);
    const auto secondStarted
        = QJsonDocument::fromJson(server.requestsForPath(u"/jf/Sessions/Playing"_s).back().body).object();
    EXPECT_EQ(secondStopped.value(u"PlaySessionId"_s), secondStarted.value(u"PlaySessionId"_s));
    std::fputs("native fixture: replay first\n", stderr);
    player->stop();
    player->startPlayback(first);
    ASSERT_TRUE(QTest::qWaitFor([&]() { return starts.size() == 3; }, 10000));
    player->stop();
    ASSERT_TRUE(
        QTest::qWaitFor([&]() { return server.requestsForPath(u"/jf/Sessions/Playing/Stopped"_s).size() == 3; }, 5000));
    const auto reports = server.requestsForPath(u"/jf/Sessions/Playing"_s);
    ASSERT_EQ(reports.size(), 3U);
    const QJsonObject report1 = QJsonDocument::fromJson(reports.at(0).body).object();
    const QJsonObject report2 = QJsonDocument::fromJson(reports.at(1).body).object();
    const QJsonObject replay  = QJsonDocument::fromJson(reports.at(2).body).object();
    EXPECT_EQ(report1.value(u"ItemId"_s).toString(), u"track1"_s);
    EXPECT_EQ(report2.value(u"ItemId"_s).toString(), u"track2"_s);
    EXPECT_EQ(replay.value(u"ItemId"_s).toString(), u"track1"_s);
    EXPECT_NE(report1.value(u"PlaySessionId"_s), replay.value(u"PlaySessionId"_s));
    std::fputs("native fixture: hide retained source\n", stderr);
    NativeWorkerBarrier background;
    application.library()->setActiveLibraryFilters(
        {{.id = 1, .name = u"Held title"_s, .expression = u"title:Synthetic"_s}});
    nativeTree->searchEvent({.text = u"Synthetic"_s, .emptyMode = EmptySearchMode::ShowAll});
    nativeFilter->searchEvent({.text = u"Synthetic"_s, .emptyMode = EmptySearchMode::ShowAll});
    access->clearAccess();
    playlistController->changeCurrentPlaylist(mirror->id());
    application.library()->setExternalLibraryActive(*sourceLibrary, false);
    EXPECT_NE(playlistController->currentPlaylist(), mirror);
    EXPECT_FALSE(treeHasTracks());
    EXPECT_FALSE(filterHasTracks());
    nativeTree->searchEvent({.text = u"Synthetic"_s, .emptyMode = EmptySearchMode::ShowAll});
    nativeFilter->searchEvent({.text = u"Synthetic"_s, .emptyMode = EmptySearchMode::ShowAll});
    background.release();
    QThreadPool::globalInstance()->waitForDone();
    QTest::qWait(100);
    EXPECT_FALSE(treeHasTracks());
    EXPECT_FALSE(filterHasTracks());
    playlistController->changeCurrentPlaylist(mirror->id());
    EXPECT_NE(playlistController->currentPlaylist(), mirror);
    ASSERT_TRUE(QTest::qWaitFor([&]() { return !treeHasTracks() && !filterHasTracks(); }, 10000));
    TrackList changedTracks = application.library()->libraryTracks();
    Track added{sourceLibrary->path + u"/new-item"_s};
    added.setTitle(u"Synthetic retained addition"_s);
    added.setArtists({u"Synthetic Artist"_s});
    added.setAlbum(u"Synthetic Album"_s);
    changedTracks.push_back(added);
    QSignalSpy applied{application.library(), &MusicLibrary::externalLibrarySnapshotFinished};
    application.library()->applyExternalLibrarySnapshot(*sourceLibrary, changedTracks, QUuid::createUuid());
    ASSERT_TRUE(QTest::qWaitFor([&]() { return !applied.isEmpty(); }, 5000));
    ASSERT_TRUE(applied.front().at(2).toBool());
    QTest::qWait(100);
    EXPECT_FALSE(treeHasTracks());
    EXPECT_FALSE(filterHasTracks());
    EXPECT_EQ(first->tracks().front().id(), track1.id());
    application.library()->setExternalLibraryActive(*sourceLibrary, true);
    ASSERT_TRUE(QTest::qWaitFor([&]() { return treeHasTracks() && filterHasTracks(); }, 10000));
}
TEST(JellyfinNativeIntegration, PlaylistTabsAndManagerProjectStoredOrderAcrossHiddenSourceRecords)
{
    Application application;
    application.settingsManager()->set<Settings::Core::FirstRun>(false);
    GuiApplication gui{&application};
    gui.layoutProvider()->changeLayout(
        FyLayout{u"Synthetic playlist regression"_s,
                 QJsonObject{{u"Widgets"_s, QJsonArray{QJsonObject{{u"Spacer"_s, QJsonObject{}}}}}}});
    gui.startup();
    ASSERT_EQ(gui.layoutProvider()->currentLayout().name(), u"Synthetic playlist regression"_s);
    auto views         = std::make_unique<QWidget>();
    auto* layout       = new QHBoxLayout(views.get());
    auto* first        = gui.widgetProvider()->createWidget(u"PlaylistTabs"_s);
    auto* second       = gui.widgetProvider()->createWidget(u"PlaylistTabs"_s);
    const auto cleanup = qScopeGuard([&]() {
        views.reset();
        for(QWidget* widget : QApplication::topLevelWidgets()) {
            if(widget->inherits("Fooyin::PlaylistManagerWidget")) {
                widget->close();
            }
        }
        gui.shutdown();
    });
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    layout->addWidget(first);
    layout->addWidget(second);
    auto* firstTabs  = first->findChild<QTabBar*>();
    auto* secondTabs = second->findChild<QTabBar*>();
    ASSERT_NE(firstTabs, nullptr);
    ASSERT_NE(secondTabs, nullptr);
    auto* managerCommand = gui.actionManager()->command(Constants::Actions::PlaylistManager);
    ASSERT_NE(managerCommand, nullptr);
    ASSERT_NE(managerCommand->action(), nullptr);
    managerCommand->action()->trigger();
    QAbstractItemView* managerView{nullptr};
    for(QWidget* widget : QApplication::topLevelWidgets()) {
        if(widget->inherits("Fooyin::PlaylistManagerWidget")) {
            managerView = widget->findChild<QAbstractItemView*>();
        }
    }
    ASSERT_NE(managerView, nullptr);
    QAbstractItemModel* model = managerView->model();
    while(auto* proxy = qobject_cast<QAbstractProxyModel*>(model)) {
        model = proxy->sourceModel();
    }
    ASSERT_NE(model, nullptr);
    QAbstractItemModelTester modelTester{model, QAbstractItemModelTester::FailureReportingMode::Fatal};
    auto* handler     = application.playlistHandler();
    const auto source = application.library()->addExternalLibrary(u"test-source://ordering"_s, u"Ordering"_s);
    ASSERT_TRUE(source);
    const auto existingPlaylists = handler->playlists();
    auto* z2                     = handler->createNewPlaylist(u"Z2"_s);
    for(const Playlist* playlist : existingPlaylists) {
        handler->removePlaylist(playlist->id());
    }
    auto* z = handler->createNewPlaylist(u"Z"_s);
    auto* a = handler->applyExternalPlaylist(source->path, u"a"_s, {}, u"A"_s, {});
    auto* b = handler->applyExternalPlaylist(source->path, u"b"_s, {}, u"B"_s, {});
    auto* c = handler->applyExternalPlaylist(source->path, u"c"_s, {}, u"C"_s, {});
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);
    CurrentPlaylistController* playlistController = gui.playlistController();
    playlistController->changeCurrentPlaylist(z2->id());
    const auto assertProjection = [&]() {
        const auto visible = handler->playlists();
        ASSERT_EQ(model->rowCount(), static_cast<int>(visible.size()));
        ASSERT_EQ(firstTabs->count(), static_cast<int>(visible.size()));
        ASSERT_EQ(secondTabs->count(), static_cast<int>(visible.size()));
        EXPECT_EQ(firstTabs->tabData(firstTabs->currentIndex()).value<UId>(), z2->id());
        EXPECT_EQ(secondTabs->tabData(secondTabs->currentIndex()).value<UId>(), z2->id());
        EXPECT_EQ(playlistController->currentPlaylist(), z2);
        for(int row{0}; row < model->rowCount(); ++row) {
            EXPECT_EQ(model->index(row, 0).data().toString(), visible.at(row)->name());
            EXPECT_EQ(firstTabs->tabData(row).value<UId>(), visible.at(row)->id());
            EXPECT_EQ(secondTabs->tabData(row).value<UId>(), visible.at(row)->id());
        }
    };
    application.library()->setExternalLibraryActive(*source, false);
    assertProjection();
    ASSERT_TRUE(handler->sortPlaylistsByName(Qt::AscendingOrder));
    assertProjection();
    EXPECT_EQ(a->index(), 0);
    EXPECT_EQ(b->index(), 1);
    EXPECT_EQ(c->index(), 2);
    EXPECT_LT(z->index(), z2->index());
    firstTabs->moveTab(0, 1);
    assertProjection();
    secondTabs->moveTab(1, 0);
    assertProjection();
    ASSERT_TRUE(handler->sortPlaylistsByName(Qt::DescendingOrder));
    assertProjection();
    EXPECT_LT(z2->index(), z->index());
    EXPECT_LT(c->index(), b->index());
    EXPECT_LT(b->index(), a->index());
    handler->changePlaylistIndex(a->id(), 1);
    assertProjection();
    application.library()->setExternalLibraryActive(*source, true);
    assertProjection();
    firstTabs->moveTab(0, firstTabs->count() - 1);
    assertProjection();
    secondTabs->moveTab(secondTabs->count() - 1, 0);
    assertProjection();
    application.library()->setExternalLibraryActive(*source, false);
    assertProjection();
    application.library()->setExternalLibraryActive(*source, true);
    assertProjection();
    handler->savePlaylists();
}
TEST(JellyfinNativeIntegration, ExpandedProxyViewInvalidatesCachedIndexesThroughoutStructuralChanges)
{
    QStandardItemModel source;
    source.appendRow(new QStandardItem{u"B"_s});
    source.appendRow(new QStandardItem{u"D"_s});
    QSortFilterProxyModel proxy;
    proxy.setSourceModel(&source);
    ExpandedTreeView view;
    view.setModel(&proxy);
    view.resize(640, 480);
    view.show();
    QAbstractItemModelTester sourceTester{&source, QAbstractItemModelTester::FailureReportingMode::Fatal};
    QAbstractItemModelTester proxyTester{&proxy, QAbstractItemModelTester::FailureReportingMode::Fatal};
    view.setCurrentIndex(proxy.index(1, 0));
    QTest::qWait(10);
    source.insertRow(0, new QStandardItem{u"A"_s});
    EXPECT_EQ(view.currentIndex().data().toString(), u"D"_s);
    source.insertRow(2, new QStandardItem{u"C"_s});
    EXPECT_EQ(view.currentIndex().data().toString(), u"D"_s);
    proxy.sort(0, Qt::DescendingOrder);
    EXPECT_EQ(view.currentIndex().data().toString(), u"D"_s);
    proxy.sort(-1);
    EXPECT_EQ(view.currentIndex().data().toString(), u"D"_s);
    ASSERT_TRUE(source.removeRow(0));
    EXPECT_EQ(view.currentIndex().data().toString(), u"D"_s);
    source.clear();
    EXPECT_FALSE(view.currentIndex().isValid());
    source.appendRow(new QStandardItem{u"Restored"_s});
    view.setCurrentIndex(proxy.index(0, 0));
    QTest::qWait(10);
    EXPECT_EQ(view.currentIndex().data().toString(), u"Restored"_s);
    EXPECT_TRUE(view.visualRect(view.currentIndex()).isValid());
}
TEST(JellyfinNativeIntegration, ReplacingModelDuringStructuralChangeDoesNotRetainOldTransitionDepth)
{
    QStandardItemModel firstSource;
    firstSource.appendRow(new QStandardItem{u"Old one"_s});
    firstSource.appendRow(new QStandardItem{u"Old two"_s});
    QStandardItemModel secondSource;
    secondSource.appendRow(new QStandardItem{u"X"_s});
    secondSource.appendRow(new QStandardItem{u"Y"_s});
    QSortFilterProxyModel firstProxy;
    QSortFilterProxyModel secondProxy;
    firstProxy.setSourceModel(&firstSource);
    secondProxy.setSourceModel(&secondSource);
    ExpandedTreeView view;
    view.setModel(&firstProxy);
    view.resize(640, 480);
    view.show();
    QAbstractItemModelTester firstTester{&firstProxy, QAbstractItemModelTester::FailureReportingMode::Fatal};
    QAbstractItemModelTester secondTester{&secondProxy, QAbstractItemModelTester::FailureReportingMode::Fatal};
    QObject::connect(&firstProxy, &QAbstractItemModel::rowsAboutToBeRemoved, &view, [&]() {
        view.setModel(&firstProxy);
        view.setModel(&secondProxy);
        secondSource.insertRow(0, new QStandardItem{u"Nested insertion"_s});
    });
    ASSERT_TRUE(firstSource.removeRow(0));
    ASSERT_EQ(view.model(), &secondProxy);
    view.setCurrentIndex(secondProxy.index(2, 0));
    view.selectionModel()->select(view.currentIndex(), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    const auto assertCurrent = [&]() {
        QTest::qWait(10);
        EXPECT_EQ(view.currentIndex().data().toString(), u"Y"_s);
        EXPECT_TRUE(view.visualRect(view.currentIndex()).isValid());
        ASSERT_EQ(view.selectionModel()->selectedRows().size(), 1);
        EXPECT_EQ(view.selectionModel()->selectedRows().front().data().toString(), u"Y"_s);
    };
    assertCurrent();
    secondSource.insertRow(0, new QStandardItem{u"Subsequent insertion"_s});
    assertCurrent();
    ASSERT_TRUE(secondSource.removeRow(0));
    assertCurrent();
    secondProxy.sort(0, Qt::DescendingOrder);
    assertCurrent();
    secondProxy.sort(-1);
    assertCurrent();
    view.setModel(&firstProxy);
    QObject::connect(&firstProxy, &QAbstractItemModel::modelAboutToBeReset, &view, [&]() {
        view.setModel(&secondProxy);
        secondSource.removeRow(0);
    });
    firstSource.clear();
    ASSERT_EQ(view.model(), &secondProxy);
    view.setCurrentIndex(secondProxy.index(1, 0));
    view.selectionModel()->select(view.currentIndex(), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    assertCurrent();
    secondSource.insertRow(0, new QStandardItem{u"After reset"_s});
    assertCurrent();
    view.setModel(&secondProxy);
    assertCurrent();
}
} // namespace Fooyin::Jellyfin::Testing

int main(int argc, char** argv)
{
    QTemporaryDir profile;
    if(!profile.isValid()) {
        return 2;
    }
    qputenv("HOME", profile.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", profile.filePath(u"config"_s).toUtf8());
    qputenv("XDG_DATA_HOME", profile.filePath(u"data"_s).toUtf8());
    qputenv("XDG_CACHE_HOME", profile.filePath(u"cache"_s).toUtf8());
    qputenv("XDG_STATE_HOME", profile.filePath(u"state"_s).toUtf8());
    QStandardPaths::setTestModeEnabled(true);
    QApplication app{argc, argv};
    QCoreApplication::setApplicationName(u"fooyin"_s);
    QGuiApplication::setQuitOnLastWindowClosed(false);
    Q_INIT_RESOURCE(data);
    Q_INIT_RESOURCE(icons);
    Q_INIT_RESOURCE(layouts);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
