/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>

/**
 * A .zip built in memory, for the log archive someone attaches to an email or
 * a GitHub issue — the one format every OS opens without installing anything
 * and GitHub accepts as an attachment (7z and rar are refused there).
 *
 * Deflate comes from qCompress, public Qt API over the zlib Qt already ships:
 * its output is a 4-byte length, then a zlib stream (RFC 1950) whose body is
 * exactly the raw deflate a zip entry holds. Names are written as UTF-8 and
 * flagged so (general purpose bit 11), which is how an archive unpacks with
 * its names intact on a Chinese or a French system alike. No zip64: an entry
 * or an archive past 4 GB is refused, and a log archive never comes close.
 */
class ZipWriter
{
public:
    /// Add one entry. Compressed unless deflate would not make it smaller
    /// (an empty file, one already compressed), which is then stored as is.
    /// Returns false, adding nothing, past the format's limits.
    bool addFile(const QString& name, const QByteArray& data, const QDateTime& modified);

    /// The whole archive: the entries, then the central directory. The writer
    /// is spent afterwards.
    QByteArray finish();

    int count() const { return m_Count; }

    /// CRC-32 (IEEE 802.3), continued from @p crc for a data split in parts.
    static quint32 crc32(const QByteArray& data, quint32 crc = 0);
    /// The raw deflate of @p data (no zlib header or trailer), or empty for
    /// empty input.
    static QByteArray rawDeflate(const QByteArray& data, int level = 6);

private:
    QByteArray m_Out;
    QByteArray m_Central;
    int m_Count = 0;
};
