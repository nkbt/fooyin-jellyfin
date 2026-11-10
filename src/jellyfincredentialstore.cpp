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

#include "jellyfincredentialstore.h"

#include <QCoreApplication>
#include <memory>

#undef signals
#include <libsecret/secret.h>
#define signals Q_SIGNALS

namespace Fooyin::Jellyfin {
namespace {
const SecretSchema* tokenSchema()
{
    static const SecretSchema schema{
        "org.fooyin.jellyfin.AccessToken",
        SECRET_SCHEMA_NONE,
        {
            {"credential-record-id", SECRET_SCHEMA_ATTRIBUTE_STRING},
            {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING},
        },
        0,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
    };
    return &schema;
}

QString unavailableError(GError* error)
{
    QString message = QCoreApplication::translate(
        "Jellyfin", "The desktop secret service is unavailable. Start or unlock a Secret Service provider such as "
                    "GNOME Keyring or KWallet, then sign in again.");
    if(error) {
        g_error_free(error);
    }
    return message;
}
} // namespace

TokenLookup SecretServiceCredentialStore::readToken(const QString& credentialRecordId, std::stop_token cancellation)
{
    const auto cancellable
        = std::unique_ptr<GCancellable, decltype(&g_object_unref)>{g_cancellable_new(), &g_object_unref};
    std::stop_callback interrupted{cancellation, [handle = cancellable.get()]() { g_cancellable_cancel(handle); }};
    GError* error                            = nullptr;
    const QByteArray credentialRecordIdBytes = credentialRecordId.toUtf8();
    gchar* password = secret_password_lookup_sync(tokenSchema(), cancellable.get(), &error, "credential-record-id",
                                                  credentialRecordIdBytes.constData(), nullptr);
    if(error) {
        return {.token = {}, .error = unavailableError(error)};
    }
    if(!password) {
        return {};
    }

    TokenLookup lookup{.token = QString::fromUtf8(password), .error = {}};
    secret_password_free(password);
    return lookup;
}

QString SecretServiceCredentialStore::writeToken(const QString& credentialRecordId, const QString& token,
                                                 std::stop_token cancellation)
{
    const auto cancellable
        = std::unique_ptr<GCancellable, decltype(&g_object_unref)>{g_cancellable_new(), &g_object_unref};
    std::stop_callback interrupted{cancellation, [handle = cancellable.get()]() { g_cancellable_cancel(handle); }};
    GError* error                            = nullptr;
    const QByteArray credentialRecordIdBytes = credentialRecordId.toUtf8();
    const QByteArray value                   = token.toUtf8();
    const gboolean stored                    = secret_password_store_sync(
        tokenSchema(), SECRET_COLLECTION_DEFAULT, "Fooyin Jellyfin access token", value.constData(), cancellable.get(),
        &error, "credential-record-id", credentialRecordIdBytes.constData(), nullptr);
    if(error || !stored) {
        return unavailableError(error);
    }
    return {};
}

QString SecretServiceCredentialStore::removeToken(const QString& credentialRecordId, std::stop_token cancellation)
{
    const auto cancellable
        = std::unique_ptr<GCancellable, decltype(&g_object_unref)>{g_cancellable_new(), &g_object_unref};
    std::stop_callback interrupted{cancellation, [handle = cancellable.get()]() { g_cancellable_cancel(handle); }};
    GError* error                            = nullptr;
    const QByteArray credentialRecordIdBytes = credentialRecordId.toUtf8();
    secret_password_clear_sync(tokenSchema(), cancellable.get(), &error, "credential-record-id",
                               credentialRecordIdBytes.constData(), nullptr);
    if(error) {
        return unavailableError(error);
    }
    return {};
}
} // namespace Fooyin::Jellyfin
