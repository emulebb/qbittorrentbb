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

#include <memory>

#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class QTimer;
class QUdpSocket;

namespace BitTorrent
{
    // Minimal BEP-15 (UDP tracker protocol) scrape client for the DHT index. Given
    // v1 infohashes it asks ONE configured tracker for their seeders/leechers and
    // emits the result per infohash. The scrape socket is bound to the session's
    // outgoing (VPN) interface address so it egresses the tunnel like the rest of the
    // BT data plane. Best-effort by design: one attempt per batch, short timeout, no
    // retry storms -- a miss just leaves the row un-scraped until next time.
    class TrackerScraper final : public QObject
    {
        Q_OBJECT
        Q_DISABLE_COPY_MOVE(TrackerScraper)

    public:
        explicit TrackerScraper(QObject *parent = nullptr);
        ~TrackerScraper() override;

        // "udp://host:port[/path]" (only the udp scheme is honoured). Empty disables.
        void setTracker(const QString &trackerUrl);
        // Local address to bind the scrape socket to (the live tunnel IP). Empty/null
        // or a wildcard binds to Any (no source pin; the split-tunnel rule still routes).
        void setBindAddress(const QString &address);

    public slots:
        // Scrape the given v1 infohashes (40-char lowercase hex). Batched internally.
        void scrape(const QStringList &infoHashesV1);

    signals:
        void scraped(const QString &infoHashV1, int seeds, int leechers);

    private slots:
        void onReadyRead();
        void onSweep();

    private:
        // One in-flight scrape batch (<= MAX_HASHES_PER_SCRAPE infohashes), tracked
        // through the connect -> scrape handshake by its current transaction id.
        struct Job
        {
            QStringList infoHashes;  // hex, in request order (to map reply triples back)
            quint32 transactionId = 0;
            qint64 connectionId = 0;
            bool connected = false;  // false: awaiting connect reply; true: awaiting scrape reply
            qint64 startedMs = 0;
        };

        void ensureSocket();
        void resolveTracker();
        void flushPending();                 // start jobs once the tracker is resolved
        void sendConnect(const std::shared_ptr<Job> &job);
        void sendScrape(const std::shared_ptr<Job> &job);
        void handleDatagram(const QByteArray &data);

        QString m_trackerHost;
        quint16 m_trackerPort = 0;
        QHostAddress m_trackerAddress;       // resolved tracker IP (null until resolved)
        qint64 m_resolvedAtMs = 0;           // for periodic re-resolution
        bool m_resolving = false;

        QHostAddress m_bindAddress;          // tunnel IP (or null/Any)
        QUdpSocket *m_socket = nullptr;
        QTimer *m_sweepTimer = nullptr;

        QStringList m_pending;               // infohashes waiting for a resolved tracker
        QHash<quint32, std::shared_ptr<Job>> m_jobs;  // keyed by current transaction id
    };
}
