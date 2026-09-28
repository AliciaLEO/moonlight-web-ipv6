/*
 * MoonlightWeb — native capture & encoding engine: Vulkan lab.
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

// `encode` — Vulkan Video HEVC as the Linux chain would drive it (plan
// pipeline-video-d3d12-v2, Phase 13, C13.1; the D3D12 lab's encode, C1.3).
//
// An endless GOP of P pictures at a steady rate, each predicting from the one
// before, with the rate control the chain would use: a constant QP set picture
// by picture (cqp, the in-house controller's lever — --qp-walk moves it on
// every picture, and the slice headers are read back to see it followed), or
// the driver's CBR or VBR with a one-picture virtual buffer (--change steps
// the rate every two seconds, without a reset). Each picture is copied into
// the encoder's own input on another queue, a stand-in for the conversion,
// and the encode waits for it on the GPU: one CPU wait per picture, the shape
// the chain would have. Wall time is submit to bitstream in hand.
//
// The parameter sets are ours — StdVideo structures, what ParameterSets would
// say — and the driver hands back the bytes it stands by
// (vkGetEncodedVideoSessionParametersKHR): they are read back and compared
// with what was asked, field by field, with the overrides it admits to.
//
// ffmpeg's error count is no proof (27/09/2026, the D3D12 chain's coded size):
// --dump-input writes the eight input pictures as raw NV12 at the coded size,
// frame n being input n % 8, and scripts/bench/hevc-psnr.py compares the
// decoded stream with them, picture by picture and band by band.

#include "Encode.h"

#include "Json.h"
#include "Lab.h"
#include "Vk.h"

#include "encode/HevcSliceParser.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace lab {
namespace {

using mw::native::encode::HevcNalUnit;
using mw::native::encode::HevcPpsFields;
using mw::native::encode::HevcSliceFields;
using mw::native::encode::HevcSpsFields;

struct Options
{
    std::string device;
    uint32_t width = 1920, height = 1080;
    /// The coded size is a multiple of this: 8 (the SPS's floor, MinCbSize),
    /// 16, or 64 (a whole coding tree block — the D3D12 lesson of C5.3).
    uint32_t align = 64;
    int fps = 60;
    int seconds = 10;
    std::string rc = "cqp";
    int qp = 30;
    bool qpWalk = false;
    int kbps = 20000;
    bool change = false;
    int quality = 0;
    /// DPB pictures, the one being coded included.
    uint32_t dpbSlots = 3;
    /// References kept in the RPS: the previous picture (used), and with 2
    /// one more before it, kept and unused — the one a repair after a loss
    /// would reach back to.
    uint32_t keep = 1;
    /// An IDR every that many pictures; 0 = the first only, 1 = all intra.
    int idrEvery = 0;
    bool sao = false;
    std::string priority = "default";
    std::string upload = "compute";
    bool split = false;
    std::string dump, dumpInput, csv, json;
};

uint32_t alignUp(uint32_t v, uint32_t a)
{
    return a ? (v + a - 1) / a * a : v;
}

/// The eight input pictures, NV12 at the coded size: a ramp that moves, fine
/// stripes (a text's worth of detail), blocks that come and go — so that
/// neither a still picture nor noise sets the rate.
std::vector<uint8_t> makePictures(uint32_t w, uint32_t h, int count)
{
    const size_t luma = static_cast<size_t>(w) * h;
    const size_t picture = luma + luma / 2;
    std::vector<uint8_t> all(picture * static_cast<size_t>(count));
    for (int k = 0; k < count; ++k) {
        uint8_t* y = all.data() + picture * static_cast<size_t>(k);
        uint8_t* uv = y + luma;
        for (uint32_t row = 0; row < h; ++row) {
            for (uint32_t col = 0; col < w; ++col) {
                int v = 40 + static_cast<int>((col + row / 2 + static_cast<uint32_t>(k) * 6) % 160);
                if (row % 90 < 30) v = ((col / 2 + static_cast<uint32_t>(k)) % 2) ? 200 : 40;
                if ((col / 16 + row / 16 + static_cast<uint32_t>(k)) % 7 == 0) v = 235 - v / 4;
                y[static_cast<size_t>(row) * w + col] =
                    static_cast<uint8_t>(std::clamp(v, 16, 235));
            }
        }
        for (uint32_t row = 0; row < h / 2; ++row) {
            for (uint32_t col = 0; col < w / 2; ++col) {
                uv[static_cast<size_t>(row) * w + 2 * col] =
                    static_cast<uint8_t>(96 + (col + static_cast<uint32_t>(k) * 3) % 64);
                uv[static_cast<size_t>(row) * w + 2 * col + 1] =
                    static_cast<uint8_t>(104 + (row + static_cast<uint32_t>(k) * 5) % 48);
            }
        }
    }
    return all;
}

/// The constant QP of picture @p n under --qp-walk: 22 to 42 and back, one
/// step a picture — the in-house controller moves it every picture too.
int walkQp(int base, int n, bool walk)
{
    if (!walk) return base;
    const int phase = n % 40;
    return 22 + (phase < 20 ? phase : 40 - phase);
}

#define VKTRY(call, what)                                                                          \
    do {                                                                                           \
        const VkResult result_ = (call);                                                           \
        if (result_ != VK_SUCCESS) {                                                               \
            say("mw-vk-lab encode: %s: %s\n", what, vkResultName(result_).c_str());                \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

bool parse(int argc, char** argv, Options& o)
{
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string& out) {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        std::string v;
        if (a == "--device") {
            if (!next(o.device)) return false;
        } else if (a == "--size") {
            if (!next(v) || std::sscanf(v.c_str(), "%ux%u", &o.width, &o.height) != 2) return false;
        } else if (a == "--align") {
            if (!next(v)) return false;
            o.align = static_cast<uint32_t>(std::stoul(v));
        } else if (a == "--fps") {
            if (!next(v)) return false;
            o.fps = std::stoi(v);
        } else if (a == "--seconds") {
            if (!next(v)) return false;
            o.seconds = std::stoi(v);
        } else if (a == "--rc") {
            if (!next(o.rc)) return false;
        } else if (a == "--qp") {
            if (!next(v)) return false;
            o.qp = std::stoi(v);
        } else if (a == "--qp-walk") {
            o.qpWalk = true;
        } else if (a == "--kbps") {
            if (!next(v)) return false;
            o.kbps = std::stoi(v);
        } else if (a == "--change") {
            o.change = true;
        } else if (a == "--quality") {
            if (!next(v)) return false;
            o.quality = std::stoi(v);
        } else if (a == "--dpb") {
            if (!next(v)) return false;
            o.dpbSlots = static_cast<uint32_t>(std::stoul(v));
        } else if (a == "--keep") {
            if (!next(v)) return false;
            o.keep = static_cast<uint32_t>(std::stoul(v));
        } else if (a == "--idr-every") {
            if (!next(v)) return false;
            o.idrEvery = std::stoi(v);
        } else if (a == "--sao") {
            o.sao = true;
        } else if (a == "--priority") {
            if (!next(o.priority)) return false;
        } else if (a == "--upload") {
            if (!next(o.upload)) return false;
        } else if (a == "--split") {
            o.split = true;
        } else if (a == "--dump") {
            if (!next(o.dump)) return false;
        } else if (a == "--dump-input") {
            if (!next(o.dumpInput)) return false;
        } else if (a == "--csv") {
            if (!next(o.csv)) return false;
        } else if (a == "--json") {
            if (!next(o.json)) return false;
        } else {
            say("unknown option %s\n", a.c_str());
            return false;
        }
    }
    const bool rcOk = o.rc == "cqp" || o.rc == "cbr" || o.rc == "vbr";
    const bool prioOk = o.priority == "default" || o.priority == "high" || o.priority == "realtime";
    const bool uploadOk = o.upload == "compute" || o.upload == "graphics";
    const bool alignOk = o.align == 8 || o.align == 16 || o.align == 32 || o.align == 64;
    if (!rcOk || !prioOk || !uploadOk || !alignOk || o.fps <= 0 || o.seconds <= 0 || o.keep < 1 ||
        o.dpbSlots < o.keep + 1 || o.width % 2 || o.height % 2)
        return false;
    return true;
}

/// One line per field that the driver's parameter sets say differently from
/// what was asked, "" when they agree.
std::string compareSets(const HevcSpsFields& asked, const HevcSpsFields& sps,
                        const HevcPpsFields& askedPps, const HevcPpsFields& pps)
{
    std::string out;
    auto field = [&](const char* name, long long a, long long d) {
        if (a == d) return;
        out += std::string("    ") + name + ": asked " + std::to_string(a) + ", driver " +
               std::to_string(d) + "\n";
    };
    field("pic_width_in_luma_samples", asked.width, sps.width);
    field("pic_height_in_luma_samples", asked.height, sps.height);
    field("conf_win_right_offset", asked.cropRight, sps.cropRight);
    field("conf_win_bottom_offset", asked.cropBottom, sps.cropBottom);
    field("log2 min coding block", asked.log2MinCodingBlock, sps.log2MinCodingBlock);
    field("log2 coding tree block", asked.log2CodingTreeBlock, sps.log2CodingTreeBlock);
    field("amp_enabled_flag", asked.asymmetricMotionPartitions, sps.asymmetricMotionPartitions);
    field("sample_adaptive_offset_enabled_flag", asked.sampleAdaptiveOffset,
          sps.sampleAdaptiveOffset);
    field("long_term_ref_pics_present_flag", asked.longTermReferences, sps.longTermReferences);
    field("sps_temporal_mvp_enabled_flag", asked.temporalMvp, sps.temporalMvp);
    field("sps_max_dec_pic_buffering_minus1", asked.maxDecPicBufferingMinus1,
          sps.maxDecPicBufferingMinus1);
    field("log2_max_pic_order_cnt_lsb", asked.log2MaxPocLsb, sps.log2MaxPocLsb);
    field("cabac_init_present_flag", askedPps.cabacInitPresent, pps.cabacInitPresent);
    field("num_ref_idx_l0_default_active", askedPps.defaultActiveL0, pps.defaultActiveL0);
    field("num_ref_idx_l1_default_active", askedPps.defaultActiveL1, pps.defaultActiveL1);
    field("init_qp", askedPps.initQp, pps.initQp);
    field("cu_qp_delta_enabled_flag", askedPps.cuQpDelta, pps.cuQpDelta);
    field("pps_cb_qp_offset", askedPps.cbQpOffset, pps.cbQpOffset);
    field("pps_cr_qp_offset", askedPps.crQpOffset, pps.crQpOffset);
    field("pps_slice_chroma_qp_offsets_present_flag", askedPps.sliceChromaQpOffsets,
          pps.sliceChromaQpOffsets);
    field("entropy_coding_sync_enabled_flag", askedPps.entropyCodingSync, pps.entropyCodingSync);
    field("pps_loop_filter_across_slices_enabled_flag", askedPps.loopFilterAcrossSlices,
          pps.loopFilterAcrossSlices);
    field("deblocking_filter_override_enabled_flag", askedPps.deblockingOverride,
          pps.deblockingOverride);
    field("pps_deblocking_filter_disabled_flag", askedPps.deblockingDisabled,
          pps.deblockingDisabled);
    field("lists_modification_present_flag", askedPps.listsModification, pps.listsModification);
    field("dependent_slice_segments_enabled_flag", askedPps.dependentSliceSegments,
          pps.dependentSliceSegments);
    return out;
}

std::string bytesHex(const uint8_t* data, size_t size, size_t limit)
{
    std::string out;
    char buf[4];
    for (size_t i = 0; i < size && i < limit; ++i) {
        std::snprintf(buf, sizeof(buf), "%02x", data[i]);
        if (!out.empty()) out += ' ';
        out += buf;
    }
    if (size > limit) out += " …";
    return out;
}

/// The rate control state: what vkCmdControlVideoCodingKHR set, which every
/// vkCmdBeginVideoCodingKHR after it must repeat. Built in place.
struct RateControl
{
    VkVideoEncodeH265RateControlLayerInfoKHR h265Layer = {};
    VkVideoEncodeRateControlLayerInfoKHR layer = {};
    VkVideoEncodeH265RateControlInfoKHR h265 = {};
    VkVideoEncodeRateControlInfoKHR info = {};

    RateControl() = default;
    RateControl(const RateControl&) = delete;
    RateControl& operator=(const RateControl&) = delete;

    void set(const std::string& mode, int kbps, int fps)
    {
        h265 = {};
        h265.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_RATE_CONTROL_INFO_KHR;
        // No GOP (one IDR, then P forever), one layer. A reference pattern
        // (flat) may only be declared with a regular GOP (VUID 08292): none.
        h265.flags = 0;
        h265.gopFrameCount = 0;
        h265.idrPeriod = 0;
        h265.consecutiveBFrameCount = 0;
        h265.subLayerCount = 1;
        info = {};
        info.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR;
        info.pNext = &h265;
        if (mode == "cqp") {
            info.rateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR;
            return;
        }
        h265Layer = {};
        h265Layer.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_RATE_CONTROL_LAYER_INFO_KHR;
        // The engine's QP range: 18 (the floor every encoder here keeps) to 51.
        h265Layer.useMinQp = VK_TRUE;
        h265Layer.minQp = {18, 18, 18};
        h265Layer.useMaxQp = VK_TRUE;
        h265Layer.maxQp = {51, 51, 51};
        layer = {};
        layer.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_LAYER_INFO_KHR;
        layer.pNext = &h265Layer;
        layer.averageBitrate = static_cast<uint64_t>(kbps) * 1000;
        layer.maxBitrate = mode == "cbr" ? layer.averageBitrate : layer.averageBitrate * 3 / 2;
        layer.frameRateNumerator = static_cast<uint32_t>(fps);
        layer.frameRateDenominator = 1;
        info.rateControlMode = mode == "cbr" ? VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR
                                             : VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR;
        info.layerCount = 1;
        info.pLayers = &layer;
        // One picture's worth, the engine's VBV (RateControl.h).
        info.virtualBufferSizeInMs = static_cast<uint32_t>((1000 + fps - 1) / fps);
        info.initialVirtualBufferSizeInMs = info.virtualBufferSizeInMs;
    }
};

} // namespace

void encodeUsage()
{
    say("mw-vk-lab encode [--device <index|name>] [--size 1920x1080] [--align 8|16|32|64]\n"
        "                 [--fps 60] [--seconds 10] [--rc cqp|cbr|vbr] [--qp 30] [--qp-walk]\n"
        "                 [--kbps 20000] [--change] [--quality 0] [--dpb 3] [--keep 1|2]\n"
        "                 [--idr-every N] [--sao]\n"
        "                 [--priority default|high|realtime] [--upload compute|graphics]\n"
        "                 [--split] [--dump x.hevc] [--dump-input x.nv12] [--csv x.csv]\n"
        "                 [--json x.json]\n"
        "  Vulkan Video HEVC Main: one IDR, then P pictures predicting from the previous one,\n"
        "  each copied into the encoder's input on another queue first (--upload), the encode\n"
        "  waiting for it on the GPU. cqp sets the QP picture by picture (--qp-walk: 22..42),\n"
        "  read back from the slice headers; cbr/vbr use the driver's rate control with a\n"
        "  one-picture buffer, --change steps it every 2 s without a reset.\n"
        "  --align: the coded size is a multiple of it (64 = whole CTBs, the conformance\n"
        "  window crops); --keep 2 keeps an unused older reference in the RPS; --idr-every 1\n"
        "  codes every picture intra (the references ruled out); --split waits\n"
        "  for the upload too, to split the wall time; high/realtime need CAP_SYS_NICE.\n"
        "  The driver's parameter sets are compared with the ones asked for.\n");
}

int runEncode(int argc, char** argv)
{
    Options o;
    try {
        if (!parse(argc, argv, o)) {
            encodeUsage();
            return 2;
        }
    } catch (...) {
        encodeUsage();
        return 2;
    }

    Vulkan vk;
    std::string error;
    if (!vk.open(VK_API_VERSION_1_3, error)) {
        say("mw-vk-lab encode: %s\n", error.c_str());
        return 1;
    }

    // ── The device and its two queues ──
    uint32_t count = 0;
    vk.vkEnumeratePhysicalDevices(vk.instance(), &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vk.vkEnumeratePhysicalDevices(vk.instance(), &count, devices.data());
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props = {};
    std::vector<std::string> extensions;
    for (uint32_t i = 0; i < count && !pd; ++i) {
        VkPhysicalDeviceProperties2 p2 = {};
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        vk.vkGetPhysicalDeviceProperties2(devices[i], &p2);
        if (p2.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
        if (!matchesDevice(o.device, i, p2.properties.deviceName)) continue;
        std::vector<std::string> ext = deviceExtensions(vk, devices[i]);
        if (!hasExtension(ext, VK_KHR_VIDEO_ENCODE_H265_EXTENSION_NAME)) {
            say("GPU %u (%s): no %s\n", i, p2.properties.deviceName,
                VK_KHR_VIDEO_ENCODE_H265_EXTENSION_NAME);
            continue;
        }
        pd = devices[i];
        props = p2.properties;
        extensions = std::move(ext);
    }
    if (!pd) {
        say("mw-vk-lab encode: no device with a Vulkan Video HEVC encoder (RADV: "
            "RADV_PERFTEST=video_encode?)\n");
        return 1;
    }
    const bool wantPriority = o.priority != "default";
    const bool priorityExt = hasExtension(extensions, VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME) ||
                             hasExtension(extensions, VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME);
    if (wantPriority && !priorityExt) {
        say("mw-vk-lab encode: --priority needs VK_KHR_global_priority\n");
        return 1;
    }

    uint32_t familyCount = 0;
    vk.vkGetPhysicalDeviceQueueFamilyProperties2(pd, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties2> families(familyCount);
    std::vector<VkQueueFamilyVideoPropertiesKHR> videoProps(familyCount);
    for (uint32_t i = 0; i < familyCount; ++i) {
        videoProps[i] = {};
        videoProps[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
        families[i] = {};
        families[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
        families[i].pNext = &videoProps[i];
    }
    vk.vkGetPhysicalDeviceQueueFamilyProperties2(pd, &familyCount, families.data());
    uint32_t encodeFamily = UINT32_MAX, uploadFamily = UINT32_MAX;
    for (uint32_t i = 0; i < familyCount; ++i) {
        const VkQueueFlags f = families[i].queueFamilyProperties.queueFlags;
        if (encodeFamily == UINT32_MAX && (f & VK_QUEUE_VIDEO_ENCODE_BIT_KHR) &&
            (videoProps[i].videoCodecOperations & VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR))
            encodeFamily = i;
        const bool compute = (f & VK_QUEUE_COMPUTE_BIT) && !(f & VK_QUEUE_GRAPHICS_BIT);
        const bool graphics = (f & VK_QUEUE_GRAPHICS_BIT) != 0;
        if (uploadFamily == UINT32_MAX &&
            ((o.upload == "compute" && compute) || (o.upload == "graphics" && graphics)))
            uploadFamily = i;
    }
    if (encodeFamily == UINT32_MAX || uploadFamily == UINT32_MAX) {
        say("mw-vk-lab encode: no %s queue family\n",
            encodeFamily == UINT32_MAX ? "HEVC encode" : o.upload.c_str());
        return 1;
    }
    const uint32_t uploadTimestampBits =
        families[uploadFamily].queueFamilyProperties.timestampValidBits;

    // ── What the encoder takes ──
    const EncodeProfileChain chain(VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR,
                                   STD_VIDEO_H265_PROFILE_IDC_MAIN,
                                   VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR);
    VkVideoEncodeH265CapabilitiesKHR c265 = {};
    c265.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_CAPABILITIES_KHR;
    VkVideoEncodeCapabilitiesKHR enc = {};
    enc.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR;
    enc.pNext = &c265;
    VkVideoCapabilitiesKHR caps = {};
    caps.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
    caps.pNext = &enc;
    if (!vk.vkGetPhysicalDeviceVideoCapabilitiesKHR) {
        say("mw-vk-lab encode: the loader has no vkGetPhysicalDeviceVideoCapabilitiesKHR\n");
        return 1;
    }
    VKTRY(vk.vkGetPhysicalDeviceVideoCapabilitiesKHR(pd, &chain.info, &caps),
          "vkGetPhysicalDeviceVideoCapabilitiesKHR");
    const VkVideoEncodeRateControlModeFlagBitsKHR wantedMode =
        o.rc == "cqp"   ? VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR
        : o.rc == "cbr" ? VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR
                        : VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR;
    if (!(enc.rateControlModes & wantedMode)) {
        say("mw-vk-lab encode: the driver has no %s rate control\n", o.rc.c_str());
        return 1;
    }
    if (o.quality < 0 || static_cast<uint32_t>(o.quality) >= enc.maxQualityLevels) {
        say("mw-vk-lab encode: quality level %d, the driver has %u\n", o.quality,
            enc.maxQualityLevels);
        return 1;
    }
    // The CTB: the largest the driver takes, as the engine would pick it.
    const uint32_t ctb = (c265.ctbSizes & VK_VIDEO_ENCODE_H265_CTB_SIZE_64_BIT_KHR)   ? 64
                         : (c265.ctbSizes & VK_VIDEO_ENCODE_H265_CTB_SIZE_32_BIT_KHR) ? 32
                                                                                      : 16;
    const uint32_t log2Ctb = ctb == 64 ? 6 : ctb == 32 ? 5 : 4;
    const uint32_t codedW = alignUp(o.width, o.align);
    const uint32_t codedH = alignUp(o.height, o.align);
    // What the images are made at: the coded size, up to the driver's access
    // granularity (it may read and write the padding).
    const uint32_t allocW = alignUp(codedW, std::max(caps.pictureAccessGranularity.width, 1u));
    const uint32_t allocH = alignUp(codedH, std::max(caps.pictureAccessGranularity.height, 1u));
    if (codedW > caps.maxCodedExtent.width || codedH > caps.maxCodedExtent.height ||
        codedW < caps.minCodedExtent.width || codedH < caps.minCodedExtent.height) {
        say("mw-vk-lab encode: coded %ux%u is outside %ux%u..%ux%u\n", codedW, codedH,
            caps.minCodedExtent.width, caps.minCodedExtent.height, caps.maxCodedExtent.width,
            caps.maxCodedExtent.height);
        return 1;
    }
    if (o.dpbSlots > caps.maxDpbSlots) {
        say("mw-vk-lab encode: --dpb %u, the driver has %u slots\n", o.dpbSlots, caps.maxDpbSlots);
        return 1;
    }

    // ── The device ──
    DeviceObjects own;
    own.vk = &vk;
    void* mapped[2] = {nullptr, nullptr}; // the staging buffer, the bitstream
    std::vector<const char*> enable = {VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
                                       VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME,
                                       VK_KHR_VIDEO_ENCODE_H265_EXTENSION_NAME};
    if (wantPriority)
        enable.push_back(hasExtension(extensions, VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME)
                             ? VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME
                             : VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME);
    VkDeviceQueueGlobalPriorityCreateInfo prio = {};
    prio.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO;
    prio.globalPriority = o.priority == "realtime" ? VK_QUEUE_GLOBAL_PRIORITY_REALTIME
                                                   : VK_QUEUE_GLOBAL_PRIORITY_HIGH;
    const float one = 1.0f;
    VkDeviceQueueCreateInfo queues[2] = {};
    for (int q = 0; q < 2; ++q) {
        queues[q].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queues[q].pNext = wantPriority ? &prio : nullptr;
        queues[q].queueFamilyIndex = q == 0 ? encodeFamily : uploadFamily;
        queues[q].queueCount = 1;
        queues[q].pQueuePriorities = &one;
    }
    VkPhysicalDeviceVulkan13Features f13 = {};
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12 = {};
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    f12.pNext = &f13;
    f12.timelineSemaphore = VK_TRUE;
    VkDeviceCreateInfo dci = {};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &f12;
    dci.queueCreateInfoCount = encodeFamily == uploadFamily ? 1 : 2;
    dci.pQueueCreateInfos = queues;
    dci.enabledExtensionCount = static_cast<uint32_t>(enable.size());
    dci.ppEnabledExtensionNames = enable.data();
    VKTRY(vk.vkCreateDevice(pd, &dci, nullptr, &own.device), "vkCreateDevice");
    std::string missing;
    if (!own.fn.load(vk, own.device, true, missing)) {
        say("mw-vk-lab encode: the device has no %s\n", missing.c_str());
        return 1;
    }
    DeviceFunctions& fn = own.fn;
    VkDevice device = own.device;
    VkQueue encodeQueue = VK_NULL_HANDLE, uploadQueue = VK_NULL_HANDLE;
    fn.vkGetDeviceQueue(device, encodeFamily, 0, &encodeQueue);
    fn.vkGetDeviceQueue(device, uploadFamily, 0, &uploadQueue);
    VkPhysicalDeviceMemoryProperties mem = {};
    vk.vkGetPhysicalDeviceMemoryProperties(pd, &mem);

    VkVideoProfileListInfoKHR profileList = {};
    profileList.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
    profileList.profileCount = 1;
    profileList.pProfiles = &chain.info;

    // ── Pictures: the encoder's input, and its DPB (one image, a layer a slot) ──
    const uint32_t families2[2] = {uploadFamily, encodeFamily};
    VkImageCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext = &profileList;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    ici.extent = {allocW, allocH, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.sharingMode =
        encodeFamily == uploadFamily ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT;
    ici.queueFamilyIndexCount = encodeFamily == uploadFamily ? 0 : 2;
    ici.pQueueFamilyIndices = families2;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage input = VK_NULL_HANDLE;
    VKTRY(fn.vkCreateImage(device, &ici, nullptr, &input), "input picture");
    own.images.push_back(input);
    ici.usage = VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR;
    ici.arrayLayers = o.dpbSlots;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.queueFamilyIndexCount = 0;
    ici.pQueueFamilyIndices = nullptr;
    VkImage dpb = VK_NULL_HANDLE;
    VKTRY(fn.vkCreateImage(device, &ici, nullptr, &dpb), "DPB");
    own.images.push_back(dpb);
    for (VkImage image : {input, dpb}) {
        VkMemoryRequirements req = {};
        fn.vkGetImageMemoryRequirements(device, image, &req);
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VKTRY(own.allocate(mem, req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, memory),
              "picture memory");
        VKTRY(fn.vkBindImageMemory(device, image, memory, 0), "vkBindImageMemory");
    }
    VkImageView inputView = VK_NULL_HANDLE, dpbView = VK_NULL_HANDLE;
    {
        VkImageViewUsageCreateInfo usage = {};
        usage.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
        usage.usage = VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR;
        VkImageViewCreateInfo vci = {};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.pNext = &usage;
        vci.image = input;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VKTRY(fn.vkCreateImageView(device, &vci, nullptr, &inputView), "input view");
        own.views.push_back(inputView);
        usage.usage = VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR;
        vci.image = dpb;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, o.dpbSlots};
        VKTRY(fn.vkCreateImageView(device, &vci, nullptr, &dpbView), "DPB view");
        own.views.push_back(dpbView);
    }

    // ── Buffers: the pictures to upload, and the bitstream ──
    const size_t luma = static_cast<size_t>(codedW) * codedH;
    const size_t pictureBytes = luma + luma / 2;
    constexpr int kPictures = 8;
    const std::vector<uint8_t> pictures = makePictures(codedW, codedH, kPictures);
    const VkDeviceSize bitstreamSize = 8u << 20;
    VkBuffer staging = VK_NULL_HANDLE, bitstream = VK_NULL_HANDLE;
    {
        VkBufferCreateInfo bci = {};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = pictures.size();
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VKTRY(fn.vkCreateBuffer(device, &bci, nullptr, &staging), "staging buffer");
        own.buffers.push_back(staging);
        bci.pNext = &profileList;
        bci.size = bitstreamSize;
        bci.usage = VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR;
        VKTRY(fn.vkCreateBuffer(device, &bci, nullptr, &bitstream), "bitstream buffer");
        own.buffers.push_back(bitstream);
        const VkMemoryPropertyFlags visible =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        int index = 0;
        for (VkBuffer buffer : {staging, bitstream}) {
            VkMemoryRequirements req = {};
            fn.vkGetBufferMemoryRequirements(device, buffer, &req);
            VkDeviceMemory memory = VK_NULL_HANDLE;
            // The bitstream is read by the CPU: cached memory if there is some.
            VKTRY(own.allocate(mem, req,
                               index ? visible | VK_MEMORY_PROPERTY_HOST_CACHED_BIT : visible,
                               visible, memory),
                  "buffer memory");
            VKTRY(fn.vkBindBufferMemory(device, buffer, memory, 0), "vkBindBufferMemory");
            VKTRY(fn.vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped[index]),
                  "vkMapMemory");
            ++index;
        }
        std::memcpy(mapped[0], pictures.data(), pictures.size());
    }
    if (!o.dumpInput.empty()) {
        std::ofstream out(o.dumpInput, std::ios::binary);
        out.write(reinterpret_cast<const char*>(pictures.data()),
                  static_cast<std::streamsize>(pictures.size()));
    }

    // ── The video session ──
    VkVideoSessionCreateInfoKHR sci = {};
    sci.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR;
    sci.queueFamilyIndex = encodeFamily;
    sci.pVideoProfile = &chain.info;
    sci.pictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    sci.maxCodedExtent = {codedW, codedH};
    sci.referencePictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    sci.maxDpbSlots = o.dpbSlots;
    sci.maxActiveReferencePictures = 1;
    sci.pStdHeaderVersion = &caps.stdHeaderVersion;
    VKTRY(fn.vkCreateVideoSessionKHR(device, &sci, nullptr, &own.session), "video session");
    {
        uint32_t n = 0;
        fn.vkGetVideoSessionMemoryRequirementsKHR(device, own.session, &n, nullptr);
        std::vector<VkVideoSessionMemoryRequirementsKHR> reqs(n);
        for (auto& r : reqs) {
            r = {};
            r.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR;
        }
        fn.vkGetVideoSessionMemoryRequirementsKHR(device, own.session, &n, reqs.data());
        std::vector<VkBindVideoSessionMemoryInfoKHR> binds(n);
        for (uint32_t i = 0; i < n; ++i) {
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VKTRY(own.allocate(mem, reqs[i].memoryRequirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                               0, memory),
                  "session memory");
            binds[i] = {};
            binds[i].sType = VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR;
            binds[i].memoryBindIndex = reqs[i].memoryBindIndex;
            binds[i].memory = memory;
            binds[i].memoryOffset = 0;
            binds[i].memorySize = reqs[i].memoryRequirements.size;
        }
        VKTRY(fn.vkBindVideoSessionMemoryKHR(device, own.session, n, binds.data()),
              "vkBindVideoSessionMemoryKHR");
    }

    // ── Our parameter sets ──
    StdVideoH265ProfileTierLevel ptl = {};
    ptl.flags.general_progressive_source_flag = 1;
    ptl.flags.general_frame_only_constraint_flag = 1;
    ptl.general_profile_idc = STD_VIDEO_H265_PROFILE_IDC_MAIN;
    ptl.general_level_idc = STD_VIDEO_H265_LEVEL_IDC_5_1;
    StdVideoH265DecPicBufMgr dpbMgr = {};
    dpbMgr.max_dec_pic_buffering_minus1[0] = static_cast<uint8_t>(o.keep);
    StdVideoH265VideoParameterSet vps = {};
    vps.flags.vps_temporal_id_nesting_flag = 1;
    vps.flags.vps_sub_layer_ordering_info_present_flag = 1;
    vps.pDecPicBufMgr = &dpbMgr;
    vps.pProfileTierLevel = &ptl;
    StdVideoH265SequenceParameterSetVui vui = {};
    vui.flags.video_signal_type_present_flag = 1;
    vui.flags.colour_description_present_flag = 1;
    vui.video_format = 5;
    vui.colour_primaries = 1;
    vui.transfer_characteristics = 1;
    vui.matrix_coeffs = 1;
    StdVideoH265SequenceParameterSet sps = {};
    sps.flags.sps_temporal_id_nesting_flag = 1;
    sps.flags.sps_sub_layer_ordering_info_present_flag = 1;
    sps.flags.conformance_window_flag = (codedW != o.width || codedH != o.height) ? 1 : 0;
    sps.flags.sample_adaptive_offset_enabled_flag = o.sao ? 1 : 0;
    sps.flags.vui_parameters_present_flag = 1;
    sps.chroma_format_idc = STD_VIDEO_H265_CHROMA_FORMAT_IDC_420;
    sps.pic_width_in_luma_samples = codedW;
    sps.pic_height_in_luma_samples = codedH;
    sps.log2_max_pic_order_cnt_lsb_minus4 = 12;
    sps.log2_min_luma_coding_block_size_minus3 = 0;
    sps.log2_diff_max_min_luma_coding_block_size = static_cast<uint8_t>(log2Ctb - 3);
    sps.log2_min_luma_transform_block_size_minus2 = 0;
    sps.log2_diff_max_min_luma_transform_block_size = 3;
    sps.max_transform_hierarchy_depth_inter = 2;
    sps.max_transform_hierarchy_depth_intra = 2;
    sps.conf_win_right_offset = (codedW - o.width) / 2;
    sps.conf_win_bottom_offset = (codedH - o.height) / 2;
    sps.pProfileTierLevel = &ptl;
    sps.pDecPicBufMgr = &dpbMgr;
    sps.pSequenceParameterSetVui = &vui;
    StdVideoH265PictureParameterSet pps = {};
    pps.flags.cu_qp_delta_enabled_flag = 1;
    pps.num_ref_idx_l0_default_active_minus1 = 0;
    pps.num_ref_idx_l1_default_active_minus1 = 0;
    // What was asked, as the parser would read it, to compare with the driver.
    HevcSpsFields askedSps;
    askedSps.width = codedW;
    askedSps.height = codedH;
    askedSps.cropRight = sps.conf_win_right_offset;
    askedSps.cropBottom = sps.conf_win_bottom_offset;
    askedSps.log2MaxPocLsb = 16;
    askedSps.maxDecPicBufferingMinus1 = o.keep;
    askedSps.log2MinCodingBlock = 3;
    askedSps.log2CodingTreeBlock = static_cast<int>(log2Ctb);
    askedSps.sampleAdaptiveOffset = o.sao;
    HevcPpsFields askedPps;
    askedPps.defaultActiveL0 = 1;
    askedPps.defaultActiveL1 = 1;
    askedPps.initQp = 26;
    askedPps.cuQpDelta = true;

    {
        VkVideoEncodeH265SessionParametersAddInfoKHR add = {};
        add.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_ADD_INFO_KHR;
        add.stdVPSCount = 1;
        add.pStdVPSs = &vps;
        add.stdSPSCount = 1;
        add.pStdSPSs = &sps;
        add.stdPPSCount = 1;
        add.pStdPPSs = &pps;
        VkVideoEncodeQualityLevelInfoKHR quality = {};
        quality.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUALITY_LEVEL_INFO_KHR;
        quality.qualityLevel = static_cast<uint32_t>(o.quality);
        VkVideoEncodeH265SessionParametersCreateInfoKHR h265p = {};
        h265p.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_CREATE_INFO_KHR;
        h265p.pNext = &quality;
        h265p.maxStdVPSCount = 1;
        h265p.maxStdSPSCount = 1;
        h265p.maxStdPPSCount = 1;
        h265p.pParametersAddInfo = &add;
        VkVideoSessionParametersCreateInfoKHR pci = {};
        pci.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR;
        pci.pNext = &h265p;
        pci.videoSession = own.session;
        VKTRY(fn.vkCreateVideoSessionParametersKHR(device, &pci, nullptr, &own.parameters),
              "session parameters");
    }

    // ── The parameter sets the driver stands by ──
    std::vector<uint8_t> headers;
    bool overrides = false, vpsOverride = false, spsOverride = false, ppsOverride = false;
    {
        VkVideoEncodeH265SessionParametersGetInfoKHR g265 = {};
        g265.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_GET_INFO_KHR;
        g265.writeStdVPS = VK_TRUE;
        g265.writeStdSPS = VK_TRUE;
        g265.writeStdPPS = VK_TRUE;
        VkVideoEncodeSessionParametersGetInfoKHR get = {};
        get.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_GET_INFO_KHR;
        get.pNext = &g265;
        get.videoSessionParameters = own.parameters;
        VkVideoEncodeH265SessionParametersFeedbackInfoKHR f265 = {};
        f265.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_FEEDBACK_INFO_KHR;
        VkVideoEncodeSessionParametersFeedbackInfoKHR feedback = {};
        feedback.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_FEEDBACK_INFO_KHR;
        feedback.pNext = &f265;
        size_t size = 0;
        VKTRY(fn.vkGetEncodedVideoSessionParametersKHR(device, &get, &feedback, &size, nullptr),
              "vkGetEncodedVideoSessionParametersKHR (size)");
        headers.resize(size);
        VKTRY(fn.vkGetEncodedVideoSessionParametersKHR(device, &get, &feedback, &size,
                                                       headers.data()),
              "vkGetEncodedVideoSessionParametersKHR");
        headers.resize(size);
        overrides = feedback.hasOverrides == VK_TRUE;
        vpsOverride = f265.hasStdVPSOverrides == VK_TRUE;
        spsOverride = f265.hasStdSPSOverrides == VK_TRUE;
        ppsOverride = f265.hasStdPPSOverrides == VK_TRUE;
    }
    HevcSpsFields driverSps;
    HevcPpsFields driverPps;
    std::string setsError;
    for (const HevcNalUnit& u : mw::native::encode::hevcNalUnits(headers.data(), headers.size())) {
        if (u.type() == 33)
            setsError += mw::native::encode::parseHevcSps(u.data, u.size, driverSps);
        if (u.type() == 34)
            setsError += mw::native::encode::parseHevcPps(u.data, u.size, driverPps);
    }
    const std::string differences = compareSets(askedSps, driverSps, askedPps, driverPps);

    // ── Queries, command buffers, semaphores ──
    VkQueryPool feedbackPool = VK_NULL_HANDLE, timestampPool = VK_NULL_HANDLE;
    {
        VkQueryPoolVideoEncodeFeedbackCreateInfoKHR fci = {};
        fci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_VIDEO_ENCODE_FEEDBACK_CREATE_INFO_KHR;
        fci.pNext = &chain.info;
        fci.encodeFeedbackFlags = VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BUFFER_OFFSET_BIT_KHR |
                                  VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BYTES_WRITTEN_BIT_KHR;
        VkQueryPoolCreateInfo qci = {};
        qci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qci.pNext = &fci;
        qci.queryType = VK_QUERY_TYPE_VIDEO_ENCODE_FEEDBACK_KHR;
        qci.queryCount = 1;
        VKTRY(fn.vkCreateQueryPool(device, &qci, nullptr, &feedbackPool), "feedback query pool");
        own.queryPools.push_back(feedbackPool);
        if (uploadTimestampBits) {
            qci.pNext = nullptr;
            qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qci.queryCount = 2;
            VKTRY(fn.vkCreateQueryPool(device, &qci, nullptr, &timestampPool),
                  "timestamp query pool");
            own.queryPools.push_back(timestampPool);
        }
    }
    VkCommandBuffer encodeCmd = VK_NULL_HANDLE, uploadCmd = VK_NULL_HANDLE;
    for (int q = 0; q < 2; ++q) {
        VkCommandPoolCreateInfo pci = {};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = q == 0 ? encodeFamily : uploadFamily;
        VkCommandPool pool = VK_NULL_HANDLE;
        VKTRY(fn.vkCreateCommandPool(device, &pci, nullptr, &pool), "command pool");
        own.pools.push_back(pool);
        VkCommandBufferAllocateInfo cai = {};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VKTRY(fn.vkAllocateCommandBuffers(device, &cai, q == 0 ? &encodeCmd : &uploadCmd),
              "command buffer");
    }
    VkSemaphore uploaded = VK_NULL_HANDLE, encoded = VK_NULL_HANDLE;
    for (VkSemaphore* s : {&uploaded, &encoded}) {
        VkSemaphoreTypeCreateInfo kind = {};
        kind.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        kind.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo sci2 = {};
        sci2.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        sci2.pNext = &kind;
        VKTRY(fn.vkCreateSemaphore(device, &sci2, nullptr, s), "timeline semaphore");
        own.semaphores.push_back(*s);
    }

    say("mw-vk-lab encode — %s, %s\n", hostName().c_str(), nowText().c_str());
    say("  %s (%s), encode family %u, upload family %u (%s, timestamps %u bits), priority %s\n",
        props.deviceName, vkVersionText(props.apiVersion).c_str(), encodeFamily, uploadFamily,
        o.upload.c_str(), uploadTimestampBits, o.priority.c_str());
    say("  HEVC Main %ux%u, coded %ux%u (align %u, CTB %u), images %ux%u, %d i/s, %d s, %s",
        o.width, o.height, codedW, codedH, o.align, ctb, allocW, allocH, o.fps, o.seconds,
        o.rc.c_str());
    if (o.rc == "cqp" && o.qpWalk)
        say(" QP 22..42 walked\n");
    else if (o.rc == "cqp")
        say(" QP %d\n", o.qp);
    else
        say(" %d kb/s%s\n", o.kbps, o.change ? ", stepped every 2 s" : "");
    say("  DPB %u slots, %u reference(s) kept, quality level %d, SAO %s\n", o.dpbSlots, o.keep,
        o.quality, o.sao ? "on" : "off");
    say("  driver's parameter sets: %zu bytes, overrides %s (VPS %s, SPS %s, PPS %s)%s%s\n",
        headers.size(), overrides ? "yes" : "no", vpsOverride ? "yes" : "no",
        spsOverride ? "yes" : "no", ppsOverride ? "yes" : "no",
        setsError.empty() ? "" : ", parse: ", setsError.c_str());
    say("    %s\n", bytesHex(headers.data(), headers.size(), 48).c_str());
    if (differences.empty())
        say("  the driver's SPS/PPS say what was asked\n");
    else
        say("  the driver's SPS/PPS differ from what was asked:\n%s", differences.c_str());

    // ── The loop ──
    std::ofstream dump;
    if (!o.dump.empty()) {
        dump.open(o.dump, std::ios::binary);
        dump.write(reinterpret_cast<const char*>(headers.data()),
                   static_cast<std::streamsize>(headers.size()));
    }
    std::ofstream csv;
    if (!o.csv.empty()) {
        csv.open(o.csv);
        csv << "frame,type,qp_asked,qp_coded,bytes,wall_us,upload_wait_us,upload_gpu_us,kbps,"
               "rps_kept,rps_used\n";
    }

    RateControl rc[2];
    int current = 0;
    rc[0].set(o.rc, o.kbps, o.fps);
    const int frames = o.fps * o.seconds;
    std::vector<double> wall, uploadGpu, uploadWait;
    std::vector<int32_t> slotPoc(o.dpbSlots, -1);
    uint64_t idrBytes = 0;
    std::vector<double> pBytes;
    int qpAsked = 0, qpFollowed = 0, qpRead = 0, rpsRight = 0, rpsRead = 0;
    int sliceErrors = 0;
    std::string firstSliceError, firstSliceBytes;
    const int64_t start = nowUs() + 20000;
    const double period = static_cast<double>(props.limits.timestampPeriod);
    int kbpsNow = o.kbps;
    int lastIdr = 0;

    for (int n = 0; n < frames; ++n) {
        sleepUntilUs(start + static_cast<int64_t>(n) * 1000000 / o.fps);
        const bool first = n == 0;
        const bool idr = first || (o.idrEvery > 0 && n % o.idrEvery == 0);
        if (idr) lastIdr = n;
        const int32_t poc = n - lastIdr;
        const uint32_t setup = static_cast<uint32_t>(n) % o.dpbSlots;
        const int qp = walkQp(o.qp, n, o.qpWalk);
        // A rate step every two seconds: the target halves, then comes back.
        const bool step = o.change && o.rc != "cqp" && n > 0 && n % (2 * o.fps) == 0;
        if (step) {
            kbpsNow = kbpsNow == o.kbps ? o.kbps / 2 : o.kbps;
            rc[1 - current].set(o.rc, kbpsNow, o.fps);
        }

        // The upload: picture n % 8 into the encoder's input.
        fn.vkResetCommandBuffer(uploadCmd, 0);
        VkCommandBufferBeginInfo cbi = {};
        cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        fn.vkBeginCommandBuffer(uploadCmd, &cbi);
        if (timestampPool) {
            fn.vkCmdResetQueryPool(uploadCmd, timestampPool, 0, 2);
            fn.vkCmdWriteTimestamp2(uploadCmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, timestampPool,
                                    0);
        }
        VkImageMemoryBarrier2 toCopy = {};
        toCopy.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        toCopy.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        toCopy.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        toCopy.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toCopy.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toCopy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toCopy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toCopy.image = input;
        toCopy.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep = {};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &toCopy;
        fn.vkCmdPipelineBarrier2(uploadCmd, &dep);
        const VkDeviceSize base = pictureBytes * static_cast<VkDeviceSize>(n % kPictures);
        VkBufferImageCopy regions[2] = {};
        regions[0].bufferOffset = base;
        regions[0].imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
        regions[0].imageExtent = {codedW, codedH, 1};
        regions[1].bufferOffset = base + luma;
        regions[1].imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1};
        regions[1].imageExtent = {codedW / 2, codedH / 2, 1};
        fn.vkCmdCopyBufferToImage(uploadCmd, staging, input, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  2, regions);
        VkImageMemoryBarrier2 toEncode = toCopy;
        toEncode.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        toEncode.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toEncode.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        toEncode.dstAccessMask = VK_ACCESS_2_NONE;
        toEncode.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toEncode.newLayout = VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR;
        dep.pImageMemoryBarriers = &toEncode;
        fn.vkCmdPipelineBarrier2(uploadCmd, &dep);
        if (timestampPool)
            fn.vkCmdWriteTimestamp2(uploadCmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, timestampPool,
                                    1);
        fn.vkEndCommandBuffer(uploadCmd);

        // The encode.
        fn.vkResetCommandBuffer(encodeCmd, 0);
        fn.vkBeginCommandBuffer(encodeCmd, &cbi);
        fn.vkCmdResetQueryPool(encodeCmd, feedbackPool, 0, 1);
        VkImageMemoryBarrier2 dpbBarrier = {};
        dpbBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        dpbBarrier.srcStageMask =
            first ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
        dpbBarrier.srcAccessMask =
            first ? VK_ACCESS_2_NONE : VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR;
        dpbBarrier.dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
        dpbBarrier.dstAccessMask =
            VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR;
        dpbBarrier.oldLayout =
            first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR;
        dpbBarrier.newLayout = VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR;
        dpbBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        dpbBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        dpbBarrier.image = dpb;
        dpbBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, o.dpbSlots};
        dep.pImageMemoryBarriers = &dpbBarrier;
        fn.vkCmdPipelineBarrier2(encodeCmd, &dep);

        // The references: the previous picture, used; with --keep 2, the one
        // before it too, kept and unused.
        std::vector<uint32_t> keptSlots;
        for (uint32_t k = 1; k <= o.keep && !idr; ++k) {
            if (n - static_cast<int>(k) < lastIdr) break;
            keptSlots.push_back(static_cast<uint32_t>(n - static_cast<int>(k)) % o.dpbSlots);
        }
        std::vector<VkVideoPictureResourceInfoKHR> resources(o.dpbSlots);
        std::vector<StdVideoEncodeH265ReferenceInfo> refStd(o.dpbSlots);
        std::vector<VkVideoEncodeH265DpbSlotInfoKHR> refDpb(o.dpbSlots);
        for (uint32_t s = 0; s < o.dpbSlots; ++s) {
            resources[s] = {};
            resources[s].sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
            resources[s].codedExtent = {codedW, codedH};
            resources[s].baseArrayLayer = s;
            resources[s].imageViewBinding = dpbView;
            refStd[s] = {};
            refStd[s].pic_type =
                slotPoc[s] == 0 ? STD_VIDEO_H265_PICTURE_TYPE_IDR : STD_VIDEO_H265_PICTURE_TYPE_P;
            refStd[s].PicOrderCntVal = slotPoc[s];
            refDpb[s] = {};
            refDpb[s].sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_DPB_SLOT_INFO_KHR;
            refDpb[s].pStdReferenceInfo = &refStd[s];
        }
        std::vector<VkVideoReferenceSlotInfoKHR> beginSlots;
        for (uint32_t s : keptSlots) {
            VkVideoReferenceSlotInfoKHR r = {};
            r.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
            r.pNext = &refDpb[s];
            r.slotIndex = static_cast<int32_t>(s);
            r.pPictureResource = &resources[s];
            beginSlots.push_back(r);
        }
        {
            VkVideoReferenceSlotInfoKHR r = {};
            r.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
            r.slotIndex = -1; // the slot this picture is reconstructed into
            r.pPictureResource = &resources[setup];
            beginSlots.push_back(r);
        }
        VkVideoBeginCodingInfoKHR begin = {};
        begin.sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;
        // The state set by the last control; none before the first.
        begin.pNext = first ? nullptr : &rc[current].info;
        begin.videoSession = own.session;
        begin.videoSessionParameters = own.parameters;
        begin.referenceSlotCount = static_cast<uint32_t>(beginSlots.size());
        begin.pReferenceSlots = beginSlots.data();
        fn.vkCmdBeginVideoCodingKHR(encodeCmd, &begin);
        if (first) {
            VkVideoEncodeQualityLevelInfoKHR quality = {};
            quality.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUALITY_LEVEL_INFO_KHR;
            quality.pNext = &rc[current].info;
            quality.qualityLevel = static_cast<uint32_t>(o.quality);
            VkVideoCodingControlInfoKHR control = {};
            control.sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR;
            control.pNext = &quality;
            control.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR |
                            VK_VIDEO_CODING_CONTROL_ENCODE_RATE_CONTROL_BIT_KHR |
                            VK_VIDEO_CODING_CONTROL_ENCODE_QUALITY_LEVEL_BIT_KHR;
            fn.vkCmdControlVideoCodingKHR(encodeCmd, &control);
        } else if (step) {
            VkVideoCodingControlInfoKHR control = {};
            control.sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR;
            control.pNext = &rc[1 - current].info;
            control.flags = VK_VIDEO_CODING_CONTROL_ENCODE_RATE_CONTROL_BIT_KHR;
            fn.vkCmdControlVideoCodingKHR(encodeCmd, &control);
            current = 1 - current;
        }

        StdVideoH265ShortTermRefPicSet rps = {};
        StdVideoEncodeH265ReferenceListsInfo lists = {};
        std::memset(lists.RefPicList0, STD_VIDEO_H265_NO_REFERENCE_PICTURE,
                    sizeof(lists.RefPicList0));
        std::memset(lists.RefPicList1, STD_VIDEO_H265_NO_REFERENCE_PICTURE,
                    sizeof(lists.RefPicList1));
        if (!idr) {
            rps.num_negative_pics = static_cast<uint8_t>(keptSlots.size());
            int32_t previous = poc;
            for (size_t i = 0; i < keptSlots.size(); ++i) {
                const int32_t refPoc = slotPoc[keptSlots[i]];
                rps.delta_poc_s0_minus1[i] = static_cast<uint16_t>(previous - refPoc - 1);
                previous = refPoc;
            }
            rps.used_by_curr_pic_s0_flag = 1; // the nearest only
            lists.num_ref_idx_l0_active_minus1 = 0;
            lists.RefPicList0[0] = static_cast<uint8_t>(keptSlots[0]);
        }
        StdVideoEncodeH265PictureInfo picture = {};
        picture.flags.is_reference = 1;
        picture.flags.IrapPicFlag = idr ? 1 : 0;
        picture.flags.pic_output_flag = 1;
        picture.pic_type = idr ? STD_VIDEO_H265_PICTURE_TYPE_IDR : STD_VIDEO_H265_PICTURE_TYPE_P;
        picture.PicOrderCntVal = poc;
        picture.pRefLists = &lists;
        picture.pShortTermRefPicSet = &rps;
        StdVideoEncodeH265SliceSegmentHeader slice = {};
        slice.flags.first_slice_segment_in_pic_flag = 1;
        slice.flags.slice_sao_luma_flag = o.sao ? 1 : 0;
        slice.flags.slice_sao_chroma_flag = o.sao ? 1 : 0;
        slice.slice_type = idr ? STD_VIDEO_H265_SLICE_TYPE_I : STD_VIDEO_H265_SLICE_TYPE_P;
        slice.MaxNumMergeCand = 5;
        VkVideoEncodeH265NaluSliceSegmentInfoKHR nalu = {};
        nalu.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_NALU_SLICE_SEGMENT_INFO_KHR;
        nalu.constantQp = o.rc == "cqp" ? qp : 0;
        nalu.pStdSliceSegmentHeader = &slice;
        VkVideoEncodeH265PictureInfoKHR h265Picture = {};
        h265Picture.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PICTURE_INFO_KHR;
        h265Picture.naluSliceSegmentEntryCount = 1;
        h265Picture.pNaluSliceSegmentEntries = &nalu;
        h265Picture.pStdPictureInfo = &picture;

        StdVideoEncodeH265ReferenceInfo setupStd = {};
        setupStd.pic_type = picture.pic_type;
        setupStd.PicOrderCntVal = poc;
        VkVideoEncodeH265DpbSlotInfoKHR setupDpb = {};
        setupDpb.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_DPB_SLOT_INFO_KHR;
        setupDpb.pStdReferenceInfo = &setupStd;
        VkVideoReferenceSlotInfoKHR setupSlot = {};
        setupSlot.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
        setupSlot.pNext = &setupDpb;
        setupSlot.slotIndex = static_cast<int32_t>(setup);
        setupSlot.pPictureResource = &resources[setup];
        VkVideoReferenceSlotInfoKHR usedSlot = {};
        if (!idr) usedSlot = beginSlots[0];

        VkVideoEncodeInfoKHR info = {};
        info.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR;
        info.pNext = &h265Picture;
        info.dstBuffer = bitstream;
        info.dstBufferOffset = 0;
        info.dstBufferRange = bitstreamSize;
        info.srcPictureResource.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
        info.srcPictureResource.codedExtent = {codedW, codedH};
        info.srcPictureResource.baseArrayLayer = 0;
        info.srcPictureResource.imageViewBinding = inputView;
        info.pSetupReferenceSlot = &setupSlot;
        info.referenceSlotCount = idr ? 0 : 1;
        info.pReferenceSlots = idr ? nullptr : &usedSlot;
        fn.vkCmdBeginQuery(encodeCmd, feedbackPool, 0, 0);
        fn.vkCmdEncodeVideoKHR(encodeCmd, &info);
        fn.vkCmdEndQuery(encodeCmd, feedbackPool, 0);
        VkVideoEndCodingInfoKHR end = {};
        end.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR;
        fn.vkCmdEndVideoCodingKHR(encodeCmd, &end);
        fn.vkEndCommandBuffer(encodeCmd);

        // Submit both, the encode waiting for the upload on the GPU.
        const uint64_t value = static_cast<uint64_t>(n) + 1;
        const int64_t t0 = nowUs();
        VkCommandBufferSubmitInfo cmdInfo = {};
        cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmdInfo.commandBuffer = uploadCmd;
        VkSemaphoreSubmitInfo signalUpload = {};
        signalUpload.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signalUpload.semaphore = uploaded;
        signalUpload.value = value;
        signalUpload.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSubmitInfo2 submit = {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmdInfo;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signalUpload;
        VKTRY(fn.vkQueueSubmit2(uploadQueue, 1, &submit, VK_NULL_HANDLE), "upload submit");
        VkSemaphoreSubmitInfo waitUpload = signalUpload;
        waitUpload.stageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
        VkSemaphoreSubmitInfo signalEncode = signalUpload;
        signalEncode.semaphore = encoded;
        cmdInfo.commandBuffer = encodeCmd;
        submit.waitSemaphoreInfoCount = 1;
        submit.pWaitSemaphoreInfos = &waitUpload;
        submit.pSignalSemaphoreInfos = &signalEncode;
        VKTRY(fn.vkQueueSubmit2(encodeQueue, 1, &submit, VK_NULL_HANDLE), "encode submit");

        VkSemaphoreWaitInfo wait = {};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wait.semaphoreCount = 1;
        double uploadWaitUs = -1;
        if (o.split) {
            wait.pSemaphores = &uploaded;
            wait.pValues = &value;
            VKTRY(fn.vkWaitSemaphores(device, &wait, 2000000000ull), "upload wait");
            uploadWaitUs = static_cast<double>(nowUs() - t0);
        }
        wait.pSemaphores = &encoded;
        wait.pValues = &value;
        VKTRY(fn.vkWaitSemaphores(device, &wait, 2000000000ull), "encode wait");
        const double wallUs = static_cast<double>(nowUs() - t0);

        struct
        {
            uint32_t offset;
            uint32_t bytes;
        } feedback = {};
        VKTRY(fn.vkGetQueryPoolResults(device, feedbackPool, 0, 1, sizeof(feedback), &feedback,
                                       sizeof(feedback), VK_QUERY_RESULT_WAIT_BIT),
              "encode feedback");
        double uploadGpuUs = -1;
        if (timestampPool) {
            uint64_t ts[2] = {};
            if (fn.vkGetQueryPoolResults(device, timestampPool, 0, 2, sizeof(ts), ts, sizeof(ts[0]),
                                         VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) ==
                VK_SUCCESS)
                uploadGpuUs = static_cast<double>(ts[1] - ts[0]) * period / 1000.0;
        }
        slotPoc[setup] = poc;

        // Read the slice header back with the driver's own SPS and PPS.
        const uint8_t* data = static_cast<const uint8_t*>(mapped[1]) + feedback.offset;
        int codedQp = -1;
        size_t kept = 0, used = 0;
        for (const HevcNalUnit& u : mw::native::encode::hevcNalUnits(data, feedback.bytes)) {
            if (u.type() > 21) continue;
            HevcSliceFields fields;
            const std::string bad = mw::native::encode::parseHevcSliceHeader(
                u.data, u.size, driverSps, driverPps, fields);
            if (!bad.empty()) {
                if (sliceErrors++ == 0) {
                    firstSliceError = bad;
                    firstSliceBytes = bytesHex(data, feedback.bytes, 24);
                }
                break;
            }
            codedQp = fields.qp;
            kept = fields.shortTerm.size();
            used = fields.referencesUsed();
            break;
        }
        if (n == 0 && firstSliceBytes.empty()) firstSliceBytes = bytesHex(data, feedback.bytes, 24);
        if (o.rc == "cqp" && codedQp >= 0) {
            ++qpRead;
            ++qpAsked;
            qpFollowed += codedQp == qp ? 1 : 0;
        }
        if (!idr && codedQp >= 0) {
            ++rpsRead;
            rpsRight += (kept == keptSlots.size() && used == 1) ? 1 : 0;
        }
        if (dump.is_open())
            dump.write(reinterpret_cast<const char*>(data),
                       static_cast<std::streamsize>(feedback.bytes));
        if (idr)
            idrBytes = feedback.bytes;
        else
            pBytes.push_back(static_cast<double>(feedback.bytes));
        // The first second fills the pipeline and the clocks: out of the stats.
        if (n >= o.fps) {
            wall.push_back(wallUs / 1000.0);
            if (uploadGpuUs >= 0) uploadGpu.push_back(uploadGpuUs / 1000.0);
            if (uploadWaitUs >= 0) uploadWait.push_back(uploadWaitUs / 1000.0);
        }
        if (csv.is_open())
            csv << n << ',' << (idr ? "IDR" : "P") << ',' << (o.rc == "cqp" ? qp : -1) << ','
                << codedQp << ',' << feedback.bytes << ',' << static_cast<long long>(wallUs) << ','
                << static_cast<long long>(uploadWaitUs) << ','
                << static_cast<long long>(uploadGpuUs) << ',' << (o.rc == "cqp" ? -1 : kbpsNow)
                << ',' << kept << ',' << used << '\n';
    }

    // ── What it says ──
    const Stats w = stats(wall), g = stats(uploadGpu), u = stats(uploadWait), b = stats(pBytes);
    say("  first picture's bitstream: %s\n", firstSliceBytes.c_str());
    say("  wall (submit to bitstream), after the first second: mean %.2f, p50 %.2f, p99 %.2f, "
        "max %.2f ms over %zu pictures\n",
        w.mean, w.p50, w.p99, w.max, wall.size());
    if (!uploadWait.empty())
        say("  of which the upload (CPU wait): mean %.2f, p99 %.2f ms\n", u.mean, u.p99);
    if (!uploadGpu.empty())
        say("  upload on the GPU (timestamps): mean %.3f, p99 %.3f ms\n", g.mean, g.p99);
    say("  bytes: IDR %llu, P mean %.0f (p99 %.0f) = %.0f kb/s at %d i/s\n",
        static_cast<unsigned long long>(idrBytes), b.mean, b.p99, b.mean * 8 * o.fps / 1000.0,
        o.fps);
    if (o.rc == "cqp") say("  QP read back: %d/%d pictures at the QP asked\n", qpFollowed, qpAsked);
    say("  RPS read back: %d/%d P pictures list %u reference(s), one used\n", rpsRight, rpsRead,
        o.keep);
    if (sliceErrors)
        say("  slice headers not read: %d (first: %s)\n", sliceErrors, firstSliceError.c_str());

    if (!o.json.empty()) {
        Json j;
        j.beginObject();
        j.field("tool", "mw-vk-lab encode");
        j.field("date", nowText());
        j.field("host", hostName());
        j.field("device", props.deviceName);
        j.field("width", o.width);
        j.field("height", o.height);
        j.field("codedWidth", codedW);
        j.field("codedHeight", codedH);
        j.field("ctb", ctb);
        j.field("fps", o.fps);
        j.field("rc", o.rc);
        j.field("qp", o.qp);
        j.field("qpWalk", o.qpWalk);
        j.field("kbps", o.kbps);
        j.field("change", o.change);
        j.field("quality", o.quality);
        j.field("priority", o.priority);
        j.field("upload", o.upload);
        j.field("keep", o.keep);
        j.field("overrides", overrides);
        j.field("differences", differences);
        j.field("wallMeanMs", w.mean);
        j.field("wallP50Ms", w.p50);
        j.field("wallP99Ms", w.p99);
        j.field("wallMaxMs", w.max);
        j.field("uploadGpuMeanMs", g.mean);
        j.field("idrBytes", static_cast<unsigned long long>(idrBytes));
        j.field("pBytesMean", b.mean);
        j.field("qpFollowed", qpFollowed);
        j.field("qpAsked", qpAsked);
        j.field("rpsRight", rpsRight);
        j.field("rpsRead", rpsRead);
        j.field("sliceErrors", sliceErrors);
        j.endObject();
        std::ofstream out(o.json);
        out << j.str() << '\n';
    }
    return 0;
}

} // namespace lab
