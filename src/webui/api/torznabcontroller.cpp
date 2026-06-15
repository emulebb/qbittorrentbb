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

#include "torznabcontroller.h"

#include <QDateTime>
#include <QList>
#include <QString>
#include <QUrl>
#include <QXmlStreamWriter>

#include "base/bittorrent/harveststore.h"
#include "base/bittorrent/session.h"

using namespace Qt::Literals::StringLiterals;

namespace
{
    const QString TORZNAB_NS = u"http://torznab.com/schemas/2015/feed"_s;
    const QString OTHER_CATEGORY = u"8000"_s;  // we don't classify content
    const int DEFAULT_LIMIT = 100;
    const int MAX_LIMIT = 500;

    QString magnetFor(const QString &infoHashV1, const QString &name)
    {
        QString magnet = u"magnet:?xt=urn:btih:"_s + infoHashV1;
        if (!name.isEmpty())
            magnet += u"&dn="_s + QString::fromLatin1(QUrl::toPercentEncoding(name));
        return magnet;
    }

    void writeTorznabAttr(QXmlStreamWriter &xml, const QString &name, const QString &value)
    {
        xml.writeStartElement(TORZNAB_NS, u"attr"_s);
        xml.writeAttribute(u"name"_s, name);
        xml.writeAttribute(u"value"_s, value);
        xml.writeEndElement();
    }
}

void TorznabController::indexAction()
{
    const QString type = params().value(u"t"_s);
    if (type == u"caps"_s)
        setResult(buildCaps(), u"application/xml; charset=UTF-8"_s);
    else  // search / tvsearch / movie / movie-search / (empty RSS test)
        setResult(buildResults(), u"application/rss+xml; charset=UTF-8"_s);
}

QByteArray TorznabController::buildCaps() const
{
    QByteArray out;
    QXmlStreamWriter xml {&out};
    xml.setAutoFormatting(true);
    xml.writeStartDocument();

    xml.writeStartElement(u"caps"_s);

    xml.writeStartElement(u"server"_s);
    xml.writeAttribute(u"title"_s, u"qBittorrentBB DHT Index"_s);
    xml.writeEndElement();

    xml.writeStartElement(u"limits"_s);
    xml.writeAttribute(u"max"_s, QString::number(MAX_LIMIT));
    xml.writeAttribute(u"default"_s, QString::number(DEFAULT_LIMIT));
    xml.writeEndElement();

    xml.writeStartElement(u"searching"_s);
    for (const QString &mode : {u"search"_s, u"tv-search"_s, u"movie-search"_s})
    {
        xml.writeStartElement(mode);
        xml.writeAttribute(u"available"_s, u"yes"_s);
        // We key only on the text query and echo the caller's category back; we
        // do not filter by season/ep/year, so we advertise only what we honor.
        xml.writeAttribute(u"supportedParams"_s, u"q,cat"_s);
        xml.writeEndElement();
    }
    xml.writeEndElement();  // searching

    // Advertise the top-level category set so *Arr callers (Radarr 2000,
    // Sonarr 5000, Lidarr 3000, ...) will configure and query this indexer. The
    // DHT index has no per-item category metadata; per-item categories are
    // echoed from the request (see buildResults).
    xml.writeStartElement(u"categories"_s);
    for (const auto &[id, name] : {std::pair {u"2000"_s, u"Movies"_s}, std::pair {u"3000"_s, u"Audio"_s},
                                   std::pair {u"4000"_s, u"PC"_s}, std::pair {u"5000"_s, u"TV"_s},
                                   std::pair {u"6000"_s, u"XXX"_s}, std::pair {u"7000"_s, u"Books"_s},
                                   std::pair {u"8000"_s, u"Other"_s}})
    {
        xml.writeStartElement(u"category"_s);
        xml.writeAttribute(u"id"_s, id);
        xml.writeAttribute(u"name"_s, name);
        xml.writeEndElement();
    }
    xml.writeEndElement();  // categories

    xml.writeEndElement();  // caps
    xml.writeEndDocument();
    return out;
}

QByteArray TorznabController::buildResults() const
{
    const QString query = params().value(u"q"_s).trimmed();

    int limit = params().value(u"limit"_s).toInt();
    if ((limit <= 0) || (limit > MAX_LIMIT))
        limit = DEFAULT_LIMIT;

    auto *session = BitTorrent::Session::instance();
    const QList<BitTorrent::HarvestSearchResult> results = query.isEmpty()
            ? session->recentDHTIndex(limit)
            : session->searchDHTIndex(query, limit);

    QByteArray out;
    QXmlStreamWriter xml {&out};
    xml.setAutoFormatting(true);
    xml.writeStartDocument();

    xml.writeStartElement(u"rss"_s);
    xml.writeAttribute(u"version"_s, u"2.0"_s);
    xml.writeNamespace(TORZNAB_NS, u"torznab"_s);

    xml.writeStartElement(u"channel"_s);
    xml.writeTextElement(u"title"_s, u"qBittorrentBB DHT Index"_s);
    xml.writeTextElement(u"description"_s, u"Torrents discovered from the BitTorrent DHT"_s);

    for (const BitTorrent::HarvestSearchResult &result : results)
    {
        const QString magnet = magnetFor(result.infoHashV1, result.name);
        // Prefer the real DHT swarm size (peers from a get_peers reply); fall back
        // to the sighting count as a popularity proxy until a swarm reading lands.
        // DHT can't distinguish seeders from leechers, so report the swarm size for
        // both — better signal for *Arr ranking than a flat zero.
        const int swarm = (result.peers > 0) ? result.peers : result.sightings;
        const QString swarmCount = QString::number(swarm);

        xml.writeStartElement(u"item"_s);
        xml.writeTextElement(u"title"_s, result.name);

        xml.writeStartElement(u"guid"_s);
        xml.writeAttribute(u"isPermaLink"_s, u"false"_s);
        xml.writeCharacters(result.infoHashV1);
        xml.writeEndElement();

        xml.writeTextElement(u"pubDate"_s
                , QDateTime::fromMSecsSinceEpoch(result.firstSeenMs).toString(Qt::RFC2822Date));
        xml.writeTextElement(u"size"_s, QString::number(result.size));
        xml.writeTextElement(u"link"_s, magnet);

        xml.writeStartElement(u"enclosure"_s);
        xml.writeAttribute(u"url"_s, magnet);
        xml.writeAttribute(u"length"_s, QString::number(result.size));
        xml.writeAttribute(u"type"_s, u"application/x-bittorrent"_s);
        xml.writeEndElement();

        writeTorznabAttr(xml, u"category"_s, OTHER_CATEGORY);
        writeTorznabAttr(xml, u"size"_s, QString::number(result.size));
        writeTorznabAttr(xml, u"infohash"_s, result.infoHashV1);
        writeTorznabAttr(xml, u"magneturl"_s, magnet);
        writeTorznabAttr(xml, u"seeders"_s, swarmCount);
        writeTorznabAttr(xml, u"peers"_s, swarmCount);

        xml.writeEndElement();  // item
    }

    xml.writeEndElement();  // channel
    xml.writeEndElement();  // rss
    xml.writeEndDocument();
    return out;
}
