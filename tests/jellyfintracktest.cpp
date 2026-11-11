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

#include <QJsonArray>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;

namespace Fooyin::Jellyfin::Testing {
TEST(JellyfinTrackTest, SourceKeyIsStableAndSeparatesAccounts)
{
    const QString sourceKey = Fooyin::Jellyfin::sourceKey(u"server-1"_s, u"user-1"_s);
    EXPECT_EQ(sourceKey, Fooyin::Jellyfin::sourceKey(u"server-1"_s, u"user-1"_s));
    EXPECT_NE(sourceKey, Fooyin::Jellyfin::sourceKey(u"server-1"_s, u"user-2"_s));
    EXPECT_NE(sourceKey, Fooyin::Jellyfin::sourceKey(u"server-2"_s, u"user-1"_s));
    EXPECT_EQ(sourceKey.size(), 32);
    EXPECT_FALSE(sourceKey.contains(u"server-1"_s));
}

TEST(JellyfinTrackTest, TrackPathRoundTripsAndIsVirtual)
{
    const QString sourceKey = Fooyin::Jellyfin::sourceKey(u"server-1"_s, u"user-1"_s);
    const QString path      = trackPath(sourceKey, u"a1b2c3"_s);
    EXPECT_EQ(path, u"jellyfin://"_s + sourceKey + u"/a1b2c3"_s);
    EXPECT_EQ(sourceKeyFromPath(path), sourceKey);
    EXPECT_EQ(itemIdFromPath(path), u"a1b2c3"_s);
    EXPECT_TRUE(Track::isVirtualPath(path));
    EXPECT_TRUE(Track::isVirtualPath(libraryUri(sourceKey)));
    EXPECT_TRUE(path.startsWith(libraryUri(sourceKey) + u'/'));
}

TEST(JellyfinTrackTest, PathParsingRejectsForeignAndInvalidPaths)
{
    EXPECT_TRUE(sourceKeyFromPath(u"/music/a.flac"_s).isEmpty());
    EXPECT_TRUE(sourceKeyFromPath(u"https://example.com/a"_s).isEmpty());
    EXPECT_TRUE(itemIdFromPath(u"jellyfin://key/../x"_s).isEmpty());
    EXPECT_TRUE(itemIdFromPath(u"jellyfin://key/a?api_key=1"_s).isEmpty());
    EXPECT_FALSE(isValidItemId(u""_s));
    EXPECT_FALSE(isValidItemId(QString(65, u'a')));
}

TEST(JellyfinTrackTest, CatalogItemMapsAudioMetadataWithoutStreamDetails)
{
    const QString sourceKey = Fooyin::Jellyfin::sourceKey(u"server-1"_s, u"user-1"_s);
    const QJsonObject item{
        {u"Id"_s, u"item1"_s},
        {u"Type"_s, u"Audio"_s},
        {u"Name"_s, u"Synthetic Song"_s},
        {u"Album"_s, u"Synthetic Album"_s},
        {u"Artists"_s, QJsonArray{u"Artist A"_s, u"Artist B"_s}},
        {u"AlbumArtists"_s, QJsonArray{QJsonObject{{u"Name"_s, u"Album Artist"_s}, {u"Id"_s, u"artist1"_s}}}},
        {u"ArtistItems"_s, QJsonArray{QJsonObject{{u"Name"_s, u"Artist A"_s}, {u"Id"_s, u"artist2"_s}}}},
        {u"Genres"_s, QJsonArray{u"Test"_s}},
        {u"IndexNumber"_s, 3},
        {u"ParentIndexNumber"_s, 2},
        {u"RunTimeTicks"_s, 1234560000LL},
        {u"ProductionYear"_s, 2001},
        {u"Container"_s, u"flac"_s},
        {u"DateCreated"_s, u"2020-01-02T03:04:05.000Z"_s},
        {u"UserData"_s, QJsonObject{{u"IsFavorite"_s, true}}},
        {u"AlbumId"_s, u"album1"_s},
        {u"AlbumPrimaryImageTag"_s, u"tag1"_s},
    };

    const auto mapped = catalogItem(item, sourceKey);
    ASSERT_TRUE(mapped.has_value());
    EXPECT_EQ(mapped->itemId, u"item1"_s);
    EXPECT_EQ(mapped->track.filepath(), trackPath(sourceKey, u"item1"_s));
    EXPECT_EQ(mapped->track.title(), u"Synthetic Song"_s);
    EXPECT_EQ(mapped->track.album(), u"Synthetic Album"_s);
    EXPECT_EQ(mapped->track.artists(), (QStringList{u"Artist A"_s, u"Artist B"_s}));
    EXPECT_EQ(mapped->track.albumArtists(), QStringList{u"Album Artist"_s});
    EXPECT_EQ(mapped->track.trackNumber(), u"3"_s);
    EXPECT_EQ(mapped->track.discNumber(), u"2"_s);
    EXPECT_EQ(mapped->track.duration(), 123456U);
    EXPECT_EQ(mapped->track.year(), 2001);
    EXPECT_EQ(mapped->track.codec(), u"FLAC"_s);
    EXPECT_TRUE(mapped->isFavorite);
    EXPECT_TRUE(mapped->track.isLoved());
    EXPECT_EQ(mapped->track.extraProperties().value(u"Jellyfin/AlbumId"_s), u"album1"_s);
    EXPECT_TRUE(mapped->track.extraProperties().value(u"Jellyfin/AlbumArtists"_s).contains(u"artist1"_s));
    EXPECT_TRUE(mapped->track.extraProperties().value(u"Jellyfin/ArtistItems"_s).contains(u"artist2"_s));
    EXPECT_EQ(mapped->track.extraProperties().value(u"Jellyfin/ImageItemId"_s), u"album1"_s);
    EXPECT_EQ(mapped->track.extraProperties().value(u"Jellyfin/ImageTag"_s), u"tag1"_s);
    ASSERT_TRUE(mapped->image.has_value());
    EXPECT_EQ(mapped->image->itemId, u"album1"_s);
    EXPECT_EQ(mapped->image->tag, u"tag1"_s);
}

TEST(JellyfinTrackTest, CatalogItemRejectsOtherTypesAndInvalidIds)
{
    const QString sourceKey = Fooyin::Jellyfin::sourceKey(u"server-1"_s, u"user-1"_s);
    EXPECT_FALSE(catalogItem(QJsonObject{{u"Id"_s, u"x"_s}, {u"Type"_s, u"MusicAlbum"_s}}, sourceKey).has_value());
    EXPECT_FALSE(catalogItem(QJsonObject{{u"Id"_s, u"../x"_s}, {u"Type"_s, u"Audio"_s}}, sourceKey).has_value());
    EXPECT_FALSE(catalogItem(QJsonObject{{u"Type"_s, u"Audio"_s}}, sourceKey).has_value());
}
} // namespace Fooyin::Jellyfin::Testing
