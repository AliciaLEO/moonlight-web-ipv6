/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "encode/Av1EncodeNegotiation.h"
#include "encode/Av1Obu.h"
#include "native_test_framework.h"

#include <string>
#include <vector>

using namespace mw::native::encode;

namespace {

av1::Sequence sequence1080p()
{
    av1::Sequence s;
    s.width = 1920;
    s.height = 1080;
    s.levelIdx = 9; // 4.1
    s.orderHintBits = 8;
    s.cdef = true;
    return s;
}

/// A driver's AV1, as the negotiation sees it: the RTX 5060 Ti's answers by
/// default (mw-d3d12-lab caps, 28/09/2026).
struct Gpu : Av1DriverQueries
{
    Av1DriverLimits caps{192, 128, 8192, 8192, 2, 2, true};
    Av1ConfigAnswer config{true,
                           av1feature::kRestoration | av1feature::kCdef | av1feature::kOrderHint |
                               0x10000 | 0x80000 | 0x1000000,
                           av1feature::kRestoration | av1feature::kCdef | 0x10000,
                           0x1 | 0x2 | 0x4 | 0x10 | 0x20 | 0x40 | 0x80,
                           0x1 | 0x2 | 0x10,
                           0x4,
                           0x4,
                           true};
    bool reconfigurable = true;
    uint32_t intraRefreshMost = 0;
    std::vector<Av1SupportQuestion> asked;

    Av1DriverLimits limits(bool) override { return caps; }
    Av1ConfigAnswer configuration() override { return config; }
    HevcSupportAnswer support(const Av1SupportQuestion& q) override
    {
        asked.push_back(q);
        HevcSupportAnswer a;
        a.ok = true;
        a.rateReconfigurable = reconfigurable;
        a.suggestedLevelIdc = 16; // the Arc's 6.0, which the stream does not need
        a.maxIntraRefreshFrames = intraRefreshMost;
        return a;
    }
};

bool contains(const std::string& text, const std::string& piece)
{
    return text.find(piece) != std::string::npos;
}

} // namespace

// The AV1 headers written for D3D12 Video Encode's tiles (plan C9.2): the
// low-overhead OBU framing, a sequence header that reads back field by field,
// frame headers whose first fields read back, the single-tile bound, the
// level from the picture's size and rate, and the negotiation against the two
// drivers of DualRTX.
void run_av1_obu_tests()
{
    SECTION("Av1Obu — OBU framing: header byte, leb128 size, the temporal delimiter");
    {
        CHECK(av1::temporalDelimiter() == std::vector<uint8_t>({0x12, 0x00}));
        CHECK(av1::detail::leb128(0) == std::vector<uint8_t>({0x00}));
        CHECK(av1::detail::leb128(127) == std::vector<uint8_t>({0x7F}));
        CHECK(av1::detail::leb128(128) == std::vector<uint8_t>({0x80, 0x01}));
        CHECK(av1::detail::leb128(300000) == std::vector<uint8_t>({0xE0, 0xA7, 0x12}));
        const std::vector<uint8_t> h = av1::detail::obuHeader(av1::ObuType::Frame, 200);
        CHECK(h == std::vector<uint8_t>({0x32, 0xC8, 0x01}));
    }

    SECTION("Av1Obu — the sequence header reads back: 1080p 8-bit BT.709, 2160p 10-bit PQ");
    {
        av1::Sequence s = sequence1080p();
        s.restoration = true;
        std::vector<uint8_t> bytes = av1::sequenceHeader(s);
        std::vector<av1::Obu> units = av1::obus(bytes.data(), bytes.size());
        CHECK_EQ(units.size(), static_cast<size_t>(1));
        av1::Sequence back;
        if (!units.empty()) {
            CHECK(units[0].type == av1::ObuType::SequenceHeader);
            CHECK(av1::parseSequenceHeader(units[0].payload, units[0].size, back).empty());
        }
        CHECK_EQ(back.width, 1920u);
        CHECK_EQ(back.height, 1080u);
        CHECK_EQ(back.levelIdx, 9);
        CHECK_EQ(back.orderHintBits, 8);
        CHECK(back.cdef);
        CHECK(back.restoration);
        CHECK(!back.tenBit);
        CHECK(!back.hdr);
        CHECK(back.separateUvDeltaQ);

        s.width = 3840;
        s.height = 2160;
        s.levelIdx = 13;
        s.tenBit = true;
        s.hdr = true;
        s.orderHintBits = 0;
        s.restoration = false;
        bytes = av1::sequenceHeader(s);
        units = av1::obus(bytes.data(), bytes.size());
        if (!units.empty())
            CHECK(av1::parseSequenceHeader(units[0].payload, units[0].size, back).empty());
        CHECK_EQ(back.width, 3840u);
        CHECK_EQ(back.height, 2160u);
        CHECK_EQ(back.levelIdx, 13);
        CHECK_EQ(back.orderHintBits, 0);
        CHECK(back.tenBit);
        CHECK(back.hdr);
    }

    SECTION("Av1Obu — the sequence headers dav1d decoded the Arc's and the RTX's tiles with "
            "(goldens)");
    {
        // 1080p60, level 4.1, order hints, CDEF (C9.2, 28/09/2026): the Arc's.
        av1::Sequence arc = sequence1080p();
        CHECK(av1::sequenceHeader(arc) ==
              std::vector<uint8_t>({0x0A, 0x0E, 0x00, 0x00, 0x00, 0x4A, 0xAB, 0xBF, 0xC3, 0x70,
                                    0x08, 0x74, 0x40, 0x40, 0x40, 0x45}));
        // The RTX's: loop restoration too, which its driver requires.
        av1::Sequence rtx = arc;
        rtx.restoration = true;
        CHECK(av1::sequenceHeader(rtx) ==
              std::vector<uint8_t>({0x0A, 0x0E, 0x00, 0x00, 0x00, 0x4A, 0xAB, 0xBF, 0xC3, 0x70,
                                    0x08, 0x76, 0x40, 0x40, 0x40, 0x45}));
    }

    SECTION("Av1Obu — frame headers: a key frame, then an inter frame naming slot 0");
    {
        const av1::Sequence s = sequence1080p();
        av1::Frame key;
        key.q.baseQIdx = 120;
        key.lf.level = {10, 8, 4, 4};
        key.cdef.bits = 1;
        key.cdef.yPri = {4, 2};
        const std::vector<uint8_t> k = av1::frameObuPrefix(s, key, 5000);
        std::vector<av1::Obu> units = av1::obus(k.data(), k.size());
        // The size counts the tile that follows: the header alone is shorter.
        CHECK(units.empty());
        CHECK_EQ(k[0], 0x32);
        const std::vector<uint8_t> bits = av1::frameHeaderBits(s, key);
        CHECK(bits.size() < 40);
        av1::FrameStart f;
        CHECK(av1::parseFrameStart(bits.data(), bits.size(), s, f).empty());
        CHECK_EQ(f.frameType, 0);
        CHECK(f.showFrame);
        CHECK(f.errorResilient);
        CHECK_EQ(f.refreshFrameFlags, 0xFF);

        av1::Frame inter = key;
        inter.key = false;
        inter.orderHint = 261; // written modulo 2^8
        inter.primaryRefFrame = 0;
        inter.refreshFrameFlags = 0xFF;
        const std::vector<uint8_t> ib = av1::frameHeaderBits(s, inter);
        CHECK(av1::parseFrameStart(ib.data(), ib.size(), s, f).empty());
        CHECK_EQ(f.frameType, 1);
        CHECK(!f.errorResilient);
        CHECK_EQ(f.orderHint, 5u);
        CHECK_EQ(f.primaryRefFrame, 0);
        CHECK_EQ(f.refreshFrameFlags, 0xFF);
        for (int i = 0; i < av1::kRefsPerFrame; ++i)
            CHECK_EQ(f.refFrameIdx[static_cast<size_t>(i)], 0);

        // Segmentation and every delta still fit the room left in front of a
        // tile.
        av1::Frame worst = inter;
        worst.seg.enabled = true;
        worst.seg.updateMap = worst.seg.updateData = true;
        for (auto& features : worst.seg.features)
            features = 0xFF;
        worst.lf.deltaEnabled = worst.lf.updateRefDelta = worst.lf.updateModeDelta = true;
        worst.cdef.bits = 3;
        worst.q.deltaQUDc = 3;
        worst.q.deltaQVDc = -3;
        worst.q.usingQmatrix = true;
        worst.deltaQPresent = worst.deltaLfPresent = true;
        CHECK(av1::frameObuPrefix(s, worst, 1u << 20).size() + av1::sequenceHeader(s).size() + 2 <
              512);
    }

    SECTION("Av1Obu — one tile holds up to 4096 wide and 4096x2304 in area");
    {
        av1::Sequence s;
        const auto refused = [&](uint32_t w, uint32_t h) {
            s.width = w;
            s.height = h;
            return !av1::singleTileRefusal(s).empty();
        };
        CHECK(!refused(1920, 1080));
        CHECK(!refused(3840, 2160));
        CHECK(!refused(4096, 2304));
        CHECK(refused(4160, 1080));
        CHECK(refused(4096, 2400));
    }

    SECTION("Av1EncodeNegotiation — the level the picture needs, not the driver's");
    {
        CHECK_EQ(av1LevelIdx(1920, 1080, 60), 9);   // 4.1
        CHECK_EQ(av1LevelIdx(1920, 1080, 120), 12); // 5.0
        CHECK_EQ(av1LevelIdx(2560, 1440, 120), 13); // 5.1
        CHECK_EQ(av1LevelIdx(3840, 2160, 60), 13);  // 5.1
        CHECK_EQ(av1LevelIdx(1280, 720, 60), 8);    // 4.0
    }

    SECTION("Av1EncodeNegotiation — the RTX: what it requires, CDEF and order hints, one tile");
    {
        Gpu g;
        Av1EncodeRequest r;
        r.width = 1920;
        r.height = 1080;
        r.fps = 60;
        const Av1EncodeSetup s = negotiateAv1(r, g);
        CHECK(s.ok);
        CHECK_EQ(s.codedWidth, 1920u);
        CHECK_EQ(s.codedHeight, 1080u);
        CHECK(s.features & av1feature::kRestoration);
        CHECK(s.features & av1feature::kCdef);
        CHECK(s.features & av1feature::kOrderHint);
        CHECK(!(s.features & 0x80000)); // q-deltas: supported, not asked for
        CHECK_EQ(s.interpolationFilter, 4);
        CHECK(s.txSelectKey);
        CHECK(s.txSelectInter);
        CHECK_EQ(s.sequence.levelIdx, 9);
        CHECK(s.sequence.cdef);
        CHECK(s.sequence.restoration);
        CHECK_EQ(s.sequence.orderHintBits, 8);
        CHECK(!g.asked.empty() && g.asked.front().features == s.features);

        // The Arc: nothing required, CDEF and order hints taken.
        Gpu arc;
        arc.config.required = 0;
        arc.config.supported =
            av1feature::kCdef | av1feature::kOrderHint | 0x20000 | 0x100000 | 0x200000;
        arc.config.postEncode = 0;
        arc.caps.widthMultiple = arc.caps.heightMultiple = 1;
        const Av1EncodeSetup a = negotiateAv1(r, arc);
        CHECK(a.ok);
        CHECK_EQ(a.features, av1feature::kCdef | av1feature::kOrderHint);
        CHECK(!a.sequence.restoration);

        // An odd size asked of a driver that wants multiples of 2 is refused
        // before it; one that requires palette coding has no encoder here.
        r.width = 1921;
        CHECK(!negotiateAv1(r, g).ok);
        r.width = 1920;
        arc.config.required = av1feature::kPalette;
        const Av1EncodeSetup p = negotiateAv1(r, arc);
        CHECK(!p.ok);
        CHECK(contains(p.reason, "cannot say"));
        arc.config.required = 0;
        arc.config.txInter = 0x1; // ONLY_4X4: a lossless frame's
        CHECK(contains(negotiateAv1(r, arc).reason, "transform mode"));
        arc.config.txInter = 0x4;
        arc.config.keyAndInterFrames = false;
        CHECK(!negotiateAv1(r, arc).ok);
    }
}
