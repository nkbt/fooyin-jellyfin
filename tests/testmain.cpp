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

#include <QApplication>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <gtest/gtest.h>

int main(int argc, char** argv)
{
    QTemporaryDir profile;
    if(!profile.isValid()) {
        return 2;
    }
    qputenv("HOME", profile.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", profile.filePath(QStringLiteral("config")).toUtf8());
    qputenv("XDG_DATA_HOME", profile.filePath(QStringLiteral("data")).toUtf8());
    qputenv("XDG_CACHE_HOME", profile.filePath(QStringLiteral("cache")).toUtf8());
    qputenv("XDG_STATE_HOME", profile.filePath(QStringLiteral("state")).toUtf8());
    QStandardPaths::setTestModeEnabled(true);
    QApplication app{argc, argv};
    QLoggingCategory::setFilterRules(QStringLiteral("fy.jellyfin.*.debug=true"));
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
