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

#include "jellyfinrequest.h"
#include "jellyfintrack.h"

#include <core/engine/audioloader.h>
#include <core/network/remotestreamdevice.h>

#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QUrlQuery>

#include <climits>

Q_LOGGING_CATEGORY(JELLYFIN_INPUT, "fy.jellyfin.input")

using namespace Qt::StringLiterals;

namespace Fooyin::Jellyfin {
namespace {
constexpr qint64 MaxCoverBytes = 32LL * 1024 * 1024;
const auto InnerDecoderName    = u"FFmpeg"_s;
} // namespace

QUrl streamUrl(const QUrl& serverUrl, const QString& itemId)
{
    if(!isValidItemId(itemId)) {
        return {};
    }
    QUrlQuery query;
    query.addQueryItem(u"static"_s, u"true"_s);
    return endpointUrl(serverUrl, u"Audio/%1/stream"_s.arg(itemId), query);
}

QUrl imageUrl(const QUrl& serverUrl, const CatalogImage& image)
{
    if(!isValidItemId(image.itemId) || image.tag.isEmpty()) {
        return {};
    }
    QUrlQuery query;
    query.addQueryItem(u"tag"_s, QString::fromLatin1(QUrl::toPercentEncoding(image.tag)));
    return endpointUrl(serverUrl, u"Items/%1/Images/Primary"_s.arg(image.itemId), query);
}

Decoder::Decoder(std::shared_ptr<AccessRegistry> access, std::weak_ptr<AudioLoader> audioLoader,
                 std::shared_ptr<RemoteIoService> remoteIo, std::chrono::milliseconds timeout)
    : m_access{std::move(access)}
    , m_audioLoader{std::move(audioLoader)}
    , m_timeout{timeout}
    , m_remoteIo{std::move(remoteIo)}
{ }

Decoder::~Decoder()
{
    Decoder::stop();
}

QStringList Decoder::extensions() const
{
    return {};
}

QStringList Decoder::supportedSchemes() const
{
    return {QString::fromLatin1(Scheme)};
}

bool Decoder::needsMoreInput() const
{
    return m_decoder && m_decoder->needsMoreInput();
}

uint64_t Decoder::sourceGeneration() const
{
    return m_sourceGeneration;
}

bool Decoder::isSeekable() const
{
    return m_decoder && m_decoder->isSeekable();
}

bool Decoder::allowsConcurrentDecoding() const
{
    return true;
}

int Decoder::playbackPrebufferMs() const
{
    return m_decoder ? m_decoder->playbackPrebufferMs() : 0;
}

QStringList Decoder::takeWarnings()
{
    return m_decoder ? m_decoder->takeWarnings() : QStringList{};
}

bool Decoder::trackHasChanged() const
{
    return m_decoder && m_decoder->trackHasChanged();
}

Track Decoder::changedTrack() const
{
    return m_decoder ? m_decoder->changedTrack() : Track{};
}

int Decoder::bitrate() const
{
    return m_decoder ? m_decoder->bitrate() : 0;
}

std::optional<AudioFormat> Decoder::init(const AudioSource& source, const Track& track, DecoderOptions options)
{
    stop();

    const QString sourceKey = sourceKeyFromPath(source.filepath);
    const QString itemId    = itemIdFromPath(source.filepath);
    const auto access       = m_access->accessForSource(sourceKey);
    if(!access) {
        qCWarning(JELLYFIN_INPUT) << "Jellyfin track is unavailable because its account is not signed in";
        return {};
    }

    m_sourceGeneration = access->sourceGeneration;
    const QUrl url     = streamUrl(access->serverUrl, itemId);
    if(url.isEmpty()) {
        qCWarning(JELLYFIN_INPUT) << "Invalid Jellyfin track path";
        return {};
    }

    const auto loader = m_audioLoader.lock();
    if(!loader) {
        return {};
    }
    for(const auto& entry : loader->decoders()) {
        if(entry.name == InnerDecoderName && entry.enabled) {
            m_decoder = entry.creator();
            break;
        }
    }
    if(!m_decoder) {
        qCWarning(JELLYFIN_INPUT) << "The FFmpeg decoder is unavailable";
        return {};
    }
    m_decoder->setPlaybackHints(playbackHints());

    std::atomic_store(&m_interruptDecoder, m_decoder);
    if(abortToken().stop_requested()) {
        m_decoder->requestAbort();
        stop();
        return {};
    }
    const auto pending
        = m_remoteIo->prepareStream(apiRequest(url, access->authorization), access->serverUrl, access->sourceLifetime);
    if(!pending->waitForReady(m_timeout, abortToken()) || access->sourceLifetime.stop_requested()) {
        stop();
        return {};
    }
    m_source = pending->takeSource();
    if(!m_source.device) {
        stop();
        return {};
    }
    AudioSource innerSource;
    innerSource.filepath           = source.filepath;
    innerSource.device             = m_source.device.get();
    innerSource.remoteStreamDevice = m_source.remoteDevice;
    innerSource.remoteOpenTimeout  = m_source.openTimeout;

    auto format = m_decoder->init(innerSource, track, options);
    if(!format) {
        stop();
    }
    return format;
}

void Decoder::start()
{
    if(m_decoder) {
        m_decoder->start();
    }
}

void Decoder::stop()
{
    std::atomic_store(&m_interruptDecoder, std::shared_ptr<AudioDecoder>{});
    m_sourceGeneration = 0;
    if(m_decoder) {
        m_decoder->stop();
        m_decoder.reset();
    }
    if(m_source.device) {
        m_source.device->close();
        m_source = {};
    }
}

void Decoder::seek(uint64_t pos)
{
    if(m_decoder) {
        m_decoder->seek(pos);
    }
}

AudioDecoder::ReadResult Decoder::readAudio(size_t bytes)
{
    if(!m_decoder) {
        return ReadResult::errorResult();
    }
    return m_decoder->readAudio(bytes);
}

AudioBuffer Decoder::readBuffer(size_t bytes)
{
    if(!m_decoder) {
        return {};
    }
    return m_decoder->readBuffer(bytes);
}

void Decoder::playbackHintsChanged(PlaybackHints hints)
{
    if(m_decoder) {
        m_decoder->setPlaybackHints(hints);
    }
}

void Decoder::interruptRead()
{
    if(const auto decoder = std::atomic_load(&m_interruptDecoder)) {
        decoder->requestAbort();
    }
}

Reader::Reader(std::shared_ptr<AccessRegistry> access, std::shared_ptr<RemoteIoService> remoteIo,
               std::chrono::milliseconds timeout)
    : m_access{std::move(access)}
    , m_timeout{timeout}
    , m_remoteIo{std::move(remoteIo)}
{ }

QStringList Reader::extensions() const
{
    return {};
}

QStringList Reader::supportedSchemes() const
{
    return {QString::fromLatin1(Scheme)};
}

bool Reader::canReadCover() const
{
    return true;
}

bool Reader::canWriteMetaData() const
{
    return false;
}

bool Reader::readTrack(const AudioSource& /*source*/, Track& /*track*/)
{
    return false;
}

QByteArray Reader::readCover(const AudioSource& source, const Track& /*track*/, Track::Cover cover)
{
    if(cover != Track::Cover::Front) {
        return {};
    }

    const QString sourceKey = sourceKeyFromPath(source.filepath);
    const auto access       = m_access->accessForSource(sourceKey);
    if(!access) {
        return {};
    }
    const auto image = m_access->image(sourceKey, itemIdFromPath(source.filepath));
    if(!image) {
        return {};
    }
    const QUrl url = imageUrl(access->serverUrl, *image);
    if(url.isEmpty()) {
        return {};
    }

    const auto pending
        = m_remoteIo->prepareStream(apiRequest(url, access->authorization), access->serverUrl, access->sourceLifetime);
    if(!pending->waitForReady(m_timeout, access->sourceLifetime)) {
        return {};
    }
    auto remoteSource = pending->takeSource();
    if(!remoteSource.device) {
        return {};
    }
    remoteSource.remoteDevice->setNonBlockingReadsEnabled(true);
    QElapsedTimer timer;
    timer.start();
    QByteArray data;
    while(!access->sourceLifetime.stop_requested()) {
        const QByteArray chunk
            = remoteSource.device->read(std::min<qint64>(64 * 1024, MaxCoverBytes + 1 - data.size()));
        if(chunk.isEmpty()) {
            if(!remoteSource.remoteDevice->readWouldBlock()) {
                break;
            }
            const qint64 remaining = m_timeout.count() - timer.elapsed();
            if(remaining <= 0
               || !remoteSource.device->waitForReadyRead(static_cast<int>(std::min<qint64>(remaining, INT_MAX)))) {
                return {};
            }
        }
        data.append(chunk);
        if(data.size() > MaxCoverBytes) {
            return {};
        }
    }
    if(access->sourceLifetime.stop_requested()
       || (remoteSource.device->size() >= 0 && data.size() != remoteSource.device->size())) {
        return {};
    }
    return data;
}
} // namespace Fooyin::Jellyfin
