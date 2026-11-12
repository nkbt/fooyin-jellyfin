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

#include "jellyfinplugin.h"

#include "jellyfinaccess.h"
#include "jellyfinaccount.h"
#include "jellyfincatalogsync.h"
#include "jellyfincredentialstatestore.h"
#include "jellyfincredentialstore.h"
#include "jellyfinfavouritebanner.h"
#include "jellyfinfavouritesync.h"
#include "jellyfininput.h"
#include "jellyfinpage.h"
#include "jellyfinplaybackreporter.h"
#include "jellyfinplaylistmirror.h"

#include <core/engine/enginecontroller.h>
#include <core/network/networkaccessmanager.h>
#include <core/network/remoteioservice.h>
#include <core/playlist/playlisthandler.h>
#include <core/plugins/coreplugincontext.h>
#include <gui/plugins/guiplugincontext.h>
#include <gui/windowcontroller.h>
#include <utils/fypaths.h>

#include <QDir>
#include <QMainWindow>
#include <QNetworkAccessManager>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace Fooyin::Jellyfin {
namespace {
constexpr auto InputTimeout             = 30s;
constexpr auto QuickConnectPollInterval = 5s;
} // namespace

JellyfinPlugin::JellyfinPlugin()
    : m_access{std::make_shared<AccessRegistry>()}
{ }

JellyfinPlugin::~JellyfinPlugin()
{
    if(m_account) {
        shutdown();
    }
}

void JellyfinPlugin::initialise(const CorePluginContext& context)
{
    m_audioLoader = context.audioLoader;
    m_settings    = context.settingsManager;
    m_remoteIo    = std::make_shared<RemoteIoService>(context.networkAccess, m_settings);
    m_network     = new QNetworkAccessManager(this);
    const auto credentialStore = std::make_shared<AccountCredentialStore>(context.dbPool);
    m_account = new Account(m_network, context.library, m_settings,
                            std::make_shared<FileCredentialStateStore>(
                                QDir{Utils::configPath(u"jellyfin"_s)}.filePath(u"credential-state.json"_s)),
                            credentialStore, m_access, QuickConnectPollInterval, this);
    m_playbackReporter = new PlaybackReporter(m_network, m_access, this);
    m_playlistMirror   = std::make_unique<PlaylistMirror>(context.playlistHandler, m_settings);

    QObject::connect(m_account->catalogSync(), &CatalogSync::playlistsReady, this,
                     [this](const QString& sourceKey, const std::vector<MirroredPlaylist>& playlists) {
                         if(!m_playlistMirror->apply(sourceKey, playlists)) {
                             m_account->catalogSync()->fail(
                                 tr("Could not save Jellyfin playlists. Refresh the library to try again."));
                         }
                     });
    QObject::connect(context.playlistHandler, &PlaylistHandler::playlistsPopulated, m_account, &Account::restore,
                     Qt::SingleShotConnection);

    QObject::connect(context.engine, &EngineController::playbackOccurrenceStarted, m_playbackReporter,
                     &PlaybackReporter::occurrenceStarted);
    QObject::connect(context.engine, &EngineController::playbackOccurrenceUpdated, m_playbackReporter,
                     &PlaybackReporter::occurrenceUpdated);
    QObject::connect(context.engine, &EngineController::playbackOccurrenceEnded, m_playbackReporter,
                     &PlaybackReporter::occurrenceEnded);
}

void JellyfinPlugin::initialise(const GuiPluginContext& context)
{
    new JellyfinPage(m_account, m_settings, this);
    connectFavouriteBanner(m_account->favouriteSync(), context.windowController->mainWindow());
}

void JellyfinPlugin::shutdown()
{
    m_account->catalogSync()->cancel();
    m_account->favouriteSync()->release();
    m_playbackReporter->shutdown();
    m_access->clearAccess();
}

QString JellyfinPlugin::inputName() const
{
    return u"Jellyfin"_s;
}

InputCreator JellyfinPlugin::inputCreator() const
{
    InputCreator creator;
    creator.decoder = [access = m_access, audioLoader = m_audioLoader, remoteIo = m_remoteIo]() {
        return std::make_unique<Decoder>(access, audioLoader, remoteIo, InputTimeout);
    };
    creator.reader = [access = m_access, remoteIo = m_remoteIo]() {
        return std::make_unique<Reader>(access, remoteIo, InputTimeout);
    };
    return creator;
}
} // namespace Fooyin::Jellyfin

#include "moc_jellyfinplugin.cpp"
