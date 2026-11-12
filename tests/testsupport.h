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

#pragma once

#include "jellyfincredentialstore.h"

#include <core/library/musiclibrary.h>
#include <core/network/remoteioservice.h>
#include <utils/settings/settingsmanager.h>

#include <QNetworkAccessManager>
#include <QPointer>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>

#include <map>
#include <mutex>
#include <vector>

namespace Fooyin::Jellyfin::Testing {
//! Library that stores external snapshots in memory and reports them asynchronously like the host.
class StubMusicLibrary : public MusicLibrary
{
    Q_OBJECT

public:
    StubMusicLibrary();

    [[nodiscard]] bool hasLibrary() const override;
    [[nodiscard]] std::optional<LibraryInfo> libraryInfo(int id) const override;
    [[nodiscard]] std::optional<LibraryInfo> libraryForPath(const QString& path) const override;
    void loadAllTracks() override;
    [[nodiscard]] bool isEmpty() const override;
    void refreshAll() override;
    void rescanAll() override;
    ScanRequest refresh(const LibraryInfo& library) override;
    ScanRequest rescan(const LibraryInfo& library) override;
    void cancelScan(int id) override;
    ScanRequest scanTracks(const TrackList& tracks) override;
    ScanRequest scanModifiedTracks(const TrackList& tracks) override;
    ScanRequest scanFiles(const QList<QUrl>& files) override;
    ScanRequest loadPlaylist(const QList<QUrl>& files) override;
    [[nodiscard]] TrackList tracks() const override;
    [[nodiscard]] TrackList libraryTracks() const override;
    [[nodiscard]] Track trackForId(int id) const override;
    [[nodiscard]] TrackList tracksForIds(const TrackIds& ids) const override;
    [[nodiscard]] std::shared_ptr<TrackMetadataStore> metadataStore() const override;
    void updateTrack(const Track& track) override;
    void updateTracks(const TrackList& tracks) override;
    void updateTrackMetadata(const TrackList& tracks) override;
    WriteRequest writeTrackMetadata(const TrackList& tracks) override;
    WriteRequest writeTrackCovers(const TrackCoverData& coverData) override;
    [[nodiscard]] PendingTrackCoverProvider* pendingTrackCoverProvider() const override;
    void updateTrackStats(const TrackList& tracks, Track::Stats stats) override;
    void updateTrackStats(const Track& track, Track::Stats stats) override;
    [[nodiscard]] bool canEditTrackStats(const Track& track, Track::Stats stats) const override;
    void editTrackStats(const TrackList& tracks, Track::Stats stats) override;
    WriteRequest removeUnavailbleTracks() override;
    WriteRequest deleteTracks(const TrackList& tracks) override;
    std::optional<LibraryInfo> addExternalLibrary(const QString& path, const QString& name) override;
    void applyExternalLibrarySnapshot(const LibraryInfo& library, const TrackList& tracks,
                                      const QUuid& operation) override;
    void setExternalLibraryReadOnlyStats(const LibraryInfo& library, Track::Stats stats) override;
    void setExternalLibraryActive(const LibraryInfo& library, bool active) override;
    void setExternalLibraryStatsHandler(const LibraryInfo& library, Track::Stats stats, QObject* handler) override;
    void applyExternalTrackStats(const LibraryInfo& library, const TrackList& tracks, Track::Stats stats,
                                 const QUuid& operation) override;
    bool isTrackSourceActive(const Track& track) const override;

    [[nodiscard]] TrackList tracksForLibrary(int libraryId) const;
    [[nodiscard]] std::map<int, LibraryInfo> libraries() const;

    bool failSnapshots{false};
    int snapshotRequests{0};
    int snapshotDelayMs{0};
    QUuid lastSnapshotOperation;
    //! Stores of later applyExternalTrackStats() calls fail after publishing.
    bool failTrackStats{false};
    //! Every accepted applyExternalTrackStats() batch in call order.
    std::vector<TrackList> trackStatsPublications;

private:
    struct StatsHandler
    {
        Track::Stats stats;
        QPointer<QObject> handler;
    };

    [[nodiscard]] Track::Stats handledStats(const Track& canonical) const;

    std::map<int, LibraryInfo> m_libraries;
    TrackList m_tracks;
    int m_nextTrackId{1};
    std::map<int, Track::Stats> m_sourceStats;
    std::map<QString, bool> m_inactiveSources;
    std::map<int, StatsHandler> m_statsHandlers;
    quint64 m_commitSequence{0};
    //! The commit sequence of the latest published Loved value per track.
    std::map<int, quint64> m_lovedPublications;
};

class RemoteIoFixture
{
public:
    RemoteIoFixture();
    explicit RemoteIoFixture(std::shared_ptr<QNetworkAccessManager> network);
    ~RemoteIoFixture();
    QTemporaryDir directory;
    SettingsManager settings;
    QThread networkThread;
    std::shared_ptr<QNetworkAccessManager> network;
    std::shared_ptr<RemoteIoService> remoteIo;
};

class MemoryCredentialStore : public CredentialStore
{
public:
    [[nodiscard]] TokenLookup readToken(const QString& credentialRecordId, std::stop_token cancellation) override;
    [[nodiscard]] QString writeToken(const QString& credentialRecordId, const QString& token,
                                     std::stop_token cancellation) override;
    [[nodiscard]] QString removeToken(const QString& credentialRecordId, std::stop_token cancellation) override;

    std::map<QString, QString> tokens;
    QString writeError;
};

//! Records every Qt log message while it is alive.
class LogCapture
{
public:
    LogCapture();
    ~LogCapture();

    [[nodiscard]] QStringList messages() const;
    [[nodiscard]] bool contains(const QString& text) const;
};
} // namespace Fooyin::Jellyfin::Testing
