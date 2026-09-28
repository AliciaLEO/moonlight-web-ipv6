/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "encode/HevcDpb.h"
#include "native_test_framework.h"

using namespace mw::native::encode;

namespace {

/// Encodes frames [from, to) the way the encoder would: plan, then record.
void run(HevcDpb& dpb, uint32_t from, uint32_t to)
{
    for (uint32_t f = from; f < to; ++f)
        dpb.encoded(dpb.plan(f, false));
}

std::vector<uint32_t> frames(const std::vector<HevcDpb::Reference>& refs)
{
    std::vector<uint32_t> out;
    for (const auto& r : refs)
        out.push_back(r.frame);
    return out;
}

bool textureFree(const HevcDpb::Plan& p)
{
    if (p.texture < 0) return false;
    for (const auto& r : p.references)
        if (r.texture == p.texture) return false;
    return true;
}

} // namespace

void run_hevc_dpb_tests()
{
    SECTION("HevcDpb — the first picture is an IDR, the next ones predict from the previous");
    {
        HevcDpb dpb(5, 60);
        CHECK_EQ(dpb.slots(), 4);
        CHECK_EQ(dpb.stride(), 2); // 125 ms at 60 fps is 8 frames: 4 slots, every 2nd frame
        CHECK_EQ(dpb.textures(), 6);
        const HevcDpb::Plan first = dpb.plan(100, false);
        CHECK(first.idr);
        CHECK_EQ(first.poc, 0u);
        CHECK(first.references.empty());
        dpb.encoded(first);
        const HevcDpb::Plan second = dpb.plan(101, false);
        CHECK(!second.idr);
        CHECK_EQ(second.poc, 1u);
        CHECK_EQ(second.references.size(), size_t(1));
        CHECK_EQ(second.references[0].frame, 100u);
        CHECK(second.references[0].used);
        CHECK(textureFree(second));
    }

    SECTION("HevcDpb — the previous picture and the marked ones, never more than the capacity");
    {
        HevcDpb dpb(5, 60);
        run(dpb, 0, 10);
        // Marked: 0 (the IDR), 2, 4, 6, 8 — 8 took 0's slot back. The previous
        // picture, 9, is not marked, and is kept all the same.
        const HevcDpb::Plan p = dpb.plan(10, false);
        CHECK(frames(p.references) == std::vector<uint32_t>({9, 8, 6, 4, 2}));
        CHECK(p.references[0].used);
        int used = 0;
        for (const auto& r : p.references)
            used += r.used ? 1 : 0;
        CHECK_EQ(used, 1);
        CHECK(textureFree(p));
        // Over a long run: the capacity holds, a texture is always free, POCs
        // count pictures.
        bool bounded = true, free = true, counted = true;
        for (uint32_t f = 10; f < 400; ++f) {
            const HevcDpb::Plan q = dpb.plan(f, false);
            bounded = bounded && q.references.size() <= size_t(dpb.capacity());
            free = free && textureFree(q) && q.texture < dpb.textures();
            counted = counted && q.poc == f;
            dpb.encoded(q);
        }
        CHECK(bounded);
        CHECK(free);
        CHECK(counted);
    }

    SECTION("HevcDpb — the reach follows the frame rate: 125 ms whatever the rate");
    {
        CHECK_EQ(HevcDpb(5, 60).reachFrames(), 8);
        CHECK_EQ(HevcDpb(5, 120).reachFrames(), 16);
        CHECK_EQ(HevcDpb(5, 240).reachFrames(), 32);
        CHECK_EQ(HevcDpb(1, 60).reachFrames(), 1); // the previous picture alone
    }

    SECTION("HevcDpb — a loss drops the whole tail, the next picture reaches back");
    {
        HevcDpb dpb(5, 60);
        run(dpb, 0, 10);
        // Frame 7 never arrived: 8 and 9 predict from it.
        CHECK(dpb.invalidate(7));
        CHECK(frames(dpb.kept()) == std::vector<uint32_t>({6, 4, 2}));
        const HevcDpb::Plan repair = dpb.plan(10, false);
        CHECK(!repair.idr);
        CHECK_EQ(repair.references[0].frame, 6u);
        CHECK(repair.references[0].used);
        CHECK(frames(repair.references) == std::vector<uint32_t>({6, 4, 2}));
        CHECK(textureFree(repair));
        dpb.encoded(repair);
        // 10 is marked into the slot frame 2 held: 6 and 4 stay, 2 is gone.
        CHECK(frames(dpb.kept()) == std::vector<uint32_t>({10, 6, 4}));
        const HevcDpb::Plan after = dpb.plan(11, false);
        CHECK_EQ(after.references[0].frame, 10u);
    }

    SECTION("HevcDpb — a loss out of reach: nothing older is kept, the next one is an IDR");
    {
        HevcDpb dpb(5, 60);
        run(dpb, 0, 40);
        CHECK(!dpb.invalidate(20));
        const HevcDpb::Plan next = dpb.plan(40, false);
        CHECK(next.idr);
        CHECK_EQ(next.poc, 0u);
        dpb.encoded(next);
        CHECK(frames(dpb.kept()) == std::vector<uint32_t>({40}));
        CHECK_EQ(dpb.plan(41, false).poc, 1u);
    }

    SECTION("HevcDpb — a loss of the newest picture only: the one before it is used");
    {
        HevcDpb dpb(1, 60); // no slot: the previous picture alone
        run(dpb, 0, 5);
        CHECK(!dpb.invalidate(4)); // 4 was the only one kept
        HevcDpb wider(3, 60);
        run(wider, 0, 5);
        CHECK(wider.invalidate(4));
        CHECK_EQ(wider.plan(5, false).references[0].frame, frames(wider.kept())[0]);
        CHECK(frames(wider.kept())[0] < 4u);
    }

    SECTION("HevcDpb — a forced IDR empties the buffer and starts the count again");
    {
        HevcDpb dpb(5, 60);
        run(dpb, 0, 13);
        const HevcDpb::Plan key = dpb.plan(13, true);
        CHECK(key.idr);
        CHECK(key.references.empty());
        dpb.encoded(key);
        CHECK(frames(dpb.kept()) == std::vector<uint32_t>({13}));
        // A loss before the IDR is nothing to the stream after it.
        CHECK(dpb.invalidate(12));
        CHECK(frames(dpb.kept()) == std::vector<uint32_t>({13}));
        const HevcDpb::Plan p = dpb.plan(14, false);
        CHECK_EQ(p.poc, 1u);
        CHECK_EQ(p.references[0].frame, 13u);
    }

    SECTION("HevcDpb — a picture that was planned but not encoded leaves no trace");
    {
        HevcDpb dpb(5, 60);
        run(dpb, 0, 3);
        const HevcDpb::Plan dropped = dpb.plan(3, false);
        // The driver failed it: encoded() is never called.
        const HevcDpb::Plan again = dpb.plan(4, false);
        CHECK_EQ(again.poc, dropped.poc);
        CHECK(frames(again.references) == frames(dropped.references));
    }
}
