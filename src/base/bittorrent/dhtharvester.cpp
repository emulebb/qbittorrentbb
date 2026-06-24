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
#include <optional>

#include <libtorrent/address.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/sha1_hash.hpp>
#include <libtorrent/socket.hpp>

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
    // BEP-51 active-crawl cadence and per-tick fan-out are runtime-tunable via
    // HarvesterTuning (INI keys, see SessionImpl): the sampling cadence, how many
    // live nodes to sample per tick, the per-tick budget that bounds keyspace
    // recursion, how many returned nodes to recurse into per reply, and the
    // metadata-fetch timeout. The harvester runs off the GUI thread, so the
    // defaults are a moderate background-indexer profile rather than the old
    // "deliberately gentle" trickle.

    // Metadata-fetch timeout housekeeping. Most DHT-sniffed infohashes have no
    // reachable peer (get_peers = someone *asking*, not *having*), so they hold a
    // fetch slot until timeout; a peer that does have it exchanges the small
    // info-dict within seconds. The per-fetch deadline is tiered by source (see
    // HarvesterTuning); the sweep runs at a fine 1s granularity so short
    // speculative timeouts free dead slots promptly. Scanning m_inFlight
    // (<= m_maxConcurrent entries) every second is negligible.
    const int TIMEOUT_SWEEP_MS = 1000;

    // Metadata scheduler: periodically top up the fetch queue from the PERSISTENT
    // store (infohashes still lacking metadata, most promising first by
    // availability/recency, off backoff). This drives coverage across sessions --
    // an infohash seen once still gets fetched eventually -- without an unbounded
    // in-memory popularity table. The in-memory pending queue is capped so a
    // discovery firehose never grows memory; the store re-supplies as slots free.
    const int SCHEDULE_INTERVAL_MS = 3000;
    const int MAX_PENDING = 2000;
    const int MAX_CONCURRENT_METADATA = 64;

    // BEP-51 sampling frontier cap. The keyspace walk discovers nodes far faster
    // than they can be sampled, so the frontier is bounded; when full, exhausted
    // nodes (sampled at least once, reporting zero infohashes) are recycled to make
    // room before new discoveries are dropped. 20k endpoints is a few MiB of state.
    const int MAX_TRACKED_NODES = 20000;
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

    // Rebuild a UDP endpoint from the (ip-string, port) carried in a HarvestAlertEvent.
    std::optional<lt::udp::endpoint> toEndpoint(const QString &ip, const quint16 port)
    {
        lt::error_code ec;
        const lt::address addr = lt::make_address(ip.toLatin1().constData(), ec);
        if (ec)
            return std::nullopt;
        return lt::udp::endpoint(addr, port);
    }
}

using namespace BitTorrent;

DHTHarvester::DHTHarvester(Session *session, lt::session *nativeSession, const Path &dbPath, QObject *parent)
    : QObject(parent)
    , m_session {session}
    , m_nativeSession {nativeSession}
    , m_path {dbPath}
{
    // Timers are NOT created here: they must have affinity to the worker thread,
    // so onThreadStarted() (which runs on that thread) creates them. The metadata
    // signal is connected now; once we moveToThread() it auto-degrades to a queued
    // cross-thread delivery (TorrentInfo is a registered metatype).
    connect(m_session, &Session::metadataDownloaded, this, &DHTHarvester::onMetadataDownloaded);
}

DHTHarvester::~DHTHarvester()
{
    // By now shutdown() has stopped the worker thread; stop() here is an idempotent
    // safety net. Tear down the persistent store (kept alive across enable/disable)
    // and the worker thread object.
    if (m_thread && m_thread->isRunning())
    {
        m_thread->quit();
        m_thread->wait();
    }
    stop();

    if (m_storeThread)
    {
        // No GUI store() reader can run concurrently here (~DHTHarvester runs on the
        // GUI thread, as do the store() callers), so unpublishing is just tidiness.
        m_storeHandle.store(nullptr, std::memory_order_release);
        if (m_store)
        {
            // Delete the store on its own thread so its SQLite connection is removed
            // from the thread that created it.
            QMetaObject::invokeMethod(m_store, [store = m_store] { delete store; }, Qt::BlockingQueuedConnection);
            m_store = nullptr;
        }
        m_storeThread->quit();
        m_storeThread->wait();
        delete m_storeThread;
        m_storeThread = nullptr;
    }

    delete m_thread;
    m_thread = nullptr;
}

void DHTHarvester::shutdown()
{
    if (!m_thread)
    {
        stop();
        return;
    }
    // Drain the crawl loop on the worker thread, then quit it. stop() posts in-flight
    // metadata cancellations to the Session (queued); the caller is responsible for
    // letting the Session process them before it is destroyed.
    QMetaObject::invokeMethod(this, &DHTHarvester::stop, Qt::BlockingQueuedConnection);
    m_thread->quit();
    m_thread->wait();
}

void DHTHarvester::startWorker()
{
    if (m_thread)
        return;

    m_thread = new QThread;
    m_thread->setObjectName(u"DHTHarvester"_s);
    moveToThread(m_thread);
    connect(m_thread, &QThread::started, this, &DHTHarvester::onThreadStarted);
    // Low priority so the harvester always yields to foreground GUI work.
    m_thread->start(QThread::LowPriority);
}

void DHTHarvester::onThreadStarted()
{
    // Runs on the worker thread: create the timers here so every timeout fires on
    // this thread, then apply whatever configuration was latched before start.
    m_sampleTimer = new QTimer(this);
    m_timeoutTimer = new QTimer(this);
    m_scheduleTimer = new QTimer(this);
    m_pruneTimer = new QTimer(this);
    m_sightingFlushTimer = new QTimer(this);
    m_sampleTimer->setInterval(m_sampleIntervalMs);
    m_timeoutTimer->setInterval(TIMEOUT_SWEEP_MS);
    m_scheduleTimer->setInterval(SCHEDULE_INTERVAL_MS);
    m_pruneTimer->setInterval(PRUNE_INTERVAL_MS);
    m_sightingFlushTimer->setInterval(SIGHTING_FLUSH_INTERVAL_MS);
    connect(m_sampleTimer, &QTimer::timeout, this, &DHTHarvester::onSampleTimer);
    connect(m_timeoutTimer, &QTimer::timeout, this, &DHTHarvester::onTimeoutTimer);
    connect(m_scheduleTimer, &QTimer::timeout, this, &DHTHarvester::onScheduleTimer);
    connect(m_pruneTimer, &QTimer::timeout, this, &DHTHarvester::onPruneTimer);
    connect(m_sightingFlushTimer, &QTimer::timeout, this, &DHTHarvester::flushSightings);

    m_workerStarted = true;
    if (m_enabled)
        start();
}

bool DHTHarvester::isEnabled() const
{
    return m_enabled;
}

void DHTHarvester::setEnabled(const bool enabled)
{
    if (m_workerStarted && (QThread::currentThread() != thread()))
    {
        QMetaObject::invokeMethod(this, [this, enabled] { setEnabled(enabled); }, Qt::QueuedConnection);
        return;
    }

    if (m_enabled == enabled)
        return;
    m_enabled = enabled;

    if (!m_workerStarted)
        return;  // latched; onThreadStarted() will start() if still enabled

    if (m_enabled)
        start();
    else
        stop();
}

void DHTHarvester::setActiveCrawlEnabled(const bool enabled)
{
    if (m_workerStarted && (QThread::currentThread() != thread()))
    {
        QMetaObject::invokeMethod(this, [this, enabled] { setActiveCrawlEnabled(enabled); }, Qt::QueuedConnection);
        return;
    }
    m_activeCrawl = enabled;
}

void DHTHarvester::setMaxConcurrentMetadata(const int max)
{
    if (m_workerStarted && (QThread::currentThread() != thread()))
    {
        QMetaObject::invokeMethod(this, [this, max] { setMaxConcurrentMetadata(max); }, Qt::QueuedConnection);
        return;
    }
    m_maxConcurrent = std::clamp(max, 1, MAX_CONCURRENT_METADATA);
}

void DHTHarvester::setTuning(const HarvesterTuning &tuning)
{
    if (m_workerStarted && (QThread::currentThread() != thread()))
    {
        QMetaObject::invokeMethod(this, [this, tuning] { setTuning(tuning); }, Qt::QueuedConnection);
        return;
    }

    // Clamp to sane bounds: a misconfigured INI must never stall the crawl (e.g.
    // a zero interval busy-loops the timer) or let it run unbounded.
    m_sampleIntervalMs = std::clamp(tuning.sampleIntervalMs, 250, 5 * 60 * 1000);
    m_maxSampleNodesPerTick = std::clamp(tuning.maxSampleNodesPerTick, 1, 200);
    m_sampleBudgetPerTick = std::clamp(tuning.sampleBudgetPerTick, 1, 1000);
    m_recurseNodesPerSample = std::clamp(tuning.recurseNodesPerSample, 0, 50);
    m_metadataTimeoutAnnounceMs = std::clamp(tuning.metadataTimeoutAnnounceMs, 1000, 5 * 60 * 1000);
    m_metadataTimeoutSpeculativeMs = std::clamp(tuning.metadataTimeoutSpeculativeMs, 1000, 5 * 60 * 1000);

    // Apply the cadence live; the timer may not exist yet (latched pre-start).
    if (m_sampleTimer)
        m_sampleTimer->setInterval(m_sampleIntervalMs);
}

void DHTHarvester::setBoundInterface(const QString &configName, const QString &humanName)
{
    if (m_workerStarted && (QThread::currentThread() != thread()))
    {
        QMetaObject::invokeMethod(this, [this, configName, humanName] { setBoundInterface(configName, humanName); }, Qt::QueuedConnection);
        return;
    }
    m_boundIfaceConfigName = configName;
    m_boundIfaceHumanName = humanName;
}

void DHTHarvester::setSessionRestored(const bool restored)
{
    if (m_workerStarted && (QThread::currentThread() != thread()))
    {
        QMetaObject::invokeMethod(this, [this, restored] { setSessionRestored(restored); }, Qt::QueuedConnection);
        return;
    }
    m_sessionRestored = restored;
    if (m_sessionRestored)
        requestPump();  // drain anything discovered during startup
}

HarvestStore *DHTHarvester::store() const
{
    // Called from the GUI thread; read the race-free published handle (the worker
    // thread creates the store and publishes it here exactly once).
    return m_storeHandle.load(std::memory_order_acquire);
}

void DHTHarvester::fillRuntimeStats(HarvestStats &stats) const
{
    stats.samplesSent = m_statSamplesSent.load(std::memory_order_relaxed);
    stats.sampleReplies = m_statSampleReplies.load(std::memory_order_relaxed);
    stats.announcesSeen = m_statAnnounces.load(std::memory_order_relaxed);
    stats.metadataOk = m_statMetadataOk.load(std::memory_order_relaxed);
    stats.metadataTimeouts = m_statMetadataTimeouts.load(std::memory_order_relaxed);
    stats.trackedNodes = m_statTrackedNodes.load(std::memory_order_relaxed);
    stats.pendingFetch = m_statPending.load(std::memory_order_relaxed);
    stats.inFlightFetch = m_statInFlight.load(std::memory_order_relaxed);
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

    // The persistent index lives for the harvester's whole life once created, so it
    // stays queryable even while crawling is paused and GUI store() readers never
    // race a teardown. Create it on first start only.
    if (!m_storeThread)
    {
        m_storeThread = new QThread;
        m_storeThread->setObjectName(u"DHTHarvestStore"_s);
        auto *store = new HarvestStore(m_path);
        connect(m_storeThread, &QThread::started, store, &HarvestStore::init);
        store->moveToThread(m_storeThread);
        m_storeThread->start();
        m_store = store;  // worker-thread handle
        m_storeHandle.store(store, std::memory_order_release);  // publish to store()
    }

    m_sampleTimer->start();
    m_timeoutTimer->start();
    m_scheduleTimer->start();
    m_pruneTimer->start();
    m_sightingFlushTimer->start();
    m_running = true;

    LogMsg(tr("DHT harvester started (egress bound to interface \"%1\").").arg(m_boundIfaceHumanName), Log::INFO);
}

void DHTHarvester::stop()
{
    // Timers may be null if the worker thread never started (e.g. ~DHTHarvester
    // before startWorker(), or a second idempotent stop()).
    if (m_sampleTimer)
        m_sampleTimer->stop();
    if (m_timeoutTimer)
        m_timeoutTimer->stop();
    if (m_scheduleTimer)
        m_scheduleTimer->stop();
    if (m_pruneTimer)
        m_pruneTimer->stop();
    if (m_sightingFlushTimer)
        m_sightingFlushTimer->stop();

    flushSightings();

    // Cancelling an ephemeral metadata download touches GUI-thread Session state, so
    // marshal it. The posted calls target the Session object; SessionImpl drains them
    // on the GUI thread (on shutdown, ~SessionImpl flushes them before tear-down).
    for (auto it = m_inFlight.cbegin(); it != m_inFlight.cend(); ++it)
    {
        const QString ih = it.key();
        QMetaObject::invokeMethod(m_session, [s = m_session, ih] { s->cancelDownloadMetadata(TorrentID::fromString(ih)); }, Qt::QueuedConnection);
    }
    m_inFlight.clear();
    m_pending.clear();
    m_queued.clear();
    m_done.clear();
    m_sightingBuffer.clear();
    m_nodes.clear();
    m_candidateRequestInFlight = false;

    // The store (persistent SQLite index) is intentionally NOT torn down here: it
    // outlives crawl pause/resume so the index stays queryable and GUI store()
    // readers never race a deletion. It is destroyed only in ~DHTHarvester.

    m_running = false;
}

bool DHTHarvester::vpnReady() const
{
    // Reads only harvester-local snapshots (refreshed from the GUI thread via
    // setBoundInterface) plus the thread-safe QNetworkInterface enumeration, so it
    // is safe to call from the worker thread.
    const QString cfgName = m_boundIfaceConfigName;
    if (cfgName.isEmpty())
    {
        // No in-app interface bind: egress routing is delegated to an external
        // VPN app (e.g. hide.me per-app/split-tunnel routing). Allow, since the
        // OS forces this process through the tunnel regardless of the socket bind.
        return true;
    }

    const QString humanName = m_boundIfaceHumanName;
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

void DHTHarvester::postAlertEvent(const HarvestAlertEvent &event)
{
    // Called by SessionImpl on the GUI thread with a value-typed snapshot of a DHT
    // alert; hand it to the worker thread, where all crawl logic runs.
    QMetaObject::invokeMethod(this, [this, event] { onAlertEvent(event); }, Qt::QueuedConnection);
}

void DHTHarvester::onAlertEvent(const HarvestAlertEvent &event)
{
    if (!m_enabled || !m_running)
        return;

    switch (event.type)
    {
    case HarvestAlertEvent::Type::GetPeers:
        // Record the sighting; the store-driven scheduler picks it up by
        // availability/recency on the next tick (no in-memory popularity gate).
        postSighting(event.infoHashHex, u"get_peers"_s, {}, 0);
        break;
    case HarvestAlertEvent::Type::Announce:
        {
            m_statAnnounces.fetch_add(1, std::memory_order_relaxed);
            postSighting(event.infoHashHex, u"announce"_s, event.ip, event.port);
            enqueue(event.infoHashHex, /*fromAnnounce*/ true);  // a peer HAS it -> fetch immediately
            // The announcing peer holds the torrent: feed it straight to the
            // in-flight metadata download instead of re-finding peers via DHT.
            // connectDHTMetadataPeer touches GUI-thread Session state, so marshal it.
            if (const auto it = m_inFlight.find(event.infoHashHex); it != m_inFlight.end())
            {
                // A real announcer arrived for an in-flight (possibly speculative)
                // fetch: extend its deadline to the announce tier so it isn't cut
                // short, and hand the peer to the download.
                it.value() = std::max(it.value(), nowMs() + m_metadataTimeoutAnnounceMs);
                const QString ih = event.infoHashHex;
                const QString ip = event.ip;
                const int port = event.port;
                QMetaObject::invokeMethod(m_session, [s = m_session, ih, ip, port] { s->connectDHTMetadataPeer(ih, ip, port); }, Qt::QueuedConnection);
            }
        }
        break;
    case HarvestAlertEvent::Type::SampleInfohashes:
        {
            m_statSampleReplies.fetch_add(1, std::memory_order_relaxed);
            for (const QString &ih : event.samples)
                postSighting(ih, u"sample"_s, {}, 0);
            if (!m_activeCrawl)
                break;
            // Record this node's BEP-51 state so the frontier re-samples it only
            // after its advertised interval and prioritises high-infohash nodes.
            const QString key = event.ip + u':' + QString::number(event.port);
            if (const auto it = m_nodes.find(key); it != m_nodes.end())
            {
                it->lastSampledMs = nowMs();
                it->intervalSec = event.intervalSec;
                it->numInfohashes = event.numInfohashes;
            }
            // Walk the keyspace: the reply hands back nodes closer to the (random)
            // target. Add a bounded number to the frontier to be sampled in turn.
            int recursed = 0;
            for (const auto &node : event.nodes)
            {
                if (recursed++ >= m_recurseNodesPerSample)
                    break;
                discoverNode(node.first, node.second);
            }
        }
        break;
    case HarvestAlertEvent::Type::LiveNodes:
        {
            if (!m_activeCrawl)
                break;
            // Seed the frontier from routing-table nodes near a random target;
            // sampling itself happens on the timer via drainFrontier().
            int added = 0;
            for (const auto &node : event.nodes)
            {
                if (added++ >= m_maxSampleNodesPerTick)
                    break;
                discoverNode(node.first, node.second);
            }
        }
        break;
    case HarvestAlertEvent::Type::GetPeersReply:
        {
            // Real swarm size for a known infohash (BEP-33-ish): store the peer
            // count so the index/Torznab can report seeders/peers instead of a
            // sighting proxy.
            if (m_store && (event.numPeers > 0))
            {
                const QString ih = event.infoHashHex;
                const int peers = event.numPeers;
                QMetaObject::invokeMethod(m_store, [store = m_store, ih, peers] { store->updateSwarm(ih, peers); }, Qt::QueuedConnection);
            }
        }
        break;
    case HarvestAlertEvent::Type::PeerConnect:
        {
            // Seed BEP-51 sampling from the DHT nodes of peers we connect to in
            // swarms (our own ephemeral metadata fetches and real torrents alike).
            // A swarm peer is a live, reachable host whose DHT node stores the
            // infohashes announced near it -> a high-quality frontier seed.
            if (!m_activeCrawl || event.nodes.isEmpty())
                break;
            const auto &node = event.nodes.first();
            discoverNode(node.first, node.second);
        }
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
        // Keep feeding the frontier with fresh routing-table nodes near a random
        // target (-> LiveNodes alert -> discoverNode), then sample the most
        // promising due nodes already in the frontier.
        m_nativeSession->dht_live_nodes(randomTarget());
        drainFrontier();
    }

    pump();

    // Refresh the gauge diagnostics once per tick (cheap, off-GUI-thread).
    m_statTrackedNodes.store(static_cast<int>(m_nodes.size()), std::memory_order_relaxed);
    m_statPending.store(static_cast<int>(m_pending.size()), std::memory_order_relaxed);
    m_statInFlight.store(static_cast<int>(m_inFlight.size()), std::memory_order_relaxed);
}

void DHTHarvester::discoverNode(const QString &ip, const quint16 port)
{
    if (ip.isEmpty() || (port == 0))
        return;

    const QString key = ip + u':' + QString::number(port);
    if (m_nodes.contains(key))
        return;

    // Full frontier: drop the discovery. drainFrontier() recycles exhausted nodes
    // each tick, so this is rarely hit and keeps discovery O(1) under the firehose.
    if (m_nodes.size() >= MAX_TRACKED_NODES)
        return;

    NodeState state;
    state.ip = ip;
    state.port = port;
    m_nodes.insert(key, state);  // lastSampledMs == 0 -> due for sampling immediately
}

void DHTHarvester::drainFrontier()
{
    if (m_nodes.isEmpty())
        return;

    const qint64 now = nowMs();

    // Single pass: collect nodes due for (re)sampling and, at the same time, mark
    // exhausted nodes (already sampled, holding no infohashes and past their
    // interval) for recycling so the frontier stays productive.
    QList<QString> due;
    QList<QString> exhausted;
    for (auto it = m_nodes.cbegin(); it != m_nodes.cend(); ++it)
    {
        const NodeState &n = it.value();
        if (n.lastSampledMs == 0)
        {
            due.append(it.key());  // never sampled -> frontier expansion
            continue;
        }
        const qint64 nextDueMs = n.lastSampledMs + (qint64(qMax(n.intervalSec, 1)) * 1000);
        if (now < nextDueMs)
            continue;  // still within the node's advertised re-sample interval
        if (n.numInfohashes <= 0)
            exhausted.append(it.key());  // gave nothing last time -> recycle
        else
            due.append(it.key());
    }

    for (const QString &key : exhausted)
        m_nodes.remove(key);

    if (due.isEmpty())
        return;

    // Prefer never-sampled nodes (they expand the keyspace walk), then the nodes
    // reporting the most infohashes (richest to re-sample).
    std::sort(due.begin(), due.end(), [this](const QString &a, const QString &b)
    {
        const NodeState &na = m_nodes[a];
        const NodeState &nb = m_nodes[b];
        const bool aNew = (na.lastSampledMs == 0);
        const bool bNew = (nb.lastSampledMs == 0);
        if (aNew != bNew)
            return aNew;
        return na.numInfohashes > nb.numInfohashes;
    });

    int issued = 0;
    for (const QString &key : due)
    {
        if (issued >= m_sampleBudgetPerTick)
            break;
        const auto it = m_nodes.find(key);
        if (it == m_nodes.end())
            continue;
        if (const auto ep = toEndpoint(it->ip, it->port))
        {
            m_nativeSession->dht_sample_infohashes(*ep, randomTarget());
            it->lastSampledMs = now;  // optimistic; refined when the reply arrives
            ++issued;
            m_statSamplesSent.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void DHTHarvester::onTimeoutTimer()
{
    const qint64 now = nowMs();
    QStringList timedOut;
    for (auto it = m_inFlight.cbegin(); it != m_inFlight.cend(); ++it)
    {
        if (now > it.value())  // value is the source-tiered deadline
            timedOut.append(it.key());
    }

    for (const QString &ih : timedOut)
    {
        // GUI-thread Session state -> marshal the cancellation.
        QMetaObject::invokeMethod(m_session, [s = m_session, ih] { s->cancelDownloadMetadata(TorrentID::fromString(ih)); }, Qt::QueuedConnection);
        m_inFlight.remove(ih);
        m_queued.remove(ih);
        m_statMetadataTimeouts.fetch_add(1, std::memory_order_relaxed);
        if (m_store)
        {
            QMetaObject::invokeMethod(m_store, [store = m_store, ih] { store->noteFetchFailure(ih); }, Qt::QueuedConnection);
        }
    }

    requestPump();
}

void DHTHarvester::enqueue(const QString &infoHashV1, const bool fromAnnounce)
{
    if (infoHashV1.isEmpty() || m_done.contains(infoHashV1) || m_queued.contains(infoHashV1))
        return;

    // Cap the in-memory queue: at the DHT discovery rate the candidate set is
    // effectively unbounded, but the persistent store holds it all and the
    // scheduler re-supplies as fetch slots free, so dropping here is safe.
    if (m_pending.size() >= MAX_PENDING)
        return;

    m_queued.insert(infoHashV1);
    m_pending.enqueue({infoHashV1, fromAnnounce});
    requestPump();
}

void DHTHarvester::onScheduleTimer()
{
    if (!m_running || !m_store || m_candidateRequestInFlight)
        return;

    const int room = m_maxConcurrent - m_inFlight.size();
    if ((room <= 0) || (m_pending.size() >= MAX_PENDING))
        return;

    // Pull a little ahead of the free slots so the pump never starves; the store
    // returns only infohashes that still need metadata and are off backoff,
    // most promising first. The query runs on the store thread and the result is
    // delivered back asynchronously (onCandidates) -- never blocking this thread.
    const int batch = qMin(room * 4, MAX_PENDING - m_pending.size());
    m_candidateRequestInFlight = true;
    QMetaObject::invokeMethod(m_store, [this, store = m_store, batch]
    {
        const QList<QString> candidates = store->candidatesForMetadata(batch);
        QMetaObject::invokeMethod(this, [this, candidates] { onCandidates(candidates); }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void DHTHarvester::onCandidates(const QList<QString> &candidates)
{
    m_candidateRequestInFlight = false;
    // Store-driven candidates come from the persistent index (get_peers/sample
    // sightings); treat them as speculative (short timeout tier).
    for (const QString &ih : candidates)
        enqueue(ih, /*fromAnnounce*/ false);
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

void DHTHarvester::requestPump()
{
    // Coalesce: at the DHT discovery rate pump() would otherwise be called
    // thousands of times per second. Run it at most once per event-loop turn.
    if (m_pumpScheduled)
        return;
    m_pumpScheduled = true;
    QMetaObject::invokeMethod(this, [this] { m_pumpScheduled = false; pump(); }, Qt::QueuedConnection);
}

void DHTHarvester::pump()
{
    // Do not add metadata-download torrents until the Session has finished restoring:
    // their add_torrent_alerts would otherwise corrupt the startup resume-data
    // accounting and stall the restore (the GUI would hang on "loading torrents").
    if (!m_sessionRestored)
        return;

    while ((m_inFlight.size() < m_maxConcurrent) && !m_pending.isEmpty())
    {
        const PendingFetch fetch = m_pending.dequeue();
        const QString ih = fetch.infoHash;
        if (m_done.contains(ih) || m_inFlight.contains(ih))
        {
            m_queued.remove(ih);
            continue;
        }

        // TorrentDescriptor::parse() is pure (no Qt/Session state), so it runs here
        // on the worker thread.
        const QString magnet = u"magnet:?xt=urn:btih:"_s + ih;
        const auto descr = TorrentDescriptor::parse(magnet);
        if (!descr)
        {
            m_queued.remove(ih);
            continue;
        }

        // Claim the slot optimistically with a source-tiered deadline, then start
        // the (GUI-thread-affine) metadata download on the Session thread. If it
        // declines (already known/fetching), it posts back onDownloadRejected().
        const int timeoutMs = fetch.fromAnnounce ? m_metadataTimeoutAnnounceMs : m_metadataTimeoutSpeculativeMs;
        m_inFlight.insert(ih, nowMs() + timeoutMs);
        QMetaObject::invokeMethod(m_session, [this, s = m_session, d = descr.value(), ih]
        {
            if (!s->downloadMetadata(d))
                QMetaObject::invokeMethod(this, [this, ih] { onDownloadRejected(ih); }, Qt::QueuedConnection);
        }, Qt::QueuedConnection);
    }
}

void DHTHarvester::onDownloadRejected(const QString &infoHashV1)
{
    // The Session already knew this torrent (or was already fetching it): release
    // the optimistic claim so the slot can be reused.
    m_inFlight.remove(infoHashV1);
    m_queued.remove(infoHashV1);
    requestPump();
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
    m_statMetadataOk.fetch_add(1, std::memory_order_relaxed);

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

    requestPump();
}
