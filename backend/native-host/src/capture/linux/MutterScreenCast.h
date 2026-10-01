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

#include <cstdint>
#include <memory>
#include <string>

// GNOME's own screen casts: org.gnome.Mutter.ScreenCast, the D-Bus API under
// GNOME's portal backend and gnome-remote-desktop. The virtual display's route
// on GNOME (plan Idées Punktfunk, C2): no dialog, ever — the portal asks the
// user once, which nobody can answer on a host without a screen — and GNOME 42
// to 45 too, whose portal makes no virtual monitor (bench §8s.1).
//
// The conversation, on the session bus:
//
//   ScreenCast.CreateSession({})           → a session
//   Session.RecordVirtual({cursor-mode})   → a stream; Mutter makes the monitor
//       when the consumer negotiates the format, of the format's size and of
//       its maxFramerate as refresh (bench §8s.2)
//   or Session.RecordMonitor(connector, {cursor-mode}) → a stream of that
//       monitor as it is: another stream's virtual one, for a guest
//   Session.Start()                        → the stream says PipeWireStreamAdded(node)
//
// The pointer comes as metadata (cursor-mode 2), as the portal's handshake
// asks: Mutter 48 paints no pointer into a virtual monitor's DMA-BUF frames
// when a physical screen shows a hardware one (Punktfunk, mutter#4939). The
// node is on the session's own PipeWire. No RemoteDesktop session beside it:
// the input goes through uinput, as everywhere else, and a screen cast alone
// is what GNOME's portal backend asks of Mutter.
//
// The session belongs to this bus connection: stop() — or this process's end
// — closes it, and Mutter removes a monitor it made for it. Mutter also closes
// a session of its own accord: the monitor it records went, the desktop was
// locked (bench §8s.6). closed() says so.
//
// Nobody is asked who calls: Mutter looks at no caller (GNOME 42, 46 and 48,
// bench §8s), and a worker holding capabilities reads its bus address as usual
// (PortalScreenCast.h). ⚠️ sd-bus, under the exception LICENSE.md § "L'exception
// sd-bus" bounds, behind MW_NATIVE_LINUX_PORTAL with the portal's route.

namespace mw::native::capture {

class MutterScreenCast
{
public:
    MutterScreenCast();
    ~MutterScreenCast();

    MutterScreenCast(const MutterScreenCast&) = delete;
    MutterScreenCast& operator=(const MutterScreenCast&) = delete;

    /// The API's version on the session bus — 4 on GNOME 42, 46 and 48 — or
    /// 0, with @p why, when nobody answers: not GNOME, no session bus, a Mutter
    /// built without screen casts.
    static int version(std::string& why);

    /// The first version measured to make virtual monitors (bench §8s.1).
    static constexpr int kVirtualVersion = 4;

    struct Request
    {
        /// Empty: a virtual monitor for this stream. Otherwise the monitor of
        /// that connector ("Meta-0"), recorded as it is.
        std::string connector;
        /// The virtual monitor's mode, passed as RecordVirtual's `modes` when
        /// faster than 60 Hz: Mutter 50 takes its refresh from there
        /// (Punktfunk, 5120×1440 at 240 Hz), 42 to 48 from the format's
        /// maxFramerate, and ignore it (bench §8s.2). Zero: none.
        int width = 0;
        int height = 0;
        int refreshHz = 0;
    };

    /// Open the session and wait for its PipeWire @p node. False, with
    /// @p error, when Mutter refuses — no such API, a locked desktop ("Session
    /// creation inhibited"), no such connector — or does not answer in time.
    bool start(const Request& request, uint32_t& node, std::string& error);

    /// After a start() that failed: Mutter did not take the call at all — no
    /// such method, object or interface, access denied — where the portal
    /// may still serve. False when it took it and failed: a locked desktop,
    /// a monitor gone, a timeout, which another route would meet too.
    bool refused() const;

    /// Whether Mutter has closed the session since start() — or the bus went,
    /// gnome-shell with it. Cheap: what the bus has brought, without a wait.
    /// False before start().
    bool closed();

    /// The session closed: the monitor made for it goes.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mw::native::capture
