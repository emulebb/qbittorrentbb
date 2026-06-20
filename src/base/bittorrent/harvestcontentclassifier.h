/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2026  qBittorrentBB contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#pragma once

#include <QList>
#include <QString>

#include "base/bittorrent/harveststore.h"

namespace BitTorrent
{
    QString classifyHarvestedContent(const QList<HarvestedFile> &files, qint64 totalSize);
}
