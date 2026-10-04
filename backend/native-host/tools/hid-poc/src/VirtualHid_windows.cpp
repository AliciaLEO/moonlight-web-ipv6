/*
 * MoonlightWeb — native capture & encoding engine: lab tools.
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

#include "VirtualHid.h"

#include <QMetaObject>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "WinUHid.h"

namespace {

QString lastError()
{
    const DWORD e = ::GetLastError();
    return QStringLiteral("error %1").arg(e);
}

/// A device made through the "MoonlightWeb Virtual HID" driver (WinUHid's
/// client library, third_party/winuhid). Input reports go straight to it;
/// what Windows asks back (output reports, features) is answered at once and
/// shown to the page, as on Linux.
class WinUHidDevice final : public VirtualHid
{
public:
    using VirtualHid::VirtualHid;
    ~WinUHidDevice() override { destroy(); }

    bool create(const VirtualHidIdentity& id, QString* error) override
    {
        WINUHID_DEVICE_CONFIG c{};
        c.SupportedEvents = static_cast<WINUHID_EVENT_TYPE>(
            WINUHID_EVENT_WRITE_REPORT | WINUHID_EVENT_GET_FEATURE | WINUHID_EVENT_SET_FEATURE);
        c.VendorID = id.vendorId;
        c.ProductID = id.productId;
        c.VersionNumber = id.version;
        c.ReportDescriptorLength = static_cast<USHORT>(id.descriptor.size());
        c.ReportDescriptor = id.descriptor.data();
        // No hardware ids: given some (VID_xxxx&PID_yyyy, REG_MULTI_SZ), VHF made no child at
        // all (04/10, UM790Pro). Without, the collections enumerate as
        // HID\HID_DEVICE_SYSTEM_VHF&COLnn, while HidD_GetAttributes still gives VID and PID.
        m_device = WinUHidCreateDevice(&c);
        if (!m_device) {
            *error = QStringLiteral("WinUHidCreateDevice failed (%1): is the driver installed?")
                         .arg(lastError());
            return false;
        }
        if (!WinUHidStartDevice(m_device, &WinUHidDevice::onEvent, this)) {
            *error = QStringLiteral("WinUHidStartDevice failed (%1)").arg(lastError());
            WinUHidDestroyDevice(m_device);
            m_device = nullptr;
            return false;
        }
        m_nodes = QStringLiteral("MoonlightWeb Virtual HID child %1:%2")
                      .arg(id.vendorId, 4, 16, QLatin1Char('0'))
                      .arg(id.productId, 4, 16, QLatin1Char('0'));
        return true;
    }

    bool input(const QByteArray& report) override
    {
        return m_device &&
               WinUHidSubmitInputReport(m_device, report.constData(), DWORD(report.size()));
    }

    void destroy() override
    {
        if (!m_device) return;
        WinUHidStopDevice(m_device);
        WinUHidDestroyDevice(m_device);
        m_device = nullptr;
    }

    QString nodes() const override { return m_nodes; }

private:
    // On WinUHid's event thread: complete the request now, tell the page from
    // the Qt thread.
    static VOID onEvent(PVOID context, PWINUHID_DEVICE device, PCWINUHID_EVENT ev)
    {
        auto* self = static_cast<WinUHidDevice*>(context);
        Request kind = Request::Output;
        QByteArray data;
        switch (ev->Type) {
        case WINUHID_EVENT_WRITE_REPORT:
            data = QByteArray(reinterpret_cast<const char*>(ev->Write.Data),
                              int(ev->Write.DataLength));
            WinUHidCompleteWriteEvent(device, ev, TRUE);
            break;
        case WINUHID_EVENT_SET_FEATURE:
            kind = Request::SetFeature;
            data = QByteArray(reinterpret_cast<const char*>(ev->Write.Data),
                              int(ev->Write.DataLength));
            WinUHidCompleteWriteEvent(device, ev, TRUE);
            break;
        case WINUHID_EVENT_GET_FEATURE:
            kind = Request::GetFeature;
            WinUHidCompleteReadEvent(device, ev, nullptr, 0); // no answer in this prototype
            break;
        default: return;
        }
        const int reportId = ev->ReportId;
        QMetaObject::invokeMethod(
            self, [self, kind, reportId, data] { emit self->request(kind, 0, reportId, data); },
            Qt::QueuedConnection);
    }

    PWINUHID_DEVICE m_device = nullptr;
    QString m_nodes;
};

} // namespace

std::unique_ptr<VirtualHid> VirtualHid::make(QObject* parent)
{
    return std::make_unique<WinUHidDevice>(parent);
}

QString VirtualHid::unavailableReason()
{
    if (WinUHidGetDriverInterfaceVersion() == 0)
        return QStringLiteral("\"MoonlightWeb Virtual HID\" driver not installed (%1)")
            .arg(lastError());
    return {};
}
