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

// What the guests of the shared feed ask of the one encoder they share: their
// keyframes rationed, their link reports folded into the slowest one's.

#include "mw/native/FeedArbiter.h"
#include "native_test_framework.h"

using namespace mw::native;

void run_feed_arbiter_tests()
{
    SECTION("FeedArbiter — three joins inside a window cost one keyframe");
    {
        FeedArbiter a;
        CHECK(!a.keyframeDue(0)); // nobody asked
        a.requestKeyframe(1, 1000);
        a.requestKeyframe(2, 1100);
        CHECK(!a.keyframeDue(1200)); // the window is still gathering
        a.requestKeyframe(3, 1240);
        CHECK(a.keyframeDue(1250));
        CHECK(!a.keyframeDue(1260)); // served: nothing pending
        CHECK_EQ(a.asked(), int64_t(3));
        CHECK_EQ(a.served(), int64_t(1));
    }

    SECTION("FeedArbiter — never two keyframes within a second, whoever asks");
    {
        FeedArbiter a;
        a.requestKeyframe(1, 0);
        CHECK(a.keyframeDue(250));
        a.requestKeyframe(2, 300);
        CHECK(!a.keyframeDue(600));  // its window has closed, but the gap has not
        CHECK(!a.keyframeDue(1200)); // 950 ms since the last
        CHECK(a.keyframeDue(1250));
        CHECK_EQ(a.served(), int64_t(2));
    }

    SECTION("FeedArbiter — the link is the slowest guest's, once per period");
    {
        FeedArbiter a;
        LinkFeedback out;
        CHECK(!a.linkDue(0, out)); // no report yet
        LinkFeedback calm;
        calm.owdRiseMs = 2;
        calm.receivedFps = 60;
        LinkFeedback slow;
        slow.owdRiseMs = 45;
        slow.gaps = 1;
        slow.evictions = 2;
        slow.receivedFps = 41;
        a.report(1, calm, 100);
        a.report(2, slow, 120);
        a.report(3, calm, 130);
        CHECK(a.linkDue(500, out));
        CHECK_EQ(out.owdRiseMs, 45);
        CHECK_EQ(out.gaps, 1);
        CHECK_EQ(out.evictions, 2);
        CHECK_EQ(out.receivedFps, 41);
        CHECK(!out.resumed);
        // Consumed: the next period starts empty, and is not due before 500 ms.
        CHECK(!a.linkDue(700, out));
        a.report(1, calm, 800);
        CHECK(!a.linkDue(900, out));
        CHECK(a.linkDue(1000, out));
        CHECK_EQ(out.owdRiseMs, 2);
    }

    SECTION("FeedArbiter — two reports from one guest in a period: its worse moment");
    {
        FeedArbiter a;
        LinkFeedback first;
        first.owdRiseMs = 40;
        first.gaps = 1;
        first.receivedFps = 58;
        LinkFeedback second;
        second.owdRiseMs = 5;
        second.gaps = 2;
        second.receivedFps = 50;
        a.report(7, first, 0);
        a.report(7, second, 250);
        LinkFeedback out;
        CHECK(a.linkDue(500, out));
        CHECK_EQ(out.owdRiseMs, 40);
        CHECK_EQ(out.gaps, 3);
        CHECK_EQ(out.receivedFps, 50);
    }

    SECTION("FeedArbiter — back from the background only when every guest is");
    {
        FeedArbiter a;
        LinkFeedback back;
        back.resumed = true;
        LinkFeedback live;
        a.report(1, back, 0);
        a.report(2, live, 0);
        LinkFeedback out;
        CHECK(a.linkDue(500, out));
        CHECK(!out.resumed);
        a.report(1, back, 600);
        CHECK(a.linkDue(1000, out));
        CHECK(out.resumed);
    }

    SECTION("FeedArbiter — a guest that left no longer counts");
    {
        FeedArbiter a;
        LinkFeedback slow;
        slow.owdRiseMs = 90;
        a.report(4, slow, 0);
        a.leave(4);
        LinkFeedback out;
        CHECK(!a.linkDue(500, out));
    }

    SECTION("FeedArbiter — the pointer is held on the display only for a guest with no other");
    {
        FeedArbiter a;
        CHECK(!a.keepPointerOnDisplay()); // nobody said: free, as for a desktop page
        a.setPointerInPicture(1, false);  // desktop mode: its own pointer
        CHECK(!a.keepPointerOnDisplay());
        a.setPointerInPicture(2, true); // a phone's trackpad
        CHECK(a.keepPointerOnDisplay());
        a.setPointerInPicture(1, true);  // the first one locks its pointer too...
        a.setPointerInPicture(2, false); // ...as the phone draws its own
        CHECK(a.keepPointerOnDisplay());
        a.leave(1);
        CHECK(!a.keepPointerOnDisplay());
    }
}
