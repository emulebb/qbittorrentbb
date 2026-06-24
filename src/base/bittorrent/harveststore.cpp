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

#include "harveststore.h"

#include <atomic>

#include <QDateTime>
#include <QPair>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>

#include "base/exceptions.h"
#include "base/global.h"
#include "base/logger.h"
#include "harvestcontentclassifier.h"

using namespace Qt::Literals::StringLiterals;

namespace
{
    const int DB_VERSION = 6;

    // Fetch-backoff policy for metadata acquisition.
    const int MAX_FETCH_ATTEMPTS = 5;
    const qint64 FETCH_BACKOFF_MS = 30 * 60 * 1000;  // 30 minutes between attempts

    std::atomic<quint64> connectionCounter = 0;

    qint64 nowMs()
    {
        return QDateTime::currentMSecsSinceEpoch();
    }

    // NFKC -> lowercase -> alphanumeric-only (others collapse to single spaces),
    // mirroring emulebb-rust normalize_search_text so the index aligns with the
    // org's search conventions.
    QString normalizeText(const QString &value)
    {
        const QString decomposed = value.normalized(QString::NormalizationForm_KC);
        QString out;
        out.reserve(decomposed.size());
        bool pendingSpace = false;
        for (const QChar ch : decomposed)
        {
            if (ch.isLetterOrNumber())
            {
                if (pendingSpace && !out.isEmpty())
                    out.append(u' ');
                pendingSpace = false;
                out.append(ch.toLower());
            }
            else
            {
                pendingSpace = true;
            }
        }
        return out;
    }

    // Turn a normalized query into a forgiving FTS5 prefix MATCH expression.
    QString toFtsMatch(const QString &normalizedQuery)
    {
        const QStringList tokens = normalizedQuery.split(u' ', Qt::SkipEmptyParts);
        QStringList terms;
        terms.reserve(tokens.size());
        for (const QString &token : tokens)
            terms.append(token + u'*');
        return terms.join(u' ');
    }

    // Shared SELECT column list (and matching reader) for every windowed index
    // read, so search/recent and their per-type variants stay in lockstep. The
    // torrents table must be aliased "t".
    const QString SELECT_RESULT_COLS =
            u"t.infohash_v1, t.name, t.content_type, t.size_bytes, t.file_count,"
            u" t.first_seen_ms, t.last_seen_ms, t.metadata_fetched, t.swarm_peers,"
            u" t.tracker_seeds, t.tracker_leechers, t.tracker_scrape_ms,"
            u" (SELECT count(*) FROM infohash_sightings s WHERE s.infohash_v1 = t.infohash_v1)"_s;

    BitTorrent::HarvestSearchResult readResultRow(const QSqlQuery &query)
    {
        BitTorrent::HarvestSearchResult row;
        row.infoHashV1 = query.value(0).toString();
        row.name = query.value(1).toString();
        row.contentType = query.value(2).toString();
        row.size = query.value(3).toLongLong();
        row.fileCount = query.value(4).toInt();
        row.firstSeenMs = query.value(5).toLongLong();
        row.lastSeenMs = query.value(6).toLongLong();
        row.metadataFetched = (query.value(7).toInt() != 0);
        row.peers = query.value(8).toInt();
        row.trackerSeeds = query.value(9).toInt();
        row.trackerLeechers = query.value(10).toInt();
        row.trackerScrapeMs = query.value(11).toLongLong();
        row.sightings = query.value(12).toInt();
        return row;
    }

    // Build the ORDER BY body for a windowed read. Default keeps the natural order
    // (search relevance / feed recency); an explicit column maps to its SQL expression.
    // A deterministic "t.id ASC" tiebreaker is always appended so LIMIT/OFFSET windows
    // never overlap or skip rows. (Sightings sorts by the indexed availability_score,
    // a cheap monotonic proxy for the displayed sighting count.)
    QString orderByClause(const BitTorrent::HarvestSortColumn col, const bool descending, const bool isSearch)
    {
        using SC = BitTorrent::HarvestSortColumn;
        if (col == SC::Default)
        {
            return isSearch
                    ? u"bm25(torrent_name_fts), t.availability_score DESC, t.last_seen_ms DESC, t.id ASC"_s
                    : u"t.first_seen_ms DESC, t.id ASC"_s;
        }

        QString expr;
        switch (col)
        {
        case SC::Name: expr = u"t.name"_s; break;
        case SC::Content: expr = u"t.content_type"_s; break;
        case SC::Size: expr = u"t.size_bytes"_s; break;
        case SC::Files: expr = u"t.file_count"_s; break;
        case SC::Seeds: expr = u"t.tracker_seeds"_s; break;
        case SC::Leechers: expr = u"t.tracker_leechers"_s; break;
        case SC::Sightings: expr = u"t.availability_score"_s; break;
        case SC::FirstSeen: expr = u"t.first_seen_ms"_s; break;
        case SC::LastSeen: expr = u"t.last_seen_ms"_s; break;
        default: expr = u"t.last_seen_ms"_s; break;
        }
        return expr + (descending ? u" DESC"_s : u" ASC"_s) + u", t.id ASC"_s;
    }

    QList<BitTorrent::HarvestedFile> filesForTorrent(QSqlDatabase &db, const qint64 torrentId)
    {
        QList<BitTorrent::HarvestedFile> files;
        QSqlQuery query {db};
        query.prepare(u"SELECT path, size_bytes FROM torrent_files WHERE torrent_id = :tid ORDER BY file_index;"_s);
        query.bindValue(u":tid"_s, torrentId);
        if (!query.exec())
            return files;

        while (query.next())
        {
            BitTorrent::HarvestedFile file;
            file.path = query.value(0).toString();
            file.size = query.value(1).toLongLong();
            files.append(file);
        }
        return files;
    }
}

using namespace BitTorrent;

HarvestStore::HarvestStore(const Path &dbPath, QObject *parent)
    : QObject(parent)
    , m_connectionName {u"DHTHarvestStore-%1"_s.arg(connectionCounter.fetch_add(1))}
    , m_path {dbPath}
{
}

HarvestStore::~HarvestStore()
{
    if (!QSqlDatabase::contains(m_connectionName))
        return;

    {
        // The QSqlDatabase instance must be out of scope before removeDatabase().
        auto db = QSqlDatabase::database(m_connectionName, false);
        if (db.isOpen())
            db.close();
    }
    QSqlDatabase::removeDatabase(m_connectionName);
}

void HarvestStore::init()
{
    openDatabase();
    ensureSchema();
}

void HarvestStore::openDatabase()
{
    auto db = QSqlDatabase::addDatabase(u"QSQLITE"_s, m_connectionName);
    db.setDatabaseName(m_path.data());
    if (!db.open())
        throw RuntimeError(db.lastError().text());

    QSqlQuery query {db};
    query.exec(u"PRAGMA journal_mode = WAL;"_s);
    query.exec(u"PRAGMA foreign_keys = ON;"_s);
    query.exec(u"PRAGMA synchronous = NORMAL;"_s);
}

void HarvestStore::ensureSchema()
{
    auto db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query {db};

    const bool hasMeta = db.tables().contains(u"harvest_meta"_s);
    int version = 0;
    if (hasMeta)
    {
        if (query.exec(u"SELECT value FROM harvest_meta WHERE name = 'version';"_s) && query.next())
            version = query.value(0).toInt();
    }

    if (hasMeta && (version == DB_VERSION))
        return;

    // Fresh database: build the current schema directly.
    if (!hasMeta)
    {
        createSchema();
        return;
    }

    // Known older schema: migrate forward one step at a time, preserving the index.
    // Unknown/newer versions fall through to a reset below.
    if ((version >= 3) && (version < DB_VERSION))
    {
        LogMsg(QObject::tr("Migrating DHT harvest index from schema %1 to %2.").arg(version).arg(DB_VERSION), Log::INFO);
        if (!db.transaction())
            throw RuntimeError(db.lastError().text());

        try
        {
            // 3 -> 4: content_type column + classify backfill.
            if (version < 4)
            {
                if (!query.exec(u"ALTER TABLE torrents ADD COLUMN content_type TEXT NOT NULL DEFAULT 'other';"_s))
                    throw RuntimeError(query.lastError().text());

                QSqlQuery torrents {db};
                if (!torrents.exec(u"SELECT id, size_bytes FROM torrents WHERE metadata_fetched = 1;"_s))
                    throw RuntimeError(torrents.lastError().text());

                QList<QPair<qint64, qint64>> torrentRows;
                while (torrents.next())
                    torrentRows.append({torrents.value(0).toLongLong(), torrents.value(1).toLongLong()});

                for (const auto &[torrentId, totalSize] : torrentRows)
                {
                    QSqlQuery update {db};
                    update.prepare(u"UPDATE torrents SET content_type = :content WHERE id = :id;"_s);
                    update.bindValue(u":content"_s, BitTorrent::classifyHarvestedContent(filesForTorrent(db, torrentId), totalSize));
                    update.bindValue(u":id"_s, torrentId);
                    if (!update.exec())
                        throw RuntimeError(update.lastError().text());
                }
            }

            // 4 -> 5: announce_count column + backfill from the sighting history so the
            // fetch scheduler immediately favours infohashes a peer was seen holding.
            if (version < 5)
            {
                if (!query.exec(u"ALTER TABLE torrents ADD COLUMN announce_count INTEGER NOT NULL DEFAULT 0;"_s))
                    throw RuntimeError(query.lastError().text());
                if (!query.exec(u"UPDATE torrents SET announce_count ="
                                u" (SELECT COUNT(*) FROM infohash_sightings s"
                                u"  WHERE s.infohash_v1 = torrents.infohash_v1 AND s.source = 'announce');"_s))
                    throw RuntimeError(query.lastError().text());
            }

            // 5 -> 6: tracker-scrape columns + content_type index (grouped tree).
            if (version < 6)
            {
                for (const QString &stmt : {
                        u"ALTER TABLE torrents ADD COLUMN tracker_seeds INTEGER NOT NULL DEFAULT -1;"_s,
                        u"ALTER TABLE torrents ADD COLUMN tracker_leechers INTEGER NOT NULL DEFAULT -1;"_s,
                        u"ALTER TABLE torrents ADD COLUMN tracker_scrape_ms INTEGER NOT NULL DEFAULT 0;"_s,
                        u"CREATE INDEX IF NOT EXISTS torrents_content_type_idx ON torrents(content_type, metadata_fetched);"_s})
                {
                    if (!query.exec(stmt))
                        throw RuntimeError(query.lastError().text());
                }
            }

            query.prepare(u"UPDATE harvest_meta SET value = :version WHERE name = 'version';"_s);
            query.bindValue(u":version"_s, DB_VERSION);
            if (!query.exec())
                throw RuntimeError(query.lastError().text());

            if (!db.commit())
                throw RuntimeError(db.lastError().text());
        }
        catch (const RuntimeError &)
        {
            db.rollback();
            throw;
        }
        return;
    }

    // Unknown version: reset our tables and (re)create.
    LogMsg(QObject::tr("Resetting DHT harvest index (schema changed)."), Log::INFO);

    for (const QString &stmt : {
            u"DROP TRIGGER IF EXISTS torrents_fts_ai;"_s,
            u"DROP TRIGGER IF EXISTS torrents_fts_ad;"_s,
            u"DROP TRIGGER IF EXISTS torrents_fts_au;"_s,
            u"DROP TABLE IF EXISTS torrent_name_fts;"_s,
            u"DROP TABLE IF EXISTS torrent_files;"_s,
            u"DROP TABLE IF EXISTS infohash_sightings;"_s,
            u"DROP TABLE IF EXISTS torrents;"_s,
            u"DROP TABLE IF EXISTS harvest_meta;"_s})
    {
        query.exec(stmt);
    }

    createSchema();
}

void HarvestStore::createSchema()
{
    auto db = QSqlDatabase::database(m_connectionName);
    if (!db.transaction())
        throw RuntimeError(db.lastError().text());

    QSqlQuery query {db};
    const auto exec = [&query](const QString &statement)
    {
        if (!query.exec(statement))
            throw RuntimeError(query.lastError().text());
    };

    try
    {
        exec(u"CREATE TABLE harvest_meta (name TEXT PRIMARY KEY, value);"_s);
        exec(u"INSERT INTO harvest_meta (name, value) VALUES ('version', %1);"_s.arg(DB_VERSION));

        exec(u"CREATE TABLE torrents ("
             u"id INTEGER PRIMARY KEY,"
             u"infohash_v1 TEXT NOT NULL UNIQUE,"
             u"infohash_v2 TEXT,"
             u"name TEXT NOT NULL DEFAULT '',"
             u"normalized_name TEXT NOT NULL DEFAULT '',"
             u"content_type TEXT NOT NULL DEFAULT 'other',"
             u"size_bytes INTEGER NOT NULL DEFAULT 0,"
             u"file_count INTEGER NOT NULL DEFAULT 0,"
             u"piece_length INTEGER NOT NULL DEFAULT 0,"
             u"metadata_fetched INTEGER NOT NULL DEFAULT 0,"
             u"metadata BLOB,"
             u"fetch_attempts INTEGER NOT NULL DEFAULT 0,"
             u"last_attempt_ms INTEGER NOT NULL DEFAULT 0,"
             u"availability_score INTEGER NOT NULL DEFAULT 0,"
             u"announce_count INTEGER NOT NULL DEFAULT 0,"
             u"swarm_peers INTEGER NOT NULL DEFAULT 0,"
             u"swarm_seen_ms INTEGER NOT NULL DEFAULT 0,"
             u"tracker_seeds INTEGER NOT NULL DEFAULT -1,"
             u"tracker_leechers INTEGER NOT NULL DEFAULT -1,"
             u"tracker_scrape_ms INTEGER NOT NULL DEFAULT 0,"
             u"first_seen_ms INTEGER NOT NULL,"
             u"last_seen_ms INTEGER NOT NULL,"
             u"updated_at_ms INTEGER NOT NULL);"_s);
        // Speeds up the grouped index tree's per-content-type counts and windows.
        exec(u"CREATE INDEX torrents_content_type_idx ON torrents(content_type, metadata_fetched);"_s);

        exec(u"CREATE TABLE torrent_files ("
             u"id INTEGER PRIMARY KEY,"
             u"torrent_id INTEGER NOT NULL REFERENCES torrents(id) ON DELETE CASCADE,"
             u"file_index INTEGER NOT NULL,"
             u"path TEXT NOT NULL,"
             u"size_bytes INTEGER NOT NULL DEFAULT 0);"_s);
        exec(u"CREATE INDEX torrent_files_torrent_idx ON torrent_files(torrent_id);"_s);

        exec(u"CREATE TABLE infohash_sightings ("
             u"id INTEGER PRIMARY KEY,"
             u"infohash_v1 TEXT NOT NULL,"
             u"source TEXT NOT NULL,"
             u"peer_ip TEXT NOT NULL DEFAULT '',"
             u"peer_port INTEGER NOT NULL DEFAULT 0,"
             u"observed_at_ms INTEGER NOT NULL);"_s);
        exec(u"CREATE INDEX infohash_sightings_idx ON infohash_sightings(infohash_v1, observed_at_ms);"_s);

        // FTS5 over normalized names, external-content on torrents, kept in sync
        // by triggers (the file_name_fts pattern from emulebb-rust).
        exec(u"CREATE VIRTUAL TABLE torrent_name_fts USING fts5("
             u"name, normalized_name,"
             u"content='torrents', content_rowid='id',"
             u"tokenize = \"unicode61 remove_diacritics 2 tokenchars '.-_'\");"_s);

        exec(u"CREATE TRIGGER torrents_fts_ai AFTER INSERT ON torrents BEGIN"
             u" INSERT INTO torrent_name_fts(rowid, name, normalized_name)"
             u" VALUES (new.id, new.name, new.normalized_name); END;"_s);
        exec(u"CREATE TRIGGER torrents_fts_ad AFTER DELETE ON torrents BEGIN"
             u" INSERT INTO torrent_name_fts(torrent_name_fts, rowid, name, normalized_name)"
             u" VALUES ('delete', old.id, old.name, old.normalized_name); END;"_s);
        exec(u"CREATE TRIGGER torrents_fts_au AFTER UPDATE ON torrents BEGIN"
             u" INSERT INTO torrent_name_fts(torrent_name_fts, rowid, name, normalized_name)"
             u" VALUES ('delete', old.id, old.name, old.normalized_name);"
             u" INSERT INTO torrent_name_fts(rowid, name, normalized_name)"
             u" VALUES (new.id, new.name, new.normalized_name); END;"_s);

        if (!db.commit())
            throw RuntimeError(db.lastError().text());
    }
    catch (const RuntimeError &)
    {
        db.rollback();
        throw;
    }
}

void HarvestStore::recordSighting(const HarvestSighting &sighting)
{
    recordSightings({sighting});
}

void HarvestStore::recordSightings(const QList<HarvestSighting> &sightings)
{
    if (sightings.isEmpty())
        return;

    const qint64 now = nowMs();
    auto db = QSqlDatabase::database(m_connectionName);
    if (!db.transaction())
        return;

    QSqlQuery torrentQuery {db};
    QSqlQuery sightingQuery {db};
    try
    {
        // announce_count tracks how often a peer was actually seen *holding* the
        // torrent (announce), as opposed to merely *asking* for it (get_peers/sample);
        // it is the strongest "this is fetchable" signal for the metadata scheduler.
        torrentQuery.prepare(u"INSERT INTO torrents (infohash_v1, first_seen_ms, last_seen_ms, updated_at_ms, availability_score, announce_count)"
                  u" VALUES (:ih, :now, :now, :now, 1, :annInc)"
                  u" ON CONFLICT(infohash_v1) DO UPDATE SET"
                  u" last_seen_ms = :now, updated_at_ms = :now,"
                  u" availability_score = availability_score + 1,"
                  u" announce_count = announce_count + :annInc;"_s);

        sightingQuery.prepare(u"INSERT INTO infohash_sightings (infohash_v1, source, peer_ip, peer_port, observed_at_ms)"
                  u" VALUES (:ih, :source, :ip, :port, :now);"_s);

        for (const HarvestSighting &sighting : sightings)
        {
            if (sighting.infoHashV1.isEmpty())
                continue;

            torrentQuery.bindValue(u":ih"_s, sighting.infoHashV1);
            torrentQuery.bindValue(u":now"_s, now);
            torrentQuery.bindValue(u":annInc"_s, (sighting.source == u"announce"_s) ? 1 : 0);
            if (!torrentQuery.exec())
                throw RuntimeError(torrentQuery.lastError().text());

            sightingQuery.bindValue(u":ih"_s, sighting.infoHashV1);
            sightingQuery.bindValue(u":source"_s, sighting.source);
            // Bind a non-null empty string for passive sightings (get_peers/sample
            // carry no peer IP); a null QString would bind SQL NULL and violate NOT NULL.
            sightingQuery.bindValue(u":ip"_s, sighting.peerIP.isNull() ? QString::fromLatin1("") : sighting.peerIP);
            sightingQuery.bindValue(u":port"_s, sighting.peerPort);
            sightingQuery.bindValue(u":now"_s, now);
            if (!sightingQuery.exec())
                throw RuntimeError(sightingQuery.lastError().text());
        }

        if (!db.commit())
            throw RuntimeError(db.lastError().text());
    }
    catch (const RuntimeError &err)
    {
        db.rollback();
        LogMsg(QObject::tr("DHT harvest: failed to record sightings. %1").arg(err.message()), Log::WARNING);
    }
}

void HarvestStore::recordMetadata(const HarvestedTorrent &torrent)
{
    if (torrent.infoHashV1.isEmpty())
        return;

    const qint64 now = nowMs();
    const QString normalized = normalizeText(torrent.name);

    auto db = QSqlDatabase::database(m_connectionName);
    if (!db.transaction())
        return;

    QSqlQuery query {db};
    try
    {
        const QString contentType = classifyHarvestedContent(torrent.files, torrent.size);

        query.prepare(u"INSERT INTO torrents"
                      u" (infohash_v1, infohash_v2, name, normalized_name, content_type, size_bytes, file_count,"
                      u"  piece_length, metadata, metadata_fetched, first_seen_ms, last_seen_ms, updated_at_ms)"
                      u" VALUES (:ih, :ih2, :name, :norm, :content, :size, :files, :piece, :meta, 1, :now, :now, :now)"
                      u" ON CONFLICT(infohash_v1) DO UPDATE SET"
                      u" infohash_v2 = :ih2, name = :name, normalized_name = :norm, content_type = :content,"
                      u" size_bytes = :size, file_count = :files, piece_length = :piece,"
                      u" metadata = :meta, metadata_fetched = 1, last_seen_ms = :now, updated_at_ms = :now;"_s);
        query.bindValue(u":ih"_s, torrent.infoHashV1);
        query.bindValue(u":ih2"_s, torrent.infoHashV2.isEmpty() ? QVariant() : QVariant(torrent.infoHashV2));
        query.bindValue(u":name"_s, torrent.name);
        query.bindValue(u":norm"_s, normalized);
        query.bindValue(u":content"_s, contentType);
        query.bindValue(u":size"_s, torrent.size);
        query.bindValue(u":files"_s, torrent.fileCount);
        query.bindValue(u":piece"_s, torrent.pieceLength);
        query.bindValue(u":meta"_s, torrent.rawMetadata.isEmpty() ? QVariant() : QVariant(torrent.rawMetadata));
        query.bindValue(u":now"_s, now);
        if (!query.exec())
            throw RuntimeError(query.lastError().text());

        query.prepare(u"SELECT id FROM torrents WHERE infohash_v1 = :ih;"_s);
        query.bindValue(u":ih"_s, torrent.infoHashV1);
        if (!query.exec() || !query.next())
            throw RuntimeError(query.lastError().text());
        const qint64 torrentRowId = query.value(0).toLongLong();

        query.prepare(u"DELETE FROM torrent_files WHERE torrent_id = :tid;"_s);
        query.bindValue(u":tid"_s, torrentRowId);
        query.exec();

        int index = 0;
        for (const HarvestedFile &file : torrent.files)
        {
            query.prepare(u"INSERT INTO torrent_files (torrent_id, file_index, path, size_bytes)"
                          u" VALUES (:tid, :idx, :path, :size);"_s);
            query.bindValue(u":tid"_s, torrentRowId);
            query.bindValue(u":idx"_s, index++);
            query.bindValue(u":path"_s, file.path);
            query.bindValue(u":size"_s, file.size);
            if (!query.exec())
                throw RuntimeError(query.lastError().text());
        }

        if (!db.commit())
            throw RuntimeError(db.lastError().text());
    }
    catch (const RuntimeError &err)
    {
        db.rollback();
        LogMsg(QObject::tr("DHT harvest: failed to store metadata. %1").arg(err.message()), Log::WARNING);
    }
}

void HarvestStore::noteFetchFailure(const QString &infoHashV1)
{
    if (infoHashV1.isEmpty())
        return;

    const qint64 now = nowMs();
    auto db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query {db};
    query.prepare(u"UPDATE torrents SET fetch_attempts = fetch_attempts + 1,"
                  u" last_attempt_ms = :now, updated_at_ms = :now WHERE infohash_v1 = :ih;"_s);
    query.bindValue(u":now"_s, now);
    query.bindValue(u":ih"_s, infoHashV1);
    query.exec();
}

void HarvestStore::updateSwarm(const QString &infoHashV1, int peers)
{
    if (infoHashV1.isEmpty() || (peers < 0))
        return;

    const qint64 now = nowMs();
    auto db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query {db};
    // Only update an already-known torrent; we don't create a row just for a
    // swarm reading (a sighting/metadata event is what introduces the torrent).
    query.prepare(u"UPDATE torrents SET swarm_peers = :peers, swarm_seen_ms = :now,"
                  u" updated_at_ms = :now WHERE infohash_v1 = :ih;"_s);
    query.bindValue(u":peers"_s, peers);
    query.bindValue(u":now"_s, now);
    query.bindValue(u":ih"_s, infoHashV1);
    query.exec();
}

void HarvestStore::updateTrackerScrape(const QString &infoHashV1, int seeds, int leechers)
{
    if (infoHashV1.isEmpty() || (seeds < 0) || (leechers < 0))
        return;

    const qint64 now = nowMs();
    auto db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query {db};
    // Like updateSwarm: only annotate an already-known torrent, never create a row
    // from a scrape alone. The scrape timestamp gates the GUI's re-scrape freshness.
    query.prepare(u"UPDATE torrents SET tracker_seeds = :seeds, tracker_leechers = :leechers,"
                  u" tracker_scrape_ms = :now, updated_at_ms = :now WHERE infohash_v1 = :ih;"_s);
    query.bindValue(u":seeds"_s, seeds);
    query.bindValue(u":leechers"_s, leechers);
    query.bindValue(u":now"_s, now);
    query.bindValue(u":ih"_s, infoHashV1);
    query.exec();
}

bool HarvestStore::needsMetadata(const QString &infoHashV1) const
{
    if (infoHashV1.isEmpty())
        return false;

    auto db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query {db};
    query.prepare(u"SELECT metadata_fetched, fetch_attempts, last_attempt_ms"
                  u" FROM torrents WHERE infohash_v1 = :ih;"_s);
    query.bindValue(u":ih"_s, infoHashV1);
    if (!query.exec())
        return false;

    if (!query.next())
        return true;  // unknown infohash

    if (query.value(0).toInt() != 0)
        return false;  // already have metadata

    const int attempts = query.value(1).toInt();
    if (attempts >= MAX_FETCH_ATTEMPTS)
        return false;

    const qint64 lastAttempt = query.value(2).toLongLong();
    if ((attempts > 0) && ((nowMs() - lastAttempt) < FETCH_BACKOFF_MS))
        return false;  // in backoff

    return true;
}

HarvestSearchPage HarvestStore::search(const QString &queryText, int limit, int offset
        , HarvestSortColumn sortColumn, bool descending) const
{
    HarvestSearchPage page;

    const QString match = toFtsMatch(normalizeText(queryText));
    if (match.isEmpty())
        return page;

    auto db = QSqlDatabase::database(m_connectionName);

    // Full match count across all pages (for the paginated response total).
    QSqlQuery countQuery {db};
    countQuery.prepare(u"SELECT count(*) FROM torrent_name_fts WHERE torrent_name_fts MATCH :match;"_s);
    countQuery.bindValue(u":match"_s, match);
    if (countQuery.exec() && countQuery.next())
        page.total = countQuery.value(0).toLongLong();

    QSqlQuery query {db};
    query.prepare(u"SELECT "_s + SELECT_RESULT_COLS
                  + u" FROM torrent_name_fts f"
                  u" JOIN torrents t ON t.id = f.rowid"
                  u" WHERE torrent_name_fts MATCH :match"
                  u" ORDER BY "_s + orderByClause(sortColumn, descending, true)
                  + u" LIMIT :limit OFFSET :offset;"_s);
    query.bindValue(u":match"_s, match);
    query.bindValue(u":limit"_s, limit);
    query.bindValue(u":offset"_s, offset);
    if (!query.exec())
    {
        LogMsg(QObject::tr("DHT harvest: search failed. %1").arg(query.lastError().text()), Log::WARNING);
        return page;
    }

    while (query.next())
        page.items.append(readResultRow(query));

    return page;
}

HarvestSearchPage HarvestStore::recent(int limit, int offset
        , HarvestSortColumn sortColumn, bool descending) const
{
    HarvestSearchPage page;

    auto db = QSqlDatabase::database(m_connectionName);

    // Full count of metadata-complete torrents (for the paginated response total).
    QSqlQuery countQuery {db};
    countQuery.prepare(u"SELECT count(*) FROM torrents WHERE metadata_fetched = 1;"_s);
    if (countQuery.exec() && countQuery.next())
        page.total = countQuery.value(0).toLongLong();

    QSqlQuery query {db};
    query.prepare(u"SELECT "_s + SELECT_RESULT_COLS
                  + u" FROM torrents t"
                  u" WHERE t.metadata_fetched = 1"
                  u" ORDER BY "_s + orderByClause(sortColumn, descending, false)
                  + u" LIMIT :limit OFFSET :offset;"_s);
    query.bindValue(u":limit"_s, limit);
    query.bindValue(u":offset"_s, offset);
    if (!query.exec())
        return page;

    while (query.next())
        page.items.append(readResultRow(query));

    return page;
}

QList<HarvestTypeCount> HarvestStore::typeCounts(const QString &queryText) const
{
    QList<HarvestTypeCount> out;
    auto db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query {db};

    const QString match = toFtsMatch(normalizeText(queryText));
    if (match.isEmpty())
    {
        // No query: bucket every metadata-complete torrent by content type.
        if (!query.exec(u"SELECT content_type, count(*) FROM torrents"
                        u" WHERE metadata_fetched = 1 GROUP BY content_type"
                        u" ORDER BY count(*) DESC;"_s))
            return out;
    }
    else
    {
        query.prepare(u"SELECT t.content_type, count(*)"
                      u" FROM torrent_name_fts f JOIN torrents t ON t.id = f.rowid"
                      u" WHERE torrent_name_fts MATCH :match"
                      u" GROUP BY t.content_type ORDER BY count(*) DESC;"_s);
        query.bindValue(u":match"_s, match);
        if (!query.exec())
            return out;
    }

    while (query.next())
    {
        HarvestTypeCount bucket;
        bucket.contentType = query.value(0).toString();
        bucket.count = query.value(1).toLongLong();
        out.append(bucket);
    }
    return out;
}

HarvestSearchPage HarvestStore::searchByType(const QString &queryText, const QString &contentType, int limit, int offset
        , HarvestSortColumn sortColumn, bool descending) const
{
    HarvestSearchPage page;

    const QString match = toFtsMatch(normalizeText(queryText));
    if (match.isEmpty())
        return page;

    auto db = QSqlDatabase::database(m_connectionName);

    QSqlQuery countQuery {db};
    countQuery.prepare(u"SELECT count(*) FROM torrent_name_fts f JOIN torrents t ON t.id = f.rowid"
                       u" WHERE torrent_name_fts MATCH :match AND t.content_type = :type;"_s);
    countQuery.bindValue(u":match"_s, match);
    countQuery.bindValue(u":type"_s, contentType);
    if (countQuery.exec() && countQuery.next())
        page.total = countQuery.value(0).toLongLong();

    QSqlQuery query {db};
    query.prepare(u"SELECT "_s + SELECT_RESULT_COLS
                  + u" FROM torrent_name_fts f"
                  u" JOIN torrents t ON t.id = f.rowid"
                  u" WHERE torrent_name_fts MATCH :match AND t.content_type = :type"
                  u" ORDER BY "_s + orderByClause(sortColumn, descending, true)
                  + u" LIMIT :limit OFFSET :offset;"_s);
    query.bindValue(u":match"_s, match);
    query.bindValue(u":type"_s, contentType);
    query.bindValue(u":limit"_s, limit);
    query.bindValue(u":offset"_s, offset);
    if (!query.exec())
    {
        LogMsg(QObject::tr("DHT harvest: search failed. %1").arg(query.lastError().text()), Log::WARNING);
        return page;
    }

    while (query.next())
        page.items.append(readResultRow(query));

    return page;
}

HarvestSearchPage HarvestStore::recentByType(const QString &contentType, int limit, int offset
        , HarvestSortColumn sortColumn, bool descending) const
{
    HarvestSearchPage page;

    auto db = QSqlDatabase::database(m_connectionName);

    QSqlQuery countQuery {db};
    countQuery.prepare(u"SELECT count(*) FROM torrents WHERE metadata_fetched = 1 AND content_type = :type;"_s);
    countQuery.bindValue(u":type"_s, contentType);
    if (countQuery.exec() && countQuery.next())
        page.total = countQuery.value(0).toLongLong();

    QSqlQuery query {db};
    query.prepare(u"SELECT "_s + SELECT_RESULT_COLS
                  + u" FROM torrents t"
                  u" WHERE t.metadata_fetched = 1 AND t.content_type = :type"
                  u" ORDER BY "_s + orderByClause(sortColumn, descending, false)
                  + u" LIMIT :limit OFFSET :offset;"_s);
    query.bindValue(u":type"_s, contentType);
    query.bindValue(u":limit"_s, limit);
    query.bindValue(u":offset"_s, offset);
    if (!query.exec())
        return page;

    while (query.next())
        page.items.append(readResultRow(query));

    return page;
}

QByteArray HarvestStore::metadataFor(const QString &infoHashV1) const
{
    if (infoHashV1.isEmpty())
        return {};

    auto db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query {db};
    query.prepare(u"SELECT metadata FROM torrents WHERE infohash_v1 = :ih AND metadata_fetched = 1;"_s);
    query.bindValue(u":ih"_s, infoHashV1);
    if (!query.exec() || !query.next())
        return {};

    return query.value(0).toByteArray();
}

HarvestStats HarvestStore::stats() const
{
    HarvestStats result;
    auto db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query {db};

    if (query.exec(u"SELECT count(*) FROM torrents;"_s) && query.next())
        result.torrentCount = query.value(0).toLongLong();
    if (query.exec(u"SELECT count(*) FROM torrents WHERE metadata_fetched = 1;"_s) && query.next())
        result.metadataCount = query.value(0).toLongLong();
    if (query.exec(u"SELECT count(*) FROM infohash_sightings;"_s) && query.next())
        result.sightingCount = query.value(0).toLongLong();

    return result;
}

QList<QString> HarvestStore::candidatesForMetadata(int limit) const
{
    QList<QString> out;
    if (limit <= 0)
        return out;

    auto db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query {db};
    // Order by the strongest fetchability signal first: infohashes a peer was seen
    // *holding* (announce_count) before those merely *asked about* (availability via
    // get_peers/sample), then by recency. This keeps the scarce metadata-fetch slots
    // on torrents that actually have a reachable peer.
    query.prepare(u"SELECT infohash_v1 FROM torrents"
                  u" WHERE metadata_fetched = 0 AND fetch_attempts < :maxAttempts"
                  u"   AND (last_attempt_ms = 0 OR (:now - last_attempt_ms) > :backoff)"
                  u" ORDER BY announce_count DESC, availability_score DESC, last_seen_ms DESC"
                  u" LIMIT :limit;"_s);
    query.bindValue(u":maxAttempts"_s, MAX_FETCH_ATTEMPTS);
    query.bindValue(u":now"_s, nowMs());
    query.bindValue(u":backoff"_s, FETCH_BACKOFF_MS);
    query.bindValue(u":limit"_s, limit);
    if (!query.exec())
        return out;

    while (query.next())
        out.append(query.value(0).toString());
    return out;
}

HarvestPruneStats HarvestStore::prune(const qint64 staleAgeMs, const qint64 sightingRetentionMs, const qint64 maxTorrents)
{
    HarvestPruneStats result;
    const qint64 now = nowMs();
    auto db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query {db};

    // 1. drop metadata-less torrents that exhausted their fetch attempts and have
    // not been seen recently (FTS rows + file rows cascade via trigger/FK).
    query.prepare(u"DELETE FROM torrents WHERE metadata_fetched = 0"
                  u" AND fetch_attempts >= :maxAttempts AND last_seen_ms < :cutoff;"_s);
    query.bindValue(u":maxAttempts"_s, MAX_FETCH_ATTEMPTS);
    query.bindValue(u":cutoff"_s, now - staleAgeMs);
    if (query.exec())
        result.torrentsPruned += query.numRowsAffected();

    // 2. prune old sightings (the fastest-growing table).
    query.prepare(u"DELETE FROM infohash_sightings WHERE observed_at_ms < :cutoff;"_s);
    query.bindValue(u":cutoff"_s, now - sightingRetentionMs);
    if (query.exec())
        result.sightingsPruned += query.numRowsAffected();

    // 3. hard cap: if still over the row budget, drop the oldest metadata-less
    // torrents (never the ones we actually have metadata for).
    if (maxTorrents > 0)
    {
        qint64 total = 0;
        if (query.exec(u"SELECT count(*) FROM torrents;"_s) && query.next())
            total = query.value(0).toLongLong();
        if (total > maxTorrents)
        {
            query.prepare(u"DELETE FROM torrents WHERE id IN ("
                          u"SELECT id FROM torrents WHERE metadata_fetched = 0"
                          u" ORDER BY last_seen_ms ASC LIMIT :excess);"_s);
            query.bindValue(u":excess"_s, (total - maxTorrents));
            if (query.exec())
                result.torrentsPruned += query.numRowsAffected();
        }
    }

    return result;
}
