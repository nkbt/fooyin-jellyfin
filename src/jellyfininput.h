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

#include <atomic>
#include <core/engine/audioinput.h>
#include <core/network/remoteioservice.h>

#include <memory>

namespace Fooyin {
class AudioLoader;

namespace Jellyfin {
//! Streams an authenticated Jellyfin item on demand and decodes it with the host FFmpeg decoder.
class Decoder : public AudioDecoder
{
public:
    Decoder(std::shared_ptr<AccessRegistry> access, std::weak_ptr<AudioLoader> audioLoader,
            std::shared_ptr<RemoteIoService> remoteIo, std::chrono::milliseconds timeout);
    ~Decoder() override;

    [[nodiscard]] QStringList extensions() const override;
    [[nodiscard]] QStringList supportedSchemes() const override;
    [[nodiscard]] bool needsMoreInput() const override;
    [[nodiscard]] bool isSeekable() const override;
    [[nodiscard]] uint64_t sourceGeneration() const override;
    [[nodiscard]] bool allowsConcurrentDecoding() const override;
    [[nodiscard]] int playbackPrebufferMs() const override;
    [[nodiscard]] QStringList takeWarnings() override;
    [[nodiscard]] bool trackHasChanged() const override;
    [[nodiscard]] Track changedTrack() const override;
    [[nodiscard]] int bitrate() const override;

    std::optional<AudioFormat> init(const AudioSource& source, const Track& track, DecoderOptions options) override;
    void start() override;
    void stop() override;
    void seek(uint64_t pos) override;
    ReadResult readAudio(size_t bytes) override;
    AudioBuffer readBuffer(size_t bytes) override;

protected:
    void playbackHintsChanged(PlaybackHints hints) override;
    void interruptRead() override;

private:
    std::shared_ptr<AccessRegistry> m_access;
    std::weak_ptr<AudioLoader> m_audioLoader;
    std::chrono::milliseconds m_timeout;
    std::shared_ptr<RemoteIoService> m_remoteIo;
    RemoteStreamSource m_source;
    std::shared_ptr<AudioDecoder> m_decoder;
    std::atomic<std::shared_ptr<AudioDecoder>> m_interruptDecoder;
    uint64_t m_sourceGeneration{0};
};

//! Supplies cover art for Jellyfin tracks. Track metadata comes only from catalog synchronisation.
class Reader : public AudioReader
{
public:
    Reader(std::shared_ptr<AccessRegistry> access, std::shared_ptr<RemoteIoService> remoteIo,
           std::chrono::milliseconds timeout);

    [[nodiscard]] QStringList extensions() const override;
    [[nodiscard]] QStringList supportedSchemes() const override;
    [[nodiscard]] bool canReadCover() const override;
    [[nodiscard]] bool canWriteMetaData() const override;
    [[nodiscard]] bool readTrack(const AudioSource& source, Track& track) override;
    [[nodiscard]] QByteArray readCover(const AudioSource& source, const Track& track, Track::Cover cover) override;

private:
    std::shared_ptr<AccessRegistry> m_access;
    std::shared_ptr<RemoteIoService> m_remoteIo;
    std::chrono::milliseconds m_timeout;
};

//! Returns the streaming URL for an item, or an empty URL for an invalid item.
[[nodiscard]] QUrl streamUrl(const QUrl& serverUrl, const QString& itemId);
//! Returns the primary image URL for an item image, or an empty URL for an invalid image.
[[nodiscard]] QUrl imageUrl(const QUrl& serverUrl, const CatalogImage& image);
} // namespace Jellyfin
} // namespace Fooyin
