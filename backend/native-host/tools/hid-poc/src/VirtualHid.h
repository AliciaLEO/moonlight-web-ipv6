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

#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <cstdint>
#include <memory>
#include <vector>

/// What the device looks like to the host's OS. The descriptor has passed
/// hid::validate() already.
struct VirtualHidIdentity
{
    QString name;
    uint16_t vendorId = 0;
    uint16_t productId = 0;
    uint16_t version = 0;
    std::vector<uint8_t> descriptor;
};

/// One virtual HID device on this host: `uhid` on Linux, nothing elsewhere yet
/// (the Windows driver is P1). The prototype of P2's IVirtualHid.
class VirtualHid : public QObject
{
    Q_OBJECT

public:
    /// What the OS asked of the device, to hand back to the page.
    enum class Request
    {
        Output,     // an output report written by an application
        GetFeature, // answered at once with an error in this prototype
        SetFeature, // acknowledged at once
    };

    static std::unique_ptr<VirtualHid> make(QObject* parent = nullptr);
    /// Empty when this platform can create devices; why not otherwise.
    static QString unavailableReason();

    using QObject::QObject;
    ~VirtualHid() override = default;

    virtual bool create(const VirtualHidIdentity& id, QString* error) = 0;
    /// One input report as the device sends it: report id first when the
    /// descriptor numbers its reports.
    virtual bool input(const QByteArray& report) = 0;
    virtual void destroy() = 0;
    /// The nodes the OS made of it (hidraw, evdev), once known.
    virtual QString nodes() const = 0;

signals:
    void request(VirtualHid::Request kind, int reportType, int reportId, const QByteArray& data);
    /// Time from one input() to the evdev event it produced, in microseconds.
    void injected(double micros);
};
