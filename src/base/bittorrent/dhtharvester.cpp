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
    const int MAX_SAMPLE_NODES_PER_TICK = 6;
    // Bounded keyspace traversal: how many sample requests may be outstanding per
    // tick (caps recursion) and how many returned nodes to recurse into per reply.
    const int SAMPLE_BUDGET_PER_TICK = 18;
    const int RECURSE_NODES_PER_SAMPLE = 2;

    // Metadata-fetch timeout housekeeping. Most DHT-sniffed infohashes have no
    // reachable peer (get_peers = someone *asking*, not *having*), so they hold a
    // fetch slot until timeout; a peer that does have it exchanges the small
    // info-dict within seconds, so a shorter timeout mainly frees dead slots.
    const int TIMEOUT_SWEEP_MS = 5000;
    const qint64 METADATA_TIMEOUT_MS = 20 * 1000;

    // Metadata scheduler: periodically top up the fetch queue from the PERSISTENT
    // store (infohashes still lacking metadata, most promising first by
    // availability/recency, off backoff). This drives coverage across sessions --
    // an infohash seen once still gets fetched eventually -- without an unbounded
    // in-memory popularity table. The in-memory pending queue is capped so a
    // discovery firehose never grows memory; the store re-supplies as slots free.
    const int SCHEDULE_INTERVAL_MS = 3000;
    const int MAX_PENDING = 2000;
    const int MAX_CONCURRENT_METADATA = 40;
    const int SIGHTING_FLUSH_INTERVAL_MS = 1000;
    const int MAX_SIGHTING_BUFFER = 500;

    // Retention: keep the index useful instead of letting metadata-less sightings
    // accumulate forever. A metadata-less infohash that exhausted its fetch
    // attempts and has not been seen for STALE_AGE is dropped; sightings older
    // than SIGHTING_RETENTION are pruned (fastest-growing table); the torrents
    // table is hard-capped at MAX_INDEX_TORRENTS (oldest metadata-less first).
    const int PRUNE_INTERVAL_MS = 30 * 60 * 1000;       // 30 min
    const qint64 STALE_AGE_MS = qint64(7) * 24 * 60 * 60 * 1000;        // 7 days
    const qint64 SIGHTING_RETENTION_MS = qint64(3) * 24 * 60 * 60 * 1000; // 3 days
    const qint64 MAX_INDEX_TORRENTS = 1000000;

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
    , m_scheduleTimer {new QTimer(this)}
    , m_pruneTimer {new QTimer(this)}
    , m_sightingFlushTimer {new QTimer(this)}
{
    m_sampleTimer->setInterval(SAMPLE_INTERVAL_MS);
    m_timeoutTimer->setInterval(TIMEOUT_SWEEP_MS);
    m_scheduleTimer->setInterval(SCHEDULE_INTERVAL_MS);
    m_pruneTimer->setInterval(PRUNE_INTERVAL_MS);
    m_sightingFlushTimer->setInterval(SIGHTING_FLUSH_INTERVAL_MS);
    connect(m_sampleTimer, &QTimer::timeout, this, &DHTHarvester::onSampleTimer);
    connect(m_timeoutTimer, &QTimer::timeout, this, &DHTHarvester::onTimeoutTimer);
    connect(m_scheduleTimer, &QTimer::timeout, this, &DHTHarvester::onScheduleTimer);
    connect(m_pruneTimer, &QTimer::timeout, this, &DHTHarvester::onPruneTimer);
    connect(m_sightingFlushTimer, &QTimer::timeout, this, &DHTHarvester::flushSightings);
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
    m_maxConcurrent = std::clamp(max, 1, MAX_CONCURRENT_METADATA);
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
    m_scheduleTimer->start();
    m_pruneTimer->start();
    m_sightingFlushTimer->start();
    m_running = true;

    LogMsg(tr("DHT harvester started (egress bound to interface \"%1\").").arg(m_session->networkInterfaceName()), Log::INFO);
}

void DHTHarvester::stop()
{
    m_sampleTimer->stop();
    m_timeoutTimer->stop();
    m_scheduleTimer->stop();
    m_pruneTimer->stop();
    m_sightingFlushTimer->stop();

    flushSightings();

    for (auto it = m_inFlight.cbegin(); it != m_inFlight.cend(); ++it)
        m_session->cancelDownloadMetadata(TorrentID::fromString(it.key()));
    m_inFlight.clear();
    m_pending.clear();
    m_queued.clear();
    m_done.clear();
    m_sightingBuffer.clear();

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
            // Record the sighting; the store-driven scheduler picks it up by
            // availability/recency on the next tick (no in-memory popularity gate).
            postSighting(ih, u"get_peers"_s, {}, 0);
        }
        break;
    case lt::dht_announce_alert::alert_type:
        {
            const auto *a = static_cast<const lt::dht_announce_alert *>(alert);
            const QString ih = hexOf(a->info_hash);
            const QString peerIP = QString::fromStdString(a->ip.to_string());
            postSighting(ih, u"announce"_s, peerIP, a->port);
            enqueue(ih);  // a peer HAS it -> fetch immediately
            // The announcing peer holds the torrent: feed it straight to the
            // in-flight metadata download instead of re-finding peers via DHT.
            if (m_inFlight.contains(ih))
                m_session->connectDHTMetadataPeer(ih, peerIP, a->port);
        }
        break;
    case lt::dht_sample_infohashes_alert::alert_type:
        {
            const auto *a = static_cast<const lt::dht_sample_infohashes_alert *>(alert);
            for (const lt::sha1_hash &sample : a->samples())
            {
                const QString ih = hexOf(sample);
                postSighting(ih, u"sample"_s, {}, 0);
            }
            // Recurse into the returned (closer) nodes to traverse this region of
            // the keyspace more deeply, bounded by the per-tick sample budget.
            if (m_activeCrawl)
            {
                int recursed = 0;
                for (const auto &node : a->nodes())
                {
                    if ((recursed++ >= RECURSE_NODES_PER_SAMPLE) || (m_sampleBudget <= 0))
                        break;
                    m_nativeSession->dht_sample_infohashes(node.second, randomTarget());
                    --m_sampleBudget;
                }
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
                if ((sampled++ >= MAX_SAMPLE_NODES_PER_TICK) || (m_sampleBudget <= 0))
                    break;
                m_nativeSession->dht_sample_infohashes(node.second, randomTarget());
                --m_sampleBudget;
            }
        }
        break;
    case lt::dht_get_peers_reply_alert::alert_type:
        {
            // Real swarm size for a known infohash (BEP-33-ish): store the peer
            // count so the index/Torznab can report seeders/peers instead of a
            // sighting proxy.
            const auto *a = static_cast<const lt::dht_get_peers_reply_alert *>(alert);
            const QString ih = hexOf(a->info_hash);
            const int peers = a->num_peers();
            if (m_store && (peers > 0))
            {
                QMetaObject::invokeMethod(m_store, [store = m_store, ih, peers] { store->updateSwarm(ih, peers); }, Qt::QueuedConnection);
            }
        }
        break;
    case lt::peer_connect_alert::alert_type:
        {
            // Seed BEP-51 sampling from the DHT nodes of peers we connect to in
            // swarms (our own ephemeral metadata fetches and real torrents alike).
            // A swarm peer is a live, reachable host whose DHT node stores the
            // infohashes announced near it -> a high-quality sample target.
            //
            // Only outgoing connections carry the peer's real listen port (we
            // dialed it); incoming connections expose an ephemeral source port.
            // libtorrent doesn't surface the peer's advertised DHT (BEP-5 PORT)
            // value, and most clients run DHT on their BT listen port, so we use
            // that as a best-effort sample endpoint. Misses just time out silently.
            if (!m_activeCrawl || (m_sampleBudget <= 0))
                break;
            const auto *a = static_cast<const lt::peer_connect_alert *>(alert);
            if (a->direction != lt::peer_connect_alert::direction_t::out)
                break;
            const lt::tcp::endpoint &tep = a->endpoint;
            if (tep.address().is_unspecified() || (tep.port() == 0))
                break;
            m_nativeSession->dht_sample_infohashes(lt::udp::endpoint(tep.address(), tep.port()), randomTarget());
            --m_sampleBudget;
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
    {
        m_sampleBudget = SAMPLE_BUDGET_PER_TICK;
        m_nativeSession->dht_live_nodes(randomTarget());
    }

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

void DHTHarvester::enqueue(const QString &infoHashV1)
{
    if (infoHashV1.isEmpty() || m_done.contains(infoHashV1) || m_queued.contains(infoHashV1))
        return;

    // Cap the in-memory queue: at the DHT discovery rate the candidate set is
    // effectively unbounded, but the persistent store holds it all and the
    // scheduler re-supplies as fetch slots free, so dropping here is safe.
    if (m_pending.size() >= MAX_PENDING)
        return;

    m_queued.insert(infoHashV1);
    m_pending.enqueue(infoHashV1);
    pump();
}

void DHTHarvester::onScheduleTimer()
{
    if (!m_running || !m_store)
        return;

    const int room = m_maxConcurrent - m_inFlight.size();
    if ((room <= 0) || (m_pending.size() >= MAX_PENDING))
        return;

    // Pull a little ahead of the free slots so the pump never starves; the store
    // returns only infohashes that still need metadata and are off backoff,
    // most promising first.
    const int batch = qMin(room * 4, MAX_PENDING - m_pending.size());
    QList<QString> candidates;
    QMetaObject::invokeMethod(m_store, [store = m_store, batch] { return store->candidatesForMetadata(batch); }
        , Qt::BlockingQueuedConnection, &candidates);

    for (const QString &ih : candidates)
        enqueue(ih);
}

void DHTHarvester::onPruneTimer()
{
    if (!m_store)
        return;

    // Run the retention sweep on the store thread and log what it removed.
    QMetaObject::invokeMethod(m_store, [store = m_store]
    {
        const HarvestPruneStats s = store->prune(STALE_AGE_MS, SIGHTING_RETENTION_MS, MAX_INDEX_TORRENTS);
        if ((s.torrentsPruned > 0) || (s.sightingsPruned > 0))
        {
            LogMsg(QObject::tr("DHT harvest retention: pruned %1 stale torrents and %2 old sightings.")
                    .arg(s.torrentsPruned).arg(s.sightingsPruned), Log::INFO);
        }
    }, Qt::QueuedConnection);
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
    m_sightingBuffer.append(sighting);
    if (m_sightingBuffer.size() >= MAX_SIGHTING_BUFFER)
        flushSightings();
}

void DHTHarvester::flushSightings()
{
    if (!m_store || m_sightingBuffer.isEmpty())
        return;

    const QList<HarvestSighting> sightings = m_sightingBuffer;
    m_sightingBuffer.clear();
    QMetaObject::invokeMethod(m_store, [store = m_store, sightings] { store->recordSightings(sightings); }, Qt::QueuedConnection);
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

    // Ask the DHT how many peers have this (real swarm size) -> dht_get_peers_reply_alert.
    const lt::sha1_hash ihHash = infoHash.v1();
    m_nativeSession->dht_get_peers(ihHash);

    emit torrentIndexed(v1, torrent.name);

    pump();
}
