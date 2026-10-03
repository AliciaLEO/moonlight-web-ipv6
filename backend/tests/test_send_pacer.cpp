/*
 * MoonlightWeb — Backend TNR. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * The sender's pacing (SendPacer.h, `pace=`; plan « Wi-Fi : la vidéo qui
 * attend dans SCTP », W2 A).
 *
 * What the bench relies on: off, nothing ever waits; on, a burst leaves at
 * once and what follows leaves at the rate, never faster; a pause refills the
 * bucket to one burst and no more; a clock that goes back costs no wait.
 */

#include "../src/streaming/SendPacer.h"

#include "test_framework.h"

void run_send_pacer_tests()
{
    SECTION("SendPacer — off, nothing waits");
    {
        SendPacer p;
        CHECK(!p.active());
        CHECK_EQ(p.waitUs(16000, 0), int64_t(0));
        p.configure(0, 16384);
        CHECK(!p.active());
        for (int64_t t = 0; t < 10; ++t) {
            CHECK_EQ(p.waitUs(16000, t), int64_t(0));
            p.sent(16000, t);
        }
    }

    SECTION("SendPacer — a burst at once, then the rate");
    {
        SendPacer p;
        p.configure(1'000'000, 16384); // 1 MB/s, 16 KB at a time
        CHECK_EQ(p.waitUs(16000, 0), int64_t(0));
        p.sent(16000, 0);
        // 384 bytes left: the next 16,000 wait for 15,616 more, 15.616 ms.
        CHECK_EQ(p.waitUs(16000, 0), int64_t(15616));
        CHECK_EQ(p.waitUs(16000, 15615), int64_t(1));
        CHECK_EQ(p.waitUs(16000, 15616), int64_t(0));
        p.sent(16000, 15616);
        CHECK(p.waitUs(16000, 15616) > 15000);
    }

    SECTION("SendPacer — a chunk bigger than the burst waits for a burst, and its debt is paid");
    {
        SendPacer p;
        p.configure(1'000'000, 4096);
        CHECK_EQ(p.waitUs(16000, 0), int64_t(0));
        p.sent(16000, 0); // 4,096 - 16,000: 11,904 owed
        // Back to a full burst: 11,904 + 4,096 = 16,000 bytes of time.
        CHECK_EQ(p.waitUs(16000, 0), int64_t(16000));
        CHECK_EQ(p.waitUs(16000, 16000), int64_t(0));
    }

    SECTION("SendPacer — a pause refills one burst, and no more");
    {
        SendPacer p;
        p.configure(1'000'000, 16384);
        p.sent(16384, 0);
        CHECK_EQ(p.tokens(), int64_t(0));
        CHECK_EQ(p.waitUs(1, 500'000), int64_t(0)); // half a second: full again
        CHECK_EQ(p.tokens(), int64_t(16384));
        CHECK_EQ(p.waitUs(16384, 900'000), int64_t(0));
        p.sent(16384, 900'000);
        p.sent(16384, 900'000); // two bursts back to back: one owed
        CHECK_EQ(p.waitUs(16384, 900'000), int64_t(32768));
        // A long pause, a second or more: full, whatever was owed.
        CHECK_EQ(p.waitUs(16384, 5'000'000), int64_t(0));
        CHECK_EQ(p.tokens(), int64_t(16384));
    }

    SECTION("SendPacer — a clock that goes back costs no wait");
    {
        SendPacer p;
        p.configure(1'000'000, 16384);
        CHECK_EQ(p.waitUs(1000, 1'000'000), int64_t(0));
        CHECK_EQ(p.waitUs(1000, 10), int64_t(0));
        CHECK_EQ(p.tokens(), int64_t(16384));
    }

    SECTION("SendPacer — a 47 KB frame at four times 45 Mbit/s leaves within 1.5 ms");
    {
        // The Mac's case of 03/10/2026: 45 Mbit/s, 120 fps, ~47 KB a frame, in
        // three chunks of 16 KB.
        SendPacer p;
        const int64_t rate = 45'000'000 / 8 * 4;
        p.configure(rate, 16384);
        int64_t now = 0;
        const size_t chunks[] = {16017, 16017, 15000};
        for (size_t c : chunks) {
            now += p.waitUs(c, now);
            CHECK_EQ(p.waitUs(c, now), int64_t(0));
            p.sent(c, now);
        }
        CHECK(now > 1300 && now < 1500);
        // The next frame, 8.3 ms later: a full burst again, no wait.
        CHECK_EQ(p.waitUs(16017, now + 8333), int64_t(0));
    }
}
