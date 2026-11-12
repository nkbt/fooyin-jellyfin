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

bool StubMusicLibrary::canEditTrackStats(const Track& track, Track::Stats stats) const
{
    const Track canonical = trackForId(track.id());
    if(!canonical.isValid()) {
        return canUpdateTrackStats(track, stats);
    }
    return ((stats & canonical.readOnlyStats()) & ~handledStats(canonical)) == Track::Stat::None;
}

void StubMusicLibrary::editTrackStats(const TrackList& tracks, Track::Stats stats)
{
    updateTrackStats(tracks, stats);

    std::map<int, TrackList> requestedTracks;
    for(const Track& track : tracks) {
        const Track canonical             = trackForId(track.id());
        const Track::Stats requestedStats = stats & handledStats(canonical);
        if(requestedStats != Track::Stat::None) {
            Track requestedTrack{canonical};
            requestedTrack.replaceStats(track, requestedStats);
            requestedTracks[canonical.libraryId()].push_back(requestedTrack);
        }
    }
    for(const auto& [libraryId, libraryTracks] : requestedTracks) {
        Q_EMIT externalTrackStatsChangeRequested(m_libraries.at(libraryId), libraryTracks,
                                                 stats & handledStats(libraryTracks.front()));
    }
}

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
    // Like an SQLite row ID, a removed highest ID is reused.
    library.id = m_libraries.empty() ? 1 : m_libraries.rbegin()->first + 1;
    m_libraries.emplace(library.id, library);
    return library;
}

void StubMusicLibrary::removeExternalLibrary(int libraryId)
{
    const auto library = m_libraries.find(libraryId);
    if(library == m_libraries.end()) {
        return;
    }
    const QString path = library->second.path;
    m_libraries.erase(library);

    if(m_statsHandlers.erase(libraryId) > 0) {
        Q_EMIT externalLibraryStatsHandlerChanged();
    }
    m_sourceStats.erase(libraryId);
    const bool wasInactive = m_inactiveSources.contains(path) && m_inactiveSources.at(path);
    m_inactiveSources.erase(path);
    if(wasInactive) {
        Q_EMIT sourceActivationChanged();
    }

    TrackList kept;
    TrackList removed;
    for(const Track& track : m_tracks) {
        if(track.libraryId() == libraryId) {
            removed.push_back(track);
        }
        else {
            kept.push_back(track);
        }
    }
    m_tracks = kept;
    Q_EMIT tracksDeleted(removed);
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

void StubMusicLibrary::setExternalLibraryStatsHandler(const LibraryInfo& library, Track::Stats stats, QObject* handler)
{
    if(!m_libraries.contains(library.id)) {
        return;
    }
    if(stats == Track::Stat::None || !handler) {
        if(m_statsHandlers.erase(library.id) > 0) {
            Q_EMIT externalLibraryStatsHandlerChanged();
        }
        return;
    }
    m_statsHandlers.insert_or_assign(library.id, StatsHandler{.stats = stats, .handler = handler});
    QObject::connect(handler, &QObject::destroyed, this, [this, libraryId = library.id]() {
        const auto current = m_statsHandlers.find(libraryId);
        if(current != m_statsHandlers.end() && current->second.handler.isNull()) {
            m_statsHandlers.erase(current);
            Q_EMIT externalLibraryStatsHandlerChanged();
        }
    });
    Q_EMIT externalLibraryStatsHandlerChanged();
}

void StubMusicLibrary::applyExternalTrackStats(const LibraryInfo& library, const TrackList& tracks, Track::Stats stats,
                                               const QUuid& operation)
{
    const Track::Stats sourceStats = stats & m_sourceStats[library.id];
    bool accepted = !operation.isNull() && m_libraries.contains(library.id) && sourceStats != Track::Stat::None
                 && !tracks.empty();
    for(const Track& track : tracks) {
        const Track canonical = trackForId(track.id());
        accepted              = accepted && canonical.isValid() && canonical.libraryId() == library.id
                             && canonical.externalLibraryPath() == library.path;
    }
    if(!accepted) {
        QTimer::singleShot(0, this, [this, operation, libraryId = library.id]() {
            Q_EMIT externalTrackStatsFinished(operation, libraryId, false);
        });
        return;
    }

    const quint64 sequence = ++m_commitSequence;
    TrackList changed;
    for(const Track& reported : tracks) {
        for(Track& track : m_tracks) {
            if(track.id() != reported.id()) {
                continue;
            }
            Track updated{track};
            updated.replaceStats(reported, sourceStats);
            if(sourceStats.testFlag(Track::Stat::Loved)) {
                m_lovedPublications[track.id()] = sequence;
            }
            if(updated.isLoved() != track.isLoved() || updated.rating() != track.rating()
               || updated.playCount() != track.playCount()) {
                track = updated;
                changed.push_back(track);
            }
        }
    }
    trackStatsPublications.push_back(tracks);
    if(!changed.empty()) {
        Q_EMIT tracksUpdated(changed);
        Q_EMIT tracksStatsChanged(changed, sourceStats);
    }
    QTimer::singleShot(0, this, [this, operation, libraryId = library.id, success = !failTrackStats]() {
        Q_EMIT externalTrackStatsFinished(operation, libraryId, success);
    });
}

Track::Stats StubMusicLibrary::handledStats(const Track& canonical) const
{
    const auto handler = m_statsHandlers.find(canonical.libraryId());
    if(handler == m_statsHandlers.end() || handler->second.handler.isNull() || !isTrackSourceActive(canonical)) {
        return Track::Stat::None;
    }
    const auto library = m_libraries.find(canonical.libraryId());
    if(library == m_libraries.end() || library->second.path != canonical.externalLibraryPath()) {
        return Track::Stat::None;
    }
    return handler->second.stats & canonical.readOnlyStats();
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
    // Like the host commit queue, statistics published after this request survive the snapshot.
    QTimer::singleShot(snapshotDelayMs, this, [this, library, tracks, operation, sequence = ++m_commitSequence]() {
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
            if(const auto published = m_lovedPublications.find(track.id());
               it != existing.end() && published != m_lovedPublications.end() && published->second > sequence) {
                track.setLoved(it->second.isLoved());
            }
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
