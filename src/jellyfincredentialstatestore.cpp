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

#include "jellyfincredentialstatestore.h"
#include "jellyfinrequest.h"
#include "jellyfintrack.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <filesystem>

using namespace Qt::StringLiterals;
namespace Fooyin::Jellyfin {
namespace {
bool validSourceKey(const QString& sourceKey)
{
    static const QRegularExpression pattern{u"^[0-9a-f]{32}$"_s};
    return pattern.match(sourceKey).hasMatch();
}

bool validCredentialRecordId(const QString& sourceKey, const QString& credentialRecordId)
{
    if(!validSourceKey(sourceKey) || !credentialRecordId.startsWith(sourceKey + u"/"_s)) {
        return false;
    }
    const auto uuid = QUuid::fromString(credentialRecordId.sliced(33));
    return !uuid.isNull() && credentialRecordId.sliced(33) == uuid.toString(QUuid::WithoutBraces);
}

bool validState(const QJsonObject& state)
{
    for(const QString& key : state.keys()) {
        if(key != "acceptedConnection"_L1 && key != "credentialRecords"_L1 && key != "sourceKeys"_L1) {
            return false;
        }
    }
    if(state.contains(u"acceptedConnection"_s) && !state.value(u"acceptedConnection"_s).isObject()) {
        return false;
    }
    const auto accepted = state.value(u"acceptedConnection"_s).toObject();
    if(!accepted.isEmpty()) {
        if(!accepted.value(u"serverUrl"_s).isString() || !accepted.value(u"serverId"_s).isString()
           || !accepted.value(u"userId"_s).isString() || !accepted.value(u"userName"_s).isString()
           || !accepted.value(u"deviceId"_s).isString() || !accepted.value(u"credentialRecordId"_s).isString()) {
            return false;
        }
        for(const QString& key : accepted.keys()) {
            if(key != "serverUrl"_L1 && key != "serverId"_L1 && key != "userId"_L1 && key != "userName"_L1
               && key != "deviceId"_L1 && key != "credentialRecordId"_L1) {
                return false;
            }
        }
        if(!serverUrlFromInput(accepted.value(u"serverUrl"_s).toString()).isValid()
           || !isValidItemId(accepted.value(u"serverId"_s).toString())
           || !isValidItemId(accepted.value(u"userId"_s).toString())
           || accepted.value(u"deviceId"_s).toString().isEmpty()
           || !validCredentialRecordId(
               sourceKey(accepted.value(u"serverId"_s).toString(), accepted.value(u"userId"_s).toString()),
               accepted.value(u"credentialRecordId"_s).toString())) {
            return false;
        }
    }
    if(state.contains(u"sourceKeys"_s) && !state.value(u"sourceKeys"_s).isArray()) {
        return false;
    }
    for(const auto& value : state.value(u"sourceKeys"_s).toArray()) {
        if(!value.isString() || !validSourceKey(value.toString())) {
            return false;
        }
    }
    if(state.contains(u"credentialRecords"_s) && !state.value(u"credentialRecords"_s).isArray()) {
        return false;
    }
    QSet<QString> credentialRecordIds;
    for(const auto& value : state.value(u"credentialRecords"_s).toArray()) {
        if(!value.isObject()) {
            return false;
        }
        const auto record                = value.toObject();
        const QString credentialRecordId = record.value(u"credentialRecordId"_s).toString();
        if(record.size() != 2 || !record.value(u"sourceKey"_s).isString()
           || !record.value(u"credentialRecordId"_s).isString()
           || !validCredentialRecordId(record.value(u"sourceKey"_s).toString(), credentialRecordId)
           || credentialRecordIds.contains(credentialRecordId)) {
            return false;
        }
        credentialRecordIds.insert(credentialRecordId);
    }
    return true;
}
QString loadError()
{
    return QCoreApplication::translate("Jellyfin",
                                       "Cannot read Jellyfin session metadata. Repair its file before signing in.");
}
QString commitError()
{
    return QCoreApplication::translate(
        "Jellyfin", "Cannot save Jellyfin session metadata. Check profile permissions and free space.");
}
} // namespace

FileCredentialStateStore::FileCredentialStateStore(QString credentialStatePath)
    : m_credentialStatePath{std::move(credentialStatePath)}
{ }

CredentialStateLoad FileCredentialStateStore::load()
{
    if(!QDir::isAbsolutePath(m_credentialStatePath)) {
        return {.state = {}, .error = loadError()};
    }
    std::error_code error;
    const std::filesystem::path credentialStatePath{m_credentialStatePath.toStdU16String()};
    const auto entry = std::filesystem::symlink_status(credentialStatePath, error);
    if(error == std::errc::no_such_file_or_directory
       || (!error && entry.type() == std::filesystem::file_type::not_found)) {
        return {.state = {}, .error = {}};
    }
    if(error) {
        return {.state = {}, .error = loadError()};
    }
    const auto status = std::filesystem::status(credentialStatePath, error);
    if(error || !std::filesystem::is_regular_file(status)) {
        return {.state = {}, .error = loadError()};
    }
    QFile file{m_credentialStatePath};
    if(!file.open(QIODevice::ReadOnly)) {
        return {.state = {}, .error = loadError()};
    }
    const QByteArray bytes = file.readAll();
    if(file.error() != QFileDevice::NoError) {
        return {.state = {}, .error = loadError()};
    }
    const auto document = QJsonDocument::fromJson(bytes);
    if(!document.isObject() || !validState(document.object())) {
        return {.state = {}, .error = loadError()};
    }
    return {.state = document.object(), .error = {}};
}

QString FileCredentialStateStore::commit(const QJsonObject& state)
{
    if(!QDir::isAbsolutePath(m_credentialStatePath) || !validState(state)) {
        return commitError();
    }
    const QByteArray bytes = QJsonDocument{state}.toJson(QJsonDocument::Compact);
    if(!QFileInfo{m_credentialStatePath}.absoluteDir().mkpath(u"."_s)) {
        return commitError();
    }
    QSaveFile file{m_credentialStatePath};
    file.setDirectWriteFallback(false);
    if(!file.open(QIODevice::WriteOnly)) {
        return commitError();
    }
    if(file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return commitError();
    }
    return file.commit() ? QString{} : commitError();
}
} // namespace Fooyin::Jellyfin
