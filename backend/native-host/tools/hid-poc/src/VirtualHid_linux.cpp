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

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSocketNotifier>
#include <QTimer>

#include <cerrno>
#include <cstring>
#include <deque>

#include <fcntl.h>
#include <linux/input.h>
#include <linux/uhid.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

namespace {

int64_t monotonicMicros()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

QString readSys(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll()).trimmed();
}

/// A device made through /dev/uhid. Its `uniq` is a token of ours, which is how
/// the evdev and hidraw nodes the kernel makes of it are found again in sysfs.
class UhidDevice final : public VirtualHid
{
public:
    using VirtualHid::VirtualHid;
    ~UhidDevice() override { destroy(); }

    bool create(const VirtualHidIdentity& id, QString* error) override
    {
        m_fd = ::open("/dev/uhid", O_RDWR | O_CLOEXEC | O_NONBLOCK);
        if (m_fd < 0) {
            *error = QStringLiteral("cannot open /dev/uhid (%1): run as root or add a udev rule")
                         .arg(QString::fromLocal8Bit(strerror(errno)));
            return false;
        }
        static int s_count = 0;
        m_uniq = QStringLiteral("mw-hid-poc-%1-%2").arg(::getpid()).arg(++s_count);

        uhid_event ev{};
        ev.type = UHID_CREATE2;
        auto& c = ev.u.create2;
        const QByteArray name = id.name.toUtf8().left(sizeof(c.name) - 1);
        memcpy(c.name, name.constData(), static_cast<size_t>(name.size()));
        const QByteArray phys = QByteArrayLiteral("moonlightweb/hid-poc");
        memcpy(c.phys, phys.constData(), static_cast<size_t>(phys.size()));
        const QByteArray uniq = m_uniq.toUtf8();
        memcpy(c.uniq, uniq.constData(), static_cast<size_t>(uniq.size()));
        if (id.descriptor.size() > sizeof(c.rd_data)) {
            *error = QStringLiteral("descriptor over %1 bytes").arg(sizeof(c.rd_data));
            destroy();
            return false;
        }
        c.rd_size = static_cast<__u16>(id.descriptor.size());
        memcpy(c.rd_data, id.descriptor.data(), id.descriptor.size());
        c.bus = BUS_USB;
        c.vendor = id.vendorId;
        c.product = id.productId;
        c.version = id.version;
        if (!send(ev)) {
            *error = QStringLiteral("UHID_CREATE2 refused (%1)")
                         .arg(QString::fromLocal8Bit(strerror(errno)));
            destroy();
            return false;
        }
        m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
        connect(m_notifier, &QSocketNotifier::activated, this, [this] { readUhid(); });
        // The kernel makes the nodes once the HID driver has bound: look for a while.
        m_findTries = 0;
        QTimer::singleShot(100, this, [this] { findNodes(); });
        return true;
    }

    bool input(const QByteArray& report) override
    {
        if (m_fd < 0 || report.isEmpty() || report.size() > UHID_DATA_MAX) return false;
        uhid_event ev{};
        ev.type = UHID_INPUT2;
        ev.u.input2.size = static_cast<__u16>(report.size());
        memcpy(ev.u.input2.data, report.constData(), static_cast<size_t>(report.size()));
        const int64_t t = monotonicMicros();
        if (!send(ev)) return false;
        if (m_evdevFd >= 0) {
            m_pending.push_back(t);
            if (m_pending.size() > 256) m_pending.pop_front();
        }
        return true;
    }

    void destroy() override
    {
        delete m_evdevNotifier;
        m_evdevNotifier = nullptr;
        if (m_evdevFd >= 0) ::close(m_evdevFd);
        m_evdevFd = -1;
        delete m_notifier;
        m_notifier = nullptr;
        if (m_fd >= 0) {
            uhid_event ev{};
            ev.type = UHID_DESTROY;
            send(ev);
            ::close(m_fd);
        }
        m_fd = -1;
    }

    QString nodes() const override { return m_nodes; }

private:
    bool send(const uhid_event& ev)
    {
        return ::write(m_fd, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev));
    }

    void readUhid()
    {
        uhid_event ev{};
        while (::read(m_fd, &ev, sizeof(ev)) > 0) {
            switch (ev.type) {
            case UHID_OUTPUT: {
                const auto& o = ev.u.output;
                const QByteArray data(reinterpret_cast<const char*>(o.data), o.size);
                emit request(Request::Output, o.rtype, data.isEmpty() ? 0 : uint8_t(data[0]), data);
                break;
            }
            case UHID_GET_REPORT: {
                const auto& g = ev.u.get_report;
                emit request(Request::GetFeature, g.rtype, g.rnum, {});
                uhid_event reply{};
                reply.type = UHID_GET_REPORT_REPLY;
                reply.u.get_report_reply.id = g.id;
                reply.u.get_report_reply.err = EIO;
                send(reply);
                break;
            }
            case UHID_SET_REPORT: {
                const auto& s = ev.u.set_report;
                emit request(Request::SetFeature, s.rtype, s.rnum,
                             QByteArray(reinterpret_cast<const char*>(s.data), s.size));
                uhid_event reply{};
                reply.type = UHID_SET_REPORT_REPLY;
                reply.u.set_report_reply.id = s.id;
                reply.u.set_report_reply.err = 0;
                send(reply);
                break;
            }
            default: break; // START, STOP, OPEN, CLOSE: nothing to do here
            }
        }
    }

    void findNodes()
    {
        QStringList found;
        QString eventNode;
        const QDir input(QStringLiteral("/sys/class/input"));
        for (const QString& e :
             input.entryList({QStringLiteral("event*")}, QDir::Dirs | QDir::System)) {
            if (readSys(input.filePath(e) + QStringLiteral("/device/uniq")) != m_uniq) continue;
            found << QStringLiteral("/dev/input/") + e;
            if (eventNode.isEmpty()) eventNode = QStringLiteral("/dev/input/") + e;
        }
        const QDir hidraw(QStringLiteral("/sys/class/hidraw"));
        for (const QString& h :
             hidraw.entryList({QStringLiteral("hidraw*")}, QDir::Dirs | QDir::System)) {
            const QString uevent = readSys(hidraw.filePath(h) + QStringLiteral("/device/uevent"));
            if (uevent.contains(QStringLiteral("HID_UNIQ=") + m_uniq))
                found << QStringLiteral("/dev/") + h;
        }
        if (found.isEmpty() && ++m_findTries < 20) {
            QTimer::singleShot(100, this, [this] { findNodes(); });
            return;
        }
        m_nodes =
            found.isEmpty() ? QStringLiteral("(no node found)") : found.join(QStringLiteral(", "));
        if (!eventNode.isEmpty()) watchEvdev(eventNode);
    }

    /// Reads the device's first evdev node to time each report from write to
    /// event, on the monotonic clock both sides read.
    void watchEvdev(const QString& node)
    {
        m_evdevFd = ::open(node.toLocal8Bit().constData(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (m_evdevFd < 0) return;
        int clk = CLOCK_MONOTONIC;
        ::ioctl(m_evdevFd, EVIOCSCLOCKID, &clk);
        m_evdevNotifier = new QSocketNotifier(m_evdevFd, QSocketNotifier::Read, this);
        connect(m_evdevNotifier, &QSocketNotifier::activated, this, [this] {
            input_event e{};
            while (::read(m_evdevFd, &e, sizeof(e)) == static_cast<ssize_t>(sizeof(e))) {
                if (e.type != EV_SYN || e.code != SYN_REPORT) continue;
                const int64_t t =
                    static_cast<int64_t>(e.input_event_sec) * 1000000 + e.input_event_usec;
                // The latest write before this event is the report that made it;
                // writes that changed nothing made no event and are dropped.
                int64_t match = -1;
                while (!m_pending.empty() && m_pending.front() <= t) {
                    match = m_pending.front();
                    m_pending.pop_front();
                }
                if (match >= 0) emit injected(static_cast<double>(t - match));
            }
        });
    }

    int m_fd = -1;
    int m_evdevFd = -1;
    QSocketNotifier* m_notifier = nullptr;
    QSocketNotifier* m_evdevNotifier = nullptr;
    QString m_uniq;
    QString m_nodes;
    int m_findTries = 0;
    std::deque<int64_t> m_pending;
};

} // namespace

std::unique_ptr<VirtualHid> VirtualHid::make(QObject* parent)
{
    return std::make_unique<UhidDevice>(parent);
}

QString VirtualHid::unavailableReason()
{
    if (::access("/dev/uhid", F_OK) != 0) return QStringLiteral("no /dev/uhid (modprobe uhid)");
    return {};
}
