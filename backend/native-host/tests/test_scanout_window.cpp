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

#include "native_test_framework.h"

#include "capture/ScanoutWindow.h"

using namespace mw::native::capture;

// Which part of its buffer a display scans out (ScanoutWindow.h). An X11
// desktop over several screens scans all of them out of the root window's one
// buffer, each from where it sits: the capture must take that window, and still
// read anything else as the mode change it is. The numbers are the UM790Pro's
// of 30/09/2026: a 1920×1080 screen at (0,0) and a 2560×1440 one at (1920,0),
// a 4480×1440 root.
void run_scanout_window_tests()
{
    SECTION("KMS — a display's window of its buffer: all of it, or its part of an X11 root");

    // The ordinary case, Wayland or a lone X11 screen: buffer and picture are
    // one. With SRC_* and without (a driver that has none reads 0).
    ScanoutWindow w = scanoutWindow(1920, 1080, 1920, 1080, 0, 0, 1920, 1080);
    CHECK(w.usable);
    CHECK(!w.windowed);
    w = scanoutWindow(1920, 1080, 1920, 1080, 0, 0, 0, 0);
    CHECK(w.usable);
    CHECK(!w.windowed);

    // The left screen of the root: from (0,0), the buffer wider and taller.
    w = scanoutWindow(4480, 1440, 1920, 1080, 0, 0, 1920, 1080);
    CHECK(w.usable);
    CHECK(w.windowed);
    CHECK_EQ(w.x, 0);
    CHECK_EQ(w.y, 0);

    // Its neighbour: the same buffer, from where it sits.
    w = scanoutWindow(4480, 1440, 2560, 1440, 1920, 0, 2560, 1440);
    CHECK(w.usable);
    CHECK(w.windowed);
    CHECK_EQ(w.x, 1920);
    CHECK_EQ(w.y, 0);

    // A third screen further right, the TV that came on (8320×2160 root).
    w = scanoutWindow(8320, 2160, 3840, 2160, 4480, 0, 3840, 2160);
    CHECK(w.usable);
    CHECK_EQ(w.x, 4480);

    // A window that reaches past the buffer is no window: refused.
    w = scanoutWindow(4480, 1440, 2560, 1440, 2000, 0, 2560, 1440);
    CHECK(!w.usable);
    w = scanoutWindow(4480, 1440, 1920, 1080, 0, 400, 1920, 1080);
    CHECK(!w.usable);

    // A plane that scales its source — SRC not at the mode's size — shows a
    // picture this capture would have to scale back: the mode change it
    // most likely is, as before.
    w = scanoutWindow(3840, 2160, 1920, 1080, 0, 0, 3840, 2160);
    CHECK(!w.usable);
    w = scanoutWindow(4480, 1440, 1920, 1080, 0, 0, 1280, 720);
    CHECK(!w.usable);

    // A bigger buffer with no SRC_* to say where the display is: nothing to
    // go on, refused — what the capture always did with such a buffer.
    w = scanoutWindow(4480, 1440, 1920, 1080, 0, 0, 0, 0);
    CHECK(!w.usable);

    // The mode changed and the buffer did not follow yet: refused, the
    // caller rebuilds at the new size.
    w = scanoutWindow(1920, 1080, 2560, 1440, 0, 0, 1920, 1080);
    CHECK(!w.usable);

    // Degenerate sizes, negative positions.
    CHECK(!scanoutWindow(0, 0, 1920, 1080, 0, 0, 1920, 1080).usable);
    CHECK(!scanoutWindow(1920, 1080, 0, 0, 0, 0, 0, 0).usable);
    CHECK(!scanoutWindow(4480, 1440, 1920, 1080, -1, 0, 1920, 1080).usable);
}
