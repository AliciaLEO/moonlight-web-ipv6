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

#pragma once

// The conversion as compute, for the `queues` probe only: what G1 needs to
// decide whether a COMPUTE route (C3.4) is worth building. It runs the very
// HLSL of ConvertHlsl.h — the pixel-shader functions are called from compute
// entry points, with Sample() spelled SampleLevel(…, 0) since compute has no
// derivatives — so what is timed is the product's arithmetic, not a stand-in.
//
// 8-bit SDR only (the probe's case), NV12 out. Two ways to reach NV12:
//  - typed UAVs straight on the NV12 planes, where the GPU has them (the RTX);
//  - R8 and R8G8 textures, copied into the planes on the same list — the only
//    way elsewhere (the Arc and the AMD iGPU have no typed UAV on NV12).
// The 8-bit resample is sRGB-encoded in the shader: a UAV has no _SRGB
// format to do it for free as the pixel-shader path does, so the bytes may
// differ from ColorConvert by a code; the timing is what matters here.

#include "capture/CaptureTypes.h"
#include "convert/CursorDraw.h"
#include "convert/ScaleFilter.h"
#include "convert/windows/ConvertShaders.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <string>
#include <utility>
#include <vector>

namespace lab {

class ComputeConvert
{
public:
    bool init(ID3D12Device* device, int sourceWidth, int sourceHeight, int outputWidth,
              int outputHeight, mw::native::convert::ScaleFilter filter, bool typedUav,
              std::string& error);

    /// Uploads a pointer shape; the list that draws first runs the copy.
    bool setCursor(ID3D12GraphicsCommandList* list, const mw::native::capture::CursorState& cursor,
                   std::string& error);

    /// Records the conversion of @p source (BGRA8, in COMMON) into output(),
    /// which ends in COMMON.
    bool record(ID3D12GraphicsCommandList* list, ID3D12Resource* source,
                const mw::native::capture::CursorState& cursor,
                const mw::native::convert::CursorDraw& draw, std::string& error);

    ID3D12Resource* output() const { return m_Output.Get(); }
    bool typedUav() const { return m_TypedUav; }

private:
    bool compile(const std::string& source, const char* entry,
                 const std::vector<std::pair<std::string, std::string>>& defines,
                 Microsoft::WRL::ComPtr<ID3DBlob>& blob, std::string& error);
    bool pso(ID3DBlob* cs, Microsoft::WRL::ComPtr<ID3D12PipelineState>& out, std::string& error);
    D3D12_GPU_DESCRIPTOR_HANDLE table(ID3D12Resource* source, DXGI_FORMAT sourceFormat, bool cursor,
                                      ID3D12Resource* target, DXGI_FORMAT targetFormat,
                                      UINT targetPlane);
    void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                    D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES next,
                    UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);

    Microsoft::WRL::ComPtr<ID3D12Device> m_Device;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_Root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_ScaleH;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_ScaleV;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_Luma;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_Chroma;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_Heap;
    UINT m_Stride = 0;
    UINT m_Next = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_Mid;
    D3D12_RESOURCE_STATES m_MidState = D3D12_RESOURCE_STATE_COMMON;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_Scaled;
    D3D12_RESOURCE_STATES m_ScaledState = D3D12_RESOURCE_STATE_COMMON;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_Luma8;
    D3D12_RESOURCE_STATES m_Luma8State = D3D12_RESOURCE_STATE_COMMON;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_Chroma8;
    D3D12_RESOURCE_STATES m_Chroma8State = D3D12_RESOURCE_STATE_COMMON;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_Output;
    D3D12_RESOURCE_STATES m_OutputState = D3D12_RESOURCE_STATE_COMMON;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_CursorPixels;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_CursorInvert;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_CursorUpload;

    mw::native::convert::ConvertGeometry m_Geometry;
    int m_SourceWidth = 0;
    int m_SourceHeight = 0;
    bool m_TypedUav = false;
};

} // namespace lab
