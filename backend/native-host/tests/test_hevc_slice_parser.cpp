/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "encode/HevcSliceParser.h"
#include "encode/ParameterSets.h"
#include "native_test_framework.h"

#include <string>

using namespace mw::native::encode;
using namespace mw::native::encode::paramsets;

namespace {

std::vector<uint8_t> fromHex(const char* hex)
{
    std::vector<uint8_t> v;
    for (; hex[0] && hex[1]; hex += 2)
        v.push_back(static_cast<uint8_t>(std::stoi(std::string(hex, 2), nullptr, 16)));
    return v;
}

HevcSpsFields sps(const std::vector<uint8_t>& unit)
{
    HevcSpsFields f;
    const std::string error = parseHevcSps(unit.data(), unit.size(), f);
    if (!error.empty()) std::fprintf(stderr, "  SPS: %s\n", error.c_str());
    CHECK(error.empty());
    return f;
}

HevcPpsFields pps(const std::vector<uint8_t>& unit)
{
    HevcPpsFields f;
    const std::string error = parseHevcPps(unit.data(), unit.size(), f);
    if (!error.empty()) std::fprintf(stderr, "  PPS: %s\n", error.c_str());
    CHECK(error.empty());
    return f;
}

std::string readSlice(const std::vector<uint8_t>& unit, const HevcSpsFields& s,
                      const HevcPpsFields& p, HevcSliceFields& f)
{
    return parseHevcSliceHeader(unit.data(), unit.size(), s, p, f);
}

/// The first five slices of each GPU's stream on 26/09/2026 — mw-d3d12-lab
/// encode, CBR 1080p60, two pictures held — cut to their first 24 bytes: the
/// header is in the first five. With the SPS our writer gives that
/// configuration (the D3D12 goldens of test_parameter_sets.cpp, with two
/// pictures held).
struct Stream
{
    const char* gpu;
    const char* sps;
    const char* slices[5];
};

const Stream kStreams[] = {
    {"RTX 5060 Ti",
     "420101016000000300b0000003000003007ba003c0801107cb94fb9084489a8080808040",
     {"2601adf8c56293e0819415dde9d6fef61d872400b2a05f1f",
      "0201d009722bc010fd72bc5bb753d9a50ebd578725f22a63",
      "0201d011f88af010fee013e3f7fa6c8f14adc88e43d3367c",
      "0201d019f88af010fee2102e6f8de78bf326d9f8cdb11af0",
      "0201d021f888f0125ed5380ef3ea334a89a4e63014de738d"}},
    {"AMD iGPU",
     "420101016000000300b0000003000003007ba003c0801107cb94f924294226a020202010",
     {"2801afe0c72bd641cdd79bb510e277e6452011f4e5e6bf92",
      "0201d009724fd343a7d94c6b3f843aca0c3bcfc301a2ad6c",
      "0201d011f889f0ea5fd4585db4d553788984cf5dd1683a62",
      "0201d019f89bc0d5abd7bdc0263361f38dfb9144808d6739",
      "0201d021f8fcce7ff79eda1a6afd0f60775f7cd3f1075387"}},
    {"Arc A380",
     "420101016000000300b0000003000003007ba003c0801107cb94f9246d226a02020201",
     {"2601afe0d384ba9a1d0a3d28f074ddb4c6a8dd8d92aa2b9a",
      "0201e025f9f8d041fe93370ca5d6868a85c0a8f13a126462",
      "0201e047ee7e1eba244e213a02db902487906d8e1a73d0d0",
      "0201e067ee7e1eba2459cd88a07967a368314fded4c2eaf1",
      "0201e087ee7e1eba28958618cc785d79dc2c4641dae3a358"}},
};

const char* const kD3d12Pps = "4401c0f3e0cc90";

/// The configuration each stream's encoder was created with.
HevcSequence shapeOf(int gpu)
{
    HevcSequence s;
    s.dialect = HevcDialect::D3d12;
    s.width = 1920;
    s.height = 1080;
    s.codedWidth = 1920;
    s.codedHeight = 1088;
    s.levelIdc = 123;
    s.maxReferences = 2;
    s.log2MaxPocLsb = 8;
    s.log2MaxCodingBlock = gpu == 0 ? 5 : 6;
    s.transformDepthInter = s.transformDepthIntra = gpu == 0 ? 3 : (gpu == 1 ? 4 : 2);
    s.asymmetricMotionPartitions = gpu != 1;
    return s;
}

} // namespace

void run_hevc_slice_parser_tests()
{
    SECTION("HevcSliceParser — Annex-B split: start codes of 3 and 4 bytes, trailing zeros off");
    {
        const std::vector<uint8_t> stream = {0, 0, 0, 1,    0x40, 0x01, 0xAA,
                                             0, 0, 1, 0x42, 0x01, 0xBB, 0x00,
                                             0, 0, 0, 1,    0x26, 0x01, 0xCC};
        const auto units = hevcNalUnits(stream.data(), stream.size());
        CHECK_EQ(units.size(), size_t(3));
        if (units.size() == 3) {
            CHECK_EQ(int(units[0].type()), 32);
            CHECK_EQ(units[0].size, size_t(3));
            CHECK_EQ(int(units[1].type()), 33);
            CHECK_EQ(units[1].size, size_t(3)); // 0x00 then the 4-byte start code's zero, off
            CHECK_EQ(int(units[2].type()), 19);
            CHECK_EQ(units[2].size, size_t(3));
        }
    }

    SECTION("HevcSliceParser — our D3D12 parameter sets read back as they were written");
    {
        HevcSequence shape = shapeOf(2);
        const HevcSpsFields s = sps(hevcSps(shape));
        CHECK_EQ(s.width, 1920u);
        CHECK_EQ(s.height, 1088u);
        CHECK_EQ(s.cropBottom, 4u);
        CHECK_EQ(s.log2MaxPocLsb, 8);
        CHECK_EQ(s.maxDecPicBufferingMinus1, 2u);
        CHECK_EQ(s.maxNumReorderPics, 0u);
        CHECK_EQ(s.log2CodingTreeBlock, 6);
        CHECK(s.asymmetricMotionPartitions);
        CHECK(!s.sampleAdaptiveOffset);
        CHECK(!s.longTermReferences);
        CHECK(!s.temporalMvp);
        const HevcPpsFields p = pps(hevcPps(shape));
        CHECK(p.cabacInitPresent);
        CHECK(p.sliceChromaQpOffsets);
        CHECK(p.cuQpDelta);
        CHECK(!p.deblockingOverride);
        CHECK(!p.deblockingDisabled);
        CHECK(p.loopFilterAcrossSlices);
        CHECK(!p.listsModification);
        CHECK_EQ(p.defaultActiveL0, 1u);

        shape.tenBit = true;
        shape.longTermReferences = true;
        shape.sampleAdaptiveOffset = true;
        shape.defaultActiveReferences = 2;
        shape.loopFilterAcrossSlices = false;
        const HevcSpsFields s10 = sps(hevcSps(shape));
        CHECK_EQ(s10.bitDepthLuma, 10u);
        CHECK(s10.longTermReferences);
        CHECK(s10.longTermPocLsb.empty());
        CHECK(s10.sampleAdaptiveOffset);
        const HevcPpsFields p10 = pps(hevcPps(shape));
        CHECK(p10.listsModification);
        CHECK_EQ(p10.defaultActiveL0, 2u);
        CHECK(!p10.loopFilterAcrossSlices);
    }

    SECTION("HevcSliceParser — the three GPUs' slices read with the sets we send ahead of them");
    {
        for (int g = 0; g < 3; ++g) {
            const Stream& stream = kStreams[g];
            // The fixtures' SPS is what our writer gives for that encoder.
            const std::vector<uint8_t> written = hevcSps(shapeOf(g));
            CHECK(fromHex(stream.sps) == std::vector<uint8_t>(written.begin() + 4, written.end()));
            const HevcSpsFields s = sps(fromHex(stream.sps));
            const HevcPpsFields p = pps(fromHex(kD3d12Pps));
            for (int i = 0; i < 5; ++i) {
                HevcSliceFields f;
                const std::string error = readSlice(fromHex(stream.slices[i]), s, p, f);
                if (!error.empty())
                    std::fprintf(stderr, "  %s slice %d: %s\n", stream.gpu, i, error.c_str());
                CHECK(error.empty());
                CHECK_EQ(f.pocLsb, uint32_t(i));
                CHECK_EQ(f.segmentAddress, 0u);
                CHECK(!f.cabacInit);
                CHECK_EQ(f.cbQpOffset, 0);
                CHECK_EQ(f.crQpOffset, 0);
                CHECK(f.loopFilterAcrossSlices);
                CHECK(!f.deblockingDisabled);
                if (i == 0) {
                    CHECK_EQ(f.sliceType, 2u);
                    CHECK(f.shortTerm.empty());
                    CHECK_EQ(f.headerBits, size_t(16));
                    continue;
                }
                // One picture predicted from, the one before; from the third
                // on, the one before that kept — unused, but still in the set.
                CHECK_EQ(f.shortTerm.size(), size_t(i == 1 ? 1 : 2));
                CHECK_EQ(f.shortTerm[0].deltaPoc, -1);
                CHECK(f.shortTerm[0].used);
                if (i > 1) {
                    CHECK_EQ(f.shortTerm[1].deltaPoc, -2);
                    CHECK(!f.shortTerm[1].used);
                }
                CHECK_EQ(f.referencesUsed(), 1u);
                CHECK_EQ(f.activeL0, 1u);
                CHECK_EQ(f.maxMergeCandidates, 5u);
            }
        }
    }

    SECTION("HevcSliceParser — each GPU's own way: IDR types, P coded as B on the Arc");
    {
        HevcSliceFields f;
        const auto first = [&](int g, int i) {
            f = HevcSliceFields{};
            return readSlice(fromHex(kStreams[g].slices[i]), sps(fromHex(kStreams[g].sps)),
                             pps(fromHex(kD3d12Pps)), f);
        };
        CHECK(first(0, 0).empty());
        CHECK_EQ(int(f.nalType), 19); // IDR_W_RADL
        CHECK_EQ(f.qp, 25);
        CHECK(first(0, 1).empty());
        CHECK_EQ(int(f.nalType), 1); // TRAIL_R
        CHECK_EQ(f.sliceType, 1u);   // P
        CHECK_EQ(f.qp, 31);
        CHECK_EQ(f.headerBits, size_t(40));

        CHECK(first(1, 0).empty());
        CHECK_EQ(int(f.nalType), 20); // IDR_N_LP: the AMD's
        CHECK(first(1, 2).empty());
        CHECK_EQ(f.sliceType, 1u);
        CHECK_EQ(f.qp, 22);

        CHECK(first(2, 1).empty());
        CHECK_EQ(f.sliceType, 0u); // B, with L1 = L0: the Arc's P
        CHECK_EQ(f.activeL0, 1u);
        CHECK_EQ(f.activeL1, 1u);
        CHECK(!f.mvdL1Zero);
        CHECK_EQ(f.qp, 26);
        CHECK_EQ(f.headerBits, size_t(32));
    }

    SECTION("HevcSliceParser — a wrong PPS is caught on most slices, never on none");
    {
        // The Mesa dialect's PPS says no cabac_init, no per-slice chroma
        // offsets, no deblocking control: read with it, the drivers' headers
        // end off their byte_alignment() — 12 of these 15, the first P of each
        // GPU among them. Three happen to land on it.
        const HevcPpsFields mesa = pps(hevcPps(HevcSequence{}));
        const bool caught[3][5] = {
            {true, true, true, true, true},
            {false, true, false, true, true},
            {false, true, true, true, true},
        };
        int count = 0;
        for (int g = 0; g < 3; ++g) {
            const HevcSpsFields s = sps(fromHex(kStreams[g].sps));
            for (int i = 0; i < 5; ++i) {
                HevcSliceFields f;
                const bool failed = !readSlice(fromHex(kStreams[g].slices[i]), s, mesa, f).empty();
                CHECK_EQ(failed, caught[g][i]);
                count += failed ? 1 : 0;
            }
        }
        CHECK_EQ(count, 12);
    }

    SECTION("HevcSliceParser — a wrong SPS: the POC width shifts everything after it");
    {
        HevcSequence wide = shapeOf(0);
        wide.log2MaxPocLsb = 16;
        const HevcSpsFields s = sps(hevcSps(wide));
        const HevcPpsFields p = pps(fromHex(kD3d12Pps));
        int failed = 0;
        for (int i = 1; i < 5; ++i) {
            HevcSliceFields f;
            failed += readSlice(fromHex(kStreams[0].slices[i]), s, p, f).empty() ? 0 : 1;
        }
        CHECK_EQ(failed, 4);
    }

    SECTION("HevcSliceParser — the Mesa dialect's slices round-trip");
    {
        HevcSequence seq;
        seq.width = 1920;
        seq.height = 1080;
        seq.codedWidth = 1920;
        seq.codedHeight = 1088;
        seq.maxReferences = 4;
        const HevcSpsFields s = sps(hevcSps(seq));
        CHECK_EQ(s.log2MaxPocLsb, 16);
        CHECK(s.sampleAdaptiveOffset);
        const HevcPpsFields p = pps(hevcPps(seq));
        CHECK(!p.cabacInitPresent);
        CHECK(!p.sliceChromaQpOffsets);

        HevcSlice idr;
        idr.idr = true;
        HevcSliceFields f;
        CHECK(readSlice(hevcSliceHeader(seq, idr), s, p, f).empty());
        CHECK_EQ(int(f.nalType), 19);
        CHECK_EQ(f.sliceType, 2u);
        CHECK(f.saoLuma && f.saoChroma);
        CHECK_EQ(f.qp, 26);

        HevcSlice predicted;
        predicted.poc = 70000; // written modulo 2^16
        predicted.kept = {69999, 69996, 69990};
        predicted.referencePoc = 69996;
        f = HevcSliceFields{};
        CHECK(readSlice(hevcSliceHeader(seq, predicted), s, p, f).empty());
        CHECK_EQ(f.sliceType, 1u);
        CHECK_EQ(f.pocLsb, 70000u - 65536u);
        CHECK_EQ(f.shortTerm.size(), size_t(3));
        if (f.shortTerm.size() == 3) {
            CHECK_EQ(f.shortTerm[0].deltaPoc, -1);
            CHECK(!f.shortTerm[0].used);
            CHECK_EQ(f.shortTerm[1].deltaPoc, -4);
            CHECK(f.shortTerm[1].used);
            CHECK_EQ(f.shortTerm[2].deltaPoc, -10);
            CHECK(!f.shortTerm[2].used);
        }
        CHECK_EQ(f.activeL0, 1u);
    }

    SECTION("HevcSliceParser — long-term pictures and a modified list");
    {
        // No writer here makes these yet: the slice is written by hand, in the
        // D3D12 dialect with long-term references on (lists_modification too).
        HevcSequence seq = shapeOf(2);
        seq.longTermReferences = true;
        seq.defaultActiveReferences = 2;
        const HevcSpsFields s = sps(hevcSps(seq));
        const HevcPpsFields p = pps(hevcPps(seq));
        h264vui_detail::BitWriter w;
        w.u(1, 1);  // first_slice_segment_in_pic_flag
        w.ue(0);    // slice_pic_parameter_set_id
        w.ue(1);    // P
        w.u(8, 40); // slice_pic_order_cnt_lsb
        w.u(1, 0);  // the set follows
        w.ue(1);    // one before
        w.ue(0);    // none after
        w.ue(0);    //   delta 1
        w.u(1, 1);  //   used
        w.ue(1);    // num_long_term_pics
        w.u(8, 12); //   poc_lsb_lt
        w.u(1, 1);  //   used_by_curr_pic_lt_flag
        w.u(1, 0);  //   delta_poc_msb_present_flag
        w.u(1, 0);  // num_ref_idx_active_override_flag: two, the PPS's
        w.u(1, 1);  // ref_pic_list_modification_flag_l0
        w.u(1, 1);  //   list_entry_l0[0]: the long-term one first
        w.u(1, 0);  //   list_entry_l0[1]
        w.u(1, 0);  // cabac_init_flag
        w.ue(0);    // five_minus_max_num_merge_cand
        w.ue(4);    // slice_qp_delta: se(-2)
        w.ue(0);    // slice_cb_qp_offset
        w.ue(0);    // slice_cr_qp_offset
        w.u(1, 1);  // slice_loop_filter_across_slices_enabled_flag
        w.u(1, 1);  // byte_alignment()
        while (w.used != 8)
            w.u(1, 0);
        std::vector<uint8_t> unit = {0x02, 0x01};
        h264vui_detail::escapeInto(w.bytes, unit);
        HevcSliceFields f;
        const std::string error = readSlice(unit, s, p, f);
        if (!error.empty()) std::fprintf(stderr, "  %s\n", error.c_str());
        CHECK(error.empty());
        CHECK_EQ(f.pocLsb, 40u);
        CHECK_EQ(f.longTerm.size(), size_t(1));
        if (!f.longTerm.empty()) {
            CHECK_EQ(f.longTerm[0].pocLsb, 12u);
            CHECK(f.longTerm[0].used);
        }
        CHECK_EQ(f.referencesUsed(), 2u);
        CHECK_EQ(f.activeL0, 2u);
        CHECK(f.listEntryL0 == std::vector<uint32_t>({1, 0}));
        CHECK_EQ(f.qp, 24);
    }

    SECTION("HevcSliceParser — what cannot be is refused, with a reason");
    {
        const HevcSpsFields s = sps(fromHex(kStreams[0].sps));
        const HevcPpsFields p = pps(fromHex(kD3d12Pps));
        HevcSliceFields f;
        // A header cut short.
        const std::vector<uint8_t> cut = {0x02, 0x01, 0xD0};
        CHECK(readSlice(cut, s, p, f).find("past the end") != std::string::npos);
        // Not a slice at all.
        CHECK(readSlice(fromHex(kD3d12Pps), s, p, f).find("not a slice") != std::string::npos);
        // A slice naming another PPS.
        HevcPpsFields other = p;
        other.ppsId = 1;
        CHECK(readSlice(fromHex(kStreams[0].slices[1]), s, other, f).find("PPS") !=
              std::string::npos);
        // An SPS that is not one, a PPS that ends wrong.
        HevcSpsFields sf;
        CHECK(!parseHevcSps(fromHex(kD3d12Pps).data(), 7, sf).empty());
        std::vector<uint8_t> longer = fromHex(kD3d12Pps);
        longer.push_back(0x80);
        HevcPpsFields pf;
        CHECK(!parseHevcPps(longer.data(), longer.size(), pf).empty());
        // Nothing is written into the output of a refused read.
        HevcSliceFields untouched;
        untouched.qp = 99;
        CHECK(!readSlice(cut, s, p, untouched).empty());
        CHECK_EQ(untouched.qp, 99);
    }
}
