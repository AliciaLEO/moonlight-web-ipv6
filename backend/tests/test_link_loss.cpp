/*
 * MoonlightWeb — Backend TNR. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * The bench's own losses on the video channel (LinkLoss.h, `loss=` and
 * `burst=`, plan Idées Punktfunk A0). What matters: off by default, the rate
 * asked for over a long run, bursts of exactly the length asked, and the same
 * sequence on every run — a bench compared against itself.
 */

#include "../src/streaming/LinkLoss.h"

#include "test_framework.h"

#include <vector>

void run_link_loss_tests()
{
    SECTION("LinkLoss — off unless asked");
    {
        LinkLoss off;
        CHECK(!off.active());
        for (int i = 0; i < 10000; ++i)
            CHECK(!off.dropNext());
        CHECK(off.dropped() == 0);
        off.configure(0, 5);
        CHECK(!off.active());
    }

    SECTION("LinkLoss — the rate asked for, in bursts of the length asked");
    {
        LinkLoss l;
        l.configure(10, 1); // 1 %
        CHECK(l.active());
        const int n = 200000;
        for (int i = 0; i < n; ++i)
            l.dropNext();
        // 2000 expected; a deterministic generator lands close.
        CHECK(l.dropped() > 1700 && l.dropped() < 2300);
        CHECK(l.dropped() == l.losses());

        LinkLoss b;
        b.configure(10, 4);
        std::vector<int> runs;
        int run = 0;
        for (int i = 0; i < n; ++i) {
            if (b.dropNext()) {
                run++;
            } else if (run > 0) {
                runs.push_back(run);
                run = 0;
            }
        }
        CHECK(!runs.empty());
        // Every loss takes exactly four (two losses back to back read as one
        // longer run: a multiple of four).
        for (int r : runs)
            CHECK(r % 4 == 0);
        CHECK(b.dropped() == 4 * b.losses());
    }

    SECTION("LinkLoss — the same sequence on every run, bounds pinned");
    {
        LinkLoss a, c;
        a.configure(50, 2);
        c.configure(50, 2);
        bool same = true;
        for (int i = 0; i < 5000; ++i)
            same = same && (a.dropNext() == c.dropNext());
        CHECK(same);
        LinkLoss all;
        all.configure(5000, 0); // pinned to 1000 ‰, burst to one
        for (int i = 0; i < 100; ++i)
            CHECK(all.dropNext());
        CHECK(all.losses() == 100);
    }
}
