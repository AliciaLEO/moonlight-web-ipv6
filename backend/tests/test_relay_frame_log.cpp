/*
 * MoonlightWeb — Backend TNR. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * The bench's frame log of the relay (RelayFrameLog.h, `relaylog=1`; plan
 * « Wi-Fi : la vidéo qui attend dans SCTP », W1).
 *
 * What a pass relies on: a decision lands on the frame it names and on no
 * other, the sender's stamps find their frame and still reach the engine's own
 * sink, the share of time the buffer held more than 1, 2 and 4 frames is put on
 * the level each sample saw, and the CSV holds one line per frame kept.
 */

#include "../src/streaming/RelayFrameLog.h"

#include "test_framework.h"

#include <cstdio>
#include <fstream>
#include <string>

namespace {

using Outcome = RelayFrameLog::Outcome;

struct CountingSink : FrameSentSink
{
    int calls = 0;
    uint32_t last = 0;
    void frameSent(uint32_t frameNumber, int64_t, int64_t) override
    {
        ++calls;
        last = frameNumber;
    }
};

size_t g_Probe = 0;
size_t probe(void*)
{
    return g_Probe;
}

} // namespace

void run_relay_frame_log_tests()
{
    SECTION("RelayFrameLog — a frame's decisions, its send, the engine's sink still told");
    {
        RelayFrameLog log(16);
        CountingSink inner;
        log.setInner(&inner);
        log.setBufferedProbe(&probe, nullptr);

        log.begin(7, false, 40000, 1'000'000, 1'002'000, {3, 2, 0}, 9);
        log.decide(7, Outcome::Sent, 1200, 0, 1'002'010);
        log.sent(7, 101);
        g_Probe = 5000;
        log.frameSent(7, 1'002'100, 1'002'400);

        const auto rs = log.records();
        CHECK_EQ(rs.size(), size_t(1));
        CHECK(rs[0].outcome == Outcome::Sent);
        CHECK_EQ(rs[0].wireId, int64_t(101));
        CHECK_EQ(rs[0].captureUs, int64_t(1'000'000));
        CHECK_EQ(rs[0].firstUs, int64_t(1'002'100));
        CHECK_EQ(rs[0].lastUs, int64_t(1'002'400));
        CHECK_EQ(rs[0].bufferedAfter, int64_t(5000));
        CHECK_EQ(rs[0].buffered, uint32_t(1200));
        CHECK_EQ(rs[0].sctp.retrans, uint32_t(3));
        CHECK_EQ(rs[0].srttMs, 9);
        CHECK_EQ(inner.calls, 1);
        CHECK_EQ(inner.last, uint32_t(7));
    }

    SECTION("RelayFrameLog — a call about another frame leaves the record alone");
    {
        RelayFrameLog log(16);
        log.begin(8, false, 1000, 0, 0, {}, -1);
        // The keyframe kept for the channel's opening goes out under -1.
        log.decide(-1, Outcome::KeyframeDrop, 99, 0, 10);
        log.sent(-1, 5);
        const auto rs = log.records();
        CHECK(rs[0].outcome == Outcome::Pending);
        CHECK_EQ(rs[0].wireId, int64_t(-1));
    }

    SECTION("RelayFrameLog — drops, the gate and evictions are told apart");
    {
        RelayFrameLog log(16);
        log.begin(1, false, 1000, 0, 0, {}, -1);
        log.decide(1, Outcome::BacklogDrop, 300000, 260, 1);
        log.begin(2, false, 1000, 0, 0, {}, -1);
        log.decide(2, Outcome::Gated, 280000, 270, 2);
        log.begin(3, false, 1000, 0, 0, {}, -1);
        log.decide(3, Outcome::Sent, 0, 0, 3);
        log.sent(3, 1);
        log.evicted(3);
        log.begin(4, true, 9000, 0, 0, {}, -1);
        log.decide(4, Outcome::NamedDrop, 300000, 300, 4);
        const auto rs = log.records();
        CHECK(rs[0].outcome == Outcome::BacklogDrop);
        CHECK_EQ(rs[0].backlogMs, 260);
        CHECK(rs[1].outcome == Outcome::Gated);
        CHECK(rs[2].outcome == Outcome::Evicted);
        CHECK(rs[3].outcome == Outcome::NamedDrop);
        const std::string s = log.summary();
        CHECK(s.find("4 frames: 0 sent, 2 dropped by the backlog (1 named), 1 gated") == 0);
        CHECK(s.find("1 evicted") != std::string::npos);
    }

    SECTION("RelayFrameLog — the ring keeps the newest, oldest first");
    {
        RelayFrameLog log(4);
        for (int i = 0; i < 10; ++i)
            log.begin(i, false, 1000, 0, 0, {}, -1);
        const auto rs = log.records();
        CHECK_EQ(rs.size(), size_t(4));
        CHECK_EQ(rs.front().frameNumber, int64_t(6));
        CHECK_EQ(rs.back().frameNumber, int64_t(9));
        CHECK_EQ(log.count(), size_t(10));
        // A frame no longer held is not found: its stamps go to the engine only.
        CountingSink inner;
        log.setInner(&inner);
        log.frameSent(2, 1, 2);
        CHECK_EQ(inner.calls, 1);
    }

    SECTION("RelayFrameLog — the time above 1, 2 and 4 frames, on the level each sample saw");
    {
        RelayFrameLog log(64);
        // Every delta 10 000 bytes: a frame's worth is 10 000.
        log.begin(0, false, 10000, 0, 0, {}, -1);
        log.decide(0, Outcome::Sent, 0, 0, 1'000'000); // 0 frames, for 100 ms
        log.begin(1, false, 10000, 0, 0, {}, -1);
        log.decide(1, Outcome::Sent, 15000, 0, 1'100'000); // 1.5 frames, for 100 ms
        log.begin(2, false, 10000, 0, 0, {}, -1);
        log.decide(2, Outcome::Sent, 30000, 0, 1'200'000); // 3 frames, for 100 ms
        log.begin(3, false, 10000, 0, 0, {}, -1);
        log.decide(3, Outcome::Sent, 50000, 0, 1'300'000); // 5 frames, for 100 ms
        log.begin(4, false, 10000, 0, 0, {}, -1);
        log.decide(4, Outcome::Sent, 0, 0, 1'400'000);
        double a1 = 0, a2 = 0, a4 = 0;
        log.occupancy(a1, a2, a4);
        CHECK(a1 > 74.9 && a1 < 75.1);
        CHECK(a2 > 49.9 && a2 < 50.1);
        CHECK(a4 > 24.9 && a4 < 25.1);
        // A keyframe does not move a frame's worth.
        log.begin(5, true, 900000, 0, 0, {}, -1);
        CHECK(log.frameBytes() > 9999.0 && log.frameBytes() < 10001.0);
    }

    SECTION("RelayFrameLog — the CSV: a header, then one line a frame");
    {
        RelayFrameLog log(8);
        log.begin(1, true, 1000, 5, 6, {1, 1, 0}, 4);
        log.decide(1, Outcome::Sent, 0, 0, 7);
        log.sent(1, 0);
        log.begin(2, false, 500, 8, 9, {}, 4);
        log.decide(2, Outcome::Gated, 70000, 280, 10);
        const std::string path = "relay_frame_log_test.csv";
        CHECK(log.writeCsv(path));
        std::ifstream in(path);
        std::string header, a, b, extra;
        std::getline(in, header);
        std::getline(in, a);
        std::getline(in, b);
        CHECK(header == RelayFrameLog::csvHeader());
        CHECK(a == "1,0,1,1000,5,6,0,0,sent,0,-1,0,1,1,0,4");
        CHECK(b == "2,-1,0,500,8,9,0,0,gated,70000,-1,280,0,0,0,4");
        CHECK(!std::getline(in, extra));
        in.close();
        std::remove(path.c_str());
    }
}
