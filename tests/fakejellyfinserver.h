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
#include <QJsonValue>
#include <QSslCertificate>
#include <QSslKey>
#include <QString>
#include <QUrl>
#include <QUrlQuery>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

class QThread;

namespace Fooyin::Jellyfin::Testing {
class FakeServerWorker;

struct FakeRequest
{
    QByteArray method;
    QString path;
    QUrlQuery query;
    //! Header names are lower case.
    std::map<QByteArray, QByteArray> headers;
    QByteArray body;

    [[nodiscard]] QByteArray header(const QByteArray& name) const;
};

struct FakeResponse
{
    int status{200};
    std::vector<std::pair<QByteArray, QByteArray>> headers;
    QByteArray body;
    int delayMs{0};
    int bodyDelayMs{0};
};

//! Minimal HTTP/1.1 server on its own thread that records every request.
class FakeJellyfinServer
{
public:
    using Handler = std::function<FakeResponse(const FakeRequest&)>;

    FakeJellyfinServer();
    FakeJellyfinServer(QSslCertificate certificate, QSslKey privateKey);
    ~FakeJellyfinServer();

    //! The handler runs on the server thread.
    void setHandler(Handler handler);
    [[nodiscard]] QUrl url(const QString& basePath) const;
    [[nodiscard]] std::vector<FakeRequest> requests() const;
    [[nodiscard]] std::vector<FakeRequest> requestsForPath(const QString& path) const;
    void clearRequests();

    static FakeResponse json(const QJsonValue& value);
    static FakeResponse text(int status, const QByteArray& body);
    static FakeResponse redirect(const QUrl& target);
    //! Serves @p data honouring a Range header when @p honourRange is true.
    static FakeResponse bytes(const FakeRequest& request, const QByteArray& data, bool honourRange);

private:
    friend class FakeServerWorker;

    FakeResponse handle(const FakeRequest& request);

    mutable std::mutex m_mutex;
    Handler m_handler;
    std::vector<FakeRequest> m_requests;
    QThread* m_thread;
    FakeServerWorker* m_worker;
    quint16 m_port{0};
    bool m_tls{false};
};
} // namespace Fooyin::Jellyfin::Testing
