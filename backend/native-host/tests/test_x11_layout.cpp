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

#include "input/linux/X11Layout.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace mw::native::input;

// The captured display among X's outputs (X11Layout.h). Measured on the
// UM790Pro (30/09/2026): X11 over two GPUs, the absolute pointer spread over
// the whole root while KMS knew one card's screen only. The recognition is the
// half that can go quietly wrong, so it is pure and tested everywhere; the X
// server itself is asked only where there is one (below).

void run_x11_layout_tests()
{
    SECTION("X11 layout — the captured connector among X's outputs");

    const std::vector<uint8_t> m27q = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x1C, 0x54};
    const std::vector<uint8_t> radeon = {0x00, 0xFF, 0xFF, 0xFF, 0xFF,
                                         0xFF, 0xFF, 0x00, 0x31, 0xD8};
    std::string how;

    // The morning topology: X on the GTX, the M27Q at 0,0; the Radeon a PRIME
    // output sink at 2560,0, named the amdgpu driver's way for a second GPU.
    // Its CRTC scans out from 0,0 of the sink's own copy — the KMS rectangle
    // says nothing of 2560 — and it is recognised by its EDID all the same.
    std::vector<X11Output> morning = {{"HDMI-0", 0, 0, 2560, 1440, m27q, 0},
                                      {"HDMI-A-1-0", 2560, 0, 1920, 1080, radeon, 95}};
    KmsIdentity sink{radeon, 95, 0, 0, 1920, 1080};
    CHECK_EQ(findX11Output(morning, sink, how), 1);
    CHECK_EQ(how, std::string("its EDID"));

    // The evening topology: X on the Radeon, its screen at 0,0 named from 0
    // ("HDMI-A-0" is the kernel's HDMI-A-1), the M27Q a sink on the GTX.
    std::vector<X11Output> evening = {{"HDMI-A-0", 0, 0, 1920, 1080, radeon, 95},
                                      {"HDMI-1-0", 1920, 0, 2560, 1440, m27q, 0}};
    KmsIdentity primary{radeon, 95, 0, 0, 1920, 1080};
    CHECK_EQ(findX11Output(evening, primary, how), 0);

    // Two monitors of one model and serial: the same EDID twice. The connector
    // id then tells them apart…
    std::vector<X11Output> twins = {{"DisplayPort-0", 0, 0, 1920, 1080, m27q, 91},
                                    {"DisplayPort-1", 1920, 0, 1920, 1080, m27q, 93}};
    KmsIdentity right{m27q, 93, 1920, 0, 1920, 1080};
    CHECK_EQ(findX11Output(twins, right, how), 1);
    CHECK_EQ(how, std::string("its EDID and connector id"));
    // …and without it (a driver that does not publish it), the place the CRTC
    // shows, which is the root's own on the GPU X renders on.
    std::vector<X11Output> twinsNoId = {{"DP-1", 0, 0, 1920, 1080, m27q, 0},
                                        {"DP-2", 1920, 0, 1920, 1080, m27q, 0}};
    CHECK_EQ(findX11Output(twinsNoId, right, how), 1);
    CHECK_EQ(how, std::string("its EDID and place on the root"));
    // Twins mirrored at the same place, nothing else to go by: no answer
    // rather than a guess — the caller keeps KMS.
    std::vector<X11Output> mirrored = {{"DP-1", 0, 0, 1920, 1080, m27q, 0},
                                       {"DP-2", 0, 0, 1920, 1080, m27q, 0}};
    KmsIdentity mirroredKms{m27q, 0, 0, 0, 1920, 1080};
    CHECK_EQ(findX11Output(mirrored, mirroredKms, how), -1);

    // A monitor without an EDID: the connector id alone.
    std::vector<X11Output> blind = {{"HDMI-1", 0, 0, 1280, 720, {}, 70},
                                    {"HDMI-2", 1280, 0, 1280, 720, {}, 72}};
    KmsIdentity noEdid{{}, 72, 0, 0, 1280, 720};
    CHECK_EQ(findX11Output(blind, noEdid, how), 1);
    CHECK_EQ(how, std::string("its connector id"));

    // The same connector id on two cards (ids are per device): ambiguous by id,
    // settled by the place.
    std::vector<X11Output> twoCards = {{"HDMI-A-0", 0, 0, 1920, 1080, {}, 95},
                                       {"HDMI-1-0", 1920, 0, 1920, 1080, {}, 95}};
    KmsIdentity firstCard{{}, 95, 0, 0, 1920, 1080};
    CHECK_EQ(findX11Output(twoCards, firstCard, how), 0);
    CHECK_EQ(how, std::string("its place on the root"));

    // An EDID X does not have (the driver read another, or none): not taken
    // for a match, and the other clues still count.
    std::vector<X11Output> otherEdid = {{"HDMI-A-0", 0, 0, 1920, 1080, m27q, 95}};
    CHECK_EQ(findX11Output(otherEdid, primary, how), 0);
    CHECK_EQ(how, std::string("its connector id"));

    // Nothing in common: no answer. And an output without a size is never one.
    KmsIdentity stranger{{0x01}, 7, 5000, 0, 800, 600};
    CHECK_EQ(findX11Output(evening, stranger, how), -1);
    std::vector<X11Output> unsized = {{"HDMI-A-0", 0, 0, 0, 0, radeon, 95}};
    CHECK_EQ(findX11Output(unsized, primary, how), -1);
    CHECK_EQ(findX11Output({}, primary, how), -1);

    // The portal route: its monitor by the place the portal named, or, when it
    // named none, by its size when only one output has it.
    CHECK_EQ(findX11OutputAt(evening, 1920, 0, 4480, 1440), 1);
    CHECK_EQ(findX11OutputAt(evening, 0, 0, 2560, 1440), -1);  // a size, not a place
    CHECK_EQ(findX11OutputAt(mirrored, 0, 0, 1920, 1080), -1); // ambiguous
    CHECK_EQ(findX11OutputSized(evening, 2560, 1440), 1);
    CHECK_EQ(findX11OutputSized(twins, 1920, 1080), -1);
    CHECK_EQ(findX11OutputSized(evening, 800, 600), -1);

#if defined(__linux__)
    // The X server, where there is one: the run on an X11 desktop prints its
    // own answer — and the EDID and connector id its driver publishes, which
    // is what the recognition above rests on. Anywhere else, the reason.
    SECTION("X11 layout — the X server's answer, on a host that has one");
    std::vector<X11Output> live;
    int rootWidth = 0;
    int rootHeight = 0;
    std::string displayUsed;
    std::string why;
    if (X11Layout::read(live, rootWidth, rootHeight, displayUsed, why)) {
        std::printf("    X layout on \"%s\", root %dx%d:\n", displayUsed.c_str(), rootWidth,
                    rootHeight);
        for (const X11Output& out : live)
            std::printf("      %s %d,%d %dx%d, EDID %zu bytes, connector id %u\n", out.name.c_str(),
                        out.x, out.y, out.width, out.height, out.edid.size(), out.connectorId);
        CHECK(!live.empty());
        CHECK(rootWidth > 0);
        CHECK(rootHeight > 0);
        for (const X11Output& out : live) {
            CHECK(out.width > 0);
            CHECK(out.height > 0);
            CHECK(out.x + out.width <= rootWidth);
            CHECK(out.y + out.height <= rootHeight);
        }
    } else {
        std::printf("    no X layout here: %s\n", why.c_str());
    }
#endif
}
