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
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QModelIndex>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableView>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "base/bittorrent/addtorrentparams.h"
#include "base/bittorrent/harveststore.h"
#include "base/bittorrent/session.h"
#include "base/bittorrent/torrentdescriptor.h"
#include "base/bittorrent/torrentinfo.h"
#include "gui/addnewtorrentdialog.h"
#include "gui/torrentcontentwidget.h"
#include "dhtindexmodel.h"
#include "harvestcontenthandler.h"

using namespace Qt::Literals::StringLiterals;

namespace
{
    // Crawl-intensity presets exposed by the GUI dial. Normal mirrors the shipped
    // defaults; High/Max push the sample fan-out and metadata concurrency harder.
    enum IntensityPreset
    {
        IntensityNormal = 0,
        IntensityHigh = 1,
        IntensityMax = 2
    };

    BitTorrent::HarvesterTuning tuningForPreset(const int preset, int &maxConcurrentOut)
    {
        BitTorrent::HarvesterTuning tuning;
        tuning.metadataTimeoutAnnounceMs = 8000;
        tuning.metadataTimeoutSpeculativeMs = 3000;
        switch (preset)
        {
        case IntensityHigh:
            tuning.sampleIntervalMs = 4000;
            tuning.maxSampleNodesPerTick = 40;
            tuning.sampleBudgetPerTick = 96;
            tuning.recurseNodesPerSample = 6;
            maxConcurrentOut = 96;
            break;
        case IntensityMax:
            tuning.sampleIntervalMs = 2000;
            tuning.maxSampleNodesPerTick = 60;
            tuning.sampleBudgetPerTick = 144;
            tuning.recurseNodesPerSample = 8;
            maxConcurrentOut = 144;
            break;
        case IntensityNormal:
        default:
            tuning.sampleIntervalMs = 4000;
            tuning.maxSampleNodesPerTick = 20;
            tuning.sampleBudgetPerTick = 48;
            tuning.recurseNodesPerSample = 4;
            maxConcurrentOut = 48;
            break;
        }
        return tuning;
    }

    int presetForCurrentSettings()
    {
        const BitTorrent::HarvesterTuning tuning = BitTorrent::Session::instance()->dhtHarvesterTuning();
        if (tuning.sampleBudgetPerTick >= 144)
            return IntensityMax;
        if (tuning.sampleBudgetPerTick >= 96)
            return IntensityHigh;
        return IntensityNormal;
    }

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

    topRow->addSpacing(16);
    topRow->addWidget(new QLabel(tr("Crawl intensity:"), this));
    m_intensityBox = new QComboBox(this);
    m_intensityBox->addItems({tr("Normal"), tr("High"), tr("Max")});
    m_intensityBox->setToolTip(tr("How hard the harvester crawls the DHT. Higher settings index"
                                  " faster but use more CPU and VPN bandwidth."));
    m_intensityBox->setCurrentIndex(presetForCurrentSettings());
    connect(m_intensityBox, &QComboBox::currentIndexChanged, this, &DHTIndexWidget::onIntensityChanged);
    topRow->addWidget(m_intensityBox);

    topRow->addStretch(1);
    m_statsLabel = new QLabel(this);
    topRow->addWidget(m_statsLabel);
    layout->addLayout(topRow);

    // Second row: live crawl diagnostics (frontier/sample/fetch counters).
    m_crawlStatsLabel = new QLabel(this);
    m_crawlStatsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_crawlStatsLabel);

    auto *searchRow = new QHBoxLayout;
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(tr("Search harvested torrents by name..."));
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &DHTIndexWidget::search);
    searchRow->addWidget(m_searchEdit);
    auto *searchButton = new QPushButton(tr("Search"), this);
    connect(searchButton, &QPushButton::clicked, this, &DHTIndexWidget::search);
    searchRow->addWidget(searchButton);
    layout->addLayout(searchRow);

    // Left: content-type filter list (qB status-filter style). Right: the results
    // table over the content pane.
    m_filterList = new QListWidget(this);
    m_filterList->setMinimumWidth(120);
    m_filterList->setMaximumWidth(220);
    connect(m_filterList, &QListWidget::currentRowChanged, this, &DHTIndexWidget::onFilterChanged);

    m_model = new DHTIndexModel(this);
    m_table = new QTableView(this);
    m_table->setModel(m_model);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->setSortingEnabled(false);  // windowed view; rows are store-ordered by relevance
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->setVisible(false);

    QHeaderView *header = m_table->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::Interactive);  // user-resizable columns
    header->setSectionsMovable(true);
    header->setStretchLastSection(false);
    m_table->setColumnWidth(DHTIndexModel::COL_NAME, 360);
    m_table->setColumnWidth(DHTIndexModel::COL_CONTENT, 80);
    m_table->setColumnWidth(DHTIndexModel::COL_SIZE, 90);
    m_table->setColumnWidth(DHTIndexModel::COL_FILES, 60);
    m_table->setColumnWidth(DHTIndexModel::COL_SEEDS, 60);
    m_table->setColumnWidth(DHTIndexModel::COL_PEERS, 60);
    m_table->setColumnWidth(DHTIndexModel::COL_SIGHTINGS, 70);
    m_table->setColumnWidth(DHTIndexModel::COL_FIRSTSEEN, 120);
    m_table->setColumnWidth(DHTIndexModel::COL_LASTSEEN, 120);

    connect(m_table, &QWidget::customContextMenuRequested, this, &DHTIndexWidget::showContextMenu);
    connect(m_table, &QTableView::doubleClicked, this, &DHTIndexWidget::onItemDoubleClicked);
    connect(m_table->selectionModel(), &QItemSelectionModel::selectionChanged
            , this, &DHTIndexWidget::onSelectionChanged);

    // Lower pane: the standard qBittorrent content tree for the selected torrent.
    m_content = new TorrentContentWidget(this);

    auto *rightSplitter = new QSplitter(Qt::Vertical, this);
    rightSplitter->setChildrenCollapsible(false);
    rightSplitter->addWidget(m_table);
    rightSplitter->addWidget(m_content);
    rightSplitter->setStretchFactor(0, 2);
    rightSplitter->setStretchFactor(1, 1);

    auto *mainSplitter = new QSplitter(Qt::Horizontal, this);
    mainSplitter->setChildrenCollapsible(false);
    mainSplitter->addWidget(m_filterList);
    mainSplitter->addWidget(rightSplitter);
    mainSplitter->setStretchFactor(0, 0);
    mainSplitter->setStretchFactor(1, 1);
    layout->addWidget(mainSplitter, 1);

    auto *bottomRow = new QHBoxLayout;
    bottomRow->addStretch(1);
    m_downloadButton = new QPushButton(tr("Download"), this);
    m_downloadButton->setEnabled(false);
    connect(m_downloadButton, &QPushButton::clicked, this, &DHTIndexWidget::downloadSelected);
    bottomRow->addWidget(m_downloadButton);
    layout->addLayout(bottomRow);

    // Build the filter list (selects "All") then load the table for it, and keep the
    // counts + crawl diagnostics fresh on a timer.
    rebuildFilter();
    m_model->setFilter(m_query, currentFilterType());

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

void DHTIndexWidget::onIntensityChanged(const int presetIndex)
{
    int maxConcurrent = 48;
    const BitTorrent::HarvesterTuning tuning = tuningForPreset(presetIndex, maxConcurrent);
    auto *session = BitTorrent::Session::instance();
    session->setDHTHarvesterTuning(tuning);
    session->setDHTHarvesterMaxConcurrentMetadata(maxConcurrent);
}

void DHTIndexWidget::refreshStats()
{
    const BitTorrent::HarvestStats stats = BitTorrent::Session::instance()->dhtHarvestStats();
    m_statsLabel->setText(tr("Infohashes: %1  |  With metadata: %2  |  Sightings: %3")
            .arg(QString::number(stats.torrentCount), QString::number(stats.metadataCount)
                , QString::number(stats.sightingCount)));
    // Live crawl diagnostics: lets you tell "slow" from "not crawling at all" at a
    // glance (no frontier nodes / no samples sent -> DHT not feeding the crawler).
    m_crawlStatsLabel->setText(tr("Nodes: %1  |  Samples: %2  |  Replies: %3  |  Announces: %4"
                                  "  |  Fetch: %5 ok / %6 timeout  |  Queue: %7 in-flight, %8 pending")
            .arg(QString::number(stats.trackedNodes), QString::number(stats.samplesSent)
                , QString::number(stats.sampleReplies), QString::number(stats.announcesSeen)
                , QString::number(stats.metadataOk), QString::number(stats.metadataTimeouts)
                , QString::number(stats.inFlightFetch), QString::number(stats.pendingFetch)));

    // Keep the per-type counts in the filter list fresh (new types appear, totals grow)
    // without disturbing the loaded table or the user's selection.
    rebuildFilter();
}

void DHTIndexWidget::search()
{
    m_query = m_searchEdit->text().trimmed();
    rebuildFilter();  // counts now reflect the query
    m_model->setFilter(m_query, currentFilterType());
}

void DHTIndexWidget::rebuildFilter()
{
    const QString selectedType = currentFilterType();
    const QList<BitTorrent::HarvestTypeCount> counts =
            BitTorrent::Session::instance()->dhtIndexTypeCounts(m_query);

    qint64 total = 0;
    for (const BitTorrent::HarvestTypeCount &c : counts)
        total += c.count;

    // Rebuild without firing currentRowChanged (the timer-driven refresh must not
    // reload the table); restore the previously-selected type by value.
    const QSignalBlocker blocker {m_filterList};
    m_filterList->clear();

    auto *allItem = new QListWidgetItem(tr("All (%1)").arg(total));
    allItem->setData(Qt::UserRole, QString());
    m_filterList->addItem(allItem);

    int rowToSelect = 0;
    int row = 1;
    for (const BitTorrent::HarvestTypeCount &c : counts)
    {
        auto *item = new QListWidgetItem(u"%1 (%2)"_s.arg(
                DHTIndexModel::displayContentType(c.contentType), QString::number(c.count)));
        item->setData(Qt::UserRole, c.contentType);
        m_filterList->addItem(item);
        if (c.contentType == selectedType)
            rowToSelect = row;
        ++row;
    }
    m_filterList->setCurrentRow(rowToSelect);
}

QString DHTIndexWidget::currentFilterType() const
{
    const QListWidgetItem *item = m_filterList->currentItem();
    return item ? item->data(Qt::UserRole).toString() : QString();
}

void DHTIndexWidget::onFilterChanged()
{
    m_model->setFilter(m_query, currentFilterType());
}

void DHTIndexWidget::onItemDoubleClicked(const QModelIndex &index)
{
    if (index.isValid())
        downloadSelected();
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
    return m_model->infoHashForIndex(m_table->currentIndex());
}

QString DHTIndexWidget::selectedMagnet() const
{
    const QModelIndex current = m_table->currentIndex();
    const QString infoHash = m_model->infoHashForIndex(current);
    if (infoHash.isEmpty())
        return {};

    const QModelIndex nameIdx = current.sibling(current.row(), DHTIndexModel::COL_NAME);
    const QString name = m_model->data(nameIdx, Qt::DisplayRole).toString();
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
