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

#include <QJsonObject>
#include <QString>
#include <optional>

namespace Fooyin::Jellyfin {
struct CredentialStateLoad
{
    std::optional<QJsonObject> state;
    QString error;
};

class CredentialStateStore
{
public:
    virtual ~CredentialStateStore()                  = default;
    virtual CredentialStateLoad load()               = 0;
    virtual QString commit(const QJsonObject& state) = 0;
};

class FileCredentialStateStore final : public CredentialStateStore
{
public:
    explicit FileCredentialStateStore(QString credentialStatePath);
    CredentialStateLoad load() override;
    QString commit(const QJsonObject& state) override;

private:
    QString m_credentialStatePath;
};
} // namespace Fooyin::Jellyfin
