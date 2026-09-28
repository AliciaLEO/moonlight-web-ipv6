/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "encode/H264EncodeNegotiation.h"
#include "encode/H264SliceParser.h"
#include "native_test_framework.h"

#include <string>
#include <vector>

using namespace mw::native::encode;

namespace {

/// A driver's H.264, as the negotiation sees it.
struct Gpu : H264DriverQueries
{
    H264DriverLimits caps{64, 64, 4096, 4096, 1, 16};
    H264ConfigAnswer config{true, true, true, true};
    bool reconfigurable = true;
    bool capRefused = false; ///< CBR's frame size cap refused
    bool cqpRefused = false;
    uint32_t intraRefreshMost = 0;
    int level = 42;

    std::vector<H264SupportQuestion> asked;

    H264DriverLimits limits() override { return caps; }
    H264ConfigAnswer configuration() override { return config; }
    HevcSupportAnswer support(const H264SupportQuestion& q) override
    {
        asked.push_back(q);
        HevcSupportAnswer a;
        a.ok = !(q.rate.mode == HevcRate::Mode::Cbr && q.rate.frameSizeCap && capRefused) &&
               !(q.rate.mode == HevcRate::Mode::Cqp && cqpRefused) && q.cabac == config.cabac &&
               q.transform8x8 == config.transform8x8;
        a.rateReconfigurable = reconfigurable;
        a.suggestedLevelIdc = level;
        a.maxIntraRefreshFrames = intraRefreshMost;
        return a;
    }
};

H264EncodeRequest request1080p()
{
    H264EncodeRequest r;
    r.width = 1920;
    r.height = 1080;
    r.fps = 60;
    return r;
}

bool contains(const std::string& text, const std::string& piece)
{
    return text.find(piece) != std::string::npos;
}

} // namespace

// What D3D12 Video Encode's H.264 asks of a driver, then less (plan C9.1):
// CABAC and the 8×8 transform where the driver codes them, whole macroblocks
// cropped back to the picture, HEVC's rate ladder and intra-refresh rule, the
// driver's level or 5.1 — and a sequence whose parameter sets say it.
void run_h264_negotiation_tests()
{
    SECTION("H264EncodeNegotiation — CABAC, 8x8, 1080 coded as 1088, the driver's level");
    {
        Gpu g;
        const H264EncodeSetup s = negotiateH264(request1080p(), g);
        CHECK(s.ok);
        CHECK(s.reason.empty());
        CHECK(s.cabac);
        CHECK(s.transform8x8);
        CHECK_EQ(s.codedWidth, 1920u);
        CHECK_EQ(s.codedHeight, 1088u);
        CHECK_EQ(s.rate.describe(), std::string("CBR1 + VBV + QP range + frame size cap"));
        CHECK_EQ(s.intraRefreshFrames, 0);
        const paramsets::H264Sequence& q = s.sequence;
        CHECK_EQ(q.profileIdc, 100);
        CHECK_EQ(q.levelIdc, 42);
        CHECK_EQ(q.widthMbs, 120u);
        CHECK_EQ(q.heightMbs, 68u);
        CHECK_EQ(q.cropRight, 0u);
        CHECK_EQ(q.cropBottom, 4u);
        CHECK_EQ(q.maxRefFrames, 1u);
        CHECK_EQ(q.log2MaxFrameNum, 16);
        CHECK(q.cabac);
        CHECK(q.transform8x8);

        // Its parameter sets read back as negotiated.
        const std::vector<uint8_t> sps = paramsets::h264Sps(q);
        const std::vector<uint8_t> pps = paramsets::h264Pps(q);
        H264SpsFields sf;
        H264PpsFields pf;
        CHECK(parseH264Sps(sps.data(), sps.size(), sf).empty());
        CHECK(parseH264Pps(pps.data(), pps.size(), pf).empty());
        CHECK_EQ(sf.heightMbs, 68u);
        CHECK_EQ(sf.maxRefFrames, 1u);
        CHECK(pf.cabac);
        CHECK(pf.transform8x8);
    }

    SECTION("H264EncodeNegotiation — a driver without CABAC or 8x8: CAVLC, said in the PPS");
    {
        Gpu g;
        g.config.cabac = false;
        g.config.transform8x8 = false;
        const H264EncodeSetup s = negotiateH264(request1080p(), g);
        CHECK(s.ok);
        CHECK(!s.cabac);
        CHECK(!s.transform8x8);
        CHECK(contains(s.reason, "no CABAC: CAVLC"));
        CHECK(!g.asked.empty() && !g.asked.front().cabac);
        const std::vector<uint8_t> pps = paramsets::h264Pps(s.sequence);
        H264PpsFields pf;
        CHECK(parseH264Pps(pps.data(), pps.size(), pf).empty());
        CHECK(!pf.cabac);
        CHECK(!pf.transform8x8);
    }

    SECTION("H264EncodeNegotiation — refused: an odd size, a size out of reach, no configuration, "
            "edges left unfiltered");
    {
        Gpu g;
        H264EncodeRequest odd = request1080p();
        odd.width = 1921;
        CHECK(!negotiateH264(odd, g).ok);
        CHECK(contains(negotiateH264(odd, g).reason, "4:2:0 wants it even"));

        H264EncodeRequest huge = request1080p();
        huge.width = 7680;
        huge.height = 4320;
        CHECK(contains(negotiateH264(huge, g).reason, "outside what the driver encodes"));

        g.config.taken = false;
        CHECK(contains(negotiateH264(request1080p(), g).reason, "no H.264 configuration"));
        g.config.taken = true;
        g.config.deblockingAllEdges = false;
        const H264EncodeSetup s = negotiateH264(request1080p(), g);
        CHECK(!s.ok);
        CHECK(contains(s.reason, "disable_deblocking_filter_idc 0"));
    }

    SECTION("H264EncodeNegotiation — the rate ladder: down a rung, and said");
    {
        Gpu g;
        g.capRefused = true;
        H264EncodeSetup s = negotiateH264(request1080p(), g);
        CHECK(s.ok);
        CHECK(!s.rate.frameSizeCap);
        CHECK(contains(s.reason, "rate control"));

        g.reconfigurable = false;
        s = negotiateH264(request1080p(), g);
        CHECK(contains(s.reason, "the bitrate cannot change without a new sequence"));

        // Our own rate control: CQP, the richest first.
        H264EncodeRequest own = request1080p();
        own.ownRateControl = true;
        s = negotiateH264(own, g);
        CHECK(s.ok);
        CHECK(s.rate.mode == HevcRate::Mode::Cqp);
        CHECK(!contains(s.reason, "new sequence")); // ours moves without one

        g.cqpRefused = true;
        s = negotiateH264(own, g);
        CHECK(!s.ok);
        CHECK(contains(s.reason, "down to plain CQP"));
    }

    SECTION("H264EncodeNegotiation — intra refresh where the driver sweeps long enough");
    {
        Gpu g;
        H264EncodeRequest r = request1080p();
        r.intraRefresh = true;
        g.intraRefreshMost = 480;
        H264EncodeSetup s = negotiateH264(r, g);
        CHECK(s.ok);
        CHECK_EQ(s.intraRefreshFrames, intraRefreshPeriodFrames(60));
        CHECK(!g.asked.empty() && g.asked.front().intraRefresh);

        g.intraRefreshMost = 1; // the Arc's HEVC answer
        s = negotiateH264(r, g);
        CHECK(s.ok);
        CHECK_EQ(s.intraRefreshFrames, 0);
        CHECK(contains(s.reason, "keyframes on demand"));
    }

    SECTION("H264EncodeNegotiation — no level suggested: 5.1");
    {
        Gpu g;
        g.level = 0;
        const H264EncodeSetup s = negotiateH264(request1080p(), g);
        CHECK(s.ok);
        CHECK_EQ(s.sequence.levelIdc, h264DefaultLevelIdc());
        CHECK_EQ(h264DefaultLevelIdc(), 51);
    }
}
