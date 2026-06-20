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

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>

#include "base/path.h"

namespace BitTorrent
{
    // A single observed file inside a harvested torrent (BEP-9 info-dict entry).
    struct HarvestedFile
    {
        QString path;
        qint64 size = 0;
    };

    // Full metadata for a torrent fetched via BEP-9.
    struct HarvestedTorrent
    {
        QString infoHashV1;   // 40-char lowercase hex (always present)
        QString infoHashV2;   // 64-char lowercase hex or empty
        QString name;
        qint64 size = 0;
        int fileCount = 0;
        qint64 pieceLength = 0;
        QList<HarvestedFile> files;
        QByteArray rawMetadata;   // bencoded info-dict, for reconstructing the torrent
    };

    // A single discovery event for an infohash (the "snoop"/demand signal).
    struct HarvestSighting
    {
        QString infoHashV1;
        QString source;       // "get_peers" | "announce" | "sample"
        QString peerIP;
        int peerPort = 0;
    };

    // A row returned by a search over the harvested index.
    struct HarvestSearchResult
    {
        QString infoHashV1;
        QString name;
        QString contentType;
        qint64 size = 0;
        int fileCount = 0;
        qint64 firstSeenMs = 0;
        qint64 lastSeenMs = 0;
        int sightings = 0;
        int peers = 0;            // last observed DHT swarm size (get_peers reply)
        bool metadataFetched = false;
    };

    struct HarvestSearchPage
    {
        QList<HarvestSearchResult> items;
        qint64 total = 0;  // full count across all pages, for paginated callers
    };

    struct HarvestStats
    {
        qint64 torrentCount = 0;     // distinct infohashes seen
        qint64 metadataCount = 0;    // infohashes with fetched metadata
        qint64 sightingCount = 0;    // total sighting rows
    };

    struct HarvestPruneStats
    {
        qint64 torrentsPruned = 0;   // metadata-less / exhausted / over-cap rows removed
        qint64 sightingsPruned = 0;  // stale sighting rows removed
    };

    // Local SQLite index of torrents harvested from the BitTorrent DHT.
    //
    // The store mirrors the emulebb-rust metadata conventions (WAL, FTS5 over
    // normalized names, first/last-seen freshness, schema-version reset). It owns
    // a single thread-affine QSqlDatabase connection, so every method must be
    // invoked on the thread the store lives in (the DHTHarvester worker thread).
    // Cross-thread callers (e.g. the GUI) use queued/blocking-queued invocation.
    class HarvestStore final : public QObject
    {
        Q_OBJECT
        Q_DISABLE_COPY_MOVE(HarvestStore)

    public:
        explicit HarvestStore(const Path &dbPath, QObject *parent = nullptr);
        ~HarvestStore() override;

        // True if the infohash is unknown or known-without-metadata and not
        // currently in fetch-backoff. Cheap; used to gate the metadata queue.
        bool needsMetadata(const QString &infoHashV1) const;

    public slots:
        // Opens the SQLite connection and ensures the schema. Must run on the
        // thread the store lives in (invoked after moveToThread()).
        void init();

        // Writers (invoked from the harvester, same thread).
        void recordSighting(const BitTorrent::HarvestSighting &sighting);
        void recordSightings(const QList<BitTorrent::HarvestSighting> &sightings);
        void recordMetadata(const BitTorrent::HarvestedTorrent &torrent);
        void noteFetchFailure(const QString &infoHashV1);
        // Records the real DHT swarm size (peer count from a get_peers reply).
        void updateSwarm(const QString &infoHashV1, int peers);

        // Readers (may be invoked via BlockingQueuedConnection from the GUI).
        BitTorrent::HarvestSearchPage search(const QString &query, int limit, int offset) const;
        BitTorrent::HarvestSearchPage recent(int limit, int offset) const;
        QByteArray metadataFor(const QString &infoHashV1) const;
        BitTorrent::HarvestStats stats() const;

        // Persistent metadata-fetch queue: infohashes still lacking metadata that
        // are not in fetch-backoff, most promising first (availability then
        // recency). Drives coverage across sessions, unlike the in-memory
        // per-session popularity gate.
        QList<QString> candidatesForMetadata(int limit) const;

        // Retention: drop metadata-less torrents that are fetch-exhausted and not
        // seen within staleAgeMs, prune sightings older than sightingRetentionMs,
        // and cap the table at maxTorrents (oldest metadata-less first).
        BitTorrent::HarvestPruneStats prune(qint64 staleAgeMs, qint64 sightingRetentionMs, qint64 maxTorrents);

    private:
        void openDatabase();
        void ensureSchema();
        void createSchema();

        const QString m_connectionName;
        const Path m_path;
    };
}

Q_DECLARE_METATYPE(BitTorrent::HarvestSighting)
Q_DECLARE_METATYPE(BitTorrent::HarvestedTorrent)
Q_DECLARE_METATYPE(BitTorrent::HarvestSearchResult)
Q_DECLARE_METATYPE(BitTorrent::HarvestStats)
Q_DECLARE_METATYPE(BitTorrent::HarvestPruneStats)
