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

#include "Capabilities.h"

#include <cctype>
#include <string>

namespace mw::native {

/// Which chain carries a Windows session's pictures from the capture to the
/// encoder. D3D11 is the one every release has shipped. D3D12 opens the
/// captured surface in D3D12 once (the capture APIs only hand it to D3D11)
/// and converts and encodes there, on queues whose priority the engine picks.
///
/// Chosen for each build of a session, never sticky: a D3D12 session that
/// cannot be built, or that fails while streaming, goes back to D3D11 on its
/// own and says why (SessionInfo). The other platforms have neither chain and
/// ignore the choice.
enum class VideoPipeline
{
    Auto, ///< the vendor table below: the product's default
    D3d11,
    D3d12,
};

inline const char* toString(VideoPipeline p)
{
    switch (p) {
    case VideoPipeline::Auto: return "auto";
    case VideoPipeline::D3d11: return "d3d11";
    case VideoPipeline::D3d12: return "d3d12";
    }
    return "auto";
}

/// "auto", "d3d11" or "d3d12", in any case. False, with @p out untouched, for
/// anything else.
inline bool parseVideoPipeline(const std::string& text, VideoPipeline& out)
{
    std::string t;
    for (char c : text)
        t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (t == "auto")
        out = VideoPipeline::Auto;
    else if (t == "d3d11")
        out = VideoPipeline::D3d11;
    else if (t == "d3d12")
        out = VideoPipeline::D3d12;
    else
        return false;
    return true;
}

/// What Auto means on a GPU reached through @p api: one line per vendor.
/// D3D11 everywhere until a gate of the D3D12 plan has measured a vendor and
/// Bruno has moved its line — a default never changes on a measurement alone.
inline VideoPipeline autoVideoPipeline(EncoderApi api)
{
    switch (api) {
    case EncoderApi::Nvenc: return VideoPipeline::D3d11; // G2, then G4 (NVENC on D3D12)
    case EncoderApi::Amf: return VideoPipeline::D3d11;   // G2, then G4 (AMF on D3D12)
    case EncoderApi::Vpl: return VideoPipeline::D3d11;   // G2, then G3 (in-house rate control)
    default: return VideoPipeline::D3d11;                // no D3D12 route at all
    }
}

} // namespace mw::native
