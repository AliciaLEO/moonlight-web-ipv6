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
#include "input/HidPid.h"
#include "input/VirtualHid.h"
#include "mw/native/HidPassthrough.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace mw::native;
using namespace mw::native::input::hid;

namespace {

constexpr uint32_t P(uint16_t id)
{
    return 0x000F0000u | id;
}

/// A wheel with report id 1 and a vendor collection with ids 17 and 18, as the
/// G923 has: the PID block must start after 18.
std::vector<Collection> wheel()
{
    ReportItem x;
    x.usages = {0x10030};
    x.reportSize = 16;
    x.reportCount = 1;
    x.logicalMaximum = 65535;
    Report r;
    r.reportId = 1;
    r.items = {x};
    Collection joystick;
    joystick.usagePage = 0x01;
    joystick.usage = 0x04;
    joystick.inputReports = {r};

    ReportItem bytes;
    bytes.isArray = true;
    bytes.usages = {0xFF430602};
    bytes.reportSize = 8;
    bytes.reportCount = 19;
    bytes.logicalMaximum = 255;
    Report v17;
    v17.reportId = 17;
    v17.items = {bytes};
    Report v18 = v17;
    v18.reportId = 18;
    v18.items[0].reportCount = 63;
    Collection vendor;
    vendor.usagePage = 0xFF43;
    vendor.usage = 0x0602;
    vendor.inputReports = {v17, v18};
    vendor.outputReports = {v17, v18};
    return {joystick, vendor};
}

/// A PID wheel (Moza, Simucube…): the wheel above with a PID block of its own,
/// a Set Effect output report 2 in a logical collection, a Block Load feature
/// report 3, and a PID "Device Paused" bit sharing input report 1 with the axis.
std::vector<Collection> pidWheel()
{
    std::vector<Collection> c = wheel();
    ReportItem paused;
    paused.usages = {P(0x9F)};
    paused.reportSize = 1;
    paused.reportCount = 1;
    paused.logicalMaximum = 1;
    ReportItem pad;
    pad.isConstant = true;
    pad.reportSize = 7;
    pad.reportCount = 1;
    c[0].inputReports[0].items.push_back(paused);
    c[0].inputReports[0].items.push_back(pad);

    ReportItem index;
    index.usages = {P(0x22)};
    index.reportSize = 8;
    index.reportCount = 1;
    index.logicalMinimum = 1;
    index.logicalMaximum = 40;
    ReportItem x;
    x.usages = {0x10030};
    x.reportSize = 1;
    x.reportCount = 1;
    x.logicalMaximum = 1;
    Report setEffect;
    setEffect.reportId = 2;
    setEffect.items = {index};
    Report axes;
    axes.reportId = 2;
    axes.items = {x};
    Collection axesEnable;
    axesEnable.usagePage = 0x0F;
    axesEnable.usage = 0x55;
    axesEnable.type = Logical;
    axesEnable.outputReports = {axes};
    Collection effect;
    effect.usagePage = 0x0F;
    effect.usage = 0x21;
    effect.type = Logical;
    effect.outputReports = {setEffect};
    effect.children = {axesEnable};
    Report load;
    load.reportId = 3;
    load.items = {index};
    Collection blockLoad;
    blockLoad.usagePage = 0x0F;
    blockLoad.usage = 0x89;
    blockLoad.type = Logical;
    blockLoad.featureReports = {load};
    c[0].children = {effect, blockLoad};
    return c;
}

bool anyPid(const Parsed& p)
{
    for (const Field& f : p.fields) {
        if ((f.application >> 16) == 0x0F) return true;
        for (uint32_t u : f.usages)
            if ((u >> 16) == 0x0F) return true;
        if (f.range && (f.usageMinimum >> 16) == 0x0F) return true;
    }
    return false;
}

/// Writes reports the way pid.dll would, by usage, from the parsed block.
struct Writer
{
    const Parsed& p;

    const Field* variable(Kind kind, uint8_t id, uint32_t usage, uint32_t& offset) const
    {
        for (const Field& f : p.fields) {
            if (f.kind != kind || f.reportId != id || !f.isVariable() || f.isConstant()) continue;
            for (uint32_t i = 0; i < f.count && i < f.usages.size(); ++i)
                if (f.usages[i] == usage) {
                    offset = f.bitOffset + i * f.size;
                    return &f;
                }
        }
        return nullptr;
    }
    static void bits(std::vector<uint8_t>& r, uint32_t offset, uint32_t size, uint32_t v)
    {
        for (uint32_t b = 0; b < size; ++b) {
            const uint32_t bit = offset + b;
            if ((v >> b) & 1u)
                r[1 + bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
            else
                r[1 + bit / 8] &= static_cast<uint8_t>(~(1u << (bit % 8)));
        }
    }
    std::vector<uint8_t> report(Kind kind, uint8_t id) const
    {
        std::vector<uint8_t> r(reportBytes(p, kind, id), 0);
        if (!r.empty()) r[0] = id;
        return r;
    }
    void set(std::vector<uint8_t>& r, Kind kind, uint32_t usage, int32_t v) const
    {
        uint32_t offset = 0;
        const Field* f = variable(kind, r[0], usage, offset);
        CHECK(f != nullptr);
        if (f) bits(r, offset, f->size, static_cast<uint32_t>(v));
    }
    void select(std::vector<uint8_t>& r, Kind kind, uint32_t usage) const
    {
        for (const Field& f : p.fields) {
            if (f.kind != kind || f.reportId != r[0] || f.isVariable()) continue;
            auto it = std::find(f.usages.begin(), f.usages.end(), usage);
            if (it == f.usages.end()) continue;
            bits(r, f.bitOffset, f.size,
                 static_cast<uint32_t>(f.logicalMinimum + (it - f.usages.begin())));
            return;
        }
        CHECK(false);
    }
    int32_t get(const std::vector<uint8_t>& r, Kind kind, uint32_t usage) const
    {
        uint32_t offset = 0;
        const Field* f = variable(kind, r[0], usage, offset);
        if (!f) return -999;
        uint32_t v = 0;
        for (uint32_t b = 0; b < f->size; ++b)
            if ((r[1 + (offset + b) / 8] >> ((offset + b) % 8)) & 1u) v |= 1u << b;
        return static_cast<int32_t>(v);
    }
};

const HidFfb* find(const std::vector<HidFfb>& ops, const std::string& op)
{
    for (const HidFfb& o : ops)
        if (o.op == op) return &o;
    return nullptr;
}

int32_t field(const HidFfb& o, const std::string& name)
{
    for (const auto& [k, v] : o.fields)
        if (k == name) return v;
    return -999;
}

/// A backend that answers GET_FEATURE the way the Windows one does.
class FeatureFake : public input::IVirtualHid
{
public:
    explicit FeatureFake(FeatureFake** self) { *self = this; }
    bool create(const input::VirtualHidIdentity& id, std::string&) override
    {
        descriptor = id.descriptor;
        return true;
    }
    bool input(const uint8_t*, size_t) override { return true; }
    void destroy() override {}
    std::vector<uint8_t> getFeature(uint8_t id)
    {
        std::vector<uint8_t> r = m_onGetFeature ? m_onGetFeature(id) : std::vector<uint8_t>{};
        if (m_onRequest) m_onRequest(Request::GetFeature, id, {});
        return r;
    }
    void write(Request kind, const std::vector<uint8_t>& data)
    {
        if (m_onRequest) m_onRequest(kind, data[0], data);
    }
    std::vector<uint8_t> descriptor;
};

} // namespace

void run_hid_pid_tests()
{
    SECTION("HID PID — the block goes after the device's own report ids, never on id 0");
    {
        CHECK_EQ(static_cast<int>(pidFirstId(wheel())), 19);
        std::vector<Collection> unnumbered = wheel();
        unnumbered.resize(1);
        unnumbered[0].inputReports[0].reportId = 0;
        CHECK_EQ(static_cast<int>(pidFirstId(unnumbered)), 0);
    }

    SECTION("HID PID — the wheel with its block is a valid game device, block inside the joystick");
    {
        const std::vector<uint8_t> d = encodeWithPid(wheel(), 19);
        CHECK(validate(d).empty());
        CHECK(d.size() < kMaxDescriptorBytes);
        const Parsed p = parse(d);
        CHECK(p.ok);
        // Every PID report sits in the joystick's application collection.
        bool allInJoystick = true;
        int pidFields = 0;
        for (const Field& f : p.fields) {
            if (f.reportId < 19) continue;
            ++pidFields;
            allInJoystick = allInJoystick && f.application == 0x00010004;
        }
        CHECK(pidFields > 20);
        CHECK(allInJoystick);
        // The device's own reports are laid out as without the block.
        const Parsed plain = parse(encode(wheel()));
        std::vector<Field> own;
        for (const Field& f : p.fields)
            if (f.reportId < 19) own.push_back(f);
        CHECK(normalise(own) == normalise(plain.fields));
        for (int id = 19; id < 19 + kPidReports; ++id) {
            const bool any = reportBytes(p, Kind::Output, static_cast<uint8_t>(id)) ||
                             reportBytes(p, Kind::Feature, static_cast<uint8_t>(id)) ||
                             reportBytes(p, Kind::Input, static_cast<uint8_t>(id));
            CHECK(any);
        }
    }

    SECTION("HID PID — a new effect gets a block from Create New Effect + Block Load");
    {
        const std::vector<uint8_t> d = encodeWithPid(wheel(), 19);
        const Parsed p = parse(d);
        PidEngine e(p, 19);
        Writer w{p};
        const uint8_t create = 19 + 10, load = 19 + 11, pool = 19 + 12;
        CHECK(e.owns(create));
        CHECK(!e.owns(18));

        const std::vector<uint8_t> poolReport = e.getFeature(pool);
        CHECK(!poolReport.empty());
        CHECK_EQ(poolReport[0], pool);
        CHECK_EQ(w.get(poolReport, Kind::Feature, P(0x83)), kPidMaxEffects);
        CHECK_EQ(w.get(poolReport, Kind::Feature, P(0xA9)), 1);

        std::vector<HidFfb> ops;
        for (int expect = 1; expect <= 2; ++expect) {
            std::vector<uint8_t> c = w.report(Kind::Feature, create);
            w.select(c, Kind::Feature, P(0x26)); // constant force
            e.write(c, ops);
            const std::vector<uint8_t> l = e.getFeature(load);
            CHECK_EQ(w.get(l, Kind::Feature, P(0x22)), expect);
        }
        CHECK(ops.empty()); // creating is the host's business only

        // Freed, the first block is handed out again.
        std::vector<uint8_t> f = w.report(Kind::Output, 19 + 7);
        w.set(f, Kind::Output, P(0x22), 1);
        e.write(f, ops);
        CHECK(find(ops, "free") != nullptr);
        std::vector<uint8_t> c = w.report(Kind::Feature, create);
        w.select(c, Kind::Feature, P(0x40)); // spring
        e.write(c, ops);
        CHECK_EQ(w.get(e.getFeature(load), Kind::Feature, P(0x22)), 1);
    }

    SECTION("HID PID — a game's constant force comes out as effect, constant and start");
    {
        const std::vector<uint8_t> d = encodeWithPid(wheel(), 19);
        const Parsed p = parse(d);
        PidEngine e(p, 19);
        Writer w{p};
        std::vector<HidFfb> ops;

        std::vector<uint8_t> se = w.report(Kind::Output, 19);
        w.set(se, Kind::Output, P(0x22), 3);
        w.select(se, Kind::Output, P(0x26));
        w.set(se, Kind::Output, P(0x50), 0xFFFF); // infinite
        w.set(se, Kind::Output, P(0xA7), 20);
        w.set(se, Kind::Output, P(0x52), 255);
        w.set(se, Kind::Output, 0x000A0001, 9000); // 90°
        w.set(se, Kind::Output, 0x00010030, 1);    // X enabled
        w.set(se, Kind::Output, P(0x56), 1);
        e.write(se, ops);

        std::vector<uint8_t> cf = w.report(Kind::Output, 19 + 4);
        w.set(cf, Kind::Output, P(0x22), 3);
        w.set(cf, Kind::Output, P(0x70), static_cast<uint16_t>(-2500));
        e.write(cf, ops);

        std::vector<uint8_t> op = w.report(Kind::Output, 19 + 6);
        w.set(op, Kind::Output, P(0x22), 3);
        w.select(op, Kind::Output, P(0x79));
        w.set(op, Kind::Output, P(0x7C), 1);
        e.write(op, ops);

        const HidFfb* effect = find(ops, "effect");
        CHECK(effect != nullptr);
        if (effect) {
            CHECK_EQ(effect->effect, 3);
            CHECK_EQ(effect->kind, std::string("constant"));
            CHECK_EQ(field(*effect, "duration"), -1);
            CHECK_EQ(field(*effect, "delay"), 20);
            CHECK_EQ(field(*effect, "gain"), 255);
            CHECK_EQ(field(*effect, "direction"), 9000);
            CHECK_EQ(field(*effect, "directionEnable"), 1);
            CHECK_EQ(field(*effect, "axes"), 1);
        }
        const HidFfb* constant = find(ops, "constant");
        CHECK(constant != nullptr);
        if (constant) CHECK_EQ(field(*constant, "magnitude"), -2500);
        const HidFfb* start = find(ops, "start");
        CHECK(start != nullptr);
        if (start) {
            CHECK_EQ(start->effect, 3);
            CHECK_EQ(field(*start, "loops"), 1);
        }
    }

    SECTION("HID PID — conditions, gain and device control");
    {
        const std::vector<uint8_t> d = encodeWithPid(wheel(), 19);
        const Parsed p = parse(d);
        PidEngine e(p, 19);
        Writer w{p};
        std::vector<HidFfb> ops;

        std::vector<uint8_t> sc = w.report(Kind::Output, 19 + 2);
        w.set(sc, Kind::Output, P(0x22), 2);
        w.set(sc, Kind::Output, P(0x60), static_cast<uint16_t>(-100));
        w.set(sc, Kind::Output, P(0x61), 8000);
        w.set(sc, Kind::Output, P(0x62), 7000);
        w.set(sc, Kind::Output, P(0x63), 10000);
        w.set(sc, Kind::Output, P(0x64), 9000);
        w.set(sc, Kind::Output, P(0x65), 50);
        e.write(sc, ops);
        const HidFfb* c = find(ops, "condition");
        CHECK(c != nullptr);
        if (c) {
            CHECK_EQ(c->effect, 2);
            CHECK_EQ(field(*c, "offset"), -100);
            CHECK_EQ(field(*c, "positiveCoefficient"), 8000);
            CHECK_EQ(field(*c, "negativeCoefficient"), 7000);
            CHECK_EQ(field(*c, "positiveSaturation"), 10000);
            CHECK_EQ(field(*c, "negativeSaturation"), 9000);
            CHECK_EQ(field(*c, "deadBand"), 50);
        }

        std::vector<uint8_t> g = w.report(Kind::Output, 19 + 9);
        w.set(g, Kind::Output, P(0x7E), 128);
        e.write(g, ops);
        const HidFfb* gain = find(ops, "gain");
        CHECK(gain != nullptr);
        if (gain) CHECK_EQ(field(*gain, "gain"), 128);

        std::vector<uint8_t> dc = w.report(Kind::Output, 19 + 8);
        w.select(dc, Kind::Output, P(0x99));
        e.write(dc, ops);
        const HidFfb* control = find(ops, "control");
        CHECK(control != nullptr);
        if (control) CHECK_EQ(control->kind, std::string("stopAll"));

        // A block index out of range is ignored rather than passed on.
        const size_t before = ops.size();
        std::vector<uint8_t> bad = w.report(Kind::Output, 19 + 4);
        w.set(bad, Kind::Output, P(0x22), 0);
        e.write(bad, ops);
        CHECK_EQ(ops.size(), before);
    }

    SECTION("HID PID — through HidPassthrough: features answered, operations to the FFB sink only");
    {
        FeatureFake* dev = nullptr;
        std::vector<HidRequest> requests;
        std::vector<HidFfb> ffb;
        HidPassthrough hp([&](const HidRequest& r) { requests.push_back(r); },
                          [&] { return std::make_unique<FeatureFake>(&dev); });
        hp.setFfbSink([&](const HidFfb& o) { ffb.push_back(o); });
        HidDeviceInfo info;
        info.name = "Wheel";
        info.vendorId = 0x046d;
        info.productId = 0xc26e;
        info.collections = wheel();
        info.forceFeedback = true;
        CHECK(hp.attach(2, info).empty());
        CHECK(dev != nullptr);
        if (!dev) return;
        const Parsed p = parse(dev->descriptor);
        Writer w{p};

        std::vector<uint8_t> c = w.report(Kind::Feature, 19 + 10);
        w.select(c, Kind::Feature, P(0x31)); // sine
        dev->write(input::IVirtualHid::Request::SetFeature, c);
        const std::vector<uint8_t> load = dev->getFeature(19 + 11);
        CHECK_EQ(w.get(load, Kind::Feature, P(0x22)), 1);

        std::vector<uint8_t> sp = w.report(Kind::Output, 19 + 3);
        w.set(sp, Kind::Output, P(0x22), 1);
        w.set(sp, Kind::Output, P(0x70), 5000);
        w.set(sp, Kind::Output, P(0x72), 100);
        dev->write(input::IVirtualHid::Request::Output, sp);
        CHECK_EQ(ffb.size(), static_cast<size_t>(1));
        if (!ffb.empty()) {
            CHECK_EQ(ffb[0].slot, 2);
            CHECK_EQ(ffb[0].op, std::string("periodic"));
            CHECK_EQ(field(ffb[0], "magnitude"), 5000);
            CHECK_EQ(field(ffb[0], "period"), 100);
        }
        CHECK(requests.empty()); // the PID block never reaches the page raw

        // The device's own vendor reports still go to the page as before.
        std::vector<uint8_t> hidpp(20, 0);
        hidpp[0] = 17;
        dev->write(input::IVirtualHid::Request::Output, hidpp);
        CHECK_EQ(requests.size(), static_cast<size_t>(1));
    }

    SECTION("HID PID — a wheel's own PID block is taken out, its input layout kept");
    {
        std::vector<Collection> c = pidWheel();
        CHECK_EQ(static_cast<int>(pidFirstId(c)), 19);
        stripPid(c);
        const Parsed p = parse(encode(c));
        CHECK(p.ok);
        CHECK(!anyPid(p));
        // Report 1 still carries the axis and 8 bits after it, now padding.
        CHECK_EQ(reportBytes(p, Kind::Input, 1), static_cast<size_t>(1 + 3));
        CHECK_EQ(reportBytes(p, Kind::Output, 2), static_cast<size_t>(0));
        CHECK_EQ(reportBytes(p, Kind::Feature, 3), static_cast<size_t>(0));
        CHECK(c[0].children.empty());
        // The vendor collection is untouched.
        CHECK_EQ(c.size(), static_cast<size_t>(2));
        CHECK_EQ(reportBytes(p, Kind::Output, 18), static_cast<size_t>(64));
        // A wheel without PID comes out as it went in.
        std::vector<Collection> plain = wheel();
        stripPid(plain);
        CHECK(encode(plain) == encode(wheel()));
    }

    SECTION("HID PID — a PID wheel through HidPassthrough: the host's block, or none");
    {
        for (bool ffb : {false, true}) {
            FeatureFake* dev = nullptr;
            std::vector<HidRequest> requests;
            HidPassthrough hp([&](const HidRequest& r) { requests.push_back(r); },
                              [&] { return std::make_unique<FeatureFake>(&dev); });
            HidDeviceInfo info;
            info.collections = pidWheel();
            info.forceFeedback = ffb;
            CHECK(hp.attach(0, info).empty());
            CHECK(dev != nullptr);
            if (!dev) continue;
            const Parsed p = parse(dev->descriptor);
            CHECK(p.ok);
            CHECK_EQ(anyPid(p), ffb);
            // Never the wheel's own Set Effect (2) or Block Load (3).
            CHECK_EQ(reportBytes(p, Kind::Output, 2), static_cast<size_t>(0));
            CHECK_EQ(reportBytes(p, Kind::Feature, 3), static_cast<size_t>(0));
            if (ffb) {
                // The host's Block Load answers at once, from ids after the wheel's.
                const std::vector<uint8_t> load = dev->getFeature(19 + 11);
                CHECK(!load.empty());
                CHECK(requests.empty());
            }
        }
    }

    SECTION("HID PID — without force feedback asked, no block");
    {
        FeatureFake* dev = nullptr;
        HidPassthrough hp(nullptr, [&] { return std::make_unique<FeatureFake>(&dev); });
        HidDeviceInfo info;
        info.collections = wheel();
        CHECK(hp.attach(0, info).empty());
        CHECK(dev != nullptr);
        if (dev) CHECK(dev->descriptor == encode(wheel()));
    }
}
