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

// macOS: out of scope for good (study §6). The page keeps the mapping there.

namespace mw::native::input {

std::unique_ptr<IVirtualHid> makeVirtualHid()
{
    return nullptr;
}

std::string virtualHidUnavailableReason()
{
    return "no virtual HID on this OS";
}

} // namespace mw::native::input
