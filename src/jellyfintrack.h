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

#include <core/track.h>

#include <QJsonObject>
#include <QString>

#include <optional>

namespace Fooyin::Jellyfin {
constexpr auto Scheme = "jellyfin";

struct CatalogImage
{
    QString itemId;
    QString tag;
};

struct CatalogItem
{
    Track track;
    QString itemId;
    bool isFavorite{false};
    std::optional<CatalogImage> image;
};

//! Stable key for one server and account; it contains no credentials.
QString sourceKey(const QString& serverId, const QString& userId);
QString libraryUri(const QString& sourceKey);
QString trackPath(const QString& sourceKey, const QString& itemId);
//! Returns the source key of a Jellyfin track path, or an empty string.
QString sourceKeyFromPath(const QString& path);
//! Returns the item ID of a Jellyfin track path, or an empty string.
QString itemIdFromPath(const QString& path);
bool isValidItemId(const QString& itemId);

//! Maps one BaseItemDto of type Audio; returns nothing for invalid or other items.
std::optional<CatalogItem> catalogItem(const QJsonObject& item, const QString& sourceKey);
} // namespace Fooyin::Jellyfin
