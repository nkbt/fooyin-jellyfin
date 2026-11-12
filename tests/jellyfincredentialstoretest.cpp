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
#include "testsupport.h"

#include <core/accountstore.h>
#include <utils/database/dbconnectionhandler.h>
#include <utils/database/dbconnectionprovider.h>
#include <utils/database/dbquery.h>

#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <future>

using namespace Qt::StringLiterals;
using namespace Fooyin;
using namespace Fooyin::Jellyfin;

TEST(JellyfinCredentialStoreTest, PersistsIsolatedRecordsThroughTheHostAccountStoreAcrossThreads)
{
    Fooyin::Jellyfin::Testing::LogCapture logs;
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    DbConnection::DbParams params;
    params.type = u"QSQLITE"_s;
    params.filePath = directory.filePath(u"fooyin.db"_s);
    const auto dbPool = DbConnectionPool::create(params, u"jellyfin-accounts"_s);
    ASSERT_TRUE(dbPool);
    const DbConnectionHandler connection{dbPool};
    ASSERT_TRUE(connection.hasConnection());
    const DbConnectionProvider provider{dbPool};
    DbQuery createSettings{provider.db(), u"CREATE TABLE Settings (Name TEXT PRIMARY KEY, Value TEXT)"_s};
    ASSERT_TRUE(createSettings.exec());
    AccountCredentialStore store{dbPool};
    EXPECT_FALSE(store.readToken(u"first"_s, {}).token);
    EXPECT_TRUE(store.readToken(u"first"_s, {}).error.isEmpty());
    ASSERT_TRUE(store.writeToken(u"first"_s, u"synthetic-token"_s, {}).isEmpty());
    ASSERT_TRUE(store.writeToken(u"second"_s, u"other-token"_s, {}).isEmpty());
    const AccountStore accountStore{dbPool};
    const auto stored = accountStore.load(u"Jellyfin/first"_s, {u"AccessToken"_s});
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->at(u"AccessToken"_s), u"synthetic-token"_s);
    AccountCredentialStore reopened{dbPool};
    EXPECT_EQ(reopened.readToken(u"first"_s, {}).token, u"synthetic-token"_s);
    auto worker = std::async(std::launch::async, [&]() {
        return store.writeToken(u"first"_s, u"replacement-token"_s, {});
    });
    EXPECT_TRUE(worker.get().isEmpty());
    EXPECT_EQ(store.readToken(u"first"_s, {}).token, u"replacement-token"_s);
    std::stop_source cancellation;
    cancellation.request_stop();
    EXPECT_FALSE(store.readToken(u"first"_s, cancellation.get_token()).error.isEmpty());
    EXPECT_FALSE(store.writeToken(u"first"_s, u"cancelled-token"_s, cancellation.get_token()).isEmpty());
    EXPECT_FALSE(store.removeToken(u"first"_s, cancellation.get_token()).isEmpty());
    EXPECT_EQ(store.readToken(u"first"_s, {}).token, u"replacement-token"_s);
    ASSERT_TRUE(store.removeToken(u"first"_s, {}).isEmpty());
    EXPECT_FALSE(store.readToken(u"first"_s, {}).token);
    EXPECT_TRUE(store.removeToken(u"first"_s, {}).isEmpty());
    EXPECT_EQ(store.readToken(u"second"_s, {}).token, u"other-token"_s);
    DbQuery rejectCredential{provider.db(), u"CREATE TRIGGER reject_credential BEFORE INSERT ON Settings "
                                           "WHEN NEW.Name = 'Accounts/Jellyfin/refused' "
                                           "BEGIN SELECT RAISE(ABORT, 'synthetic refusal'); END"_s};
    ASSERT_TRUE(rejectCredential.exec());
    EXPECT_FALSE(store.writeToken(u"refused"_s, u"must-not-enter-logs"_s, {}).isEmpty());
    EXPECT_FALSE(logs.contains(u"must-not-enter-logs"_s));
    DbQuery dropSettings{provider.db(), u"DROP TABLE Settings"_s};
    ASSERT_TRUE(dropSettings.exec());
    EXPECT_FALSE(store.readToken(u"second"_s, {}).error.isEmpty());
    EXPECT_FALSE(store.writeToken(u"second"_s, u"rejected-token"_s, {}).isEmpty());
    EXPECT_FALSE(store.removeToken(u"second"_s, {}).isEmpty());
    EXPECT_FALSE(logs.contains(u"synthetic-token"_s));
    EXPECT_FALSE(logs.contains(u"replacement-token"_s));
    EXPECT_FALSE(logs.contains(u"other-token"_s));
    EXPECT_FALSE(logs.contains(u"rejected-token"_s));
}
