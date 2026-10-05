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

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

/// The HID passthrough (docs/design/hid-passthrough-study.md, plan P2): a game
/// device the browser reads through WebHID is recreated on this host, same
/// vendor and product, same reports, so a game sees the real wheel or radio
/// rather than an Xbox pad.
///
/// The page sends the device's `collections` (WebHID's description of its
/// report descriptor, which the browser never shows) and then its input
/// reports. The relay reads the collections into the structures below;
/// HidPassthrough rebuilds the descriptor, checks it and creates the device.

namespace mw::native::input::hid {

/// HID unit systems, as the low nibble of the Unit item.
enum class UnitSystem : uint8_t
{
    None = 0x0,
    SiLinear = 0x1,
    SiRotation = 0x2,
    EnglishLinear = 0x3,
    EnglishRotation = 0x4,
    Vendor = 0xF,
};

/// One main item of a report, as WebHID's HIDReportItem shows it. Usages are
/// extended: usage page in the high 16 bits, usage id in the low 16.
struct ReportItem
{
    bool isAbsolute = true;
    bool isArray = false;
    bool isBufferedBytes = false;
    bool isConstant = false;
    bool isLinear = true;
    bool isRange = false;
    bool isVolatile = false;
    bool hasNull = false;
    bool hasPreferredState = true;
    bool wrap = false;
    std::vector<uint32_t> usages;
    uint32_t usageMinimum = 0;
    uint32_t usageMaximum = 0;
    uint16_t reportSize = 0;
    uint16_t reportCount = 0;
    int8_t unitExponent = 0;
    UnitSystem unitSystem = UnitSystem::None;
    /// Length, mass, time, temperature, current, luminous intensity.
    int8_t unitFactors[6] = {0, 0, 0, 0, 0, 0};
    int32_t logicalMinimum = 0;
    int32_t logicalMaximum = 0;
    int32_t physicalMinimum = 0;
    int32_t physicalMaximum = 0;
};

struct Report
{
    uint8_t reportId = 0;
    std::vector<ReportItem> items;
};

/// HID collection types (Collection item data).
enum CollectionType : uint8_t
{
    Physical = 0x00,
    Application = 0x01,
    Logical = 0x02,
};

/// WebHID's HIDCollectionInfo: the items directly inside the collection, by
/// report and kind, then its child collections.
struct Collection
{
    uint16_t usagePage = 0;
    uint16_t usage = 0;
    uint8_t type = Application;
    std::vector<Collection> children;
    std::vector<Report> inputReports;
    std::vector<Report> outputReports;
    std::vector<Report> featureReports;
};

} // namespace mw::native::input::hid

namespace mw::native {

namespace input {
class IVirtualHid;
}

/// A device as the page describes it.
struct HidDeviceInfo
{
    std::string name;
    uint16_t vendorId = 0;
    uint16_t productId = 0;
    uint16_t version = 0;
    std::vector<input::hid::Collection> collections;
    /// The page can play force feedback on the real device (it found a way to
    /// drive its motor, e.g. Logitech's HID++ 0x8123): the recreated device
    /// gets a PID block, and what games ask of it comes back as HidFfb.
    bool forceFeedback = false;
};

/// What the host's OS asked of a recreated device, for the page to pass on.
struct HidRequest
{
    enum class Kind
    {
        Output,
        GetFeature,
        SetFeature
    };
    int slot = 0;
    Kind kind = Kind::Output;
    uint8_t reportId = 0;
    std::vector<uint8_t> data;
};

/// One force-feedback operation a game asked of a recreated device, decoded
/// from the PID reports pid.dll wrote (input/HidPid.h), for the page to play on
/// the real wheel. Levels use DirectInput's scale, -10000..10000; times are in
/// milliseconds; angles in hundredths of a degree.
///  - "effect": an effect's header; `kind` is constant, ramp, square, sine,
///    triangle, sawtoothUp, sawtoothDown, spring, damper, inertia or friction;
///    fields duration (-1 = infinite), delay, gain (0..255), direction,
///    directionEnable (0: apply along the enabled axes as is), axes (bit 0 X,
///    bit 1 Y);
///  - "envelope": attackLevel, attackTime, fadeLevel, fadeTime;
///  - "condition": axis, offset, positiveCoefficient, negativeCoefficient,
///    positiveSaturation, negativeSaturation, deadBand;
///  - "periodic": magnitude, offset, phase, period;
///  - "constant": magnitude;  "ramp": start, end;
///  - "start" (loops), "solo" (loops: stop the others first), "stop", "free";
///  - "gain": gain (0..255, the device's master gain);
///  - "control": `kind` enable, disable, stopAll, reset, pause or continue.
/// `effect` is the effect block (1..40), 0 for gain and control.
struct HidFfb
{
    int slot = 0;
    std::string op;
    int effect = 0;
    std::string kind;
    std::vector<std::pair<std::string, int32_t>> fields;
};

/// The devices one stream session recreates. Thread-safe: attach and detach
/// come from the relay's input handler, input() from the `hid` channel's
/// callback, requests leave from the backends' threads.
///
/// Policy, in one place:
///  - at most kMaxSlots devices, each checked by hid::validate() first;
///  - an input report of the wrong size for its id is dropped, as is one older
///    than the newest already applied (the channel is unordered);
///  - after kSilenceMs without a report, each input report goes back to rest
///    once: buttons up, hats centred, sticks and wheels centred, pedals and
///    throttles at their rest end, read from the device's first report when
///    its usages do not say (hid::restReport, Bruno's rule of 04/10).
class HidPassthrough
{
public:
    static constexpr int kMaxSlots = 8;
    static constexpr int kSilenceMs = 3000;

    using RequestSink = std::function<void(const HidRequest&)>;
    using FfbSink = std::function<void(const HidFfb&)>;
    using Factory = std::function<std::unique_ptr<input::IVirtualHid>()>;
    using Clock = std::function<int64_t()>; // milliseconds, monotonic

    /// `factory` and `clock` are for tests; by default the OS's backend and
    /// the steady clock.
    explicit HidPassthrough(RequestSink onRequest, Factory factory = nullptr,
                            Clock clock = nullptr);
    ~HidPassthrough();
    HidPassthrough(const HidPassthrough&) = delete;
    HidPassthrough& operator=(const HidPassthrough&) = delete;

    /// Where force-feedback operations go; set before the first attach. Called
    /// on a backend thread, outside this object's lock.
    void setFfbSink(FfbSink sink) { m_onFfb = std::move(sink); }

    /// Empty when this host can recreate devices; why not otherwise.
    static std::string unavailableReason();

    /// Empty on success; otherwise why the device was refused (for the log and
    /// the page). A slot already in use is replaced.
    std::string attach(int slot, const HidDeviceInfo& device);
    /// One frame of the `hid` channel's payload: report id, sequence number and
    /// the report as WebHID gives it (without its id byte).
    void input(int slot, uint8_t reportId, uint16_t seq, const uint8_t* bytes, size_t size);
    /// The real device's answer to a request the host's OS made (a Logitech
    /// HID++ reply the page relayed, plan P4 under Linux), injected once as it
    /// came: no sequence number, never repeated, never put to rest. Ignored when
    /// the device has no input report of that id and size.
    void reply(int slot, uint8_t reportId, const uint8_t* bytes, size_t size);
    void detach(int slot);
    void detachAll();

    /// For the watchdog's thread and the tests: puts silent devices to rest.
    void checkSilence(int64_t nowMs);

private:
    struct Slot;
    void watchdog();

    RequestSink m_onRequest;
    FfbSink m_onFfb;
    Factory m_factory;
    Clock m_clock;
    std::mutex m_mutex;
    std::map<int, std::unique_ptr<Slot>> m_slots;
    std::atomic<bool> m_stop{false};
    std::thread m_watchdog;
};

} // namespace mw::native
