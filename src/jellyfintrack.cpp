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

#include "jellyfintrack.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

using namespace Qt::StringLiterals;

namespace Fooyin::Jellyfin {
namespace {
constexpr auto TicksPerMillisecond = 10000;

QStringList stringList(const QJsonValue& value)
{
    QStringList ret;
    const QJsonArray array = value.toArray();
    for(const auto& entry : array) {
        if(const QString text = entry.toString(); !text.isEmpty()) {
            ret.append(text);
        }
    }
    return ret;
}

QStringList namedItems(const QJsonValue& value)
{
    QStringList ret;
    const QJsonArray array = value.toArray();
    for(const auto& entry : array) {
        if(const QString name = entry.toObject().value("Name"_L1).toString(); !name.isEmpty()) {
            ret.append(name);
        }
    }
    return ret;
}

std::optional<CatalogImage> catalogImage(const QJsonObject& item, const QString& itemId)
{
    const QString ownTag = item.value("ImageTags"_L1).toObject().value("Primary"_L1).toString();
    if(!ownTag.isEmpty()) {
        return CatalogImage{.itemId = itemId, .tag = ownTag};
    }

    const QString albumId  = item.value("AlbumId"_L1).toString();
    const QString albumTag = item.value("AlbumPrimaryImageTag"_L1).toString();
    if(isValidItemId(albumId) && !albumTag.isEmpty()) {
        return CatalogImage{.itemId = albumId, .tag = albumTag};
    }
    return {};
}
} // namespace

QString sourceKey(const QString& serverId, const QString& userId)
{
    const QByteArray identity = serverId.toUtf8() + '\n' + userId.toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex().left(32));
}

QString libraryUri(const QString& sourceKey)
{
    return QLatin1StringView{Scheme} + "://"_L1 + sourceKey;
}

QString trackPath(const QString& sourceKey, const QString& itemId)
{
    return libraryUri(sourceKey) + u'/' + itemId;
}

QString sourceKeyFromPath(const QString& path)
{
    const QString prefix = QLatin1StringView{Scheme} + "://"_L1;
    if(!path.startsWith(prefix)) {
        return {};
    }
    const QStringView rest = QStringView{path}.sliced(prefix.size());
    const qsizetype slash  = rest.indexOf(u'/');
    return slash > 0 ? rest.first(slash).toString() : QString{};
}

QString itemIdFromPath(const QString& path)
{
    const QString key = sourceKeyFromPath(path);
    if(key.isEmpty()) {
        return {};
    }
    const QString itemId = path.sliced(libraryUri(key).size() + 1);
    return isValidItemId(itemId) ? itemId : QString{};
}

bool isValidItemId(const QString& itemId)
{
    static const QRegularExpression pattern{u"^[A-Za-z0-9-]{1,64}$"_s};
    return pattern.match(itemId).hasMatch();
}

std::optional<CatalogItem> catalogItem(const QJsonObject& item, const QString& sourceKey)
{
    const QString itemId = item.value("Id"_L1).toString();
    if(item.value("Type"_L1).toString() != "Audio"_L1 || !isValidItemId(itemId)) {
        return {};
    }

    Track track{trackPath(sourceKey, itemId)};
    track.setTitle(item.value("Name"_L1).toString());
    track.setAlbum(item.value("Album"_L1).toString());
    track.setArtists(stringList(item.value("Artists"_L1)));

    QStringList albumArtists = namedItems(item.value("AlbumArtists"_L1));
    if(albumArtists.isEmpty()) {
        if(const QString albumArtist = item.value("AlbumArtist"_L1).toString(); !albumArtist.isEmpty()) {
            albumArtists.append(albumArtist);
        }
    }
    track.setAlbumArtists(albumArtists);
    track.setGenres(stringList(item.value("Genres"_L1)));

    if(const QJsonValue index = item.value("IndexNumber"_L1); index.isDouble()) {
        track.setTrackNumber(QString::number(index.toInteger()));
    }
    if(const QJsonValue disc = item.value("ParentIndexNumber"_L1); disc.isDouble()) {
        track.setDiscNumber(QString::number(disc.toInteger()));
    }
    if(const QJsonValue ticks = item.value("RunTimeTicks"_L1); ticks.isDouble() && ticks.toInteger() > 0) {
        track.setDuration(static_cast<uint64_t>(ticks.toInteger() / TicksPerMillisecond));
    }

    const QDateTime premiere = QDateTime::fromString(item.value("PremiereDate"_L1).toString(), Qt::ISODateWithMs);
    if(premiere.isValid()) {
        track.setDate(premiere.toUTC().date().toString(Qt::ISODate));
    }
    else if(const QJsonValue year = item.value("ProductionYear"_L1); year.isDouble()) {
        track.setYear(year.toInt());
    }

    const QDateTime created = QDateTime::fromString(item.value("DateCreated"_L1).toString(), Qt::ISODateWithMs);
    if(created.isValid()) {
        track.setAddedTime(static_cast<uint64_t>(created.toMSecsSinceEpoch()));
    }
    if(const QString container = item.value("Container"_L1).toString(); !container.isEmpty()) {
        track.setCodec(container.toUpper());
    }

    track.setLoved(item.value("UserData"_L1).toObject().value("IsFavorite"_L1).toBool());
    track.setExtraProperty(u"Jellyfin/AlbumId"_s, item.value("AlbumId"_L1).toString(),
                           Track::ExtraPropertyReloadPolicy::Preserve);
    track.setExtraProperty(
        u"Jellyfin/ArtistItems"_s,
        QString::fromUtf8(QJsonDocument{item.value("ArtistItems"_L1).toArray()}.toJson(QJsonDocument::Compact)),
        Track::ExtraPropertyReloadPolicy::Preserve);
    track.setExtraProperty(
        u"Jellyfin/AlbumArtists"_s,
        QString::fromUtf8(QJsonDocument{item.value("AlbumArtists"_L1).toArray()}.toJson(QJsonDocument::Compact)),
        Track::ExtraPropertyReloadPolicy::Preserve);
    if(const auto image = catalogImage(item, itemId)) {
        track.setExtraProperty(u"Jellyfin/ImageItemId"_s, image->itemId, Track::ExtraPropertyReloadPolicy::Preserve);
        track.setExtraProperty(u"Jellyfin/ImageTag"_s, image->tag, Track::ExtraPropertyReloadPolicy::Preserve);
    }

    return CatalogItem{
        .track      = track,
        .itemId     = itemId,
        .isFavorite = item.value("UserData"_L1).toObject().value("IsFavorite"_L1).toBool(),
        .image      = catalogImage(item, itemId),
    };
}
} // namespace Fooyin::Jellyfin
