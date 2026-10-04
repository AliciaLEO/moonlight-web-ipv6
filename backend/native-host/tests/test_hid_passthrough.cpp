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
#include "input/VirtualHid.h"
#include "mw/native/HidPassthrough.h"

#include <memory>
#include <vector>

using namespace mw::native;
using namespace mw::native::input::hid;

namespace {

/// What a backend was asked to do, kept for the test to read.
struct FakeLog
{
    std::vector<input::VirtualHidIdentity> created;
    std::vector<std::vector<uint8_t>> inputs;
    int destroyed = 0;
    input::IVirtualHid* last = nullptr;
};

class FakeHid : public input::IVirtualHid
{
public:
    explicit FakeHid(FakeLog& log)
        : m_log(log)
    {
        m_log.last = this;
    }
    ~FakeHid() override { destroy(); }
    bool create(const input::VirtualHidIdentity& id, std::string&) override
    {
        m_log.created.push_back(id);
        m_alive = true;
        return true;
    }
    bool input(const uint8_t* r, size_t n) override
    {
        m_log.inputs.emplace_back(r, r + n);
        return true;
    }
    void destroy() override
    {
        if (m_alive) ++m_log.destroyed;
        m_alive = false;
    }
    void ask(Request kind, uint8_t id, std::vector<uint8_t> data)
    {
        if (m_onRequest) m_onRequest(kind, id, data);
    }

private:
    FakeLog& m_log;
    bool m_alive = false;
};

ReportItem item(std::vector<uint32_t> usages, uint16_t size, int32_t lmin, int32_t lmax)
{
    ReportItem it;
    it.reportCount = static_cast<uint16_t>(usages.size());
    it.usages = std::move(usages);
    it.reportSize = size;
    it.logicalMinimum = lmin;
    it.logicalMaximum = lmax;
    return it;
}

/// A small wheel, report id 1, 4 bytes: a 4-bit hat with a null state, four
/// buttons, a 16-bit wheel and an 8-bit pedal.
HidDeviceInfo wheel()
{
    Report r;
    r.reportId = 1;
    ReportItem hat = item({0x10039}, 4, 0, 7);
    hat.hasNull = true;
    r.items.push_back(hat);
    ReportItem buttons;
    buttons.isRange = true;
    buttons.usageMinimum = 0x90001;
    buttons.usageMaximum = 0x90004;
    buttons.reportSize = 1;
    buttons.reportCount = 4;
    buttons.logicalMaximum = 1;
    r.items.push_back(buttons);
    r.items.push_back(item({0x10030}, 16, 0, 65535));
    r.items.push_back(item({0x200C4}, 8, 0, 255));
    Collection c;
    c.usagePage = 0x01;
    c.usage = 0x04;
    c.inputReports.push_back(r);
    HidDeviceInfo d;
    d.name = "Test wheel";
    d.vendorId = 0x046d;
    d.productId = 0xc26e;
    d.collections = {c};
    return d;
}

} // namespace

void run_hid_passthrough_tests()
{
    SECTION("HID passthrough — a game device is created, its reports go through with their id");
    {
        FakeLog log;
        int64_t now = 1000;
        HidPassthrough hp(
            nullptr, [&] { return std::make_unique<FakeHid>(log); }, [&] { return now; });
        CHECK(hp.attach(0, wheel()).empty());
        CHECK_EQ(log.created.size(), static_cast<size_t>(1));
        CHECK_EQ(log.created[0].vendorId, 0x046d);
        CHECK(validate(log.created[0].descriptor).empty());
        const uint8_t r[4] = {0x23, 0x34, 0x12, 0xff};
        hp.input(0, 1, 0, r, 4);
        CHECK_EQ(log.inputs.size(), static_cast<size_t>(1));
        CHECK(log.inputs[0] == (std::vector<uint8_t>{1, 0x23, 0x34, 0x12, 0xff}));
    }

    SECTION(
        "HID passthrough — wrong sizes, unknown ids, late frames and unknown slots are dropped");
    {
        FakeLog log;
        HidPassthrough hp(
            nullptr, [&] { return std::make_unique<FakeHid>(log); }, [] { return int64_t{0}; });
        CHECK(hp.attach(0, wheel()).empty());
        const uint8_t r[5] = {0, 0, 0, 0, 0};
        hp.input(0, 1, 0, r, 3); // short
        hp.input(0, 1, 1, r, 5); // long
        hp.input(0, 2, 2, r, 4); // no report 2
        hp.input(3, 1, 3, r, 4); // no slot 3
        CHECK(log.inputs.empty());
        hp.input(0, 1, 10, r, 4);
        hp.input(0, 1, 9, r, 4);  // older than the newest applied
        hp.input(0, 1, 10, r, 4); // the same one again
        CHECK_EQ(log.inputs.size(), static_cast<size_t>(1));
        hp.input(0, 1, 11, r, 4);
        CHECK_EQ(log.inputs.size(), static_cast<size_t>(2));
        // The sequence wraps at 16 bits: 0 comes after 65535.
        CHECK(hp.attach(1, wheel()).empty());
        hp.input(1, 1, 65534, r, 4);
        hp.input(1, 1, 65535, r, 4);
        hp.input(1, 1, 0, r, 4);
        CHECK_EQ(log.inputs.size(), static_cast<size_t>(5));
    }

    SECTION(
        "HID passthrough — after a silence, everything to rest once: buttons up, hat null, wheel "
        "centred, pedal released");
    {
        FakeLog log;
        int64_t now = 0;
        HidPassthrough hp(
            nullptr, [&] { return std::make_unique<FakeHid>(log); }, [&] { return now; });
        CHECK(hp.attach(0, wheel()).empty());
        // hat 3, buttons 1 and 3 down (0b0101 in the high nibble), wheel 0x1234, pedal 200
        const uint8_t r[4] = {0x53, 0x34, 0x12, 200};
        hp.input(0, 1, 1, r, 4);
        hp.checkSilence(HidPassthrough::kSilenceMs - 1);
        CHECK_EQ(log.inputs.size(), static_cast<size_t>(1));
        hp.checkSilence(HidPassthrough::kSilenceMs);
        CHECK_EQ(log.inputs.size(), static_cast<size_t>(2));
        // hat null (8), no button, wheel at the middle of 0..65535, the
        // accelerator (first seen half-pressed, so its usage decides) at 0
        CHECK(log.inputs[1] == (std::vector<uint8_t>{1, 0x08, 0x00, 0x80, 0}));
        hp.checkSilence(HidPassthrough::kSilenceMs * 3);
        CHECK_EQ(log.inputs.size(), static_cast<size_t>(2));
        // A device already at rest needs no rest report.
        const uint8_t idle[4] = {0x08, 0x00, 0x80, 0};
        now = 10000;
        hp.input(0, 1, 2, idle, 4);
        hp.checkSilence(10000 + HidPassthrough::kSilenceMs);
        CHECK_EQ(log.inputs.size(), static_cast<size_t>(3));
    }

    SECTION("HID passthrough — refused devices, slots replaced and removed, requests reported");
    {
        FakeLog log;
        std::vector<HidRequest> asked;
        HidPassthrough hp([&](const HidRequest& r) { asked.push_back(r); },
                          [&] { return std::make_unique<FakeHid>(log); },
                          [] { return int64_t{0}; });
        HidDeviceInfo keyboard = wheel();
        keyboard.collections[0].usage = 0x06;
        CHECK(!hp.attach(0, keyboard).empty());
        CHECK(!hp.attach(HidPassthrough::kMaxSlots, wheel()).empty());
        CHECK(log.created.empty());

        CHECK(hp.attach(2, wheel()).empty());
        static_cast<FakeHid*>(log.last)->ask(input::IVirtualHid::Request::Output, 5, {5, 1, 2});
        CHECK_EQ(asked.size(), static_cast<size_t>(1));
        CHECK(asked[0].slot == 2 && asked[0].kind == HidRequest::Kind::Output &&
              asked[0].reportId == 5);
        CHECK(asked[0].data == (std::vector<uint8_t>{5, 1, 2}));

        CHECK(hp.attach(2, wheel()).empty()); // replaces the first
        CHECK_EQ(log.destroyed, 1);
        hp.detach(2);
        CHECK_EQ(log.destroyed, 2);
        hp.detach(2); // nothing left: no harm
        CHECK(hp.attach(0, wheel()).empty());
        CHECK(hp.attach(1, wheel()).empty());
        hp.detachAll();
        CHECK_EQ(log.destroyed, 4);
    }

    SECTION("HID passthrough — a backend that cannot be made refuses the device");
    {
        HidPassthrough hp(
            nullptr, [] { return std::unique_ptr<input::IVirtualHid>(); },
            [] { return int64_t{0}; });
        CHECK(!hp.attach(0, wheel()).empty());
    }
}
