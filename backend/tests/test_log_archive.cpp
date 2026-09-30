/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "test_framework.h"
#include "common/Logger.h"
#include "common/ZipWriter.h"
#include "server/LogArchive.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMap>
#include <QTemporaryDir>
#include <QThread>

namespace {

quint32 le32(const QByteArray& b, qsizetype at)
{
    return quint32(quint8(b[at])) | quint32(quint8(b[at + 1])) << 8 |
           quint32(quint8(b[at + 2])) << 16 | quint32(quint8(b[at + 3])) << 24;
}

quint16 le16(const QByteArray& b, qsizetype at)
{
    return quint16(quint8(b[at]) | quint8(b[at + 1]) << 8);
}

quint32 adler32(const QByteArray& data)
{
    quint32 a = 1, b = 0;
    for (const char c : data) {
        a = (a + quint8(c)) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

struct Entry
{
    quint16 flags = 0;
    quint16 method = 0;
    quint32 crc = 0;
    quint32 size = 0;
    QByteArray raw;
};

// Every entry of an archive, read the way an unzip tool finds them: from the
// end record to the central directory, then to each local header. Empty when
// any signature is not where it should be.
QMap<QString, Entry> readZip(const QByteArray& zip)
{
    QMap<QString, Entry> out;
    if (zip.size() < 22) return {};
    const qsizetype eocd = zip.size() - 22;
    if (le32(zip, eocd) != 0x06054b50) return {};
    const int count = le16(zip, eocd + 10);
    qsizetype cd = le32(zip, eocd + 16);
    for (int i = 0; i < count; ++i) {
        if (le32(zip, cd) != 0x02014b50) return {};
        Entry e;
        e.flags = le16(zip, cd + 8);
        e.method = le16(zip, cd + 10);
        e.crc = le32(zip, cd + 16);
        const quint32 csize = le32(zip, cd + 20);
        e.size = le32(zip, cd + 24);
        const quint16 nlen = le16(zip, cd + 28);
        const quint32 local = le32(zip, cd + 42);
        const QString name = QString::fromUtf8(zip.mid(cd + 46, nlen));
        if (le32(zip, local) != 0x04034b50) return {};
        const qsizetype body = local + 30 + le16(zip, local + 26) + le16(zip, local + 28);
        e.raw = zip.mid(body, csize);
        out.insert(name, e);
        cd += 46 + nlen + le16(zip, cd + 30) + le16(zip, cd + 32);
    }
    return out;
}

// Whether @p e holds exactly @p expected: stored as is, or deflated so that
// zlib inflates it to those bytes. qUncompress wants the zlib wrapper and its
// Adler-32, which only the expected bytes can give; zlib refuses the stream
// unless its output matches that checksum, and the CRC is checked on top.
bool holds(const Entry& e, const QByteArray& expected)
{
    if (e.size != quint32(expected.size()) || e.crc != ZipWriter::crc32(expected)) return false;
    if (e.method == 0) return e.raw == expected;
    if (e.method != 8) return false;
    QByteArray z;
    for (int i = 3; i >= 0; --i)
        z.append(char((e.size >> (8 * i)) & 0xFF));
    z.append(char(0x78));
    z.append(char(0x9C));
    z.append(e.raw);
    const quint32 adler = adler32(expected);
    for (int i = 3; i >= 0; --i)
        z.append(char((adler >> (8 * i)) & 0xFF));
    return qUncompress(z) == expected;
}

void touch(const QString& path, const QByteArray& data, const QDateTime& when)
{
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(data);
    f.close();
    QFile g(path);
    g.open(QIODevice::ReadWrite);
    g.setFileTime(when, QFileDevice::FileModificationTime);
    g.close();
}

} // namespace

void run_log_archive_tests()
{
    SECTION("LogArchive / ZipWriter");

    // The check value every CRC-32 implementation is held to.
    CHECK_EQ(ZipWriter::crc32(QByteArray("123456789")), quint32(0xCBF43926));
    CHECK_EQ(ZipWriter::crc32(QByteArray()), quint32(0));
    CHECK(ZipWriter::rawDeflate(QByteArray()).isEmpty());

    // An archive read back as an unzip tool would: names (a Chinese one among
    // them) flagged UTF-8, bodies inflated to the same bytes, CRCs right; a
    // log compresses, an empty file is stored.
    {
        QByteArray log;
        for (int i = 0; i < 2000; ++i)
            log += "[2026-09-30 10:00:00.000] [INFO] [NETWORK] poll serverinfo — 日志 " +
                   QByteArray::number(i) + "\n";
        ZipWriter zip;
        CHECK(zip.addFile(QStringLiteral("about.txt"), "MoonlightWeb test\n",
                          QDateTime::currentDateTime()));
        CHECK(zip.addFile(QStringLiteral("服务器日志.log"), log, QDateTime::currentDateTime()));
        CHECK(zip.addFile(QStringLiteral("empty.log"), QByteArray(), QDateTime()));
        CHECK(!zip.addFile(QString(), "x", QDateTime::currentDateTime()));
        CHECK_EQ(zip.count(), 3);
        const QByteArray bytes = zip.finish();
        CHECK(bytes.size() < log.size() / 4); // deflate did its work
        const QMap<QString, Entry> entries = readZip(bytes);
        CHECK_EQ(entries.size(), qsizetype(3));
        for (const Entry& e : entries)
            CHECK(e.flags & (1 << 11)); // names flagged UTF-8
        CHECK(holds(entries.value(QStringLiteral("服务器日志.log")), log));
        CHECK_EQ(entries.value(QStringLiteral("服务器日志.log")).method, quint16(8));
        CHECK(holds(entries.value(QStringLiteral("about.txt")), "MoonlightWeb test\n"));
        CHECK(holds(entries.value(QStringLiteral("empty.log")), QByteArray()));
        CHECK_EQ(entries.value(QStringLiteral("empty.log")).method, quint16(0));
    }

    // Kinds: a log's archives are that log, every worker's file is one kind.
    CHECK_EQ(LogArchive::kindOf("moonlightweb.log"), QString("moonlightweb.log"));
    CHECK_EQ(LogArchive::kindOf("moonlightweb.log.20260930-101010"), QString("moonlightweb.log"));
    CHECK_EQ(LogArchive::kindOf("moonlightweb.log.20260930-101010-2"), QString("moonlightweb.log"));
    CHECK_EQ(LogArchive::kindOf("moonlightweb-worker-13372.log"),
             QString("moonlightweb-worker-*.log"));
    CHECK_EQ(LogArchive::kindOf("moonlightweb-worker-service.log"),
             QString("moonlightweb-worker-service.log"));
    CHECK_EQ(LogArchive::kindOf("moonlightweb-client.log.20260930-101010"),
             QString("moonlightweb-client.log"));

    // Only the newest non-empty file of each kind, never the directory.
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString dir = tmp.path();
    const QDateTime now = QDateTime::currentDateTime();
    touch(dir + "/moonlightweb.log", "live\n", now.addSecs(-10));
    touch(dir + "/moonlightweb.log.20260929-080000", "yesterday\n", now.addDays(-1));
    touch(dir + "/moonlightweb-worker-100.log", "old stream\n", now.addSecs(-3600));
    touch(dir + "/moonlightweb-worker-200.log", "last stream\n", now.addSecs(-60));
    touch(dir + "/moonlightweb-worker-300.log", "", now); // started, wrote nothing
    touch(dir + "/moonlightweb-probe.log", "probe\n", now.addDays(-3));
    touch(dir + "/moonlightweb-vdisplay.log", "", now.addDays(-2));
    {
        const QList<QFileInfo> picked =
            LogArchive::latestPerKind(QDir(dir).entryInfoList(QDir::Files));
        QStringList names;
        for (const QFileInfo& f : picked)
            names << f.fileName();
        CHECK_EQ(names, QStringList({"moonlightweb-probe.log", "moonlightweb-worker-200.log",
                                     "moonlightweb.log"}));
    }

    // The whole job, on its thread: about.txt plus those three, nothing else.
    {
        LogArchive archive;
        CHECK_EQ(archive.status().state, QString("idle"));
        CHECK(archive.start(dir, QString(), "about\n", "logs.zip"));
        QElapsedTimer t;
        t.start();
        while (archive.status().state == QLatin1String("running") && t.elapsed() < 10000)
            QThread::msleep(10);
        const LogArchive::Status s = archive.status();
        CHECK_EQ(s.state, QString("done"));
        CHECK_EQ(s.files, 3);
        CHECK_EQ(s.doneBytes, s.totalBytes);
        CHECK_EQ(s.fileName, QString("logs.zip"));
        const QMap<QString, Entry> entries = readZip(archive.result());
        CHECK_EQ(entries.keys(), QStringList({"about.txt", "moonlightweb-probe.log",
                                              "moonlightweb-worker-200.log", "moonlightweb.log"}));
        CHECK(holds(entries.value("moonlightweb-worker-200.log"), "last stream\n"));
        CHECK(holds(entries.value("about.txt"), "about\n"));

        // A server logging elsewhere (--log): its file stands for the server's.
        QTemporaryDir other;
        touch(other.path() + "/dev-run.log", "the running server\n", now);
        CHECK(archive.start(dir, other.path() + "/dev-run.log", "about\n", "logs.zip"));
        t.restart();
        while (archive.status().state == QLatin1String("running") && t.elapsed() < 10000)
            QThread::msleep(10);
        const QMap<QString, Entry> again = readZip(archive.result());
        CHECK(again.contains("dev-run.log"));
        CHECK(!again.contains("moonlightweb.log"));
    }

    // The Logger's threshold: DEBUG dropped unless asked for.
    {
        Logger* log = Logger::client();
        log->setLogFile(dir + "/threshold.log", /*rotating=*/false);
        log->setMinLevel(Logger::Info);
        log->log(Logger::Debug, "hidden line");
        log->log(Logger::Info, "shown line");
        log->setMinLevel(Logger::Debug);
        log->log(Logger::Debug, "verbose line");
        QFile f(dir + "/threshold.log");
        f.open(QIODevice::ReadOnly);
        const QByteArray text = f.readAll();
        CHECK(!text.contains("hidden line"));
        CHECK(text.contains("shown line"));
        CHECK(text.contains("verbose line"));
        CHECK_EQ(Logger::instance()->minLevel(), Logger::Info); // the default
    }
}
