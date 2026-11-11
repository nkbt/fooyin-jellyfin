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

#include "jellyfinaccess.h"

#include <core/engine/enginedefs.h>
#include <core/track.h>

#include <QJsonObject>
#include <QObject>
#include <QPointer>

#include <deque>
#include <functional>
#include <memory>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;

namespace Fooyin::Jellyfin {
//! Reports native Jellyfin playback start, progress and stop for each playback occurrence.
class PlaybackReporter : public QObject
{
    Q_OBJECT

public:
    static constexpr uint64_t ProgressIntervalMs = 10000;

    PlaybackReporter(QNetworkAccessManager* network, std::shared_ptr<AccessRegistry> access, QObject* parent);
    ~PlaybackReporter() override;

    void occurrenceStarted(const Engine::PlaybackOccurrenceContext& context);
    void occurrenceUpdated(const Engine::PlaybackOccurrenceContext& context);
    void occurrenceEnded(const Engine::PlaybackOccurrenceContext& context);
    void shutdown();

Q_SIGNALS:
    //! Emitted after each report request completes, with the endpoint path and success.
    void reportFinished(const QString& path, bool success);

private:
    struct Occurrence
    {
        QString itemId;
        QString playSessionId;
        Access access;
        bool started{false};
        bool paused{false};
        bool seekable{false};
        uint64_t positionMs{0};
        uint64_t reportedPositionMs{0};
    };

    struct Report
    {
        QString path;
        QJsonObject body;
        Access access;
    };

    void beginOccurrence(const Track& track);
    void positionChanged(uint64_t ms);
    void positionMoved(uint64_t ms);
    void startOccurrence();
    void finishOccurrence(bool failed);
    void reportProgress();
    void enqueue(const QString& path, const QJsonObject& body);
    void sendNext();

    QNetworkAccessManager* m_network;
    std::shared_ptr<AccessRegistry> m_access;
    std::optional<Occurrence> m_occurrence;
    std::deque<Report> m_reports;
    QPointer<QNetworkReply> m_reply;
    uint64_t m_hostPlaybackOccurrenceId{0};
    uint64_t m_seekRequestId{0};
    std::optional<std::stop_callback<std::function<void()>>> m_reportCancellation;
};
} // namespace Fooyin::Jellyfin
