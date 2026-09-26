/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "encode/HevcEncodeNegotiation.h"
#include "encode/HevcSliceParser.h"
#include "native_test_framework.h"

using namespace mw::native::encode;

namespace {

/// A GPU of DualRTX as its driver answered on 26/09/2026 (mw-d3d12-lab caps,
/// HEVC Main and Main 10 alike): the one block configuration it took in the
/// negotiation's order, and its support answers.
struct Gpu : HevcDriverQueries
{
    HevcDriverLimits caps;
    HevcBlocks taken;
    HevcBlocksAnswer flags;
    bool reconfigurable = true;
    bool reconArray = false;
    bool capRefused = false; ///< CBR's frame size cap refused
    uint32_t qpMapCbr = 32, qpMapCqp = 32;
    uint32_t intraRefreshMost = 0;
    uint32_t qualityVsSpeedMost = 0;
    int level = 123;

    std::vector<HevcRate> asked;
    std::vector<bool> askedRefresh;
    uint32_t askedDpb = 0;

    HevcDriverLimits limits(bool) override { return caps; }
    HevcBlocksAnswer blocks(const HevcBlocks& b, bool) override
    {
        HevcBlocksAnswer a = flags;
        a.taken = b == taken;
        return a;
    }
    HevcSupportAnswer support(const HevcSupportQuestion& q) override
    {
        asked.push_back(q.rate);
        askedRefresh.push_back(q.intraRefresh);
        askedDpb = q.dpb;
        HevcSupportAnswer a;
        a.ok = q.blocks == taken && q.blockFlags.ampRequired == flags.ampRequired &&
               !(q.rate.mode == HevcRate::Mode::Cbr && q.rate.frameSizeCap && capRefused);
        a.rateReconfigurable = reconfigurable;
        a.reconTextureArray = reconArray;
        a.suggestedLevelIdc = level;
        a.qpMapRegion = q.rate.mode == HevcRate::Mode::Cbr ? qpMapCbr : qpMapCqp;
        a.maxIntraRefreshFrames = intraRefreshMost;
        a.maxQualityVsSpeed = qualityVsSpeedMost;
        return a;
    }
};

Gpu rtx()
{
    Gpu g;
    g.caps = {160, 64, 8192, 8192, 8, 8, true};
    g.taken = {3, 5, 2, 5, 3};
    g.flags.ampRequired = true;
    g.capRefused = true;
    g.qualityVsSpeedMost = 6;
    return g;
}

Gpu amd()
{
    Gpu g;
    g.caps = {128, 128, 8192, 4352, 1, 16, true};
    g.taken = {3, 6, 2, 5, 4};
    g.reconArray = true;
    g.qpMapCbr = g.qpMapCqp = 64;
    g.intraRefreshMost = 480;
    g.qualityVsSpeedMost = 3;
    return g;
}

Gpu arc()
{
    Gpu g;
    g.caps = {64, 64, 16384, 16384, 3, 15, true};
    g.taken = {3, 6, 2, 5, 2};
    g.flags.ampRequired = true;
    g.flags.pAsLowDelayB = true;
    g.reconfigurable = false;
    g.qpMapCqp = 16;
    g.intraRefreshMost = 1;
    g.qualityVsSpeedMost = 7;
    return g;
}

HevcEncodeRequest request1080p()
{
    HevcEncodeRequest r;
    r.width = 1920;
    r.height = 1080;
    r.fps = 60;
    return r;
}

} // namespace

void run_hevc_negotiation_tests()
{
    SECTION("HevcEncodeNegotiation — RTX: 8..32, depth 3, AMP; CBR without the frame size cap");
    {
        Gpu g = rtx();
        const HevcEncodeSetup s = negotiateHevc(request1080p(), g);
        CHECK(s.ok);
        CHECK(s.blocks == HevcBlocks({3, 5, 2, 5, 3}));
        CHECK(s.rate.describe() == "CBR1 + VBV + QP range");
        CHECK(s.support.rateReconfigurable);
        CHECK_EQ(s.codedWidth, 1920u);
        CHECK_EQ(s.codedHeight, 1088u);
        CHECK_EQ(s.dpbCapacity, 4);
        CHECK_EQ(g.askedDpb, 4u);
        CHECK(s.reason.find("frame size cap") != std::string::npos);
        // What the parameter sets say is the configuration of 26/09, with the
        // product's four pictures kept.
        const paramsets::HevcSequence& q = s.sequence;
        CHECK(q.dialect == paramsets::HevcDialect::D3d12);
        CHECK_EQ(q.levelIdc, 123);
        CHECK_EQ(q.log2MaxCodingBlock, 5);
        CHECK_EQ(q.transformDepthInter, 3);
        CHECK(q.asymmetricMotionPartitions);
        CHECK_EQ(q.maxReferences, 4u);
        CHECK_EQ(q.log2MaxPocLsb, 8);
    }

    SECTION("HevcEncodeNegotiation — AMD iGPU: every CBR option, AMP left off, recon in an array");
    {
        Gpu g = amd();
        const HevcEncodeSetup s = negotiateHevc(request1080p(), g);
        CHECK(s.ok);
        CHECK(s.blocks == HevcBlocks({3, 6, 2, 5, 4}));
        CHECK(!s.sequence.asymmetricMotionPartitions);
        CHECK(s.rate.describe() == "CBR1 + VBV + QP range + frame size cap");
        CHECK(s.reason.empty());
        CHECK(s.support.reconTextureArray);
        CHECK_EQ(s.support.qpMapRegion, 64u);
        CHECK_EQ(g.asked.size(), size_t(1));
    }

    SECTION("HevcEncodeNegotiation — Arc: P as low-delay B, and no bitrate change in flight");
    {
        Gpu g = arc();
        const HevcEncodeSetup s = negotiateHevc(request1080p(), g);
        CHECK(s.ok);
        CHECK(s.blocks == HevcBlocks({3, 6, 2, 5, 2}));
        CHECK(s.blocksAnswer.pAsLowDelayB);
        CHECK(s.sequence.asymmetricMotionPartitions);
        CHECK(!s.support.rateReconfigurable);
        CHECK(s.reason.find("without a new sequence") != std::string::npos);
        // With our own controller: CQP, whose QP the Arc follows picture by
        // picture, and its finer QP map.
        HevcEncodeRequest own = request1080p();
        own.ownRateControl = true;
        Gpu g2 = arc();
        const HevcEncodeSetup c = negotiateHevc(own, g2);
        CHECK(c.ok);
        CHECK(c.rate.describe() == "CQP1");
        CHECK_EQ(c.support.qpMapRegion, 16u);
        CHECK(c.reason.empty());
    }

    SECTION("HevcEncodeNegotiation — the D3D12 dialect's SPS for each, read back");
    {
        Gpu gpus[] = {rtx(), amd(), arc()};
        for (Gpu& g : gpus) {
            const HevcEncodeSetup s = negotiateHevc(request1080p(), g);
            const auto sps = paramsets::hevcSps(s.sequence);
            HevcSpsFields f;
            CHECK(parseHevcSps(sps.data(), sps.size(), f).empty());
            CHECK_EQ(f.width, 1920u);
            CHECK_EQ(f.height, 1088u);
            CHECK_EQ(f.cropBottom, 4u);
            CHECK_EQ(f.maxDecPicBufferingMinus1, 4u);
            CHECK_EQ(f.log2CodingTreeBlock, s.blocks.log2MaxCodingBlock);
            CHECK_EQ(f.asymmetricMotionPartitions, s.blocksAnswer.ampRequired);
        }
    }

    SECTION("HevcEncodeNegotiation — intra refresh only over a real sweep");
    {
        HevcEncodeRequest r = request1080p();
        r.intraRefresh = true;
        Gpu a = amd();
        const HevcEncodeSetup s = negotiateHevc(r, a);
        CHECK(s.ok);
        CHECK_EQ(s.intraRefreshFrames, 120); // two seconds at 60
        CHECK(a.askedRefresh.front());
        r.fps = 300;
        Gpu a2 = amd();
        CHECK_EQ(negotiateHevc(r, a2).intraRefreshFrames, 480); // the driver's most
        // The Arc sweeps one frame at most, the RTX none: keyframes on demand,
        // and the rate control asked again without it.
        r.fps = 60;
        Gpu b = arc();
        const HevcEncodeSetup t = negotiateHevc(r, b);
        CHECK(t.ok);
        CHECK_EQ(t.intraRefreshFrames, 0);
        CHECK(t.reason.find("intra refresh") != std::string::npos);
        CHECK(!b.askedRefresh.back());
        Gpu c = rtx();
        CHECK_EQ(negotiateHevc(r, c).intraRefreshFrames, 0);
    }

    SECTION("HevcEncodeNegotiation — the DPB: the driver's and the level's limits");
    {
        HevcEncodeRequest r = request1080p();
        r.dpbFrames = 12;
        Gpu g = rtx(); // a DPB of 8
        const HevcEncodeSetup s = negotiateHevc(r, g);
        CHECK_EQ(g.askedDpb, 8u);
        // Level 4.1 holds 6 pictures of 1080p, the current one included.
        CHECK_EQ(s.dpbCapacity, 5);
        CHECK(s.reason.find("level") != std::string::npos);
        // Where the level allows more, the driver's DPB is the limit.
        Gpu a = amd(); // a DPB of 16
        a.level = 153;
        CHECK_EQ(negotiateHevc(r, a).dpbCapacity, 12);
        CHECK_EQ(a.askedDpb, 12u);
        CHECK_EQ(hevcMaxDpbSize(123, 1920u * 1088u), 6);
        CHECK_EQ(hevcMaxDpbSize(153, 1920u * 1088u), 16);
        CHECK_EQ(hevcMaxDpbSize(153, 3840u * 2160u), 6);
        CHECK_EQ(hevcMaxDpbSize(150, 2560u * 1440u), 12);
    }

    SECTION("HevcEncodeNegotiation — Main 10, sizes, and what is refused outright");
    {
        HevcEncodeRequest r = request1080p();
        r.hdr = true;
        Gpu g = rtx();
        const HevcEncodeSetup s = negotiateHevc(r, g);
        CHECK(s.ok);
        CHECK(s.sequence.tenBit);
        CHECK(s.sequence.hdr);
        Gpu none = rtx();
        none.caps.main10 = false;
        CHECK(!negotiateHevc(r, none).ok);

        HevcEncodeRequest odd = request1080p();
        odd.width = 1365;
        Gpu o = rtx();
        CHECK(!negotiateHevc(odd, o).ok);

        HevcEncodeRequest small = request1080p();
        small.width = 1366;
        small.height = 768;
        Gpu w = rtx();
        const HevcEncodeSetup m = negotiateHevc(small, w);
        CHECK(m.ok);
        CHECK_EQ(m.codedWidth, 1376u); // 16-aligned; the SPS crops 5 chroma columns
        CHECK_EQ(m.codedHeight, 768u);

        HevcEncodeRequest huge = request1080p();
        huge.width = 8192;
        huge.height = 8192;
        Gpu a = amd(); // 4352 lines at most
        const HevcEncodeSetup h = negotiateHevc(huge, a);
        CHECK(!h.ok);
        CHECK(h.reason.find("outside") != std::string::npos);

        Gpu refuses = rtx();
        refuses.taken = {3, 3, 2, 2, 9}; // a depth no proposal has
        const HevcEncodeSetup n = negotiateHevc(request1080p(), refuses);
        CHECK(!n.ok);
        CHECK(n.reason.find("block") != std::string::npos);
    }
}
