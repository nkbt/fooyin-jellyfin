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

#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QUrlQuery>

class QNetworkReply;
class QNetworkRequest;

namespace Fooyin::Jellyfin {
//! Returns a normalised server URL (scheme, host, port and base path), or an invalid URL.
QUrl serverUrlFromInput(const QString& input);
//! Builds the URL for an API path below the configured server base path.
QUrl endpointUrl(const QUrl& serverUrl, const QString& path, const QUrlQuery& query);
//! True when the target stays on the configured origin and below its base path.
bool isAllowedTarget(const QUrl& serverUrl, const QUrl& target);
//! Value of the Authorization header; the token is omitted when empty.
QByteArray authorizationHeader(const QString& deviceId, const QString& token);
QNetworkRequest apiRequest(const QUrl& url, const QByteArray& authorization);
//! Follows redirects only within the configured origin and base path.
void restrictRedirects(QNetworkReply* reply, const QUrl& serverUrl);
//! A user-facing error message without URLs, headers or response bodies.
QString replyError(QNetworkReply* reply);
} // namespace Fooyin::Jellyfin
