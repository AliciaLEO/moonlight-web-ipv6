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

// mw-d3d12-lab scale — what the Lanczos-2 resample costs on a GPU, and what
// would make it cheaper (plan pipeline-video-d3d12-v2, C11.1).
//
// The product's resample is two 1-D passes of Lanczos-2 dilated to the ratio,
// in linear light (ConvertHlsl.h): every tap decodes its sRGB texel with a
// pow() and computes its weight with two sin(). On the N95's UHD Graphics the
// two passes cost ~11 ms a frame at 1440p -> 1080p, against the 1.5 ms the
// resample guard allows (ResampleCost), so the N95 streams bilinear. C11.1
// planned SM 6 shaders with 16-bit types; this probe first measures where the
// time goes, with the same passes written four ways:
//   product  the product's own shaders, compiled the product's way (FXC, O3)
//   lut      the weights precomputed on the CPU, one load per tap instead of
//            two sin() and a division; the sRGB decode still per tap
//   once     lut, and the capture decoded to linear light once, in a pass of
//            its own, instead of once per tap (seven times per texel)
//   half     once, with min16float arithmetic in the two resample passes
// and, for scale, the bilinear fetch the N95 falls back to. Each variant's
// picture is read back and compared with the product's.

#include "Scale.h"

#include "Lab.h"
#include "LabD3d12.h"

#include "convert/windows/ConvertShaders.h"
#include "mw/native/NativeHost.h"
#include "platform/windows/StreamPriority.h"
#include "platform/windows/d3d12/D3d12Device.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace lab {
namespace {

using namespace mw::native;

struct Options
{
    std::string adapter;
    int srcW = 2560, srcH = 1440;
    int outW = 1920, outH = 1080;
    int frames = 200;
    std::string gpuClass = "auto";
};

void say(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::fflush(stdout);
}

bool parseSize(const std::wstring& text, int& w, int& h)
{
    const size_t x = text.find(L'x');
    if (x == std::wstring::npos) return false;
    w = static_cast<int>(std::wcstol(text.c_str(), nullptr, 10));
    h = static_cast<int>(std::wcstol(text.c_str() + x + 1, nullptr, 10));
    return w > 0 && h > 0;
}

bool parse(int argc, wchar_t** argv, Options& o)
{
    for (int i = 0; i < argc; ++i) {
        const std::wstring arg = argv[i];
        const auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (arg == L"--adapter") {
            o.adapter = utf8(next().c_str());
        } else if (arg == L"--source") {
            if (!parseSize(next(), o.srcW, o.srcH)) return false;
        } else if (arg == L"--size") {
            if (!parseSize(next(), o.outW, o.outH)) return false;
        } else if (arg == L"--frames") {
            o.frames = std::clamp(std::atoi(utf8(next().c_str()).c_str()), 20, 5000);
        } else if (arg == L"--class") {
            o.gpuClass = utf8(next().c_str());
            if (!isGpuClassOption(o.gpuClass)) return false;
        } else {
            return false;
        }
    }
    return o.srcW >= o.outW && o.srcH >= o.outH;
}

// A full-screen triangle, the uv the product's passes read.
const char kVertexShader[] = R"HLSL(
struct VsOut
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};
VsOut VsMain(uint id : SV_VertexID)
{
    VsOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.position = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
)HLSL";

// The variants: the same Lanczos-2, the weights read from a buffer the CPU
// filled, the sRGB decode per tap or not, float or min16float.
const char kVariantShaders[] = R"HLSL(
Texture2D<float4> Source : register(t0);
Buffer<float> Weights    : register(t1);
Buffer<int> First        : register(t2);
SamplerState Linear      : register(s0);

struct VsOut
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

#if MW_HALF
typedef min16float3 Acc;
typedef min16float Real;
#else
typedef float3 Acc;
typedef float Real;
#endif

float3 SrgbToLinear(float3 c)
{
    return c <= 0.04045 ? c / 12.92 : pow(max(c + 0.055, 0.0) / 1.055, 2.4);
}

float4 PsLut(VsOut i) : SV_TARGET
{
    int2 p = int2(i.position.xy);
#if MW_HORIZONTAL
    int o = p.x;
    int fixed = p.y;
#else
    int o = p.y;
    int fixed = p.x;
#endif
    int first = First[o];
    Acc acc = 0;
    [unroll]
    for (int t = 0; t < MW_TAPS; ++t) {
        int q = clamp(first + t, 0, MW_LEN - 1);
#if MW_HORIZONTAL
        float3 c = Source.Load(int3(q, fixed, 0)).rgb;
#else
        float3 c = Source.Load(int3(fixed, q, 0)).rgb;
#endif
#if MW_DECODE
        c = SrgbToLinear(c);
#endif
        acc += (Acc)c * (Real)Weights[o * MW_TAPS + t];
    }
    return float4(max((float3)acc, 0.0), 1.0);
}

float4 PsDecode(VsOut i) : SV_TARGET
{
    return float4(SrgbToLinear(Source.Load(int3(int2(i.position.xy), 0)).rgb), 1.0);
}

float4 PsBilinear(VsOut i) : SV_TARGET
{
    return Source.SampleLevel(Linear, i.uv, 0);
}
)HLSL";

bool compile(const char* source, size_t size, const char* entry, const char* target,
             const std::vector<std::pair<std::string, std::string>>& macros, ComPtr<ID3DBlob>& blob,
             std::string& error)
{
    std::vector<D3D_SHADER_MACRO> defines;
    for (const auto& m : macros)
        defines.push_back({m.first.c_str(), m.second.c_str()});
    defines.push_back({nullptr, nullptr});
    ComPtr<ID3DBlob> errors;
    const HRESULT h =
        ::D3DCompile(source, size, "scale-lab.hlsl", defines.data(), nullptr, entry, target,
                     D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob.GetAddressOf(), errors.GetAddressOf());
    if (SUCCEEDED(h)) return true;
    error = std::string(entry) + ": " +
            (errors ? static_cast<const char*>(errors->GetBufferPointer()) : hr(h));
    return false;
}

/// The product's Lanczos-2 (ConvertHlsl.h), for the weights on the CPU.
double lanczos2(double x)
{
    x = std::fabs(x);
    if (x < 1e-5) return 1.0;
    if (x >= 2.0) return 0.0;
    const double px = 3.14159265358979 * x;
    return 2.0 * std::sin(px) * std::sin(px * 0.5) / (px * px);
}

/// One axis of the resample: per output sample, the first source texel and
/// the normalised weights of its taps — what the product's shader computes
/// per pixel, computed once.
struct Axis
{
    int taps = 0;
    std::vector<float> weights;
    std::vector<int> first;
};

Axis lanczosAxis(int length, int output)
{
    Axis a;
    const double dilate = std::max(1.0, static_cast<double>(length) / output);
    a.taps = static_cast<int>(std::ceil(4.0 * dilate)) + 1;
    a.weights.resize(static_cast<size_t>(output) * a.taps);
    a.first.resize(static_cast<size_t>(output));
    for (int o = 0; o < output; ++o) {
        const double centre = (o + 0.5) * length / output - 0.5;
        const int first = static_cast<int>(std::ceil(centre - 2.0 * dilate));
        double sum = 0;
        for (int t = 0; t < a.taps; ++t)
            sum += lanczos2((first + t - centre) / dilate);
        for (int t = 0; t < a.taps; ++t)
            a.weights[static_cast<size_t>(o) * a.taps + t] =
                static_cast<float>(lanczos2((first + t - centre) / dilate) / sum);
        a.first[static_cast<size_t>(o)] = first;
    }
    return a;
}

struct Pass
{
    ComPtr<ID3D12PipelineState> pso;
    int table = 0;  ///< descriptor table: t0-t2
    int target = 0; ///< RTV index
    int width = 0, height = 0;
    ID3D12Resource* written = nullptr;
    D3D12_RESOURCE_STATES after = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
};

struct Variant
{
    const char* name;
    std::vector<Pass> passes;
    bool compared = true;
};

} // namespace

void scaleUsage()
{
    std::puts("mw-d3d12-lab scale [options]\n"
              "  The Lanczos-2 resample, timed on the GPU pass by pass, the product's way and\n"
              "  three cheaper ways (weights precomputed, the sRGB decoded once, min16float),\n"
              "  each picture compared with the product's (plan C11.1).\n"
              "  --adapter <gpu>     a DXGI index or a piece of the name (UHD, Arc, AMD...)\n"
              "  --source WxH        the desktop (default 2560x1440)\n"
              "  --size WxH          the stream (default 1920x1080)\n"
              "  --frames <n>        timed frames per variant, after 20 untimed (default 200)\n"
              "  --class auto|high|normal   the GPU class, the product's way (default auto)");
}

int runScale(int argc, wchar_t** argv)
{
    Options o;
    if (!parse(argc, argv, o)) {
        scaleUsage();
        return 2;
    }
    NativeHost::setLogSink([](int level, const std::string& message) {
        if (level >= 2) std::printf("    %s\n", message.c_str());
    });
    StreamPriority priority;
    const GpuScheduling scheduling = takeGpuClass(priority, o.gpuClass);
    const std::vector<Adapter> all = adapters(false);
    const Adapter* adapter = pickAdapter(all, o.adapter);
    if (!adapter) {
        say("no GPU answers '%s'\n", o.adapter.c_str());
        return 2;
    }
    say("mw-d3d12-lab scale - %s on %s\n", nowText().c_str(), computerName().c_str());
    say("%s, driver %s; %dx%d -> %dx%d, %d frames per variant\n", adapter->name.c_str(),
        umdVersion(adapter->adapter.Get()).c_str(), o.srcW, o.srcH, o.outW, o.outH, o.frames);
    say("token %s, base-priority privilege %s, GPU class %s\n", scheduling.token.c_str(),
        scheduling.privilege ? "enabled" : "not held", scheduling.gpuClass.c_str());

    std::string error;
    std::shared_ptr<d3d12::D3d12Device> device =
        d3d12::D3d12Device::forAdapter(adapter->adapter.Get(), error);
    if (!device) {
        say("no D3D12: %s\n", error.c_str());
        return 1;
    }
    ID3D12Device* d = device->device();
    Direct direct;
    if (!direct.init(*device, error)) {
        say("%s\n", error.c_str());
        return 1;
    }
    d3d12::QueueTimer timer;
    if (!timer.init(d, direct.queue.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, error)) {
        say("the queue cannot be timed: %s\n", error.c_str());
        return 1;
    }

    // The textures: the desktop (BGRA8), its linear copy (FP16), the pass
    // between the two axes (FP16, output-wide, source-high, as the product's)
    // and the picture (RGBA8 written through an _SRGB view, as the product's).
    ComPtr<ID3D12Resource> source =
        makeTexture(d, o.srcW, o.srcH, 1, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
    ComPtr<ID3D12Resource> linear =
        makeTexture(d, o.srcW, o.srcH, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
                    D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    ComPtr<ID3D12Resource> mid = makeTexture(d, o.outW, o.srcH, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                             D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    ComPtr<ID3D12Resource> out = makeTexture(d, o.outW, o.outH, 1, DXGI_FORMAT_R8G8B8A8_TYPELESS,
                                             D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    if (!source || !linear || !mid || !out ||
        !upload(d, direct, source.Get(), {desktopFrame(o.srcW, o.srcH, 0)}, error)) {
        say("textures refused: %s\n", error.c_str());
        return 1;
    }

    // The weights of the two axes, in upload buffers the shaders read.
    const Axis h = lanczosAxis(o.srcW, o.outW);
    const Axis v = lanczosAxis(o.srcH, o.outH);
    const auto buffer = [&](const void* data, size_t bytes) {
        ComPtr<ID3D12Resource> b =
            makeBuffer(d, (bytes + 255) & ~size_t(255), D3D12_HEAP_TYPE_UPLOAD, false,
                       D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped = nullptr;
        if (b && SUCCEEDED(b->Map(0, nullptr, &mapped))) {
            std::memcpy(mapped, data, bytes);
            b->Unmap(0, nullptr);
        }
        return b;
    };
    ComPtr<ID3D12Resource> hWeights = buffer(h.weights.data(), h.weights.size() * sizeof(float));
    ComPtr<ID3D12Resource> hFirst = buffer(h.first.data(), h.first.size() * sizeof(int));
    ComPtr<ID3D12Resource> vWeights = buffer(v.weights.data(), v.weights.size() * sizeof(float));
    ComPtr<ID3D12Resource> vFirst = buffer(v.first.data(), v.first.size() * sizeof(int));

    // Descriptors: tables of three SRVs; four render targets.
    enum Table
    {
        TableSource,  // the desktop, no weights
        TableMid,     // the pass between, no weights
        TableSourceH, // the desktop, the horizontal weights
        TableLinearH, // the linear desktop, the horizontal weights
        TableMidV,    // the pass between, the vertical weights
        TableCount
    };
    enum Target
    {
        TargetMid,
        TargetOutSrgb,
        TargetOutUnorm,
        TargetLinear,
        TargetCount
    };
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = TableCount * 3;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> srvHeap, rtvHeap;
    d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeap));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = TargetCount;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap));
    if (!srvHeap || !rtvHeap) {
        say("descriptor heaps refused\n");
        return 1;
    }
    const UINT srvStride =
        d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT rtvStride = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const auto srvAt = [&](int table, int slot) {
        D3D12_CPU_DESCRIPTOR_HANDLE c = srvHeap->GetCPUDescriptorHandleForHeapStart();
        c.ptr += static_cast<SIZE_T>(table * 3 + slot) * srvStride;
        return c;
    };
    const auto textureSrv = [&](int table, ID3D12Resource* r, DXGI_FORMAT format) {
        D3D12_SHADER_RESOURCE_VIEW_DESC s = {};
        s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Format = format;
        s.Texture2D.MipLevels = 1;
        d->CreateShaderResourceView(r, &s, srvAt(table, 0));
    };
    const auto bufferSrv = [&](int table, int slot, ID3D12Resource* r, DXGI_FORMAT format,
                               UINT count) {
        D3D12_SHADER_RESOURCE_VIEW_DESC s = {};
        s.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Format = format;
        s.Buffer.NumElements = r ? count : 1;
        d->CreateShaderResourceView(r, &s, srvAt(table, slot));
    };
    for (int t = 0; t < TableCount; ++t) {
        bufferSrv(t, 1, nullptr, DXGI_FORMAT_R32_FLOAT, 0);
        bufferSrv(t, 2, nullptr, DXGI_FORMAT_R32_SINT, 0);
    }
    textureSrv(TableSource, source.Get(), DXGI_FORMAT_B8G8R8A8_UNORM);
    textureSrv(TableMid, mid.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    textureSrv(TableSourceH, source.Get(), DXGI_FORMAT_B8G8R8A8_UNORM);
    textureSrv(TableLinearH, linear.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    textureSrv(TableMidV, mid.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    for (int t : {TableSourceH, TableLinearH}) {
        bufferSrv(t, 1, hWeights.Get(), DXGI_FORMAT_R32_FLOAT, static_cast<UINT>(h.weights.size()));
        bufferSrv(t, 2, hFirst.Get(), DXGI_FORMAT_R32_SINT, static_cast<UINT>(h.first.size()));
    }
    bufferSrv(TableMidV, 1, vWeights.Get(), DXGI_FORMAT_R32_FLOAT,
              static_cast<UINT>(v.weights.size()));
    bufferSrv(TableMidV, 2, vFirst.Get(), DXGI_FORMAT_R32_SINT, static_cast<UINT>(v.first.size()));
    const auto rtvAt = [&](int target) {
        D3D12_CPU_DESCRIPTOR_HANDLE c = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        c.ptr += static_cast<SIZE_T>(target) * rtvStride;
        return c;
    };
    const auto rtv = [&](int target, ID3D12Resource* r, DXGI_FORMAT format) {
        D3D12_RENDER_TARGET_VIEW_DESC t = {};
        t.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        t.Format = format;
        d->CreateRenderTargetView(r, &t, rtvAt(target));
    };
    rtv(TargetMid, mid.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    rtv(TargetOutSrgb, out.Get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    rtv(TargetOutUnorm, out.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    rtv(TargetLinear, linear.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);

    // One table of three SRVs, a linear sampler for the bilinear fetch.
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 3;
    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 1;
    param.DescriptorTable.pDescriptorRanges = &range;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 1;
    rs.pParameters = &param;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &sampler;
    ComPtr<ID3DBlob> serialized, rsErrors;
    ComPtr<ID3D12RootSignature> root;
    if (FAILED(::D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &serialized,
                                             &rsErrors)) ||
        FAILED(d->CreateRootSignature(0, serialized->GetBufferPointer(),
                                      serialized->GetBufferSize(), IID_PPV_ARGS(&root)))) {
        say("root signature refused\n");
        return 1;
    }

    ComPtr<ID3DBlob> vs;
    if (!compile(kVertexShader, sizeof(kVertexShader) - 1, "VsMain", "vs_5_0", {}, vs, error)) {
        say("%s\n", error.c_str());
        return 1;
    }
    const auto pso = [&](ID3DBlob* ps, DXGI_FORMAT format) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p = {};
        p.pRootSignature = root.Get();
        p.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        p.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        p.SampleMask = UINT_MAX;
        p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        p.RasterizerState.DepthClipEnable = TRUE;
        p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        p.NumRenderTargets = 1;
        p.RTVFormats[0] = format;
        p.SampleDesc.Count = 1;
        ComPtr<ID3D12PipelineState> state;
        d->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&state));
        return state;
    };
    const auto variantShader = [&](const char* entry, bool horizontal, bool decode, bool half,
                                   ComPtr<ID3DBlob>& blob) {
        const Axis& a = horizontal ? h : v;
        return compile(kVariantShaders, sizeof(kVariantShaders) - 1, entry, "ps_5_0",
                       {{"MW_HORIZONTAL", horizontal ? "1" : "0"},
                        {"MW_DECODE", decode ? "1" : "0"},
                        {"MW_HALF", half ? "1" : "0"},
                        {"MW_TAPS", std::to_string(a.taps)},
                        {"MW_LEN", std::to_string(horizontal ? o.srcW : o.srcH)}},
                       blob, error);
    };
    ComPtr<ID3DBlob> productH, productV, lutH, lutV, onceH, halfH, halfV, decode, bilinear;
    const bool compiled =
        convert::compileScaleShader(true, true, o.srcW, o.outW, o.srcH, productH, error) &&
        convert::compileScaleShader(false, false, o.srcH, o.outH, o.outW, productV, error) &&
        variantShader("PsLut", true, true, false, lutH) &&
        variantShader("PsLut", false, false, false, lutV) &&
        variantShader("PsLut", true, false, false, onceH) &&
        variantShader("PsLut", true, false, true, halfH) &&
        variantShader("PsLut", false, false, true, halfV) &&
        variantShader("PsDecode", true, false, false, decode) &&
        variantShader("PsBilinear", true, false, false, bilinear);
    if (!compiled) {
        say("compile: %s\n", error.c_str());
        return 1;
    }

    const DXGI_FORMAT fp16 = DXGI_FORMAT_R16G16B16A16_FLOAT;
    const DXGI_FORMAT srgb = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    const auto pass = [&](ID3DBlob* ps, DXGI_FORMAT format, int table, int target, int w, int hh,
                          ID3D12Resource* written, D3D12_RESOURCE_STATES after) {
        Pass p;
        p.pso = pso(ps, format);
        p.table = table;
        p.target = target;
        p.width = w;
        p.height = hh;
        p.written = written;
        p.after = after;
        return p;
    };
    const auto shaderRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const auto kept = D3D12_RESOURCE_STATE_RENDER_TARGET;
    std::vector<Variant> variants = {
        {"product",
         {pass(productH.Get(), fp16, TableSource, TargetMid, o.outW, o.srcH, mid.Get(), shaderRead),
          pass(productV.Get(), srgb, TableMid, TargetOutSrgb, o.outW, o.outH, out.Get(), kept)}},
        {"lut",
         {pass(lutH.Get(), fp16, TableSourceH, TargetMid, o.outW, o.srcH, mid.Get(), shaderRead),
          pass(lutV.Get(), srgb, TableMidV, TargetOutSrgb, o.outW, o.outH, out.Get(), kept)}},
        {"once",
         {pass(decode.Get(), fp16, TableSource, TargetLinear, o.srcW, o.srcH, linear.Get(),
               shaderRead),
          pass(onceH.Get(), fp16, TableLinearH, TargetMid, o.outW, o.srcH, mid.Get(), shaderRead),
          pass(lutV.Get(), srgb, TableMidV, TargetOutSrgb, o.outW, o.outH, out.Get(), kept)}},
        {"half",
         {pass(decode.Get(), fp16, TableSource, TargetLinear, o.srcW, o.srcH, linear.Get(),
               shaderRead),
          pass(halfH.Get(), fp16, TableLinearH, TargetMid, o.outW, o.srcH, mid.Get(), shaderRead),
          pass(halfV.Get(), srgb, TableMidV, TargetOutSrgb, o.outW, o.outH, out.Get(), kept)}},
        {"bilinear",
         {pass(bilinear.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, TableSource, TargetOutUnorm, o.outW,
               o.outH, out.Get(), kept)},
         false},
    };
    for (const Variant& var : variants)
        for (const Pass& p : var.passes)
            if (!p.pso) {
                say("%s: a pipeline state was refused\n", var.name);
                return 1;
            }

    // States: the desktop stays readable; the others move between target and
    // shader resource within a frame; the picture stays a target.
    {
        ID3D12GraphicsCommandList* l = direct.begin();
        const D3D12_RESOURCE_BARRIER b[] = {
            transition(source.Get(), D3D12_RESOURCE_STATE_COMMON, shaderRead),
            transition(linear.Get(), D3D12_RESOURCE_STATE_COMMON, shaderRead),
            transition(mid.Get(), D3D12_RESOURCE_STATE_COMMON, shaderRead),
            transition(out.Get(), D3D12_RESOURCE_STATE_COMMON, kept),
        };
        l->ResourceBarrier(4, b);
        if (!direct.run(error)) {
            say("%s\n", error.c_str());
            return 1;
        }
    }

    const auto record = [&](ID3D12GraphicsCommandList* l, const Variant& var) {
        ID3D12DescriptorHeap* heaps[] = {srvHeap.Get()};
        l->SetDescriptorHeaps(1, heaps);
        l->SetGraphicsRootSignature(root.Get());
        l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for (const Pass& p : var.passes) {
            if (p.after != kept) {
                const D3D12_RESOURCE_BARRIER toTarget = transition(p.written, p.after, kept);
                l->ResourceBarrier(1, &toTarget);
            }
            const D3D12_CPU_DESCRIPTOR_HANDLE target = rtvAt(p.target);
            l->OMSetRenderTargets(1, &target, FALSE, nullptr);
            const D3D12_VIEWPORT vp = {
                0, 0, static_cast<float>(p.width), static_cast<float>(p.height), 0, 1};
            const D3D12_RECT scissor = {0, 0, p.width, p.height};
            l->RSSetViewports(1, &vp);
            l->RSSetScissorRects(1, &scissor);
            l->SetPipelineState(p.pso.Get());
            D3D12_GPU_DESCRIPTOR_HANDLE table = srvHeap->GetGPUDescriptorHandleForHeapStart();
            table.ptr += static_cast<UINT64>(p.table) * 3 * srvStride;
            l->SetGraphicsRootDescriptorTable(0, table);
            l->DrawInstanced(3, 1, 0, 0);
            if (p.after != kept) {
                const D3D12_RESOURCE_BARRIER back = transition(p.written, kept, p.after);
                l->ResourceBarrier(1, &back);
            }
        }
    };

    // The picture, read back: the product's is the reference.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot = {};
    UINT64 total = 0;
    const D3D12_RESOURCE_DESC outDesc = out->GetDesc();
    d->GetCopyableFootprints(&outDesc, 0, 1, 0, &foot, nullptr, nullptr, &total);
    ComPtr<ID3D12Resource> readback =
        makeBuffer(d, total, D3D12_HEAP_TYPE_READBACK, false, D3D12_RESOURCE_STATE_COPY_DEST);
    const auto readPicture = [&](std::vector<uint8_t>& pixels) {
        ID3D12GraphicsCommandList* l = direct.begin();
        D3D12_RESOURCE_BARRIER b = transition(out.Get(), kept, D3D12_RESOURCE_STATE_COPY_SOURCE);
        l->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION to = {}, from = {};
        to.pResource = readback.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint = foot;
        from.pResource = out.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        l->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        b = transition(out.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, kept);
        l->ResourceBarrier(1, &b);
        std::string why;
        if (!direct.run(why)) return false;
        uint8_t* mapped = nullptr;
        if (FAILED(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return false;
        pixels.resize(static_cast<size_t>(o.outW) * o.outH * 4);
        for (int y = 0; y < o.outH; ++y)
            std::memcpy(&pixels[static_cast<size_t>(y) * o.outW * 4],
                        mapped + foot.Offset + static_cast<size_t>(y) * foot.Footprint.RowPitch,
                        static_cast<size_t>(o.outW) * 4);
        const D3D12_RANGE none = {0, 0};
        readback->Unmap(0, &none);
        return true;
    };

    say("%-10s %9s %9s %9s   %s\n", "variant", "median", "p90", "min", "against the product");
    std::vector<uint8_t> reference;
    for (const Variant& var : variants) {
        std::vector<int64_t> us;
        for (int f = 0; f < o.frames + 20; ++f) {
            ID3D12GraphicsCommandList* l = direct.begin();
            const int slot = timer.begin(l);
            record(l, var);
            if (slot >= 0) timer.end(l, slot);
            if (!direct.run(error)) {
                say("%s: %s\n", var.name, error.c_str());
                return 1;
            }
            d3d12::QueueTimer::Sample sample;
            if (slot >= 0 && timer.read(slot, sample) && f >= 20) us.push_back(sample.gpuUs);
        }
        std::sort(us.begin(), us.end());
        const auto at = [&](double q) {
            return us.empty()
                       ? 0.0
                       : us[std::min(us.size() - 1, static_cast<size_t>(us.size() * q))] / 1000.0;
        };
        std::string against = "-";
        std::vector<uint8_t> pixels;
        if (var.compared && readPicture(pixels)) {
            if (reference.empty()) {
                reference = pixels;
                against = "(the reference)";
            } else {
                int worst = 0;
                size_t overOne = 0;
                double sum = 0;
                for (size_t i = 0; i < pixels.size(); ++i) {
                    if ((i & 3) == 3) continue;
                    const int diff = std::abs(static_cast<int>(pixels[i]) - reference[i]);
                    worst = std::max(worst, diff);
                    overOne += diff > 1;
                    sum += diff;
                }
                char text[96];
                std::snprintf(text, sizeof(text), "max %d, %zu over 1, mean %.4f", worst, overOne,
                              sum / (pixels.size() * 3 / 4));
                against = text;
            }
        }
        say("%-10s %6.2f ms %6.2f ms %6.2f ms   %s\n", var.name, at(0.5), at(0.9),
            us.empty() ? 0.0 : us.front() / 1000.0, against.c_str());
    }
    std::string gone;
    if (device->removed(gone)) say("the device is gone: %s\n", gone.c_str());
    return 0;
}

} // namespace lab
