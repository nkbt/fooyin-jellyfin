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
#include "jellyfintrack.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <filesystem>
#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
namespace Fooyin::Jellyfin::Testing {
namespace {
QJsonObject validCredentialState()
{
    const QString sourceKey          = Fooyin::Jellyfin::sourceKey(u"server1"_s, u"user1"_s);
    const QString credentialRecordId = sourceKey + u"/00000000-0000-4000-8000-000000000001"_s;
    return {{u"acceptedConnection"_s, QJsonObject{{u"serverUrl"_s, u"https://synthetic.invalid/jf"_s},
                                                  {u"serverId"_s, u"server1"_s},
                                                  {u"userId"_s, u"user1"_s},
                                                  {u"userName"_s, u"Synthetic"_s},
                                                  {u"deviceId"_s, u"committed-device-id"_s},
                                                  {u"credentialRecordId"_s, credentialRecordId}}},
            {u"sourceKeys"_s, QJsonArray{sourceKey}},
            {u"credentialRecords"_s, QJsonArray{}}};
}
QByteArray readCredentialStateBytes(const QString& credentialStatePath)
{
    QFile file{credentialStatePath};
    if(!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}
} // namespace

TEST(JellyfinCredentialStateTest, MissingFileAndCheckedCommitReconstructThroughIndependentFileReads)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString credentialStatePath = directory.filePath(u"jellyfin/credential-state.json"_s);
    FileCredentialStateStore credentialStateStore{credentialStatePath};
    const auto initial = credentialStateStore.load();
    EXPECT_FALSE(initial.state);
    EXPECT_TRUE(initial.error.isEmpty());
    QFile unrelated{directory.filePath(u"host.ini"_s)};
    ASSERT_TRUE(unrelated.open(QIODevice::WriteOnly));
    ASSERT_EQ(unrelated.write("unchanged-host-preference"), 25);
    unrelated.close();
    const auto state = validCredentialState();
    ASSERT_TRUE(credentialStateStore.commit(state).isEmpty());
    const QByteArray bytes = readCredentialStateBytes(credentialStatePath);
    EXPECT_EQ(QJsonDocument::fromJson(bytes).object(), state);
    FileCredentialStateStore reconstructed{credentialStatePath};
    const auto loaded = reconstructed.load();
    ASSERT_TRUE(loaded.state);
    EXPECT_TRUE(loaded.error.isEmpty());
    EXPECT_EQ(*loaded.state, state);
    EXPECT_EQ(loaded.state->value(u"acceptedConnection"_s).toObject().value(u"deviceId"_s).toString(),
              u"committed-device-id"_s);
    EXPECT_EQ(readCredentialStateBytes(unrelated.fileName()), "unchanged-host-preference");
}

TEST(JellyfinCredentialStateTest, MalformedAndUnreadableStatesNeverBecomeAnAuthoritativeEmptyDocument)
{
    QTemporaryDir directory;
    const QString credentialStatePath = directory.filePath(u"credential-state.json"_s);
    QFile file{credentialStatePath};
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write("{"), 1);
    file.close();
    FileCredentialStateStore malformed{credentialStatePath};
    const auto malformedLoad = malformed.load();
    EXPECT_FALSE(malformedLoad.state);
    EXPECT_FALSE(malformedLoad.error.isEmpty());
    FileCredentialStateStore unreadable{directory.path()};
    const auto unreadableLoad = unreadable.load();
    EXPECT_FALSE(unreadableLoad.state);
    EXPECT_FALSE(unreadableLoad.error.isEmpty());
    FileCredentialStateStore blockedParent{credentialStatePath + u"/child.json"_s};
    const auto blockedLoad = blockedParent.load();
    EXPECT_FALSE(blockedLoad.state);
    EXPECT_FALSE(blockedLoad.error.isEmpty());
    const QString danglingPath = directory.filePath(u"dangling-state.json"_s);
    std::error_code error;
    std::filesystem::create_symlink(std::filesystem::path{directory.filePath(u"missing-state.json"_s).toStdU16String()},
                                    std::filesystem::path{danglingPath.toStdU16String()}, error);
    ASSERT_FALSE(error);
    FileCredentialStateStore dangling{danglingPath};
    const auto danglingLoad = dangling.load();
    EXPECT_FALSE(danglingLoad.state);
    EXPECT_FALSE(danglingLoad.error.isEmpty());
}

TEST(JellyfinCredentialStateTest, ActualWriterFailureKeepsPriorCommittedBytesForIndependentReconstruction)
{
    QTemporaryDir directory;
    const QString credentialStatePath = directory.filePath(u"metadata/state.json"_s);
    FileCredentialStateStore credentialStateStore{credentialStatePath};
    const auto initial = validCredentialState();
    ASSERT_TRUE(credentialStateStore.commit(initial).isEmpty());
    const QByteArray bytes = readCredentialStateBytes(credentialStatePath);
    QDir root{directory.path()};
    ASSERT_TRUE(root.rename(u"metadata"_s, u"retained"_s));
    QFile blocked{directory.filePath(u"metadata"_s)};
    ASSERT_TRUE(blocked.open(QIODevice::WriteOnly));
    ASSERT_EQ(blocked.write("regular-file-parent"), 19);
    blocked.close();
    EXPECT_FALSE(credentialStateStore.commit({}).isEmpty());
    EXPECT_EQ(readCredentialStateBytes(directory.filePath(u"retained/state.json"_s)), bytes);
    ASSERT_TRUE(blocked.remove());
    ASSERT_TRUE(root.rename(u"retained"_s, u"metadata"_s));
    FileCredentialStateStore reconstructed{credentialStatePath};
    const auto loaded = reconstructed.load();
    ASSERT_TRUE(loaded.state);
    EXPECT_EQ(*loaded.state, initial);
}

TEST(JellyfinCredentialStateTest, CrossSourceRecordBindingAndUnexpectedFieldsCannotReplaceCommittedState)
{
    QTemporaryDir directory;
    const QString credentialStatePath = directory.filePath(u"state.json"_s);
    FileCredentialStateStore credentialStateStore{credentialStatePath};
    auto state = validCredentialState();
    ASSERT_TRUE(credentialStateStore.commit(state).isEmpty());
    const auto bytes                 = readCredentialStateBytes(credentialStatePath);
    auto acceptedConnection          = state.value(u"acceptedConnection"_s).toObject();
    const QString credentialRecordId = sourceKey(u"server1"_s, u"user2"_s) + u"/00000000-0000-4000-8000-000000000001"_s;
    acceptedConnection.insert(u"credentialRecordId"_s, credentialRecordId);
    state.insert(u"acceptedConnection"_s, acceptedConnection);
    EXPECT_FALSE(credentialStateStore.commit(state).isEmpty());
    EXPECT_EQ(readCredentialStateBytes(credentialStatePath), bytes);
    state = validCredentialState();
    state.insert(u"token"_s, u"synthetic-not-to-be-persisted"_s);
    EXPECT_FALSE(credentialStateStore.commit(state).isEmpty());
    EXPECT_EQ(readCredentialStateBytes(credentialStatePath), bytes);
}
} // namespace Fooyin::Jellyfin::Testing
