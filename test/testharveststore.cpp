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
 */

#include <QObject>
#include <QTemporaryDir>
#include <QTest>

#include "base/bittorrent/harveststore.h"
#include "base/global.h"
#include "base/path.h"

class TestHarvestStore final : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(TestHarvestStore)

public:
    TestHarvestStore() = default;

private slots:
    void testCandidateLeasesPreventHeadOfQueueStarvation() const
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        BitTorrent::HarvestStore store {Path(tempDir.filePath(u"harvest.db"_s))};
        store.init();

        const QString announcedHash = u"1111111111111111111111111111111111111111"_s;
        const QString popularHash = u"2222222222222222222222222222222222222222"_s;
        const QString remainingHash = u"3333333333333333333333333333333333333333"_s;

        store.recordSightings({
            {announcedHash, u"announce"_s, u"192.0.2.1"_s, 6881},
            {popularHash, u"sample"_s, {}, 0},
            {popularHash, u"get_peers"_s, {}, 0},
            {remainingHash, u"sample"_s, {}, 0}
        });

        const QList<QString> firstBatch = store.candidatesForMetadata(2);
        QCOMPARE(firstBatch, QList<QString>({announcedHash, popularHash}));
        QVERIFY(!store.needsMetadata(announcedHash));
        QVERIFY(!store.needsMetadata(popularHash));

        // The first two rows are under a persistent lease, so the next scheduler
        // tick advances instead of selecting the same silent hashes again.
        QCOMPARE(store.candidatesForMetadata(2), QList<QString>({remainingHash}));
    }
};

QTEST_GUILESS_MAIN(TestHarvestStore)
#include "testharveststore.moc"
