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

#include "testsupport.h"

#include <QTimer>

#include <algorithm>
#include <future>
#include <ranges>

using namespace Qt::StringLiterals;

namespace Fooyin::Jellyfin::Testing {
namespace {
std::mutex logMutex;
QStringList logMessages;
QtMessageHandler previousHandler{nullptr};

void captureMessage(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    {
        const std::scoped_lock lock{logMutex};
        logMessages.append(message);
    }
    if(previousHandler) {
        previousHandler(type, context, message);
    }
}
} // namespace

StubMusicLibrary::StubMusicLibrary() = default;

bool StubMusicLibrary::hasLibrary() const
{
    return !m_libraries.empty();
}

std::optional<LibraryInfo> StubMusicLibrary::libraryInfo(int id) const
{
    if(const auto it = m_libraries.find(id); it != m_libraries.end()) {
        return it->second;
    }
    return {};
}

std::optional<LibraryInfo> StubMusicLibrary::libraryForPath(const QString& path) const
{
    for(const auto& library : m_libraries | std::views::values) {
        if(path == library.path || path.startsWith(library.path + u'/')) {
            return library;
        }
    }
    return {};
}

void StubMusicLibrary::loadAllTracks()
{
    Q_EMIT tracksLoaded(m_tracks);
}

bool StubMusicLibrary::isEmpty() const
{
    return m_tracks.empty();
}

void StubMusicLibrary::refreshAll() { }

void StubMusicLibrary::rescanAll() { }

ScanRequest StubMusicLibrary::refresh(const LibraryInfo& library)
{
    Q_EMIT externalLibraryRefreshRequested(library);
    return {};
}

ScanRequest StubMusicLibrary::rescan(const LibraryInfo& library)
{
    Q_EMIT externalLibraryRefreshRequested(library);
    return {};
}

void StubMusicLibrary::cancelScan(int /*id*/) { }

ScanRequest StubMusicLibrary::scanTracks(const TrackList& /*tracks*/)
{
    return {};
}

ScanRequest StubMusicLibrary::scanModifiedTracks(const TrackList& /*tracks*/)
{
    return {};
}

ScanRequest StubMusicLibrary::scanFiles(const QList<QUrl>& /*files*/)
{
    return {};
}

ScanRequest StubMusicLibrary::loadPlaylist(const QList<QUrl>& /*files*/)
{
    return {};
}

TrackList StubMusicLibrary::tracks() const
{
    return m_tracks;
}

TrackList StubMusicLibrary::libraryTracks() const
{
    return m_tracks;
}

Track StubMusicLibrary::trackForId(int id) const
{
    for(const auto& track : m_tracks) {
        if(track.id() == id) {
            return track;
        }
    }
    return {};
}

TrackList StubMusicLibrary::tracksForIds(const TrackIds& ids) const
{
    TrackList found;
    for(const int id : ids) {
        found.push_back(trackForId(id));
    }
    return found;
}

std::shared_ptr<TrackMetadataStore> StubMusicLibrary::metadataStore() const
{
    return {};
}

void StubMusicLibrary::updateTrack(const Track& /*track*/) { }

void StubMusicLibrary::updateTracks(const TrackList& /*tracks*/) { }

void StubMusicLibrary::updateTrackMetadata(const TrackList& /*tracks*/) { }

WriteRequest StubMusicLibrary::writeTrackMetadata(const TrackList& /*tracks*/)
{
    return {};
}

WriteRequest StubMusicLibrary::writeTrackCovers(const TrackCoverData& /*coverData*/)
{
    return {};
}

PendingTrackCoverProvider* StubMusicLibrary::pendingTrackCoverProvider() const
{
    return nullptr;
}

void StubMusicLibrary::updateTrackStats(const TrackList& /*tracks*/, Track::Stats /*stats*/) { }

void StubMusicLibrary::updateTrackStats(const Track& /*track*/, Track::Stats /*stats*/) { }

WriteRequest StubMusicLibrary::removeUnavailbleTracks()
{
    return {};
}

WriteRequest StubMusicLibrary::deleteTracks(const TrackList& /*tracks*/)
{
    return {};
}

std::optional<LibraryInfo> StubMusicLibrary::addExternalLibrary(const QString& path, const QString& name)
{
    if(!Track::isVirtualPath(path) || path.endsWith(u'/')) {
        return {};
    }
    for(const auto& library : m_libraries | std::views::values) {
        if(library.path == path) {
            return library;
        }
    }
    LibraryInfo library;
    library.name = name;
    library.path = path;
    library.id   = static_cast<int>(m_libraries.size()) + 1;
    m_libraries.emplace(library.id, library);
    return library;
}

void StubMusicLibrary::setExternalLibraryReadOnlyStats(const LibraryInfo& library, Track::Stats stats)
{
    m_sourceStats[library.id] = stats;
}

void StubMusicLibrary::setExternalLibraryActive(const LibraryInfo& library, bool active)
{
    m_inactiveSources[library.path] = !active;
    Q_EMIT sourceActivationChanged();
}

bool StubMusicLibrary::isTrackSourceActive(const Track& track) const
{
    const auto it = m_inactiveSources.find(track.externalLibraryPath());
    return it == m_inactiveSources.end() || !it->second;
}

void StubMusicLibrary::applyExternalLibrarySnapshot(const LibraryInfo& library, const TrackList& tracks,
                                                    const QUuid& operation)
{
    ++snapshotRequests;
    lastSnapshotOperation = operation;
    QTimer::singleShot(snapshotDelayMs, this, [this, library, tracks, operation]() {
        if(failSnapshots || !m_libraries.contains(library.id)) {
            Q_EMIT externalLibrarySnapshotFinished(operation, library.id, false);

            return;
        }

        std::map<QString, Track> existing;
        TrackList kept;
        for(const auto& track : m_tracks) {
            if(track.libraryId() == library.id) {
                existing.emplace(track.filepath(), track);
            }
            else {
                kept.push_back(track);
            }
        }
        for(Track track : tracks) {
            const auto it = existing.find(track.filepath());
            track.setId(it != existing.end() ? it->second.id() : m_nextTrackId++);
            track.setExternalLibraryPath(library.path);
            track.setReadOnlyStats(m_sourceStats[library.id]);
            track.setLibraryId(library.id);
            kept.push_back(track);
        }
        m_tracks = kept;
        Q_EMIT externalLibrarySnapshotFinished(operation, library.id, true);
    });
}

RemoteIoFixture::RemoteIoFixture()
    : RemoteIoFixture{std::shared_ptr<QNetworkAccessManager>{
          new QNetworkAccessManager(), [](QNetworkAccessManager* manager) { manager->deleteLater(); }}}
{ }

RemoteIoFixture::RemoteIoFixture(std::shared_ptr<QNetworkAccessManager> network)
    : settings{directory.filePath(u"settings.ini"_s)}
    , network{std::move(network)}
    , remoteIo{std::make_shared<RemoteIoService>(this->network, &settings)}
{
    this->network->moveToThread(&networkThread);
    auto started = std::make_shared<std::promise<void>>();
    auto ready   = started->get_future();
    QObject::connect(
        &networkThread, &QThread::started, &networkThread, [started]() { started->set_value(); }, Qt::DirectConnection);
    networkThread.start();
    ready.get();
}

RemoteIoFixture::~RemoteIoFixture()
{
    remoteIo.reset();
    network.reset();
    networkThread.quit();
    networkThread.wait();
}

TrackList StubMusicLibrary::tracksForLibrary(int libraryId) const
{
    TrackList found;
    for(const auto& track : m_tracks) {
        if(track.libraryId() == libraryId) {
            found.push_back(track);
        }
    }
    return found;
}

std::map<int, LibraryInfo> StubMusicLibrary::libraries() const
{
    return m_libraries;
}

TokenLookup MemoryCredentialStore::readToken(const QString& credentialRecordId, std::stop_token cancellation)
{
    if(cancellation.stop_requested()) {
        return {};
    }
    if(const auto it = tokens.find(credentialRecordId); it != tokens.end()) {
        return {.token = it->second, .error = {}};
    }
    return {.token = {}, .error = {}};
}

QString MemoryCredentialStore::writeToken(const QString& credentialRecordId, const QString& token,
                                          std::stop_token cancellation)
{
    if(cancellation.stop_requested()) {
        return {};
    }
    if(!writeError.isEmpty()) {
        return writeError;
    }
    tokens[credentialRecordId] = token;
    return {};
}

QString MemoryCredentialStore::removeToken(const QString& credentialRecordId, std::stop_token cancellation)
{
    if(cancellation.stop_requested()) {
        return {};
    }
    tokens.erase(credentialRecordId);
    return {};
}

LogCapture::LogCapture()
{
    const std::scoped_lock lock{logMutex};
    logMessages.clear();
    previousHandler = qInstallMessageHandler(captureMessage);
}

LogCapture::~LogCapture()
{
    qInstallMessageHandler(previousHandler);
}

QStringList LogCapture::messages() const
{
    const std::scoped_lock lock{logMutex};
    return logMessages;
}

bool LogCapture::contains(const QString& text) const
{
    const std::scoped_lock lock{logMutex};
    return std::ranges::any_of(logMessages, [&text](const QString& message) { return message.contains(text); });
}
} // namespace Fooyin::Jellyfin::Testing

#include "moc_testsupport.cpp"
