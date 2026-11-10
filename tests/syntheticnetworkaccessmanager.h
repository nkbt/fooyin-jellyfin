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

#include <QNetworkAccessManager>
#include <atomic>

namespace Fooyin::Jellyfin::Testing {
class SyntheticNetworkAccessManager final : public QNetworkAccessManager
{
public:
    explicit SyntheticNetworkAccessManager(QObject* parent);
    std::atomic<bool> streamRequestAuthenticated{false};

protected:
    QNetworkReply* createRequest(Operation operation, const QNetworkRequest& request, QIODevice* outgoingData) override;
};
} // namespace Fooyin::Jellyfin::Testing
