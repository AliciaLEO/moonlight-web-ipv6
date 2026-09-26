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

#include "LabD3d12.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace lab {

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after, UINT sub)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = sub;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

ComPtr<ID3D12Resource> makeBuffer(ID3D12Device* d, UINT64 size, D3D12_HEAP_TYPE type,
                                  bool systemMemory, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp = {};
    if (systemMemory) {
        hp.Type = D3D12_HEAP_TYPE_CUSTOM;
        hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
        hp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    } else {
        hp.Type = type;
    }
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r));
    return r;
}

ComPtr<ID3D12Resource> makeTexture(ID3D12Device* d, UINT w, UINT h, UINT16 slices,
                                   DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = slices;
    rd.MipLevels = 1;
    rd.Format = format;
    rd.SampleDesc.Count = 1;
    rd.Flags = flags;
    ComPtr<ID3D12Resource> r;
    d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                               IID_PPV_ARGS(&r));
    return r;
}

bool upload(ID3D12Device* d, Direct& direct, ID3D12Resource* texture,
            const std::vector<std::vector<uint8_t>>& planes, std::string& error)
{
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    const UINT count = static_cast<UINT>(planes.size());
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> feet(count);
    std::vector<UINT> rows(count);
    std::vector<UINT64> rowBytes(count);
    UINT64 total = 0;
    d->GetCopyableFootprints(&desc, 0, count, 0, feet.data(), rows.data(), rowBytes.data(), &total);
    ComPtr<ID3D12Resource> staging =
        makeBuffer(d, total, D3D12_HEAP_TYPE_UPLOAD, false, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t* p = nullptr;
    if (!staging || FAILED(staging->Map(0, nullptr, reinterpret_cast<void**>(&p)))) {
        error = "upload buffer";
        return false;
    }
    for (UINT s = 0; s < count; ++s)
        for (UINT y = 0; y < rows[s]; ++y)
            std::memcpy(p + feet[s].Offset + y * static_cast<size_t>(feet[s].Footprint.RowPitch),
                        planes[s].data() + y * static_cast<size_t>(rowBytes[s]),
                        static_cast<size_t>(rowBytes[s]));
    staging->Unmap(0, nullptr);
    ID3D12GraphicsCommandList* l = direct.begin();
    D3D12_RESOURCE_BARRIER b =
        transition(texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    l->ResourceBarrier(1, &b);
    for (UINT s = 0; s < count; ++s) {
        D3D12_TEXTURE_COPY_LOCATION to = {}, from = {};
        to.pResource = texture;
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.SubresourceIndex = s;
        from.pResource = staging.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint = feet[s];
        l->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    l->ResourceBarrier(1, &b);
    return direct.run(error);
}

std::vector<std::vector<uint8_t>> nv12Frame(int w, int h, int f)
{
    std::vector<uint8_t> luma(static_cast<size_t>(w) * h), chroma(static_cast<size_t>(w) * (h / 2));
    uint32_t seed = 12345u + static_cast<uint32_t>(f);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            seed = seed * 1664525u + 1013904223u;
            double v = 110 + 70 * std::sin((x + 12.0 * f) / 37.0) * std::cos((y + 5.0 * f) / 29.0) +
                       static_cast<double>((seed >> 24) & 15) - 8;
            if (((x + 24 * f) / 160 + y / 120) % 5 == 0) v = 230 - (y % 64);
            luma[static_cast<size_t>(y) * w + x] = static_cast<uint8_t>(std::clamp(v, 16.0, 235.0));
        }
    for (int y = 0; y < h / 2; ++y)
        for (int x = 0; x < w / 2; ++x) {
            chroma[static_cast<size_t>(y) * w + 2 * x] =
                static_cast<uint8_t>(128 + 40 * std::sin((x + 6.0 * f) / 23.0));
            chroma[static_cast<size_t>(y) * w + 2 * x + 1] =
                static_cast<uint8_t>(128 + 40 * std::cos((y + 3.0 * f) / 17.0));
        }
    return {luma, chroma};
}

std::vector<uint8_t> desktopFrame(int w, int h, int f)
{
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &out[(static_cast<size_t>(y) * w + x) * 4];
            const int sx = x + 9 * f, sy = y + 4 * f;
            p[0] = static_cast<uint8_t>((sx * 7 + sy * 3) & 0xff);
            p[1] = static_cast<uint8_t>(((sx ^ sy) * 5) & 0xff);
            p[2] = static_cast<uint8_t>((sx + sy * 11) & 0xff);
            p[3] = 255;
        }
    return out;
}

} // namespace lab
