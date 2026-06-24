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

#include <QString>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QModelIndex;
class QPoint;
class QPushButton;
class QTableView;
class QTimer;

class DHTIndexModel;
class HarvestContentHandler;
class TorrentContentWidget;

// In-app search/browse view over the locally harvested BitTorrent DHT index. Results
// are a flat table; a filter panel on the left lists the detected content types (with
// counts) and filters the table when a type is selected, like qBittorrent's status
// filter on the transfer list. Table rows load lazily as the view is scrolled.
class DHTIndexWidget final : public QWidget
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(DHTIndexWidget)

public:
    explicit DHTIndexWidget(QWidget *parent = nullptr);

private slots:
    void search();
    void refreshStats();
    void onEnabledToggled(bool enabled);
    void onIntensityChanged(int presetIndex);
    void onFilterChanged();
    void onSelectionChanged();
    void onItemDoubleClicked(const QModelIndex &index);
    void downloadSelected();
    void showContextMenu(const QPoint &pos);

private:
    void rebuildFilter();          // refresh the left content-type list + counts
    QString currentFilterType() const;  // selected content type ("" = all)
    QString selectedInfoHash() const;
    QString selectedMagnet() const;

    QCheckBox *m_enableBox = nullptr;
    QComboBox *m_intensityBox = nullptr;
    QLabel *m_statsLabel = nullptr;
    QLabel *m_crawlStatsLabel = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QListWidget *m_filterList = nullptr;
    QTableView *m_table = nullptr;
    DHTIndexModel *m_model = nullptr;
    QPushButton *m_downloadButton = nullptr;
    TorrentContentWidget *m_content = nullptr;
    HarvestContentHandler *m_contentHandler = nullptr;
    QTimer *m_statsTimer = nullptr;
    QString m_query;
};
