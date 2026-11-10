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

#include "fakejellyfinserver.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>

using namespace Qt::StringLiterals;

namespace Fooyin::Jellyfin::Testing {
namespace {
QByteArray reasonPhrase(int status)
{
    switch(status) {
        case 200:
            return "OK";
        case 206:
            return "Partial Content";
        case 302:
            return "Found";
        case 401:
            return "Unauthorized";
        case 404:
            return "Not Found";
        case 416:
            return "Range Not Satisfiable";
        default:
            return "Status";
    }
}
class SyntheticTcpServer : public QTcpServer
{
public:
    SyntheticTcpServer(QSslCertificate certificate, QSslKey privateKey, std::function<void(QTcpSocket*)> accepted,
                       QObject* parent)
        : QTcpServer{parent}
        , m_certificate{std::move(certificate)}
        , m_privateKey{std::move(privateKey)}
        , m_accepted{std::move(accepted)}
    { }

protected:
    void incomingConnection(qintptr descriptor) override
    {
        QTcpSocket* socket = m_certificate.isNull() ? new QTcpSocket(this) : new QSslSocket(this);
        if(!socket->setSocketDescriptor(descriptor)) {
            socket->deleteLater();
            return;
        }
        m_accepted(socket);
        if(auto* encrypted = qobject_cast<QSslSocket*>(socket)) {
            encrypted->setLocalCertificate(m_certificate);
            encrypted->setPrivateKey(m_privateKey);
            encrypted->setPeerVerifyMode(QSslSocket::VerifyNone);
            encrypted->startServerEncryption();
        }
    }

private:
    QSslCertificate m_certificate;
    QSslKey m_privateKey;
    std::function<void(QTcpSocket*)> m_accepted;
};
} // namespace

class FakeServerWorker : public QObject
{
public:
    FakeServerWorker(FakeJellyfinServer* server, QSslCertificate certificate, QSslKey privateKey)
        : m_server{server}
        , m_certificate{std::move(certificate)}
        , m_privateKey{std::move(privateKey)}
    { }

    quint16 listen()
    {
        m_tcpServer = new SyntheticTcpServer(
            m_certificate, m_privateKey,
            [this](QTcpSocket* socket) {
                QObject::connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { readRequest(socket); });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            },
            this);
        if(!m_tcpServer->listen(QHostAddress::LocalHost, 0)) {
            return 0;
        }
        return m_tcpServer->serverPort();
    }

    void close()
    {
        delete m_tcpServer;
        m_tcpServer = nullptr;
    }

private:
    void readRequest(QTcpSocket* socket)
    {
        QByteArray& buffer = m_buffers[socket];
        buffer.append(socket->readAll());

        const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
        if(headerEnd < 0) {
            return;
        }

        const QList<QByteArray> lines       = buffer.left(headerEnd).split('\n');
        const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');

        FakeRequest request;
        request.method = requestLine.value(0);
        const QUrl target{QString::fromLatin1(requestLine.value(1))};
        request.path  = target.path();
        request.query = QUrlQuery{target};
        for(qsizetype i{1}; i < lines.size(); ++i) {
            const QByteArray line = lines.at(i).trimmed();
            const qsizetype colon = line.indexOf(':');
            if(colon > 0) {
                request.headers[line.left(colon).trimmed().toLower()] = line.sliced(colon + 1).trimmed();
            }
        }

        const qsizetype length = request.header("content-length").toLongLong();
        if(buffer.size() < headerEnd + 4 + length) {
            return;
        }
        request.body = buffer.mid(headerEnd + 4, length);
        m_buffers.erase(socket);

        const FakeResponse response = m_server->handle(request);
        QPointer<QTcpSocket> guarded{socket};
        auto write = [guarded, response]() {
            if(!guarded) {
                return;
            }
            QByteArray data = "HTTP/1.1 " + QByteArray::number(response.status) + " " + reasonPhrase(response.status)
                            + "\r\nConnection: close\r\nContent-Length: " + QByteArray::number(response.body.size())
                            + "\r\n";
            for(const auto& [name, value] : response.headers) {
                data += name + ": " + value + "\r\n";
            }
            data += "\r\n";
            guarded->write(data);
            auto writeBody = [guarded, response]() {
                if(guarded) {
                    guarded->write(response.body);
                    guarded->disconnectFromHost();
                }
            };
            if(response.bodyDelayMs > 0) {
                QTimer::singleShot(response.bodyDelayMs, guarded, writeBody);
            }
            else {
                writeBody();
            }
        };
        if(response.delayMs > 0) {
            QTimer::singleShot(response.delayMs, this, write);
        }
        else {
            write();
        }
    }

    FakeJellyfinServer* m_server;
    QSslCertificate m_certificate;
    QSslKey m_privateKey;
    QTcpServer* m_tcpServer{nullptr};
    std::map<QTcpSocket*, QByteArray> m_buffers;
};

QByteArray FakeRequest::header(const QByteArray& name) const
{
    const auto it = headers.find(name);
    return it != headers.end() ? it->second : QByteArray{};
}

FakeJellyfinServer::FakeJellyfinServer()
    : FakeJellyfinServer{QSslCertificate{}, QSslKey{}}
{ }

FakeJellyfinServer::FakeJellyfinServer(QSslCertificate certificate, QSslKey privateKey)
    : m_thread{new QThread()}
    , m_worker{new FakeServerWorker(this, certificate, std::move(privateKey))}
    , m_tls{!certificate.isNull()}
{
    m_worker->moveToThread(m_thread);
    m_thread->start();
    QMetaObject::invokeMethod(m_worker, [this]() { m_port = m_worker->listen(); }, Qt::BlockingQueuedConnection);
}

FakeJellyfinServer::~FakeJellyfinServer()
{
    QMetaObject::invokeMethod(m_worker, [this]() { m_worker->close(); }, Qt::BlockingQueuedConnection);
    m_thread->quit();
    m_thread->wait();
    delete m_worker;
    delete m_thread;
}

void FakeJellyfinServer::setHandler(Handler handler)
{
    const std::scoped_lock lock{m_mutex};
    m_handler = std::move(handler);
}

QUrl FakeJellyfinServer::url(const QString& basePath) const
{
    QUrl url;
    url.setScheme(m_tls ? u"https"_s : u"http"_s);
    url.setHost(u"127.0.0.1"_s);
    url.setPort(m_port);
    url.setPath(basePath);
    return url;
}

std::vector<FakeRequest> FakeJellyfinServer::requests() const
{
    const std::scoped_lock lock{m_mutex};
    return m_requests;
}

std::vector<FakeRequest> FakeJellyfinServer::requestsForPath(const QString& path) const
{
    std::vector<FakeRequest> matching;
    for(const auto& request : requests()) {
        if(request.path == path) {
            matching.push_back(request);
        }
    }
    return matching;
}

void FakeJellyfinServer::clearRequests()
{
    const std::scoped_lock lock{m_mutex};
    m_requests.clear();
}

FakeResponse FakeJellyfinServer::json(const QJsonValue& value)
{
    QByteArray body;
    if(value.isObject()) {
        body = QJsonDocument{value.toObject()}.toJson(QJsonDocument::Compact);
    }
    else if(value.isArray()) {
        body = QJsonDocument{value.toArray()}.toJson(QJsonDocument::Compact);
    }
    else if(value.isBool()) {
        body = value.toBool() ? "true" : "false";
    }
    return {.status = 200, .headers = {{"Content-Type", "application/json"}}, .body = body, .delayMs = 0};
}

FakeResponse FakeJellyfinServer::text(int status, const QByteArray& body)
{
    return {.status = status, .headers = {{"Content-Type", "text/plain"}}, .body = body, .delayMs = 0};
}

FakeResponse FakeJellyfinServer::redirect(const QUrl& target)
{
    return {.status = 302, .headers = {{"Location", target.toEncoded()}}, .body = {}, .delayMs = 0};
}

FakeResponse FakeJellyfinServer::bytes(const FakeRequest& request, const QByteArray& data, bool honourRange)
{
    static const QRegularExpression rangePattern{uR"(^bytes=(\d+)-$)"_s};
    const auto match = rangePattern.match(QString::fromLatin1(request.header("range")));
    if(!honourRange || !match.hasMatch()) {
        FakeResponse response{.status = 200, .headers = {{"Content-Type", "audio/flac"}}, .body = data, .delayMs = 0};
        if(honourRange) {
            response.headers.emplace_back("Accept-Ranges", "bytes");
        }
        return response;
    }
    const qint64 start = match.captured(1).toLongLong();
    if(start >= data.size()) {
        return {.status  = 416,
                .headers = {{"Content-Range", "bytes */" + QByteArray::number(data.size())}},
                .body    = {},
                .delayMs = 0};
    }
    return {.status = 206,
            .headers
            = {{"Content-Type", "audio/flac"},
               {"Content-Range", "bytes " + QByteArray::number(start) + "-" + QByteArray::number(data.size() - 1) + "/"
                                     + QByteArray::number(data.size())}},
            .body    = data.sliced(start),
            .delayMs = 0};
}

FakeResponse FakeJellyfinServer::handle(const FakeRequest& request)
{
    Handler handler;
    {
        const std::scoped_lock lock{m_mutex};
        m_requests.push_back(request);
        handler = m_handler;
    }
    if(!handler) {
        return text(404, "Not found");
    }
    return handler(request);
}
} // namespace Fooyin::Jellyfin::Testing
