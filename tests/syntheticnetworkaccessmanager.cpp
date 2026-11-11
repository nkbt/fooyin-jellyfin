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

#include "syntheticnetworkaccessmanager.h"

#include <QNetworkReply>
#include <QTimer>

#include <cstring>

using namespace Qt::StringLiterals;
namespace Fooyin::Jellyfin::Testing {
namespace {
class SyntheticReply final : public QNetworkReply
{
public:
    SyntheticReply(QNetworkAccessManager::Operation operation, const QNetworkRequest& request, QByteArray body,
                   QObject* parent)
        : QNetworkReply{parent}
        , m_body{std::move(body)}
    {
        setOperation(operation);
        setRequest(request);
        setUrl(request.url());
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
        setHeader(QNetworkRequest::ContentLengthHeader, m_body.size());
        setRawHeader("Accept-Ranges", "bytes");
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(0, this, [this]() {
            if(isFinished()) {
                return;
            }
            Q_EMIT metaDataChanged();
            Q_EMIT readyRead();
            setFinished(true);
            Q_EMIT finished();
        });
    }
    void abort() override
    {
        if(!isFinished()) {
            setError(OperationCanceledError, u"Cancelled"_s);
            setFinished(true);
            Q_EMIT errorOccurred(OperationCanceledError);
            Q_EMIT finished();
        }
    }
    qint64 bytesAvailable() const override
    {
        return m_body.size() - m_offset + QNetworkReply::bytesAvailable();
    }

protected:
    qint64 readData(char* data, qint64 maximum) override
    {
        if(m_offset == m_body.size()) {
            return -1;
        }
        const auto count = std::min(maximum, m_body.size() - m_offset);
        std::memcpy(data, m_body.constData() + m_offset, static_cast<size_t>(count));
        m_offset += count;
        return count;
    }

private:
    QByteArray m_body;
    qint64 m_offset{0};
};
} // namespace
SyntheticNetworkAccessManager::SyntheticNetworkAccessManager(QObject* parent)
    : QNetworkAccessManager{parent}
{ }
QNetworkReply* SyntheticNetworkAccessManager::createRequest(Operation operation, const QNetworkRequest& request,
                                                            QIODevice* /*outgoingData*/)
{
    const QString path = request.url().path();
    if(path.endsWith(u"/QuickConnect/Enabled"_s)) {
        return new SyntheticReply(operation, request, "true", this);
    }
    if(path.endsWith(u"/QuickConnect/Initiate"_s)) {
        return new SyntheticReply(operation, request,
                                  R"({"Code":"246810","Secret":"0123456789abcdef0123456789abcdef"})", this);
    }
    if(path.endsWith(u"/QuickConnect/Connect"_s)) {
        return new SyntheticReply(operation, request,
                                  R"({"Authenticated":true,"Secret":"0123456789abcdef0123456789abcdef"})", this);
    }
    if(path.endsWith(u"/Users/AuthenticateWithQuickConnect"_s)) {
        return new SyntheticReply(
            operation, request,
            R"({"AccessToken":"synthetic-race-token","ServerId":"server1","User":{"Id":"user1","Name":"Synthetic"}})",
            this);
    }
    if(path.endsWith(u"/Items"_s)) {
        return new SyntheticReply(operation, request, R"({"Items":[],"TotalRecordCount":0})", this);
    }
    streamRequestAuthenticated.store(request.rawHeader("Authorization").startsWith("MediaBrowser ")
                                     && request.rawHeader("Authorization").contains("Token="));
    return new SyntheticReply(operation, request, QByteArray(16, 'a'), this);
}
} // namespace Fooyin::Jellyfin::Testing
