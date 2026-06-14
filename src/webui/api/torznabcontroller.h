/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2026  qBittorrentBB contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link this program with the OpenSSL project's "OpenSSL" library (or with
 * modified versions of it that use the same license as the "OpenSSL" library),
 * and distribute the linked executables. You must obey the GNU General Public
 * License in all respects for all of the code used other than "OpenSSL".  If you
 * modify file(s), you may extend this exception to your version of the file(s),
 * but you are not obligated to do so. If you do not wish to do so, delete this
 * exception statement from your version.
 */

#pragma once

#include <QByteArray>

#include "apicontroller.h"

// Torznab indexer endpoint over the local DHT torrent index, so external tools
// (e.g. Prowlarr) can search/pull harvested torrents.
//
// qBittorrent's WebUI routes only /api/v2/<scope>/<action>, so a single action
// (`/api/v2/torznab/index`) handles the whole Torznab surface, dispatching on the
// `t` query param (caps / search / movie / tv).
class TorznabController final : public APIController
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(TorznabController)

public:
    using APIController::APIController;

private slots:
    void indexAction();

private:
    QByteArray buildCaps() const;
    QByteArray buildResults() const;
};
