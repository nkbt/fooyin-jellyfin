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

#include "jellyfinaccess.h"
#include "jellyfinaccount.h"
#include "jellyfincredentialstatestore.h"
#include "jellyfinpage.h"
#include "testsupport.h"

#include <gui/guiconstants.h>
#include <utils/settings/settingsmanager.h>
#include <utils/settings/settingsmodel.h>

#include <QNetworkAccessManager>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

// The host defines this category in its utils library without exporting it.
Q_LOGGING_CATEGORY(SETTINGS, "fy.settings")

namespace Fooyin::Jellyfin::Testing {
namespace {
class NativeIntegrationPage : public SettingsPage
{
public:
    explicit NativeIntegrationPage(SettingsManager* settings)
        : SettingsPage{settings->settingsDialog(), nullptr}
    {
        setId(Constants::Page::ShellIntegration);
        setName(u"System"_s);
        setCategory({u"Integrations"_s, u"System"_s});
    }
};
} // namespace

TEST(JellyfinSettingsPageTest, JoinsTheNativeIntegrationsCategory)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    SettingsManager settings{dir.filePath(u"settings.ini"_s)};
    StubMusicLibrary library;
    QNetworkAccessManager network;
    Account account{&network,
                    &library,
                    &settings,
                    std::make_shared<FileCredentialStateStore>(dir.filePath(u"credential-state.json"_s)),
                    std::make_shared<MemoryCredentialStore>(),
                    std::make_shared<AccessRegistry>(),
                    20ms,
                    nullptr};
    NativeIntegrationPage nativePage{&settings};
    JellyfinPage jellyfinPage{&account, &settings, nullptr};

    SettingsModel model;
    model.setPages({&nativePage, &jellyfinPage});

    ASSERT_EQ(model.rowCount({}), 1);
    const QModelIndex integrations = model.index(0, 0, {});
    EXPECT_EQ(model.data(integrations, Qt::DisplayRole).toString(), u"Integrations"_s);
    EXPECT_EQ(model.rowCount(integrations), 2);

    const SettingsCategory* jellyfinCategory = model.categoryForPage(jellyfinPage.id());
    ASSERT_NE(jellyfinCategory, nullptr);
    EXPECT_EQ(jellyfinCategory->name, u"Jellyfin"_s);
    EXPECT_EQ(model.indexForCategory(jellyfinCategory->id).parent(), integrations);
    const SettingsCategory* nativeCategory = model.categoryForPage(nativePage.id());
    ASSERT_NE(nativeCategory, nullptr);
    EXPECT_EQ(model.indexForCategory(nativeCategory->id).parent(), integrations);
}
} // namespace Fooyin::Jellyfin::Testing
