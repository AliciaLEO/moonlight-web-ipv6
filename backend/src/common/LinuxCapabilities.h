/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
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

#include <QString>

/**
 * What this process does with the capabilities the Linux launcher hands it.
 *
 * Two, both for the native engine:
 *  - CAP_SYS_ADMIN: the screen is captured through KMS, and the kernel hands
 *    framebuffer handles only to a process holding it;
 *  - CAP_SYS_NICE: the stream's GPU work — the colour conversion today — runs
 *    above normal priority, which amdgpu, i915 and xe grant to nobody else. Under
 *    a game that saturates the GPU, that halves the conversion's wait (46 → 23 ms
 *    on a Radeon 780M, docs/bench-native-host.md §8o.1).
 *
 * The Linux packages deliver them through `moonlightweb-launch`
 * (backend/packaging/linux), which carries the file capabilities and execs this
 * binary with them in its AMBIENT set — see that file for why they cannot sit on
 * this binary itself. So at main() this process may hold each in all of
 * permitted, effective and ambient. That is more than it should keep:
 *
 *  - EFFECTIVE is dropped. The HTTP server, the signalling, the relays run
 *    without either; the native engine raises one into the effective set of
 *    ITS thread around the one call that checks it, and lowers it again
 *    (native-host/src/platform/linux/ScopedCapability.h). Sunshine's posture
 *    (`cap_sys_admin=p` on its binary, raised per call), reproduced.
 *  - AMBIENT is lowered. Everything this process spawns — xdg-open and the
 *    browser behind it, the gio helper, a package installer — would otherwise
 *    inherit both silently. Only the native stream worker gets them,
 *    explicitly, through raiseStreamCapabilitiesForChild().
 *  - PERMITTED and INHERITABLE keep them: that is what lets the two raises above
 *    happen at all.
 *
 * Every function here is a no-op off Linux, and a no-op on a Linux process
 * that was not handed the capabilities (a tree run by hand, the AppImage, the
 * Docker image): the probe then reports the capture as unavailable and says
 * why, and the conversion runs at normal priority and says that too.
 */
namespace mw {

/// Call first thing in main(), before any thread exists (capabilities are
/// per-thread; the main thread's are what every later thread inherits).
/// Returns a one-line description of what was done for the log, or an empty
/// string when the process holds neither capability and nothing changed.
QString confineCapabilities();

/// True when this process may capture through KMS — CAP_SYS_ADMIN in its
/// permitted set, whatever the effective set says right now.
bool hasCaptureCapability();

/// For QProcess::setChildProcessModifier on the NATIVE stream worker: runs in
/// the forked child before exec, and puts back into the ambient set whichever
/// of the two capabilities this process holds, so the worker starts with them.
/// Raw syscalls only — the child is between fork and exec, where nothing may
/// allocate or lock. Does nothing when the parent was handed neither.
void raiseStreamCapabilitiesForChild();

} // namespace mw
