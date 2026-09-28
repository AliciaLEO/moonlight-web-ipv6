/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#if defined(_WIN32)
#include "capture/windows/DxgiDuplication.h"
#include "mw/native/NativeHost.h"
#include "platform/windows/d3d12/DdaInterop.h"

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace mw::native;
using Microsoft::WRL::ComPtr;

namespace {

/// A D3D12 list that copies the top-left @p w × @p h of @p texture into a
/// readback buffer, bracketed by the barriers a shared surface needs (it is in
/// COMMON for the other API before and after).
struct Readback
{
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Resource> buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};

    bool record(ID3D12Device* device, ID3D12Resource* texture, UINT w, UINT h)
    {
        D3D12_RESOURCE_DESC desc = texture->GetDesc();
        desc.Width = w;
        desc.Height = h;
        UINT64 total = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = total;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&buffer))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                             nullptr, IID_PPV_ARGS(&list))))
            return false;

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier);

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = buffer.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = texture;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        const D3D12_BOX box = {0, 0, 0, w, h, 1};
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
        return SUCCEEDED(list->Close());
    }

    /// The copied pixels, @p bpp bytes each, rows packed.
    std::vector<uint8_t> pixels(UINT w, UINT h, UINT bpp)
    {
        std::vector<uint8_t> out;
        void* mapped = nullptr;
        // The whole buffer: its last row is not padded to the pitch, so pitch
        // × rows would run past its end, and Map refuses that.
        const D3D12_RANGE range = {0, static_cast<SIZE_T>(buffer->GetDesc().Width)};
        if (FAILED(buffer->Map(0, &range, &mapped))) return out;
        out.resize(static_cast<size_t>(w) * h * bpp);
        for (UINT y = 0; y < h; ++y)
            std::memcpy(&out[static_cast<size_t>(y) * w * bpp],
                        static_cast<const uint8_t*>(mapped) + footprint.Offset +
                            static_cast<size_t>(y) * footprint.Footprint.RowPitch,
                        static_cast<size_t>(w) * bpp);
        const D3D12_RANGE none = {0, 0};
        buffer->Unmap(0, &none);
        return out;
    }
};

/// The top-left @p w × @p h of a D3D11 texture of @p bpp bytes per pixel,
/// through a staging copy made on @p context now.
std::vector<uint8_t> read11(ID3D11Device* device, ID3D11DeviceContext* context,
                            ID3D11Texture2D* texture, UINT w, UINT h, UINT bpp)
{
    std::vector<uint8_t> out;
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    desc.Width = w;
    desc.Height = h;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return out;
    const D3D11_BOX box = {0, 0, 0, w, h, 1};
    context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture, 0, &box);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return out;
    out.resize(static_cast<size_t>(w) * h * bpp);
    for (UINT y = 0; y < h; ++y)
        std::memcpy(&out[static_cast<size_t>(y) * w * bpp],
                    static_cast<const uint8_t*>(mapped.pData) +
                        static_cast<size_t>(y) * mapped.RowPitch,
                    static_cast<size_t>(w) * bpp);
    context->Unmap(staging.Get(), 0);
    return out;
}

void warpRoundTrip()
{
    SECTION("DDA interop — fences both ways between D3D11 and D3D12, on WARP");

    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> warp;
    if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) {
        std::fprintf(stderr, "  no WARP adapter here — skipped\n");
        return;
    }
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(::D3D11CreateDevice(warp.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
                                   &device11, nullptr, &context))) {
        std::fprintf(stderr, "  no D3D11 on WARP — skipped\n");
        return;
    }
    std::string error;
    std::shared_ptr<d3d12::D3d12Device> device = d3d12::D3d12Device::forAdapter(warp.Get(), error);
    if (!device) {
        std::fprintf(stderr, "  no D3D12 on WARP (%s) — skipped\n", error.c_str());
        return;
    }

    d3d12::DdaInterop interop;
    std::string refusal;
    if (!interop.bind(device11.Get(), device, refusal)) {
        std::fprintf(stderr, "  WARP refuses the fences (%s) — skipped\n", refusal.c_str());
        return;
    }
    CHECK(interop.bound());
    CHECK(interop.captureDevice() != nullptr);

    // A shareable surface, as Desktop Duplication hands out, with one picture
    // in it — and another written by D3D11 once D3D12 has had its turn.
    constexpr UINT kW = 16;
    constexpr UINT kH = 8;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = kW;
    desc.Height = kH;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    ComPtr<ID3D11Texture2D> surface;
    CHECK(SUCCEEDED(device11->CreateTexture2D(&desc, nullptr, &surface)));
    if (!surface) return;
    std::vector<uint8_t> first(kW * kH * 4), second(kW * kH * 4);
    for (size_t i = 0; i < first.size(); ++i) {
        first[i] = static_cast<uint8_t>(i * 7);
        second[i] = static_cast<uint8_t>(255 - i * 3);
    }
    // The writes are GPU copies, as the compositor's are. An UpdateSubresource
    // would not do: WARP carries it out on the CPU at once when D3D11 sees
    // nothing pending on the texture — outside the context's order, and so
    // outside what a fence wait on that context can hold back.
    desc.MiscFlags = 0;
    D3D11_SUBRESOURCE_DATA seed = {};
    seed.SysMemPitch = kW * 4;
    seed.pSysMem = first.data();
    ComPtr<ID3D11Texture2D> firstPicture, secondPicture;
    CHECK(SUCCEEDED(device11->CreateTexture2D(&desc, &seed, &firstPicture)));
    seed.pSysMem = second.data();
    CHECK(SUCCEEDED(device11->CreateTexture2D(&desc, &seed, &secondPicture)));
    if (!firstPicture || !secondPicture) return;
    context->CopyResource(surface.Get(), firstPicture.Get());

    ID3D12Resource* opened = interop.open(surface.Get(), error);
    CHECK(opened != nullptr);
    CHECK(interop.open(surface.Get(), error) == opened);
    CHECK_EQ(interop.openedSurfaces(), static_cast<size_t>(1));
    if (!opened) return;

    d3d12::Queue queue;
    d3d12::QueueRequest request;
    CHECK(device->createQueue(request, queue, error));
    Readback readback;
    CHECK(readback.record(device->device(), opened, kW, kH));

    // §3.1: signal A after the "acquire", the queue waits for it, reads, and
    // signals B; the capture context waits for B before its next write.
    const uint64_t a = interop.signalAcquired(context.Get(), d3d12::DdaSync::Gpu, error);
    CHECK(a == 1);
    CHECK(interop.conversionWaits(queue.queue.Get(), a, error));
    ID3D12CommandList* lists[] = {readback.list.Get()};
    queue.queue->ExecuteCommandLists(1, lists);
    const uint64_t b = interop.conversionSignals(queue.queue.Get(), error);
    CHECK(b == 1);
    CHECK(interop.beforeRelease(context.Get(), b, d3d12::DdaSync::Gpu, 1000, error));
    context->CopyResource(surface.Get(), secondPicture.Get());
    context->Flush();

    CHECK(interop.converted().wait(b, 2000, error) == d3d12::GpuFence::Wait::Done);
    CHECK(readback.pixels(kW, kH, 4) == first);
    CHECK(read11(device11.Get(), context.Get(), surface.Get(), kW, kH, 4) == second);

    // The bench variants: nothing at all, and the CPU holding the release.
    CHECK(interop.signalAcquired(context.Get(), d3d12::DdaSync::None, error) == 0);
    CHECK(interop.conversionWaits(queue.queue.Get(), 0, error));
    CHECK(interop.beforeRelease(context.Get(), 7, d3d12::DdaSync::None, 0, error));
    const uint64_t b2 = interop.conversionSignals(queue.queue.Get(), error);
    CHECK(b2 == 2);
    CHECK(interop.beforeRelease(context.Get(), b2, d3d12::DdaSync::Cpu, 1000, error));
    CHECK(interop.converted().completed() >= b2);

    // A restart: everything of the old capture device is let go.
    interop.unbind();
    CHECK(!interop.bound());
    CHECK_EQ(interop.openedSurfaces(), static_cast<size_t>(0));
    CHECK(interop.open(surface.Get(), error) == nullptr);
}

void realDuplication()
{
    SECTION("DDA interop — a real Desktop Duplication frame, read by D3D12");

    const Capabilities caps = NativeHost::probe();
    if (caps.displays.empty()) {
        std::fprintf(stderr, "  skipped: no display attached\n");
        return;
    }
    const DisplayInfo* target = &caps.displays.front();
    for (const DisplayInfo& d : caps.displays)
        if (d.primary) target = &d;
    const GpuInfo* gpu = caps.gpuFor(*target);
    if (!gpu) {
        std::fprintf(stderr, "  skipped: display %d names no GPU\n", target->id);
        return;
    }
    unsigned outputIndex = 0;
    for (const DisplayInfo& d : caps.displays) {
        if (d.id == target->id) break;
        if (d.gpuId == target->gpuId) ++outputIndex;
    }
    capture::DxgiDuplication duplication(gpu->nativeHandle, outputIndex);
    std::string error;
    if (!duplication.start(error)) {
        std::fprintf(stderr, "  skipped: %s\n", error.c_str());
        return;
    }
    std::shared_ptr<d3d12::D3d12Device> device =
        d3d12::D3d12Device::forAdapter(gpu->nativeHandle, error);
    if (!device) {
        std::fprintf(stderr, "  skipped: %s\n", error.c_str());
        return;
    }

    // On real hardware the handshake is the route's precondition: probed on
    // the RTX, the Arc and the AMD iGPU (d3d12-lab caps, 26/09/2026).
    d3d12::DdaInterop interop;
    std::string refusal;
    CHECK(interop.bind(duplication.device(), device, refusal));
    if (!interop.bound()) {
        std::fprintf(stderr, "  refused: %s\n", refusal.c_str());
        return;
    }
    d3d12::Queue queue;
    d3d12::QueueRequest request;
    CHECK(device->createQueue(request, queue, error));

    // A few frames: each read by D3D12 through the handshake and by D3D11
    // directly, before the release — the same pixels, or D3D12 read another
    // frame than the one acquired.
    constexpr UINT kW = 64;
    constexpr UINT kH = 32;
    // BGRA8 from an SDR desktop, FP16 from an HDR one.
    const UINT bpp = duplication.format() == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
    int frames = 0, same = 0;
    for (int attempt = 0; attempt < 40 && frames < 3; ++attempt) {
        capture::CapturedFrame frame;
        const capture::AcquireStatus status = duplication.acquire(100, frame);
        if (status != capture::AcquireStatus::Ok) continue;
        ++frames;
        ID3D12Resource* opened = interop.open(frame.texture, error);
        CHECK(opened != nullptr);
        Readback readback;
        const bool recorded = opened && readback.record(device->device(), opened, kW, kH);
        CHECK(recorded);
        if (recorded) {
            const uint64_t a =
                interop.signalAcquired(duplication.context(), d3d12::DdaSync::Gpu, error);
            CHECK(a > 0);
            CHECK(interop.conversionWaits(queue.queue.Get(), a, error));
            ID3D12CommandList* lists[] = {readback.list.Get()};
            queue.queue->ExecuteCommandLists(1, lists);
            const uint64_t b = interop.conversionSignals(queue.queue.Get(), error);
            CHECK(b > 0);
            const std::vector<uint8_t> direct =
                read11(duplication.device(), duplication.context(), frame.texture, kW, kH, bpp);
            CHECK(
                interop.beforeRelease(duplication.context(), b, d3d12::DdaSync::Gpu, 1000, error));
            duplication.release();
            CHECK(interop.converted().wait(b, 2000, error) == d3d12::GpuFence::Wait::Done);
            if (!direct.empty() && readback.pixels(kW, kH, bpp) == direct) ++same;
        } else {
            duplication.release();
        }
    }
    std::fprintf(stderr,
                 "  %d frame(s) through the handshake, %d read the same by both APIs, "
                 "%zu surface(s) opened\n",
                 frames, same, interop.openedSurfaces());
    CHECK(frames == 0 || same == frames);
    std::string gone;
    CHECK(!device->removed(gone));
    interop.unbind();
    duplication.stop();
}

} // namespace
#endif

// DdaInterop (plan pipeline-video-d3d12-v2, C2.2-C2.3): the capture's D3D11
// device and the pipeline's D3D12 device ordered by two shared fences — D3D12
// reads what D3D11 wrote before the signal and not what it wrote after the
// wait — on WARP, then on a real duplicated display where there is one.
void run_dda_interop_tests()
{
#if defined(_WIN32)
    warpRoundTrip();
    realDuplication();
#endif
}
