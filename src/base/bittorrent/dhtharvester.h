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

#include <libtorrent/fwd.hpp>

#include <atomic>

#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QQueue>
#include <QSet>
#include <QString>

#include "base/path.h"

class QThread;
class QTimer;

namespace BitTorrent
{
    class HarvestStore;
    class Session;
    class TorrentInfo;
    struct HarvestSighting;
    struct HarvestStats;
    struct HarvesterTuning;

    // A DHT alert reduced to a thread-safe value. SessionImpl extracts the fields
    // it needs from the (short-lived, libtorrent-owned) lt::alert on the GUI thread
    // and posts one of these to the harvester's worker thread, where it is handled
    // without ever touching libtorrent's recycled alert memory. Endpoints are kept
    // as (ip-string, port) so this struct stays free of libtorrent types.
    struct HarvestAlertEvent
    {
        enum class Type : quint8
        {
            GetPeers,
            Announce,
            SampleInfohashes,
            LiveNodes,
            GetPeersReply,
            PeerConnect
        };

        Type type;
        QString infoHashHex;                  // GetPeers / Announce / GetPeersReply
        QString ip;                           // Announce peer / SampleInfohashes responding node
        int port = 0;                         // Announce peer / SampleInfohashes responding node
        int numPeers = 0;                     // GetPeersReply
        int numInfohashes = 0;                // SampleInfohashes: total keys the responding node holds
        int intervalSec = 0;                  // SampleInfohashes: min seconds before re-sampling that node
        QList<QString> samples;               // SampleInfohashes (hex infohashes)
        QList<QPair<QString, quint16>> nodes; // sample/live-node/peer-connect endpoints to traverse into
    };

    // Magnetico/btdigg-style BitTorrent DHT harvester.
    //
    // Runs entirely on its own low-priority worker thread (created by startWorker())
    // so its crawling never steals cycles from — or blocks — the GUI thread. It also
    // owns a second worker thread hosting the HarvestStore (SQLite). SessionImpl
    // reduces each DHT alert to a HarvestAlertEvent on the GUI thread and posts it
    // here; the harvester discovers infohashes passively (incoming get_peers/announce)
    // and actively (BEP-51 sample_infohashes), then fetches full metadata via the
    // Session's existing ephemeral metadata-download path (BEP-9) and indexes the
    // result. Calls back into the (GUI-thread-affine) Session are marshalled.
    //
    // All DHT/BT traffic egresses through libtorrent's bound interface; the
    // harvester refuses to run unless the Session is bound to a (VPN) network
    // interface that is currently up (fail-closed) — see vpnReady().
    class DHTHarvester final : public QObject
    {
        Q_OBJECT
        Q_DISABLE_COPY_MOVE(DHTHarvester)

    public:
        DHTHarvester(Session *session, lt::session *nativeSession, const Path &dbPath, QObject *parent = nullptr);
        ~DHTHarvester() override;

        // Move the harvester onto its own worker thread and start its event loop.
        // Call once, on the GUI thread, after the initial setEnabled/setActiveCrawl/
        // setMaxConcurrent/setBoundInterface configuration has been latched.
        void startWorker();

        // Stop crawling and quit the worker thread (GUI thread, blocking). Call this
        // before deleting the harvester so any in-flight metadata cancellations are
        // posted to the Session before tear-down.
        void shutdown();

        // All setters are thread-safe: when called from another thread they marshal
        // onto the worker thread. isEnabled() returns the last value set (atomic).
        void setEnabled(bool enabled);
        bool isEnabled() const;
        void setActiveCrawlEnabled(bool enabled);
        void setMaxConcurrentMetadata(int max);
        // Active-crawl throughput knobs (sample cadence/fan-out, fetch timeout).
        // Applied live: the sampling timer interval is updated in place.
        void setTuning(const HarvesterTuning &tuning);
        // Snapshot of the Session's bound (VPN) interface, since the harvester must
        // not read SessionImpl's CachedSettingValue members across threads.
        void setBoundInterface(const QString &configName, const QString &humanName);

        // Gate: until the Session has finished restoring its torrents, the harvester
        // must NOT start metadata downloads. Those add ephemeral torrents whose
        // add_torrent_alerts would corrupt the startup resume-data accounting and
        // stall the session restore. Passive discovery (sightings/sampling) is fine.
        void setSessionRestored(bool restored);

        // Invoked by SessionImpl::dispatchHarvesterAlert() on the GUI thread; posts
        // the event to the worker thread.
        void postAlertEvent(const HarvestAlertEvent &event);

        // The store lives on its own worker thread; the GUI uses it via
        // blocking-queued invocation.
        HarvestStore *store() const;

    signals:
        // Emitted (main thread) when a newly discovered torrent's metadata has
        // been fetched and indexed.
        void torrentIndexed(const QString &infoHashV1, const QString &name);

    private slots:
        void onThreadStarted();   // runs on the worker thread: create timers, apply latched state
        void onAlertEvent(const BitTorrent::HarvestAlertEvent &event);
        void onMetadataDownloaded(const BitTorrent::TorrentInfo &info);
        void onSampleTimer();
        void onTimeoutTimer();
        void onScheduleTimer();   // pull persistent metadata candidates from the store
        void onCandidates(const QList<QString> &candidates);  // async reply from the store
        void onDownloadRejected(const QString &infoHashV1);   // GUI declined a metadata fetch
        void onPruneTimer();      // retention sweep
        void flushSightings();

    private:
        void start();
        void stop();
        void enqueue(const QString &infoHashV1);
        void pump();
        void requestPump();       // coalesce pump() to at most once per event-loop turn
        void postSighting(const QString &infoHashV1, const QString &source, const QString &ip, int port);
        bool vpnReady() const;
        // Add a freshly discovered DHT node to the sampling frontier (deduped,
        // capped). Newly added nodes are immediately due for a BEP-51 sample.
        void discoverNode(const QString &ip, quint16 port);
        // Issue up to m_sampleBudgetPerTick BEP-51 sample_infohashes requests to the
        // most promising frontier nodes that are off their per-node interval.
        void drainFrontier();

        Session *m_session = nullptr;
        lt::session *m_nativeSession = nullptr;
        const Path m_path;
        HarvestStore *m_store = nullptr;            // worker-thread-only handle
        // Published copy of m_store read by store() from the GUI thread. The store is
        // created once and lives until ~DHTHarvester, so this only ever transitions
        // null -> store; an atomic makes that publication race-free.
        std::atomic<HarvestStore *> m_storeHandle = nullptr;
        QThread *m_storeThread = nullptr;
        QThread *m_thread = nullptr;        // the harvester's own (low-priority) worker thread

        bool m_workerStarted = false;       // worker event loop running; setters then marshal
        bool m_sessionRestored = false;     // Session finished restoring -> fetches allowed
        bool m_enabled = false;
        bool m_activeCrawl = true;
        int m_maxConcurrent = 8;
        // Active-crawl throughput knobs, latched from Session INI via setTuning().
        // Defaults mirror HarvesterTuning; SessionImpl overrides them at startup.
        int m_sampleIntervalMs = 4000;
        int m_maxSampleNodesPerTick = 10;
        int m_sampleBudgetPerTick = 24;
        int m_recurseNodesPerSample = 3;
        int m_metadataTimeoutMs = 10000;
        bool m_running = false;
        bool m_warnedNoVPN = false;
        bool m_pumpScheduled = false;            // a coalesced pump() is already queued
        bool m_candidateRequestInFlight = false; // a store candidate query is outstanding
        QString m_boundIfaceConfigName;     // snapshot of Session::networkInterface()
        QString m_boundIfaceHumanName;      // snapshot of Session::networkInterfaceName()

        QQueue<QString> m_pending;
        QSet<QString> m_queued;             // in m_pending or m_inFlight (dedupe)
        QHash<QString, qint64> m_inFlight;  // infoHashV1 -> fetch start (ms)
        QSet<QString> m_done;               // fetched this session
        QList<HarvestSighting> m_sightingBuffer;

        // BEP-51 sampling frontier: every DHT node we have discovered (via live-node
        // seeding, sample replies, and swarm peer connects), keyed by "ip:port". The
        // active crawl walks the keyspace by repeatedly sampling these nodes -- each
        // sample reply hands back closer nodes, which are added here in turn -- while
        // honouring each node's advertised re-sample interval and preferring nodes
        // that hold more infohashes.
        struct NodeState
        {
            QString ip;
            quint16 port = 0;
            qint64 lastSampledMs = 0;  // 0 = never sampled (due immediately)
            int intervalSec = 0;       // node-advertised min seconds between samples
            int numInfohashes = 0;     // keys the node reported holding (priority hint)
        };
        QHash<QString, NodeState> m_nodes;  // "ip:port" -> state

        QTimer *m_sampleTimer = nullptr;
        QTimer *m_timeoutTimer = nullptr;
        QTimer *m_scheduleTimer = nullptr;
        QTimer *m_pruneTimer = nullptr;
        QTimer *m_sightingFlushTimer = nullptr;

        // Live-crawl diagnostics. Written on the worker thread, read lock-free from
        // any thread via fillRuntimeStats(). Cumulative counters plus current gauges.
        std::atomic<qint64> m_statSamplesSent = 0;
        std::atomic<qint64> m_statSampleReplies = 0;
        std::atomic<qint64> m_statAnnounces = 0;
        std::atomic<qint64> m_statMetadataOk = 0;
        std::atomic<qint64> m_statMetadataTimeouts = 0;
        std::atomic<int> m_statTrackedNodes = 0;
        std::atomic<int> m_statPending = 0;
        std::atomic<int> m_statInFlight = 0;
    };
}
