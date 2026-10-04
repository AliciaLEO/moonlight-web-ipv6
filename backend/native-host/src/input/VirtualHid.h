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

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

/// One device of the HID passthrough recreated on this host
/// (docs/design/hid-passthrough-study.md, plan P2): the browser reads it
/// through WebHID, the host makes the same one here, `uhid` on Linux and the
/// "MoonlightWeb Virtual HID" driver on Windows (drivers/vhid).
///
/// The descriptor handed to create() has passed hid::validate() already: the
/// engine rebuilds it from the page's collections and checks it before any
/// backend sees it. The Windows driver checks it once more.
namespace mw::native::input {

struct VirtualHidIdentity
{
    std::string name;
    uint16_t vendorId = 0;
    uint16_t productId = 0;
    uint16_t version = 0;
    std::vector<uint8_t> descriptor;
};

class IVirtualHid
{
public:
    /// What the OS asked of the device, for the page to pass to the real one.
    enum class Request
    {
        Output,     // an output report written by an application
        GetFeature, // answered at once with an error until P3 forwards it
        SetFeature, // acknowledged at once
    };
    /// Called on a backend thread, never under a lock the caller holds; data
    /// starts with the report id when the descriptor numbers its reports.
    using RequestHandler =
        std::function<void(Request kind, uint8_t reportId, const std::vector<uint8_t>& data)>;

    virtual ~IVirtualHid() = default;

    virtual bool create(const VirtualHidIdentity& identity, std::string& error) = 0;
    /// One input report as the device sends it, report id first when the
    /// descriptor numbers its reports. Safe from any thread, never blocks long.
    virtual bool input(const uint8_t* report, size_t size) = 0;
    /// Removes the device; also done by the destructor.
    virtual void destroy() = 0;

    void setRequestHandler(RequestHandler handler) { m_onRequest = std::move(handler); }

protected:
    RequestHandler m_onRequest;
};

/// A backend for this OS, or null where there is none (macOS).
std::unique_ptr<IVirtualHid> makeVirtualHid();

/// Empty when this host can create devices now; otherwise why not, in a short
/// English sentence (no /dev/uhid access, driver not installed).
std::string virtualHidUnavailableReason();

} // namespace mw::native::input
