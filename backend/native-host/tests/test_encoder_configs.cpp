/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#if defined(_WIN32)
#include "encode/ConfigFingerprint.h"
#include "encode/windows/AmfEncoder.h"
#include "encode/windows/NvencEncoder.h"
#include "encode/windows/d3d12/NvencEncoder12.h"
#include "mw/native/NativeHost.h"
#include "platform/windows/d3d12/D3d12Device.h"

#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#endif

#include <cstdio>

// Every hardware encoder this machine has, opened under a matrix of settings —
// codecs, HDR, 4:4:4, intra-refresh, the bench's keys — with nothing captured
// and nothing encoded: what is checked is the configuration each one hands its
// driver, through its fingerprint (encode/ConfigFingerprint.h).
//
// The lines this prints are the proof of an extraction that must not change a
// setting (plan pipeline-video-d3d12-v2, C7.1 and C7.3): the same lines before
// and after, row for row. A machine without the vendor prints nothing for it.
//
// The D3D12 encoders (C7.2, C7.4) open the same rows beside them — 4:4:4
// aside, which the D3D12 chain does not take — and must hand their drivers
// the very configuration the D3D11 ones do: the same fingerprint, or a test
// failure.

#if defined(_WIN32)
using namespace mw::native;
using Microsoft::WRL::ComPtr;

namespace {

/// A D3D11 device on the adapter @p luid, as an encoder's session wants one.
ComPtr<ID3D11Device> deviceOn(uint64_t luid)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        if (d3d12::luidValue(desc.AdapterLuid) != luid) continue;
        ComPtr<ID3D11Device> device;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                     D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
                                     &device, nullptr, nullptr)))
            return nullptr;
        return device;
    }
    return nullptr;
}

/// One configuration of the matrix.
struct Row
{
    std::string name;
    Codec codec = Codec::Hevc;
    int width = 1920;
    int height = 1080;
    int fps = 60;
    int kbps = 20000;
    bool yuv444 = false;
    bool hdr = false;
    bool intraRefresh = false;
    EncoderTuning tuning;
    /// A row every GPU of the vendor must open: its refusal fails the test.
    bool required = false;
};

bool has(const std::vector<Codec>& codecs, Codec c)
{
    return std::find(codecs.begin(), codecs.end(), c) != codecs.end();
}

/// The matrix for @p gpu, driven by @p api: rows each vendor reads, and the
/// bench keys it honours.
std::vector<Row> matrixFor(const GpuInfo& gpu, EncoderApi api)
{
    std::vector<Row> rows;
    for (Codec codec : {Codec::H264, Codec::Hevc, Codec::Av1}) {
        if (!has(gpu.codecs, codec)) continue;
        const std::string c = toString(codec);
        const auto add = [&](const std::string& name, const std::function<void(Row&)>& set) {
            Row r;
            r.name = c + " " + name;
            r.codec = codec;
            set(r);
            rows.push_back(r);
        };
        add("1080p60", [](Row& r) { r.required = true; });
        add("1440p120", [](Row& r) {
            r.width = 2560;
            r.height = 1440;
            r.fps = 120;
            r.kbps = 40000;
        });
        add("intra-refresh", [](Row& r) { r.intraRefresh = true; });
        add("vbv=2", [](Row& r) { r.tuning.vbvFrames = 2; });
        add("dpb=1", [](Row& r) { r.tuning.dpbFrames = 1; });
        if (api == EncoderApi::Nvenc) {
            if (gpu.supports10Bit && codec != Codec::H264) add("hdr", [](Row& r) { r.hdr = true; });
            if (has(gpu.codecs444, codec)) add("4:4:4", [](Row& r) { r.yuv444 = true; });
            add("preset=4", [](Row& r) { r.tuning.nvencPreset = 4; });
            add("tuning=ll", [](Row& r) { r.tuning.nvencTuning = EncoderTuning::Latency::Low; });
            add("multipass=off",
                [](Row& r) { r.tuning.nvencMultiPass = EncoderTuning::MultiPass::Off; });
            add("aq=1 taq=1", [](Row& r) {
                r.tuning.spatialAq = EncoderTuning::Choice::On;
                r.tuning.temporalAq = EncoderTuning::Choice::On;
            });
            add("minqp=-1", [](Row& r) { r.tuning.nvencMinQp = -1; });
            add("minqp=22", [](Row& r) { r.tuning.nvencMinQp = 22; });
            add("intra-refresh 240/60", [](Row& r) {
                r.intraRefresh = true;
                r.tuning.nvencIntraRefreshPeriod = 240;
                r.tuning.nvencIntraRefreshCount = 60;
            });
        } else if (api == EncoderApi::Amf) {
            add("quality=speed",
                [](Row& r) { r.tuning.amfQuality = EncoderTuning::AmfQuality::Speed; });
            add("quality=quality",
                [](Row& r) { r.tuning.amfQuality = EncoderTuning::AmfQuality::Quality; });
            add("preanalysis=1", [](Row& r) { r.tuning.preAnalysis = EncoderTuning::Choice::On; });
            add("aq=1", [](Row& r) { r.tuning.spatialAq = EncoderTuning::Choice::On; });
            add("lowlatency=1", [](Row& r) { r.tuning.amfLowLatency = EncoderTuning::Choice::On; });
            add("minqp=-1", [](Row& r) { r.tuning.amfMinQp = -1; });
            add("minqp=22", [](Row& r) { r.tuning.amfMinQp = 22; });
        }
    }
    return rows;
}

/// Opens @p row on the D3D11 encoder of @p api, and says the fingerprint —
/// "refused: …" instead when the driver would not have it.
std::string openD3d11(EncoderApi api, ID3D11Device* device, const Row& row, bool& opened)
{
    std::string error;
    opened = false;
    if (api == EncoderApi::Nvenc) {
        auto encoder = std::make_unique<encode::NvencEncoder>();
        opened = encoder->init(device, row.codec, row.width, row.height, row.fps, row.kbps,
                               row.yuv444, row.hdr, row.intraRefresh, row.tuning, error);
        const uint32_t fingerprint = encoder->configFingerprint();
        encoder->stop();
        if (opened) return encode::ConfigFingerprint::text(fingerprint);
    } else {
        auto encoder = std::make_unique<encode::AmfEncoder>();
        opened = encoder->init(device, row.codec, row.width, row.height, row.fps, row.kbps,
                               row.yuv444, row.hdr, row.intraRefresh, row.tuning, error);
        const uint32_t fingerprint = encoder->configFingerprint();
        encoder->stop();
        if (opened) return encode::ConfigFingerprint::text(fingerprint);
    }
    return "refused: " + error;
}

/// The same for the D3D12 encoder of @p api, on @p device.
std::string openD3d12(EncoderApi api, const std::shared_ptr<d3d12::D3d12Device>& device,
                      const Row& row, bool& opened)
{
    std::string error;
    opened = false;
    if (api == EncoderApi::Nvenc) {
        auto encoder = std::make_unique<encode::NvencEncoder12>();
        opened = encoder->init(device, row.codec, row.width, row.height, row.fps, row.kbps, row.hdr,
                               row.intraRefresh, row.tuning, error);
        const uint32_t fingerprint = encoder->configFingerprint();
        encoder->stop();
        if (opened) return encode::ConfigFingerprint::text(fingerprint);
        return "refused: " + error;
    }
    return "";
}

} // namespace
#endif

void run_encoder_configs_tests()
{
    SECTION("Encoders — the configuration each one hands its driver, by fingerprint");

#if !defined(_WIN32)
    std::fprintf(stderr, "  skipped: Windows only\n");
#else
    const Capabilities caps = NativeHost::probe();
    int vendors = 0;
    for (const GpuInfo& gpu : caps.gpus) {
        if (gpu.encoders.empty()) continue;
        const EncoderApi api = gpu.encoders.front();
        if (api != EncoderApi::Nvenc && api != EncoderApi::Amf) continue;
        ComPtr<ID3D11Device> device = deviceOn(gpu.nativeHandle);
        if (!device) {
            std::fprintf(stderr, "  %s: no D3D11 device\n", gpu.name.c_str());
            continue;
        }
        ++vendors;
        std::fprintf(stderr, "  %s — %s\n", gpu.name.c_str(), toString(api));
        std::string why;
        const std::shared_ptr<d3d12::D3d12Device> device12 =
            d3d12::D3d12Device::forAdapter(gpu.nativeHandle, why);
        for (const Row& row : matrixFor(gpu, api)) {
            bool opened = false;
            const std::string d3d11 = openD3d11(api, device.Get(), row, opened);
            std::fprintf(stderr, "  %-6s %-24s %s\n", toString(api), row.name.c_str(),
                         d3d11.c_str());
            if (row.required) CHECK(opened);
            if (!device12 || row.yuv444) continue;
            bool opened12 = false;
            const std::string d3d12 = openD3d12(api, device12, row, opened12);
            if (d3d12.empty()) continue; // no D3D12 encoder of this vendor yet
            if (d3d12 != d3d11)
                std::fprintf(stderr, "  %-6s %-24s D3D12: %s\n", toString(api), row.name.c_str(),
                             d3d12.c_str());
            CHECK_EQ(opened12, opened);
            if (opened && opened12) CHECK_EQ(d3d12, d3d11);
        }
    }
    if (vendors == 0) std::fprintf(stderr, "  skipped: no NVENC or AMF GPU here\n");
#endif
}
