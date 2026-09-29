/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#include "encode/IntraRefreshSweep.h"

#include <cstdio>

using mw::native::encode::IntraRefreshSweep;

// The sweeps told to Vulkan Video's intra refresh (plan C13.9): which picture
// refreshes which region, and how dirty its reference is — the one number
// the driver cannot work out itself and must not be told wrong.

void run_intra_refresh_sweep_tests()
{
    SECTION("IntraRefreshSweep — disabled: no picture is part of a sweep");
    {
        IntraRefreshSweep none;
        CHECK(!none.enabled());
        CHECK_EQ(none.horizon(), 0u);
        CHECK(!none.next(true, false).refresh);
        CHECK(!none.next(false, true).refresh);
    }

    SECTION("IntraRefreshSweep — back to back: every picture after the first period is in one");
    {
        IntraRefreshSweep sweep(4, -1);
        CHECK(sweep.enabled());
        CHECK_EQ(sweep.distance(), 4u);
        CHECK_EQ(sweep.horizon(), 4u);           // the sweep alone
        CHECK(!sweep.next(true, false).refresh); // the IDR
        // Pictures 1 to 3: the IDR is a whole refresh of its own.
        for (int p = 1; p < 4; ++p)
            CHECK(!sweep.next(false, true).refresh);
        // Pictures 4 to 11: two sweeps, index 0 to 3 each, the reference as
        // dirty as the current picture has left to do.
        for (int s = 0; s < 2; ++s) {
            for (uint32_t i = 0; i < 4; ++i) {
                const IntraRefreshSweep::Step step = sweep.next(false, true);
                CHECK(step.refresh);
                CHECK_EQ(step.duration, 4u);
                CHECK_EQ(step.index, i);
                CHECK_EQ(step.referenceDirty, 4u - i);
            }
        }
    }

    SECTION("IntraRefreshSweep — the engine's gap: a sweep every four periods");
    {
        IntraRefreshSweep sweep(120, 480);
        CHECK_EQ(sweep.horizon(), 600u); // the gap plus one sweep, as oneVPL says it
        CHECK(!sweep.next(true, false).refresh);
        int inSweep = 0, firstAt = -1;
        for (int p = 1; p <= 1200; ++p) {
            const IntraRefreshSweep::Step step = sweep.next(false, true);
            if (!step.refresh) continue;
            if (firstAt < 0) firstAt = p;
            ++inSweep;
        }
        CHECK_EQ(firstAt, 480);
        // Starts at 480 and 960: 120 pictures each, the second one whole
        // before picture 1200 is out.
        CHECK_EQ(inSweep, 240);
    }

    SECTION("IntraRefreshSweep — a repair in a sweep starts it over, wholly dirty");
    {
        IntraRefreshSweep sweep(4, -1);
        sweep.next(true, false);
        for (int p = 1; p < 4; ++p)
            sweep.next(false, true);
        CHECK_EQ(sweep.next(false, true).index, 0u); // picture 4
        CHECK_EQ(sweep.next(false, true).index, 1u); // picture 5
        // Picture 6 predicts from 3 (4 and 5 were lost): only the previous
        // picture may be partly clean, so the sweep starts over.
        const IntraRefreshSweep::Step repair = sweep.next(false, false);
        CHECK(repair.refresh);
        CHECK_EQ(repair.index, 0u);
        CHECK_EQ(repair.referenceDirty, 4u);
        CHECK_EQ(sweep.next(false, true).index, 1u);
        CHECK_EQ(sweep.next(false, true).referenceDirty, 2u); // index 2
    }

    SECTION("IntraRefreshSweep — an IDR in a sweep ends it; the next is due a distance on");
    {
        IntraRefreshSweep sweep(4, 8);
        sweep.next(true, false);
        for (int p = 1; p < 8; ++p)
            CHECK(!sweep.next(false, true).refresh);
        CHECK(sweep.next(false, true).refresh);  // picture 8, index 0
        CHECK(!sweep.next(true, false).refresh); // a keyframe asked for
        int firstAfter = -1;
        for (int p = 1; p <= 12 && firstAfter < 0; ++p)
            if (sweep.next(false, true).refresh) firstAfter = p;
        CHECK_EQ(firstAfter, 8);
    }

    SECTION("IntraRefreshSweep — a repair between sweeps changes nothing");
    {
        IntraRefreshSweep sweep(4, 8);
        sweep.next(true, false);
        for (int p = 1; p < 5; ++p)
            sweep.next(false, true);
        CHECK(!sweep.next(false, false).refresh); // picture 5, a repair
        CHECK(!sweep.next(false, true).refresh);
        CHECK(!sweep.next(false, true).refresh);
        CHECK(sweep.next(false, true).refresh); // picture 8, on schedule
    }
}
