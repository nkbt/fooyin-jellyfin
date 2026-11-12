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

#include "jellyfintrack.h"

#include <QByteArray>
#include <QString>
#include <QUrl>

#include <mutex>
#include <optional>
#include <stop_token>
#include <unordered_map>

namespace Fooyin::Jellyfin {
struct Connection
{
    QUrl serverUrl;
    QString serverId;
    QString userId;
    QString userName;
    QString deviceId;
};

struct Access
{
    QUrl serverUrl;
    QString userId;
    QString sourceKey;
    QByteArray authorization;
    //! Changes whenever the signed-in account or its token changes.
    quint64 sourceGeneration{0};
    std::stop_token sourceLifetime;
};

//! Thread-safe current account access shared by synchronisation, playback and artwork.
class AccessRegistry
{
public:
    void setAccess(const Connection& connection, const QString& token);
    void clearAccess();

    [[nodiscard]] std::optional<Access> access() const;
    [[nodiscard]] std::optional<Access> accessForSource(const QString& sourceKey) const;
    [[nodiscard]] bool isCurrent(quint64 sourceGeneration) const;

    void setImages(const QString& sourceKey, std::unordered_map<QString, CatalogImage> images);
    [[nodiscard]] std::optional<CatalogImage> image(const QString& sourceKey, const QString& itemId) const;

private:
    mutable std::mutex m_mutex;
    std::optional<Access> m_access;
    quint64 m_sourceGeneration{0};
    std::stop_source m_sourceCancellation;
    QString m_imageSourceKey;
    std::unordered_map<QString, CatalogImage> m_images;
};
} // namespace Fooyin::Jellyfin
