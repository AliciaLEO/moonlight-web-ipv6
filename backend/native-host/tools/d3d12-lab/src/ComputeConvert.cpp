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

#include "ComputeConvert.h"

#include "Lab.h"

#include "convert/windows/ConvertHlsl.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

using mw::native::capture::CursorState;
using mw::native::convert::CursorDraw;
using mw::native::convert::ScaleFilter;

namespace lab {
namespace {

// Compute has no derivatives: the scene's one Sample() becomes the level-0
// fetch it is on a single-mip texture. Every other fetch is SampleLevel or
// Load already.
constexpr char kComputePrelude[] = "#define Sample(s, uv) SampleLevel(s, uv, 0)\n";

// The pixel-shader entry points called per texel, with the uv the rasterizer
// would have handed them: the texel's centre over the target.
constexpr char kComputeEntries[] = R"HLSL(
#if MW_CHROMA
RWTexture2D<float2> Target : register(u0);
#else
RWTexture2D<float>  Target : register(u0);
#endif

[numthreads(8, 8, 1)]
void CsMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= uint(MW_TARGET_W) || id.y >= uint(MW_TARGET_H)) return;
    VsOut v;
    v.position = float4(0.0, 0.0, 0.0, 1.0);
    v.uv = (float2(id.xy) + 0.5) / float2(MW_TARGET_W, MW_TARGET_H);
#if MW_CHROMA
    Target[id.xy] = PsChroma(v);
#else
    Target[id.xy] = PsLuma(v);
#endif
}
)HLSL";

// The resample over its target: the picture's rectangle, bars black around it
// (no ClearRenderTargetView on a compute list), and on the 8-bit path the sRGB
// encode an _SRGB render target does for free.
constexpr char kComputeScaleEntries[] = R"HLSL(
RWTexture2D<float4> Target : register(u0);

float3 LinearToSrgbOut(float3 c)
{
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055;
}

[numthreads(8, 8, 1)]
void CsMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= uint(MW_TARGET_W) || id.y >= uint(MW_TARGET_H)) return;
    int2 p = int2(id.xy) - int2(MW_PIC_X, MW_PIC_Y);
    if (p.x < 0 || p.y < 0 || p.x >= int(MW_PIC_W) || p.y >= int(MW_PIC_H)) {
        Target[id.xy] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    VsOut v;
    v.position = float4(0.0, 0.0, 0.0, 1.0);
    v.uv = (float2(p) + 0.5) / float2(MW_PIC_W, MW_PIC_H);
    float4 c = PsScale(v);
#if MW_ENCODE
    c.rgb = LinearToSrgbOut(saturate(c.rgb));
#endif
    Target[id.xy] = c;
}
)HLSL";

constexpr UINT kTableSize = 4; // t0-t2, u0
constexpr UINT kTables = 16;   // four passes × four frames

HRESULT committed(ID3D12Device* device, D3D12_HEAP_TYPE type, const D3D12_RESOURCE_DESC& desc,
                  D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = type;
    return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                           IID_PPV_ARGS(out.ReleaseAndGetAddressOf()));
}

D3D12_RESOURCE_DESC texture(int w, int h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = static_cast<UINT64>(w);
    desc.Height = static_cast<UINT>(h);
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    return desc;
}

D3D12_STATIC_SAMPLER_DESC sampler(UINT reg, D3D12_FILTER filter)
{
    D3D12_STATIC_SAMPLER_DESC s = {};
    s.Filter = filter;
    s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    s.MaxLOD = D3D12_FLOAT32_MAX;
    s.ShaderRegister = reg;
    s.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    return s;
}

UINT groups(int extent)
{
    return static_cast<UINT>((extent + 7) / 8);
}

} // namespace

bool ComputeConvert::compile(const std::string& source, const char* entry,
                             const std::vector<std::pair<std::string, std::string>>& defines,
                             ComPtr<ID3DBlob>& blob, std::string& error)
{
    std::vector<D3D_SHADER_MACRO> macros;
    for (const auto& d : defines)
        macros.push_back({d.first.c_str(), d.second.c_str()});
    macros.push_back({nullptr, nullptr});
    ComPtr<ID3DBlob> errors;
    const HRESULT h =
        ::D3DCompile(source.data(), source.size(), "ComputeConvert.hlsl", macros.data(), nullptr,
                     entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
    if (SUCCEEDED(h)) return true;
    error = std::string("could not compile the compute conversion (") + lab::hr(h) + ")";
    if (errors && errors->GetBufferPointer())
        error += ": " + std::string(static_cast<const char*>(errors->GetBufferPointer()));
    return false;
}

bool ComputeConvert::pso(ID3DBlob* cs, ComPtr<ID3D12PipelineState>& out, std::string& error)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = m_Root.Get();
    desc.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    const HRESULT h = m_Device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&out));
    if (FAILED(h)) {
        error = "could not create a compute pipeline state (" + lab::hr(h) + ")";
        return false;
    }
    return true;
}

bool ComputeConvert::init(ID3D12Device* device, int sourceWidth, int sourceHeight, int outputWidth,
                          int outputHeight, ScaleFilter filter, bool typedUav, std::string& error)
{
    m_Device = device;
    m_SourceWidth = sourceWidth;
    m_SourceHeight = sourceHeight;
    if (!mw::native::convert::convertGeometry(sourceWidth, sourceHeight, outputWidth, outputHeight,
                                              filter, m_Geometry, error))
        return false;

    if (typedUav) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {DXGI_FORMAT_NV12, {}, {}};
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support,
                                               sizeof(support))) ||
            !(support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) ||
            !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) {
            error = "no typed UAV on NV12 on this GPU";
            return false;
        }
    }
    m_TypedUav = typedUav;

    // b0 as root constants, one table of t0-t2 + u0, the two samplers static.
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 3;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = 3;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 8;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    const D3D12_STATIC_SAMPLER_DESC samplers[] = {
        sampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR),
        sampler(1, D3D12_FILTER_MIN_MAG_MIP_POINT),
    };
    D3D12_ROOT_SIGNATURE_DESC root = {};
    root.NumParameters = 2;
    root.pParameters = params;
    root.NumStaticSamplers = 2;
    root.pStaticSamplers = samplers;
    ComPtr<ID3DBlob> serialized, errors;
    HRESULT h =
        ::D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
    if (SUCCEEDED(h))
        h = device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                        serialized->GetBufferSize(), IID_PPV_ARGS(&m_Root));
    if (FAILED(h)) {
        error = "compute root signature: " + lab::hr(h);
        return false;
    }

    const int outW = m_Geometry.outputWidth;
    const int outH = m_Geometry.outputHeight;
    const std::string convertSource =
        std::string(kComputePrelude) + mw::native::convert::kShaderSource + kComputeEntries;
    ComPtr<ID3DBlob> luma, chroma;
    if (!compile(convertSource, "CsMain",
                 {{"MW_SCRGB_SOURCE", "0"},
                  {"MW_CHROMA", "0"},
                  {"MW_TARGET_W", std::to_string(outW) + ".0"},
                  {"MW_TARGET_H", std::to_string(outH) + ".0"}},
                 luma, error) ||
        !compile(convertSource, "CsMain",
                 {{"MW_SCRGB_SOURCE", "0"},
                  {"MW_CHROMA", "1"},
                  {"MW_TARGET_W", std::to_string(outW / 2) + ".0"},
                  {"MW_TARGET_H", std::to_string(outH / 2) + ".0"}},
                 chroma, error) ||
        !pso(luma.Get(), m_Luma, error) || !pso(chroma.Get(), m_Chroma, error))
        return false;

    if (m_Geometry.filter != ScaleFilter::Bilinear) {
        const int picW = static_cast<int>(m_Geometry.pictureWidth);
        const int picH = static_cast<int>(m_Geometry.pictureHeight);
        // The same geometry macros ConvertShaders gives the pixel shaders.
        const auto scaleDefines = [](bool horizontal, bool decode, int length, int output,
                                     int fixed) {
            const double dilate = std::max(1.0, static_cast<double>(length) / output);
            const int taps = static_cast<int>(std::ceil(4.0 * dilate)) + 1;
            std::vector<std::pair<std::string, std::string>> d = {
                {"MW_HORIZONTAL", horizontal ? "1" : "0"},
                {"MW_DECODE", decode ? "1" : "0"},
                {"MW_DILATE", length <= output ? std::string("1.0")
                                               : "(" + std::to_string(length) + ".0 / " +
                                                     std::to_string(output) + ".0)"},
                {"MW_TAPS", std::to_string(taps)},
                {"MW_LEN", std::to_string(length) + ".0"},
                {"MW_FIXED", std::to_string(fixed) + ".0"}};
            return d;
        };
        const std::string scaleSource =
            std::string(mw::native::convert::kScaleShaderSource) + kComputeScaleEntries;
        auto hDefines = scaleDefines(true, true, sourceWidth, picW, sourceHeight);
        hDefines.insert(hDefines.end(), {{"MW_TARGET_W", std::to_string(picW)},
                                         {"MW_TARGET_H", std::to_string(sourceHeight)},
                                         {"MW_PIC_X", "0"},
                                         {"MW_PIC_Y", "0"},
                                         {"MW_PIC_W", std::to_string(picW) + ".0"},
                                         {"MW_PIC_H", std::to_string(sourceHeight) + ".0"},
                                         {"MW_ENCODE", "0"}});
        auto vDefines = scaleDefines(false, false, sourceHeight, picH, picW);
        vDefines.insert(vDefines.end(),
                        {{"MW_TARGET_W", std::to_string(outW)},
                         {"MW_TARGET_H", std::to_string(outH)},
                         {"MW_PIC_X", std::to_string(static_cast<int>(m_Geometry.pictureX))},
                         {"MW_PIC_Y", std::to_string(static_cast<int>(m_Geometry.pictureY))},
                         {"MW_PIC_W", std::to_string(picW) + ".0"},
                         {"MW_PIC_H", std::to_string(picH) + ".0"},
                         {"MW_ENCODE", "1"}});
        ComPtr<ID3DBlob> hBlob, vBlob;
        if (!compile(scaleSource, "CsMain", hDefines, hBlob, error) ||
            !compile(scaleSource, "CsMain", vDefines, vBlob, error) ||
            !pso(hBlob.Get(), m_ScaleH, error) || !pso(vBlob.Get(), m_ScaleV, error))
            return false;
        if (FAILED(h = committed(device, D3D12_HEAP_TYPE_DEFAULT,
                                 texture(picW, sourceHeight, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                                 D3D12_RESOURCE_STATE_COMMON, m_Mid)) ||
            FAILED(h = committed(device, D3D12_HEAP_TYPE_DEFAULT,
                                 texture(outW, outH, DXGI_FORMAT_R8G8B8A8_UNORM,
                                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                                 D3D12_RESOURCE_STATE_COMMON, m_Scaled))) {
            error = "resample textures: " + lab::hr(h);
            return false;
        }
    }

    // Straight into the planes where typed UAVs exist; through R8/R8G8 and a
    // copy elsewhere.
    if (FAILED(h = committed(device, D3D12_HEAP_TYPE_DEFAULT,
                             texture(outW, outH, DXGI_FORMAT_NV12,
                                     m_TypedUav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                                : D3D12_RESOURCE_FLAG_NONE),
                             D3D12_RESOURCE_STATE_COMMON, m_Output))) {
        error = "NV12 output: " + lab::hr(h);
        return false;
    }
    if (!m_TypedUav && (FAILED(h = committed(device, D3D12_HEAP_TYPE_DEFAULT,
                                             texture(outW, outH, DXGI_FORMAT_R8_UNORM,
                                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                                             D3D12_RESOURCE_STATE_COMMON, m_Luma8)) ||
                        FAILED(h = committed(device, D3D12_HEAP_TYPE_DEFAULT,
                                             texture(outW / 2, outH / 2, DXGI_FORMAT_R8G8_UNORM,
                                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                                             D3D12_RESOURCE_STATE_COMMON, m_Chroma8)))) {
        error = "R8/R8G8 planes: " + lab::hr(h);
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap = {};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = kTables * kTableSize;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(h = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_Heap)))) {
        error = "descriptor heap: " + lab::hr(h);
        return false;
    }
    m_Stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

void ComputeConvert::transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                                D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES next,
                                UINT subresource)
{
    if (state == next) return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = subresource;
    b.Transition.StateBefore = state;
    b.Transition.StateAfter = next;
    list->ResourceBarrier(1, &b);
    state = next;
}

D3D12_GPU_DESCRIPTOR_HANDLE ComputeConvert::table(ID3D12Resource* source, DXGI_FORMAT sourceFormat,
                                                  bool cursor, ID3D12Resource* target,
                                                  DXGI_FORMAT targetFormat, UINT targetPlane)
{
    const UINT index = m_Next;
    m_Next = (m_Next + 1) % kTables;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_Heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(index) * kTableSize * m_Stride;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_Heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(index) * kTableSize * m_Stride;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    srv.Format = sourceFormat;
    m_Device->CreateShaderResourceView(source, &srv, cpu);
    cpu.ptr += m_Stride;
    srv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    m_Device->CreateShaderResourceView(cursor ? m_CursorPixels.Get() : nullptr, &srv, cpu);
    cpu.ptr += m_Stride;
    srv.Format = DXGI_FORMAT_R8_UNORM;
    m_Device->CreateShaderResourceView(cursor ? m_CursorInvert.Get() : nullptr, &srv, cpu);
    cpu.ptr += m_Stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Format = targetFormat;
    uav.Texture2D.PlaneSlice = targetPlane;
    m_Device->CreateUnorderedAccessView(target, nullptr, &uav, cpu);
    return gpu;
}

bool ComputeConvert::setCursor(ID3D12GraphicsCommandList* list, const CursorState& cursor,
                               std::string& error)
{
    if (cursor.width <= 0 || cursor.height <= 0) return true;
    const D3D12_RESOURCE_DESC pixelsDesc =
        texture(cursor.width, cursor.height, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
    const D3D12_RESOURCE_DESC invertDesc =
        texture(cursor.width, cursor.height, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_NONE);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT pf = {}, inf = {};
    UINT64 pixelsBytes = 0, invertBytes = 0;
    m_Device->GetCopyableFootprints(&pixelsDesc, 0, 1, 0, &pf, nullptr, nullptr, &pixelsBytes);
    const UINT64 offset = (pixelsBytes + 511) & ~511ull;
    m_Device->GetCopyableFootprints(&invertDesc, 0, 1, offset, &inf, nullptr, nullptr,
                                    &invertBytes);
    D3D12_RESOURCE_DESC buffer = {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = offset + invertBytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT h = committed(m_Device.Get(), D3D12_HEAP_TYPE_UPLOAD, buffer,
                          D3D12_RESOURCE_STATE_GENERIC_READ, m_CursorUpload);
    if (SUCCEEDED(h))
        h = committed(m_Device.Get(), D3D12_HEAP_TYPE_DEFAULT, pixelsDesc,
                      D3D12_RESOURCE_STATE_COPY_DEST, m_CursorPixels);
    if (SUCCEEDED(h))
        h = committed(m_Device.Get(), D3D12_HEAP_TYPE_DEFAULT, invertDesc,
                      D3D12_RESOURCE_STATE_COPY_DEST, m_CursorInvert);
    uint8_t* mapped = nullptr;
    if (SUCCEEDED(h)) h = m_CursorUpload->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    if (FAILED(h)) {
        error = "cursor upload: " + lab::hr(h);
        return false;
    }
    for (int y = 0; y < cursor.height; ++y) {
        std::memcpy(mapped + pf.Offset + y * static_cast<size_t>(pf.Footprint.RowPitch),
                    cursor.pixels.data() + static_cast<size_t>(y) * cursor.width * 4,
                    static_cast<size_t>(cursor.width) * 4);
        std::memcpy(mapped + inf.Offset + y * static_cast<size_t>(inf.Footprint.RowPitch),
                    cursor.invert.data() + static_cast<size_t>(y) * cursor.width,
                    static_cast<size_t>(cursor.width));
    }
    m_CursorUpload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION from = {}, to = {};
    from.pResource = m_CursorUpload.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.PlacedFootprint = pf;
    to.pResource = m_CursorPixels.Get();
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    from.PlacedFootprint = inf;
    to.pResource = m_CursorInvert.Get();
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    D3D12_RESOURCE_STATES a = D3D12_RESOURCE_STATE_COPY_DEST, b = D3D12_RESOURCE_STATE_COPY_DEST;
    transition(list, m_CursorPixels.Get(), a, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(list, m_CursorInvert.Get(), b, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return true;
}

bool ComputeConvert::record(ID3D12GraphicsCommandList* list, ID3D12Resource* source,
                            const CursorState& cursor, const CursorDraw& draw, std::string& error)
{
    if (!list || !source || !m_Luma) {
        error = "the compute conversion is not initialized";
        return false;
    }
    ID3D12DescriptorHeap* heaps[] = {m_Heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(m_Root.Get());

    D3D12_RESOURCE_STATES sourceState = D3D12_RESOURCE_STATE_COMMON;
    transition(list, source, sourceState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const int outW = m_Geometry.outputWidth;
    const int outH = m_Geometry.outputHeight;

    const bool resampled = m_Geometry.filter != ScaleFilter::Bilinear;
    if (resampled) {
        const int picW = static_cast<int>(m_Geometry.pictureWidth);
        transition(list, m_Mid.Get(), m_MidState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->SetPipelineState(m_ScaleH.Get());
        list->SetComputeRootDescriptorTable(1,
                                            table(source, DXGI_FORMAT_B8G8R8A8_UNORM, false,
                                                  m_Mid.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 0));
        list->Dispatch(groups(picW), groups(m_SourceHeight), 1);
        transition(list, m_Mid.Get(), m_MidState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(list, m_Scaled.Get(), m_ScaledState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->SetPipelineState(m_ScaleV.Get());
        list->SetComputeRootDescriptorTable(1, table(m_Mid.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
                                                     false, m_Scaled.Get(),
                                                     DXGI_FORMAT_R8G8B8A8_UNORM, 0));
        list->Dispatch(groups(outW), groups(outH), 1);
        transition(list, m_Scaled.Get(), m_ScaledState,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    const mw::native::convert::OverlayConstants overlay = mw::native::convert::overlayConstants(
        cursor, draw, m_CursorPixels != nullptr, m_SourceWidth, m_SourceHeight, resampled,
        m_Geometry, 1.0f);
    list->SetComputeRoot32BitConstants(0, 8, overlay.values, 0);
    ID3D12Resource* picture = resampled ? m_Scaled.Get() : source;
    const DXGI_FORMAT pictureFormat =
        resampled ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
    const bool withCursor = m_CursorPixels != nullptr;

    if (m_TypedUav) {
        transition(list, m_Output.Get(), m_OutputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->SetPipelineState(m_Luma.Get());
        list->SetComputeRootDescriptorTable(
            1, table(picture, pictureFormat, withCursor, m_Output.Get(), DXGI_FORMAT_R8_UNORM, 0));
        list->Dispatch(groups(outW), groups(outH), 1);
        list->SetPipelineState(m_Chroma.Get());
        list->SetComputeRootDescriptorTable(1, table(picture, pictureFormat, withCursor,
                                                     m_Output.Get(), DXGI_FORMAT_R8G8_UNORM, 1));
        list->Dispatch(groups(outW / 2), groups(outH / 2), 1);
        transition(list, m_Output.Get(), m_OutputState, D3D12_RESOURCE_STATE_COMMON);
    } else {
        transition(list, m_Luma8.Get(), m_Luma8State, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(list, m_Chroma8.Get(), m_Chroma8State, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->SetPipelineState(m_Luma.Get());
        list->SetComputeRootDescriptorTable(
            1, table(picture, pictureFormat, withCursor, m_Luma8.Get(), DXGI_FORMAT_R8_UNORM, 0));
        list->Dispatch(groups(outW), groups(outH), 1);
        list->SetPipelineState(m_Chroma.Get());
        list->SetComputeRootDescriptorTable(1, table(picture, pictureFormat, withCursor,
                                                     m_Chroma8.Get(), DXGI_FORMAT_R8G8_UNORM, 0));
        list->Dispatch(groups(outW / 2), groups(outH / 2), 1);
        transition(list, m_Luma8.Get(), m_Luma8State, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(list, m_Chroma8.Get(), m_Chroma8State, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(list, m_Output.Get(), m_OutputState, D3D12_RESOURCE_STATE_COPY_DEST);
        for (UINT plane = 0; plane < 2; ++plane) {
            D3D12_TEXTURE_COPY_LOCATION to = {}, from = {};
            to.pResource = m_Output.Get();
            to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.SubresourceIndex = plane;
            from.pResource = plane == 0 ? m_Luma8.Get() : m_Chroma8.Get();
            from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            from.SubresourceIndex = 0;
            list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        }
        transition(list, m_Output.Get(), m_OutputState, D3D12_RESOURCE_STATE_COMMON);
    }
    transition(list, source, sourceState, D3D12_RESOURCE_STATE_COMMON);
    return true;
}

} // namespace lab
