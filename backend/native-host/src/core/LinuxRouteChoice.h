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

// Which chain a build of a Linux session runs its pictures through (plan
// pipeline-video-d3d12-v2, Phase 13). Pure, so that it is tested everywhere.
//
// Two questions, in this order. The encoder: VA-API (today's), Vulkan Video,
// or the CPU — the bench key over the setting over the vendor table. Then,
// in front of VA-API, the conversion: GL, or Vulkan on a compute queue (the
// split route, plan §9-17) — the bench key over the vendor table.
//
// Whatever cannot carry THIS build is refused, by name, and the chain drops to
// the next one down — Vulkan Video → VA-API → the CPU, Vulkan compute → GL —
// never forced onto a machine that cannot run it (Bruno, 28/09/2026: a
// firmware or a driver that does not encode reliably in Vulkan gets VA-API,
// on its own). What refuses is asked again at the next build; what the
// session learned while streaming (a Vulkan device lost, a pixel proof that
// failed) comes in as a refusal it carries for the rest of the session.
//
// The vendor table is where defaults change, one line per vendor, after a
// bench and on Bruno's word — never on a measurement alone.

#include "mw/native/Capabilities.h"
#include "mw/native/EncoderTuning.h"
#include "mw/native/VideoPipeline.h"

#include <cstdint>
#include <string>

namespace mw::native {

/// What one build of a Linux session knows when it picks its chain.
struct LinuxRouteFacts
{
    /// The bench's keys (EncoderTuning::pipeline, convertLinux) and the setting
    /// (SessionConfig::videoPipeline). Auto — and Windows' values — are no
    /// opinion.
    VideoPipeline benchKey = VideoPipeline::Auto;
    VideoPipeline setting = VideoPipeline::Auto;
    EncoderTuning::ConvertLinux convertKey = EncoderTuning::ConvertLinux::Default;

    /// What the Selector picked: VaApi when the GPU encodes through VA-API,
    /// Software (OpenH264) when nothing on the machine does.
    EncoderApi encoder = EncoderApi::None;
    Codec codec = Codec::H264;
    /// PCI vendor of the GPU the display scans out of (0x1002 AMD, 0x8086
    /// Intel, 0x10DE NVIDIA); 0 when unknown. The vendor table's key.
    uint32_t vendorId = 0;

    // ── What rules a chain out for this build ───────────────────────────────

    /// The portal hands shared memory, not a DMA-BUF: only the CPU reads it.
    bool sharedMemory = false;
    /// VA-API encodes but writes no parameter sets (VaapiEncoder::
    /// kNoParameterSets), learned by an earlier build of this session.
    bool vaapiUnusable = false;

    /// The Vulkan conversion is compiled in (MW_NATIVE_LINUX_VULKAN), and why
    /// it was given up on for this session — its device lost, an import
    /// refused, a device missing what it needs — or "" while it is not.
    bool vulkanConvertBuilt = false;
    std::string vulkanConvertRefusal;

    /// The Vulkan Video encoder (C13.5) is compiled in, and why this GPU's is
    /// not trusted — no encode queue for the codec, a driver or firmware
    /// under the minimums, the pixel proof at its opening failed — or "".
    bool vulkanEncoderBuilt = false;
    std::string vulkanEncoderRefusal;
};

struct LinuxRoute
{
    enum class Encoder
    {
        Vaapi,
        Vulkan,
        Cpu
    };
    enum class Conversion
    {
        Gl,
        Vulkan,
        Cpu
    };
    Encoder encoder = Encoder::Vaapi;
    Conversion conversion = Conversion::Gl;
    /// For SessionInfo: Vaapi or Vulkan; Auto on the CPU tier, which is no
    /// chain of either.
    VideoPipeline pipeline = VideoPipeline::Vaapi;
    /// "EGL → VA-API", "Vulkan compute → VA-API", "Vulkan compute → Vulkan
    /// Video", "CPU → OpenH264": the log's words and the bench's column.
    std::string route;
    /// Why: which of the bench key, the setting and the vendor table decided,
    /// and what refused whatever was asked for and could not run.
    std::string reason;
    /// Something was asked for — a chain or a conversion — and another runs.
    bool refused = false;
};

/// The vendor table's chain on Linux: VA-API wherever the Selector found a
/// VA-API encoder, until G5 (C13.7) has measured Vulkan Video on a vendor and
/// Bruno has moved its line. On the CPU tier (NVIDIA today: VA-API does not
/// encode there) the table has no opinion.
inline VideoPipeline autoLinuxPipeline(uint32_t vendorId)
{
    switch (vendorId) {
    case 0x1002: return VideoPipeline::Vaapi; // AMD: G5 first (C13.7)
    case 0x8086: return VideoPipeline::Vaapi; // Intel: ANV's encoder still young (§4.8)
    case 0x10DE: return VideoPipeline::Vaapi; // NVIDIA: the Selector gives it the CPU
    default: return VideoPipeline::Vaapi;
    }
}

/// The vendor table's conversion in front of VA-API: GL everywhere until the
/// split route's bench (C13.4 ter) has measured a vendor and Bruno has moved
/// its line.
inline EncoderTuning::ConvertLinux autoLinuxConversion(uint32_t vendorId)
{
    switch (vendorId) {
    case 0x1002: return EncoderTuning::ConvertLinux::Gl; // bench §8o, then Bruno
    case 0x8086: return EncoderTuning::ConvertLinux::Gl;
    default: return EncoderTuning::ConvertLinux::Gl;
    }
}

namespace linuxroute_detail {

inline const char* vendorName(uint32_t vendorId)
{
    switch (vendorId) {
    case 0x1002: return "AMD";
    case 0x8086: return "Intel";
    case 0x10DE: return "NVIDIA";
    default: return "this GPU";
    }
}

/// A Linux value of @p p, or Auto: Windows' chains are no opinion here.
inline VideoPipeline linuxValue(VideoPipeline p)
{
    return p == VideoPipeline::Vaapi || p == VideoPipeline::Vulkan ? p : VideoPipeline::Auto;
}

/// Why the Vulkan Video chain cannot carry this build, or "".
inline std::string vulkanEncoderRefusal(const LinuxRouteFacts& f)
{
    if (!f.vulkanEncoderBuilt) return "the Vulkan Video encoder is not built yet";
    if (!f.vulkanEncoderRefusal.empty()) return f.vulkanEncoderRefusal;
    if (f.sharedMemory) return "the portal gives shared memory, which only the CPU reads";
    if (f.codec != Codec::Hevc)
        return std::string(toString(f.codec)) + " is not done by the Vulkan Video encoder yet";
    return {};
}

/// Why the Vulkan conversion cannot feed VA-API in this build, or "".
inline std::string vulkanConvertRefusal(const LinuxRouteFacts& f)
{
    if (!f.vulkanConvertBuilt) return "the Vulkan conversion is not built in";
    if (!f.vulkanConvertRefusal.empty()) return f.vulkanConvertRefusal;
    return {};
}

inline LinuxRoute cpuRoute(std::string reason, bool refused)
{
    LinuxRoute r;
    r.encoder = LinuxRoute::Encoder::Cpu;
    r.conversion = LinuxRoute::Conversion::Cpu;
    r.pipeline = VideoPipeline::Auto;
    r.route = "CPU → OpenH264";
    r.reason = std::move(reason);
    r.refused = refused;
    return r;
}

} // namespace linuxroute_detail

inline LinuxRoute chooseLinuxRoute(const LinuxRouteFacts& f)
{
    using namespace linuxroute_detail;

    // ── The encoder ──
    VideoPipeline wanted = VideoPipeline::Auto;
    std::string why;
    if (linuxValue(f.benchKey) != VideoPipeline::Auto) {
        wanted = f.benchKey;
        why = std::string("the bench key pipeline=") + toString(f.benchKey);
    } else if (linuxValue(f.setting) != VideoPipeline::Auto) {
        wanted = f.setting;
        why = std::string("the setting (") + toString(f.setting) + ")";
    } else {
        wanted = autoLinuxPipeline(f.vendorId);
        why = std::string("auto: the vendor table has ") +
              (wanted == VideoPipeline::Vulkan ? "Vulkan Video" : "VA-API") + " for " +
              vendorName(f.vendorId);
    }

    std::string refusedBecause;
    if (wanted == VideoPipeline::Vulkan) {
        const std::string no = vulkanEncoderRefusal(f);
        if (no.empty()) {
            LinuxRoute r;
            r.encoder = LinuxRoute::Encoder::Vulkan;
            r.conversion = LinuxRoute::Conversion::Vulkan;
            r.pipeline = VideoPipeline::Vulkan;
            r.route = "Vulkan compute → Vulkan Video";
            r.reason = why;
            return r;
        }
        refusedBecause = why + " asks for Vulkan Video, which cannot run: " + no;
    }

    // VA-API, or the CPU when there is none to be had.
    const bool vaapi = f.encoder == EncoderApi::VaApi && !f.sharedMemory && !f.vaapiUnusable;
    if (!vaapi) {
        std::string cpu = f.sharedMemory
                              ? "the portal gives shared memory, which only the CPU reads"
                          : f.vaapiUnusable ? "VA-API writes no parameter sets on this driver"
                                            : "no GPU encoder on this machine";
        if (!refusedBecause.empty())
            return cpuRoute(refusedBecause + "; the CPU runs: " + cpu, true);
        // Refused only when someone asked for VA-API by name; the table's
        // VA-API on a machine without it is the CPU tier doing its job.
        const bool asked = linuxValue(f.benchKey) != VideoPipeline::Auto ||
                           linuxValue(f.setting) != VideoPipeline::Auto;
        return cpuRoute(asked ? why + " asks for VA-API; the CPU runs: " + cpu : cpu, asked);
    }

    // ── The conversion in front of VA-API ──
    LinuxRoute r;
    r.encoder = LinuxRoute::Encoder::Vaapi;
    r.pipeline = VideoPipeline::Vaapi;
    r.refused = !refusedBecause.empty();
    std::string convertWhy;
    EncoderTuning::ConvertLinux conversion = f.convertKey;
    if (conversion != EncoderTuning::ConvertLinux::Default) {
        convertWhy = std::string("the bench key convert=") +
                     (conversion == EncoderTuning::ConvertLinux::Vulkan ? "vulkan" : "gl");
    } else {
        conversion = autoLinuxConversion(f.vendorId);
        convertWhy = std::string("the vendor table converts with ") +
                     (conversion == EncoderTuning::ConvertLinux::Vulkan ? "Vulkan compute" : "GL") +
                     " for " + vendorName(f.vendorId);
    }
    if (conversion == EncoderTuning::ConvertLinux::Vulkan) {
        const std::string no = vulkanConvertRefusal(f);
        if (no.empty()) {
            r.conversion = LinuxRoute::Conversion::Vulkan;
            r.route = "Vulkan compute → VA-API";
        } else {
            r.conversion = LinuxRoute::Conversion::Gl;
            r.route = "EGL → VA-API";
            r.refused = true;
            convertWhy += " asks for Vulkan compute, GL converts: " + no;
        }
    } else {
        r.conversion = LinuxRoute::Conversion::Gl;
        r.route = "EGL → VA-API";
    }
    r.reason =
        (refusedBecause.empty() ? why : refusedBecause + "; VA-API runs") + "; " + convertWhy;
    return r;
}

} // namespace mw::native
