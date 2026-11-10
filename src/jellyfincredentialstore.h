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

#include <QString>

#include <optional>
#include <stop_token>

namespace Fooyin::Jellyfin {
struct TokenLookup
{
    std::optional<QString> token;
    //! Non-empty when the credential store could not be used.
    QString error;
};

//! Holds access tokens outside the player profile.
class CredentialStore
{
public:
    virtual ~CredentialStore() = default;

    [[nodiscard]] virtual TokenLookup readToken(const QString& credentialRecordId, std::stop_token cancellation) = 0;
    //! Returns an error message, or an empty string on success.
    [[nodiscard]] virtual QString writeToken(const QString& credentialRecordId, const QString& token,
                                             std::stop_token cancellation) = 0;
    //! Returns an error message, or an empty string on success.
    [[nodiscard]] virtual QString removeToken(const QString& credentialRecordId, std::stop_token cancellation) = 0;
};

//! Stores tokens through the desktop Secret Service (libsecret).
class SecretServiceCredentialStore : public CredentialStore
{
public:
    [[nodiscard]] TokenLookup readToken(const QString& credentialRecordId, std::stop_token cancellation) override;
    [[nodiscard]] QString writeToken(const QString& credentialRecordId, const QString& token,
                                     std::stop_token cancellation) override;
    [[nodiscard]] QString removeToken(const QString& credentialRecordId, std::stop_token cancellation) override;
};
} // namespace Fooyin::Jellyfin
