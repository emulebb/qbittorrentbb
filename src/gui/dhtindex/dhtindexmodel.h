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

#include <QAbstractTableModel>
#include <QList>
#include <QString>

#include "base/bittorrent/harveststore.h"

// Flat, lazily-windowed table model over the local DHT index for the DHT Index tab.
// It holds a single page-grown list of rows for the current (search query, content-type
// filter) and fetches the next window only as the view scrolls near the end
// (canFetchMore/fetchMore) -- the "dynamic range" that keeps the table snappy no matter
// how large the index grows. The content-type filter is driven by the left-hand filter
// sidebar; an empty filter means "all types".
//
// Each newly fetched page is handed to Session::scrapeTrackerFor() for its seeders/
// leechers; results arrive via Session::dhtTrackerScrapeUpdated and update the Seeds/
// Peers cells in place. The model talks to BitTorrent::Session::instance() directly.
class DHTIndexModel final : public QAbstractTableModel
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(DHTIndexModel)

public:
    enum Column
    {
        COL_NAME = 0,
        COL_CONTENT,
        COL_SIZE,
        COL_FILES,
        COL_SEEDS,
        COL_PEERS,
        COL_SIGHTINGS,
        COL_FIRSTSEEN,
        COL_LASTSEEN,
        COL_COUNT
    };

    explicit DHTIndexModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool canFetchMore(const QModelIndex &parent) const override;
    void fetchMore(const QModelIndex &parent) override;

    // Reload the table for a (query, contentType) filter. Empty contentType = all types.
    void setFilter(const QString &query, const QString &contentType);
    QString query() const;

    // The infohash for a row index (empty if invalid), for selection/download.
    QString infoHashForIndex(const QModelIndex &index) const;

    // Human label for a stored content_type value (shared with the filter sidebar).
    static QString displayContentType(const QString &contentType);

private slots:
    void onTrackerScrapeUpdated(const QString &infoHashV1, int seeds, int leechers);

private:
    BitTorrent::HarvestSearchPage fetchPage(int offset) const;
    void scrapeStale(const QList<BitTorrent::HarvestSearchResult> &rows);

    QString m_query;          // current FTS query ("" = recent feed)
    QString m_contentType;    // current type filter ("" = all types)
    QList<BitTorrent::HarvestSearchResult> m_rows;
    qint64 m_total = 0;       // total rows for this filter (for canFetchMore)
};
