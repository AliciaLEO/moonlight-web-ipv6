/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The guests' shared feed over its local pipe (plan « flux commun des
 * invités », S3): who gets in, what they are told first, the pictures in order
 * with their stamps, the way back for control messages, and a subscriber that
 * stops reading, which must never hold the feed back.
 */
#include "test_framework.h"

#include "streaming/FeedPublisher.h"
#include "streaming/FeedSubscriber.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QLocalSocket>
#include <QThread>

#include <atomic>
#include <mutex>
#include <vector>

namespace {

/// Pump this thread's events until @p done or @p ms pass.
template <class F> bool waitFor(F done, int ms = 3000)
{
    QElapsedTimer t;
    t.start();
    while (!done()) {
        if (t.elapsed() > ms) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return true;
}

struct Received
{
    std::mutex mutex;
    std::vector<uint32_t> numbers;
    std::vector<bool> keyframes;
    std::vector<int64_t> present;
    std::vector<size_t> sizes;
    bool payloadIntact = true;
};

} // namespace

void run_feed_ipc_tests()
{
    SECTION("Feed pipe — names differ per launch, tokens are 128 bits, compared whole");
    {
        const QString a = feedpipe::newName(QStringLiteral("MoonlightWeb-dev"));
        const QString b = feedpipe::newName(QStringLiteral("MoonlightWeb-dev"));
        CHECK(a != b);
        CHECK(a.startsWith(QStringLiteral("mw-feed-MoonlightWeb-dev-")));
        const QByteArray t = feedpipe::newToken();
        CHECK_EQ(t.size(), qsizetype(32));
        CHECK(feedpipe::sameToken(t, t));
        QByteArray other = t;
        other[31] = other[31] == 'a' ? 'b' : 'a';
        CHECK(!feedpipe::sameToken(t, other));
        CHECK(!feedpipe::sameToken(t, t.left(31)));
        CHECK(!feedpipe::sameToken(QByteArray(), QByteArray()));
    }

    SECTION("Feed pipe — a wrong token is refused, the right one is told `info` first");
    {
        const QString name = feedpipe::newName(QStringLiteral("test"));
        const QByteArray token = feedpipe::newToken();
        FeedPublisher pub(name, token);
        CHECK(pub.start());
        pub.setInfo(QJsonObject{{QStringLiteral("type"), QStringLiteral("info")},
                                {QStringLiteral("codec"), QStringLiteral("HEVC")},
                                {QStringLiteral("width"), 1920},
                                {QStringLiteral("height"), 1080}});

        FeedSubscriber intruder(name, feedpipe::newToken(), 3);
        QString why;
        QJsonObject info;
        CHECK(!intruder.start(1500, &info, &why));
        CHECK(!why.isEmpty());
        CHECK_EQ(pub.subscribers(), 0);

        std::atomic<int> joined{0};
        QObject::connect(
            &pub, &FeedPublisher::subscriberJoined, &pub,
            [&joined](int, int slot) { joined = slot; }, Qt::DirectConnection);
        FeedSubscriber guest(name, token, 2);
        CHECK(guest.start(3000, &info, &why));
        CHECK_EQ(info.value(QStringLiteral("codec")).toString(), QStringLiteral("HEVC"));
        CHECK_EQ(info.value(QStringLiteral("height")).toInt(), 1080);
        CHECK(waitFor([&]() { return pub.subscribers() == 1; }));
        CHECK(waitFor([&]() { return joined.load() == 2; }));
        guest.stop();
        CHECK(waitFor([&]() { return pub.subscribers() == 0; }));
        pub.stop();
    }

    SECTION("Feed pipe — pictures arrive in order, whole, with the frame's own stamps");
    {
        const QString name = feedpipe::newName(QStringLiteral("test"));
        const QByteArray token = feedpipe::newToken();
        FeedPublisher pub(name, token);
        CHECK(pub.start());
        pub.setInfo(QJsonObject{{QStringLiteral("type"), QStringLiteral("info")}});

        Received got;
        FeedSubscriber guest(name, token, 2);
        guest.setFrameCallback(
            [&got](const mw::native::feed::Header& h, const uint8_t* data, size_t size) {
                std::lock_guard<std::mutex> lock(got.mutex);
                got.numbers.push_back(h.frameNumber);
                got.keyframes.push_back(h.keyframe);
                got.present.push_back(h.presentUs);
                got.sizes.push_back(size);
                for (size_t i = 0; i < size; ++i)
                    if (data[i] != static_cast<uint8_t>(h.frameNumber + i))
                        got.payloadIntact = false;
            });
        QJsonObject info;
        CHECK(guest.start(3000, &info, nullptr));
        CHECK(waitFor([&]() { return pub.subscribers() == 1; }));

        for (uint32_t n = 0; n < 50; ++n) {
            std::vector<uint8_t> frame(1000 + n * 37);
            for (size_t i = 0; i < frame.size(); ++i)
                frame[i] = static_cast<uint8_t>(n + i);
            FeedPublisher::Stamps s;
            s.frameNumber = n;
            s.presentUs = 1000000 + n * 16667;
            s.capturedUs = s.presentUs + 100;
            s.submittedUs = s.capturedUs;
            s.convertedUs = s.submittedUs + 200;
            s.encodedUs = s.convertedUs + 3000;
            pub.publishFrame(frame.data(), frame.size(), n % 25 == 0, s);
            // The pace of a stream, so no picture has a reason to be dropped.
            QThread::msleep(2);
        }
        CHECK(waitFor([&]() {
            std::lock_guard<std::mutex> lock(got.mutex);
            return got.numbers.size() == 50;
        }));
        {
            std::lock_guard<std::mutex> lock(got.mutex);
            bool ordered = true;
            for (size_t i = 0; i < got.numbers.size(); ++i)
                if (got.numbers[i] != i) ordered = false;
            CHECK(ordered);
            CHECK(got.payloadIntact);
            CHECK(got.keyframes.size() == 50 && got.keyframes[0] && got.keyframes[25] &&
                  !got.keyframes[1]);
            CHECK(got.present.size() == 50 && got.present[3] == 1000000 + 3 * 16667);
            CHECK(got.sizes.size() == 50 && got.sizes[49] == 1000 + 49 * 37);
        }
        CHECK_EQ(guest.frames(), int64_t(50));
        CHECK_EQ(pub.dropped(), int64_t(0));
        guest.stop();
        pub.stop();
    }

    SECTION("Feed pipe — control messages both ways: idr up, cursor down");
    {
        const QString name = feedpipe::newName(QStringLiteral("test"));
        const QByteArray token = feedpipe::newToken();
        FeedPublisher pub(name, token);
        CHECK(pub.start());
        pub.setInfo(QJsonObject{{QStringLiteral("type"), QStringLiteral("info")}});

        std::mutex m;
        QString upType;
        int upFrom = -1;
        QObject::connect(
            &pub, &FeedPublisher::controlReceived, &pub,
            [&](int id, const QJsonObject& msg) {
                std::lock_guard<std::mutex> lock(m);
                upType = msg.value(QStringLiteral("type")).toString();
                upFrom = id;
            },
            Qt::DirectConnection);
        FeedSubscriber guest(name, token, 4);
        QString downType;
        QObject::connect(
            &guest, &FeedSubscriber::controlReceived, &guest,
            [&](const QJsonObject& msg) {
                std::lock_guard<std::mutex> lock(m);
                downType = msg.value(QStringLiteral("type")).toString();
            },
            Qt::DirectConnection);
        CHECK(guest.start(3000, nullptr, nullptr));
        CHECK(waitFor([&]() { return pub.subscribers() == 1; }));

        guest.sendControl(QJsonObject{{QStringLiteral("type"), QStringLiteral("idr")}});
        CHECK(waitFor([&]() {
            std::lock_guard<std::mutex> lock(m);
            return upType == QLatin1String("idr");
        }));
        CHECK(upFrom > 0);
        pub.publishControl(QJsonObject{{QStringLiteral("type"), QStringLiteral("cursor")}});
        CHECK(waitFor([&]() {
            std::lock_guard<std::mutex> lock(m);
            return downType == QLatin1String("cursor");
        }));
        guest.stop();
        pub.stop();
    }

    SECTION("Feed pipe — a subscriber that stops reading never holds the feed back");
    {
        const QString name = feedpipe::newName(QStringLiteral("test"));
        const QByteArray token = feedpipe::newToken();
        FeedPublisher pub(name, token);
        CHECK(pub.start());
        pub.setInfo(QJsonObject{{QStringLiteral("type"), QStringLiteral("info")}});

        // A raw client on this thread: it says hello, then reads no more than
        // 64 KB — its buffer full, the pipe behind it fills up too.
        QLocalSocket stuck;
        stuck.setReadBufferSize(64 * 1024);
        stuck.connectToServer(name);
        CHECK(stuck.waitForConnected(2000));
        {
            const QByteArray json = QByteArrayLiteral("{\"type\":\"hello\",\"token\":\"") + token +
                                    QByteArrayLiteral("\",\"slot\":3}");
            mw::native::feed::Header h;
            h.kind = mw::native::feed::Kind::Control;
            const std::vector<uint8_t> hello =
                mw::native::feed::message(h, reinterpret_cast<const uint8_t*>(json.constData()),
                                          static_cast<size_t>(json.size()));
            stuck.write(reinterpret_cast<const char*>(hello.data()),
                        static_cast<qint64>(hello.size()));
            CHECK(stuck.waitForBytesWritten(2000));
        }
        CHECK(waitFor([&]() { return pub.subscribers() == 1; }));

        // 1 MB pictures, as fast as they come: far more than any pipe holds.
        std::vector<uint8_t> big(1024 * 1024, 0x42);
        QElapsedTimer t;
        t.start();
        for (uint32_t n = 0; n < 120; ++n) {
            FeedPublisher::Stamps s;
            s.frameNumber = n;
            pub.publishFrame(big.data(), big.size(), n == 0, s);
        }
        // Each call is a copy, never a wait on the pipe.
        CHECK(t.elapsed() < 2000);
        CHECK(waitFor([&]() { return pub.dropped() > 0; }, 5000));
        CHECK_EQ(pub.subscribers(), 1);
        stuck.abort();
        CHECK(waitFor([&]() { return pub.subscribers() == 0; }));
        pub.stop();
    }
}
