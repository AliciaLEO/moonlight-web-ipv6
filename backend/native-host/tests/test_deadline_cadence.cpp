/*
 * MoonlightWeb — native capture & encoding engine.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "core/DeadlineCadence.h"
#include "native_test_framework.h"

#include <cmath>
#include <cstdlib>

using mw::native::DeadlineCadence;

namespace {

constexpr double kPeriod120 = 1000000.0 / 120.0;

} // namespace

void run_deadline_cadence_tests()
{
    SECTION("DeadlineCadence — nothing heard, nothing aimed");
    {
        DeadlineCadence d;
        CHECK(!d.fresh(0));
        CHECK(!d.next(1000000).valid());
        CHECK_EQ(d.grids(), 0);
    }

    SECTION("DeadlineCadence — a grid that makes no sense is not kept");
    {
        DeadlineCadence d;
        const int64_t now = 50000000;
        CHECK(!d.note(1000.0, now, 10000, now));      // 1000 Hz
        CHECK(!d.note(200000.0, now, 10000, now));    // 5 Hz
        CHECK(!d.note(kPeriod120, now, 0, now));      // no lead
        CHECK(!d.note(kPeriod120, now, 600000, now)); // over half a second
        CHECK(!d.note(kPeriod120, now + 20000000, 10000, now));
        CHECK(!d.fresh(now));
        CHECK(d.note(kPeriod120, now, 10000, now));
        CHECK(d.fresh(now));
        CHECK_EQ(d.grids(), 1);
    }

    SECTION("DeadlineCadence — the picture is taken one lead before the refresh");
    {
        DeadlineCadence d;
        d.note(kPeriod120, 1000000, 12000, 1000000);
        // The first refresh whose instant is not behind: 1 016 667, taken at
        // 1 004 667 — 12 ms before it.
        const DeadlineCadence::Aim a = d.next(1000000);
        CHECK(a.valid());
        CHECK_EQ(a.refreshUs, 1016667);
        CHECK_EQ(a.captureUs, 1004667);
    }

    SECTION("DeadlineCadence — each refresh is served once");
    {
        DeadlineCadence d;
        d.note(kPeriod120, 1000000, 12000, 1000000);
        const DeadlineCadence::Aim a = d.next(1000000);
        d.served(a);
        // Asked again at the same instant: the refresh after.
        const DeadlineCadence::Aim b = d.next(a.captureUs);
        CHECK_EQ(b.refreshUs, 1025000);
        CHECK_EQ(b.captureUs - a.captureUs, 8333);
    }

    SECTION("DeadlineCadence — a late wake-up keeps its refresh within kLateUs");
    {
        DeadlineCadence d;
        d.note(kPeriod120, 1000000, 12000, 1000000);
        const DeadlineCadence::Aim a = d.next(1000000);
        CHECK_EQ(d.next(a.captureUs + DeadlineCadence::kLateUs / 2).refreshUs, a.refreshUs);
        // Later than that: the refresh is given up for the next one.
        CHECK_EQ(d.next(a.captureUs + DeadlineCadence::kLateUs + 1).refreshUs, 1025000);
    }

    SECTION("DeadlineCadence — a grid not heard again goes stale");
    {
        DeadlineCadence d;
        d.note(kPeriod120, 1000000, 12000, 1000000);
        CHECK(d.fresh(1000000 + DeadlineCadence::kStaleUs - 1));
        CHECK(!d.fresh(1000000 + DeadlineCadence::kStaleUs));
        CHECK(!d.next(1000000 + DeadlineCadence::kStaleUs).valid());
    }

    SECTION("DeadlineCadence — a grid anchored on a later refresh serves nothing twice");
    {
        DeadlineCadence d;
        d.note(kPeriod120, 1000000, 12000, 1000000);
        const DeadlineCadence::Aim a = d.next(1000000);
        d.served(a);
        // The next grid names a refresh 60 periods on, with a µs of rounding:
        // the refresh just served is still told apart from the next one.
        d.note(kPeriod120, 1000000 + 500001, 12000, 1004000);
        const DeadlineCadence::Aim b = d.next(a.captureUs);
        CHECK(b.refreshUs > a.refreshUs + 8000);
        CHECK(b.refreshUs < a.refreshUs + 8700);
    }

    SECTION("DeadlineCadence — one picture per client refresh, a second long");
    {
        // A loop that sleeps to each instant, wakes up to 300 µs late, and
        // serves: 120 pictures for a 120 Hz client, one period apart.
        DeadlineCadence d;
        int64_t now = 5000000;
        d.note(kPeriod120, now, 15000, now);
        int served = 0;
        int64_t lastRefresh = 0;
        bool evenlySpaced = true;
        std::srand(3);
        while (now < 6000000) {
            // Every half second the client re-anchors on one of its refreshes,
            // the latest before now: the same grid, a later phase.
            if (served % 60 == 0) {
                const int64_t k = static_cast<int64_t>((now - 5000000) / kPeriod120);
                d.note(kPeriod120, 5000000 + std::llround(k * kPeriod120), 15000, now);
            }
            const DeadlineCadence::Aim a = d.next(now);
            if (a.captureUs > now) now = a.captureUs;
            now += std::rand() % 300;
            const DeadlineCadence::Aim b = d.next(now);
            if (lastRefresh != 0) {
                const int64_t step = b.refreshUs - lastRefresh;
                evenlySpaced = evenlySpaced && step >= 8333 && step <= 8334;
            }
            lastRefresh = b.refreshUs;
            d.served(b);
            served++;
        }
        CHECK(served >= 119 && served <= 121);
        CHECK(evenlySpaced);
    }
}
