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

#include "harvestcontenthandler.h"

#include <QFuture>

#include "base/path.h"

HarvestContentHandler::HarvestContentHandler(const BitTorrent::TorrentInfo &torrentInfo, QObject *parent)
    : BitTorrent::TorrentContentHandler(parent)
    , m_torrentInfo {torrentInfo}
{
}

bool HarvestContentHandler::hasMetadata() const
{
    return m_torrentInfo.isValid();
}

int HarvestContentHandler::filesCount() const
{
    return m_torrentInfo.filesCount();
}

Path HarvestContentHandler::filePath(const int index) const
{
    return m_torrentInfo.filePath(index);
}

qlonglong HarvestContentHandler::fileSize(const int index) const
{
    return m_torrentInfo.fileSize(index);
}

Path HarvestContentHandler::actualStorageLocation() const
{
    return {};
}

Path HarvestContentHandler::actualFilePath(const int fileIndex) const
{
    return m_torrentInfo.filePath(fileIndex);
}

QList<BitTorrent::DownloadPriority> HarvestContentHandler::filePriorities() const
{
    return QList<BitTorrent::DownloadPriority>(m_torrentInfo.filesCount(), BitTorrent::DownloadPriority::Normal);
}

QList<qreal> HarvestContentHandler::filesProgress() const
{
    return QList<qreal>(m_torrentInfo.filesCount(), 0);
}

QFuture<QList<qreal>> HarvestContentHandler::fetchAvailableFileFractions() const
{
    return QtFuture::makeReadyValueFuture(QList<qreal>(m_torrentInfo.filesCount(), 0));
}

void HarvestContentHandler::renameFile(int, const Path &)
{
    // Not-yet-added torrent: nothing to rename.
}

void HarvestContentHandler::prioritizeFiles(const QList<BitTorrent::DownloadPriority> &)
{
    // Not-yet-added torrent: priorities are not applicable.
}

void HarvestContentHandler::flushCache() const
{
}
