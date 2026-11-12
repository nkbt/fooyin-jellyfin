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

#include <utils/database/dbconnectionhandler.h>

#include <QCoreApplication>

using namespace Qt::StringLiterals;

namespace Fooyin::Jellyfin {
AccountCredentialStore::AccountCredentialStore(DbConnectionPoolPtr dbPool)
    : m_dbPool{std::move(dbPool)}
    , m_accountStore{m_dbPool}
{ }

TokenLookup AccountCredentialStore::readToken(const QString& credentialRecordId, std::stop_token cancellation)
{
    if(cancellation.stop_requested()) {
        return {.token = {}, .error = QCoreApplication::translate("Jellyfin", "Credential operation cancelled.")};
    }
    const DbConnectionHandler connection{m_dbPool};
    if(!m_dbPool || !m_dbPool->hasThreadConnection()) {
        return {.token = {}, .error = QCoreApplication::translate("Jellyfin", "Cannot open the fooyin account database.")};
    }
    const auto values = m_accountStore.load(u"Jellyfin/"_s + credentialRecordId, {u"AccessToken"_s});
    if(!values) {
        return {.token = {}, .error = QCoreApplication::translate("Jellyfin", "Cannot read the Jellyfin account.")};
    }
    if(cancellation.stop_requested()) {
        return {.token = {}, .error = QCoreApplication::translate("Jellyfin", "Credential operation cancelled.")};
    }
    const auto token = values->find(u"AccessToken"_s);
    if(token == values->end() || token->second.isEmpty()) {
        return {};
    }
    return {.token = token->second, .error = {}};
}

QString AccountCredentialStore::writeToken(const QString& credentialRecordId, const QString& token,
                                          std::stop_token cancellation)
{
    if(cancellation.stop_requested()) {
        return QCoreApplication::translate("Jellyfin", "Credential operation cancelled.");
    }
    const DbConnectionHandler connection{m_dbPool};
    if(!m_dbPool || !m_dbPool->hasThreadConnection()) {
        return QCoreApplication::translate("Jellyfin", "Cannot open the fooyin account database.");
    }
    if(!m_accountStore.save(u"Jellyfin/"_s + credentialRecordId, {{u"AccessToken"_s, token}})) {
        return QCoreApplication::translate("Jellyfin", "Cannot save the Jellyfin account.");
    }
    return {};
}

QString AccountCredentialStore::removeToken(const QString& credentialRecordId, std::stop_token cancellation)
{
    if(cancellation.stop_requested()) {
        return QCoreApplication::translate("Jellyfin", "Credential operation cancelled.");
    }
    const DbConnectionHandler connection{m_dbPool};
    if(!m_dbPool || !m_dbPool->hasThreadConnection()) {
        return QCoreApplication::translate("Jellyfin", "Cannot open the fooyin account database.");
    }
    if(!m_accountStore.remove(u"Jellyfin/"_s + credentialRecordId, {u"AccessToken"_s})) {
        return QCoreApplication::translate("Jellyfin", "Cannot remove the Jellyfin account.");
    }
    return {};
}
} // namespace Fooyin::Jellyfin
