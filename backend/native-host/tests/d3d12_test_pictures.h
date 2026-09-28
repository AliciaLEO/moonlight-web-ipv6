/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#pragma once

// Pictures for the D3D12 encoders' tests: an NV12 or P010 texture, and an
// uploader that writes a moving test pattern into it on a queue of its own,
// behind a fence the encoder waits for on the GPU — as it waits for the
// conversion in the product. Shared by video_encode12 and vendor_encode12.

#if defined(_WIN32)
#include "platform/windows/d3d12/D3d12Device.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace d3d12_test {

using Microsoft::WRL::ComPtr;
namespace d3d12 = mw::native::d3d12;

/// The test pattern's value at (@p x, @p y) of @p plane in picture @p frame:
/// a diagonal ramp scrolling and a bright square crossing it on the luma —
/// enough motion for every P to carry something — or a flat grey (@p flat),
/// which costs next to nothing. @p x counts bytes on the chroma plane.
inline int patternValue(unsigned plane, unsigned x, unsigned y, int frame, bool flat)
{
    if (flat) return 128;
    if (plane == 0) {
        const bool square = (x + static_cast<unsigned>(frame) * 12) % 1920 < 200 && y % 1088 < 200;
        return square ? 235
                      : 16 + static_cast<int>((x + 2 * y + static_cast<unsigned>(frame) * 8) % 200);
    }
    return 128 + static_cast<int>((x + static_cast<unsigned>(frame)) % 32) - 16;
}

/// Picture @p frame as raw 8-bit NV12, @p width x @p height, the pattern the
/// uploader writes — the input half of scripts/bench/hevc-psnr.py's proof.
inline std::vector<uint8_t> nv12Frame(int width, int height, int frame)
{
    std::vector<uint8_t> out(static_cast<size_t>(width) * height * 3 / 2);
    uint8_t* p = out.data();
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            *p++ = static_cast<uint8_t>(
                patternValue(0, static_cast<unsigned>(x), static_cast<unsigned>(y), frame, false));
    for (int y = 0; y < height / 2; ++y)
        for (int x = 0; x < width; ++x)
            *p++ = static_cast<uint8_t>(
                patternValue(1, static_cast<unsigned>(x), static_cast<unsigned>(y), frame, false));
    return out;
}

/// Pictures into an NV12/P010 texture from the CPU, on a DIRECT queue with a
/// fence of its own.
struct Uploader
{
    ID3D12Device* device = nullptr;
    d3d12::Queue queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Resource> staging;
    d3d12::GpuFence fence;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT feet[2] = {};
    UINT rows[2] = {};
    UINT64 rowBytes[2] = {};

    bool init(d3d12::D3d12Device& d, ID3D12Resource* target, std::string& error)
    {
        device = d.device();
        d3d12::QueueRequest request;
        request.type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (!d.createQueue(request, queue, error) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                             nullptr, IID_PPV_ARGS(&list))) ||
            FAILED(list->Close()) || !fence.create(device, false, error))
            return false;
        const D3D12_RESOURCE_DESC desc = target->GetDesc();
        UINT64 total = 0;
        device->GetCopyableFootprints(&desc, 0, 2, 0, feet, rows, rowBytes, &total);
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = total;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&staging)));
    }

    /// Picture @p frame of the pattern (patternValue) into @p target; the
    /// fence value the encoder is to wait for, 0 on failure.
    uint64_t upload(ID3D12Resource* target, int frame, bool tenBit, std::string& error,
                    bool flat = false)
    {
        uint8_t* p = nullptr;
        if (FAILED(staging->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return 0;
        const int bytes = tenBit ? 2 : 1;
        for (UINT plane = 0; plane < 2; ++plane) {
            const UINT width = static_cast<UINT>(rowBytes[plane] / bytes);
            for (UINT y = 0; y < rows[plane]; ++y) {
                uint8_t* line = p + feet[plane].Offset +
                                y * static_cast<size_t>(feet[plane].Footprint.RowPitch);
                for (UINT x = 0; x < width; ++x) {
                    const int v = patternValue(plane, x, y, frame, flat);
                    if (tenBit) {
                        const uint16_t w = static_cast<uint16_t>(v << 8);
                        std::memcpy(line + x * 2, &w, 2);
                    } else {
                        line[x] = static_cast<uint8_t>(v);
                    }
                }
            }
        }
        staging->Unmap(0, nullptr);
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = target;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(1, &b);
        for (UINT plane = 0; plane < 2; ++plane) {
            D3D12_TEXTURE_COPY_LOCATION to = {}, from = {};
            to.pResource = target;
            to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.SubresourceIndex = plane;
            from.pResource = staging.Get();
            from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            from.PlacedFootprint = feet[plane];
            list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        }
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        list->ResourceBarrier(1, &b);
        list->Close();
        ID3D12CommandList* lists[] = {list.Get()};
        queue.queue->ExecuteCommandLists(1, lists);
        const uint64_t value = fence.signal(queue.queue.Get(), error);
        // The staging buffer is rewritten for the next picture: wait for this
        // copy (the encoder, not the CPU, waits for it in the product).
        if (value) fence.wait(value, 1000, error);
        return value;
    }
};

inline ComPtr<ID3D12Resource> picture(ID3D12Device* device, int width, int height, bool tenBit)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = static_cast<UINT64>(width);
    rd.Height = static_cast<UINT>(height);
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    rd.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> r;
    device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON,
                                    nullptr, IID_PPV_ARGS(&r));
    return r;
}

} // namespace d3d12_test
#endif
