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

// `encode` — D3D12 Video Encode HEVC as the product would drive it (plan
// pipeline-video-d3d12-v2, C1.3; the first attempt's verc.cpp, grown).
//
// An endless GOP of P frames at a steady rate, with the rate control the
// product would use: CBR with a one-frame VBV and a QP range (cbr), constant
// QP (cqp), constant QP plus a per-frame delta-QP map (delta, the in-house
// controller's lever on Intel), or an absolute QP map (absolute). Optional:
// row-based intra refresh, a bitrate step every two seconds (--change), two
// references in the DPB with only the newest used (--refs 2: does the slice's
// RPS keep the other? reference invalidation depends on it), and the
// product's own conversion in front of each frame on a DIRECT queue (--convert
// ps), so the wall time includes the hand-off between the two queues. Wall
// time is submit to bitstream in hand; --dump writes the stream with our
// parameter sets, for ffmpeg to check (and `-bsf:v trace_headers` to read the
// slice headers).
//
// ffmpeg's error count is no proof: over a coded size that ended inside a
// coding tree block, every picture of the Arc's and the AMD iGPU's 1440p
// streams decoded wrong, and ffmpeg flagged a few in a thousand (27/09/2026).
// The pixels are the proof: --dump-input writes the eight input pictures (raw
// NV12, the coded size), frame n being input n % 8, and
// scripts/bench/hevc-psnr.py compares the decoded stream with them, picture by
// picture and band by band. A stream decoded as coded holds the same PSNR from
// its first picture to its last; one that does not falls to noise past the
// first wrong row of blocks.

#include "Encode.h"

#include "HevcHeaders.h"
#include "LabD3d12.h"
#include "Json.h"
#include "Lab.h"

#include "convert/windows/d3d12/ColorConvert12.h"
#include "mw/native/NativeHost.h"
#include "platform/windows/StreamPriority.h"
#include "platform/windows/d3d12/D3d12Device.h"

#include <d3d12video.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace lab {
namespace {

using namespace mw::native;

struct Options
{
    std::string adapter;
    int width = 1920, height = 1080;
    int sourceW = 0, sourceH = 0;
    int fps = 60;
    int seconds = 20;
    std::string rc = "cbr";
    int kbps = 20000;
    int qp = 30;
    int maxQp = 51;
    int qvs = -1;
    int deltaSweep = 0;
    int intraRefresh = 0;
    bool change = false;
    bool flagChange = false;
    int refs = 1;
    std::string convert = "none";
    std::string bitstream = "auto";
    bool checkSizes = false;
    std::string align = "ctb";
    std::wstring dump;
    std::wstring dumpInput;
    std::string gpuClass = "auto";
    std::wstring jsonPath;
    bool json = true;
};

void say(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::fflush(stdout);
}

int levelIdc(D3D12_VIDEO_ENCODER_LEVELS_HEVC level)
{
    static const int kIdc[] = {30, 60, 63, 90, 93, 120, 123, 150, 153, 156, 180, 183, 186};
    const int i = static_cast<int>(level);
    return i >= 0 && i < 13 ? kIdc[i] : 153;
}

struct Stats
{
    double mean = 0, p50 = 0, p99 = 0, max = 0;
};

Stats stats(std::vector<double> v)
{
    Stats s;
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v)
        sum += x;
    s.mean = sum / v.size();
    s.p50 = v[v.size() / 2];
    s.p99 = v[std::min(v.size() - 1, static_cast<size_t>(v.size() * 0.99))];
    s.max = v.back();
    return s;
}

bool parse(int argc, wchar_t** argv, Options& o)
{
    for (int i = 0; i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? utf8(argv[++i]) : std::string(); };
        if (arg == L"--adapter") {
            o.adapter = next();
        } else if (arg == L"--size") {
            if (sscanf_s(next().c_str(), "%dx%d", &o.width, &o.height) != 2) return false;
        } else if (arg == L"--source") {
            if (sscanf_s(next().c_str(), "%dx%d", &o.sourceW, &o.sourceH) != 2) return false;
        } else if (arg == L"--fps") {
            o.fps = std::max(1, std::atoi(next().c_str()));
        } else if (arg == L"--seconds") {
            o.seconds = std::max(1, std::atoi(next().c_str()));
        } else if (arg == L"--rc") {
            o.rc = next();
        } else if (arg == L"--kbps") {
            o.kbps = std::max(100, std::atoi(next().c_str()));
        } else if (arg == L"--qp") {
            o.qp = std::clamp(std::atoi(next().c_str()), 0, 51);
        } else if (arg == L"--qvs") {
            o.qvs = std::clamp(std::atoi(next().c_str()), 0, 15);
        } else if (arg == L"--max-qp") {
            o.maxQp = std::clamp(std::atoi(next().c_str()), 18, 51);
        } else if (arg == L"--delta-sweep") {
            o.deltaSweep = std::clamp(std::atoi(next().c_str()), 0, 20);
        } else if (arg == L"--intra-refresh") {
            o.intraRefresh = std::max(0, std::atoi(next().c_str()));
        } else if (arg == L"--change") {
            o.change = true;
        } else if (arg == L"--flag-change") {
            o.flagChange = true;
        } else if (arg == L"--refs") {
            o.refs = std::clamp(std::atoi(next().c_str()), 1, 4);
        } else if (arg == L"--convert") {
            o.convert = next();
        } else if (arg == L"--bitstream") {
            o.bitstream = next();
        } else if (arg == L"--check-sizes") {
            o.checkSizes = true;
        } else if (arg == L"--align") {
            o.align = next();
        } else if (arg == L"--dump") {
            o.dump = i + 1 < argc ? argv[++i] : L"";
        } else if (arg == L"--dump-input") {
            o.dumpInput = i + 1 < argc ? argv[++i] : L"";
        } else if (arg == L"--class") {
            o.gpuClass = next();
        } else if (arg == L"--json") {
            o.jsonPath = i + 1 < argc ? argv[++i] : L"";
        } else if (arg == L"--no-json") {
            o.json = false;
        } else {
            return false;
        }
    }
    if (o.rc != "cbr" && o.rc != "cqp" && o.rc != "delta" && o.rc != "absolute") return false;
    if (o.convert != "none" && o.convert != "ps") return false;
    if (o.align != "ctb" && o.align != "16" && o.align != "asked") return false;
    if (!o.dumpInput.empty() && o.convert != "none") return false;
    if (!isGpuClassOption(o.gpuClass)) return false;
    if (o.sourceW <= 0) {
        o.sourceW = o.width;
        o.sourceH = o.height;
    }
    return true;
}

} // namespace

void encodeUsage()
{
    std::puts(
        "mw-d3d12-lab encode [options]\n"
        "  D3D12 Video Encode HEVC, an endless GOP of P frames at a steady rate.\n"
        "  --adapter <gpu>         a DXGI index or a piece of the name (RTX, Arc...)\n"
        "  --size WxH              the stream (default 1920x1080)\n"
        "  --fps <n> --seconds <s> (default 60, 20)\n"
        "  --rc cbr|cqp|delta|absolute\n"
        "                          cbr: the product's (one-frame VBV, QP 18-51, frame cap where\n"
        "                          taken); delta: CQP + a delta-QP map; absolute: a QP map\n"
        "  --kbps <n> --qp <n>     target (cbr) or QP (the others); default 20000 and 30\n"
        "  --max-qp <n>            cbr: the top of the QP range (default 51)\n"
        "  --qvs <n>               cbr/cqp/delta: QualityVsSpeed through the EXTENSION1\n"
        "                          structures (0 = fastest); default: the driver's\n"
        "  --delta-sweep <n>       delta/absolute: the map moves by +-n every second\n"
        "  --intra-refresh <n>     row-based intra refresh over n frames\n"
        "  --change                cbr: the target steps x1/4 and back every 2 s\n"
        "  --flag-change           cqp + --delta-sweep: flag the per-second QP change as a\n"
        "                          rate-control change (unflagged without it)\n"
        "  --refs 1|2              references in the DPB, only the newest used\n"
        "  --convert none|ps       ps: ColorConvert12 in front of each frame, DIRECT queue\n"
        "  --source WxH            ps: the desktop size (default the stream's)\n"
        "  --bitstream auto|sysmem|copy\n"
        "  --check-sizes           sysmem: zero the buffer before each frame, then check the\n"
        "                          written size against the data and the subregion metadata\n"
        "  --align ctb|16|asked    the size the driver codes: whole coding tree blocks (the\n"
        "                          product's), whole blocks of 16 or the size as asked; the\n"
        "                          SPS says it and crops (default ctb)\n"
        "  --dump <file.hevc>      the stream with our VPS/SPS/PPS (check with ffmpeg)\n"
        "  --dump-input <file>     --convert none: the eight input pictures, raw NV12 at the\n"
        "                          coded size, for a PSNR check of the decoded stream\n"
        "  --class auto|high|normal the process's GPU scheduling class (auto = REALTIME\n"
        "                          where the token allows it, as the product asks); the\n"
        "                          queues follow it\n"
        "  --json <file> | --no-json");
}

int runEncode(int argc, wchar_t** argv)
{
    Options o;
    if (!parse(argc, argv, o)) {
        encodeUsage();
        return 2;
    }
    NativeHost::setLogSink([](int level, const std::string& message) {
        if (level >= 2) std::printf("    %s\n", message.c_str());
    });

    // The process's GPU class, the product's way; the queues below follow it.
    StreamPriority priority;
    const GpuScheduling scheduling = takeGpuClass(priority, o.gpuClass);

    const std::vector<Adapter> all = adapters(false);
    const Adapter* a = pickAdapter(all, o.adapter);
    if (!a) {
        say("no such adapter\n");
        return 2;
    }
    std::string error;
    std::shared_ptr<d3d12::D3d12Device> device =
        d3d12::D3d12Device::forAdapter(d3d12::luidValue(a->desc.AdapterLuid), error);
    if (!device) {
        say("no D3D12: %s\n", error.c_str());
        return 1;
    }
    ID3D12Device* d = device->device();
    ComPtr<ID3D12VideoDevice3> video;
    if (FAILED(d->QueryInterface(IID_PPV_ARGS(&video)))) {
        say("no ID3D12VideoDevice3 on %s\n", a->name.c_str());
        return 1;
    }
    say("mw-d3d12-lab encode - %s on %s\n%s, driver %s\n", nowText().c_str(),
        computerName().c_str(), a->name.c_str(), umdVersion(a->adapter.Get()).c_str());
    say("token %s, base-priority privilege %s, GPU class %s\n", scheduling.token.c_str(),
        scheduling.privilege ? "enabled" : "not held", scheduling.gpuClass.c_str());

    // ── What the driver takes ──────────────────────────────────────────────
    D3D12_VIDEO_ENCODER_PROFILE_HEVC profile = D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN;
    D3D12_VIDEO_ENCODER_PROFILE_DESC profileDesc = {sizeof(profile), {}};
    profileDesc.pHEVCProfile = &profile;

    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC caps = {};
    D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT ccs = {};
    ccs.Codec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
    ccs.Profile = profileDesc;
    ccs.CodecSupportLimits.DataSize = sizeof(caps);
    ccs.CodecSupportLimits.pHEVCSupport = &caps;
    // Block sizes are proposals the driver accepts or not (as Mesa asks).
    for (int minCu = 0; minCu < 4 && !ccs.IsSupported; ++minCu)
        for (int maxCu = 3; maxCu >= minCu && !ccs.IsSupported; --maxCu)
            for (int minTu = 0; minTu < 4 && !ccs.IsSupported; ++minTu)
                for (int maxTu = 3; maxTu >= minTu && !ccs.IsSupported; --maxTu)
                    for (int depth = 0; depth < 5 && !ccs.IsSupported; ++depth) {
                        caps = {};
                        caps.MinLumaCodingUnitSize =
                            D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE(minCu);
                        caps.MaxLumaCodingUnitSize =
                            D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE(maxCu);
                        caps.MinLumaTransformUnitSize =
                            D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE(minTu);
                        caps.MaxLumaTransformUnitSize =
                            D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE(maxTu);
                        caps.max_transform_hierarchy_depth_inter = static_cast<UCHAR>(depth);
                        caps.max_transform_hierarchy_depth_intra = static_cast<UCHAR>(depth);
                        ccs.IsSupported = FALSE;
                        video->CheckFeatureSupport(
                            D3D12_FEATURE_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT, &ccs,
                            sizeof(ccs));
                    }
    if (!ccs.IsSupported) {
        say("no HEVC configuration accepted\n");
        return 1;
    }
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
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION codecCfg = {sizeof(cfg), {}};
    codecCfg.pHEVCConfig = &cfg;

    // The product's sequence: endless, P only, POC in 8 bits.
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_HEVC gop = {0, 1, 4};
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE gopDesc = {sizeof(gop), {}};
    gopDesc.pHEVCGroupOfPictures = &gop;

    const UINT64 bps = static_cast<UINT64>(o.kbps) * 1000;
    D3D12_VIDEO_ENCODER_RATE_CONTROL_CQP cqp = {static_cast<UINT>(o.qp), static_cast<UINT>(o.qp),
                                                static_cast<UINT>(o.qp)};
    D3D12_VIDEO_ENCODER_RATE_CONTROL_CBR cbr = {};
    cbr.InitialQP = 30;
    cbr.MinQP = 18;
    cbr.MaxQP = static_cast<UINT>(o.maxQp);
    cbr.TargetBitRate = bps;
    cbr.VBVCapacity = bps / static_cast<UINT64>(o.fps);
    cbr.InitialVBVFullness = cbr.VBVCapacity;
    cbr.MaxFrameBitSize = cbr.VBVCapacity;
    D3D12_VIDEO_ENCODER_RATE_CONTROL_ABSOLUTE_QP_MAP absolute = {0};
    // The EXTENSION1 twins, which carry QualityVsSpeed (--qvs): refreshed from
    // cbr and cqp before every frame, so a rate step moves them too.
    const bool ext1 = o.qvs >= 0 && o.rc != "absolute";
    D3D12_VIDEO_ENCODER_RATE_CONTROL_CBR1 cbr1 = {};
    D3D12_VIDEO_ENCODER_RATE_CONTROL_CQP1 cqp1 = {};
    const auto syncExt1 = [&] {
        cbr1.InitialQP = cbr.InitialQP;
        cbr1.MinQP = cbr.MinQP;
        cbr1.MaxQP = cbr.MaxQP;
        cbr1.MaxFrameBitSize = cbr.MaxFrameBitSize;
        cbr1.TargetBitRate = cbr.TargetBitRate;
        cbr1.VBVCapacity = cbr.VBVCapacity;
        cbr1.InitialVBVFullness = cbr.InitialVBVFullness;
        cbr1.QualityVsSpeed = static_cast<UINT>(std::max(0, o.qvs));
        cqp1.ConstantQP_FullIntracodedFrame = cqp.ConstantQP_FullIntracodedFrame;
        cqp1.ConstantQP_InterPredictedFrame_PrevRefOnly =
            cqp.ConstantQP_InterPredictedFrame_PrevRefOnly;
        cqp1.ConstantQP_InterPredictedFrame_BiDirectionalRef =
            cqp.ConstantQP_InterPredictedFrame_BiDirectionalRef;
        cqp1.QualityVsSpeed = static_cast<UINT>(std::max(0, o.qvs));
    };
    syncExt1();
    const D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAGS ext1Flags =
        ext1 ? D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_EXTENSION1_SUPPORT |
                   D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_QUALITY_VS_SPEED
             : D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_NONE;
    D3D12_VIDEO_ENCODER_RATE_CONTROL rc = {};
    rc.TargetFrameRate = {static_cast<UINT>(o.fps), 1};
    std::vector<D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAGS> flagChain;
    if (o.rc == "cbr") {
        rc.Mode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CBR;
        if (ext1) {
            rc.ConfigParams.DataSize = sizeof(cbr1);
            rc.ConfigParams.pConfiguration_CBR1 = &cbr1;
        } else {
            rc.ConfigParams.DataSize = sizeof(cbr);
            rc.ConfigParams.pConfiguration_CBR = &cbr;
        }
        // Everything the product asks, then less until the driver says yes.
        const auto vbv = D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_VBV_SIZES;
        const auto range = D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_QP_RANGE;
        const auto cap = D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_MAX_FRAME_SIZE;
        flagChain = {vbv | range | cap | ext1Flags, vbv | range | ext1Flags, vbv | ext1Flags};
    } else if (o.rc == "absolute") {
        rc.Mode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_ABSOLUTE_QP_MAP;
        rc.ConfigParams.DataSize = sizeof(absolute);
        rc.ConfigParams.pConfiguration_AbsoluteQPMap = &absolute;
        flagChain = {D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_NONE};
    } else {
        rc.Mode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CQP;
        if (ext1) {
            rc.ConfigParams.DataSize = sizeof(cqp1);
            rc.ConfigParams.pConfiguration_CQP1 = &cqp1;
        } else {
            rc.ConfigParams.DataSize = sizeof(cqp);
            rc.ConfigParams.pConfiguration_CQP = &cqp;
        }
        flagChain = {(o.rc == "delta" ? D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_DELTA_QP
                                      : D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_NONE) |
                     ext1Flags};
    }

    const D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE refreshMode =
        o.intraRefresh > 0 ? D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_ROW_BASED
                           : D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_NONE;
    // The size the driver codes: whole coding tree blocks, as the product
    // codes (HevcEncodeNegotiation.h). The drivers code every CTB whole
    // whatever size they accept, so --align 16 or asked shows what an SPS
    // that ends inside one does to the pictures.
    const UINT unit = o.align == "ctb"  ? 8u << cfg.MaxLumaCodingUnitSize
                      : o.align == "16" ? 16u
                                        : 1u;
    D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC res = {
        (static_cast<UINT>(o.width) + unit - 1) / unit * unit,
        (static_cast<UINT>(o.height) + unit - 1) / unit * unit};
    // SUPPORT1 (it knows QualityVsSpeed), the original SUPPORT where a runtime
    // or driver refuses it: the same structure without its last members.
    D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT1 sup = {};
    D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_LIMITS limits = {};
    D3D12_VIDEO_ENCODER_PROFILE_HEVC suggestedProfile = {};
    D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC suggestedLevel = {};
    bool ok = false;
    for (D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAGS flags : flagChain) {
        rc.Flags = flags;
        sup = {};
        sup.Codec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
        sup.InputFormat = DXGI_FORMAT_NV12;
        sup.CodecConfiguration = codecCfg;
        sup.CodecGopSequence = gopDesc;
        sup.RateControl = rc;
        sup.IntraRefresh = refreshMode;
        sup.SubregionFrameEncoding = D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME;
        sup.ResolutionsListCount = 1;
        sup.pResolutionList = &res;
        sup.MaxReferenceFramesInDPB = static_cast<UINT>(o.refs);
        sup.SuggestedProfile.DataSize = sizeof(suggestedProfile);
        sup.SuggestedProfile.pHEVCProfile = &suggestedProfile;
        sup.SuggestedLevel.DataSize = sizeof(suggestedLevel);
        sup.SuggestedLevel.pHEVCLevelSetting = &suggestedLevel;
        sup.pResolutionDependentSupport = &limits;
        HRESULT asked =
            video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_SUPPORT1, &sup, sizeof(sup));
        if (FAILED(asked))
            asked = video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_SUPPORT, &sup,
                                               sizeof(D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT));
        if (SUCCEEDED(asked) &&
            (sup.SupportFlags & D3D12_VIDEO_ENCODER_SUPPORT_FLAG_GENERAL_SUPPORT_OK)) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        say("the driver takes none of it at %ux%u (validation 0x%x)\n", res.Width, res.Height,
            sup.ValidationFlags);
        return 1;
    }
    const int codedW = static_cast<int>(res.Width);
    const int codedH = static_cast<int>(res.Height);
    const bool reconfigurable =
        (sup.SupportFlags &
         D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RATE_CONTROL_RECONFIGURATION_AVAILABLE) != 0;
    const bool reconArrays =
        (sup.SupportFlags &
         D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RECONSTRUCTED_FRAMES_REQUIRE_TEXTURE_ARRAYS) != 0;
    const UINT region = limits.QPMapRegionPixelsSize ? limits.QPMapRegionPixelsSize : 16;
    say("coded %dx%d (CTB %u, --align %s), rc %s flags 0x%x, support 0x%x, qp-map region %u px, "
        "recon %s, rate reconfiguration %s, quality vs speed <= %u%s\n",
        codedW, codedH, 8u << cfg.MaxLumaCodingUnitSize, o.align.c_str(), o.rc.c_str(),
        static_cast<unsigned>(rc.Flags), sup.SupportFlags, region,
        reconArrays ? "texture array" : "textures", reconfigurable ? "yes" : "no",
        sup.MaxQualityVsSpeed, ext1 ? (" (asked " + std::to_string(o.qvs) + ")").c_str() : "");

    D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOURCE_REQUIREMENTS req = {};
    req.Codec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
    req.Profile = profileDesc;
    req.InputFormat = DXGI_FORMAT_NV12;
    req.PictureTargetResolution = res;
    video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_RESOURCE_REQUIREMENTS, &req,
                               sizeof(req));

    // ── Encoder, heap, queues ──────────────────────────────────────────────
    D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC level = suggestedLevel;
    D3D12_VIDEO_ENCODER_LEVEL_SETTING levelDesc = {sizeof(level), {}};
    levelDesc.pHEVCLevelSetting = &level;
    D3D12_VIDEO_ENCODER_DESC ed = {};
    ed.EncodeCodec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
    ed.EncodeProfile = profileDesc;
    ed.InputFormat = DXGI_FORMAT_NV12;
    ed.CodecConfiguration = codecCfg;
    ed.MaxMotionEstimationPrecision = D3D12_VIDEO_ENCODER_MOTION_ESTIMATION_PRECISION_MODE_MAXIMUM;
    ComPtr<ID3D12VideoEncoder> encoder;
    D3D12_VIDEO_ENCODER_HEAP_DESC hd = {};
    hd.EncodeCodec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
    hd.EncodeProfile = profileDesc;
    hd.EncodeLevel = levelDesc;
    hd.ResolutionsListCount = 1;
    hd.pResolutionList = &res;
    ComPtr<ID3D12VideoEncoderHeap> heap;
    HRESULT h = video->CreateVideoEncoder(&ed, IID_PPV_ARGS(&encoder));
    if (SUCCEEDED(h)) h = video->CreateVideoEncoderHeap(&hd, IID_PPV_ARGS(&heap));
    if (FAILED(h)) {
        say("encoder or heap refused: %s\n", hr(h).c_str());
        return 1;
    }
    d3d12::Queue encodeQueue;
    d3d12::QueueRequest request;
    request.type = D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE;
    request.priority = d3d12::QueuePriority::Auto;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12VideoEncodeCommandList2> list;
    if (!device->createQueue(request, encodeQueue, error) ||
        FAILED(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE,
                                         IID_PPV_ARGS(&allocator))) ||
        FAILED(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE, allocator.Get(),
                                    nullptr, IID_PPV_ARGS(&list))) ||
        FAILED(list->Close())) {
        say("video-encode queue or list: %s\n", error.c_str());
        return 1;
    }
    say("%s\n", encodeQueue.description.c_str());
    Direct direct;
    if (!direct.init(*device, error)) {
        say("direct queue: %s\n", error.c_str());
        return 1;
    }

    // ── Inputs ─────────────────────────────────────────────────────────────
    constexpr int kFrames = 8;
    std::vector<ComPtr<ID3D12Resource>> inputs, desktops;
    convert::ColorConvert12 converter;
    d3d12::GpuFence converted;
    if (o.convert == "none") {
        std::ofstream inputDump;
        if (!o.dumpInput.empty()) inputDump.open(o.dumpInput, std::ios::binary);
        for (int f = 0; f < kFrames; ++f) {
            ComPtr<ID3D12Resource> t =
                makeTexture(d, static_cast<UINT>(codedW), static_cast<UINT>(codedH), 1,
                            DXGI_FORMAT_NV12, D3D12_RESOURCE_FLAG_NONE);
            const std::vector<std::vector<uint8_t>> planes = nv12Frame(codedW, codedH, f);
            if (!t || !upload(d, direct, t.Get(), planes, error)) {
                say("input frames: %s\n", error.c_str());
                return 1;
            }
            for (const std::vector<uint8_t>& plane : planes)
                inputDump.write(reinterpret_cast<const char*>(plane.data()),
                                static_cast<std::streamsize>(plane.size()));
            inputs.push_back(t);
        }
    } else {
        for (int f = 0; f < kFrames; ++f) {
            ComPtr<ID3D12Resource> t =
                makeTexture(d, static_cast<UINT>(o.sourceW), static_cast<UINT>(o.sourceH), 1,
                            DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
            if (!t || !upload(d, direct, t.Get(), {desktopFrame(o.sourceW, o.sourceH, f)}, error)) {
                say("desktop frames: %s\n", error.c_str());
                return 1;
            }
            desktops.push_back(t);
        }
        if (!converter.init(d, direct.queue.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, o.sourceW, o.sourceH,
                            o.width, o.height, codedW, codedH, false,
                            convert::ScaleFilter::Lanczos2, error) ||
            !converted.create(d, false, error)) {
            say("conversion: %s\n", error.c_str());
            return 1;
        }
    }

    // ── Outputs ────────────────────────────────────────────────────────────
    const UINT reconCount = static_cast<UINT>(o.refs + 1);
    const D3D12_RESOURCE_FLAGS reconFlags =
        (sup.SupportFlags &
         D3D12_VIDEO_ENCODER_SUPPORT_FLAG_READABLE_RECONSTRUCTED_PICTURE_LAYOUT_AVAILABLE)
            ? D3D12_RESOURCE_FLAG_NONE
            : D3D12_RESOURCE_FLAG_VIDEO_ENCODE_REFERENCE_ONLY |
                  D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    std::vector<ComPtr<ID3D12Resource>> recon;
    if (reconArrays) {
        recon.push_back(makeTexture(d, static_cast<UINT>(codedW), static_cast<UINT>(codedH),
                                    static_cast<UINT16>(reconCount), DXGI_FORMAT_NV12, reconFlags));
    } else {
        for (UINT i = 0; i < reconCount; ++i)
            recon.push_back(makeTexture(d, static_cast<UINT>(codedW), static_cast<UINT>(codedH), 1,
                                        DXGI_FORMAT_NV12, reconFlags));
    }
    for (const auto& r : recon)
        if (!r) {
            say("reconstructed pictures refused\n");
            return 1;
        }
    const auto reconRes = [&](UINT i) { return reconArrays ? recon[0].Get() : recon[i].Get(); };
    const auto reconSub = [&](UINT i) { return reconArrays ? i : 0u; };

    const UINT64 bitstreamSize = 8ull << 20;
    const UINT64 metaSize = std::max<UINT64>(4096, req.MaxEncoderOutputMetadataBufferSize);
    bool sysmem = o.bitstream != "copy";
    ComPtr<ID3D12Resource> bitstream, meta;
    if (sysmem) {
        bitstream = makeBuffer(d, bitstreamSize, D3D12_HEAP_TYPE_DEFAULT, true,
                               D3D12_RESOURCE_STATE_COMMON);
        meta = makeBuffer(d, 4096, D3D12_HEAP_TYPE_DEFAULT, true, D3D12_RESOURCE_STATE_COMMON);
        if (!bitstream || !meta) {
            if (o.bitstream == "sysmem") {
                say("no system-memory buffers for the video engine\n");
                return 1;
            }
            sysmem = false;
        }
    }
    ComPtr<ID3D12Resource> bitstreamBack, metaBack;
    if (!sysmem) {
        bitstream = makeBuffer(d, bitstreamSize, D3D12_HEAP_TYPE_DEFAULT, false,
                               D3D12_RESOURCE_STATE_COMMON);
        meta = makeBuffer(d, 4096, D3D12_HEAP_TYPE_DEFAULT, false, D3D12_RESOURCE_STATE_COMMON);
        bitstreamBack = makeBuffer(d, bitstreamSize, D3D12_HEAP_TYPE_READBACK, false,
                                   D3D12_RESOURCE_STATE_COPY_DEST);
        metaBack =
            makeBuffer(d, 4096, D3D12_HEAP_TYPE_READBACK, false, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    ComPtr<ID3D12Resource> hwMeta =
        makeBuffer(d, metaSize, D3D12_HEAP_TYPE_DEFAULT, false, D3D12_RESOURCE_STATE_COMMON);
    if (!bitstream || !meta || !hwMeta || (!sysmem && (!bitstreamBack || !metaBack))) {
        say("output buffers refused\n");
        return 1;
    }
    uint8_t* bitstreamCpu = nullptr;
    uint8_t* metaCpu = nullptr;
    if (sysmem) {
        bitstream->Map(0, nullptr, reinterpret_cast<void**>(&bitstreamCpu));
        meta->Map(0, nullptr, reinterpret_cast<void**>(&metaCpu));
    }
    say("bitstream: %s\n", sysmem ? "system memory, read in place" : "video memory + copy");

    // The copy path's COPY queue: bitstream and metadata to readback.
    d3d12::Queue copyQueue;
    ComPtr<ID3D12CommandAllocator> copyAllocator;
    ComPtr<ID3D12GraphicsCommandList> copyList;
    d3d12::GpuFence copied;
    if (!sysmem) {
        d3d12::QueueRequest cr;
        cr.type = D3D12_COMMAND_LIST_TYPE_COPY;
        cr.priority = d3d12::QueuePriority::Auto;
        if (!device->createQueue(cr, copyQueue, error) ||
            FAILED(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                                             IID_PPV_ARGS(&copyAllocator))) ||
            FAILED(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, copyAllocator.Get(),
                                        nullptr, IID_PPV_ARGS(&copyList))) ||
            FAILED(copyList->Close()) || !copied.create(d, false, error)) {
            say("copy queue: %s\n", error.c_str());
            return 1;
        }
    }

    // The QP map, one INT8 per region.
    const size_t mapW = (static_cast<size_t>(codedW) + region - 1) / region;
    const size_t mapH = (static_cast<size_t>(codedH) + region - 1) / region;
    std::vector<INT8> qpMap(o.rc == "delta" || o.rc == "absolute" ? mapW * mapH : 0);

    HevcShape shape;
    shape.width = o.width;
    shape.height = o.height;
    // The size the driver was asked to code, cropped to the size asked.
    shape.codedWidth = codedW;
    shape.codedHeight = codedH;
    shape.levelIdc = levelIdc(level.Level);
    shape.log2MinCodingBlock = 3 + static_cast<int>(cfg.MinLumaCodingUnitSize);
    shape.log2MaxCodingBlock = 3 + static_cast<int>(cfg.MaxLumaCodingUnitSize);
    shape.log2MinTransformBlock = 2 + static_cast<int>(cfg.MinLumaTransformUnitSize);
    shape.log2MaxTransformBlock = 2 + static_cast<int>(cfg.MaxLumaTransformUnitSize);
    shape.transformDepthInter = cfg.max_transform_hierarchy_depth_inter;
    shape.transformDepthIntra = cfg.max_transform_hierarchy_depth_intra;
    shape.asymmetricMotionPartitions =
        (cfg.ConfigurationFlags &
         D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_USE_ASYMETRIC_MOTION_PARTITION) != 0;
    shape.log2MaxPicOrderCntLsb = 4 + gop.log2_max_pic_order_cnt_lsb_minus4;
    shape.decodedPictureBuffer = static_cast<int>(reconCount);
    const std::vector<uint8_t> parameterSets = hevcParameterSets(shape);
    std::ofstream dump;
    if (!o.dump.empty()) dump.open(o.dump, std::ios::binary);

    // ── The loop ───────────────────────────────────────────────────────────
    d3d12::GpuFence encoded;
    if (!encoded.create(d, false, error)) {
        say("fence: %s\n", error.c_str());
        return 1;
    }
    // A steady rate on a high-resolution timer; Sleep() alone rounds to the
    // scheduler's tick.
    HANDLE timer = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                            TIMER_ALL_ACCESS);
    std::vector<double> wallP, wallIdr, bytesP, qps;
    std::vector<std::pair<double, double>> steps; // (target Mbps, measured Mbps) per second
    std::vector<uint8_t> host(bitstreamSize);
    int errors = 0, frames = 0;
    UINT64 windowBytes = 0;
    UINT lastIdr = 0;
    UINT64 target = bps;
    const int64_t periodUs = 1000000 / o.fps;
    int64_t next = nowUs();
    const int total = o.fps * o.seconds;
    // Late frames stretch the run; under load, past the load's own end. So
    // --seconds is a deadline too (one period of slack), and a late run ends
    // with fewer frames.
    const int64_t deadline = next + static_cast<int64_t>(o.seconds) * 1000000 + periodUs;
    int cut = 0;
    for (int n = 0; n < total; ++n) {
        if (nowUs() >= deadline) {
            cut = total - n;
            break;
        }
        next += periodUs;
        const UINT poc = static_cast<UINT>(n) - lastIdr;
        const bool isIdr = poc == 0;
        const UINT cur = static_cast<UINT>(n) % reconCount;

        // The rate step, on the frame that opens a new two-second window.
        bool changed = false;
        if (o.change && o.rc == "cbr" && n > 0 && n % (2 * o.fps) == 0) {
            target = target == bps ? bps / 4 : bps;
            cbr.TargetBitRate = target;
            cbr.VBVCapacity = target / static_cast<UINT64>(o.fps);
            cbr.InitialVBVFullness = cbr.VBVCapacity;
            cbr.MaxFrameBitSize = cbr.VBVCapacity;
            changed = true;
        }
        // CQP moved by the second, as the in-house controller would move it
        // where the driver takes no QP map: flagged or not (--flag-change).
        if (o.rc == "cqp" && o.deltaSweep && n > 0 && n % o.fps == 0) {
            const int swing = (((n / o.fps) % 3) - 1) * o.deltaSweep;
            const UINT q = static_cast<UINT>(std::clamp(o.qp + swing, 1, 51));
            cqp = {q, q, q};
            changed = o.flagChange;
        }
        if (!qpMap.empty()) {
            const int second = n / o.fps;
            const int swing = o.deltaSweep ? ((second % 3) - 1) * o.deltaSweep : 0;
            const int value = o.rc == "absolute" ? std::clamp(o.qp + swing, 0, 51) : swing;
            std::fill(qpMap.begin(), qpMap.end(), static_cast<INT8>(value));
        }

        // The written size is only checkable against a buffer that holds
        // nothing else: the start of it zeroed before each frame.
        if (sysmem && o.checkSizes) std::memset(bitstreamCpu, 0, 2u << 20);
        const int64_t started = nowUs();
        ID3D12Resource* input = nullptr;
        uint64_t convertedValue = 0;
        if (o.convert == "ps") {
            ID3D12GraphicsCommandList* l = direct.begin();
            converter.recordConvert(l, desktops[n % kFrames].Get(), capture::CursorState{},
                                    convert::CursorDraw{}, error);
            l->Close();
            ID3D12CommandList* lists[] = {l};
            direct.queue->ExecuteCommandLists(1, lists);
            convertedValue = converted.signal(direct.queue.Get(), error);
            input = converter.output();
        } else {
            input = inputs[n % kFrames].Get();
        }

        allocator->Reset();
        list->Reset(allocator.Get());
        std::vector<D3D12_RESOURCE_BARRIER> pre = {
            transition(input, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ),
            transition(bitstream.Get(), D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE),
            transition(hwMeta.Get(), D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE),
        };
        const UINT available = std::min<UINT>(static_cast<UINT>(o.refs), poc);
        std::vector<ID3D12Resource*> refTextures;
        std::vector<UINT> refSubresources;
        std::vector<D3D12_VIDEO_ENCODER_REFERENCE_PICTURE_DESCRIPTOR_HEVC> descriptors;
        const auto both = [&](UINT picture, D3D12_RESOURCE_STATES state) {
            if (reconArrays) {
                pre.push_back(transition(recon[0].Get(), D3D12_RESOURCE_STATE_COMMON, state,
                                         reconSub(picture)));
                pre.push_back(transition(recon[0].Get(), D3D12_RESOURCE_STATE_COMMON, state,
                                         reconSub(picture) + reconCount));
            } else {
                pre.push_back(transition(reconRes(picture), D3D12_RESOURCE_STATE_COMMON, state));
            }
        };
        both(cur, D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE);
        for (UINT k = 0; k < available; ++k) {
            const UINT picture = (static_cast<UINT>(n) - 1 - k) % reconCount;
            both(picture, D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ);
            refTextures.push_back(reconRes(picture));
            refSubresources.push_back(reconSub(picture));
            D3D12_VIDEO_ENCODER_REFERENCE_PICTURE_DESCRIPTOR_HEVC ref = {};
            ref.ReconstructedPictureResourceIndex = k;
            // Only the newest predicts; an older one stays in the DPB, marked
            // unused — whether the driver keeps it in the slice's RPS is what
            // --refs 2 asks.
            ref.IsRefUsedByCurrentPic = k == 0;
            ref.IsLongTermReference = FALSE;
            ref.PictureOrderCountNumber = poc - 1 - k;
            descriptors.push_back(ref);
        }
        list->ResourceBarrier(static_cast<UINT>(pre.size()), pre.data());

        UINT list0[1] = {0};
        D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC pic = {};
        pic.FrameType = isIdr ? D3D12_VIDEO_ENCODER_FRAME_TYPE_HEVC_IDR_FRAME
                              : D3D12_VIDEO_ENCODER_FRAME_TYPE_HEVC_P_FRAME;
        pic.PictureOrderCountNumber = poc;
        if (!isIdr) {
            pic.List0ReferenceFramesCount = 1;
            pic.pList0ReferenceFrames = list0;
            pic.ReferenceFramesReconPictureDescriptorsCount = static_cast<UINT>(descriptors.size());
            pic.pReferenceFramesReconPictureDescriptors = descriptors.data();
        }
        if (!qpMap.empty()) {
            pic.QPMapValuesCount = static_cast<UINT>(qpMap.size());
            pic.pRateControlQPMap = qpMap.data();
        }

        syncExt1();
        D3D12_VIDEO_ENCODER_ENCODEFRAME_INPUT_ARGUMENTS in = {};
        in.SequenceControlDesc.Flags =
            changed ? D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_FLAG_RATE_CONTROL_CHANGE
                    : D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_FLAG_NONE;
        in.SequenceControlDesc.IntraRefreshConfig = {refreshMode,
                                                     static_cast<UINT>(o.intraRefresh)};
        in.SequenceControlDesc.RateControl = rc;
        in.SequenceControlDesc.PictureTargetResolution = res;
        in.SequenceControlDesc.SelectedLayoutMode =
            D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME;
        in.SequenceControlDesc.CodecGopSequence = gopDesc;
        in.PictureControlDesc.IntraRefreshFrameIndex =
            o.intraRefresh > 0 ? static_cast<UINT>(n % o.intraRefresh) : 0;
        in.PictureControlDesc.Flags =
            D3D12_VIDEO_ENCODER_PICTURE_CONTROL_FLAG_USED_AS_REFERENCE_PICTURE;
        in.PictureControlDesc.PictureControlCodecData.DataSize = sizeof(pic);
        in.PictureControlDesc.PictureControlCodecData.pHEVCPicData = &pic;
        if (!isIdr) {
            in.PictureControlDesc.ReferenceFrames.NumTexture2Ds =
                static_cast<UINT>(refTextures.size());
            in.PictureControlDesc.ReferenceFrames.ppTexture2Ds = refTextures.data();
            in.PictureControlDesc.ReferenceFrames.pSubresources =
                reconArrays ? refSubresources.data() : nullptr;
        }
        in.pInputFrame = input;
        in.InputFrameSubresource = 0;
        D3D12_VIDEO_ENCODER_ENCODEFRAME_OUTPUT_ARGUMENTS out = {};
        out.Bitstream = {bitstream.Get(), 0};
        out.ReconstructedPicture = {reconRes(cur), reconSub(cur)};
        out.EncoderOutputMetadata = {hwMeta.Get(), 0};
        list->EncodeFrame(encoder.Get(), heap.Get(), &in, &out);

        D3D12_RESOURCE_BARRIER mid[2] = {
            transition(hwMeta.Get(), D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE,
                       D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ),
            transition(meta.Get(), D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE),
        };
        list->ResourceBarrier(2, mid);
        D3D12_VIDEO_ENCODER_RESOLVE_METADATA_INPUT_ARGUMENTS rin = {};
        rin.EncoderCodec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
        rin.EncoderProfile = profileDesc;
        rin.EncoderInputFormat = DXGI_FORMAT_NV12;
        rin.EncodedPictureEffectiveResolution = res;
        rin.HWLayoutMetadata = {hwMeta.Get(), 0};
        D3D12_VIDEO_ENCODER_RESOLVE_METADATA_OUTPUT_ARGUMENTS rout = {};
        rout.ResolvedLayoutMetadata = {meta.Get(), 0};
        list->ResolveEncoderOutputMetadata(&rin, &rout);
        std::vector<D3D12_RESOURCE_BARRIER> post;
        for (D3D12_RESOURCE_BARRIER b : pre) {
            std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
            if (b.Transition.pResource == hwMeta.Get())
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ;
            post.push_back(b);
        }
        post.push_back(transition(meta.Get(), D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE,
                                  D3D12_RESOURCE_STATE_COMMON));
        list->ResourceBarrier(static_cast<UINT>(post.size()), post.data());
        if (FAILED(h = list->Close())) {
            say("frame %d: the encode list was refused (%s)\n", n, hr(h).c_str());
            device->relayMessages(); // MW_D3D12_DEBUG=1: what the layer said about it
            return 1;
        }

        if (convertedValue) converted.gpuWait(encodeQueue.queue.Get(), convertedValue, error);
        ID3D12CommandList* lists[] = {list.Get()};
        encodeQueue.queue->ExecuteCommandLists(1, lists);
        uint64_t value = encoded.signal(encodeQueue.queue.Get(), error);
        const uint8_t* metaBytes = metaCpu;
        const uint8_t* streamBytes = bitstreamCpu;
        if (!sysmem) {
            copyAllocator->Reset();
            copyList->Reset(copyAllocator.Get(), nullptr);
            copyList->CopyBufferRegion(metaBack.Get(), 0, meta.Get(), 0, 4096);
            copyList->CopyBufferRegion(bitstreamBack.Get(), 0, bitstream.Get(), 0, 2ull << 20);
            copyList->Close();
            encoded.gpuWait(copyQueue.queue.Get(), value, error);
            ID3D12CommandList* copies[] = {copyList.Get()};
            copyQueue.queue->ExecuteCommandLists(1, copies);
            value = copied.signal(copyQueue.queue.Get(), error);
        }
        d3d12::GpuFence& last = sysmem ? encoded : copied;
        const d3d12::GpuFence::Wait waited = last.wait(value, 1000, error);
        if (waited != d3d12::GpuFence::Wait::Done) {
            say("frame %d: %s\n", n, error.c_str());
            return 1;
        }
        void* mappedMeta = nullptr;
        void* mappedStream = nullptr;
        if (!sysmem) {
            const D3D12_RANGE mr = {0, 4096}, br = {0, static_cast<SIZE_T>(2ull << 20)};
            metaBack->Map(0, &mr, &mappedMeta);
            bitstreamBack->Map(0, &br, &mappedStream);
            metaBytes = static_cast<const uint8_t*>(mappedMeta);
            streamBytes = static_cast<const uint8_t*>(mappedStream);
        }
        const auto* md = reinterpret_cast<const D3D12_VIDEO_ENCODER_OUTPUT_METADATA*>(metaBytes);
        const UINT64 bytes = md->EncodedBitstreamWrittenBytesCount;
        const UINT64 averageQp = md->EncodeStats.AverageQP;
        if (sysmem && o.checkSizes && bytes <= (2ull << 20)) {
            // Past the count, the driver should have written nothing; and the
            // subregion (one, full frame) should say the same size.
            size_t lastByte = 0;
            for (size_t k = 0; k < (2u << 20); ++k)
                if (bitstreamCpu[k]) lastByte = k + 1;
            const auto* sub = reinterpret_cast<const D3D12_VIDEO_ENCODER_FRAME_SUBREGION_METADATA*>(
                metaBytes + sizeof(D3D12_VIDEO_ENCODER_OUTPUT_METADATA));
            if (lastByte > bytes || md->WrittenSubregionsCount != 1 || sub->bSize != bytes ||
                sub->bStartOffset != 0)
                say("  frame %d: %llu bytes counted, data up to %zu, %llu subregion(s), first "
                    "size %llu at %llu, header %llu\n",
                    n, static_cast<unsigned long long>(bytes), lastByte,
                    static_cast<unsigned long long>(md->WrittenSubregionsCount),
                    static_cast<unsigned long long>(sub->bSize),
                    static_cast<unsigned long long>(sub->bStartOffset),
                    static_cast<unsigned long long>(sub->bHeaderSize));
        }
        const bool bad = md->EncodeErrorFlags != 0 || bytes == 0 || bytes > (2ull << 20);
        if (!bad) std::memcpy(host.data(), streamBytes, static_cast<size_t>(bytes));
        if (!sysmem) {
            const D3D12_RANGE none = {0, 0};
            metaBack->Unmap(0, &none);
            bitstreamBack->Unmap(0, &none);
        }
        const double wall = static_cast<double>(nowUs() - started) / 1000.0;
        ++frames;
        if (bad) {
            if (errors++ < 5)
                say("frame %d: error flags 0x%llx, %llu bytes\n", n,
                    static_cast<unsigned long long>(md->EncodeErrorFlags),
                    static_cast<unsigned long long>(bytes));
        } else {
            if (dump.is_open()) {
                if (isIdr)
                    dump.write(reinterpret_cast<const char*>(parameterSets.data()),
                               static_cast<std::streamsize>(parameterSets.size()));
                dump.write(reinterpret_cast<const char*>(host.data()),
                           static_cast<std::streamsize>(bytes));
            }
            (isIdr ? wallIdr : wallP).push_back(wall);
            if (!isIdr) bytesP.push_back(static_cast<double>(bytes));
            qps.push_back(static_cast<double>(averageQp));
            windowBytes += bytes;
        }
        if ((n + 1) % o.fps == 0) {
            const double mbps = static_cast<double>(windowBytes) * 8.0 / 1e6;
            steps.push_back({static_cast<double>(target) / 1e6, mbps});
            windowBytes = 0;
        }
        if (n < 3 || changed)
            say("  frame %d %s: %llu bytes, QP %llu, %.2f ms%s\n", n, isIdr ? "IDR" : "P",
                static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(averageQp),
                wall, changed ? " (rate step)" : "");

        const int64_t now = nowUs();
        if (next > now) {
            LARGE_INTEGER due = {};
            due.QuadPart = -(next - now) * 10;
            if (timer && ::SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
                ::WaitForSingleObject(timer, INFINITE);
            else
                ::Sleep(static_cast<DWORD>((next - now) / 1000));
        } else {
            next = now;
        }
    }
    if (timer) ::CloseHandle(timer);

    const Stats p = stats(wallP), i = stats(wallIdr), b = stats(bytesP), q = stats(qps);
    say("\n%d frames, %d errors\n", frames, errors);
    if (cut > 0) say("out of time: %d frames not encoded\n", cut);
    say("wall ms, P:   mean %.2f  p50 %.2f  p99 %.2f  max %.2f\n", p.mean, p.p50, p.p99, p.max);
    say("wall ms, IDR: mean %.2f  max %.2f (%zu)\n", i.mean, i.max, wallIdr.size());
    say("P frame bytes: mean %.0f  p99 %.0f  max %.0f;  average QP %.1f (p99 %.0f)\n", b.mean,
        b.p99, b.max, q.mean, q.p99);
    say("Mbit/s per second:");
    for (const auto& s : steps)
        say(" %.1f/%.1f", s.second, s.first);
    say("  (measured/target)\n");

    if (o.json) {
        Json j;
        j.beginObject();
        j.field("tool", "mw-d3d12-lab").field("command", "encode").field("schema", 1);
        j.field("date", nowText()).field("computer", computerName()).field("gpu", a->name);
        j.field("driver", umdVersion(a->adapter.Get()));
        j.field("token", scheduling.token).field("basePriorityPrivilege", scheduling.privilege);
        j.field("gpuClass", scheduling.gpuClass);
        j.field("rc", o.rc).field("rcFlags", static_cast<unsigned>(rc.Flags));
        j.field("kbps", o.kbps).field("qp", o.qp).field("fps", o.fps).field("seconds", o.seconds);
        j.field("coded", std::to_string(codedW) + "x" + std::to_string(codedH));
        j.field("intraRefresh", o.intraRefresh).field("refs", o.refs).field("convert", o.convert);
        j.field("bitstream", sysmem ? "sysmem" : "copy").field("align", o.align);
        j.field("reconfigurable", reconfigurable).field("qpMapRegion", region);
        j.field("frames", frames).field("errors", errors).field("cut", cut);
        j.field("wallPMean", p.mean).field("wallPP50", p.p50).field("wallPP99", p.p99);
        j.field("wallPMax", p.max).field("wallIdrMean", i.mean);
        j.field("bytesPMean", b.mean).field("bytesPP99", b.p99).field("qpMean", q.mean);
        j.key("mbpsPerSecond").beginArray();
        for (const auto& s : steps) {
            j.beginObject();
            j.field("target", s.first).field("measured", s.second);
            j.endObject();
        }
        j.endArray();
        j.endObject();
        std::wstring path = o.jsonPath;
        if (path.empty()) path = wide("encode-" + computerName() + "-" + nowStamp() + ".json");
        std::ofstream file(path, std::ios::binary);
        file << j.str() << '\n';
        say("JSON: %s\n", utf8(path.c_str()).c_str());
    }
    return errors ? 1 : 0;
}

} // namespace lab
