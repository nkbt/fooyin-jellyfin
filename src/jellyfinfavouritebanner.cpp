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

#include "jellyfinfavouritebanner.h"

#include "jellyfinfavouritesync.h"

#include <gui/widgets/messagebanner.h>

#include <QPointer>

namespace Fooyin::Jellyfin {
void connectFavouriteBanner(FavouriteSync* favourites, QWidget* parent)
{
    QObject::connect(favourites, &FavouriteSync::failed, parent,
                     [parent, banner = QPointer<MessageBanner>{}](const QString& message) mutable {
                         if(!banner) {
                             banner = new MessageBanner(parent);
                             QObject::connect(banner, &MessageBanner::closed, banner, &QObject::deleteLater);
                         }
                         banner->setText(message);
                     });
}
} // namespace Fooyin::Jellyfin
