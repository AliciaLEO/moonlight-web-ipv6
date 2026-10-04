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

#include "input/VirtualHid.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "WinUHid.h"

namespace mw::native::input {

namespace {

/// A device made through the "MoonlightWeb Virtual HID" driver
/// (drivers/vhid), with WinUHid's client library built in. The driver lets
/// administrators and SYSTEM only open its control device: the worker the
/// service launches runs with the user's elevated token, so it can.
class VhidDevice final : public IVirtualHid
{
public:
    ~VhidDevice() override { destroy(); }

    bool create(const VirtualHidIdentity& id, std::string& error) override
    {
        WINUHID_DEVICE_CONFIG c{};
        c.SupportedEvents = static_cast<WINUHID_EVENT_TYPE>(
            WINUHID_EVENT_WRITE_REPORT | WINUHID_EVENT_GET_FEATURE | WINUHID_EVENT_SET_FEATURE);
        c.VendorID = id.vendorId;
        c.ProductID = id.productId;
        c.VersionNumber = id.version;
        c.ReportDescriptorLength = static_cast<USHORT>(id.descriptor.size());
        c.ReportDescriptor = id.descriptor.data();
        // No hardware ids: given some, VHF made no child at all (04/10). The
        // collections enumerate as HID\HID_DEVICE_SYSTEM_VHF&COLnn, and
        // HidD_GetAttributes still gives the vendor and product ids.
        m_device = WinUHidCreateDevice(&c);
        if (!m_device) {
            error = "WinUHidCreateDevice failed (error " + std::to_string(::GetLastError()) + ")";
            return false;
        }
        if (!WinUHidStartDevice(m_device, &VhidDevice::onEvent, this)) {
            error = "WinUHidStartDevice failed (error " + std::to_string(::GetLastError()) + ")";
            WinUHidDestroyDevice(m_device);
            m_device = nullptr;
            return false;
        }
        return true;
    }

    bool input(const uint8_t* report, size_t size) override
    {
        return m_device && WinUHidSubmitInputReport(m_device, report, static_cast<DWORD>(size));
    }

    void destroy() override
    {
        if (!m_device) return;
        WinUHidStopDevice(m_device);
        WinUHidDestroyDevice(m_device);
        m_device = nullptr;
    }

private:
    // On WinUHid's event thread: every request is completed at once.
    static VOID onEvent(PVOID context, PWINUHID_DEVICE device, PCWINUHID_EVENT ev)
    {
        auto* self = static_cast<VhidDevice*>(context);
        switch (ev->Type) {
        case WINUHID_EVENT_WRITE_REPORT:
        case WINUHID_EVENT_SET_FEATURE: {
            std::vector<uint8_t> data(ev->Write.Data, ev->Write.Data + ev->Write.DataLength);
            WinUHidCompleteWriteEvent(device, ev, TRUE);
            if (self->m_onRequest)
                self->m_onRequest(ev->Type == WINUHID_EVENT_WRITE_REPORT ? Request::Output
                                                                         : Request::SetFeature,
                                  ev->ReportId, data);
            break;
        }
        case WINUHID_EVENT_GET_FEATURE:
            WinUHidCompleteReadEvent(device, ev, nullptr, 0);
            if (self->m_onRequest) self->m_onRequest(Request::GetFeature, ev->ReportId, {});
            break;
        default: break;
        }
    }

    PWINUHID_DEVICE m_device = nullptr;
};

} // namespace

std::unique_ptr<IVirtualHid> makeVirtualHid()
{
    return std::make_unique<VhidDevice>();
}

std::string virtualHidUnavailableReason()
{
    if (WinUHidGetDriverInterfaceVersion() == 0)
        return "the MoonlightWeb Virtual HID driver is not installed (error " +
               std::to_string(::GetLastError()) + ")";
    return {};
}

} // namespace mw::native::input
