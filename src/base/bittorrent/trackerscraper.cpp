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

#include "trackerscraper.h"

#include <QDataStream>
#include <QDateTime>
#include <QHostInfo>
#include <QRandomGenerator>
#include <QTimer>
#include <QUdpSocket>
#include <QUrl>

using namespace Qt::Literals::StringLiterals;

namespace
{
    const quint64 CONNECT_MAGIC = 0x41727101980ULL;  // BEP-15 protocol id
    const quint32 ACTION_CONNECT = 0;
    const quint32 ACTION_SCRAPE = 2;
    const quint32 ACTION_ERROR = 3;

    const int MAX_HASHES_PER_SCRAPE = 70;  // BEP-15 caps a scrape datagram at ~74 hashes
    const int INFOHASH_HEX_LEN = 40;
    const qint64 JOB_TIMEOUT_MS = 15000;   // drop a batch that never gets a reply
    const qint64 RESOLVE_TTL_MS = 10 * 60 * 1000;  // re-resolve the tracker host periodically
    const int SWEEP_INTERVAL_MS = 5000;

    qint64 nowMs()
    {
        return QDateTime::currentMSecsSinceEpoch();
    }
}

using namespace BitTorrent;

TrackerScraper::TrackerScraper(QObject *parent)
    : QObject(parent)
{
    m_sweepTimer = new QTimer(this);
    m_sweepTimer->setInterval(SWEEP_INTERVAL_MS);
    connect(m_sweepTimer, &QTimer::timeout, this, &TrackerScraper::onSweep);
    m_sweepTimer->start();
}

TrackerScraper::~TrackerScraper() = default;

void TrackerScraper::setTracker(const QString &trackerUrl)
{
    const QUrl url {trackerUrl.trimmed()};
    if (!url.isValid() || (url.scheme().compare(u"udp"_s, Qt::CaseInsensitive) != 0)
            || url.host().isEmpty() || (url.port() <= 0))
    {
        m_trackerHost.clear();
        m_trackerPort = 0;
        m_trackerAddress.clear();
        return;
    }

    const QString host = url.host();
    const auto port = static_cast<quint16>(url.port());
    if ((host == m_trackerHost) && (port == m_trackerPort))
        return;

    m_trackerHost = host;
    m_trackerPort = port;
    m_trackerAddress.clear();  // force a fresh resolution for the new host
    m_resolvedAtMs = 0;
}

void TrackerScraper::setBindAddress(const QString &address)
{
    QHostAddress addr;
    if (!address.isEmpty() && (address != u"0.0.0.0"_s) && (address != u"::"_s))
        addr = QHostAddress(address);

    if (addr == m_bindAddress)
        return;

    m_bindAddress = addr;
    // Rebind on next use so the scrape egresses the (new) tunnel address.
    if (m_socket)
    {
        m_socket->deleteLater();
        m_socket = nullptr;
    }
}

void TrackerScraper::ensureSocket()
{
    if (m_socket)
        return;

    m_socket = new QUdpSocket(this);
    const QHostAddress bindAddr = m_bindAddress.isNull() ? QHostAddress(QHostAddress::AnyIPv4) : m_bindAddress;
    m_socket->bind(bindAddr, 0);
    connect(m_socket, &QUdpSocket::readyRead, this, &TrackerScraper::onReadyRead);
}

void TrackerScraper::scrape(const QStringList &infoHashesV1)
{
    if (m_trackerHost.isEmpty())
        return;

    for (const QString &ih : infoHashesV1)
    {
        const QString hex = ih.trimmed().toLower();
        if ((hex.size() == INFOHASH_HEX_LEN) && !m_pending.contains(hex))
            m_pending.append(hex);
    }
    if (m_pending.isEmpty())
        return;

    // Re-resolve periodically (tracker IPs rotate); null address triggers a lookup.
    if (!m_trackerAddress.isNull() && ((nowMs() - m_resolvedAtMs) > RESOLVE_TTL_MS))
        m_trackerAddress.clear();

    if (m_trackerAddress.isNull())
    {
        resolveTracker();
        return;
    }
    flushPending();
}

void TrackerScraper::resolveTracker()
{
    if (m_resolving || m_trackerHost.isEmpty())
        return;

    m_resolving = true;
    QHostInfo::lookupHost(m_trackerHost, this, [this](const QHostInfo &info)
    {
        m_resolving = false;
        for (const QHostAddress &addr : info.addresses())
        {
            if (addr.protocol() == QAbstractSocket::IPv4Protocol)
            {
                m_trackerAddress = addr;
                break;
            }
        }
        if (m_trackerAddress.isNull() && !info.addresses().isEmpty())
            m_trackerAddress = info.addresses().constFirst();

        if (m_trackerAddress.isNull())
        {
            m_pending.clear();  // can't resolve; drop this round, try again next call
            return;
        }
        m_resolvedAtMs = nowMs();
        flushPending();
    });
}

void TrackerScraper::flushPending()
{
    if (m_pending.isEmpty() || m_trackerAddress.isNull())
        return;

    ensureSocket();

    while (!m_pending.isEmpty())
    {
        auto job = std::make_shared<Job>();
        job->infoHashes = m_pending.mid(0, MAX_HASHES_PER_SCRAPE);
        m_pending = m_pending.mid(job->infoHashes.size());
        job->startedMs = nowMs();
        sendConnect(job);
    }
}

void TrackerScraper::sendConnect(const std::shared_ptr<Job> &job)
{
    // Assign a fresh, unused transaction id and register the job under it.
    quint32 txid = 0;
    do { txid = QRandomGenerator::global()->generate(); } while ((txid == 0) || m_jobs.contains(txid));
    job->transactionId = txid;
    job->connected = false;
    m_jobs.insert(txid, job);

    QByteArray packet;
    QDataStream out {&packet, QIODevice::WriteOnly};  // QDataStream is big-endian by default
    out << CONNECT_MAGIC << ACTION_CONNECT << txid;
    m_socket->writeDatagram(packet, m_trackerAddress, m_trackerPort);
}

void TrackerScraper::sendScrape(const std::shared_ptr<Job> &job)
{
    QByteArray packet;
    QDataStream out {&packet, QIODevice::WriteOnly};
    out << static_cast<quint64>(job->connectionId) << ACTION_SCRAPE << job->transactionId;
    for (const QString &hex : job->infoHashes)
    {
        const QByteArray raw = QByteArray::fromHex(hex.toLatin1());
        out.writeRawData(raw.constData(), static_cast<int>(raw.size()));
    }
    m_socket->writeDatagram(packet, m_trackerAddress, m_trackerPort);
}

void TrackerScraper::onReadyRead()
{
    while (m_socket && m_socket->hasPendingDatagrams())
    {
        QByteArray data;
        data.resize(static_cast<int>(m_socket->pendingDatagramSize()));
        m_socket->readDatagram(data.data(), data.size());
        handleDatagram(data);
    }
}

void TrackerScraper::handleDatagram(const QByteArray &data)
{
    if (data.size() < 8)
        return;

    QDataStream in {data};
    quint32 action = 0;
    quint32 txid = 0;
    in >> action >> txid;

    const auto it = m_jobs.find(txid);
    if (it == m_jobs.end())
        return;
    const std::shared_ptr<Job> job = it.value();

    if ((action == ACTION_CONNECT) && !job->connected)
    {
        if (data.size() < 16)
            return;
        quint64 connectionId = 0;
        in >> connectionId;
        job->connectionId = static_cast<qint64>(connectionId);
        job->connected = true;

        // Re-key the job under a new transaction id for the scrape phase.
        m_jobs.erase(it);
        quint32 scrapeTx = 0;
        do { scrapeTx = QRandomGenerator::global()->generate(); } while ((scrapeTx == 0) || m_jobs.contains(scrapeTx));
        job->transactionId = scrapeTx;
        m_jobs.insert(scrapeTx, job);
        sendScrape(job);
        return;
    }

    if ((action == ACTION_SCRAPE) && job->connected)
    {
        for (const QString &hex : job->infoHashes)
        {
            quint32 seeders = 0;
            quint32 completed = 0;
            quint32 leechers = 0;
            in >> seeders >> completed >> leechers;
            if (in.status() != QDataStream::Ok)
                break;
            emit scraped(hex, static_cast<int>(seeders), static_cast<int>(leechers));
        }
        m_jobs.erase(it);
        return;
    }

    if (action == ACTION_ERROR)
        m_jobs.erase(it);
}

void TrackerScraper::onSweep()
{
    const qint64 now = nowMs();
    for (auto it = m_jobs.begin(); it != m_jobs.end();)
    {
        if ((now - it.value()->startedMs) > JOB_TIMEOUT_MS)
            it = m_jobs.erase(it);
        else
            ++it;
    }
}
