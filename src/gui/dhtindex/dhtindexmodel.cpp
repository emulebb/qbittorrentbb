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

#include "dhtindexmodel.h"

#include <QDateTime>

#include "base/bittorrent/session.h"
#include "base/utils/misc.h"

using namespace Qt::Literals::StringLiterals;

namespace
{
    const int PAGE = 200;                    // rows fetched per fetchMore() call
    const qint64 FRESH_MS = 30 * 60 * 1000;  // re-scrape a row at most this often
    const QString EM_DASH = QChar(0x2014);   // shown for a never-scraped seeds/peers cell
}

DHTIndexModel::DHTIndexModel(QObject *parent)
    : QAbstractTableModel(parent)
{
    connect(BitTorrent::Session::instance(), &BitTorrent::Session::dhtTrackerScrapeUpdated
            , this, &DHTIndexModel::onTrackerScrapeUpdated);
}

QString DHTIndexModel::displayContentType(const QString &contentType)
{
    if (contentType == u"disk-image"_s)
        return tr("Disk image");
    if (contentType == u"mixed"_s)
        return tr("Mixed");
    if (contentType == u"video"_s)
        return tr("Video");
    if (contentType == u"audio"_s)
        return tr("Audio");
    if (contentType == u"archive"_s)
        return tr("Archive");
    if (contentType == u"document"_s)
        return tr("Document");
    if (contentType == u"image"_s)
        return tr("Image");
    if (contentType == u"software"_s)
        return tr("Software");
    if (contentType == u"subtitle"_s)
        return tr("Subtitle");
    return tr("Other");
}

int DHTIndexModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

int DHTIndexModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : COL_COUNT;
}

QVariant DHTIndexModel::data(const QModelIndex &index, const int role) const
{
    if (!index.isValid() || (index.row() < 0) || (index.row() >= m_rows.size()))
        return {};

    const int col = index.column();

    if (role == Qt::TextAlignmentRole)
    {
        switch (col)
        {
        case COL_SIZE:
        case COL_FILES:
        case COL_SEEDS:
        case COL_PEERS:
        case COL_SIGHTINGS:
            return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        default:
            return {};
        }
    }

    if (role != Qt::DisplayRole)
        return {};

    const BitTorrent::HarvestSearchResult &row = m_rows[index.row()];
    switch (col)
    {
    case COL_NAME:
        return row.name;
    case COL_CONTENT:
        return displayContentType(row.contentType);
    case COL_SIZE:
        return Utils::Misc::friendlyUnit(row.size);
    case COL_FILES:
        return QString::number(row.fileCount);
    case COL_SEEDS:
        return (row.trackerSeeds < 0) ? EM_DASH : QString::number(row.trackerSeeds);
    case COL_PEERS:
        return (row.trackerLeechers < 0) ? EM_DASH : QString::number(row.trackerLeechers);
    case COL_SIGHTINGS:
        return QString::number(row.sightings);
    case COL_FIRSTSEEN:
        return QDateTime::fromMSecsSinceEpoch(row.firstSeenMs).toString(u"yyyy-MM-dd hh:mm"_s);
    case COL_LASTSEEN:
        return QDateTime::fromMSecsSinceEpoch(row.lastSeenMs).toString(u"yyyy-MM-dd hh:mm"_s);
    default:
        return {};
    }
}

QVariant DHTIndexModel::headerData(const int section, const Qt::Orientation orientation, const int role) const
{
    if ((orientation != Qt::Horizontal) || (role != Qt::DisplayRole))
        return {};

    switch (section)
    {
    case COL_NAME: return tr("Name");
    case COL_CONTENT: return tr("Content");
    case COL_SIZE: return tr("Size");
    case COL_FILES: return tr("Files");
    case COL_SEEDS: return tr("Seeds");
    case COL_PEERS: return tr("Peers");
    case COL_SIGHTINGS: return tr("Sightings");
    case COL_FIRSTSEEN: return tr("First seen");
    case COL_LASTSEEN: return tr("Last seen");
    default: return {};
    }
}

Qt::ItemFlags DHTIndexModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

bool DHTIndexModel::canFetchMore(const QModelIndex &parent) const
{
    if (parent.isValid())
        return false;
    return m_rows.size() < m_total;
}

void DHTIndexModel::fetchMore(const QModelIndex &parent)
{
    if (parent.isValid())
        return;

    const int offset = static_cast<int>(m_rows.size());
    const BitTorrent::HarvestSearchPage page = fetchPage(offset);
    if (page.items.isEmpty())
    {
        // The count snapshot promised more than the store returned; clamp so
        // canFetchMore() stops asking.
        m_total = m_rows.size();
        return;
    }

    beginInsertRows({}, offset, offset + static_cast<int>(page.items.size()) - 1);
    m_rows.append(page.items);
    if (page.total > m_total)
        m_total = page.total;
    endInsertRows();

    scrapeStale(page.items);
}

void DHTIndexModel::setFilter(const QString &query, const QString &contentType)
{
    m_query = query;
    m_contentType = contentType;
    reload();
}

void DHTIndexModel::setSort(const BitTorrent::HarvestSortColumn sortColumn, const bool descending)
{
    m_sortColumn = sortColumn;
    m_descending = descending;
    reload();
}

void DHTIndexModel::reload()
{
    beginResetModel();
    const BitTorrent::HarvestSearchPage page = fetchPage(0);
    m_rows = page.items;
    m_total = page.total;
    endResetModel();

    scrapeStale(m_rows);
}

BitTorrent::HarvestSortColumn DHTIndexModel::sortColumnForView(const int viewColumn)
{
    using SC = BitTorrent::HarvestSortColumn;
    switch (viewColumn)
    {
    case COL_NAME: return SC::Name;
    case COL_CONTENT: return SC::Content;
    case COL_SIZE: return SC::Size;
    case COL_FILES: return SC::Files;
    case COL_SEEDS: return SC::Seeds;
    case COL_PEERS: return SC::Leechers;
    case COL_SIGHTINGS: return SC::Sightings;
    case COL_FIRSTSEEN: return SC::FirstSeen;
    case COL_LASTSEEN: return SC::LastSeen;
    default: return SC::Default;
    }
}

QString DHTIndexModel::query() const
{
    return m_query;
}

BitTorrent::HarvestSearchPage DHTIndexModel::fetchPage(const int offset) const
{
    auto *session = BitTorrent::Session::instance();
    if (m_contentType.isEmpty())
    {
        return m_query.isEmpty()
                ? session->recentDHTIndex(PAGE, offset, m_sortColumn, m_descending)
                : session->searchDHTIndex(m_query, PAGE, offset, m_sortColumn, m_descending);
    }
    return m_query.isEmpty()
            ? session->recentDHTIndexByType(m_contentType, PAGE, offset, m_sortColumn, m_descending)
            : session->searchDHTIndexByType(m_query, m_contentType, PAGE, offset, m_sortColumn, m_descending);
}

QString DHTIndexModel::infoHashForIndex(const QModelIndex &index) const
{
    if (!index.isValid() || (index.row() < 0) || (index.row() >= m_rows.size()))
        return {};
    return m_rows[index.row()].infoHashV1;
}

void DHTIndexModel::scrapeStale(const QList<BitTorrent::HarvestSearchResult> &rows)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QStringList stale;
    for (const BitTorrent::HarvestSearchResult &row : rows)
    {
        if ((row.trackerScrapeMs == 0) || ((now - row.trackerScrapeMs) > FRESH_MS))
            stale.append(row.infoHashV1);
    }
    if (!stale.isEmpty())
        BitTorrent::Session::instance()->scrapeTrackerFor(stale);
}

void DHTIndexModel::onTrackerScrapeUpdated(const QString &infoHashV1, const int seeds, const int leechers)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int r = 0; r < m_rows.size(); ++r)
    {
        if (m_rows[r].infoHashV1 != infoHashV1)
            continue;
        m_rows[r].trackerSeeds = seeds;
        m_rows[r].trackerLeechers = leechers;
        m_rows[r].trackerScrapeMs = now;
        emit dataChanged(index(r, COL_SEEDS), index(r, COL_PEERS));
        return;
    }
}
