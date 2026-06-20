/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2026  qBittorrentBB contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#include "harvestcontentclassifier.h"

#include <algorithm>

#include <QHash>
#include <QRegularExpression>
#include <QSet>

using namespace Qt::Literals::StringLiterals;

namespace
{
    const QString CONTENT_ARCHIVE = u"archive"_s;
    const QString CONTENT_AUDIO = u"audio"_s;
    const QString CONTENT_DISK_IMAGE = u"disk-image"_s;
    const QString CONTENT_DOCUMENT = u"document"_s;
    const QString CONTENT_IMAGE = u"image"_s;
    const QString CONTENT_MIXED = u"mixed"_s;
    const QString CONTENT_OTHER = u"other"_s;
    const QString CONTENT_SOFTWARE = u"software"_s;
    const QString CONTENT_SUBTITLE = u"subtitle"_s;
    const QString CONTENT_VIDEO = u"video"_s;

    QString extensionForPath(const QString &path)
    {
        const qsizetype slash = std::max(path.lastIndexOf(u'/'), path.lastIndexOf(u'\\'));
        const QString fileName = path.sliced(slash + 1).toLower();
        const qsizetype dot = fileName.lastIndexOf(u'.');
        if ((dot <= 0) || (dot == (fileName.size() - 1)))
            return {};
        return fileName.sliced(dot + 1);
    }

    QString categoryForExtension(const QString &ext)
    {
        static const QSet<QString> videos {
            u"avi"_s, u"m2ts"_s, u"m4v"_s, u"mkv"_s, u"mov"_s, u"mp4"_s, u"mpeg"_s, u"mpg"_s,
            u"ts"_s, u"vob"_s, u"webm"_s, u"wmv"_s};
        static const QSet<QString> audio {
            u"aac"_s, u"ac3"_s, u"ape"_s, u"flac"_s, u"m4a"_s, u"mp3"_s, u"ogg"_s, u"opus"_s,
            u"wav"_s, u"wma"_s};
        static const QSet<QString> archives {
            u"7z"_s, u"ace"_s, u"arj"_s, u"bz2"_s, u"cab"_s, u"gz"_s, u"rar"_s, u"tar"_s,
            u"tgz"_s, u"xz"_s, u"zip"_s};
        static const QSet<QString> diskImages {u"bin"_s, u"cue"_s, u"dmg"_s, u"img"_s, u"iso"_s, u"mdf"_s, u"nrg"_s};
        static const QSet<QString> documents {
            u"azw3"_s, u"cb7"_s, u"cbr"_s, u"cbz"_s, u"doc"_s, u"docx"_s, u"epub"_s, u"mobi"_s,
            u"pdf"_s, u"rtf"_s, u"txt"_s, u"xls"_s, u"xlsx"_s};
        static const QSet<QString> images {
            u"bmp"_s, u"gif"_s, u"jpeg"_s, u"jpg"_s, u"png"_s, u"svg"_s, u"tif"_s, u"tiff"_s, u"webp"_s};
        static const QSet<QString> software {
            u"apk"_s, u"appimage"_s, u"bat"_s, u"cmd"_s, u"deb"_s, u"exe"_s, u"msi"_s, u"pkg"_s,
            u"rpm"_s, u"sh"_s};
        static const QSet<QString> subtitles {u"ass"_s, u"srt"_s, u"ssa"_s, u"sub"_s, u"vtt"_s};
        static const QRegularExpression rarPart {uR"(^r\d{2,3}$)"_s};

        if (videos.contains(ext))
            return CONTENT_VIDEO;
        if (audio.contains(ext))
            return CONTENT_AUDIO;
        if (archives.contains(ext) || rarPart.match(ext).hasMatch())
            return CONTENT_ARCHIVE;
        if (diskImages.contains(ext))
            return CONTENT_DISK_IMAGE;
        if (documents.contains(ext))
            return CONTENT_DOCUMENT;
        if (images.contains(ext))
            return CONTENT_IMAGE;
        if (software.contains(ext))
            return CONTENT_SOFTWARE;
        if (subtitles.contains(ext))
            return CONTENT_SUBTITLE;
        return {};
    }
}

QString BitTorrent::classifyHarvestedContent(const QList<HarvestedFile> &files, const qint64 totalSize)
{
    if (files.isEmpty())
        return CONTENT_OTHER;

    QHash<QString, qint64> bytesByCategory;
    QHash<QString, int> countsByCategory;
    qint64 knownBytes = 0;
    int knownFiles = 0;
    for (const HarvestedFile &file : files)
    {
        const QString category = categoryForExtension(extensionForPath(file.path));
        if (category.isEmpty())
            continue;

        ++knownFiles;
        countsByCategory[category] += 1;
        const qint64 size = std::max<qint64>(file.size, 0);
        bytesByCategory[category] += size;
        knownBytes += size;
    }

    if (knownFiles == 0)
        return CONTENT_OTHER;

    const bool useBytes = (knownBytes > 0);
    const qint64 denominator = useBytes ? ((totalSize > 0) ? totalSize : knownBytes) : knownFiles;
    QString bestCategory;
    qint64 bestValue = 0;
    if (useBytes)
    {
        for (auto it = bytesByCategory.cbegin(); it != bytesByCategory.cend(); ++it)
        {
            if (it.value() > bestValue)
            {
                bestValue = it.value();
                bestCategory = it.key();
            }
        }
    }
    else
    {
        for (auto it = countsByCategory.cbegin(); it != countsByCategory.cend(); ++it)
        {
            if (it.value() > bestValue)
            {
                bestValue = it.value();
                bestCategory = it.key();
            }
        }
    }

    if ((bestValue * 100) >= (denominator * 70))
        return bestCategory;

    return CONTENT_MIXED;
}
