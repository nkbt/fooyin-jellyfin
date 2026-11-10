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

#include "jellyfinrequest.h"
#include "jellyfintrack.h"

#include <QJsonDocument>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QUuid>

Q_LOGGING_CATEGORY(JELLYFIN_PLAYBACK, "fy.jellyfin.playback")

using namespace Qt::StringLiterals;

namespace Fooyin::Jellyfin {
namespace {
constexpr qint64 TicksPerMs = 10000;

qint64 positionTicks(uint64_t ms)
{
    const auto bounded = std::min<uint64_t>(ms, static_cast<uint64_t>(std::numeric_limits<qint64>::max() / TicksPerMs));
    return static_cast<qint64>(bounded) * TicksPerMs;
}
} // namespace

PlaybackReporter::PlaybackReporter(QNetworkAccessManager* network, std::shared_ptr<AccessRegistry> access,
                                   QObject* parent)
    : QObject{parent}
    , m_network{network}
    , m_access{std::move(access)}
{ }

PlaybackReporter::~PlaybackReporter()
{
    m_reportCancellation.reset();
    if(m_reply) {
        m_reply->disconnect(this);
    }
}

void PlaybackReporter::occurrenceStarted(const Engine::PlaybackOccurrenceContext& context)
{
    if(context.playbackOccurrenceId == 0 || context.playbackOccurrenceId <= m_hostPlaybackOccurrenceId) {
        return;
    }
    const auto access = m_access->accessForSource(sourceKeyFromPath(context.track.filepath()));
    if(!access || context.sourceGeneration != access->sourceGeneration) {
        return;
    }
    finishOccurrence(false);
    m_hostPlaybackOccurrenceId = context.playbackOccurrenceId;
    m_seekRequestId            = context.seekRequestId;
    beginOccurrence(context.track);
    if(m_occurrence) {
        m_occurrence->positionMs = context.positionMs;
        m_occurrence->seekable   = context.seekable;
        startOccurrence();
    }
}

void PlaybackReporter::occurrenceUpdated(const Engine::PlaybackOccurrenceContext& context)
{
    if(context.playbackOccurrenceId != m_hostPlaybackOccurrenceId || !m_occurrence
       || context.sourceGeneration != m_occurrence->access.sourceGeneration) {
        return;
    }
    const bool changed   = m_occurrence->paused != context.paused || m_seekRequestId != context.seekRequestId;
    m_occurrence->paused = context.paused;
    m_seekRequestId      = context.seekRequestId;
    if(changed) {
        positionMoved(context.positionMs);
    }
    else {
        positionChanged(context.positionMs);
    }
}

void PlaybackReporter::occurrenceEnded(const Engine::PlaybackOccurrenceContext& context)
{
    if(context.playbackOccurrenceId != m_hostPlaybackOccurrenceId || !m_occurrence
       || context.sourceGeneration != m_occurrence->access.sourceGeneration) {
        return;
    }
    m_occurrence->positionMs = context.positionMs;
    finishOccurrence(context.failed);
}

void PlaybackReporter::shutdown()
{
    finishOccurrence(false);
}

void PlaybackReporter::positionChanged(uint64_t ms)
{
    if(!m_occurrence) {
        return;
    }
    m_occurrence->positionMs = ms;
    if(m_occurrence->started && !m_occurrence->paused
       && (ms < m_occurrence->reportedPositionMs || ms - m_occurrence->reportedPositionMs >= ProgressIntervalMs)) {
        reportProgress();
    }
}

void PlaybackReporter::positionMoved(uint64_t ms)
{
    if(!m_occurrence) {
        return;
    }
    m_occurrence->positionMs = ms;
    if(m_occurrence->started) {
        reportProgress();
    }
}

void PlaybackReporter::beginOccurrence(const Track& track)
{
    m_occurrence.reset();
    const QString path = track.filepath();
    if(!path.startsWith(QString::fromLatin1(Scheme) + "://"_L1)) {
        return;
    }
    const QString itemId = itemIdFromPath(path);
    const auto access    = m_access->accessForSource(sourceKeyFromPath(path));
    if(!access || !isValidItemId(itemId)) {
        return;
    }
    m_occurrence = Occurrence{
        .itemId             = itemId,
        .playSessionId      = QUuid::createUuid().toString(QUuid::Id128),
        .access             = *access,
        .started            = false,
        .paused             = false,
        .positionMs         = 0,
        .reportedPositionMs = 0,
    };
}

void PlaybackReporter::startOccurrence()
{
    if(!m_occurrence || m_occurrence->started || !m_access->isCurrent(m_occurrence->access.sourceGeneration)) {
        return;
    }
    m_occurrence->started            = true;
    m_occurrence->paused             = false;
    m_occurrence->reportedPositionMs = m_occurrence->positionMs;
    enqueue(u"Sessions/Playing"_s, QJsonObject{
                                       {u"ItemId"_s, m_occurrence->itemId},
                                       {u"PlaySessionId"_s, m_occurrence->playSessionId},
                                       {u"PositionTicks"_s, positionTicks(m_occurrence->positionMs)},
                                       {u"CanSeek"_s, m_occurrence->seekable},
                                       {u"IsPaused"_s, false},
                                       {u"PlayMethod"_s, u"DirectPlay"_s},
                                   });
}

void PlaybackReporter::reportProgress()
{
    m_occurrence->reportedPositionMs = m_occurrence->positionMs;
    enqueue(u"Sessions/Playing/Progress"_s, QJsonObject{
                                                {u"ItemId"_s, m_occurrence->itemId},
                                                {u"PlaySessionId"_s, m_occurrence->playSessionId},
                                                {u"PositionTicks"_s, positionTicks(m_occurrence->positionMs)},
                                                {u"CanSeek"_s, m_occurrence->seekable},
                                                {u"IsPaused"_s, m_occurrence->paused},
                                                {u"PlayMethod"_s, u"DirectPlay"_s},
                                            });
}

void PlaybackReporter::finishOccurrence(bool failed)
{
    if(!m_occurrence) {
        return;
    }
    if(m_occurrence->started) {
        enqueue(u"Sessions/Playing/Stopped"_s, QJsonObject{
                                                   {u"ItemId"_s, m_occurrence->itemId},
                                                   {u"PlaySessionId"_s, m_occurrence->playSessionId},
                                                   {u"PositionTicks"_s, positionTicks(m_occurrence->positionMs)},
                                                   {u"Failed"_s, failed},
                                               });
    }
    m_occurrence.reset();
}

void PlaybackReporter::enqueue(const QString& path, const QJsonObject& body)
{
    m_reports.push_back(Report{.path = path, .body = body, .access = m_occurrence->access});
    sendNext();
}

void PlaybackReporter::sendNext()
{
    if(m_reply) {
        return;
    }
    while(!m_reports.empty() && !m_access->isCurrent(m_reports.front().access.sourceGeneration)) {
        qCInfo(JELLYFIN_PLAYBACK) << "Dropped a playback report for a previous account";
        Q_EMIT reportFinished(m_reports.front().path, false);
        m_reports.pop_front();
    }
    if(m_reports.empty()) {
        return;
    }

    const Report report = m_reports.front();
    m_reports.pop_front();

    QNetworkRequest request
        = apiRequest(endpointUrl(report.access.serverUrl, report.path, {}), report.access.authorization);
    request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    auto* reply = m_network->post(request, QJsonDocument{report.body}.toJson(QJsonDocument::Compact));
    restrictRedirects(reply, report.access.serverUrl);
    m_reply = reply;
    m_reportCancellation.emplace(report.access.sourceLifetime, [reply]() {
        QMetaObject::invokeMethod(reply, &QNetworkReply::abort, Qt::QueuedConnection);
    });

    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, path = report.path]() {
        m_reportCancellation.reset();
        reply->deleteLater();
        const bool success = reply->error() == QNetworkReply::NoError;
        if(!success) {
            qCWarning(JELLYFIN_PLAYBACK) << "Playback report failed:" << path << replyError(reply);
        }
        m_reply = nullptr;
        Q_EMIT reportFinished(path, success);
        sendNext();
    });
}
} // namespace Fooyin::Jellyfin
