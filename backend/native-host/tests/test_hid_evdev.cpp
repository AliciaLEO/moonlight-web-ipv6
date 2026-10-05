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

#include "input/HidDescriptor.h"
#include "input/HidEvdev.h"
#include "input/VirtualHid.h"
#include "mw/native/HidPassthrough.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace mw::native;
using namespace mw::native::input::hid;

namespace {

ReportItem axis(std::vector<uint32_t> usages, uint16_t size, int32_t lmin, int32_t lmax)
{
    ReportItem it;
    it.reportCount = static_cast<uint16_t>(usages.size());
    it.usages = std::move(usages);
    it.reportSize = size;
    it.logicalMinimum = lmin;
    it.logicalMaximum = lmax;
    return it;
}

ReportItem buttons(uint32_t count)
{
    ReportItem it;
    it.isRange = true;
    it.usageMinimum = 0x90001;
    it.usageMaximum = 0x90000 + count;
    it.reportSize = 1;
    it.reportCount = static_cast<uint16_t>(count);
    it.logicalMaximum = 1;
    return it;
}

ReportItem padding(uint16_t bits)
{
    ReportItem it;
    it.isConstant = true;
    it.reportSize = bits;
    it.reportCount = 1;
    return it;
}

/// The G923's joystick (report 1, 10 bytes): hat, 23 buttons, padding, a
/// 16-bit wheel, three 8-bit pedals, 3 vendor bits and padding.
Parsed g923()
{
    Report r;
    r.reportId = 1;
    ReportItem hat = axis({0x10039}, 4, 0, 7);
    hat.hasNull = true;
    r.items = {hat, buttons(23), padding(5), axis({0x10030}, 16, 0, 65535)};
    for (uint32_t u : {0x10031u, 0x10032u, 0x10035u})
        r.items.push_back(axis({u}, 8, 0, 255));
    ReportItem vendor = axis({0xFF000001, 0xFF000002, 0xFF000003}, 1, 0, 1);
    r.items.push_back(vendor);
    r.items.push_back(padding(5));
    Collection c;
    c.usagePage = 0x01;
    c.usage = 0x04;
    c.inputReports = {r};
    return parse(encode({c}));
}

/// A radio as Chrome shows the TX12 (game pad, no report id): 24 buttons and
/// eight 0..2047 axes, the last two both Slider.
Parsed radio()
{
    Report r;
    r.items = {buttons(24),
               axis({0x10030, 0x10031, 0x10032, 0x10033, 0x10034, 0x10035, 0x10036, 0x10036}, 16, 0,
                    2047)};
    Collection c;
    c.usagePage = 0x01;
    c.usage = 0x05;
    c.inputReports = {r};
    return parse(encode({c}));
}

bool hasAxis(const EvdevMap& m, uint16_t code)
{
    return std::any_of(m.axes.begin(), m.axes.end(),
                       [code](const EvdevAxis& a) { return a.code == code; });
}

int32_t field(const HidFfb& o, const std::string& name)
{
    for (const auto& [k, v] : o.fields)
        if (k == name) return v;
    return -999;
}

/// A backend that services force feedback itself, as UinputWheel does.
class UinputFake : public input::IVirtualHid
{
public:
    explicit UinputFake(UinputFake** self) { *self = this; }
    bool forceFeedbackByPid() const override { return false; }
    bool create(const input::VirtualHidIdentity& id, std::string&) override
    {
        descriptor = id.descriptor;
        return true;
    }
    bool input(const uint8_t*, size_t) override { return true; }
    void destroy() override {}
    void upload(const LinuxFfEffect& e)
    {
        for (const HidFfb& op : ffbUpload(e))
            if (m_onFfb) m_onFfb(op);
    }
    std::vector<uint8_t> descriptor;
};

} // namespace

void run_hid_evdev_tests()
{
    SECTION("HID evdev — the G923's axes, hat and 23 buttons, named as hid-input names them");
    {
        const EvdevMap m = evdevMap(g923());
        CHECK(m.numbered);
        for (uint16_t c : {ev::AbsX, ev::AbsY, ev::AbsZ, ev::AbsRz, ev::AbsHat0X,
                           static_cast<uint16_t>(ev::AbsHat0X + 1)})
            CHECK(hasAxis(m, c));
        CHECK_EQ(m.axes.size(), static_cast<size_t>(6));
        CHECK_EQ(m.keys.size(), static_cast<size_t>(23));
        CHECK_EQ(m.keys.front(), ev::BtnJoystick);
        CHECK_EQ(m.keys[15], static_cast<uint16_t>(ev::BtnJoystick + 15));
        CHECK_EQ(m.keys[16], ev::BtnTriggerHappy);
        CHECK_EQ(m.keys.back(), static_cast<uint16_t>(ev::BtnTriggerHappy + 6));
    }

    SECTION("HID evdev — a report becomes the events that changed, the hat in two axes");
    {
        const EvdevMap m = evdevMap(g923());
        std::vector<int32_t> state;
        std::vector<EvdevEvent> out;
        // hat null, no button, wheel 0x8000, pedals released (255)
        const uint8_t first[11] = {1, 0x08, 0, 0, 0, 0x00, 0x80, 0xff, 0xff, 0xff, 0};
        evdevDecode(m, first, sizeof first, state, out);
        CHECK_EQ(out.size(), static_cast<size_t>(2 + 23 + 4)); // everything once
        out.clear();
        // hat east (2), button 1, wheel 0x1234, throttle (Y) pressed
        const uint8_t next[11] = {1, 0x12, 0, 0, 0, 0x34, 0x12, 0x00, 0xff, 0xff, 0};
        evdevDecode(m, next, sizeof next, state, out);
        CHECK_EQ(out.size(), static_cast<size_t>(5));
        bool hatX = false, button = false, wheel = false, throttle = false;
        for (const EvdevEvent& e : out) {
            if (e.type == ev::Abs && e.code == ev::AbsHat0X) hatX = e.value == 1;
            if (e.type == ev::Key && e.code == ev::BtnJoystick) button = e.value == 1;
            if (e.type == ev::Abs && e.code == ev::AbsX) wheel = e.value == 0x1234;
            if (e.type == ev::Abs && e.code == ev::AbsY) throttle = e.value == 0;
        }
        CHECK(hatX && button && wheel && throttle);
        out.clear();
        evdevDecode(m, next, sizeof next, state, out);
        CHECK(out.empty());
        const uint8_t other[3] = {2, 0, 0}; // another report id
        evdevDecode(m, other, sizeof other, state, out);
        CHECK(out.empty());
    }

    SECTION("HID evdev — a game pad's buttons from BTN_GAMEPAD, a second Slider to ABS_MISC");
    {
        const EvdevMap m = evdevMap(radio());
        CHECK(!m.numbered);
        CHECK_EQ(m.keys.front(), ev::BtnGamepad);
        CHECK_EQ(m.keys.size(), static_cast<size_t>(24));
        CHECK_EQ(m.axes.size(), static_cast<size_t>(8));
        CHECK(hasAxis(m, ev::AbsThrottle));
        CHECK(hasAxis(m, ev::AbsMisc));
    }

    SECTION("HID evdev — a constant force upload: header, envelope, magnitude");
    {
        LinuxFfEffect e;
        e.type = ev::FfConstant;
        e.id = 2;
        e.direction = 0xC000; // 270°: to the left on the wheel
        e.length = 0;
        e.delay = 10;
        e.level = 0x4000;
        const std::vector<HidFfb> ops = ffbUpload(e);
        CHECK_EQ(ops.size(), static_cast<size_t>(3));
        CHECK_EQ(ops[0].op, std::string("effect"));
        CHECK_EQ(ops[0].effect, 3);
        CHECK_EQ(ops[0].kind, std::string("constant"));
        CHECK_EQ(field(ops[0], "duration"), -1);
        CHECK_EQ(field(ops[0], "direction"), 27000);
        CHECK_EQ(field(ops[0], "directionEnable"), 1);
        CHECK_EQ(ops[1].op, std::string("envelope"));
        CHECK_EQ(ops[2].op, std::string("constant"));
        CHECK_EQ(field(ops[2], "magnitude"), 5000);
    }

    SECTION("HID evdev — conditions, periodic waves, and what a wheel cannot play");
    {
        LinuxFfEffect s;
        s.type = ev::FfSpring;
        s.length = 500;
        s.rightCoeff = 0x7FFF;
        s.leftCoeff = -0x7FFF;
        s.rightSaturation = 0xFFFF;
        s.leftSaturation = 0x7FFF;
        const std::vector<HidFfb> spring = ffbUpload(s);
        CHECK_EQ(spring.size(), static_cast<size_t>(2)); // no envelope
        CHECK_EQ(field(spring[0], "duration"), 500);
        CHECK_EQ(field(spring[0], "directionEnable"), 0);
        CHECK_EQ(spring[1].op, std::string("condition"));
        CHECK_EQ(field(spring[1], "positiveCoefficient"), 10000);
        CHECK_EQ(field(spring[1], "negativeCoefficient"), -10000);
        CHECK_EQ(field(spring[1], "positiveSaturation"), 10000);
        CHECK_EQ(field(spring[1], "negativeSaturation"), 4999);

        LinuxFfEffect w;
        w.type = ev::FfPeriodic;
        w.waveform = ev::FfSine;
        w.magnitude = -0x7FFF;
        w.period = 50;
        w.phase = 0x4000;
        const std::vector<HidFfb> sine = ffbUpload(w);
        CHECK_EQ(sine[0].kind, std::string("sine"));
        CHECK_EQ(field(sine[2], "magnitude"), 10000);
        CHECK_EQ(field(sine[2], "phase"), 27000); // 90° + half a turn
        CHECK_EQ(field(sine[2], "period"), 50);

        LinuxFfEffect rumble;
        rumble.type = ev::FfRumble;
        CHECK(ffbUpload(rumble).empty());
        LinuxFfEffect custom;
        custom.type = ev::FfPeriodic;
        custom.waveform = 0x5d;
        CHECK(ffbUpload(custom).empty());
    }

    SECTION("HID evdev — play, stop, erase and gain");
    {
        const HidFfb play = ffbPlay(0, 1);
        CHECK_EQ(play.op, std::string("start"));
        CHECK_EQ(play.effect, 1);
        CHECK_EQ(field(play, "loops"), 1);
        CHECK_EQ(ffbPlay(4, 0).op, std::string("stop"));
        CHECK_EQ(ffbErase(4).op, std::string("free"));
        CHECK_EQ(ffbErase(4).effect, 5);
        CHECK_EQ(field(ffbGain(0xFFFF), "gain"), 255);
        CHECK_EQ(field(ffbGain(0x8000), "gain"), 127);
    }

    SECTION("HID evdev — a backend with its own force feedback: no PID block, ops to the sink");
    {
        UinputFake* dev = nullptr;
        std::vector<HidFfb> ffb;
        HidPassthrough hp(nullptr, [&] { return std::make_unique<UinputFake>(&dev); });
        hp.setFfbSink([&](const HidFfb& o) { ffb.push_back(o); });
        HidDeviceInfo info;
        Report r;
        r.reportId = 1;
        r.items = {axis({0x10030}, 16, 0, 65535)};
        Collection c;
        c.usagePage = 0x01;
        c.usage = 0x04;
        c.inputReports = {r};
        info.collections = {c};
        info.forceFeedback = true;
        CHECK(hp.attach(5, info).empty());
        CHECK(dev != nullptr);
        if (!dev) return;
        CHECK(dev->descriptor == encode({c})); // no PID block
        LinuxFfEffect e;
        e.type = ev::FfConstant;
        e.level = 0x7FFF;
        dev->upload(e);
        CHECK_EQ(ffb.size(), static_cast<size_t>(3));
        if (!ffb.empty()) CHECK_EQ(ffb[0].slot, 5);
    }
}
