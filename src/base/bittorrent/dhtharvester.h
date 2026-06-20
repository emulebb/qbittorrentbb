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

#include <QHash>
#include <QObject>
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

    // Magnetico/btdigg-style BitTorrent DHT harvester.
    //
    // Lives on the Session thread (where libtorrent alerts are dispatched) and
    // owns a worker thread hosting the HarvestStore (SQLite). It discovers
    // infohashes passively (incoming get_peers/announce) and actively (BEP-51
    // sample_infohashes), then fetches full metadata via the Session's existing
    // ephemeral metadata-download path (BEP-9) and indexes the result.
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

        void setEnabled(bool enabled);
        bool isEnabled() const;
        void setActiveCrawlEnabled(bool enabled);
        void setMaxConcurrentMetadata(int max);

        // Invoked by SessionImpl::handleAlert() for the DHT alert types.
        void handleAlert(const lt::alert *alert);

        // The store lives on the worker thread; the GUI uses it via
        // blocking-queued invocation.
        HarvestStore *store() const;

    signals:
        // Emitted (main thread) when a newly discovered torrent's metadata has
        // been fetched and indexed.
        void torrentIndexed(const QString &infoHashV1, const QString &name);

    private slots:
        void onMetadataDownloaded(const BitTorrent::TorrentInfo &info);
        void onSampleTimer();
        void onTimeoutTimer();
        void onScheduleTimer();   // pull persistent metadata candidates from the store
        void onPruneTimer();      // retention sweep
        void flushSightings();

    private:
        void start();
        void stop();
        void enqueue(const QString &infoHashV1);
        void pump();
        void postSighting(const QString &infoHashV1, const QString &source, const QString &ip, int port);
        bool vpnReady() const;

        Session *m_session = nullptr;
        lt::session *m_nativeSession = nullptr;
        const Path m_path;
        HarvestStore *m_store = nullptr;
        QThread *m_storeThread = nullptr;

        bool m_enabled = false;
        bool m_activeCrawl = true;
        int m_maxConcurrent = 20;
        bool m_running = false;
        bool m_warnedNoVPN = false;

        QQueue<QString> m_pending;
        QSet<QString> m_queued;             // in m_pending or m_inFlight (dedupe)
        QHash<QString, qint64> m_inFlight;  // infoHashV1 -> fetch start (ms)
        QSet<QString> m_done;               // fetched this session
        QList<HarvestSighting> m_sightingBuffer;

        QTimer *m_sampleTimer = nullptr;
        QTimer *m_timeoutTimer = nullptr;
        QTimer *m_scheduleTimer = nullptr;
        QTimer *m_pruneTimer = nullptr;
        QTimer *m_sightingFlushTimer = nullptr;
        int m_sampleBudget = 0;  // per-tick cap on outstanding BEP-51 sample requests
    };
}
