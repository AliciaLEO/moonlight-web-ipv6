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

#include "ZipWriter.h"

#include <array>
#include <limits>

namespace {

constexpr quint16 kVersion = 20;       // 2.0: deflate, directories
constexpr quint16 kFlagUtf8 = 1 << 11; // names are UTF-8
constexpr quint16 kStored = 0;
constexpr quint16 kDeflated = 8;

void put16(QByteArray& out, quint16 v)
{
    out.append(char(v & 0xFF));
    out.append(char(v >> 8));
}

void put32(QByteArray& out, quint32 v)
{
    for (int i = 0; i < 4; ++i)
        out.append(char((v >> (8 * i)) & 0xFF));
}

// MS-DOS time and date, local time, two-second resolution. The format starts
// in 1980; an earlier clock (a machine that booted without one) is clamped.
void dosDateTime(const QDateTime& when, quint16* time, quint16* date)
{
    QDateTime t = when.isValid() ? when.toLocalTime() : QDateTime::currentDateTime();
    if (t.date().year() < 1980) t = QDateTime(QDate(1980, 1, 1), QTime(0, 0));
    *time = quint16((t.time().hour() << 11) | (t.time().minute() << 5) | (t.time().second() / 2));
    *date = quint16(((t.date().year() - 1980) << 9) | (t.date().month() << 5) | t.date().day());
}

std::array<quint32, 256> makeCrcTable()
{
    std::array<quint32, 256> table{};
    for (quint32 n = 0; n < 256; ++n) {
        quint32 c = n;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table[n] = c;
    }
    return table;
}

} // namespace

quint32 ZipWriter::crc32(const QByteArray& data, quint32 crc)
{
    static const std::array<quint32, 256> table = makeCrcTable();
    crc = ~crc;
    for (const char ch : data)
        crc = table[(crc ^ quint8(ch)) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

QByteArray ZipWriter::rawDeflate(const QByteArray& data, int level)
{
    if (data.isEmpty()) return {};
    // qCompress: [4-byte big-endian length][CMF FLG][deflate][Adler-32]. zlib's
    // compress2 never sets a preset dictionary, so the header is two bytes.
    const QByteArray z = qCompress(data, level);
    if (z.size() < 4 + 2 + 4) return {};
    return z.mid(4 + 2, z.size() - 4 - 2 - 4);
}

bool ZipWriter::addFile(const QString& name, const QByteArray& data, const QDateTime& modified)
{
    constexpr qint64 kMax = std::numeric_limits<quint32>::max();
    const QByteArray fileName = name.toUtf8();
    if (fileName.isEmpty() || fileName.size() > 0xFFFF || data.size() >= kMax || m_Count >= 0xFFFF)
        return false;

    QByteArray body = rawDeflate(data);
    quint16 method = kDeflated;
    if (body.isEmpty() || body.size() >= data.size()) {
        body = data;
        method = kStored;
    }
    const qint64 offset = m_Out.size();
    if (offset + 30 + fileName.size() + body.size() >= kMax) return false;

    const quint32 crc = crc32(data);
    quint16 time = 0, date = 0;
    dosDateTime(modified, &time, &date);

    // Local file header.
    put32(m_Out, 0x04034b50);
    put16(m_Out, kVersion);
    put16(m_Out, kFlagUtf8);
    put16(m_Out, method);
    put16(m_Out, time);
    put16(m_Out, date);
    put32(m_Out, crc);
    put32(m_Out, quint32(body.size()));
    put32(m_Out, quint32(data.size()));
    put16(m_Out, quint16(fileName.size()));
    put16(m_Out, 0); // extra field
    m_Out.append(fileName);
    m_Out.append(body);

    // Its central directory record, written out by finish().
    put32(m_Central, 0x02014b50);
    put16(m_Central, kVersion); // made by: MS-DOS attributes, spec 2.0
    put16(m_Central, kVersion);
    put16(m_Central, kFlagUtf8);
    put16(m_Central, method);
    put16(m_Central, time);
    put16(m_Central, date);
    put32(m_Central, crc);
    put32(m_Central, quint32(body.size()));
    put32(m_Central, quint32(data.size()));
    put16(m_Central, quint16(fileName.size()));
    put16(m_Central, 0); // extra field
    put16(m_Central, 0); // comment
    put16(m_Central, 0); // disk
    put16(m_Central, 0); // internal attributes
    put32(m_Central, 0); // external attributes
    put32(m_Central, quint32(offset));
    m_Central.append(fileName);

    ++m_Count;
    return true;
}

QByteArray ZipWriter::finish()
{
    const quint32 centralOffset = quint32(m_Out.size());
    m_Out.append(m_Central);
    // End of central directory.
    put32(m_Out, 0x06054b50);
    put16(m_Out, 0); // this disk
    put16(m_Out, 0); // disk holding the directory
    put16(m_Out, quint16(m_Count));
    put16(m_Out, quint16(m_Count));
    put32(m_Out, quint32(m_Central.size()));
    put32(m_Out, centralOffset);
    put16(m_Out, 0); // comment
    m_Central.clear();
    m_Count = 0;
    return std::move(m_Out);
}
