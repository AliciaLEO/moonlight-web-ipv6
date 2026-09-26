/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "encode/ParameterSets.h"
#include "native_test_framework.h"

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
}
