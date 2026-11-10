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

#include "jellyfinrequest.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;

namespace Fooyin::Jellyfin::Testing {
TEST(JellyfinRequestTest, ServerUrlKeepsBasePathAndDropsCredentialsQueryAndFragment)
{
    const QUrl url = serverUrlFromInput(u"  https://user:secret@jellyfin.example.com:8920/jf/?x=1#frag  "_s);
    EXPECT_EQ(url.toString(), u"https://jellyfin.example.com:8920/jf"_s);
    EXPECT_TRUE(url.userInfo().isEmpty());
}

TEST(JellyfinRequestTest, ServerUrlRejectsUnsupportedInput)
{
    EXPECT_FALSE(serverUrlFromInput(u"ftp://jellyfin.example.com"_s).isValid());
    EXPECT_FALSE(serverUrlFromInput(u""_s).isValid());
    EXPECT_FALSE(serverUrlFromInput(u"file:///music"_s).isValid());
}

TEST(JellyfinRequestTest, EndpointUrlPreservesBasePath)
{
    QUrlQuery query;
    query.addQueryItem(u"userId"_s, u"abc"_s);
    EXPECT_EQ(endpointUrl(QUrl{u"https://jellyfin.example.com/jf"_s}, u"Items"_s, query).toString(),
              u"https://jellyfin.example.com/jf/Items?userId=abc"_s);
    EXPECT_EQ(endpointUrl(QUrl{u"https://jellyfin.example.com"_s}, u"Items"_s, {}).toString(),
              u"https://jellyfin.example.com/Items"_s);
}

TEST(JellyfinRequestTest, AllowedTargetRequiresSameOriginAndBasePath)
{
    const QUrl server{u"https://jellyfin.example.com/jf"_s};
    EXPECT_TRUE(isAllowedTarget(server, QUrl{u"https://jellyfin.example.com/jf/Items"_s}));
    EXPECT_TRUE(isAllowedTarget(server, QUrl{u"https://JELLYFIN.example.com:443/jf/Audio/1/stream"_s}));
    EXPECT_FALSE(isAllowedTarget(server, QUrl{u"https://jellyfin.example.com/other/Items"_s}));
    EXPECT_FALSE(isAllowedTarget(server, QUrl{u"https://jellyfin.example.com/jfx/Items"_s}));
    EXPECT_FALSE(isAllowedTarget(server, QUrl{u"http://jellyfin.example.com/jf/Items"_s}));
    EXPECT_FALSE(isAllowedTarget(server, QUrl{u"https://jellyfin.example.com:8443/jf/Items"_s}));
    EXPECT_FALSE(isAllowedTarget(server, QUrl{u"https://media.example.com/jf/Items"_s}));
    EXPECT_FALSE(isAllowedTarget(server, QUrl{u"https://user@jellyfin.example.com/jf/Items"_s}));
}

TEST(JellyfinRequestTest, AuthorizationHeaderOmitsEmptyTokenAndEncodesValues)
{
    const QByteArray anonymous = authorizationHeader(u"device-1"_s, {});
    EXPECT_TRUE(anonymous.startsWith("MediaBrowser Client=\"Fooyin\""));
    EXPECT_TRUE(anonymous.contains("DeviceId=\"device-1\""));
    EXPECT_TRUE(anonymous.contains("Version=\""));
    EXPECT_FALSE(anonymous.contains("Token"));

    const QByteArray signedIn = authorizationHeader(u"device-1"_s, u"tok\"en"_s);
    EXPECT_TRUE(signedIn.contains("Token=\"tok%22en\""));
}
} // namespace Fooyin::Jellyfin::Testing
