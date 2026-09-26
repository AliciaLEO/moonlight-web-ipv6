/*
 * MoonlightWeb — native capture & encoding engine: D3D12 lab.
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

// `caps` — what every GPU of this machine answers, before anything is built on
// it (plan pipeline-video-d3d12-v2, C0.2). The first version of this probe
// (21/09) listed the D3D12 Video Encode capabilities; this one adds what the
// second attempt depends on:
//
//  - the rate control the in-house controller needs: CQP with a delta-QP map,
//    ABSOLUTE_QP_MAP, CBR with a QP range and a frame-size cap, and the size of
//    a QP-map region;
//  - the handshake the pipeline makes on every frame: a shared D3D11 fence
//    opened in D3D12 and the other way round, timed at rest;
//  - the queues: every type at every priority, created plainly and with a
//    CreatorID of our own — under HAGS that is what takes a queue out of the
//    default group whose priority is ignored;
//  - whether each queue type can be timestamped, and its clock;
//  - the driver and OS build, without which none of it can be compared later.
//
// Intel answers BOOLs that are not 1 (2, 4, 128…): every IsSupported here is
// printed raw and tested `!= 0`.
//
// The JSON written next to the text is the fixture the negotiation tests of
// C4.5 are built from.

#include "Caps.h"

#include "Json.h"
#include "Lab.h"

#include <objbase.h>

#include <d3d11_4.h>
#include <d3d12.h>
#include <d3d12video.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

namespace lab {

namespace {

struct Options
{
    std::wstring jsonPath;
    bool json = true;
    bool privilege = true;
    bool software = false;
    bool dda = true;
    bool videoTimestamps = true;
    int rounds = 200;
};

void say(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
}

// ── Names ───────────────────────────────────────────────────────────────────

struct FlagName
{
    unsigned long long bit;
    const char* name;
};

/// "a|b|0x40": the named bits of @p flags, then whatever is left unnamed when
/// @p rest is asked for.
std::string decode(unsigned long long flags, std::initializer_list<FlagName> names,
                   bool rest = true)
{
    std::string out;
    unsigned long long left = flags;
    for (const FlagName& n : names) {
        if (!(flags & n.bit)) continue;
        if (!out.empty()) out += '|';
        out += n.name;
        left &= ~n.bit;
    }
    if (rest && left) {
        if (!out.empty()) out += '|';
        out += hex(left);
    }
    return out.empty() ? "none" : out;
}

const std::initializer_list<FlagName> kSupportFlags = {
    {D3D12_VIDEO_ENCODER_SUPPORT_FLAG_GENERAL_SUPPORT_OK, "ok"},
    {D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RATE_CONTROL_RECONFIGURATION_AVAILABLE, "rc-reconfig"},
    {D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RESOLUTION_RECONFIGURATION_AVAILABLE, "res-reconfig"},
    {D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RATE_CONTROL_VBV_SIZE_CONFIG_AVAILABLE, "vbv-size"},
    {D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RATE_CONTROL_FRAME_ANALYSIS_AVAILABLE, "frame-analysis"},
    {D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RECONSTRUCTED_FRAMES_REQUIRE_TEXTURE_ARRAYS,
     "recon-texture-array"},
    {D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RATE_CONTROL_DELTA_QP_AVAILABLE, "delta-qp"},
    {D3D12_VIDEO_ENCODER_SUPPORT_FLAG_SUBREGION_LAYOUT_RECONFIGURATION_AVAILABLE,
     "subregion-reconfig"},
    {D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RATE_CONTROL_ADJUSTABLE_QP_RANGE_AVAILABLE, "qp-range"},
    {D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RATE_CONTROL_INITIAL_QP_AVAILABLE, "initial-qp"},
    {D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RATE_CONTROL_MAX_FRAME_SIZE_AVAILABLE, "max-frame-size"},
    {D3D12_VIDEO_ENCODER_SUPPORT_FLAG_SEQUENCE_GOP_RECONFIGURATION_AVAILABLE, "gop-reconfig"},
    {D3D12_VIDEO_ENCODER_SUPPORT_FLAG_MOTION_ESTIMATION_PRECISION_MODE_LIMIT_AVAILABLE,
     "me-precision-limit"},
    {D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RATE_CONTROL_EXTENSION1_SUPPORT, "rc-extension1"},
    {D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RATE_CONTROL_QUALITY_VS_SPEED_AVAILABLE, "quality-vs-speed"},
    {D3D12_VIDEO_ENCODER_SUPPORT_FLAG_READABLE_RECONSTRUCTED_PICTURE_LAYOUT_AVAILABLE,
     "readable-recon"},
};

const std::initializer_list<FlagName> kValidationFlags = {
    {D3D12_VIDEO_ENCODER_VALIDATION_FLAG_CODEC_NOT_SUPPORTED, "codec"},
    {D3D12_VIDEO_ENCODER_VALIDATION_FLAG_INPUT_FORMAT_NOT_SUPPORTED, "input-format"},
    {D3D12_VIDEO_ENCODER_VALIDATION_FLAG_CODEC_CONFIGURATION_NOT_SUPPORTED, "codec-config"},
    {D3D12_VIDEO_ENCODER_VALIDATION_FLAG_RATE_CONTROL_MODE_NOT_SUPPORTED, "rc-mode"},
    {D3D12_VIDEO_ENCODER_VALIDATION_FLAG_RATE_CONTROL_CONFIGURATION_NOT_SUPPORTED, "rc-config"},
    {D3D12_VIDEO_ENCODER_VALIDATION_FLAG_INTRA_REFRESH_MODE_NOT_SUPPORTED, "intra-refresh"},
    {D3D12_VIDEO_ENCODER_VALIDATION_FLAG_SUBREGION_LAYOUT_MODE_NOT_SUPPORTED, "subregion-mode"},
    {D3D12_VIDEO_ENCODER_VALIDATION_FLAG_RESOLUTION_NOT_SUPPORTED_IN_LIST, "resolution"},
    {D3D12_VIDEO_ENCODER_VALIDATION_FLAG_GOP_STRUCTURE_NOT_SUPPORTED, "gop"},
    {D3D12_VIDEO_ENCODER_VALIDATION_FLAG_SUBREGION_LAYOUT_DATA_NOT_SUPPORTED, "subregion-data"},
    {D3D12_VIDEO_ENCODER_VALIDATION_FLAG_QPMAP_NOT_SUPPORTED, "qp-map"},
    {D3D12_VIDEO_ENCODER_VALIDATION_FLAG_DIRTY_REGIONS_NOT_SUPPORTED, "dirty-regions"},
    {D3D12_VIDEO_ENCODER_VALIDATION_FLAG_MOTION_SEARCH_NOT_SUPPORTED, "motion-search"},
    {D3D12_VIDEO_ENCODER_VALIDATION_FLAG_FRAME_ANALYSIS_NOT_SUPPORTED, "frame-analysis"},
};

const std::initializer_list<FlagName> kHevcConfigFlags = {
    {D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG_BFRAME_LTR_COMBINED_SUPPORT,
     "b-ltr"},
    {D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG_INTRA_SLICE_CONSTRAINED_ENCODING_SUPPORT,
     "intra-slice-constrained"},
    {D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG_CONSTRAINED_INTRAPREDICTION_SUPPORT,
     "constrained-intra"},
    {D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG_SAO_FILTER_SUPPORT, "sao"},
    {D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG_ASYMETRIC_MOTION_PARTITION_SUPPORT,
     "amp"},
    {D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG_ASYMETRIC_MOTION_PARTITION_REQUIRED,
     "amp-required"},
    {D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG_TRANSFORM_SKIP_SUPPORT,
     "transform-skip"},
    {D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG_DISABLING_LOOP_FILTER_ACROSS_SLICES_SUPPORT,
     "no-loop-filter-across-slices"},
    {D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG_P_FRAMES_IMPLEMENTED_AS_LOW_DELAY_B_FRAMES,
     "p-as-low-delay-b"},
    {D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG_NUM_REF_IDX_ACTIVE_OVERRIDE_FLAG_SLICE_SUPPORT,
     "num-ref-idx-override"},
};

const std::initializer_list<FlagName> kH264ConfigFlags = {
    {D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264_FLAG_CABAC_ENCODING_SUPPORT, "cabac"},
    {D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264_FLAG_INTRA_SLICE_CONSTRAINED_ENCODING_SUPPORT,
     "intra-slice-constrained"},
    {D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264_FLAG_BFRAME_LTR_COMBINED_SUPPORT,
     "b-ltr"},
    {D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264_FLAG_ADAPTIVE_8x8_TRANSFORM_ENCODING_SUPPORT,
     "adaptive-8x8"},
    {D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264_FLAG_DIRECT_SPATIAL_ENCODING_SUPPORT,
     "direct-spatial"},
    {D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264_FLAG_DIRECT_TEMPORAL_ENCODING_SUPPORT,
     "direct-temporal"},
    {D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264_FLAG_CONSTRAINED_INTRAPREDICTION_SUPPORT,
     "constrained-intra"},
    {D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264_FLAG_NUM_REF_IDX_ACTIVE_OVERRIDE_FLAG_SLICE_SUPPORT,
     "num-ref-idx-override"},
};

const std::initializer_list<FlagName> kFormatSupport1 = {
    {D3D12_FORMAT_SUPPORT1_TEXTURE2D, "texture2d"},
    {D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE, "sample"},
    {D3D12_FORMAT_SUPPORT1_RENDER_TARGET, "render-target"},
    {D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW, "typed-uav"},
    {D3D12_FORMAT_SUPPORT1_DECODER_OUTPUT, "decoder-output"},
    {D3D12_FORMAT_SUPPORT1_VIDEO_PROCESSOR_OUTPUT, "vp-output"},
    {D3D12_FORMAT_SUPPORT1_VIDEO_PROCESSOR_INPUT, "vp-input"},
    {D3D12_FORMAT_SUPPORT1_VIDEO_ENCODER, "video-encoder"},
};

const std::initializer_list<FlagName> kFormatSupport2 = {
    {D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD, "uav-typed-load"},
    {D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE, "uav-typed-store"},
    {D3D12_FORMAT_SUPPORT2_MULTIPLANE_OVERLAY, "multiplane-overlay"},
};

const char* formatName(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_NV12: return "NV12";
    case DXGI_FORMAT_P010: return "P010";
    case DXGI_FORMAT_AYUV: return "AYUV";
    case DXGI_FORMAT_Y410: return "Y410";
    default: return "?";
    }
}

const char* queueTypeName(D3D12_COMMAND_LIST_TYPE t)
{
    switch (t) {
    case D3D12_COMMAND_LIST_TYPE_DIRECT: return "DIRECT";
    case D3D12_COMMAND_LIST_TYPE_COMPUTE: return "COMPUTE";
    case D3D12_COMMAND_LIST_TYPE_COPY: return "COPY";
    case D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE: return "VIDEO_DECODE";
    case D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS: return "VIDEO_PROCESS";
    case D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE: return "VIDEO_ENCODE";
    default: return "?";
    }
}

const char* priorityName(int p)
{
    switch (p) {
    case D3D12_COMMAND_QUEUE_PRIORITY_NORMAL: return "NORMAL";
    case D3D12_COMMAND_QUEUE_PRIORITY_HIGH: return "HIGH";
    case D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME: return "GLOBAL_REALTIME";
    default: return "?";
    }
}

const char* rateModeName(int m)
{
    switch (m) {
    case D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_ABSOLUTE_QP_MAP: return "absolute-qp-map";
    case D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CQP: return "cqp";
    case D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CBR: return "cbr";
    case D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_VBR: return "vbr";
    case D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_QVBR: return "qvbr";
    default: return "?";
    }
}

const char* subregionModeName(int m)
{
    switch (m) {
    case D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME: return "full-frame";
    case D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_BYTES_PER_SUBREGION: return "bytes";
    case D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_SQUARE_UNITS_PER_SUBREGION_ROW_UNALIGNED:
        return "units";
    case D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_ROWS_PER_SUBREGION:
        return "rows";
    case D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_SUBREGIONS_PER_FRAME:
        return "subregions-per-frame";
    case D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_GRID_PARTITION:
        return "uniform-grid";
    case D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_CONFIGURABLE_GRID_PARTITION:
        return "configurable-grid";
    default: return "?";
    }
}

std::string hevcLevelName(D3D12_VIDEO_ENCODER_LEVELS_HEVC level)
{
    static const char* kNames[] = {"1", "2",   "2.1", "3", "3.1", "4",  "4.1",
                                   "5", "5.1", "5.2", "6", "6.1", "6.2"};
    const auto i = static_cast<size_t>(level);
    return i < sizeof(kNames) / sizeof(kNames[0]) ? kNames[i] : std::to_string(i);
}

std::string h264LevelName(D3D12_VIDEO_ENCODER_LEVELS_H264 level)
{
    static const char* kNames[] = {"1",   "1b",  "1.1", "1.2", "1.3", "2",   "2.1",
                                   "2.2", "3",   "3.1", "3.2", "4",   "4.1", "4.2",
                                   "5",   "5.1", "5.2", "6",   "6.1", "6.2"};
    const auto i = static_cast<size_t>(level);
    return i < sizeof(kNames) / sizeof(kNames[0]) ? kNames[i] : std::to_string(i);
}

const char* cuSizeName(D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE s)
{
    switch (s) {
    case D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_8x8: return "8";
    case D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_16x16: return "16";
    case D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_32x32: return "32";
    case D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_64x64: return "64";
    default: return "?";
    }
}

const char* tuSizeName(D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE s)
{
    switch (s) {
    case D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE_4x4: return "4";
    case D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE_8x8: return "8";
    case D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE_16x16: return "16";
    case D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE_32x32: return "32";
    default: return "?";
    }
}

std::string featureLevelName(D3D_FEATURE_LEVEL fl)
{
    char out[16];
    std::snprintf(out, sizeof(out), "%d_%d", (fl >> 12) & 0xf, (fl >> 8) & 0xf);
    return out;
}

std::string shaderModelName(D3D_SHADER_MODEL sm)
{
    char out[16];
    std::snprintf(out, sizeof(out), "%d.%d", (sm >> 4) & 0xf, sm & 0xf);
    return out;
}

// ── Small helpers ───────────────────────────────────────────────────────────

/// Waits for everything submitted to @p queue so far.
bool drain(ID3D12Device* device, ID3D12CommandQueue* queue, DWORD timeoutMs = 2000)
{
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
    if (FAILED(queue->Signal(fence.Get(), 1))) return false;
    if (fence->GetCompletedValue() >= 1) return true;
    HANDLE event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return false;
    fence->SetEventOnCompletion(1, event);
    const bool done = ::WaitForSingleObject(event, timeoutMs) == WAIT_OBJECT_0;
    ::CloseHandle(event);
    return done;
}

ComPtr<ID3D12Resource> makeBuffer(ID3D12Device* device, D3D12_HEAP_TYPE heap, UINT64 size,
                                  D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr,
                                    IID_PPV_ARGS(&buffer));
    return buffer;
}

struct Stats
{
    double mean = 0, p50 = 0, p99 = 0, max = 0;
};

Stats stats(std::vector<double> samples)
{
    Stats s;
    if (samples.empty()) return s;
    std::sort(samples.begin(), samples.end());
    double sum = 0;
    for (double v : samples)
        sum += v;
    const size_t n = samples.size();
    s.mean = sum / static_cast<double>(n);
    s.p50 = samples[n / 2];
    s.p99 = samples[std::min(n - 1, static_cast<size_t>(static_cast<double>(n) * 0.99))];
    s.max = samples.back();
    return s;
}

// ── Identity and outputs ────────────────────────────────────────────────────

void identity(const Adapter& a, Json& j)
{
    const DXGI_ADAPTER_DESC1& d = a.desc;
    const std::string umd = umdVersion(a.adapter.Get());
    const std::string hags = hagsState(d.AdapterLuid);
    say("=== adapter %u: %s (vendor 0x%04x, device 0x%04x)", a.index, a.name.c_str(), d.VendorId,
        d.DeviceId);
    for (UINT dup : a.duplicates)
        say(" [also #%u]", dup);
    say("\n  LUID %s (Chrome %s), driver %s, %llu MB dedicated, HAGS %s\n",
        luidHex(d.AdapterLuid).c_str(), luidChrome(d.AdapterLuid).c_str(),
        umd.empty() ? "?" : umd.c_str(),
        static_cast<unsigned long long>(d.DedicatedVideoMemory / (1024 * 1024)), hags.c_str());

    j.field("index", a.index).field("name", a.name);
    j.field("vendorId", hex(d.VendorId)).field("deviceId", hex(d.DeviceId));
    j.field("subSysId", hex(d.SubSysId)).field("revision", d.Revision);
    j.field("luid", luidHex(d.AdapterLuid)).field("luidChrome", luidChrome(d.AdapterLuid));
    j.key("duplicateIndices").beginArray();
    for (UINT dup : a.duplicates)
        j.value(dup);
    j.endArray();
    j.field("umd", umd);
    j.field("dedicatedVideoMemoryMB",
            static_cast<unsigned long long>(d.DedicatedVideoMemory / (1024 * 1024)));
    j.field("software", (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0);
    j.field("hags", hags);

    j.key("outputs").beginArray();
    ComPtr<IDXGIOutput> output;
    for (UINT i = 0; a.adapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND;
         ++i, output.Reset()) {
        DXGI_OUTPUT_DESC od = {};
        if (FAILED(output->GetDesc(&od))) continue;
        DEVMODEW mode = {};
        mode.dmSize = sizeof(mode);
        const bool haveMode =
            ::EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &mode) != FALSE;
        bool hdr = false;
        ComPtr<IDXGIOutput6> output6;
        DXGI_OUTPUT_DESC1 od1 = {};
        if (SUCCEEDED(output.As(&output6)) && SUCCEEDED(output6->GetDesc1(&od1)))
            hdr = od1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        const std::string name = utf8(od.DeviceName);
        say("  output %s: %lux%lu @ %lu Hz at (%ld,%ld)%s%s\n", name.c_str(),
            haveMode ? mode.dmPelsWidth : 0, haveMode ? mode.dmPelsHeight : 0,
            haveMode ? mode.dmDisplayFrequency : 0, od.DesktopCoordinates.left,
            od.DesktopCoordinates.top, hdr ? ", HDR" : "",
            od.AttachedToDesktop ? "" : ", not attached");
        j.beginObject();
        j.field("name", name).field("attached", od.AttachedToDesktop != FALSE);
        j.field("width", haveMode ? mode.dmPelsWidth : 0UL);
        j.field("height", haveMode ? mode.dmPelsHeight : 0UL);
        j.field("refreshHz", haveMode ? mode.dmDisplayFrequency : 0UL);
        j.field("x", od.DesktopCoordinates.left).field("y", od.DesktopCoordinates.top);
        j.field("hdr", hdr);
        j.endObject();
    }
    j.endArray();
}

// ── The D3D12 device ────────────────────────────────────────────────────────

void deviceFeatures(ID3D12Device* d12, Json& j)
{
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1,
                                        D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_12_1,
                                        D3D_FEATURE_LEVEL_12_2};
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl = {};
    fl.NumFeatureLevels = static_cast<UINT>(sizeof(levels) / sizeof(levels[0]));
    fl.pFeatureLevelsRequested = levels;
    d12->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl));

    // Asked from the top down: a runtime answers E_INVALIDARG to a model it
    // does not know, and the first one it accepts carries the real ceiling.
    D3D_SHADER_MODEL highest = D3D_SHADER_MODEL_5_1;
    for (int sm = D3D_HIGHEST_SHADER_MODEL; sm >= D3D_SHADER_MODEL_6_0; --sm) {
        D3D12_FEATURE_DATA_SHADER_MODEL m = {static_cast<D3D_SHADER_MODEL>(sm)};
        if (SUCCEEDED(d12->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &m, sizeof(m)))) {
            highest = m.HighestShaderModel;
            break;
        }
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS o = {};
    d12->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o));
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1 = {};
    d12->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof(o1));
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3 = {};
    d12->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &o3, sizeof(o3));
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 o4 = {};
    d12->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &o4, sizeof(o4));
    D3D12_FEATURE_DATA_ARCHITECTURE1 arch = {};
    d12->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &arch, sizeof(arch));

    say("  D3D12: feature level %s, shader model %s, binding tier %d, typed UAV loads+ %s, "
        "waves %s %u-%u, 16-bit ops %s, copy-queue timestamps %s, UMA %s\n",
        featureLevelName(fl.MaxSupportedFeatureLevel).c_str(), shaderModelName(highest).c_str(),
        static_cast<int>(o.ResourceBindingTier), o.TypedUAVLoadAdditionalFormats ? "yes" : "no",
        o1.WaveOps ? "yes" : "no", o1.WaveLaneCountMin, o1.WaveLaneCountMax,
        o4.Native16BitShaderOpsSupported ? "yes" : "no",
        o3.CopyQueueTimestampQueriesSupported ? "yes" : "no",
        arch.UMA ? (arch.CacheCoherentUMA ? "yes (cache-coherent)" : "yes") : "no");

    j.field("featureLevel", featureLevelName(fl.MaxSupportedFeatureLevel));
    j.field("shaderModel", shaderModelName(highest));
    j.field("resourceBindingTier", static_cast<int>(o.ResourceBindingTier));
    j.field("typedUavLoadAdditionalFormats", o.TypedUAVLoadAdditionalFormats != FALSE);
    j.field("waveOps", o1.WaveOps != FALSE);
    j.field("waveLaneCountMin", o1.WaveLaneCountMin).field("waveLaneCountMax", o1.WaveLaneCountMax);
    j.field("native16BitShaderOps", o4.Native16BitShaderOpsSupported != FALSE);
    j.field("copyQueueTimestamps", o3.CopyQueueTimestampQueriesSupported != FALSE);
    j.field("uma", arch.UMA != FALSE).field("cacheCoherentUma", arch.CacheCoherentUMA != FALSE);
}

void formats(ID3D12Device* d12, Json& j)
{
    j.key("formats").beginArray();
    for (DXGI_FORMAT f : {DXGI_FORMAT_NV12, DXGI_FORMAT_P010, DXGI_FORMAT_AYUV}) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = {f, D3D12_FORMAT_SUPPORT1_NONE,
                                                D3D12_FORMAT_SUPPORT2_NONE};
        d12->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs));
        HRESULT created[3] = {};
        const D3D12_RESOURCE_FLAGS flags[3] = {D3D12_RESOURCE_FLAG_NONE,
                                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                               D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
        for (int i = 0; i < 3; ++i) {
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = 1920;
            rd.Height = 1080;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.Format = f;
            rd.SampleDesc.Count = 1;
            rd.Flags = flags[i];
            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            ComPtr<ID3D12Resource> r;
            created[i] = d12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                      D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                      IID_PPV_ARGS(&r));
        }
        const std::string s1 = decode(fs.Support1, kFormatSupport1, false);
        const std::string s2 = decode(fs.Support2, kFormatSupport2, false);
        say("  %s: %s | %s; create plain/UAV/RT %s/%s/%s\n", formatName(f), s1.c_str(), s2.c_str(),
            hr(created[0]).c_str(), hr(created[1]).c_str(), hr(created[2]).c_str());
        j.beginObject();
        j.field("format", formatName(f));
        j.field("support1", hex(fs.Support1)).field("support1Names", s1);
        j.field("support2", hex(fs.Support2)).field("support2Names", s2);
        j.field("createPlain", hr(created[0])).field("createUav", hr(created[1]));
        j.field("createRenderTarget", hr(created[2]));
        j.endObject();
    }
    j.endArray();
}

// ── Desktop Duplication, opened in D3D12 ────────────────────────────────────

void ddaOpen(IDXGIAdapter1* adapter, ID3D11Device* d11, ID3D12Device* d12, Json& j)
{
    j.key("dda").beginArray();
    ComPtr<IDXGIOutput> output;
    for (UINT i = 0; adapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND;
         ++i, output.Reset()) {
        DXGI_OUTPUT_DESC od = {};
        output->GetDesc(&od);
        const std::string name = utf8(od.DeviceName);
        j.beginObject();
        j.field("output", name);
        ComPtr<IDXGIOutput5> output5;
        HRESULT h = output.As(&output5);
        ComPtr<IDXGIOutputDuplication> dupl;
        if (SUCCEEDED(h)) {
            const DXGI_FORMAT formats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT,
                                           DXGI_FORMAT_B8G8R8A8_UNORM};
            h = output5->DuplicateOutput1(d11, 0, 2, formats, &dupl);
        }
        j.field("duplicate", hr(h));
        if (FAILED(h)) {
            say("  DDA %s: duplicate %s\n", name.c_str(), hr(h).c_str());
            j.endObject();
            continue;
        }
        // The first acquire hands the current desktop over; a few tries cover a
        // duplication that is not ready yet.
        HRESULT acquired = DXGI_ERROR_WAIT_TIMEOUT;
        for (int attempt = 0; attempt < 8 && acquired == DXGI_ERROR_WAIT_TIMEOUT; ++attempt) {
            DXGI_OUTDUPL_FRAME_INFO info = {};
            ComPtr<IDXGIResource> resource;
            acquired = dupl->AcquireNextFrame(250, &info, &resource);
            if (FAILED(acquired)) continue;
            ComPtr<IDXGIResource1> shareable;
            HANDLE handle = nullptr;
            HRESULT shared = resource.As(&shareable);
            if (SUCCEEDED(shared))
                shared = shareable->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr,
                                                       &handle);
            HRESULT opened = E_FAIL;
            D3D12_RESOURCE_DESC rd = {};
            if (SUCCEEDED(shared)) {
                ComPtr<ID3D12Resource> texture;
                opened = d12->OpenSharedHandle(handle, IID_PPV_ARGS(&texture));
                ::CloseHandle(handle);
                if (SUCCEEDED(opened)) rd = texture->GetDesc();
            }
            say("  DDA %s: shared handle %s, opened in D3D12 %s", name.c_str(), hr(shared).c_str(),
                hr(opened).c_str());
            if (SUCCEEDED(opened))
                say(" (%llux%u, format %d, flags %s)", static_cast<unsigned long long>(rd.Width),
                    rd.Height, static_cast<int>(rd.Format), hex(rd.Flags).c_str());
            say("\n");
            j.field("sharedHandle", hr(shared)).field("d3d12Open", hr(opened));
            if (SUCCEEDED(opened)) {
                j.field("width", static_cast<unsigned long long>(rd.Width));
                j.field("height", rd.Height).field("dxgiFormat", static_cast<int>(rd.Format));
                j.field("resourceFlags", hex(rd.Flags));
            }
            dupl->ReleaseFrame();
        }
        if (FAILED(acquired)) {
            say("  DDA %s: acquire %s\n", name.c_str(), hr(acquired).c_str());
            j.field("acquire", hr(acquired));
        }
        j.endObject();
    }
    j.endArray();
}

// ── The handshake every frame will make ─────────────────────────────────────

/// A shared D3D11 fence opened in D3D12 (fence A of the plan), a shared D3D12
/// fence opened in D3D11 (fence B), and the round the pipeline makes with
/// them: the capture context signals A, the D3D12 queue waits A and signals
/// B, the capture context waits B on the GPU — then, for the clock only, a
/// local fence the CPU waits on. Timed at rest: what the handshake itself
/// costs before any game competes for the GPU.
void fences(ID3D11Device* d11, ID3D12Device* d12, int rounds, Json& j)
{
    j.key("fences").beginObject();
    ComPtr<ID3D11Device5> d11_5;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11DeviceContext4> ctx4;
    d11->GetImmediateContext(&ctx);
    if (FAILED(d11->QueryInterface(IID_PPV_ARGS(&d11_5))) || FAILED(ctx.As(&ctx4))) {
        say("  fences: no ID3D11Device5 / ID3D11DeviceContext4\n");
        j.field("error", "no ID3D11Device5").endObject();
        return;
    }

    ComPtr<ID3D11Fence> a11;
    ComPtr<ID3D12Fence> a12;
    HANDLE handle = nullptr;
    HRESULT h = d11_5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&a11));
    if (SUCCEEDED(h)) h = a11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle);
    if (SUCCEEDED(h)) {
        h = d12->OpenSharedHandle(handle, IID_PPV_ARGS(&a12));
        ::CloseHandle(handle);
    }
    const HRESULT aResult = h;

    ComPtr<ID3D12Fence> b12;
    ComPtr<ID3D11Fence> b11;
    handle = nullptr;
    h = d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&b12));
    if (SUCCEEDED(h))
        h = d12->CreateSharedHandle(b12.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
    if (SUCCEEDED(h)) {
        h = d11_5->OpenSharedFence(handle, IID_PPV_ARGS(&b11));
        ::CloseHandle(handle);
    }
    const HRESULT bResult = h;
    j.field("d3d11ToD3d12", hr(aResult)).field("d3d12ToD3d11", hr(bResult));

    ComPtr<ID3D11Fence> c11;
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(aResult) || FAILED(bResult) ||
        FAILED(d11_5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&c11))) ||
        FAILED(d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) {
        say("  fences: D3D11->D3D12 %s, D3D12->D3D11 %s - no handshake\n", hr(aResult).c_str(),
            hr(bResult).c_str());
        j.endObject();
        return;
    }

    HANDLE event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::vector<double> samples;
    std::string failure;
    for (UINT64 v = 1; v <= static_cast<UINT64>(rounds); ++v) {
        const int64_t start = nowUs();
        ctx4->Signal(a11.Get(), v);
        ctx4->Flush();
        queue->Wait(a12.Get(), v);
        queue->Signal(b12.Get(), v);
        ctx4->Wait(b11.Get(), v);
        ctx4->Signal(c11.Get(), v);
        ctx4->Flush();
        if (c11->GetCompletedValue() < v) {
            c11->SetEventOnCompletion(v, event);
            if (::WaitForSingleObject(event, 1000) != WAIT_OBJECT_0) {
                failure = "timed out at round " + std::to_string(v);
                break;
            }
        }
        samples.push_back(static_cast<double>(nowUs() - start));
    }
    ::CloseHandle(event);
    const Stats s = stats(samples);
    say("  fences: D3D11->D3D12 %s, D3D12->D3D11 %s; handshake at rest %.0f us mean, %.0f p50, "
        "%.0f p99, %.0f max (%zu rounds)%s%s\n",
        hr(aResult).c_str(), hr(bResult).c_str(), s.mean, s.p50, s.p99, s.max, samples.size(),
        failure.empty() ? "" : " - ", failure.c_str());
    j.field("rounds", static_cast<unsigned long long>(samples.size()));
    j.field("meanUs", s.mean).field("p50Us", s.p50).field("p99Us", s.p99).field("maxUs", s.max);
    if (!failure.empty()) j.field("error", failure);
    j.endObject();
}

// ── Queues, priorities and clocks ───────────────────────────────────────────

const D3D12_COMMAND_LIST_TYPE kQueueTypes[] = {
    D3D12_COMMAND_LIST_TYPE_DIRECT,        D3D12_COMMAND_LIST_TYPE_COMPUTE,
    D3D12_COMMAND_LIST_TYPE_COPY,          D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE,
    D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS, D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE,
};

const int kPriorities[] = {D3D12_COMMAND_QUEUE_PRIORITY_NORMAL, D3D12_COMMAND_QUEUE_PRIORITY_HIGH,
                           D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME};

void queues(ID3D12Device* d12, const GUID& creator, Json& j)
{
    ComPtr<ID3D12Device9> d9;
    d12->QueryInterface(IID_PPV_ARGS(&d9));
    say("  queues (plain / own CreatorID):\n");
    j.field("createCommandQueue1", d9 != nullptr);
    j.key("queues").beginArray();
    for (D3D12_COMMAND_LIST_TYPE type : kQueueTypes) {
        say("    %-14s", queueTypeName(type));
        for (int priority : kPriorities) {
            D3D12_COMMAND_QUEUE_DESC qd = {};
            qd.Type = type;
            qd.Priority = priority;
            ComPtr<ID3D12CommandQueue> plain;
            const HRESULT h1 = d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&plain));
            HRESULT h2 = E_NOINTERFACE;
            if (d9) {
                ComPtr<ID3D12CommandQueue> own;
                h2 = d9->CreateCommandQueue1(&qd, creator, IID_PPV_ARGS(&own));
            }
            say(" %s %s/%s", priorityName(priority), SUCCEEDED(h1) ? "ok" : hr(h1).c_str(),
                SUCCEEDED(h2) ? "ok" : hr(h2).c_str());
            j.beginObject();
            j.field("type", queueTypeName(type)).field("priority", priorityName(priority));
            j.field("create", hr(h1)).field("createWithCreatorId", hr(h2));
            j.endObject();
        }
        say("\n");
    }
    j.endArray();
}

/// Two timestamps on a queue of @p type, resolved and read back: whether the
/// queue can be timed at all, and at which frequency. A video-encode list
/// resolves into a default buffer in its own write state, then a copy queue
/// brings it back — a readback heap cannot be written from a video queue.
std::string timestampRoundTrip(ID3D12Device* d12, ID3D12CommandQueue* queue,
                               D3D12_COMMAND_LIST_TYPE type, UINT64& ticks)
{
    ticks = 0;
    D3D12_QUERY_HEAP_DESC hd = {};
    hd.Type = type == D3D12_COMMAND_LIST_TYPE_COPY ? D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP
                                                   : D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    hd.Count = 2;
    ComPtr<ID3D12QueryHeap> heap;
    HRESULT h = d12->CreateQueryHeap(&hd, IID_PPV_ARGS(&heap));
    if (FAILED(h)) return "query heap " + hr(h);
    ComPtr<ID3D12Resource> readback =
        makeBuffer(d12, D3D12_HEAP_TYPE_READBACK, 16, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!readback) return "readback buffer";
    ComPtr<ID3D12CommandAllocator> allocator;
    h = d12->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator));
    if (FAILED(h)) return "allocator " + hr(h);

    if (type != D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE) {
        ComPtr<ID3D12GraphicsCommandList> list;
        h = d12->CreateCommandList(0, type, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
        if (FAILED(h)) return "list " + hr(h);
        list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, readback.Get(), 0);
        h = list->Close();
        if (FAILED(h)) return "close " + hr(h);
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        if (!drain(d12, queue)) return "no completion";
    } else {
        ComPtr<ID3D12Resource> gpu =
            makeBuffer(d12, D3D12_HEAP_TYPE_DEFAULT, 16, D3D12_RESOURCE_STATE_COMMON);
        if (!gpu) return "default buffer";
        ComPtr<ID3D12VideoEncodeCommandList> list;
        h = d12->CreateCommandList(0, type, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
        if (FAILED(h)) return "list " + hr(h);
        D3D12_RESOURCE_BARRIER toWrite = {};
        toWrite.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toWrite.Transition.pResource = gpu.Get();
        toWrite.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toWrite.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        toWrite.Transition.StateAfter = D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE;
        D3D12_RESOURCE_BARRIER back = toWrite;
        back.Transition.StateBefore = D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE;
        back.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        list->ResourceBarrier(1, &toWrite);
        list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, gpu.Get(), 0);
        list->ResourceBarrier(1, &back);
        h = list->Close();
        if (FAILED(h)) return "close " + hr(h);
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        if (!drain(d12, queue)) return "no completion";
        const HRESULT removed = d12->GetDeviceRemovedReason();
        if (FAILED(removed)) return "device removed " + hr(removed);

        D3D12_COMMAND_QUEUE_DESC cd = {};
        cd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        ComPtr<ID3D12CommandQueue> copyQueue;
        ComPtr<ID3D12CommandAllocator> copyAllocator;
        ComPtr<ID3D12GraphicsCommandList> copy;
        if (FAILED(d12->CreateCommandQueue(&cd, IID_PPV_ARGS(&copyQueue))) ||
            FAILED(d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                                               IID_PPV_ARGS(&copyAllocator))) ||
            FAILED(d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, copyAllocator.Get(),
                                          nullptr, IID_PPV_ARGS(&copy))))
            return "copy path";
        copy->CopyBufferRegion(readback.Get(), 0, gpu.Get(), 0, 16);
        copy->Close();
        ID3D12CommandList* copies[] = {copy.Get()};
        copyQueue->ExecuteCommandLists(1, copies);
        if (!drain(d12, copyQueue.Get())) return "copy did not complete";
    }

    const HRESULT removed = d12->GetDeviceRemovedReason();
    if (FAILED(removed)) return "device removed " + hr(removed);
    void* mapped = nullptr;
    D3D12_RANGE range = {0, 16};
    if (FAILED(readback->Map(0, &range, &mapped))) return "map";
    const auto* stamps = static_cast<const UINT64*>(mapped);
    const UINT64 t0 = stamps[0];
    const UINT64 t1 = stamps[1];
    D3D12_RANGE none = {0, 0};
    readback->Unmap(0, &none);
    if (t0 == 0 && t1 == 0) return "zeros";
    ticks = t1 >= t0 ? t1 - t0 : 0;
    return "ok";
}

void timestamps(ID3D12Device* d12, bool videoToo, Json& j)
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3 = {};
    d12->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &o3, sizeof(o3));
    say("  timestamps:");
    j.key("timestamps").beginArray();
    for (D3D12_COMMAND_LIST_TYPE type : kQueueTypes) {
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = type;
        ComPtr<ID3D12CommandQueue> queue;
        if (FAILED(d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) continue;
        UINT64 frequency = 0;
        const HRESULT hf = queue->GetTimestampFrequency(&frequency);
        UINT64 gpu = 0, cpu = 0;
        const HRESULT hc = queue->GetClockCalibration(&gpu, &cpu);
        std::string trip = "not tried";
        UINT64 ticks = 0;
        const bool timeable =
            type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_COMPUTE ||
            (type == D3D12_COMMAND_LIST_TYPE_COPY && o3.CopyQueueTimestampQueriesSupported) ||
            (type == D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE && videoToo);
        if (timeable && SUCCEEDED(hf)) trip = timestampRoundTrip(d12, queue.Get(), type, ticks);
        say(" %s %s%s", queueTypeName(type),
            SUCCEEDED(hf) ? (std::to_string(frequency / 1000) + " kHz").c_str() : hr(hf).c_str(),
            timeable ? (" (" + trip + ")").c_str() : "");
        j.beginObject();
        j.field("type", queueTypeName(type)).field("frequency", hr(hf));
        j.field("frequencyHz", static_cast<unsigned long long>(frequency));
        j.field("calibration", hr(hc)).field("roundTrip", trip);
        j.field("roundTripTicks", static_cast<unsigned long long>(ticks));
        j.endObject();
    }
    j.endArray();
    say("\n");
}

// ── D3D12 Video Encode ──────────────────────────────────────────────────────

template <typename T> bool feature(ID3D12VideoDevice3* v, D3D12_FEATURE_VIDEO f, T& data)
{
    return SUCCEEDED(v->CheckFeatureSupport(f, &data, sizeof(data)));
}

void outputResolution(ID3D12VideoDevice3* v, D3D12_VIDEO_ENCODER_CODEC codec, Json& j)
{
    D3D12_FEATURE_DATA_VIDEO_ENCODER_OUTPUT_RESOLUTION_RATIOS_COUNT count = {};
    count.Codec = codec;
    feature(v, D3D12_FEATURE_VIDEO_ENCODER_OUTPUT_RESOLUTION_RATIOS_COUNT, count);
    std::vector<D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_RATIO_DESC> ratios(
        std::max<UINT>(1, count.ResolutionRatiosCount));
    D3D12_FEATURE_DATA_VIDEO_ENCODER_OUTPUT_RESOLUTION res = {};
    res.Codec = codec;
    res.ResolutionRatiosCount = count.ResolutionRatiosCount;
    res.pResolutionRatios = count.ResolutionRatiosCount ? ratios.data() : nullptr;
    feature(v, D3D12_FEATURE_VIDEO_ENCODER_OUTPUT_RESOLUTION, res);
    say("    output resolution: %s, %ux%u .. %ux%u, width multiple of %u, height multiple of %u\n",
        res.IsSupported ? "ok" : "refused", res.MinResolutionSupported.Width,
        res.MinResolutionSupported.Height, res.MaxResolutionSupported.Width,
        res.MaxResolutionSupported.Height, res.ResolutionWidthMultipleRequirement,
        res.ResolutionHeightMultipleRequirement);
    j.key("outputResolution").beginObject();
    j.field("supported", static_cast<unsigned>(res.IsSupported));
    j.field("minWidth", res.MinResolutionSupported.Width);
    j.field("minHeight", res.MinResolutionSupported.Height);
    j.field("maxWidth", res.MaxResolutionSupported.Width);
    j.field("maxHeight", res.MaxResolutionSupported.Height);
    j.field("widthMultiple", res.ResolutionWidthMultipleRequirement);
    j.field("heightMultiple", res.ResolutionHeightMultipleRequirement);
    j.field("ratiosCount", count.ResolutionRatiosCount);
    j.endObject();
}

void rateModes(ID3D12VideoDevice3* v, D3D12_VIDEO_ENCODER_CODEC codec, Json& j)
{
    say("      rate control:");
    j.key("rateControlModes").beginObject();
    for (int m = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_ABSOLUTE_QP_MAP;
         m <= D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_QVBR; ++m) {
        D3D12_FEATURE_DATA_VIDEO_ENCODER_RATE_CONTROL_MODE rm = {};
        rm.Codec = codec;
        rm.RateControlMode = static_cast<D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE>(m);
        feature(v, D3D12_FEATURE_VIDEO_ENCODER_RATE_CONTROL_MODE, rm);
        say(" %s %d", rateModeName(m), rm.IsSupported);
        j.field(rateModeName(m), static_cast<unsigned>(rm.IsSupported));
    }
    j.endObject();
    say("\n");
}

/// The first HEVC configuration the driver accepts, searching from the
/// smallest minimum sizes and the largest maximum ones — the same order the
/// 21/09 probe used, so the two reports compare.
bool hevcConfigSupport(ID3D12VideoDevice3* v, D3D12_VIDEO_ENCODER_PROFILE_DESC profile,
                       D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC& caps)
{
    D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT ccs = {};
    ccs.Codec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
    ccs.Profile = profile;
    ccs.CodecSupportLimits.DataSize = sizeof(caps);
    ccs.CodecSupportLimits.pHEVCSupport = &caps;
    for (int minCu = 0; minCu < 4; ++minCu)
        for (int maxCu = 3; maxCu >= minCu; --maxCu)
            for (int minTu = 0; minTu < 4; ++minTu)
                for (int maxTu = 3; maxTu >= minTu; --maxTu)
                    for (int depth = 0; depth < 5; ++depth) {
                        caps = {};
                        caps.MinLumaCodingUnitSize =
                            static_cast<D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE>(minCu);
                        caps.MaxLumaCodingUnitSize =
                            static_cast<D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE>(maxCu);
                        caps.MinLumaTransformUnitSize =
                            static_cast<D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE>(minTu);
                        caps.MaxLumaTransformUnitSize =
                            static_cast<D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE>(maxTu);
                        caps.max_transform_hierarchy_depth_inter = static_cast<UCHAR>(depth);
                        caps.max_transform_hierarchy_depth_intra = static_cast<UCHAR>(depth);
                        ccs.IsSupported = FALSE;
                        if (feature(v, D3D12_FEATURE_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT,
                                    ccs) &&
                            ccs.IsSupported)
                            return true;
                    }
    return false;
}

/// One rate-control arrangement to put to D3D12_FEATURE_VIDEO_ENCODER_SUPPORT.
struct RcCase
{
    const char* id;
    D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE mode;
    D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAGS flags;
    bool intraRefresh;
};

// 20 Mbit/s at 60 fps; the VBV holds one frame, as the product's does.
constexpr UINT64 kBitrate = 20'000'000;
constexpr UINT64 kVbv = kBitrate / 60;

const RcCase kRcCases[] = {
    {"cbr", D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CBR, D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_NONE,
     false},
    {"cbr+vbv", D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CBR,
     D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_VBV_SIZES, false},
    {"cbr+vbv+ir", D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CBR,
     D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_VBV_SIZES, true},
    {"cbr+vbv+qprange", D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CBR,
     D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_VBV_SIZES |
         D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_QP_RANGE,
     false},
    {"cbr+vbv+maxframe", D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CBR,
     D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_VBV_SIZES |
         D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_MAX_FRAME_SIZE,
     false},
    {"cbr1+vbv+qprange+qvs", D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CBR,
     D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_VBV_SIZES |
         D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_QP_RANGE |
         D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_EXTENSION1_SUPPORT |
         D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_QUALITY_VS_SPEED,
     false},
    {"cqp", D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CQP, D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_NONE,
     false},
    {"cqp+deltaqp", D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CQP,
     D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_DELTA_QP, false},
    {"cqp+deltaqp+ir", D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CQP,
     D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_DELTA_QP, true},
    {"absolute-qp-map", D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_ABSOLUTE_QP_MAP,
     D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_NONE, false},
    {"absolute-qp-map+ext1", D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_ABSOLUTE_QP_MAP,
     D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_EXTENSION1_SUPPORT, false},
};

/// The parameter block a case points at: the plain structures, or their
/// "1" versions when the EXTENSION1 flag says so.
struct RcParams
{
    D3D12_VIDEO_ENCODER_RATE_CONTROL_CQP cqp = {30, 30, 30};
    D3D12_VIDEO_ENCODER_RATE_CONTROL_CQP1 cqp1 = {30, 30, 30, 0};
    D3D12_VIDEO_ENCODER_RATE_CONTROL_CBR cbr = {};
    D3D12_VIDEO_ENCODER_RATE_CONTROL_CBR1 cbr1 = {};
    D3D12_VIDEO_ENCODER_RATE_CONTROL_ABSOLUTE_QP_MAP qpMap = {0};

    RcParams()
    {
        cbr.InitialQP = 30;
        cbr.MinQP = 18;
        cbr.MaxQP = 51;
        cbr.MaxFrameBitSize = kVbv;
        cbr.TargetBitRate = kBitrate;
        cbr.VBVCapacity = kVbv;
        cbr.InitialVBVFullness = kVbv;
        cbr1.InitialQP = cbr.InitialQP;
        cbr1.MinQP = cbr.MinQP;
        cbr1.MaxQP = cbr.MaxQP;
        cbr1.MaxFrameBitSize = cbr.MaxFrameBitSize;
        cbr1.TargetBitRate = cbr.TargetBitRate;
        cbr1.VBVCapacity = cbr.VBVCapacity;
        cbr1.InitialVBVFullness = cbr.InitialVBVFullness;
        cbr1.QualityVsSpeed = 0;
    }

    void fill(const RcCase& c, D3D12_VIDEO_ENCODER_RATE_CONTROL& rc)
    {
        rc.Mode = c.mode;
        rc.Flags = c.flags;
        rc.TargetFrameRate = {60, 1};
        const bool ext1 =
            (c.flags & D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_EXTENSION1_SUPPORT) != 0;
        switch (c.mode) {
        case D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CQP:
            if (ext1) {
                rc.ConfigParams.DataSize = sizeof(cqp1);
                rc.ConfigParams.pConfiguration_CQP1 = &cqp1;
            } else {
                rc.ConfigParams.DataSize = sizeof(cqp);
                rc.ConfigParams.pConfiguration_CQP = &cqp;
            }
            break;
        case D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CBR:
            if (ext1) {
                rc.ConfigParams.DataSize = sizeof(cbr1);
                rc.ConfigParams.pConfiguration_CBR1 = &cbr1;
            } else {
                rc.ConfigParams.DataSize = sizeof(cbr);
                rc.ConfigParams.pConfiguration_CBR = &cbr;
            }
            break;
        case D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_ABSOLUTE_QP_MAP:
            rc.ConfigParams.DataSize = sizeof(qpMap);
            rc.ConfigParams.pConfiguration_AbsoluteQPMap = &qpMap;
            break;
        default: break;
        }
    }
};

/// What SUPPORT (or SUPPORT1, when the runtime and driver take it) answers
/// for one arrangement. The limits are per resolution of the list.
struct SupportAnswer
{
    HRESULT result = E_FAIL;
    bool usedSupport1 = false;
    unsigned support = 0;
    unsigned validation = 0;
    std::string level;
    unsigned maxQualityVsSpeed = 0;
    std::vector<D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_LIMITS> limits;
};

void printAnswer(const char* id, const SupportAnswer& a, Json& j)
{
    say("        %-22s %s support %s (%s) validation %s level %s", id,
        SUCCEEDED(a.result) ? "ok " : hr(a.result).c_str(), hex(a.support).c_str(),
        decode(a.support, kSupportFlags).c_str(), decode(a.validation, kValidationFlags).c_str(),
        a.level.c_str());
    if (a.usedSupport1) say(" qvs<=%u", a.maxQualityVsSpeed);
    if (!a.limits.empty())
        say(" | qp-map %u px, subregion block %u px, IR <= %u frames, subregions <= %u",
            a.limits[0].QPMapRegionPixelsSize, a.limits[0].SubregionBlockPixelsSize,
            a.limits[0].MaxIntraRefreshFrameDuration, a.limits[0].MaxSubregionsNumber);
    say("\n");
    j.beginObject();
    j.field("case", id).field("hr", hr(a.result)).field("support1", a.usedSupport1);
    j.field("supportFlags", hex(a.support)).field("supportNames", decode(a.support, kSupportFlags));
    j.field("validationFlags", hex(a.validation));
    j.field("validationNames", decode(a.validation, kValidationFlags));
    j.field("suggestedLevel", a.level);
    j.field("maxQualityVsSpeed", a.maxQualityVsSpeed);
    j.key("limits").beginArray();
    for (const auto& l : a.limits) {
        j.beginObject();
        j.field("qpMapRegionPixels", l.QPMapRegionPixelsSize);
        j.field("subregionBlockPixels", l.SubregionBlockPixelsSize);
        j.field("maxIntraRefreshFrames", l.MaxIntraRefreshFrameDuration);
        j.field("maxSubregions", l.MaxSubregionsNumber);
        j.endObject();
    }
    j.endArray();
    j.endObject();
}

/// SUPPORT1 first; a runtime or driver that refuses it is asked the original
/// SUPPORT, and the report says which one answered.
template <typename Fill>
SupportAnswer askSupport(ID3D12VideoDevice3* v,
                         const std::vector<D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC>& res,
                         Fill&& fill, const std::function<std::string()>& levelName)
{
    SupportAnswer a;
    a.limits.resize(res.size());
    D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT1 s1 = {};
    fill(s1);
    s1.ResolutionsListCount = static_cast<UINT>(res.size());
    s1.pResolutionList = res.data();
    s1.pResolutionDependentSupport = a.limits.data();
    a.result = v->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_SUPPORT1, &s1, sizeof(s1));
    if (SUCCEEDED(a.result)) {
        a.usedSupport1 = true;
        a.support = s1.SupportFlags;
        a.validation = s1.ValidationFlags;
        a.maxQualityVsSpeed = s1.MaxQualityVsSpeed;
        a.level = levelName();
        return a;
    }
    D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT s = {};
    // SUPPORT is SUPPORT1 without its last two members.
    std::memcpy(&s, &s1, sizeof(s));
    s.ResolutionsListCount = static_cast<UINT>(res.size());
    s.pResolutionList = res.data();
    s.pResolutionDependentSupport = a.limits.data();
    a.result = v->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_SUPPORT, &s, sizeof(s));
    a.support = s.SupportFlags;
    a.validation = s.ValidationFlags;
    a.level = levelName();
    return a;
}

struct HevcProfile
{
    D3D12_VIDEO_ENCODER_PROFILE_HEVC profile;
    DXGI_FORMAT input;
    const char* name;
};

void hevcProfile(ID3D12VideoDevice3* v, const HevcProfile& p, Json& j)
{
    D3D12_VIDEO_ENCODER_PROFILE_HEVC profile = p.profile;
    D3D12_VIDEO_ENCODER_PROFILE_DESC pd = {};
    pd.DataSize = sizeof(profile);
    pd.pHEVCProfile = &profile;

    j.beginObject();
    j.field("profile", p.name).field("input", formatName(p.input));

    D3D12_FEATURE_DATA_VIDEO_ENCODER_INPUT_FORMAT fi = {};
    fi.Codec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
    fi.Profile = pd;
    fi.Format = p.input;
    feature(v, D3D12_FEATURE_VIDEO_ENCODER_INPUT_FORMAT, fi);
    say("    HEVC %s, input %s: %d\n", p.name, formatName(p.input), fi.IsSupported);
    j.field("inputSupported", static_cast<unsigned>(fi.IsSupported));
    if (!fi.IsSupported) {
        j.endObject();
        return;
    }

    D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC minLevel = {}, maxLevel = {};
    D3D12_FEATURE_DATA_VIDEO_ENCODER_PROFILE_LEVEL pl = {};
    pl.Codec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
    pl.Profile = pd;
    pl.MinSupportedLevel.DataSize = sizeof(minLevel);
    pl.MinSupportedLevel.pHEVCLevelSetting = &minLevel;
    pl.MaxSupportedLevel.DataSize = sizeof(maxLevel);
    pl.MaxSupportedLevel.pHEVCLevelSetting = &maxLevel;
    feature(v, D3D12_FEATURE_VIDEO_ENCODER_PROFILE_LEVEL, pl);
    say("      levels %s .. %s%s\n", hevcLevelName(minLevel.Level).c_str(),
        hevcLevelName(maxLevel.Level).c_str(),
        maxLevel.Tier == D3D12_VIDEO_ENCODER_TIER_HEVC_HIGH ? " (high tier)" : "");
    j.field("minLevel", hevcLevelName(minLevel.Level));
    j.field("maxLevel", hevcLevelName(maxLevel.Level));
    j.field("maxTierHigh", maxLevel.Tier == D3D12_VIDEO_ENCODER_TIER_HEVC_HIGH);

    rateModes(v, D3D12_VIDEO_ENCODER_CODEC_HEVC, j);

    D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC level51 = {D3D12_VIDEO_ENCODER_LEVELS_HEVC_51,
                                                               D3D12_VIDEO_ENCODER_TIER_HEVC_MAIN};
    D3D12_VIDEO_ENCODER_LEVEL_SETTING ls = {};
    ls.DataSize = sizeof(level51);
    ls.pHEVCLevelSetting = &level51;

    D3D12_FEATURE_DATA_VIDEO_ENCODER_INTRA_REFRESH_MODE ir = {};
    ir.Codec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
    ir.Profile = pd;
    ir.Level = ls;
    ir.IntraRefreshMode = D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_ROW_BASED;
    feature(v, D3D12_FEATURE_VIDEO_ENCODER_INTRA_REFRESH_MODE, ir);
    j.field("intraRefreshRowBased", static_cast<unsigned>(ir.IsSupported));

    say("      intra refresh row-based %d; subregions:", ir.IsSupported);
    j.key("subregionModes").beginObject();
    for (int m = D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME;
         m <= D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_CONFIGURABLE_GRID_PARTITION; ++m) {
        D3D12_FEATURE_DATA_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE sm = {};
        sm.Codec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
        sm.Profile = pd;
        sm.Level = ls;
        sm.SubregionMode = static_cast<D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE>(m);
        feature(v, D3D12_FEATURE_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE, sm);
        say(" %s %d", subregionModeName(m), sm.IsSupported);
        j.field(subregionModeName(m), static_cast<unsigned>(sm.IsSupported));
    }
    j.endObject();
    say("\n");

    D3D12_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT_HEVC pc = {};
    D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT pcs = {};
    pcs.Codec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
    pcs.Profile = pd;
    pcs.PictureSupport.DataSize = sizeof(pc);
    pcs.PictureSupport.pHEVCSupport = &pc;
    feature(v, D3D12_FEATURE_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT, pcs);
    say("      picture control %d: L0 for P %u, L0/L1 for B %u/%u, long-term %u, DPB %u\n",
        pcs.IsSupported, pc.MaxL0ReferencesForP, pc.MaxL0ReferencesForB, pc.MaxL1ReferencesForB,
        pc.MaxLongTermReferences, pc.MaxDPBCapacity);
    j.key("pictureControl").beginObject();
    j.field("supported", static_cast<unsigned>(pcs.IsSupported));
    j.field("maxL0ForP", pc.MaxL0ReferencesForP).field("maxL0ForB", pc.MaxL0ReferencesForB);
    j.field("maxL1ForB", pc.MaxL1ReferencesForB).field("maxLongTerm", pc.MaxLongTermReferences);
    j.field("maxDpb", pc.MaxDPBCapacity);
    j.endObject();

    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC caps = {};
    const bool configured = hevcConfigSupport(v, pd, caps);
    j.key("config").beginObject();
    j.field("found", configured);
    if (!configured) {
        say("      no codec configuration accepted\n");
        j.endObject();
        j.endObject();
        return;
    }
    say("      config: CU %s..%s, TU %s..%s, depth %u, flags %s (%s)\n",
        cuSizeName(caps.MinLumaCodingUnitSize), cuSizeName(caps.MaxLumaCodingUnitSize),
        tuSizeName(caps.MinLumaTransformUnitSize), tuSizeName(caps.MaxLumaTransformUnitSize),
        caps.max_transform_hierarchy_depth_inter, hex(caps.SupportFlags).c_str(),
        decode(caps.SupportFlags, kHevcConfigFlags).c_str());
    j.field("minCu", cuSizeName(caps.MinLumaCodingUnitSize));
    j.field("maxCu", cuSizeName(caps.MaxLumaCodingUnitSize));
    j.field("minTu", tuSizeName(caps.MinLumaTransformUnitSize));
    j.field("maxTu", tuSizeName(caps.MaxLumaTransformUnitSize));
    j.field("depth", static_cast<unsigned>(caps.max_transform_hierarchy_depth_inter));
    j.field("flags", hex(caps.SupportFlags))
        .field("flagNames", decode(caps.SupportFlags, kHevcConfigFlags));
    j.endObject();

    j.key("resources").beginArray();
    say("      resources:");
    for (const auto& r : {D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC{1920, 1080},
                          D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC{2560, 1440},
                          D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC{3840, 2160}}) {
        D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOURCE_REQUIREMENTS rr = {};
        rr.Codec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
        rr.Profile = pd;
        rr.InputFormat = p.input;
        rr.PictureTargetResolution = r;
        feature(v, D3D12_FEATURE_VIDEO_ENCODER_RESOURCE_REQUIREMENTS, rr);
        say(" %ux%u %d (bitstream align %u, metadata align %u, metadata max %u)", r.Width, r.Height,
            rr.IsSupported, rr.CompressedBitstreamBufferAccessAlignment,
            rr.EncoderMetadataBufferAccessAlignment, rr.MaxEncoderOutputMetadataBufferSize);
        j.beginObject();
        j.field("width", r.Width).field("height", r.Height);
        j.field("supported", static_cast<unsigned>(rr.IsSupported));
        j.field("bitstreamAlignment", rr.CompressedBitstreamBufferAccessAlignment);
        j.field("metadataAlignment", rr.EncoderMetadataBufferAccessAlignment);
        j.field("maxMetadataSize", rr.MaxEncoderOutputMetadataBufferSize);
        j.endObject();
    }
    j.endArray();
    say("\n");

    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC cfg = {};
    if (caps.SupportFlags &
        D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG_ASYMETRIC_MOTION_PARTITION_REQUIRED)
        cfg.ConfigurationFlags |=
            D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_USE_ASYMETRIC_MOTION_PARTITION;
    cfg.MinLumaCodingUnitSize = caps.MinLumaCodingUnitSize;
    cfg.MaxLumaCodingUnitSize = caps.MaxLumaCodingUnitSize;
    cfg.MinLumaTransformUnitSize = caps.MinLumaTransformUnitSize;
    cfg.MaxLumaTransformUnitSize = caps.MaxLumaTransformUnitSize;
    cfg.max_transform_hierarchy_depth_inter = caps.max_transform_hierarchy_depth_inter;
    cfg.max_transform_hierarchy_depth_intra = caps.max_transform_hierarchy_depth_intra;

    // The product's sequence: an endless GOP of P frames, one reference.
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_HEVC gop = {0, 1, 4};

    say("      support at 1920x1080 @ 60, endless GOP of P frames, one reference:\n");
    j.key("support").beginArray();
    const std::vector<D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC> fullHd = {{1920, 1080}};
    for (const RcCase& c : kRcCases) {
        RcParams params;
        D3D12_VIDEO_ENCODER_PROFILE_HEVC suggestedProfile = {};
        D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC suggestedLevel = {};
        const SupportAnswer a = askSupport(
            v, fullHd,
            [&](D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT1& s) {
                s.Codec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
                s.InputFormat = p.input;
                s.CodecConfiguration.DataSize = sizeof(cfg);
                s.CodecConfiguration.pHEVCConfig = &cfg;
                s.CodecGopSequence.DataSize = sizeof(gop);
                s.CodecGopSequence.pHEVCGroupOfPictures = &gop;
                params.fill(c, s.RateControl);
                s.IntraRefresh = c.intraRefresh ? D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_ROW_BASED
                                                : D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_NONE;
                s.SubregionFrameEncoding =
                    D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME;
                s.MaxReferenceFramesInDPB = 1;
                s.SuggestedProfile.DataSize = sizeof(suggestedProfile);
                s.SuggestedProfile.pHEVCProfile = &suggestedProfile;
                s.SuggestedLevel.DataSize = sizeof(suggestedLevel);
                s.SuggestedLevel.pHEVCLevelSetting = &suggestedLevel;
            },
            [&] {
                return hevcLevelName(suggestedLevel.Level) +
                       (suggestedLevel.Tier == D3D12_VIDEO_ENCODER_TIER_HEVC_HIGH ? " high" : "");
            });
        printAnswer(c.id, a, j);
    }
    j.endArray();

    // The same plain CBR at three sizes: how the per-resolution limits move.
    say("      limits by size (cbr+vbv):\n");
    j.key("limitsBySize").beginArray();
    for (const auto& r : {D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC{1920, 1080},
                          D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC{2560, 1440},
                          D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC{3840, 2160}}) {
        RcParams params;
        D3D12_VIDEO_ENCODER_PROFILE_HEVC suggestedProfile = {};
        D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC suggestedLevel = {};
        const std::vector<D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC> one = {r};
        const SupportAnswer a = askSupport(
            v, one,
            [&](D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT1& s) {
                s.Codec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
                s.InputFormat = p.input;
                s.CodecConfiguration.DataSize = sizeof(cfg);
                s.CodecConfiguration.pHEVCConfig = &cfg;
                s.CodecGopSequence.DataSize = sizeof(gop);
                s.CodecGopSequence.pHEVCGroupOfPictures = &gop;
                params.fill(kRcCases[1], s.RateControl);
                s.IntraRefresh = D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_NONE;
                s.SubregionFrameEncoding =
                    D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME;
                s.MaxReferenceFramesInDPB = 1;
                s.SuggestedProfile.DataSize = sizeof(suggestedProfile);
                s.SuggestedProfile.pHEVCProfile = &suggestedProfile;
                s.SuggestedLevel.DataSize = sizeof(suggestedLevel);
                s.SuggestedLevel.pHEVCLevelSetting = &suggestedLevel;
            },
            [&] { return hevcLevelName(suggestedLevel.Level); });
        const std::string id = std::to_string(r.Width) + "x" + std::to_string(r.Height);
        printAnswer(id.c_str(), a, j);
    }
    j.endArray();
    j.endObject();
}

void h264(ID3D12VideoDevice3* v, Json& j)
{
    D3D12_VIDEO_ENCODER_PROFILE_H264 profile = D3D12_VIDEO_ENCODER_PROFILE_H264_HIGH;
    D3D12_VIDEO_ENCODER_PROFILE_DESC pd = {};
    pd.DataSize = sizeof(profile);
    pd.pH264Profile = &profile;
    j.key("h264").beginObject();

    D3D12_FEATURE_DATA_VIDEO_ENCODER_INPUT_FORMAT fi = {};
    fi.Codec = D3D12_VIDEO_ENCODER_CODEC_H264;
    fi.Profile = pd;
    fi.Format = DXGI_FORMAT_NV12;
    feature(v, D3D12_FEATURE_VIDEO_ENCODER_INPUT_FORMAT, fi);
    say("    H.264 High, input NV12: %d\n", fi.IsSupported);
    j.field("profile", "high").field("inputSupported", static_cast<unsigned>(fi.IsSupported));
    if (!fi.IsSupported) {
        j.endObject();
        return;
    }
    outputResolution(v, D3D12_VIDEO_ENCODER_CODEC_H264, j);
    rateModes(v, D3D12_VIDEO_ENCODER_CODEC_H264, j);

    D3D12_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT_H264 pc = {};
    D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT pcs = {};
    pcs.Codec = D3D12_VIDEO_ENCODER_CODEC_H264;
    pcs.Profile = pd;
    pcs.PictureSupport.DataSize = sizeof(pc);
    pcs.PictureSupport.pH264Support = &pc;
    feature(v, D3D12_FEATURE_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT, pcs);
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264 caps = {};
    D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT ccs = {};
    ccs.Codec = D3D12_VIDEO_ENCODER_CODEC_H264;
    ccs.Profile = pd;
    ccs.CodecSupportLimits.DataSize = sizeof(caps);
    ccs.CodecSupportLimits.pH264Support = &caps;
    feature(v, D3D12_FEATURE_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT, ccs);
    say("      picture control %d: L0 for P %u, long-term %u, DPB %u; config %d flags %s (%s)\n",
        pcs.IsSupported, pc.MaxL0ReferencesForP, pc.MaxLongTermReferences, pc.MaxDPBCapacity,
        ccs.IsSupported, hex(caps.SupportFlags).c_str(),
        decode(caps.SupportFlags, kH264ConfigFlags).c_str());
    j.key("pictureControl").beginObject();
    j.field("supported", static_cast<unsigned>(pcs.IsSupported));
    j.field("maxL0ForP", pc.MaxL0ReferencesForP).field("maxLongTerm", pc.MaxLongTermReferences);
    j.field("maxDpb", pc.MaxDPBCapacity);
    j.endObject();
    j.field("configFlags", hex(caps.SupportFlags));
    j.field("configNames", decode(caps.SupportFlags, kH264ConfigFlags));

    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264 cfg = {};
    if (caps.SupportFlags &
        D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264_FLAG_CABAC_ENCODING_SUPPORT)
        cfg.ConfigurationFlags |=
            D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_ENABLE_CABAC_ENCODING;
    cfg.DirectModeConfig = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_DIRECT_MODES_DISABLED;
    cfg.DisableDeblockingFilterConfig =
        D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_SLICES_DEBLOCKING_MODE_0_ALL_LUMA_CHROMA_SLICE_BLOCK_EDGES_ALWAYS_FILTERED;
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_H264 gop = {0, 1, 2, 4, 4};

    say("      support at 1920x1080 @ 60:\n");
    j.key("support").beginArray();
    const std::vector<D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC> fullHd = {{1920, 1080}};
    for (const RcCase& c : {kRcCases[1], kRcCases[2], kRcCases[7]}) {
        RcParams params;
        D3D12_VIDEO_ENCODER_PROFILE_H264 suggestedProfile = {};
        D3D12_VIDEO_ENCODER_LEVELS_H264 suggestedLevel = {};
        const SupportAnswer a = askSupport(
            v, fullHd,
            [&](D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT1& s) {
                s.Codec = D3D12_VIDEO_ENCODER_CODEC_H264;
                s.InputFormat = DXGI_FORMAT_NV12;
                s.CodecConfiguration.DataSize = sizeof(cfg);
                s.CodecConfiguration.pH264Config = &cfg;
                s.CodecGopSequence.DataSize = sizeof(gop);
                s.CodecGopSequence.pH264GroupOfPictures = &gop;
                params.fill(c, s.RateControl);
                s.IntraRefresh = c.intraRefresh ? D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_ROW_BASED
                                                : D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_NONE;
                s.SubregionFrameEncoding =
                    D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME;
                s.MaxReferenceFramesInDPB = 1;
                s.SuggestedProfile.DataSize = sizeof(suggestedProfile);
                s.SuggestedProfile.pH264Profile = &suggestedProfile;
                s.SuggestedLevel.DataSize = sizeof(suggestedLevel);
                s.SuggestedLevel.pH264LevelSetting = &suggestedLevel;
            },
            [&] { return h264LevelName(suggestedLevel); });
        printAnswer(c.id, a, j);
    }
    j.endArray();
    j.endObject();
}

void av1(ID3D12VideoDevice3* v, Json& j)
{
    D3D12_VIDEO_ENCODER_AV1_PROFILE profile = D3D12_VIDEO_ENCODER_AV1_PROFILE_MAIN;
    D3D12_VIDEO_ENCODER_PROFILE_DESC pd = {};
    pd.DataSize = sizeof(profile);
    pd.pAV1Profile = &profile;
    j.key("av1").beginObject();
    say("    AV1 Main:");
    for (DXGI_FORMAT f : {DXGI_FORMAT_NV12, DXGI_FORMAT_P010}) {
        D3D12_FEATURE_DATA_VIDEO_ENCODER_INPUT_FORMAT fi = {};
        fi.Codec = D3D12_VIDEO_ENCODER_CODEC_AV1;
        fi.Profile = pd;
        fi.Format = f;
        feature(v, D3D12_FEATURE_VIDEO_ENCODER_INPUT_FORMAT, fi);
        say(" input %s %d", formatName(f), fi.IsSupported);
        j.field(std::string("input") + formatName(f), static_cast<unsigned>(fi.IsSupported));
    }
    say(" (configuration and support: Phase 9)\n");
    outputResolution(v, D3D12_VIDEO_ENCODER_CODEC_AV1, j);
    rateModes(v, D3D12_VIDEO_ENCODER_CODEC_AV1, j);
    j.endObject();
}

void videoEncode(ID3D12Device* d12, Json& j)
{
    j.key("video").beginObject();
    ComPtr<ID3D12VideoDevice3> v;
    if (FAILED(d12->QueryInterface(IID_PPV_ARGS(&v)))) {
        say("  video encode: no ID3D12VideoDevice3 (Windows 10?)\n");
        j.field("videoDevice3", false).endObject();
        return;
    }
    j.field("videoDevice3", true);
    unsigned supported[3] = {};
    say("  video encode codecs:");
    j.key("codecs").beginObject();
    for (auto c : {D3D12_VIDEO_ENCODER_CODEC_H264, D3D12_VIDEO_ENCODER_CODEC_HEVC,
                   D3D12_VIDEO_ENCODER_CODEC_AV1}) {
        D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC fc = {};
        fc.Codec = c;
        feature(v.Get(), D3D12_FEATURE_VIDEO_ENCODER_CODEC, fc);
        const char* name = c == D3D12_VIDEO_ENCODER_CODEC_H264   ? "H264"
                           : c == D3D12_VIDEO_ENCODER_CODEC_HEVC ? "HEVC"
                                                                 : "AV1";
        supported[static_cast<int>(c)] = static_cast<unsigned>(fc.IsSupported);
        say(" %s %d", name, fc.IsSupported);
        j.field(name, static_cast<unsigned>(fc.IsSupported));
    }
    j.endObject();
    say("\n");

    if (supported[D3D12_VIDEO_ENCODER_CODEC_HEVC]) {
        j.key("hevc").beginObject();
        outputResolution(v.Get(), D3D12_VIDEO_ENCODER_CODEC_HEVC, j);
        j.key("profiles").beginArray();
        const HevcProfile profiles[] = {
            {D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN, DXGI_FORMAT_NV12, "main"},
            {D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN10, DXGI_FORMAT_P010, "main10"},
            {D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN_444, DXGI_FORMAT_AYUV, "main444"},
        };
        for (const HevcProfile& p : profiles)
            hevcProfile(v.Get(), p, j);
        j.endArray();
        j.endObject();
    }
    if (supported[D3D12_VIDEO_ENCODER_CODEC_H264]) h264(v.Get(), j);
    if (supported[D3D12_VIDEO_ENCODER_CODEC_AV1]) av1(v.Get(), j);
    j.endObject();
}

// ── One adapter ─────────────────────────────────────────────────────────────

void adapter(const Adapter& a, const Options& options, const GUID& creator, Json& j)
{
    j.beginObject();
    identity(a, j);

    ComPtr<ID3D12Device> d12;
    const HRESULT h12 =
        ::D3D12CreateDevice(a.adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d12));
    j.key("d3d12").beginObject();
    j.field("create", hr(h12));
    if (FAILED(h12)) {
        say("  no D3D12 device: %s\n", hr(h12).c_str());
        j.endObject();
        j.endObject();
        return;
    }
    deviceFeatures(d12.Get(), j);
    j.endObject();
    formats(d12.Get(), j);

    ComPtr<ID3D11Device> d11;
    const HRESULT h11 = ::D3D11CreateDevice(a.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                            D3D11_SDK_VERSION, &d11, nullptr, nullptr);
    j.field("d3d11Create", hr(h11));
    if (SUCCEEDED(h11)) {
        if (options.dda) ddaOpen(a.adapter.Get(), d11.Get(), d12.Get(), j);
        fences(d11.Get(), d12.Get(), options.rounds, j);
    } else {
        say("  no D3D11 device: %s\n", hr(h11).c_str());
    }

    queues(d12.Get(), creator, j);
    videoEncode(d12.Get(), j);
    // Last: a video-encode list that does something the driver dislikes can
    // remove the device, and nothing after it would mean anything.
    timestamps(d12.Get(), options.videoTimestamps, j);
    const HRESULT removed = d12->GetDeviceRemovedReason();
    j.field("deviceRemovedAfter", hr(removed));
    if (FAILED(removed)) say("  !! the D3D12 device was removed: %s\n", hr(removed).c_str());
    j.endObject();
}

bool parse(int argc, wchar_t** argv, Options& o)
{
    for (int i = 0; i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto next = [&]() -> const wchar_t* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (arg == L"--json") {
            const wchar_t* path = next();
            if (!path) return false;
            o.jsonPath = path;
        } else if (arg == L"--no-json") {
            o.json = false;
        } else if (arg == L"--no-privilege") {
            o.privilege = false;
        } else if (arg == L"--software") {
            o.software = true;
        } else if (arg == L"--no-dda") {
            o.dda = false;
        } else if (arg == L"--no-video-timestamps") {
            o.videoTimestamps = false;
        } else if (arg == L"--rounds") {
            const wchar_t* n = next();
            if (!n) return false;
            o.rounds = std::max(1, _wtoi(n));
        } else {
            return false;
        }
    }
    return true;
}

} // namespace

void capsUsage()
{
    std::puts("mw-d3d12-lab caps [options]\n"
              "  What every GPU answers: driver, HAGS, D3D12 features, planar formats, the\n"
              "  DDA surface opened in D3D12, the D3D11<->D3D12 fence handshake, every queue\n"
              "  type at every priority (plain and with a CreatorID), timestamps per queue\n"
              "  type, and the D3D12 Video Encode capabilities (HEVC in full, H.264, AV1).\n"
              "  --json <file>          where the JSON goes (default caps-<machine>-<time>.json)\n"
              "  --no-json              text only\n"
              "  --no-privilege         do not switch SeIncreaseBasePriorityPrivilege on first\n"
              "  --software             include WARP\n"
              "  --no-dda               skip the Desktop Duplication test\n"
              "  --no-video-timestamps  skip the video-encode queue timestamp test\n"
              "  --rounds <n>           fence handshake rounds (default 200)");
}

int runCaps(int argc, wchar_t** argv)
{
    Options options;
    if (!parse(argc, argv, options)) {
        capsUsage();
        return 2;
    }

    const std::string token = tokenKind();
    std::string privilege = "not asked";
    if (options.privilege) privilege = enableBasePriorityPrivilege() ? "enabled" : "not held";

    GUID creator = {};
    ::CoCreateGuid(&creator);

    const std::string os = osBuild();
    const std::string core = d3d12CoreVersion();
    say("mw-d3d12-lab caps - %s on %s\n", nowText().c_str(), computerName().c_str());
    say("Windows %s, D3D12Core %s, SDK %d, token %s, base-priority privilege %s\n\n", os.c_str(),
        core.empty() ? "?" : core.c_str(), D3D12_SDK_VERSION, token.c_str(), privilege.c_str());

    Json j;
    j.beginObject();
    j.field("tool", "mw-d3d12-lab").field("command", "caps").field("schema", 1);
    j.field("date", nowText()).field("computer", computerName());
    j.field("os", os).field("d3d12Core", core).field("sdkVersion", D3D12_SDK_VERSION);
    j.field("token", token).field("basePriorityPrivilege", privilege);
    j.key("adapters").beginArray();
    for (const Adapter& a : adapters(options.software)) {
        adapter(a, options, creator, j);
        say("\n");
    }
    j.endArray();
    j.endObject();

    if (options.json) {
        std::wstring path = options.jsonPath;
        if (path.empty()) path = wide("caps-" + computerName() + "-" + nowStamp() + ".json");
        std::ofstream file(path, std::ios::binary);
        file << j.str() << '\n';
        if (file.good())
            say("JSON: %s\n", utf8(path.c_str()).c_str());
        else
            say("JSON: could not write %s\n", utf8(path.c_str()).c_str());
    }
    return 0;
}

} // namespace lab
