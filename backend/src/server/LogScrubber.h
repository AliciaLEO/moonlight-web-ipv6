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
#include <QHash>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

/**
 * What the "Download logs" archive does to every file before it goes in: the
 * archive is made to be attached to a public GitHub issue, and a log written
 * for its owner holds what nobody else should read.
 *
 * Hidden, as "(hidden)": what grants access or decrypts — PINs, passwords,
 * tokens, keys (rikey, pairing secrets, ICE credentials, the tunnel's host
 * key), cookies and Authorization headers, PEM blocks, the client's unique id,
 * the rendezvous and share links, and what a person typed or looked at
 * (keyboard diagnostics, window titles, geolocation, e-mail and MAC addresses).
 *
 * Replaced, the same value by the same stand-in across the whole archive so a
 * reader can still follow one machine or one address from line to line:
 *   - public IP addresses by documentation addresses (203.0.113.x, then
 *     198.51.100.x and 192.0.2.x; 2001:db8::x). LAN, loopback, link-local,
 *     CGNAT/Tailscale and ULA addresses stay: they are what explains a LAN
 *     problem, and they locate nobody;
 *   - the names it is given (Names) by host-N, this-pc, instance-name and
 *     user, and the user name in home paths (C:\Users\<name>, /home/<name>,
 *     /Users/<name>), the instance's own moonlightweb.top subdomain and
 *     Tailscale names.
 *
 * A best effort over free text, not a guarantee: it knows the formats
 * MoonlightWeb writes, and the names it is told.
 */
class LogScrubber
{
public:
    struct Names
    {
        QStringList hosts;   ///< the streaming hosts' names (host-N)
        QString thisMachine; ///< this PC's name (this-pc)
        QString instance;    ///< the instance name the owner chose (instance-name)
        QString user;        ///< the OS account the server runs as (user)
    };

    LogScrubber() = default;
    explicit LogScrubber(const Names& names);

    /// A whole file (UTF-8), line by line.
    QByteArray scrub(const QByteArray& text);
    /// One line, no newline.
    QString scrubLine(QString line);

    /// For about.txt: what was done to the files, in a few lines.
    static QString notice();

private:
    QString publicV4(const QString& ip);
    QString publicV6(const QString& ip);
    QString nameFor(const QString& key, const QString& kind);
    void replaceNames(QString& line);
    void replaceAddresses(QString& line);

    struct NameRule
    {
        QString name;
        QString kind;
        QString key;             ///< the name it stands for: a host's short name is its full one
        QRegularExpression word; ///< the name as a whole word, any case
    };
    // Longest first, so "leos-macbook-pro2.home" goes before "leos-macbook-pro2".
    QList<NameRule> m_Names;
    QHash<QString, QString> m_Stand; ///< lower-cased value → its stand-in
    int m_Hosts = 0;
    int m_V4 = 0;
    int m_V6 = 0;
    int m_Tailnet = 0;
};
