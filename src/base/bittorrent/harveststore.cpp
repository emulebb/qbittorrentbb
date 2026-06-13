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
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>

#include "base/exceptions.h"
#include "base/global.h"
#include "base/logger.h"

using namespace Qt::Literals::StringLiterals;

namespace
{
    const int DB_VERSION = 1;

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
    bool versionMatches = false;
    if (hasMeta)
    {
        if (query.exec(u"SELECT value FROM harvest_meta WHERE name = 'version';"_s) && query.next())
            versionMatches = (query.value(0).toInt() == DB_VERSION);
    }

    if (hasMeta && versionMatches)
        return;

    // Fresh DB or version mismatch: reset our tables and (re)create.
    if (hasMeta)
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
             u"size_bytes INTEGER NOT NULL DEFAULT 0,"
             u"file_count INTEGER NOT NULL DEFAULT 0,"
             u"piece_length INTEGER NOT NULL DEFAULT 0,"
             u"metadata_fetched INTEGER NOT NULL DEFAULT 0,"
             u"fetch_attempts INTEGER NOT NULL DEFAULT 0,"
             u"last_attempt_ms INTEGER NOT NULL DEFAULT 0,"
             u"availability_score INTEGER NOT NULL DEFAULT 0,"
             u"first_seen_ms INTEGER NOT NULL,"
             u"last_seen_ms INTEGER NOT NULL,"
             u"updated_at_ms INTEGER NOT NULL);"_s);

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
    if (sighting.infoHashV1.isEmpty())
        return;

    const qint64 now = nowMs();
    auto db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query {db};

    query.prepare(u"INSERT INTO torrents (infohash_v1, first_seen_ms, last_seen_ms, updated_at_ms, availability_score)"
                  u" VALUES (:ih, :now, :now, :now, 1)"
                  u" ON CONFLICT(infohash_v1) DO UPDATE SET"
                  u" last_seen_ms = :now, updated_at_ms = :now,"
                  u" availability_score = availability_score + 1;"_s);
    query.bindValue(u":ih"_s, sighting.infoHashV1);
    query.bindValue(u":now"_s, now);
    if (!query.exec())
    {
        LogMsg(QObject::tr("DHT harvest: failed to record sighting. %1").arg(query.lastError().text()), Log::WARNING);
        return;
    }

    query.prepare(u"INSERT INTO infohash_sightings (infohash_v1, source, peer_ip, peer_port, observed_at_ms)"
                  u" VALUES (:ih, :source, :ip, :port, :now);"_s);
    query.bindValue(u":ih"_s, sighting.infoHashV1);
    query.bindValue(u":source"_s, sighting.source);
    // Bind a non-null empty string for passive sightings (get_peers/sample carry
    // no peer IP); a null QString would bind SQL NULL and violate NOT NULL.
    query.bindValue(u":ip"_s, sighting.peerIP.isNull() ? QString::fromLatin1("") : sighting.peerIP);
    query.bindValue(u":port"_s, sighting.peerPort);
    query.bindValue(u":now"_s, now);
    if (!query.exec())
        LogMsg(QObject::tr("DHT harvest: failed to record sighting. %1").arg(query.lastError().text()), Log::WARNING);
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
        query.prepare(u"INSERT INTO torrents"
                      u" (infohash_v1, infohash_v2, name, normalized_name, size_bytes, file_count,"
                      u"  piece_length, metadata_fetched, first_seen_ms, last_seen_ms, updated_at_ms)"
                      u" VALUES (:ih, :ih2, :name, :norm, :size, :files, :piece, 1, :now, :now, :now)"
                      u" ON CONFLICT(infohash_v1) DO UPDATE SET"
                      u" infohash_v2 = :ih2, name = :name, normalized_name = :norm,"
                      u" size_bytes = :size, file_count = :files, piece_length = :piece,"
                      u" metadata_fetched = 1, last_seen_ms = :now, updated_at_ms = :now;"_s);
        query.bindValue(u":ih"_s, torrent.infoHashV1);
        query.bindValue(u":ih2"_s, torrent.infoHashV2.isEmpty() ? QVariant() : QVariant(torrent.infoHashV2));
        query.bindValue(u":name"_s, torrent.name);
        query.bindValue(u":norm"_s, normalized);
        query.bindValue(u":size"_s, torrent.size);
        query.bindValue(u":files"_s, torrent.fileCount);
        query.bindValue(u":piece"_s, torrent.pieceLength);
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

QList<HarvestSearchResult> HarvestStore::search(const QString &queryText, int limit) const
{
    QList<HarvestSearchResult> results;

    const QString match = toFtsMatch(normalizeText(queryText));
    if (match.isEmpty())
        return results;

    auto db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query {db};
    query.prepare(u"SELECT t.infohash_v1, t.name, t.size_bytes, t.file_count,"
                  u" t.first_seen_ms, t.last_seen_ms, t.metadata_fetched,"
                  u" (SELECT count(*) FROM infohash_sightings s WHERE s.infohash_v1 = t.infohash_v1)"
                  u" FROM torrent_name_fts f"
                  u" JOIN torrents t ON t.id = f.rowid"
                  u" WHERE torrent_name_fts MATCH :match"
                  u" ORDER BY bm25(torrent_name_fts), t.availability_score DESC, t.last_seen_ms DESC"
                  u" LIMIT :limit;"_s);
    query.bindValue(u":match"_s, match);
    query.bindValue(u":limit"_s, limit);
    if (!query.exec())
    {
        LogMsg(QObject::tr("DHT harvest: search failed. %1").arg(query.lastError().text()), Log::WARNING);
        return results;
    }

    while (query.next())
    {
        HarvestSearchResult row;
        row.infoHashV1 = query.value(0).toString();
        row.name = query.value(1).toString();
        row.size = query.value(2).toLongLong();
        row.fileCount = query.value(3).toInt();
        row.firstSeenMs = query.value(4).toLongLong();
        row.lastSeenMs = query.value(5).toLongLong();
        row.metadataFetched = (query.value(6).toInt() != 0);
        row.sightings = query.value(7).toInt();
        results.append(row);
    }

    return results;
}

QList<HarvestSearchResult> HarvestStore::recent(int limit) const
{
    QList<HarvestSearchResult> results;

    auto db = QSqlDatabase::database(m_connectionName);
    QSqlQuery query {db};
    query.prepare(u"SELECT t.infohash_v1, t.name, t.size_bytes, t.file_count,"
                  u" t.first_seen_ms, t.last_seen_ms, t.metadata_fetched,"
                  u" (SELECT count(*) FROM infohash_sightings s WHERE s.infohash_v1 = t.infohash_v1)"
                  u" FROM torrents t"
                  u" WHERE t.metadata_fetched = 1"
                  u" ORDER BY t.updated_at_ms DESC"
                  u" LIMIT :limit;"_s);
    query.bindValue(u":limit"_s, limit);
    if (!query.exec())
        return results;

    while (query.next())
    {
        HarvestSearchResult row;
        row.infoHashV1 = query.value(0).toString();
        row.name = query.value(1).toString();
        row.size = query.value(2).toLongLong();
        row.fileCount = query.value(3).toInt();
        row.firstSeenMs = query.value(4).toLongLong();
        row.lastSeenMs = query.value(5).toLongLong();
        row.metadataFetched = (query.value(6).toInt() != 0);
        row.sightings = query.value(7).toInt();
        results.append(row);
    }

    return results;
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
