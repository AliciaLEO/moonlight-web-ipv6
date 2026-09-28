/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "encode/H264SliceParser.h"
#include "encode/ParameterSets.h"
#include "native_test_framework.h"

#include <string>
#include <vector>

using namespace mw::native::encode;
using namespace mw::native::encode::paramsets;

namespace {

/// What D3D12 Video Encode's H.264 is created with on DualRTX (plan C9.1):
/// 1080 lines coded as 1088, one picture kept.
H264Sequence d3d12Sequence()
{
    H264Sequence s;
    s.profileIdc = 100;
    s.levelIdc = 42;
    s.widthMbs = 120;
    s.heightMbs = 68;
    s.cropBottom = 4;
    s.maxRefFrames = 1;
    s.fps = 60;
    return s;
}

bool contains(const std::string& text, const std::string& piece)
{
    return text.find(piece) != std::string::npos;
}

} // namespace

// The reader D3D12 Video Encode's H.264 guard judges a driver's slices with:
// our own parameter sets read back field by field, then slice headers written
// by ParameterSets.h — which Mesa's VA-API encoders stream — read with them.
void run_h264_slice_parser_tests()
{
    SECTION("H264SliceParser — our SPS reads back: High, POC type 2, 16-bit frame_num, 1088 lines");
    {
        const std::vector<uint8_t> sps = h264Sps(d3d12Sequence());
        H264SpsFields f;
        CHECK(parseH264Sps(sps.data(), sps.size(), f).empty());
        CHECK_EQ(f.profileIdc, 100u);
        CHECK_EQ(f.levelIdc, 42u);
        CHECK_EQ(f.chromaFormatIdc, 1u);
        CHECK_EQ(f.log2MaxFrameNum, 16u);
        CHECK_EQ(f.pocType, 2u);
        CHECK_EQ(f.maxRefFrames, 1u);
        CHECK(!f.gapsAllowed);
        CHECK_EQ(f.widthMbs, 120u);
        CHECK_EQ(f.heightMbs, 68u);
        CHECK(f.frameMbsOnly);
        CHECK(f.direct8x8);
        // Without its start code too: the guard hands units as they are cut.
        H264SpsFields g;
        CHECK(parseH264Sps(sps.data() + 4, sps.size() - 4, g).empty());
        CHECK_EQ(g.heightMbs, 68u);
    }

    SECTION("H264SliceParser — our PPS reads back, the High tail found by the stop bit");
    {
        H264Sequence s = d3d12Sequence();
        const std::vector<uint8_t> high = h264Pps(s);
        H264PpsFields f;
        CHECK(parseH264Pps(high.data(), high.size(), f).empty());
        CHECK(f.cabac);
        CHECK(f.transform8x8);
        CHECK_EQ(f.numRefIdxL0Default, 1u);
        CHECK_EQ(f.initQp, 26);
        CHECK(f.deblockingControl);
        CHECK(!f.weightedPred);
        CHECK(!f.constrainedIntra);
        CHECK(!f.redundantPicCnt);

        // A driver without CABAC or the 8×8 transform: the PPS says so.
        s.cabac = false;
        s.transform8x8 = false;
        const std::vector<uint8_t> plain = h264Pps(s);
        CHECK(parseH264Pps(plain.data(), plain.size(), f).empty());
        CHECK(!f.cabac);
        CHECK(!f.transform8x8);

        // Constrained Baseline has no High tail: nothing past the stop bit.
        s = d3d12Sequence();
        s.profileIdc = 66;
        const std::vector<uint8_t> baseline = h264Pps(s);
        CHECK(parseH264Pps(baseline.data(), baseline.size(), f).empty());
        CHECK(!f.cabac);
        CHECK(!f.transform8x8);
    }

    SECTION("H264SliceParser — slice headers read with the parameter sets they were written for");
    {
        const H264Sequence s = d3d12Sequence();
        const std::vector<uint8_t> spsUnit = h264Sps(s);
        const std::vector<uint8_t> ppsUnit = h264Pps(s);
        H264SpsFields sps;
        H264PpsFields pps;
        CHECK(parseH264Sps(spsUnit.data(), spsUnit.size(), sps).empty());
        CHECK(parseH264Pps(ppsUnit.data(), ppsUnit.size(), pps).empty());

        H264Slice idr;
        idr.idr = true;
        idr.idrPicId = 5;
        std::vector<uint8_t> nal = h264SliceHeader(s, idr);
        H264SliceFields f;
        CHECK(parseH264SliceHeader(nal.data(), nal.size(), sps, pps, f).empty());
        CHECK_EQ(f.nalType, 5u);
        CHECK_EQ(f.nalRefIdc, 3u);
        CHECK_EQ(f.sliceType, 2u);
        CHECK_EQ(f.frameNum, 0u);
        CHECK_EQ(f.idrPicId, 5u);
        CHECK_EQ(f.qp, 26); // slice_qp_delta 0 over the PPS's 26

        H264Slice p;
        p.frameNum = 12;
        p.referenceFrameNum = 11;
        nal = h264SliceHeader(s, p);
        CHECK(parseH264SliceHeader(nal.data(), nal.size(), sps, pps, f).empty());
        CHECK_EQ(f.nalType, 1u);
        CHECK_EQ(f.sliceType, 0u);
        CHECK_EQ(f.frameNum, 12u);
        CHECK_EQ(f.numRefIdxL0Active, 1u);
        CHECK(f.listModifications.empty());
        CHECK(!f.adaptiveMarking);
        CHECK_EQ(f.qp, 26);

        // Reaching back past a loss: the list reordered, read as written.
        p.referenceFrameNum = 9;
        nal = h264SliceHeader(s, p);
        CHECK(parseH264SliceHeader(nal.data(), nal.size(), sps, pps, f).empty());
        CHECK_EQ(f.listModifications.size(), static_cast<size_t>(1));
        if (!f.listModifications.empty()) {
            CHECK_EQ(f.listModifications[0].first, 0u);  // subtract
            CHECK_EQ(f.listModifications[0].second, 2u); // abs_diff_pic_num_minus1
        }

        // CAVLC: no cabac_init_idc in front of the QP, which still reads.
        H264Sequence cavlc = s;
        cavlc.cabac = false;
        const std::vector<uint8_t> cavlcPps = h264Pps(cavlc);
        H264PpsFields noCabac;
        CHECK(parseH264Pps(cavlcPps.data(), cavlcPps.size(), noCabac).empty());
        p.referenceFrameNum = 11;
        nal = h264SliceHeader(cavlc, p);
        CHECK(parseH264SliceHeader(nal.data(), nal.size(), sps, noCabac, f).empty());
        CHECK_EQ(f.qp, 26);
    }

    SECTION("H264SliceParser — what it does not read, it says");
    {
        const H264Sequence s = d3d12Sequence();
        const std::vector<uint8_t> spsUnit = h264Sps(s);
        const std::vector<uint8_t> ppsUnit = h264Pps(s);
        H264SpsFields sps;
        H264PpsFields pps;
        parseH264Sps(spsUnit.data(), spsUnit.size(), sps);
        parseH264Pps(ppsUnit.data(), ppsUnit.size(), pps);

        H264SpsFields f;
        CHECK(contains(parseH264Sps(ppsUnit.data(), ppsUnit.size(), f), "not an SPS"));
        H264PpsFields g;
        CHECK(contains(parseH264Pps(spsUnit.data(), spsUnit.size(), g), "not a PPS"));
        H264SliceFields h;
        CHECK(contains(parseH264SliceHeader(spsUnit.data(), spsUnit.size(), sps, pps, h),
                       "not a slice"));

        // A slice cut short reads past its end.
        H264Slice p;
        p.frameNum = 3;
        p.referenceFrameNum = 2;
        const std::vector<uint8_t> nal = h264SliceHeader(s, p);
        CHECK(!parseH264SliceHeader(nal.data(), 6, sps, pps, h).empty());

        // A P slice in an IDR unit.
        std::vector<uint8_t> wrong = nal;
        wrong[4] = 0x65;
        CHECK(contains(parseH264SliceHeader(wrong.data(), wrong.size(), sps, pps, h),
                       "an IDR with a P slice"));

        // Read with an SPS whose frame_num is narrower than the writer's, the
        // header does not read, or says another picture: the guard's sieve.
        H264SpsFields narrow = sps;
        narrow.log2MaxFrameNum = 4;
        h = H264SliceFields();
        const bool reads = parseH264SliceHeader(nal.data(), nal.size(), narrow, pps, h).empty();
        CHECK(!reads || h.frameNum != 3);
    }
}
