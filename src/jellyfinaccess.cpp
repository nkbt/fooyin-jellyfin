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

#include "jellyfinaccess.h"

#include "jellyfinrequest.h"

namespace Fooyin::Jellyfin {
void AccessRegistry::setAccess(const Connection& connection, const QString& token)
{
    std::stop_source previous;
    {
        const std::scoped_lock lock{m_mutex};
        previous             = std::move(m_sourceCancellation);
        m_sourceCancellation = std::stop_source{};
        m_access             = Access{
            .serverUrl        = connection.serverUrl,
            .userId           = connection.userId,
            .sourceKey        = sourceKey(connection.serverId, connection.userId),
            .authorization    = authorizationHeader(connection.deviceId, token),
            .sourceGeneration = ++m_sourceGeneration,
            .sourceLifetime   = m_sourceCancellation.get_token(),
        };
    }
    previous.request_stop();
}

void AccessRegistry::clearAccess()
{
    std::stop_source previous;
    {
        const std::scoped_lock lock{m_mutex};
        previous             = std::move(m_sourceCancellation);
        m_sourceCancellation = std::stop_source{};
        ++m_sourceGeneration;
        m_access.reset();
    }
    previous.request_stop();
}

std::optional<Access> AccessRegistry::access() const
{
    const std::scoped_lock lock{m_mutex};
    return m_access;
}

std::optional<Access> AccessRegistry::accessForSource(const QString& sourceKey) const
{
    const std::scoped_lock lock{m_mutex};
    if(m_access && m_access->sourceKey == sourceKey) {
        return m_access;
    }
    return {};
}

bool AccessRegistry::isCurrent(quint64 sourceGeneration) const
{
    const std::scoped_lock lock{m_mutex};
    return m_access && m_access->sourceGeneration == sourceGeneration;
}

void AccessRegistry::setImages(const QString& sourceKey, std::unordered_map<QString, CatalogImage> images)
{
    const std::scoped_lock lock{m_mutex};
    m_imageSourceKey = sourceKey;
    m_images         = std::move(images);
}

std::optional<CatalogImage> AccessRegistry::image(const QString& sourceKey, const QString& itemId) const
{
    const std::scoped_lock lock{m_mutex};
    if(sourceKey != m_imageSourceKey) {
        return {};
    }
    if(const auto it = m_images.find(itemId); it != m_images.end()) {
        return it->second;
    }
    return {};
}
} // namespace Fooyin::Jellyfin
