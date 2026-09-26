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

// The D3D12 plumbing the probes share: buffers and textures, a one-shot DIRECT
// queue for uploads, and the synthetic pictures they encode or convert.

#include "Lab.h"

#include "platform/windows/d3d12/D3d12Device.h"

#include <d3d12.h>

#include <cstdint>
#include <string>
#include <vector>

namespace lab {

namespace d3d12 = mw::native::d3d12;

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after,
                                  UINT sub = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);

/// A buffer; @p systemMemory = a CPU-cached L0 custom heap the video engine
/// writes and the CPU reads without a copy (the first attempt's way).
ComPtr<ID3D12Resource> makeBuffer(ID3D12Device* d, UINT64 size, D3D12_HEAP_TYPE type,
                                  bool systemMemory, D3D12_RESOURCE_STATES state);

ComPtr<ID3D12Resource> makeTexture(ID3D12Device* d, UINT w, UINT h, UINT16 slices,
                                   DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags);

/// A one-shot DIRECT list, run and waited for: the uploads.
struct Direct
{
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    d3d12::GpuFence fence;

    bool init(d3d12::D3d12Device& device, std::string& error)
    {
        d3d12::Queue q;
        d3d12::QueueRequest request;
        request.priority = d3d12::QueuePriority::High;
        if (!device.createQueue(request, q, error)) return false;
        queue = q.queue;
        if (FAILED(device.device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                           IID_PPV_ARGS(&allocator))) ||
            FAILED(device.device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                      allocator.Get(), nullptr,
                                                      IID_PPV_ARGS(&list))) ||
            FAILED(list->Close())) {
            error = "direct list";
            return false;
        }
        return fence.create(device.device(), false, error);
    }
    ID3D12GraphicsCommandList* begin()
    {
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
        return list.Get();
    }
    bool run(std::string& error)
    {
        list->Close();
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        const uint64_t v = fence.signal(queue.Get(), error);
        return v && fence.wait(v, 10000, error) == d3d12::GpuFence::Wait::Done;
    }
};

/// Uploads @p planes (rows packed) into every subresource of @p texture, left
/// in COMMON.
bool upload(ID3D12Device* d, Direct& direct, ID3D12Resource* texture,
            const std::vector<std::vector<uint8_t>>& planes, std::string& error);

/// Moving synthetic pictures: NV12 planes, or a BGRA desktop, different enough
/// from frame to frame that P frames carry real motion.
std::vector<std::vector<uint8_t>> nv12Frame(int w, int h, int f);
std::vector<uint8_t> desktopFrame(int w, int h, int f);

} // namespace lab
