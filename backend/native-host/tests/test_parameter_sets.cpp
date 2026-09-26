/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "encode/ParameterSets.h"
#include "native_test_framework.h"

#include <string>

using namespace mw::native::encode;
using namespace mw::native::encode::paramsets;
using h264vui_detail::BitReader;

namespace {

/// The RBSP of an Annex-B NAL unit: start code and @p headerBytes of NAL
/// header off, emulation prevention undone.
std::vector<uint8_t> rbspOf(const std::vector<uint8_t>& nal, size_t headerBytes)
{
    return h264vui_detail::unescape(nal.data() + 4 + headerBytes, nal.size() - 4 - headerBytes);
}

/// No 00 00 0x (x ≤ 3) after the start code: a decoder would read a start code
/// or an escape where there is none.
bool escaped(const std::vector<uint8_t>& nal)
{
    for (size_t i = 4; i + 2 < nal.size(); ++i)
        if (nal[i] == 0 && nal[i + 1] == 0 && nal[i + 2] <= 3 &&
            !(nal[i + 2] == 3)) // 00 00 03 is the escape itself
            return false;
    return true;
}

H264Sequence sequence1080p()
{
    H264Sequence s;
    s.profileIdc = 100;
    s.widthMbs = 120;
    s.heightMbs = 68; // 1088 coded
    s.cropBottom = 4; // 8 lines of padding, in 2-line units
    s.maxRefFrames = 4;
    s.fps = 60;
    return s;
}

HevcSequence hevc1080p()
{
    HevcSequence s;
    s.width = 1920;
    s.height = 1080;
    s.codedWidth = 1920;
    s.codedHeight = 1088;
    s.maxReferences = 4;
    s.fps = 60;
    return s;
}

/// What the Arc A380's D3D12 encoder was created with on 21/09/2026: 8..64
/// coding blocks, 4..32 transforms, depth 2, AMP (which that driver requires),
/// 1080 lines coded as 1088, one picture held, an 8-bit POC.
HevcSequence arcShape()
{
    HevcSequence s;
    s.dialect = HevcDialect::D3d12;
    s.width = 1920;
    s.height = 1080;
    s.codedWidth = 1920;
    s.codedHeight = 1088;
    s.maxReferences = 1;
    s.log2MaxPocLsb = 8;
    s.asymmetricMotionPartitions = true;
    return s;
}

/// A unit written without its start code — as the v1 tests kept them — in
/// Annex-B.
std::vector<uint8_t> annexB(const char* hex)
{
    std::vector<uint8_t> v = {0, 0, 0, 1};
    for (; hex[0] && hex[1]; hex += 2)
        v.push_back(static_cast<uint8_t>(std::stoi(std::string(hex, 2), nullptr, 16)));
    return v;
}

} // namespace

void run_parameter_sets_tests()
{
    SECTION("ParameterSets — the H.264 SPS is one the VUI rewriter reads and leaves alone");
    {
        // H264Vui parses an SPS up to the VUI independently of this writer, and
        // rewrites bitstream_restriction to "nothing reordered". An SPS that
        // already says so, in the same terms, comes back byte for byte — which
        // proves both that it parses and that the restriction is the right one.
        const auto sps = h264Sps(sequence1080p());
        CHECK_EQ(sps[4], 0x67);
        const std::vector<uint8_t> unit(sps.begin() + 4, sps.end());
        CHECK(h264WithNoReorderVui(unit.data(), unit.size()) == unit);
        CHECK(escaped(sps));
    }

    SECTION("ParameterSets — the H.264 SPS says the size and the crop");
    {
        const auto rbsp = rbspOf(h264Sps(sequence1080p()), 1);
        BitReader r{rbsp};
        CHECK_EQ(r.u(8), 100u); // profile_idc
        CHECK_EQ(r.u(8), 0u);   // constraint flags
        CHECK_EQ(r.u(8), 51u);  // level_idc
        CHECK_EQ(r.ue(), 0u);   // sps id
        CHECK_EQ(r.ue(), 1u);   // chroma_format_idc
        r.ue();
        r.ue();
        r.u(2);
        CHECK_EQ(r.ue(), 12u); // log2_max_frame_num_minus4
        CHECK_EQ(r.ue(), 2u);  // pic_order_cnt_type
        CHECK_EQ(r.ue(), 4u);  // max_num_ref_frames
        r.u(1);
        CHECK_EQ(r.ue(), 119u); // width in MBs - 1
        CHECK_EQ(r.ue(), 67u);  // height in map units - 1
        CHECK_EQ(r.u(2), 3u);   // frame_mbs_only, direct_8x8
        CHECK_EQ(r.u(1), 1u);   // frame_cropping_flag
        CHECK_EQ(r.ue(), 0u);
        CHECK_EQ(r.ue(), 0u);
        CHECK_EQ(r.ue(), 0u);
        CHECK_EQ(r.ue(), 4u); // bottom: 1088 → 1080
        CHECK(!r.overrun);
    }

    SECTION("ParameterSets — an H.264 P slice reorders only when it reaches back");
    {
        const H264Sequence s = sequence1080p();
        const auto readModification = [&](uint32_t frameNum, uint32_t reference) {
            H264Slice slice;
            slice.frameNum = frameNum;
            slice.referenceFrameNum = reference;
            const auto nal = h264SliceHeader(s, slice);
            CHECK_EQ(nal[4], 0x41); // nal_ref_idc 2, non-IDR slice
            const auto rbsp = rbspOf(nal, 1);
            BitReader r{rbsp};
            CHECK_EQ(r.ue(), 0u); // first_mb_in_slice
            CHECK_EQ(r.ue(), 5u); // P
            CHECK_EQ(r.ue(), 0u); // pps id
            CHECK_EQ(r.u(16), frameNum & 0xFFFF);
            CHECK_EQ(r.u(1), 0u); // num_ref_idx_active_override_flag
            if (!r.u(1)) return -1;
            CHECK_EQ(r.ue(), 0u); // subtract
            const int diff = static_cast<int>(r.ue()) + 1;
            CHECK_EQ(r.ue(), 3u);
            return diff;
        };
        CHECK_EQ(readModification(10, 9), -1); // the previous picture: the default
        CHECK_EQ(readModification(10, 7), 3);  // three back, past a loss
        // frame_num wraps at 2^16; the distance does not.
        CHECK_EQ(readModification(1, 65534), 3);
    }

    SECTION("ParameterSets — an H.264 IDR slice carries its id and no reordering");
    {
        H264Slice slice;
        slice.idr = true;
        slice.idrPicId = 7;
        const auto nal = h264SliceHeader(sequence1080p(), slice);
        CHECK_EQ(nal[4], 0x65);
        const auto rbsp = rbspOf(nal, 1);
        BitReader r{rbsp};
        r.ue();
        CHECK_EQ(r.ue(), 7u); // I
        r.ue();
        CHECK_EQ(r.u(16), 0u);
        CHECK_EQ(r.ue(), 7u); // idr_pic_id
        CHECK_EQ(r.u(2), 0u); // no_output_of_prior_pics, long_term_reference
        CHECK(escaped(nal));
    }

    SECTION("ParameterSets — HEVC SPS: the conformance window hides the coded padding");
    {
        HevcSequence s = hevc1080p();
        s.width = 1352; // the aligned shape of a 1366x768 desktop
        s.height = 760;
        s.codedWidth = 1408; // 22 CTBs of 64
        s.codedHeight = 768;
        const auto sps = hevcSps(s);
        CHECK_EQ(sps[4], 0x42);
        CHECK_EQ(sps[5], 0x01);
        const auto rbsp = rbspOf(sps, 2);
        BitReader r{rbsp};
        r.u(4);
        CHECK_EQ(r.u(3), 0u); // max_sub_layers_minus1
        r.u(1);
        r.u(32); // profile_tier_level up to the level: 88 bits…
        r.u(32);
        r.u(24);
        CHECK_EQ(r.u(8), 153u); // …general_level_idc
        CHECK_EQ(r.ue(), 0u);   // sps id
        CHECK_EQ(r.ue(), 1u);   // chroma_format_idc
        CHECK_EQ(r.ue(), 1408u);
        CHECK_EQ(r.ue(), 768u);
        CHECK_EQ(r.u(1), 1u); // conformance_window_flag
        CHECK_EQ(r.ue(), 0u);
        CHECK_EQ(r.ue(), 28u); // (1408 - 1352) / 2
        CHECK_EQ(r.ue(), 0u);
        CHECK_EQ(r.ue(), 4u); // (768 - 760) / 2
        r.ue();
        r.ue();
        CHECK_EQ(r.ue(), 12u); // log2_max_pic_order_cnt_lsb_minus4
        CHECK_EQ(r.u(1), 1u);
        CHECK_EQ(r.ue(), 4u); // sps_max_dec_pic_buffering_minus1
        CHECK_EQ(r.ue(), 0u); // sps_max_num_reorder_pics
        CHECK(!r.overrun);
        CHECK(escaped(sps));
        CHECK(escaped(hevcVps(s)));
        CHECK(escaped(hevcPps(s)));
    }

    SECTION("ParameterSets — an HEVC P slice keeps every held picture, uses one");
    {
        const HevcSequence s = hevc1080p();
        HevcSlice slice;
        slice.poc = 20;
        slice.kept = {17, 19, 15}; // any order
        slice.referencePoc = 17;   // the frames after 17 were lost
        const auto nal = hevcSliceHeader(s, slice);
        CHECK_EQ(nal[4], 0x02); // TRAIL_R
        CHECK_EQ(nal[5], 0x01);
        const auto rbsp = rbspOf(nal, 2);
        BitReader r{rbsp};
        CHECK_EQ(r.u(1), 1u); // first_slice_segment_in_pic_flag
        CHECK_EQ(r.ue(), 0u); // pps id
        CHECK_EQ(r.ue(), 1u); // P
        CHECK_EQ(r.u(16), 20u);
        CHECK_EQ(r.u(1), 0u); // the set is in the slice
        CHECK_EQ(r.ue(), 3u); // three before
        CHECK_EQ(r.ue(), 0u); // none after
        // Newest first, each delta from the previous: 19, 17, 15.
        CHECK_EQ(r.ue(), 0u);
        CHECK_EQ(r.u(1), 0u); // 19 kept, not used
        CHECK_EQ(r.ue(), 1u);
        CHECK_EQ(r.u(1), 1u); // 17 used
        CHECK_EQ(r.ue(), 1u);
        CHECK_EQ(r.u(1), 0u); // 15 kept
        CHECK_EQ(r.u(2), 3u); // SAO luma, chroma
        CHECK_EQ(r.u(1), 0u); // num_ref_idx_active_override_flag
        CHECK_EQ(r.ue(), 0u); // five_minus_max_num_merge_cand
        CHECK_EQ(r.ue(), 0u); // slice_qp_delta 0
        CHECK(!r.overrun);
    }

    SECTION("ParameterSets — the HEVC POC is written modulo its width");
    {
        HevcSlice slice;
        slice.poc = 70000;
        slice.kept = {69999};
        slice.referencePoc = 69999;
        const auto rbsp = rbspOf(hevcSliceHeader(hevc1080p(), slice), 2);
        BitReader r{rbsp};
        r.u(1);
        r.ue();
        r.ue();
        CHECK_EQ(r.u(16), 70000u - 65536u);
        r.u(1);
        CHECK_EQ(r.ue(), 1u);
        CHECK_EQ(r.ue(), 0u);
        CHECK_EQ(r.ue(), 0u); // delta 1
        CHECK_EQ(r.u(1), 1u);
    }

    SECTION("ParameterSets — every header survives emulation prevention");
    {
        // Values chosen to put runs of zero bits in the RBSP.
        bool all = true;
        for (uint32_t n = 0; n < 600; ++n) {
            H264Slice h;
            h.idr = (n % 7) == 0;
            h.frameNum = n * 257;
            h.idrPicId = n;
            h.referenceFrameNum = h.frameNum - 1 - (n % 4);
            all = all && escaped(h264SliceHeader(sequence1080p(), h));
            HevcSlice v;
            v.poc = n * 256;
            v.kept = {v.poc - 1, v.poc - 2};
            v.referencePoc = v.poc - 1;
            all = all && escaped(hevcSliceHeader(hevc1080p(), v));
        }
        CHECK(all);
    }

    SECTION("ParameterSets — the bytes Mesa's VA-API encoders are handed today (goldens)");
    {
        // Frozen on 26/09/2026, before the D3D12 dialect is written beside them
        // (plan pipeline-video-d3d12-v2, C4.1). The Linux hosts get no bench of
        // their own in that plan, so a byte that moves here has to be a
        // decision, not a side effect: change the golden and say why in the
        // commit.
        using Bytes = std::vector<uint8_t>;
        const H264Sequence high = sequence1080p();
        CHECK(h264Sps(high) ==
              Bytes({0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x33, 0xAC, 0x1A, 0xCA, 0x03,
                     0xC0, 0x11, 0x3F, 0x2C, 0xD4, 0x04, 0x04, 0x05, 0x00, 0x00, 0x03, 0x00,
                     0x01, 0x00, 0x00, 0x03, 0x00, 0x78, 0x0D, 0xA0, 0x80, 0x42, 0x58}));
        CHECK(h264Pps(high) == Bytes({0x00, 0x00, 0x00, 0x01, 0x68, 0xEE, 0x3C, 0xB0}));
        H264Slice idr;
        idr.idr = true;
        idr.idrPicId = 3;
        CHECK(h264SliceHeader(high, idr) ==
              Bytes({0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x80, 0x00, 0x10, 0xF8}));
        H264Slice p;
        p.frameNum = 7;
        p.referenceFrameNum = 6;
        CHECK(h264SliceHeader(high, p) ==
              Bytes({0x00, 0x00, 0x00, 0x01, 0x41, 0x9A, 0x00, 0x0E, 0x3F}));
        p.referenceFrameNum = 4;
        CHECK(h264SliceHeader(high, p) ==
              Bytes({0x00, 0x00, 0x00, 0x01, 0x41, 0x9A, 0x00, 0x0E, 0xD9, 0x1F, 0x80}));
        H264Sequence constrained = high;
        constrained.profileIdc = 66;
        constrained.levelIdc = 42;
        CHECK(h264Sps(constrained) ==
              Bytes({0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xC0, 0x2A, 0x8D, 0x65, 0x01,
                     0xE0, 0x08, 0x9F, 0x96, 0x6A, 0x02, 0x02, 0x02, 0x80, 0x00, 0x00,
                     0x03, 0x00, 0x80, 0x00, 0x00, 0x3C, 0x06, 0xD0, 0x40, 0x21, 0x2C}));
        CHECK(h264SliceHeader(constrained, p) ==
              Bytes({0x00, 0x00, 0x00, 0x01, 0x41, 0x9A, 0x00, 0x0E, 0xD9, 0x1F}));

        HevcSequence hevc = hevc1080p();
        hevc.maxReferences = 3;
        CHECK(hevcVps(hevc) == Bytes({0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0C, 0x01, 0xFF, 0xFF,
                                      0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03,
                                      0x00, 0x00, 0x03, 0x00, 0x99, 0x93, 0x02, 0x40}));
        CHECK(hevcSps(hevc) ==
              Bytes({0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00,
                     0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x99,
                     0xA0, 0x03, 0xC0, 0x80, 0x11, 0x07, 0xCB, 0x8D, 0x93, 0x92, 0x46,
                     0xDA, 0x66, 0xA0, 0x20, 0x20, 0x20, 0x80, 0x00, 0x00, 0x03, 0x00,
                     0x80, 0x00, 0x00, 0x1E, 0x17, 0x68, 0x20, 0x10, 0x40}));
        CHECK(hevcPps(hevc) == Bytes({0x00, 0x00, 0x00, 0x01, 0x44, 0x01, 0xC0, 0x73, 0xC0, 0x09}));
        HevcSlice keyframe;
        keyframe.idr = true;
        CHECK(hevcSliceHeader(hevc, keyframe) ==
              Bytes({0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0xAF, 0xC0}));
        HevcSlice predicted;
        predicted.poc = 9;
        predicted.kept = {8, 6, 3};
        predicted.referencePoc = 8;
        CHECK(hevcSliceHeader(hevc, predicted) ==
              Bytes({0x00, 0x00, 0x00, 0x01, 0x02, 0x01, 0xD0, 0x00, 0x48, 0x9D, 0x1B, 0x70}));
        predicted.referencePoc = 6;
        CHECK(hevcSliceHeader(hevc, predicted) ==
              Bytes({0x00, 0x00, 0x00, 0x01, 0x02, 0x01, 0xD0, 0x00, 0x48, 0x99, 0x5B, 0x70}));
    }

    SECTION("ParameterSets — the D3D12 fields leave the VA-API bytes alone");
    {
        const HevcSequence plain = hevc1080p();
        HevcSequence loaded = plain;
        loaded.tenBit = true;
        loaded.hdr = true;
        loaded.log2MaxCodingBlock = 5;
        loaded.transformDepthInter = 3;
        loaded.sampleAdaptiveOffset = true;
        loaded.longTermReferences = true;
        loaded.transformSkip = true;
        loaded.constrainedIntraPrediction = true;
        loaded.loopFilterAcrossSlices = false;
        loaded.defaultActiveReferences = 2;
        CHECK(hevcVps(loaded) == hevcVps(plain));
        CHECK(hevcSps(loaded) == hevcSps(plain));
        CHECK(hevcPps(loaded) == hevcPps(plain));
        HevcSlice slice;
        slice.poc = 5;
        slice.kept = {4, 2};
        slice.referencePoc = 4;
        CHECK(hevcSliceHeader(loaded, slice) == hevcSliceHeader(plain, slice));
    }

    SECTION("ParameterSets — D3D12: the bytes a decoder accepted over the drivers' slices "
            "(goldens)");
    {
        // The first D3D12 attempt's (84524e7f^, test_hevc_param_sets.cpp), in
        // its own hex. These went ahead of 800 frames of the Arc's encoder on
        // 21/09 and ffmpeg decoded the lot, P frames included; the same writer,
        // fed each GPU's own configuration, did as well over the three GPUs of
        // DualRTX on 26/09 (mw-d3d12-lab encode). That is the only proof there
        // is that the PPS says what the drivers' slice headers assume.
        const HevcSequence s = arcShape();
        CHECK(hevcVps(s) == annexB("40010c01ffff016000000300b000000300000300992c09"));
        CHECK(hevcSps(s) == annexB("420101016000000300b00000030000030099a003c0801107cb94b9246d226a"
                                   "02020201"));
        CHECK(hevcPps(s) == annexB("4401c0f3e0cc90"));
        CHECK(escaped(hevcVps(s)));
        CHECK(escaped(hevcSps(s)));
        CHECK(escaped(hevcPps(s)));
    }

    SECTION("ParameterSets — D3D12: the three GPUs of DualRTX as created on 26/09 (goldens)");
    {
        // mw-d3d12-lab encode (CBR 1080p60, level 4.1, one picture held) put
        // these ahead of each GPU's own slices, and ffmpeg decoded the three
        // streams without an error. The PPS is the same for all.
        const auto gpu = [](int log2MaxCodingBlock, int depth, bool amp) {
            HevcSequence s = arcShape();
            s.levelIdc = 123;
            s.log2MaxCodingBlock = log2MaxCodingBlock;
            s.transformDepthInter = s.transformDepthIntra = depth;
            s.asymmetricMotionPartitions = amp;
            return s;
        };
        const HevcSequence rtx = gpu(5, 3, true);  // RTX 5060 Ti: 8..32, AMP required
        const HevcSequence amd = gpu(6, 4, false); // the AMD iGPU: AMP free, left off
        const HevcSequence arc = gpu(6, 2, true);  // Arc A380: AMP required
        const auto vps = annexB("40010c01ffff016000000300b0000003000003007b2c09");
        CHECK(hevcVps(rtx) == vps);
        CHECK(hevcVps(amd) == vps);
        CHECK(hevcVps(arc) == vps);
        CHECK(hevcSps(rtx) ==
              annexB("420101016000000300b0000003000003007ba003c0801107cb94bb9084489a8080808040"));
        CHECK(hevcSps(amd) ==
              annexB("420101016000000300b0000003000003007ba003c0801107cb94b924294226a020202010"));
        CHECK(hevcSps(arc) ==
              annexB("420101016000000300b0000003000003007ba003c0801107cb94b9246d226a02020201"));
        CHECK(hevcPps(rtx) == annexB("4401c0f3e0cc90"));
        CHECK(hevcPps(amd) == annexB("4401c0f3e0cc90"));
        CHECK(hevcPps(arc) == annexB("4401c0f3e0cc90"));
    }

    SECTION("ParameterSets — D3D12: Main 10, BT.2020 PQ (golden)");
    {
        // The first attempt's writer gave these for the shape its last test
        // used: the Arc's, 1440p, 10 bits, HDR. No decoder has seen them yet —
        // the encoder's test in Main 10 comes with it (C4.6).
        HevcSequence s = arcShape();
        s.width = s.codedWidth = 2560;
        s.height = s.codedHeight = 1440;
        s.tenBit = true;
        s.hdr = true;
        const auto sps = hevcSps(s);
        CHECK(hevcVps(s) == annexB("40010c01ffff022000000300b000000300000300992c09"));
        CHECK(sps == annexB("420101022000000300b00000030000030099a001402005a13652e491b489a8"
                            "48804804"));
        CHECK(hevcPps(s) == annexB("4401c0f3e0cc90")); // the PPS knows neither
        const auto rbsp = rbspOf(sps, 2);
        BitReader r{rbsp};
        r.u(8);
        r.u(3);
        CHECK_EQ(r.u(5), 2u);           // general_profile_idc: Main 10
        CHECK_EQ(r.u(32), 0x20000000u); // compatible with Main 10 alone
        r.u(32);                        // the constraint flags, the 44 reserved bits
        r.u(16);
        CHECK_EQ(r.u(8), 153u);
        CHECK_EQ(r.ue(), 0u);
        CHECK_EQ(r.ue(), 1u);
        CHECK_EQ(r.ue(), 2560u);
        CHECK_EQ(r.ue(), 1440u);
        CHECK_EQ(r.u(1), 0u); // no window: nothing coded beyond the picture
        CHECK_EQ(r.ue(), 2u); // bit_depth_luma_minus8
        CHECK_EQ(r.ue(), 2u); // bit_depth_chroma_minus8
        CHECK(!r.overrun);
        // BT.2020, PQ, BT.2020 NCL: three bytes in a row somewhere in the VUI,
        // byte-aligned or not.
        bool found = false;
        for (size_t bit = 0; bit + 24 <= rbsp.size() * 8 && !found; ++bit) {
            BitReader at{rbsp, bit};
            found = at.u(8) == 9 && at.u(8) == 16 && at.u(8) == 9;
        }
        CHECK(found);
    }

    SECTION("ParameterSets — D3D12: every switch of the encoder's configuration (golden)");
    {
        // No GPU's: every field the dialect reads, off its default at once,
        // against the first attempt's writer — its branches all taken.
        HevcSequence s = arcShape();
        s.width = 1366;
        s.height = 768;
        s.codedWidth = 1376;
        s.codedHeight = 768;
        s.levelIdc = 120;
        s.log2MaxCodingBlock = 5;
        s.transformDepthInter = 3;
        s.transformDepthIntra = 3;
        s.sampleAdaptiveOffset = true;
        s.longTermReferences = true;
        s.transformSkip = true;
        s.constrainedIntraPrediction = true;
        s.loopFilterAcrossSlices = false;
        s.log2MaxPocLsb = 16;
        s.maxReferences = 4;
        s.defaultActiveReferences = 2;
        CHECK(hevcVps(s) == annexB("40010c01ffff016000000300b00000030000030078170240"));
        CHECK(hevcSps(s) == annexB("420101016000000300b00000030000030078a002b080301cde345ee4211b93"
                                   "5010101008"));
        CHECK(hevcPps(s) == annexB("4401c0aff81364"));
        // The window, in chroma samples: 10 luma columns.
        const auto rbsp = rbspOf(hevcSps(s), 2);
        BitReader r{rbsp};
        r.u(8);
        r.u(32); // profile_tier_level: 96 bits with no sub-layers
        r.u(32);
        CHECK_EQ(r.u(32) & 0xFF, 120u);
        CHECK_EQ(r.ue(), 0u);
        CHECK_EQ(r.ue(), 1u);
        CHECK_EQ(r.ue(), 1376u);
        CHECK_EQ(r.ue(), 768u);
        CHECK_EQ(r.u(1), 1u);
        CHECK_EQ(r.ue(), 0u);
        CHECK_EQ(r.ue(), 5u);
        CHECK_EQ(r.ue(), 0u);
        CHECK_EQ(r.ue(), 0u);
        CHECK(!r.overrun);
    }

    SECTION("ParameterSets — D3D12: no start code inside a unit, whatever the size");
    {
        // The profile_tier_level alone holds 44 zero bits in a row.
        bool all = true;
        for (uint32_t height : {720u, 1080u, 1440u, 2160u})
            for (bool tenBit : {false, true}) {
                HevcSequence s = arcShape();
                s.height = height;
                s.width = height * 16 / 9;
                s.codedWidth = (s.width + 15) / 16 * 16;
                s.codedHeight = (height + 15) / 16 * 16;
                s.tenBit = s.hdr = tenBit;
                all = all && escaped(hevcVps(s)) && escaped(hevcSps(s)) && escaped(hevcPps(s));
            }
        CHECK(all);
    }
}
