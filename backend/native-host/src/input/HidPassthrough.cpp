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

#include "mw/native/HidPassthrough.h"

#include "core/Log.h"
#include "input/HidDescriptor.h"
#include "input/VirtualHid.h"

#include <chrono>
#include <cstdio>

namespace mw::native {

namespace {

int64_t steadyMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

struct HidPassthrough::Slot
{
    std::unique_ptr<input::IVirtualHid> device;
    input::hid::Parsed parsed;
    bool numbered = false;
    std::map<uint8_t, size_t> inputBytes; // per report id, without the id byte
    std::map<uint8_t, std::vector<uint8_t>> last;
    std::map<uint8_t, uint16_t> lastSeq;
    int64_t lastAtMs = 0;
    bool resting = true;
};

HidPassthrough::HidPassthrough(RequestSink onRequest, Factory factory, Clock clock)
    : m_onRequest(std::move(onRequest))
    , m_factory(std::move(factory))
    , m_clock(clock ? std::move(clock) : Clock(steadyMs))
{}

HidPassthrough::~HidPassthrough()
{
    m_stop = true;
    if (m_watchdog.joinable()) m_watchdog.join();
    detachAll();
}

std::string HidPassthrough::unavailableReason()
{
    return input::virtualHidUnavailableReason();
}

std::string HidPassthrough::attach(int slot, const HidDeviceInfo& device)
{
    if (slot < 0 || slot >= kMaxSlots) return "slot out of range";

    std::vector<input::hid::Collection> collections = device.collections;
    input::hid::repairBounds(collections);
    const std::vector<uint8_t> descriptor = input::hid::encode(collections);
    if (std::string why = input::hid::validate(descriptor); !why.empty()) return why;

    std::unique_ptr<input::IVirtualHid> dev = m_factory ? m_factory() : input::makeVirtualHid();
    if (!dev) return unavailableReason();

    auto s = std::make_unique<Slot>();
    s->parsed = input::hid::parse(descriptor);
    s->numbered = !(s->parsed.reportIds.size() == 1 && s->parsed.reportIds.front() == 0);
    for (uint8_t id : s->parsed.reportIds) {
        const size_t n = input::hid::reportBytes(s->parsed, input::hid::Kind::Input, id);
        if (n) s->inputBytes[id] = n - (s->numbered ? 1 : 0);
    }
    dev->setRequestHandler([this, slot](input::IVirtualHid::Request kind, uint8_t reportId,
                                        const std::vector<uint8_t>& data) {
        if (!m_onRequest) return;
        HidRequest r;
        r.slot = slot;
        r.kind = kind == input::IVirtualHid::Request::Output       ? HidRequest::Kind::Output
                 : kind == input::IVirtualHid::Request::GetFeature ? HidRequest::Kind::GetFeature
                                                                   : HidRequest::Kind::SetFeature;
        r.reportId = reportId;
        r.data = data;
        m_onRequest(r);
    });
    input::VirtualHidIdentity id;
    id.name = device.name;
    id.vendorId = device.vendorId;
    id.productId = device.productId;
    id.version = device.version;
    id.descriptor = descriptor;
    std::string error;
    if (!dev->create(id, error)) return error;
    s->device = std::move(dev);

    std::unique_ptr<Slot> previous;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        previous = std::move(m_slots[slot]);
        m_slots[slot] = std::move(s);
        if (!m_watchdog.joinable()) m_watchdog = std::thread([this] { watchdog(); });
    }
    previous.reset(); // its device goes away outside the lock
    char ids[16];
    std::snprintf(ids, sizeof ids, "%04x:%04x", device.vendorId, device.productId);
    log::info("[hid] slot " + std::to_string(slot) + ": " + device.name + " " + ids +
              " created, descriptor " + std::to_string(descriptor.size()) + " bytes");
    return {};
}

void HidPassthrough::input(int slot, uint8_t reportId, uint16_t seq, const uint8_t* bytes,
                           size_t size)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_slots.find(slot);
    if (it == m_slots.end()) return;
    Slot& s = *it->second;
    auto want = s.inputBytes.find(reportId);
    if (want == s.inputBytes.end() || want->second != size) return; // not a report this device has
    auto seen = s.lastSeq.find(reportId);
    if (seen != s.lastSeq.end() && static_cast<int16_t>(seq - seen->second) <= 0) return; // late
    s.lastSeq[reportId] = seq;

    std::vector<uint8_t> report;
    report.reserve(size + 1);
    if (s.numbered) report.push_back(reportId);
    report.insert(report.end(), bytes, bytes + size);
    s.device->input(report.data(), report.size());
    s.last[reportId].assign(bytes, bytes + size);
    s.lastAtMs = m_clock();
    s.resting = false;
}

void HidPassthrough::detach(int slot)
{
    std::unique_ptr<Slot> gone;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_slots.find(slot);
        if (it == m_slots.end()) return;
        gone = std::move(it->second);
        m_slots.erase(it);
    }
    log::info("[hid] slot " + std::to_string(slot) + " removed");
}

void HidPassthrough::detachAll()
{
    std::map<int, std::unique_ptr<Slot>> gone;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        gone.swap(m_slots);
    }
}

void HidPassthrough::checkSilence(int64_t nowMs)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [slot, sp] : m_slots) {
        Slot& s = *sp;
        if (s.resting || nowMs - s.lastAtMs < kSilenceMs) continue;
        s.resting = true;
        for (auto& [id, last] : s.last) {
            std::vector<uint8_t> rest = input::hid::restReport(s.parsed, id, last);
            if (rest == last) continue;
            last = rest;
            if (s.numbered) rest.insert(rest.begin(), id);
            s.device->input(rest.data(), rest.size());
        }
        log::info("[hid] slot " + std::to_string(slot) + " silent for " +
                  std::to_string(kSilenceMs) + " ms: put to rest");
    }
}

void HidPassthrough::watchdog()
{
    while (!m_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        checkSilence(m_clock());
    }
}

} // namespace mw::native
