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

#include "LogScrubber.h"

#include <QHostAddress>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace {

const QString kHidden = QStringLiteral("(hidden)");

// Every match of @p re in @p s, replaced by what @p f makes of it.
template <typename F> QString replaceEach(const QString& s, const QRegularExpression& re, F f)
{
    QRegularExpressionMatchIterator it = re.globalMatch(s);
    if (!it.hasNext()) return s;
    QString out;
    out.reserve(s.size());
    qsizetype last = 0;
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += QStringView(s).mid(last, m.capturedStart() - last);
        out += f(m);
        last = m.capturedEnd();
    }
    out += QStringView(s).mid(last);
    return out;
}

QRegularExpression re(const char* pattern)
{
    return QRegularExpression(QString::fromUtf8(pattern),
                              QRegularExpression::CaseInsensitiveOption);
}

enum class V4Kind
{
    Keep,
    Lan,
    Public
};

// @p o gets the four octets. Kept as they are: what is the same on every
// machine (loopback, 0/8, multicast, broadcast) and the documentation ranges,
// which are the stand-ins themselves. LAN: the private ranges, CGNAT (which
// Tailscale uses) and link-local.
V4Kind classifyV4(const QString& ip, int o[4])
{
    const QStringList parts = ip.split(QLatin1Char('.'));
    if (parts.size() != 4) return V4Kind::Keep;
    for (int i = 0; i < 4; ++i) {
        bool ok = false;
        o[i] = parts[i].toInt(&ok);
        if (!ok || o[i] < 0 || o[i] > 255) return V4Kind::Keep;
    }
    if (o[0] == 127 || o[0] == 0 || o[0] >= 224 || (o[0] == 192 && o[1] == 0 && o[2] == 2) ||
        (o[0] == 198 && o[1] == 51 && o[2] == 100) || (o[0] == 203 && o[1] == 0 && o[2] == 113) ||
        (o[0] == 198 && (o[1] & 0xFE) == 18))
        return V4Kind::Keep;
    if (o[0] == 10 || (o[0] == 172 && o[1] >= 16 && o[1] <= 31) || (o[0] == 192 && o[1] == 168) ||
        (o[0] == 169 && o[1] == 254) || (o[0] == 100 && o[1] >= 64 && o[1] <= 127))
        return V4Kind::Lan;
    return V4Kind::Public;
}

// A, B, ... Z, AA, AB...: the letters of the n-th subnet (1-based).
QString subnetLetters(int n)
{
    QString s;
    while (n > 0) {
        --n;
        s.prepend(QChar(u'A' + n % 26));
        n /= 26;
    }
    return s;
}

// Words a host may well be called that are also words of the log itself.
const QSet<QString>& genericNames()
{
    static const QSet<QString> s{
        QStringLiteral("wolf"),      QStringLiteral("sunshine"),  QStringLiteral("apollo"),
        QStringLiteral("host"),      QStringLiteral("server"),    QStringLiteral("desktop"),
        QStringLiteral("localhost"), QStringLiteral("moonlight"), QStringLiteral("moonlightweb"),
        QStringLiteral("windows"),   QStringLiteral("linux"),     QStringLiteral("macos"),
        QStringLiteral("ubuntu"),    QStringLiteral("debian"),    QStringLiteral("native"),
        QStringLiteral("multiseat"), QStringLiteral("user"),      QStringLiteral("admin"),
        QStringLiteral("public"),    QStringLiteral("default"),   QStringLiteral("system"),
        QStringLiteral("root"),      QStringLiteral("guest")};
    return s;
}

// Subdomains of moonlightweb.top that are the project's own, not an instance's.
const QSet<QString>& projectLabels()
{
    static const QSet<QString> s{QStringLiteral("stream"), QStringLiteral("app"),
                                 QStringLiteral("www"),    QStringLiteral("dev"),
                                 QStringLiteral("docs"),   QStringLiteral("dns"),
                                 QStringLiteral("ns1"),    QStringLiteral("ns2"),
                                 QStringLiteral("api"),    QStringLiteral("instance")};
    return s;
}

} // namespace

LogScrubber::LogScrubber(const Names& names)
{
    QSet<QString> seen;
    auto add = [&](const QString& raw, const QString& kind, const QString& of = QString()) {
        const QString name = raw.trimmed();
        const QString key = name.toLower();
        if (name.size() < 3 || seen.contains(key) || genericNames().contains(key)) return;
        seen.insert(key);
        m_Names.append({name, kind, of.isEmpty() ? name : of,
                        QRegularExpression(QStringLiteral("(?<![A-Za-z0-9])%1(?![A-Za-z0-9])")
                                               .arg(QRegularExpression::escape(name)),
                                           QRegularExpression::CaseInsensitiveOption)});
    };
    // This PC first: it is often in the host list too, and this-pc says more.
    add(names.thisMachine, QStringLiteral("this-pc"));
    add(names.instance, QStringLiteral("instance-name"));
    add(names.user, QStringLiteral("user"));
    for (const QString& h : names.hosts) {
        add(h, QStringLiteral("host"));
        // "leos-macbook-pro2.home" is also written without its domain.
        const qsizetype dot = h.indexOf(QLatin1Char('.'));
        if (dot > 0) add(h.left(dot), QStringLiteral("host"), h.trimmed());
    }
    std::sort(m_Names.begin(), m_Names.end(),
              [](const NameRule& a, const NameRule& b) { return a.name.size() > b.name.size(); });
}

QString LogScrubber::nameFor(const QString& key, const QString& kind)
{
    const QString k = kind + QLatin1Char('|') + key.toLower();
    auto it = m_Stand.constFind(k);
    if (it != m_Stand.constEnd()) return *it;
    QString stand;
    if (kind == QLatin1String("host"))
        stand = QStringLiteral("host-%1").arg(++m_Hosts);
    else if (kind == QLatin1String("tailnet"))
        stand = QStringLiteral("tailnet-%1.ts.net").arg(++m_Tailnet);
    else
        stand = kind;
    m_Stand.insert(k, stand);
    return stand;
}

QString LogScrubber::publicV4(const QString& ip)
{
    const QString k = QStringLiteral("v4|") + ip;
    auto it = m_Stand.constFind(k);
    if (it != m_Stand.constEnd()) return *it;
    const int n = m_V4++;
    // The three documentation ranges, one after the other.
    const QString stand = n < 254   ? QStringLiteral("203.0.113.%1").arg(n + 1)
                          : n < 508 ? QStringLiteral("198.51.100.%1").arg(n - 253)
                                    : QStringLiteral("192.0.2.%1").arg((n - 508) % 254 + 1);
    m_Stand.insert(k, stand);
    return stand;
}

QString LogScrubber::publicV6(const QString& ip)
{
    const QString k = QStringLiteral("v6|") + QHostAddress(ip).toString();
    auto it = m_Stand.constFind(k);
    if (it != m_Stand.constEnd()) return *it;
    const QString stand = QStringLiteral("2001:db8::%1").arg(++m_V6, 0, 16);
    m_Stand.insert(k, stand);
    return stand;
}

QString LogScrubber::lanHost(const QString& subnet, const QString& host, int fixed)
{
    const QString k = QStringLiteral("lan|") + subnet + QLatin1Char('|') + host;
    auto it = m_Stand.constFind(k);
    if (it != m_Stand.constEnd()) return *it;
    QString& letters = m_SubnetLetters[subnet];
    if (letters.isEmpty()) letters = subnetLetters(++m_Subnets);
    int n = fixed;
    if (n < 0) {
        n = ++m_SubnetHosts[subnet];
        if (n == 255) n = ++m_SubnetHosts[subnet]; // .255 is the broadcast's
    }
    const QString stand = letters + QString::number(n);
    m_Stand.insert(k, stand);
    return stand;
}

void LogScrubber::replaceNames(QString& line)
{
    for (const NameRule& rule : m_Names) {
        if (!line.contains(rule.name, Qt::CaseInsensitive)) continue;
        line = replaceEach(line, rule.word, [&](const QRegularExpressionMatch& m) {
            return nameFor(rule.key, rule.kind);
        });
    }
}

void LogScrubber::replaceAddresses(QString& line)
{
    static const QRegularExpression v4(
        QStringLiteral(R"((?<![\w.])((?:\d{1,3}\.){3}\d{1,3})(?!\w|\.\d))"));
    if (line.contains(QLatin1Char('.'))) {
        line = replaceEach(line, v4, [&](const QRegularExpressionMatch& m) {
            const QString ip = m.captured(1);
            // "Chrome/153.0.0.0": a product's version, not an address.
            const qsizetype at = m.capturedStart();
            if (at >= 2 && line[at - 1] == QLatin1Char('/') && line[at - 2].isLetterOrNumber())
                return ip;
            int o[4];
            switch (classifyV4(ip, o)) {
            case V4Kind::Keep: return ip;
            case V4Kind::Public: return publicV4(ip);
            case V4Kind::Lan: break;
            }
            // The range's own prefix stays, so a reader still sees which kind
            // of network it is: 192.168.A1, 10.B2, 172.24.C1, 100.D1.
            QString prefix;
            if (o[0] == 192 || o[0] == 169)
                prefix = QStringLiteral("%1.%2.").arg(o[0]).arg(o[1]);
            else if (o[0] == 172)
                prefix = QStringLiteral("172.%1.").arg(o[1]);
            else
                prefix = QStringLiteral("%1.").arg(o[0]);
            // The network and the broadcast keep their number.
            const int fixed = o[3] == 0 ? 0 : o[3] == 255 ? 255 : -1;
            return prefix + lanHost(QStringLiteral("%1.%2.%3").arg(o[0]).arg(o[1]).arg(o[2]),
                                    QString::number(o[3]), fixed);
        });
    }

    // Groups of up to five characters: libdatachannel writes an address and
    // its port without brackets ("fd7a:115c:a1e0::c036:292d:48550/UDP").
    static const QRegularExpression v6(
        QStringLiteral(R"((?<![\w:.])([0-9A-Fa-f]{0,5}(?::[0-9A-Fa-f]{0,5}){2,8})(?![\w:.]))"));
    if (line.contains(QLatin1Char(':'))) {
        static const QPair<QHostAddress, int> doc = QHostAddress::parseSubnet("2001:db8::/32");
        static const QPair<QHostAddress, int> mapped = QHostAddress::parseSubnet("::ffff:0:0/96");
        line = replaceEach(line, v6, [&](const QRegularExpressionMatch& m) {
            QString text = m.captured(1);
            QString port;
            QHostAddress a(text);
            if (a.protocol() != QAbstractSocket::IPv6Protocol) {
                const qsizetype colon = text.lastIndexOf(QLatin1Char(':'));
                bool digits = colon > 0 && colon + 1 < text.size();
                for (qsizetype i = colon + 1; digits && i < text.size(); ++i)
                    digits = text[i].isDigit();
                if (!digits) return text;
                a = QHostAddress(text.left(colon));
                if (a.protocol() != QAbstractSocket::IPv6Protocol) return text;
                port = text.mid(colon);
                text.truncate(colon);
            }
            if (a.isLoopback() || a.isMulticast() || a == QHostAddress(QHostAddress::AnyIPv6) ||
                a.isInSubnet(doc) || a.isInSubnet(mapped))
                return text + port;
            if (!a.isLinkLocal() && !a.isUniqueLocalUnicast()) return publicV6(text) + port;
            if (!a.isLinkLocal() && !a.isUniqueLocalUnicast()) return publicV6(text);
            // ULA (Tailscale's among them) and link-local, whose host part may
            // be the card's MAC: a letter for the /64, a number for the host.
            const Q_IPV6ADDR b = a.toIPv6Address();
            const QByteArray bytes(reinterpret_cast<const char*>(b.c), 16);
            return (a.isLinkLocal() ? QStringLiteral("fe80::") : QStringLiteral("fd::")) +
                   lanHost(QString::fromLatin1(bytes.left(8).toHex()),
                           QString::fromLatin1(bytes.mid(8).toHex()), -1) +
                   port;
        });
    }
}

QString LogScrubber::scrubLine(QString line)
{
    // Keyboard diagnostics (keyboard_debug): each line is a key someone typed,
    // passwords included. The line stays, so the timing does.
    static const QRegularExpression kbd(QStringLiteral(R"(\[KBD\] (?!keyboard diagnostics).*$)"));
    if (line.contains(QLatin1String("[KBD]")))
        line.replace(kbd, QStringLiteral("[KBD] (keystroke hidden)"));

    // ── Secrets ──────────────────────────────────────────────────────────────
    // key=value in URLs and settings: rikey and the pairing handshake ride in
    // Qt's own error strings, which quote the whole URL.
    static const QRegularExpression param = re(
        R"((?<![A-Za-z0-9_-])(rikey|rikeyid|uniqueid|salt|clientcert|clientchallenge|serverchallengeresp|clientpairingsecret|pairingsecret|devicename|token|access_token|refresh_token|mwk|k|key|pin|password|passwd|pwd|secret|sig|signature|auth)=(?!\(hidden\))([^&\s"'#<>]+))");
    line.replace(param, QStringLiteral("\\1=") + kHidden);

    // key="value", key= 'value', key: "value".
    static const QRegularExpression quoted = re(
        R"((?<![A-Za-z0-9_-])(token|key|password|passwd|pwd|secret|ufrag|username|uid|uniqueid|unique id|pin)(\s*[=:]\s*)(["'])(?!\(hidden\))(.*?)\3)");
    line.replace(quoted, QStringLiteral("\\1\\2\\3") + kHidden + QStringLiteral("\\3"));
    // libjuice's ufrag checks name both sides.
    static const QRegularExpression ufragSides = re(R"(\b(expected|actual)=(["'])(.*?)\2)");
    if (line.contains(QLatin1String("ufrag"), Qt::CaseInsensitive))
        line.replace(ufragSides, QStringLiteral("\\1=\\2") + kHidden + QStringLiteral("\\2"));

    // The client's unique id, the way IdentityManager and the DNS side say it.
    static const QRegularExpression uniqueId =
        re(R"(\b(unique ?id|uid)(\s*[:=]\s*|\s+)(?!\(hidden\))([0-9A-Za-z_-]{6,}))");
    line.replace(uniqueId, QStringLiteral("\\1\\2") + kHidden);

    // "[Auth] PIN changed: 482913", "…, PIN: 0427".
    static const QRegularExpression pin = re(R"(\bPIN\b([^\d\n]{0,12}?)\b\d{4,8}\b)");
    line.replace(pin, QStringLiteral("PIN\\1") + kHidden);

    // ICE credentials: SDP attributes and the ufrag of a browser's candidate.
    static const QRegularExpression iceAttr = re(R"(a=ice-(ufrag|pwd):\S+)");
    line.replace(iceAttr, QStringLiteral("a=ice-\\1:") + kHidden);
    static const QRegularExpression iceUfrag = re(R"(\bufrag\s+(?!\(hidden\))\S+)");
    if (line.contains(QLatin1String("candidate"), Qt::CaseInsensitive))
        line.replace(iceUfrag, QStringLiteral("ufrag ") + kHidden);

    static const QRegularExpression header =
        re(R"(\b(authorization|proxy-authorization|cookie|set-cookie|x-api-key)(\s*[:=]\s*)\S.*$)");
    line.replace(header, QStringLiteral("\\1\\2") + kHidden);

    // Key fingerprints (the tunnel's host key) and pairing key ids.
    static const QRegularExpression fingerprint(
        QStringLiteral(R"((?:[0-9A-Fa-f]{2}:){7,}[0-9A-Fa-f]{2})"));
    line.replace(fingerprint, kHidden);
    static const QRegularExpression keyId = re(R"(\b(pairing key|host key)\s+(?!\(hidden\))\S+)");
    line.replace(keyId, QStringLiteral("\\1 ") + kHidden);

    // A pairing reply's XML.
    static const QRegularExpression xml = re(
        R"(<(plaincert|pairingsecret|challengeresponse|encodedcipher|uniqueid|salt|clientcert|mac|clientchallenge|serverchallenge|serverchallengeresp|clientpairingsecret)>[^<]*</\1>)");
    line.replace(xml, QStringLiteral("<\\1>") + kHidden + QStringLiteral("</\\1>"));

    // ── What locates a person ────────────────────────────────────────────────
    static const QRegularExpression email(
        QStringLiteral(R"([A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,})"));
    line.replace(email, QStringLiteral("(hidden e-mail)"));
    static const QRegularExpression mac(QStringLiteral(
        R"((?<![0-9A-Fa-f:-])(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}(?![0-9A-Fa-f:-]))"));
    line.replace(mac, QStringLiteral("(hidden MAC)"));
    static const QRegularExpression geo(
        QStringLiteral(R"((Geo data stored for session \S+: ).*$)"));
    line.replace(geo, QStringLiteral("\\1") + kHidden);
    // What window the host had in front: its title may name a document.
    static const QRegularExpression window(QStringLiteral(R"((Input gate closed \([^)]*\): ).*$)"));
    line.replace(window, QStringLiteral("\\1") + kHidden);

    // ── Links that open this instance ────────────────────────────────────────
    static const QRegularExpression rdv =
        re(R"(\b((?:stream|app)(?:\.dev)?\.moonlightweb\.top)/(?!\(link\))([A-Za-z0-9_-]{6,}))");
    line.replace(rdv, QStringLiteral("\\1/(link)"));
    static const QRegularExpression share(
        QStringLiteral(R"((?<![A-Za-z0-9_])/p/[A-Za-z0-9_-]{6,})"));
    line.replace(share, QStringLiteral("/p/(link)"));
    // A client log's page path, when the page came through the rendezvous.
    static const QRegularExpression pagePath(QStringLiteral(R"(\bpage /[a-z0-9]{20,}\b)"));
    line.replace(pagePath, QStringLiteral("page /(link)"));
    // The instance's own name under moonlightweb.top.
    static const QRegularExpression subdomain =
        re(R"(\b([a-z0-9][a-z0-9-]*)\.((?:dev\.)?moonlightweb\.top)\b)");
    if (line.contains(QLatin1String("moonlightweb.top"), Qt::CaseInsensitive)) {
        line = replaceEach(line, subdomain, [](const QRegularExpressionMatch& m) {
            if (projectLabels().contains(m.captured(1).toLower())) return m.captured(0);
            return QStringLiteral("instance.") + m.captured(2);
        });
    }
    static const QRegularExpression tailnet = re(R"(\b[a-z0-9-]+(?:\.[a-z0-9-]+)*\.ts\.net\b)");
    if (line.contains(QLatin1String(".ts.net"), Qt::CaseInsensitive)) {
        line = replaceEach(line, tailnet, [&](const QRegularExpressionMatch& m) {
            if (m.captured(0).startsWith(QLatin1String("tailnet-"))) return m.captured(0);
            return nameFor(m.captured(0), QStringLiteral("tailnet"));
        });
    }

    // ── Names ────────────────────────────────────────────────────────────────
    // Home paths, in both of the log's spellings (Qt's << doubles backslashes).
    static const QRegularExpression home =
        re(R"(((?:[A-Z]:(?:\\{1,2}|/)+Users(?:\\{1,2}|/)+)|/home/|/Users/)([^\\/\s"':*?<>|]+))");
    line = replaceEach(line, home, [](const QRegularExpressionMatch& m) {
        const QString who = m.captured(2).toLower();
        if (who == QLatin1String("public") || who == QLatin1String("default") ||
            who == QLatin1String("shared") || who == QLatin1String("user"))
            return m.captured(0);
        return m.captured(1) + QStringLiteral("user");
    });
    replaceNames(line);
    // A machine the host list does not know, by its LAN name (mDNS, DHCP):
    // "Leos-MacBook-Pro.local". Chrome's random mDNS candidates (a UUID) stay,
    // and so do the stand-ins put in above.
    static const QRegularExpression lanName =
        re(R"((?<![\w.-])([a-z0-9][a-z0-9-]*)\.(local|lan|home|localdomain|internal)\b)");
    static const QRegularExpression uuidLike(
        QStringLiteral(R"(^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression standIn(
        QStringLiteral(R"(^(host-\d+|this-pc|instance-name|user|tailnet-\d+)$)"));
    line = replaceEach(line, lanName, [&](const QRegularExpressionMatch& m) {
        const QString label = m.captured(1);
        if (uuidLike.match(label).hasMatch() || standIn.match(label).hasMatch())
            return m.captured(0);
        return nameFor(label, QStringLiteral("host")) + QLatin1Char('.') + m.captured(2);
    });

    replaceAddresses(line);
    return line;
}

QByteArray LogScrubber::scrub(const QByteArray& text)
{
    QString s = QString::fromUtf8(text);
    // A PEM block spans lines; nothing of it is worth keeping.
    static const QRegularExpression pem(
        QStringLiteral(R"(-----BEGIN ([A-Z0-9 ]+)-----.*?-----END \1-----)"),
        QRegularExpression::DotMatchesEverythingOption);
    s.replace(pem, QStringLiteral("-----BEGIN \\1----- (hidden) -----END \\1-----"));

    QStringList lines = s.split(QLatin1Char('\n'));
    for (QString& line : lines) {
        const bool cr = line.endsWith(QLatin1Char('\r'));
        if (cr) line.chop(1);
        line = scrubLine(line);
        if (cr) line += QLatin1Char('\r');
    }
    return lines.join(QLatin1Char('\n')).toUtf8();
}

QString LogScrubber::notice()
{
    return QStringLiteral(
        "Before these logs were archived, what they held of the machine and its owner was\n"
        "taken out: PINs, passwords, keys and tokens, cookies, the links that open this\n"
        "instance, typed keys, window titles, e-mail and MAC addresses show as \"(hidden)\".\n"
        "Public IP addresses are replaced by documentation ones (203.0.113.x, 2001:db8::x),\n"
        "LAN ones by a letter for their network and a number for the machine (192.168.A1,\n"
        "10.B2, 100.C1, fd::D1), machine and user names by host-N, this-pc, instance-name\n"
        "and user: the same value always by the same stand-in.\n");
}
