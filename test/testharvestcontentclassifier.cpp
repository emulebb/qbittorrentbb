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

#include <QList>
#include <QObject>
#include <QTest>

#include "base/bittorrent/harvestcontentclassifier.h"
#include "base/global.h"

class TestHarvestContentClassifier final : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(TestHarvestContentClassifier)

public:
    TestHarvestContentClassifier() = default;

private slots:
    void testDominantCategoryByBytes() const
    {
        const QList<BitTorrent::HarvestedFile> files {
            {u"Samples/Alpha.mkv"_s, 900},
            {u"Samples/Readme.txt"_s, 20}
        };

        QCOMPARE(BitTorrent::classifyHarvestedContent(files, 1000), u"video"_s);
    }

    void testMixedWhenNoCategoryDominates() const
    {
        const QList<BitTorrent::HarvestedFile> files {
            {u"Samples/Alpha.mp4"_s, 500},
            {u"Samples/Beta.flac"_s, 500}
        };

        QCOMPARE(BitTorrent::classifyHarvestedContent(files, 1000), u"mixed"_s);
    }

    void testArchiveAndSplitRarParts() const
    {
        const QList<BitTorrent::HarvestedFile> files {
            {u"Bundle/Payload.r00"_s, 100},
            {u"Bundle/Payload.r01"_s, 100},
            {u"Bundle/Payload.rar"_s, 100}
        };

        QCOMPARE(BitTorrent::classifyHarvestedContent(files, 300), u"archive"_s);
    }

    void testKnownSingleCategories() const
    {
        QCOMPARE(BitTorrent::classifyHarvestedContent({{u"Disc/Image.iso"_s, 1}}, 1), u"disk-image"_s);
        QCOMPARE(BitTorrent::classifyHarvestedContent({{u"Docs/Manual.pdf"_s, 1}}, 1), u"document"_s);
        QCOMPARE(BitTorrent::classifyHarvestedContent({{u"Pictures/Frame.png"_s, 1}}, 1), u"image"_s);
        QCOMPARE(BitTorrent::classifyHarvestedContent({{u"Setup/Installer.exe"_s, 1}}, 1), u"software"_s);
        QCOMPARE(BitTorrent::classifyHarvestedContent({{u"Captions/Track.srt"_s, 1}}, 1), u"subtitle"_s);
    }

    void testFallsBackToCountsWhenSizesAreUnknown() const
    {
        const QList<BitTorrent::HarvestedFile> files {
            {u"Zero/One.mp3"_s, 0},
            {u"Zero/Two.flac"_s, 0},
            {u"Zero/Three.txt"_s, 0}
        };

        QCOMPARE(BitTorrent::classifyHarvestedContent(files, 0), u"audio"_s);
    }

    void testUnknownContent() const
    {
        QCOMPARE(BitTorrent::classifyHarvestedContent({}, 0), u"other"_s);
        QCOMPARE(BitTorrent::classifyHarvestedContent({{u"Unknown/blob.data"_s, 10}}, 10), u"other"_s);
    }
};

QTEST_APPLESS_MAIN(TestHarvestContentClassifier)
#include "testharvestcontentclassifier.moc"
