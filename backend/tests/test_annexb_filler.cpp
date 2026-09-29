/*
 * MoonlightWeb — Backend TNR. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * Filler NALs removed from a Sunshine access unit (AnnexBFiller.h).
 *
 * The frame that motivated it is rebuilt below: the keyframe of Sunshine's
 * hevc_vulkan on RADV, VPS SPS PPS, FILLER, IDR, FILLER. What must hold is
 * that the filler goes, every other byte stays in order, and a unit with no
 * filler is left untouched.
 */

#include "../src/streaming/AnnexBFiller.h"

#include "test_framework.h"

#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

void append(Bytes& out, std::initializer_list<uint8_t> bytes)
{
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void appendFiller(Bytes& out, size_t payload, bool hevc, bool fourByteStart = true)
{
    if (fourByteStart) out.push_back(0);
    append(out, {0, 0, 1});
    if (hevc)
        append(out, {0x4C, 0x01}); // type 38
    else
        out.push_back(0x0C); // type 12
    out.insert(out.end(), payload, 0xFF);
    out.push_back(0x80);
}

size_t strip(Bytes& b, bool hevc)
{
    const size_t kept = AnnexBFiller::strip(b.data(), b.size(), hevc);
    b.resize(kept);
    return kept;
}

} // namespace

void run_annexb_filler_tests()
{
    SECTION("AnnexBFiller — the RADV HEVC keyframe loses both fillers, nothing else");
    {
        Bytes frame, expected;
        append(frame, {0, 0, 0, 1, 0x40, 0x01, 0x0C, 0x01}); // VPS
        append(frame, {0, 0, 0, 1, 0x42, 0x01, 0x01, 0x02}); // SPS
        append(frame, {0, 0, 0, 1, 0x44, 0x01, 0xE0, 0x77}); // PPS
        expected = frame;
        appendFiller(frame, 150, true);
        const Bytes idr = {0, 0, 0, 1, 0x26, 0x01, 0xAF, 0x68, 0x12, 0x00, 0x00, 0x03, 0x01, 0x34};
        frame.insert(frame.end(), idr.begin(), idr.end());
        expected.insert(expected.end(), idr.begin(), idr.end());
        appendFiller(frame, 16000, true);

        CHECK_EQ(strip(frame, true), expected.size());
        CHECK(frame == expected);
    }

    SECTION("AnnexBFiller — a unit without filler is returned whole");
    {
        Bytes frame;
        append(frame, {0, 0, 0, 1, 0x02, 0x01, 0xD0, 0x00, 0x00, 0x03, 0x01, 0x55}); // TRAIL_R
        append(frame, {0, 0, 1, 0x4E, 0x01, 0x05, 0x10});                            // prefix SEI
        const Bytes before = frame;
        CHECK_EQ(strip(frame, true), before.size());
        CHECK(frame == before);
    }

    SECTION("AnnexBFiller — H.264 type 12, three-byte start codes, filler first");
    {
        Bytes frame, expected;
        appendFiller(frame, 40, false, false);
        const Bytes slice = {0, 0, 1, 0x65, 0x88, 0x84, 0x00, 0x33};
        frame.insert(frame.end(), slice.begin(), slice.end());
        expected = slice;
        appendFiller(frame, 10, false, false);
        const Bytes sei = {0, 0, 0, 1, 0x06, 0x05, 0x01};
        frame.insert(frame.end(), sei.begin(), sei.end());
        expected.insert(expected.end(), sei.begin(), sei.end());

        CHECK_EQ(strip(frame, false), expected.size());
        CHECK(frame == expected);
    }

    SECTION("AnnexBFiller — the codec decides: HEVC type 38 is not H.264 filler");
    {
        // 0x4C read as H.264 is type 12 — so the same bytes are filler in one
        // codec and a slice header byte in the other; and 0x0C read as HEVC is
        // type 6, a slice.
        Bytes h264;
        append(h264, {0, 0, 0, 1, 0x0C, 0xFF, 0x80});
        append(h264, {0, 0, 0, 1, 0x65, 0x88});
        Bytes asHevc = h264;
        CHECK_EQ(strip(asHevc, true), h264.size());
        CHECK_EQ(strip(h264, false), size_t(6));
    }

    SECTION("AnnexBFiller — a unit that is only filler comes out empty");
    {
        Bytes frame;
        appendFiller(frame, 100, true);
        appendFiller(frame, 100, true);
        CHECK_EQ(strip(frame, true), size_t(0));
    }

    SECTION("AnnexBFiller — degenerate inputs");
    {
        Bytes empty;
        CHECK_EQ(strip(empty, true), size_t(0));
        Bytes tiny = {0, 0, 1};
        CHECK_EQ(strip(tiny, true), size_t(3));
        Bytes noStartCode = {0x12, 0x34, 0x01, 0x56};
        CHECK_EQ(strip(noStartCode, true), size_t(4));
    }
}
