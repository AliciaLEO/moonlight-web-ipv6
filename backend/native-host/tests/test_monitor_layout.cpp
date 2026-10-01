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

#include "capture/linux/MonitorLayout.h"

#include <string>
#include <vector>

using namespace mw::native::capture;

// The portal's virtual monitor made GNOME's primary (MonitorLayout.h): which
// monitor is this session's, and the layout that puts it on the left of the
// others without switching any of them off. Pure arithmetic, so every
// platform's run covers it; the live layout is test_linux_session's.

namespace {

LayoutMonitor physical(const char* connector, int width, int height)
{
    LayoutMonitor m;
    m.connector = connector;
    m.vendor = "GSM";
    m.modeId = std::to_string(width) + "x" + std::to_string(height) + "@60";
    m.width = width;
    m.height = height;
    return m;
}

LayoutMonitor virtualMonitor(const char* connector, int width, int height)
{
    LayoutMonitor m = physical(connector, width, height);
    m.vendor = "MetaVendor";
    m.product = "Virtual remote monitor";
    return m;
}

LayoutLogical at(const char* connector, int x, int y, bool primary = false, double scale = 1.0,
                 uint32_t transform = 0)
{
    LayoutLogical l;
    l.x = x;
    l.y = y;
    l.scale = scale;
    l.transform = transform;
    l.primary = primary;
    l.connectors = {connector};
    return l;
}

const LayoutLogical* placeOf(const std::vector<LayoutLogical>& layout, const std::string& connector)
{
    for (const LayoutLogical& l : layout)
        for (const std::string& c : l.connectors)
            if (c == connector) return &l;
    return nullptr;
}

} // namespace

void run_monitor_layout_tests()
{
    SECTION("Monitor layout — the virtual monitor as GNOME's primary, nothing switched off");

    // The UM790Pro: the M27Q primary, the HDMI dummy at its right, and the
    // portal's monitor added at the right end — where GNOME leaves it.
    DisplayLayout um;
    um.monitors = {physical("DP-1", 2560, 1440), physical("HDMI-1", 1920, 1080),
                   virtualMonitor("Meta-0", 1600, 900)};
    um.logical = {at("DP-1", 0, 0, true), at("HDMI-1", 2560, 0), at("Meta-0", 4480, 0)};
    const std::vector<std::string> before = {"DP-1", "HDMI-1"};
    CHECK_EQ(findSessionVirtual(um, before, 1600, 900), std::string("Meta-0"));

    std::vector<LayoutLogical> out;
    bool unchanged = true;
    std::string why;
    CHECK(primaryLayout(um, "Meta-0", out, unchanged, why));
    CHECK(!unchanged);
    CHECK_EQ(out.size(), static_cast<size_t>(3));
    const LayoutLogical* v = placeOf(out, "Meta-0");
    const LayoutLogical* dp = placeOf(out, "DP-1");
    const LayoutLogical* hdmi = placeOf(out, "HDMI-1");
    CHECK(v && dp && hdmi);
    if (v && dp && hdmi) {
        CHECK(v->primary);
        CHECK_EQ(v->x, 0);
        CHECK_EQ(v->y, 0);
        // The physical screens keep their places among themselves, moved
        // right by the virtual monitor's width; neither is primary any more.
        CHECK(!dp->primary);
        CHECK(!hdmi->primary);
        CHECK_EQ(dp->x, 1600);
        CHECK_EQ(dp->y, 0);
        CHECK_EQ(hdmi->x, 4160);
        CHECK_EQ(hdmi->y, 0);
    }

    // Which monitor is this session's. Another stream's monitor of the same
    // size was there before: it is not ours.
    DisplayLayout two = um;
    two.monitors.push_back(virtualMonitor("Meta-1", 1600, 900));
    two.logical.push_back(at("Meta-1", 6080, 0));
    CHECK_EQ(findSessionVirtual(two, {"DP-1", "HDMI-1", "Meta-0"}, 1600, 900),
             std::string("Meta-1"));
    // Two new ones of the size: no guess.
    CHECK_EQ(findSessionVirtual(two, before, 1600, 900), std::string());
    // Another size, or a physical screen of the size: not ours.
    CHECK_EQ(findSessionVirtual(um, before, 1920, 1080), std::string());
    CHECK_EQ(findSessionVirtual(um, before, 2560, 1440), std::string());
    // A restart: the old monitor was still listed when the new one was asked
    // for, and Mutter gave the freed name to the new one — the only one there.
    CHECK_EQ(findSessionVirtual(um, {"DP-1", "HDMI-1", "Meta-0"}, 1600, 900),
             std::string("Meta-0"));

    // Screens stacked: the virtual monitor sits level with the top of the
    // leftmost column, and touches it.
    DisplayLayout stacked;
    stacked.monitors = {physical("HDMI-1", 1920, 1080), physical("DP-1", 2560, 1440),
                        virtualMonitor("Meta-0", 1600, 900)};
    stacked.logical = {at("HDMI-1", 0, 0), at("DP-1", 0, 1080, true), at("Meta-0", 2560, 1080)};
    CHECK(primaryLayout(stacked, "Meta-0", out, unchanged, why));
    v = placeOf(out, "Meta-0");
    hdmi = placeOf(out, "HDMI-1");
    dp = placeOf(out, "DP-1");
    CHECK(v && dp && hdmi);
    if (v && dp && hdmi) {
        CHECK_EQ(v->x, 0);
        CHECK_EQ(v->y, 0);
        CHECK_EQ(hdmi->x, 1600);
        CHECK_EQ(hdmi->y, 0);
        CHECK_EQ(dp->x, 1600);
        CHECK_EQ(dp->y, 1080);
    }

    // A column that starts lower: the monitor goes level with it, and the
    // layout still begins at the origin (the monitor above holds y = 0).
    DisplayLayout offset;
    offset.monitors = {physical("DP-1", 2560, 1440), physical("HDMI-1", 1920, 1080),
                       virtualMonitor("Meta-0", 1600, 900)};
    offset.logical = {at("DP-1", 0, 400, true), at("HDMI-1", 2560, 0), at("Meta-0", 4480, 0)};
    CHECK(primaryLayout(offset, "Meta-0", out, unchanged, why));
    v = placeOf(out, "Meta-0");
    CHECK(v != nullptr);
    if (v) CHECK_EQ(v->y, 400);

    // A scaled virtual monitor takes its logical width — unless the layout
    // counts physical pixels — and a turned one its height.
    DisplayLayout scaled;
    scaled.monitors = {physical("DP-1", 2560, 1440), virtualMonitor("Meta-0", 3840, 2160)};
    scaled.logical = {at("DP-1", 0, 0, true), at("Meta-0", 2560, 0, false, 2.0)};
    CHECK(primaryLayout(scaled, "Meta-0", out, unchanged, why));
    dp = placeOf(out, "DP-1");
    if (dp) CHECK_EQ(dp->x, 1920);
    v = placeOf(out, "Meta-0");
    if (v) CHECK(v->scale == 2.0);
    scaled.physicalLayout = true;
    CHECK(primaryLayout(scaled, "Meta-0", out, unchanged, why));
    dp = placeOf(out, "DP-1");
    if (dp) CHECK_EQ(dp->x, 3840);
    DisplayLayout turned;
    turned.monitors = {physical("DP-1", 2560, 1440), virtualMonitor("Meta-0", 1600, 900)};
    turned.logical = {at("DP-1", 0, 0, true), at("Meta-0", 2560, 0, false, 1.0, 1)};
    CHECK(primaryLayout(turned, "Meta-0", out, unchanged, why));
    dp = placeOf(out, "DP-1");
    if (dp) CHECK_EQ(dp->x, 900);

    // The others keep their own scale and transform.
    DisplayLayout keep;
    keep.monitors = {physical("eDP-1", 2880, 1800), virtualMonitor("Meta-0", 1600, 900)};
    keep.logical = {at("eDP-1", 0, 0, true, 2.0, 0), at("Meta-0", 1440, 0)};
    CHECK(primaryLayout(keep, "Meta-0", out, unchanged, why));
    const LayoutLogical* edp = placeOf(out, "eDP-1");
    CHECK(edp != nullptr);
    if (edp) {
        CHECK(edp->scale == 2.0);
        CHECK_EQ(edp->x, 1600);
    }

    // Nothing to do: alone (a host without a screen), or already there.
    DisplayLayout alone;
    alone.monitors = {virtualMonitor("Meta-0", 1600, 900)};
    alone.logical = {at("Meta-0", 0, 0, true)};
    CHECK(primaryLayout(alone, "Meta-0", out, unchanged, why));
    CHECK(unchanged);
    unchanged = false;
    DisplayLayout done = um;
    done.logical = {at("Meta-0", 0, 0, true), at("DP-1", 1600, 0), at("HDMI-1", 4160, 0)};
    CHECK(primaryLayout(done, "Meta-0", out, unchanged, why));
    CHECK(unchanged);

    // Refused, and said why: not in the layout, mirrored, or sitting between
    // two screens that would no longer touch without it.
    CHECK(!primaryLayout(um, "Meta-7", out, unchanged, why));
    CHECK(why.find("not in the layout") != std::string::npos);
    DisplayLayout mirrored = um;
    mirrored.logical = {at("DP-1", 0, 0, true), at("HDMI-1", 2560, 0)};
    mirrored.logical[1].connectors.push_back("Meta-0");
    CHECK(!primaryLayout(mirrored, "Meta-0", out, unchanged, why));
    CHECK(why.find("mirrors") != std::string::npos);
    DisplayLayout between = um;
    between.logical = {at("DP-1", 0, 0, true), at("Meta-0", 2560, 0), at("HDMI-1", 4160, 0)};
    CHECK(!primaryLayout(between, "Meta-0", out, unchanged, why));
    CHECK(why.find("would not touch") != std::string::npos);
    CHECK(out.empty());

    // Mutter's adjacency: a shared stretch of edge, never a corner alone.
    CHECK(layoutAdjacent(0, 0, 100, 100, 100, 50, 100, 100));
    CHECK(layoutAdjacent(0, 0, 100, 100, 50, 100, 100, 100));
    CHECK(!layoutAdjacent(0, 0, 100, 100, 100, 100, 100, 100));
    CHECK(!layoutAdjacent(0, 0, 100, 100, 101, 0, 100, 100));
}
