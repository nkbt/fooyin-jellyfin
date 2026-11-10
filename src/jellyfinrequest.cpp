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

#include <QCoreApplication>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSysInfo>

using namespace Qt::StringLiterals;

namespace Fooyin::Jellyfin {
namespace {
QString basePath(const QUrl& serverUrl)
{
    QString path = serverUrl.path();
    while(path.endsWith(u'/')) {
        path.chop(1);
    }
    return path;
}

QByteArray headerValue(const QString& value)
{
    return QUrl::toPercentEncoding(value, " .-_()");
}
} // namespace

QUrl serverUrlFromInput(const QString& input)
{
    QUrl url = QUrl::fromUserInput(input.trimmed());
    if(!url.isValid() || url.host().isEmpty()) {
        return {};
    }
    const QString scheme = url.scheme().toLower();
    if(scheme != "https"_L1 && scheme != "http"_L1) {
        return {};
    }

    url.setUserInfo({});
    url.setQuery(QString{});
    url.setFragment(QString{});
    url.setPath(basePath(url));
    return url;
}

QUrl endpointUrl(const QUrl& serverUrl, const QString& path, const QUrlQuery& query)
{
    QUrl url{serverUrl};
    url.setPath(basePath(serverUrl) + u'/' + path);
    url.setQuery(query);
    return url;
}

bool isAllowedTarget(const QUrl& serverUrl, const QUrl& target)
{
    if(target.scheme().compare(serverUrl.scheme(), Qt::CaseInsensitive) != 0
       || target.host().compare(serverUrl.host(), Qt::CaseInsensitive) != 0) {
        return false;
    }
    const int defaultPort = serverUrl.scheme().compare("https"_L1, Qt::CaseInsensitive) == 0 ? 443 : 80;
    if(target.port(defaultPort) != serverUrl.port(defaultPort) || !target.userInfo().isEmpty()) {
        return false;
    }

    const QString base = basePath(serverUrl);
    return base.isEmpty() || target.path() == base || target.path().startsWith(base + u'/');
}

QByteArray authorizationHeader(const QString& deviceId, const QString& token)
{
    QByteArray header = "MediaBrowser Client=\"" + headerValue(u"Fooyin"_s) + "\", Device=\""
                      + headerValue(QSysInfo::machineHostName()) + "\", DeviceId=\"" + headerValue(deviceId)
                      + "\", Version=\"" + headerValue(QStringLiteral(JELLYFIN_PLUGIN_VERSION)) + "\"";
    if(!token.isEmpty()) {
        header += ", Token=\"" + headerValue(token) + "\"";
    }
    return header;
}

QNetworkRequest apiRequest(const QUrl& url, const QByteArray& authorization)
{
    QNetworkRequest request{url};
    request.setRawHeader("Authorization", authorization);
    request.setRawHeader("Accept", "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setTransferTimeout(30000);
    return request;
}

void restrictRedirects(QNetworkReply* reply, const QUrl& serverUrl)
{
    QObject::connect(reply, &QNetworkReply::redirected, reply, [reply, serverUrl](const QUrl& target) {
        if(isAllowedTarget(serverUrl, target)) {
            Q_EMIT reply->redirectAllowed();
        }
        else {
            reply->abort();
        }
    });
}

QString replyError(QNetworkReply* reply)
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if(status == 401 || status == 403) {
        return QCoreApplication::translate("Jellyfin", "The server rejected the credentials. Sign in again.");
    }
    if(status >= 400) {
        return QCoreApplication::translate("Jellyfin", "The server returned HTTP status %1.").arg(status);
    }

    switch(reply->error()) {
        case QNetworkReply::NoError:
            return {};
        case QNetworkReply::OperationCanceledError:
            return QCoreApplication::translate("Jellyfin",
                                               "The request was cancelled or redirected to another origin.");
        case QNetworkReply::SslHandshakeFailedError:
            return QCoreApplication::translate("Jellyfin", "The server certificate could not be verified.");
        case QNetworkReply::TimeoutError:
            return QCoreApplication::translate("Jellyfin", "The server did not respond in time.");
        case QNetworkReply::ConnectionRefusedError:
        case QNetworkReply::HostNotFoundError:
        case QNetworkReply::RemoteHostClosedError:
            return QCoreApplication::translate("Jellyfin", "The server could not be reached.");
        default:
            return QCoreApplication::translate("Jellyfin", "The request failed with a network error.");
    }
}
} // namespace Fooyin::Jellyfin
