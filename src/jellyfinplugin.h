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

#include <core/engine/inputplugin.h>
#include <core/plugins/coreplugin.h>
#include <core/plugins/plugin.h>
#include <gui/plugins/guiplugin.h>

#include <memory>

class QNetworkAccessManager;

namespace Fooyin {
class AudioLoader;
class RemoteIoService;

namespace Jellyfin {
class Account;
class AccessRegistry;
class PlaybackReporter;
class PlaylistMirror;

class JellyfinPlugin : public QObject,
                       public Plugin,
                       public CorePlugin,
                       public GuiPlugin,
                       public InputPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID FOOYIN_PLUGIN_IID FILE "jellyfin.json")
    Q_INTERFACES(Fooyin::Plugin Fooyin::CorePlugin Fooyin::GuiPlugin Fooyin::InputPlugin)

public:
    JellyfinPlugin();
    ~JellyfinPlugin() override;

    void initialise(const CorePluginContext& context) override;
    void initialise(const GuiPluginContext& context) override;
    void shutdown() override;

    [[nodiscard]] QString inputName() const override;
    [[nodiscard]] InputCreator inputCreator() const override;

private:
    std::shared_ptr<AccessRegistry> m_access;
    std::weak_ptr<AudioLoader> m_audioLoader;
    std::shared_ptr<RemoteIoService> m_remoteIo;
    SettingsManager* m_settings{nullptr};
    QNetworkAccessManager* m_network{nullptr};
    Account* m_account{nullptr};
    PlaybackReporter* m_playbackReporter{nullptr};
    std::unique_ptr<PlaylistMirror> m_playlistMirror;
};
} // namespace Jellyfin
} // namespace Fooyin
