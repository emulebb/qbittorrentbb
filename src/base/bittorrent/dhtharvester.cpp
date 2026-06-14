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

#include "dhtharvester.h"

#include <algorithm>
#include <array>

#include <libtorrent/alert_types.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/sha1_hash.hpp>

#include <QDateTime>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QStringList>
#include <QThread>
#include <QTimer>

#include "base/digest32.h"
#include "base/global.h"
#include "base/logger.h"
#include "harveststore.h"
#include "infohash.h"
#include "session.h"
#include "torrentdescriptor.h"
#include "torrentinfo.h"

using namespace Qt::Literals::StringLiterals;

namespace
{
    // BEP-51 active-crawl cadence and per-tick fan-out.
    const int SAMPLE_INTERVAL_MS = 8000;
    const int MAX_SAMPLE_NODES_PER_TICK = 8;

    // Metadata-fetch timeout housekeeping.
    const int TIMEOUT_SWEEP_MS = 5000;
    const qint64 METADATA_TIMEOUT_MS = 30 * 1000;

    // Popularity gate: an infohash seen via get_peers/sample is only queued for
    // metadata once it has been observed this many times. Repeat sightings mean
    // peers are actively looking for it (= alive = fetchable), so fetch slots are
    // spent on torrents that resolve instead of dead one-off hashes. An announce
    // (a peer declaring it HAS the content) bypasses this and queues immediately.
    const int METADATA_POPULARITY_THRESHOLD = 2;

    qint64 nowMs()
    {
        return QDateTime::currentMSecsSinceEpoch();
    }

    lt::sha1_hash randomTarget()
    {
        std::array<quint32, 5> words {};
        QRandomGenerator::global()->fillRange(words.data(), static_cast<qsizetype>(words.size()));
        lt::sha1_hash target;
        target.assign(reinterpret_cast<const char *>(words.data()));
        return target;
    }

    QString hexOf(const lt::sha1_hash &hash)
    {
        return SHA1Hash(hash).toString();
    }
}

using namespace BitTorrent;

DHTHarvester::DHTHarvester(Session *session, lt::session *nativeSession, const Path &dbPath, QObject *parent)
    : QObject(parent)
    , m_session {session}
    , m_nativeSession {nativeSession}
    , m_path {dbPath}
    , m_sampleTimer {new QTimer(this)}
    , m_timeoutTimer {new QTimer(this)}
{
    m_sampleTimer->setInterval(SAMPLE_INTERVAL_MS);
    m_timeoutTimer->setInterval(TIMEOUT_SWEEP_MS);
    connect(m_sampleTimer, &QTimer::timeout, this, &DHTHarvester::onSampleTimer);
    connect(m_timeoutTimer, &QTimer::timeout, this, &DHTHarvester::onTimeoutTimer);
    connect(m_session, &Session::metadataDownloaded, this, &DHTHarvester::onMetadataDownloaded);
}

DHTHarvester::~DHTHarvester()
{
    stop();
}

bool DHTHarvester::isEnabled() const
{
    return m_enabled;
}

void DHTHarvester::setEnabled(const bool enabled)
{
    if (m_enabled == enabled)
        return;

    m_enabled = enabled;
    if (m_enabled)
        start();
    else
        stop();
}

void DHTHarvester::setActiveCrawlEnabled(const bool enabled)
{
    m_activeCrawl = enabled;
}

void DHTHarvester::setMaxConcurrentMetadata(const int max)
{
    m_maxConcurrent = std::max(1, max);
}

HarvestStore *DHTHarvester::store() const
{
    return m_store;
}

void DHTHarvester::start()
{
    if (m_running)
        return;

    if (!vpnReady())
    {
        if (!m_warnedNoVPN)
        {
            LogMsg(tr("DHT harvester not started: it requires binding to the VPN network interface"
                      " (Tools > Options > Advanced > Network Interface). Refusing to crawl on the default route.")
                , Log::WARNING);
            m_warnedNoVPN = true;
        }
        // Stay enabled but idle; re-check on the next sample tick.
        m_sampleTimer->start();
        return;
    }

    m_warnedNoVPN = false;

    m_storeThread = new QThread;
    m_storeThread->setObjectName(u"DHTHarvestStore"_s);
    m_store = new HarvestStore(m_path);
    m_store->moveToThread(m_storeThread);
    connect(m_storeThread, &QThread::started, m_store, &HarvestStore::init);
    m_storeThread->start();

    m_sampleTimer->start();
    m_timeoutTimer->start();
    m_running = true;

    LogMsg(tr("DHT harvester started (egress bound to interface \"%1\").").arg(m_session->networkInterfaceName()), Log::INFO);
}

void DHTHarvester::stop()
{
    m_sampleTimer->stop();
    m_timeoutTimer->stop();

    for (auto it = m_inFlight.cbegin(); it != m_inFlight.cend(); ++it)
        m_session->cancelDownloadMetadata(TorrentID::fromString(it.key()));
    m_inFlight.clear();
    m_pending.clear();
    m_queued.clear();
    m_sightCount.clear();
    m_done.clear();

    if (m_storeThread)
    {
        if (m_store)
        {
            // Delete the store on its own thread so its SQLite connection is
            // removed from the thread that created it.
            QMetaObject::invokeMethod(m_store, [store = m_store] { delete store; }, Qt::BlockingQueuedConnection);
            m_store = nullptr;
        }
        m_storeThread->quit();
        m_storeThread->wait();
        delete m_storeThread;
        m_storeThread = nullptr;
    }

    m_running = false;
}

bool DHTHarvester::vpnReady() const
{
    const QString cfgName = m_session->networkInterface();
    if (cfgName.isEmpty())
    {
        // No in-app interface bind: egress routing is delegated to an external
        // VPN app (e.g. hide.me per-app/split-tunnel routing). Allow, since the
        // OS forces this process through the tunnel regardless of the socket bind.
        return true;
    }

    const QString humanName = m_session->networkInterfaceName();
    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : interfaces)
    {
        const bool matches = (iface.name() == cfgName)
                || (iface.humanReadableName() == cfgName)
                || (!humanName.isEmpty() && (iface.humanReadableName() == humanName));
        if (matches)
        {
            const QNetworkInterface::InterfaceFlags flags = iface.flags();
            return flags.testFlag(QNetworkInterface::IsUp) && flags.testFlag(QNetworkInterface::IsRunning);
        }
    }
    return false;  // bound interface not present/up -> fail closed
}

void DHTHarvester::handleAlert(const lt::alert *alert)
{
    if (!m_enabled || !m_running)
        return;

    switch (alert->type())
    {
    case lt::dht_get_peers_alert::alert_type:
        {
            const auto *a = static_cast<const lt::dht_get_peers_alert *>(alert);
            const QString ih = hexOf(a->info_hash);
            postSighting(ih, u"get_peers"_s, {}, 0);
            considerForMetadata(ih);
        }
        break;
    case lt::dht_announce_alert::alert_type:
        {
            const auto *a = static_cast<const lt::dht_announce_alert *>(alert);
            const QString ih = hexOf(a->info_hash);
            postSighting(ih, u"announce"_s, QString::fromStdString(a->ip.to_string()), a->port);
            ++m_sightCount[ih];
            enqueue(ih);  // a peer HAS it -> fetch immediately
        }
        break;
    case lt::dht_sample_infohashes_alert::alert_type:
        {
            const auto *a = static_cast<const lt::dht_sample_infohashes_alert *>(alert);
            for (const lt::sha1_hash &sample : a->samples())
            {
                const QString ih = hexOf(sample);
                postSighting(ih, u"sample"_s, {}, 0);
                considerForMetadata(ih);
            }
        }
        break;
    case lt::dht_live_nodes_alert::alert_type:
        {
            if (!m_activeCrawl)
                break;
            const auto *a = static_cast<const lt::dht_live_nodes_alert *>(alert);
            const auto nodes = a->nodes();
            int sampled = 0;
            for (const auto &node : nodes)
            {
                if (sampled++ >= MAX_SAMPLE_NODES_PER_TICK)
                    break;
                m_nativeSession->dht_sample_infohashes(node.second, randomTarget());
            }
        }
        break;
    default:
        break;
    }
}

void DHTHarvester::onSampleTimer()
{
    // Periodic fail-closed re-check: if the VPN went away, go idle; if it came
    // back and we are enabled but not yet running, (re)start the pipeline.
    if (!vpnReady())
    {
        if (m_running)
        {
            LogMsg(tr("DHT harvester paused: VPN network interface is down. Crawling stopped (fail-closed)."), Log::WARNING);
            stop();
            m_sampleTimer->start();  // keep re-checking
        }
        return;
    }

    if (!m_running)
    {
        start();
        return;
    }

    if (m_activeCrawl)
        m_nativeSession->dht_live_nodes(randomTarget());

    pump();
}

void DHTHarvester::onTimeoutTimer()
{
    const qint64 now = nowMs();
    QStringList timedOut;
    for (auto it = m_inFlight.cbegin(); it != m_inFlight.cend(); ++it)
    {
        if ((now - it.value()) > METADATA_TIMEOUT_MS)
            timedOut.append(it.key());
    }

    for (const QString &ih : timedOut)
    {
        m_session->cancelDownloadMetadata(TorrentID::fromString(ih));
        m_inFlight.remove(ih);
        m_queued.remove(ih);
        if (m_store)
        {
            QMetaObject::invokeMethod(m_store, [store = m_store, ih] { store->noteFetchFailure(ih); }, Qt::QueuedConnection);
        }
    }

    pump();
}

void DHTHarvester::considerForMetadata(const QString &infoHashV1)
{
    if (infoHashV1.isEmpty() || m_done.contains(infoHashV1) || m_queued.contains(infoHashV1))
        return;

    // Only queue once the infohash has been observed enough times to look alive.
    if (++m_sightCount[infoHashV1] >= METADATA_POPULARITY_THRESHOLD)
        enqueue(infoHashV1);
}

void DHTHarvester::enqueue(const QString &infoHashV1)
{
    if (infoHashV1.isEmpty() || m_done.contains(infoHashV1) || m_queued.contains(infoHashV1))
        return;

    m_queued.insert(infoHashV1);
    m_pending.enqueue(infoHashV1);
    pump();
}

void DHTHarvester::pump()
{
    while ((m_inFlight.size() < m_maxConcurrent) && !m_pending.isEmpty())
    {
        const QString ih = m_pending.dequeue();
        if (m_done.contains(ih) || m_inFlight.contains(ih))
        {
            m_queued.remove(ih);
            continue;
        }

        const QString magnet = u"magnet:?xt=urn:btih:"_s + ih;
        const auto descr = TorrentDescriptor::parse(magnet);
        if (!descr)
        {
            m_queued.remove(ih);
            continue;
        }

        if (m_session->downloadMetadata(descr.value()))
        {
            m_inFlight.insert(ih, nowMs());
        }
        else
        {
            // Already a known torrent / already fetching: drop our claim.
            m_queued.remove(ih);
        }
    }
}

void DHTHarvester::postSighting(const QString &infoHashV1, const QString &source, const QString &ip, const int port)
{
    if (!m_store || infoHashV1.isEmpty())
        return;

    HarvestSighting sighting;
    sighting.infoHashV1 = infoHashV1;
    sighting.source = source;
    sighting.peerIP = ip;
    sighting.peerPort = port;
    QMetaObject::invokeMethod(m_store, [store = m_store, sighting] { store->recordSighting(sighting); }, Qt::QueuedConnection);
}

void DHTHarvester::onMetadataDownloaded(const TorrentInfo &info)
{
    const InfoHash infoHash = info.infoHash();
    const QString v1 = infoHash.v1().toString();
    if (!m_inFlight.contains(v1))
        return;  // not one of ours (or already handled)

    m_inFlight.remove(v1);
    m_queued.remove(v1);
    m_done.insert(v1);

    HarvestedTorrent torrent;
    torrent.infoHashV1 = v1;
    torrent.infoHashV2 = infoHash.isHybrid() ? infoHash.v2().toString() : QString();
    torrent.name = info.name();
    torrent.size = info.totalSize();
    torrent.fileCount = info.filesCount();
    torrent.rawMetadata = info.rawData();  // bencoded info-dict, for reconstruction
    torrent.files.reserve(info.filesCount());
    for (int i = 0; i < info.filesCount(); ++i)
    {
        HarvestedFile file;
        file.path = info.filePath(i).toString();
        file.size = info.fileSize(i);
        torrent.files.append(file);
    }

    if (m_store)
    {
        QMetaObject::invokeMethod(m_store, [store = m_store, torrent] { store->recordMetadata(torrent); }, Qt::QueuedConnection);
    }

    emit torrentIndexed(v1, torrent.name);

    pump();
}
