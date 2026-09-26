/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#if defined(_WIN32)
#include "encode/HevcDpb.h"
#include "encode/HevcSliceParser.h"
#include "encode/windows/d3d12/VideoEncode12.h"
#include "platform/windows/d3d12/D3d12Device.h"

#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace mw::native;
using namespace mw::native::encode;
using Microsoft::WRL::ComPtr;

namespace {

/// Pictures into an NV12/P010 texture from the CPU, on a DIRECT queue with a
/// fence of its own — the encoder waits for it on the GPU, as it waits for
/// the conversion in the product.
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

    /// A moving picture: a diagonal ramp scrolling, and a bright square
    /// crossing it — enough motion for every P to carry something.
    uint64_t upload(ID3D12Resource* target, int frame, bool tenBit, std::string& error)
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
                    int v;
                    if (plane == 0) {
                        const bool square =
                            (x + static_cast<UINT>(frame) * 12) % 1920 < 200 && y % 1088 < 200;
                        v = square ? 235
                                   : 16 + static_cast<int>(
                                              (x + 2 * y + static_cast<UINT>(frame) * 8) % 200);
                    } else {
                        v = 128 + ((x + static_cast<UINT>(frame)) % 32) - 16;
                    }
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

ComPtr<ID3D12Resource> picture(ID3D12Device* device, int width, int height, bool tenBit)
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

/// The slices of @p out read with the encoder's parameter sets.
std::vector<HevcSliceFields> slicesOf(const EncoderOutput& out, const VideoEncode12& encoder,
                                      std::string& error)
{
    const std::vector<uint8_t>& headers = encoder.parameterSets();
    HevcSpsFields sps;
    HevcPpsFields pps;
    for (const HevcNalUnit& u : hevcNalUnits(headers.data(), headers.size())) {
        if (u.type() == 33) parseHevcSps(u.data, u.size, sps);
        if (u.type() == 34) parseHevcPps(u.data, u.size, pps);
    }
    std::vector<HevcSliceFields> slices;
    for (const HevcNalUnit& u : hevcNalUnits(out.data, out.size)) {
        if (u.type() > 31) continue;
        HevcSliceFields f;
        error = parseHevcSliceHeader(u.data, u.size, sps, pps, f);
        if (!error.empty()) return {};
        slices.push_back(f);
    }
    return slices;
}

void runOn(const std::shared_ptr<d3d12::D3d12Device>& device, bool tenBit)
{
    VideoEncode12 encoder;
    std::string error;
    EncoderTuning tuning;
    if (!encoder.init(device, Codec::Hevc, 1920, 1080, 60, 20000, tenBit, false, tuning, error)) {
        std::fprintf(stderr, "  %s: %s\n", device->name().c_str(), error.c_str());
        CHECK(false);
        return;
    }
    CHECK_EQ(encoder.codedWidth(), 1920);
    CHECK_EQ(encoder.codedHeight(), 1088);
    ComPtr<ID3D12Resource> input =
        picture(device->device(), encoder.codedWidth(), encoder.codedHeight(), tenBit);
    Uploader uploader;
    if (!input || !uploader.init(*device, input.Get(), error)) {
        std::fprintf(stderr, "  %s: the test's upload: %s\n", device->name().c_str(),
                     error.c_str());
        CHECK(false);
        return;
    }

    std::ofstream dump;
    if (const char* dir = std::getenv("MW_TEST_DUMP_HEVC")) {
        std::string name = device->name();
        for (char& c : name)
            if (!std::isalnum(static_cast<unsigned char>(c))) c = '-';
        dump.open(std::string(dir) + "\\ve12-" + name + (tenBit ? "-main10" : "") + ".hevc",
                  std::ios::binary);
    }

    // The same DPB, run beside the encoder: what the driver was asked for.
    HevcDpb expected(encoder.setup().dpbCapacity, 60);
    const int frames = tenBit ? 8 : 40;
    int keyframes = 0, errors = 0, wrongPoc = 0, wrongReference = 0;
    for (int n = 0; n < frames; ++n) {
        encoder.releaseOutput(); // the last picture's buffer, read below
        const bool force = n == 30;
        if (n == 20) {
            // Frame 18 never arrived: the next picture reaches back past it.
            std::string why;
            CHECK(encoder.invalidateReference(18, why));
            expected.invalidate(18);
        }
        const HevcDpb::Plan plan = expected.plan(static_cast<uint32_t>(n), force);
        const uint64_t ready = uploader.upload(input.Get(), n, tenBit, error);
        EncoderOutput out;
        if (!ready || !encoder.encode(input.Get(), uploader.fence.fence(), ready, force,
                                      static_cast<uint32_t>(n), out, error)) {
            std::fprintf(stderr, "  %s, frame %d: %s\n", device->name().c_str(), n, error.c_str());
            ++errors;
            break;
        }
        expected.encoded(plan);
        if (dump.is_open())
            dump.write(reinterpret_cast<const char*>(out.data),
                       static_cast<std::streamsize>(out.size));
        CHECK_EQ(out.keyframe, plan.idr);
        if (out.keyframe) {
            ++keyframes;
            // The parameter sets go out in front of the slices, in one run.
            const std::vector<uint8_t>& headers = encoder.parameterSets();
            CHECK(out.size > headers.size());
            CHECK(std::memcmp(out.data, headers.data(), headers.size()) == 0);
        }
        const std::vector<HevcSliceFields> slices = slicesOf(out, encoder, error);
        if (slices.empty()) {
            std::fprintf(stderr, "  %s, frame %d: %s\n", device->name().c_str(), n, error.c_str());
            ++errors;
            continue;
        }
        const HevcSliceFields& f = slices.front();
        if (!plan.idr && f.pocLsb != (plan.poc & 0xFF)) ++wrongPoc;
        if (!plan.idr) {
            const int32_t wanted =
                static_cast<int32_t>(plan.references[0].poc) - static_cast<int32_t>(plan.poc);
            bool found = false;
            for (const auto& r : f.shortTerm)
                found = found || (r.used && r.deltaPoc == wanted);
            if (!found) ++wrongReference;
            if (n == 20)
                std::fprintf(stderr,
                             "  %s: after the loss of frame 18, frame 20 predicts from %d "
                             "pictures back\n",
                             device->name().c_str(), -wanted);
        }
    }
    CHECK_EQ(errors, 0);
    CHECK_EQ(wrongPoc, 0);
    CHECK_EQ(wrongReference, 0);
    CHECK_EQ(keyframes, tenBit ? 1 : 2);
    CHECK_EQ(encoder.guardLeft(), 0);

    std::string refused;
    const bool changed = encoder.setBitrate(10000, refused);
    CHECK_EQ(changed, encoder.setup().support.rateReconfigurable);
    if (changed) {
        const uint64_t ready = uploader.upload(input.Get(), frames, tenBit, error);
        EncoderOutput out;
        encoder.releaseOutput();
        CHECK(encoder.encode(input.Get(), uploader.fence.fence(), ready, false,
                             static_cast<uint32_t>(frames), out, error));
    }
    encoder.releaseOutput();
    std::fprintf(stderr, "  %s%s: %d frames, %d keyframes, bitrate change %s\n",
                 device->name().c_str(), tenBit ? " (Main 10)" : "", frames, keyframes,
                 changed ? "taken" : refused.c_str());
    encoder.stop();
}

} // namespace
#endif

// D3D12 Video Encode as the product drives it (plan C4.6), on every GPU that
// has it: the IDR with its parameter sets in front, P pictures numbered in
// order and predicting from the picture HevcDpb chose — after a loss too —
// the header guard passed, a forced keyframe, a bitrate change where the
// driver takes one; a short Main 10 run. MW_TEST_DUMP_HEVC=<dir> writes the
// streams out for ffmpeg.
void run_video_encode12_tests()
{
#if defined(_WIN32)
    SECTION("VideoEncode12 — HEVC through D3D12 Video Encode, on each GPU");

    ComPtr<IDXGIFactory4> factory;
    if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        std::fprintf(stderr, "  no DXGI factory — skipped\n");
        return;
    }
    std::set<uint64_t> seen;
    int ran = 0;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        const uint64_t luid = d3d12::luidValue(desc.AdapterLuid);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || !seen.insert(luid).second) continue;
        std::string error;
        const std::shared_ptr<d3d12::D3d12Device> device =
            d3d12::D3d12Device::forAdapter(adapter.Get(), error);
        ComPtr<ID3D12VideoDevice3> video;
        if (!device || FAILED(device->device()->QueryInterface(IID_PPV_ARGS(&video)))) continue;
        ++ran;
        runOn(device, false);
        runOn(device, true);
    }
    if (ran == 0) std::fprintf(stderr, "  no GPU with D3D12 Video Encode here — skipped\n");
#endif
}
