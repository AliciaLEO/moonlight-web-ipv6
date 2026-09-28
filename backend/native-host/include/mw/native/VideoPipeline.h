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

/// Which chain carries a session's pictures from the capture to the encoder.
///
/// Windows: D3D11 is the one every release has shipped. D3D12 opens the
/// captured surface in D3D12 once (the capture APIs only hand it to D3D11)
/// and converts and encodes there, on queues whose priority the engine picks.
///
/// Linux (plan Phase 13): VA-API is today's encoder, fed by a GL conversion
/// or — the split route — by a Vulkan compute one; Vulkan is the whole chain
/// in Vulkan, Vulkan Video encoding included. A platform ignores the other's
/// values, as it ignores Auto's table for a chain it does not have.
///
/// Chosen for each build of a session, never sticky: a chain that cannot be
/// built, or that fails while streaming, goes back to the default one on its
/// own and says why (SessionInfo). macOS has no choice and ignores it.
enum class VideoPipeline
{
    Auto, ///< the vendor table below: the product's default
    D3d11,
    D3d12,
    Vaapi,  ///< Linux: VA-API encoding (today's)
    Vulkan, ///< Linux: Vulkan Video encoding, the whole chain in Vulkan
};

inline const char* toString(VideoPipeline p)
{
    switch (p) {
    case VideoPipeline::Auto: return "auto";
    case VideoPipeline::D3d11: return "d3d11";
    case VideoPipeline::D3d12: return "d3d12";
    case VideoPipeline::Vaapi: return "vaapi";
    case VideoPipeline::Vulkan: return "vulkan";
    }
    return "auto";
}

/// Whether @p p names one of Windows' chains — Auto included, which is every
/// platform's.
inline bool isWindowsPipeline(VideoPipeline p)
{
    return p == VideoPipeline::Auto || p == VideoPipeline::D3d11 || p == VideoPipeline::D3d12;
}

/// "auto", "d3d11", "d3d12", "vaapi" or "vulkan", in any case. False, with @p
/// out untouched, for anything else.
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
    else if (t == "vaapi")
        out = VideoPipeline::Vaapi;
    else if (t == "vulkan")
        out = VideoPipeline::Vulkan;
    else
        return false;
    return true;
}

/// What Auto means on a GPU reached through @p api: one line per vendor.
/// A line moves once a gate of the D3D12 plan has measured its vendor and
/// Bruno has decided — a default never changes on a measurement alone.
/// Intel's moved on 28/09/2026 (plan §9-23): D3D12 halved the Arc's time on
/// the host and kept a game's pace where D3D11 skipped pictures; the N95 was
/// faster at rest and no slower under load (G3, then Bruno's own test, C5.7).
inline VideoPipeline autoVideoPipeline(EncoderApi api)
{
    switch (api) {
    case EncoderApi::Nvenc: return VideoPipeline::D3d11; // G2, then G4 (NVENC on D3D12)
    case EncoderApi::Amf: return VideoPipeline::D3d11;   // G2, then G4 (AMF on D3D12)
    case EncoderApi::Vpl: return VideoPipeline::D3d12;   // G3 and C5.7: Bruno, 28/09 (§9-23)
    case EncoderApi::VaApi: return VideoPipeline::Vaapi; // Linux: until G5 (Phase 13)
    default: return VideoPipeline::D3d11;                // no D3D12 route at all
    }
}

} // namespace mw::native
