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

#include "MonitorLayout.h"

#include <cstdint>
#include <string>
#include <vector>

// GNOME's monitor layout, read and changed over Mutter's DisplayConfig D-Bus
// interface — the one GNOME's own Settings use. It serves the portal's
// virtual monitor alone (MonitorLayout.h says why it is made primary), and so
// lives on the portal's route: sd-bus, behind MW_NATIVE_LINUX_PORTAL, under
// the exception LICENSE.md § "L'exception sd-bus" bounds.
//
// Not GNOME — KDE, wlroots, X11 — and nobody answers: every call returns
// false with the reason, and the monitor stays where its compositor put it.
//
// No capability is needed, and none is in the way: Mutter does not look at
// who calls, and the package's worker reads its bus address as usual (no
// AT_SECURE, PortalScreenCast.h). A binary setcap'd directly cannot find the
// bus, and says so.

namespace mw::native::capture {

class MutterDisplayConfig
{
public:
    /// The layout as Mutter has it now, with the serial a change must quote.
    static bool read(DisplayLayout& out, uint32_t& serial, std::string& why);

    /// The connectors Mutter knows now. False, with @p why, when nobody answers.
    static bool connectors(std::vector<std::string>& out, std::string& why);

    /// Make @p connector the desktop's primary, on the left of every other
    /// monitor, until it goes (Mutter's temporary configuration). @p how says
    /// what was done, or why nothing was.
    static bool makePrimary(const std::string& connector, std::string& how);
};

} // namespace mw::native::capture
