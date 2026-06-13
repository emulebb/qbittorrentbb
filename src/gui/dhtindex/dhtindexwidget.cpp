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

#include "dhtindexwidget.h"

#include <optional>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "base/bittorrent/addtorrentparams.h"
#include "base/bittorrent/session.h"
#include "base/bittorrent/torrentdescriptor.h"
#include "base/bittorrent/torrentinfo.h"
#include "base/utils/misc.h"
#include "gui/addnewtorrentdialog.h"
#include "gui/torrentcontentwidget.h"
#include "harvestcontenthandler.h"

using namespace Qt::Literals::StringLiterals;

namespace
{
    enum Column
    {
        COL_NAME = 0,
        COL_SIZE,
        COL_FILES,
        COL_SIGHTINGS,
        COL_LASTSEEN,
        COL_COUNT
    };

    const int SEARCH_LIMIT = 500;
    const int FEED_LIMIT = 200;  // capped live feed of most-recent indexed torrents

    // Reconstruct a torrent descriptor from a stored bencoded info-dict by wrapping
    // it back into a minimal torrent ("d4:info<info-dict>e").
    std::optional<BitTorrent::TorrentDescriptor> descriptorFromMetadata(const QByteArray &infoDict)
    {
        if (infoDict.isEmpty())
            return std::nullopt;

        const QByteArray buffer = QByteArrayLiteral("d4:info") + infoDict + QByteArrayLiteral("e");
        const auto descr = BitTorrent::TorrentDescriptor::load(buffer);
        if (!descr)
            return std::nullopt;
        return descr.value();
    }
}

DHTIndexWidget::DHTIndexWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);

    auto *topRow = new QHBoxLayout;
    m_enableBox = new QCheckBox(tr("Enable DHT harvesting"), this);
    m_enableBox->setChecked(BitTorrent::Session::instance()->isDHTHarvesterEnabled());
    m_enableBox->setToolTip(tr("Crawls the BitTorrent DHT and indexes discovered torrents into a local database."
                               " Requires binding to the VPN network interface (Tools > Options > Advanced)."));
    connect(m_enableBox, &QCheckBox::toggled, this, &DHTIndexWidget::onEnabledToggled);
    topRow->addWidget(m_enableBox);
    topRow->addStretch(1);
    m_statsLabel = new QLabel(this);
    topRow->addWidget(m_statsLabel);
    layout->addLayout(topRow);

    auto *searchRow = new QHBoxLayout;
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(tr("Search harvested torrents by name..."));
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &DHTIndexWidget::search);
    searchRow->addWidget(m_searchEdit);
    auto *searchButton = new QPushButton(tr("Search"), this);
    connect(searchButton, &QPushButton::clicked, this, &DHTIndexWidget::search);
    searchRow->addWidget(searchButton);
    layout->addLayout(searchRow);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(COL_COUNT);
    m_table->setHorizontalHeaderLabels({tr("Name"), tr("Size"), tr("Files"), tr("Sightings"), tr("Last seen")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(COL_NAME, QHeaderView::Stretch);
    connect(m_table, &QWidget::customContextMenuRequested, this, &DHTIndexWidget::showContextMenu);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &DHTIndexWidget::onSelectionChanged);

    // Lower pane: the standard qBittorrent content tree for the selected torrent.
    m_content = new TorrentContentWidget(this);

    auto *splitter = new QSplitter(Qt::Vertical, this);
    splitter->setChildrenCollapsible(false);
    splitter->addWidget(m_table);
    splitter->addWidget(m_content);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);

    auto *bottomRow = new QHBoxLayout;
    bottomRow->addStretch(1);
    m_downloadButton = new QPushButton(tr("Download"), this);
    m_downloadButton->setEnabled(false);
    connect(m_downloadButton, &QPushButton::clicked, this, &DHTIndexWidget::downloadSelected);
    bottomRow->addWidget(m_downloadButton);
    layout->addLayout(bottomRow);

    m_statsTimer = new QTimer(this);
    m_statsTimer->setInterval(2000);
    connect(m_statsTimer, &QTimer::timeout, this, &DHTIndexWidget::refreshStats);
    m_statsTimer->start();
    refreshStats();
}

void DHTIndexWidget::onEnabledToggled(const bool enabled)
{
    BitTorrent::Session::instance()->setDHTHarvesterEnabled(enabled);
}

void DHTIndexWidget::refreshStats()
{
    const BitTorrent::HarvestStats stats = BitTorrent::Session::instance()->dhtHarvestStats();
    m_statsLabel->setText(tr("Infohashes: %1  |  With metadata: %2  |  Sightings: %3")
            .arg(QString::number(stats.torrentCount), QString::number(stats.metadataCount)
                , QString::number(stats.sightingCount)));

    // Live feed: while no search is active, keep showing the most recent indexed
    // torrents as they come in (capped).
    if (m_searchEdit->text().trimmed().isEmpty())
        setRows(BitTorrent::Session::instance()->recentDHTIndex(FEED_LIMIT));
}

void DHTIndexWidget::search()
{
    const QString query = m_searchEdit->text().trimmed();
    if (query.isEmpty())
    {
        setRows(BitTorrent::Session::instance()->recentDHTIndex(FEED_LIMIT));
        return;
    }
    setRows(BitTorrent::Session::instance()->searchDHTIndex(query, SEARCH_LIMIT));
}

void DHTIndexWidget::setRows(const QList<BitTorrent::HarvestSearchResult> &results)
{
    // Preserve the current selection across refreshes of the live feed.
    const QString selected = selectedInfoHash();

    m_table->setRowCount(results.size());
    int row = 0;
    int selectedRow = -1;
    for (const BitTorrent::HarvestSearchResult &result : results)
    {
        auto *nameItem = new QTableWidgetItem(result.name);
        nameItem->setData(Qt::UserRole, result.infoHashV1);
        m_table->setItem(row, COL_NAME, nameItem);
        m_table->setItem(row, COL_SIZE, new QTableWidgetItem(Utils::Misc::friendlyUnit(result.size)));
        m_table->setItem(row, COL_FILES, new QTableWidgetItem(QString::number(result.fileCount)));
        m_table->setItem(row, COL_SIGHTINGS, new QTableWidgetItem(QString::number(result.sightings)));
        m_table->setItem(row, COL_LASTSEEN, new QTableWidgetItem(
                QDateTime::fromMSecsSinceEpoch(result.lastSeenMs).toString(u"yyyy-MM-dd hh:mm"_s)));
        if (!selected.isEmpty() && (result.infoHashV1 == selected))
            selectedRow = row;
        ++row;
    }

    if (selectedRow >= 0)
        m_table->selectRow(selectedRow);
}

void DHTIndexWidget::onSelectionChanged()
{
    if (m_contentHandler)
    {
        m_content->setContentHandler(nullptr);
        delete m_contentHandler;
        m_contentHandler = nullptr;
    }

    const QString infoHash = selectedInfoHash();
    const QByteArray infoDict = infoHash.isEmpty()
            ? QByteArray() : BitTorrent::Session::instance()->dhtTorrentMetadata(infoHash);
    if (const auto descr = descriptorFromMetadata(infoDict); descr && descr->info())
    {
        m_contentHandler = new HarvestContentHandler(descr->info().value(), this);
        m_content->setContentHandler(m_contentHandler);
        m_content->refresh();
    }

    // Allow downloading any selected torrent (metadata is fetched on add if absent).
    m_downloadButton->setEnabled(!infoHash.isEmpty());
}

void DHTIndexWidget::downloadSelected()
{
    const QString infoHash = selectedInfoHash();
    if (infoHash.isEmpty())
        return;

    std::optional<BitTorrent::TorrentDescriptor> descr =
            descriptorFromMetadata(BitTorrent::Session::instance()->dhtTorrentMetadata(infoHash));
    if (!descr)
    {
        // No stored metadata yet: fall back to a magnet, the add dialog fetches it.
        const auto parsed = BitTorrent::TorrentDescriptor::parse(selectedMagnet());
        if (!parsed)
            return;
        descr = parsed.value();
    }

    // Open the standard "Add new torrent" dialog so it behaves exactly like adding
    // any other torrent (content tree, save path, start option).
    auto *dlg = new AddNewTorrentDialog(descr.value(), {}, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &AddNewTorrentDialog::torrentAccepted, this
            , [](const BitTorrent::TorrentDescriptor &td, const BitTorrent::AddTorrentParams &ap)
    {
        BitTorrent::Session::instance()->addTorrent(td, ap);
    });
    dlg->show();
}

QString DHTIndexWidget::selectedInfoHash() const
{
    const QTableWidgetItem *item = m_table->item(m_table->currentRow(), COL_NAME);
    return item ? item->data(Qt::UserRole).toString() : QString();
}

QString DHTIndexWidget::selectedMagnet() const
{
    const QString infoHash = selectedInfoHash();
    if (infoHash.isEmpty())
        return {};

    const QTableWidgetItem *nameItem = m_table->item(m_table->currentRow(), COL_NAME);
    const QString name = nameItem ? nameItem->text() : QString();
    QString magnet = u"magnet:?xt=urn:btih:"_s + infoHash;
    if (!name.isEmpty())
        magnet += u"&dn="_s + QString::fromLatin1(QUrl::toPercentEncoding(name));
    return magnet;
}

void DHTIndexWidget::showContextMenu(const QPoint &pos)
{
    if (selectedInfoHash().isEmpty())
        return;

    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    menu->addAction(tr("Copy magnet link"), this, [this]
    {
        QApplication::clipboard()->setText(selectedMagnet());
    });
    menu->addAction(tr("Download..."), this, &DHTIndexWidget::downloadSelected);

    menu->popup(m_table->viewport()->mapToGlobal(pos));
}
