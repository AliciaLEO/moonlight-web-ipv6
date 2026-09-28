/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#if defined(_WIN32)
#include "d3d12_test_pictures.h"
#include "encode/windows/d3d12/AmfEncoder12.h"
#include "encode/windows/d3d12/NvencEncoder12.h"
#include "mw/native/NativeHost.h"
#include "platform/windows/IndirectDisplay.h"
#include "platform/windows/d3d12/D3d12Device.h"

#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>
#endif

#include <cstdio>

// The vendors' own encoders fed D3D12 pictures (plan pipeline-video-d3d12-v2,
// Phase 7), as the D3D12 chain drives them: a picture written on a queue of
// the test's own, behind a fence the encoder waits for on the GPU; the
// bitstream read once the encoder's fence says it is written. Checked on each:
// a keyframe first and where one is forced, the parameter sets on it, a loss
// named, a bitrate change, Main 10, and the other codecs its D3D11 path codes.
//
// MW_TEST_DUMP_HEVC=<dir> writes each stream beside its input pictures, for
// the pixel proof: python scripts/bench/hevc-psnr.py <stream> <input.nv12>
// 1920x1080 1920x1080 (frame n of the stream against input n).

#if defined(_WIN32)
using namespace mw::native;
using namespace mw::native::encode;
using d3d12_test::picture;
using d3d12_test::Uploader;
using Microsoft::WRL::ComPtr;

namespace {

/// The NAL (H.264, HEVC) or OBU (AV1) types in @p data, in order.
std::vector<int> unitTypes(const uint8_t* data, size_t size, Codec codec)
{
    std::vector<int> types;
    if (codec == Codec::Av1) {
        // Low-overhead OBUs: a header byte, an extension byte when flagged, a
        // LEB128 size (always present in NVENC's and AMF's output).
        size_t i = 0;
        while (i < size) {
            const uint8_t h = data[i++];
            types.push_back((h >> 3) & 0xF);
            if (h & 0x4) ++i;
            if (!(h & 0x2)) break;
            uint64_t length = 0;
            for (int shift = 0; i < size && shift < 56; shift += 7) {
                const uint8_t b = data[i++];
                length |= static_cast<uint64_t>(b & 0x7F) << shift;
                if (!(b & 0x80)) break;
            }
            i += static_cast<size_t>(length);
        }
        return types;
    }
    for (size_t i = 0; i + 3 < size; ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            const uint8_t h = data[i + 3];
            types.push_back(codec == Codec::Hevc ? (h >> 1) & 0x3F : h & 0x1F);
            i += 3;
        }
    }
    return types;
}

/// Whether the parameter sets a decoder configures itself from are in front
/// of a keyframe: VPS, SPS, PPS (HEVC), SPS and PPS (H.264), the sequence
/// header (AV1).
bool carriesParameterSets(const std::vector<int>& types, Codec codec)
{
    const auto has = [&](int t) { return std::find(types.begin(), types.end(), t) != types.end(); };
    switch (codec) {
    case Codec::Hevc: return has(32) && has(33) && has(34);
    case Codec::H264: return has(7) && has(8);
    case Codec::Av1: return has(1);
    }
    return false;
}

std::string fileSafe(std::string name)
{
    for (char& c : name)
        if (!std::isalnum(static_cast<unsigned char>(c))) c = '-';
    return name;
}

const char* extensionOf(Codec codec)
{
    switch (codec) {
    case Codec::Hevc: return ".hevc";
    case Codec::H264: return ".h264";
    case Codec::Av1: return ".obu";
    }
    return ".bin";
}

/// @p frames pictures through @p encoder, a keyframe forced at 30 and frame
/// 18 named lost at 20, then a bitrate change.
void runOn(IVideoEncoder12& encoder, const std::shared_ptr<d3d12::D3d12Device>& device, Codec codec,
           bool tenBit, int frames)
{
    const std::string label = device->name() + " " + encoder.describe() + " " + toString(codec) +
                              (tenBit ? " Main10" : "");
    std::string error;
    EncoderTuning tuning;
    if (!encoder.init(device, codec, 1920, 1080, 60, 20000, tenBit, false, tuning, error)) {
        std::fprintf(stderr, "  %s: %s\n", label.c_str(), error.c_str());
        CHECK(false);
        return;
    }
    CHECK_EQ(encoder.codedWidth(), 1920);
    CHECK_EQ(encoder.codedHeight(), 1080);
    ComPtr<ID3D12Resource> input =
        picture(device->device(), encoder.codedWidth(), encoder.codedHeight(), tenBit);
    Uploader uploader;
    if (!input || !uploader.init(*device, input.Get(), error)) {
        std::fprintf(stderr, "  %s: the test's upload: %s\n", label.c_str(), error.c_str());
        CHECK(false);
        return;
    }

    std::ofstream dump;
    std::ofstream dumpInput;
    if (const char* dir = std::getenv("MW_TEST_DUMP_HEVC")) {
        const std::string base = std::string(dir) + "\\vendor12-" + fileSafe(label);
        dump.open(base + extensionOf(codec), std::ios::binary);
        if (!tenBit) dumpInput.open(base + ".nv12", std::ios::binary);
    }

    int keyframes = 0, unexpectedKeyframes = 0, errors = 0, noHeaders = 0, badQp = 0;
    int lowQp = 99, highQp = -1;
    size_t bytes = 0;
    for (int n = 0; n < frames; ++n) {
        encoder.releaseOutput();
        // The first picture asked as a keyframe, as a session's is: AMF's
        // H.264 puts its parameter sets on a forced IDR only (AmfConfig).
        const bool force = n == 0 || n == 30;
        if (n == 20 && encoder.supportsReferenceInvalidation()) {
            std::string why;
            CHECK(encoder.invalidateReference(18, why));
        }
        uint64_t released = 0;
        ID3D12Fence* after = encoder.inputReleased(released);
        const uint64_t ready =
            uploader.upload(input.Get(), n, tenBit, error, false, after, released);
        EncoderOutput out;
        if (!ready || !encoder.encode(input.Get(), uploader.fence.fence(), ready, force,
                                      static_cast<uint32_t>(n), out, error)) {
            std::fprintf(stderr, "  %s, frame %d: %s\n", label.c_str(), n, error.c_str());
            ++errors;
            break;
        }
        if (dump.is_open())
            dump.write(reinterpret_cast<const char*>(out.data),
                       static_cast<std::streamsize>(out.size));
        if (dumpInput.is_open()) {
            const std::vector<uint8_t> nv12 = d3d12_test::nv12Frame(1920, 1080, n);
            dumpInput.write(reinterpret_cast<const char*>(nv12.data()),
                            static_cast<std::streamsize>(nv12.size()));
        }
        bytes += out.size;
        CHECK(out.size > 0);
        if (n == 0 || force) CHECK(out.keyframe);
        if (out.keyframe) {
            ++keyframes;
            if (n != 0 && !force) ++unexpectedKeyframes;
            if (!carriesParameterSets(unitTypes(out.data, out.size, codec), codec)) ++noHeaders;
        }
        if (out.avgQp != -1) {
            if (out.avgQp < 0 || out.avgQp > 255) ++badQp;
            lowQp = (std::min)(lowQp, out.avgQp);
            highQp = (std::max)(highQp, out.avgQp);
        }
    }
    CHECK_EQ(errors, 0);
    CHECK_EQ(noHeaders, 0);
    CHECK_EQ(badQp, 0);
    CHECK_EQ(unexpectedKeyframes, 0);
    CHECK_EQ(keyframes, frames > 30 ? 2 : 1);

    std::string refused;
    const bool changed = encoder.setBitrate(10000, refused);
    CHECK(changed);
    if (changed) {
        encoder.releaseOutput();
        uint64_t released = 0;
        ID3D12Fence* after = encoder.inputReleased(released);
        const uint64_t ready =
            uploader.upload(input.Get(), frames, tenBit, error, false, after, released);
        EncoderOutput out;
        CHECK(encoder.encode(input.Get(), uploader.fence.fence(), ready, false,
                             static_cast<uint32_t>(frames), out, error));
    }
    encoder.releaseOutput();
    std::fprintf(stderr,
                 "  %s: %d frames, %d keyframes, %.1f KB/frame, QP %d..%d, bitrate change %s\n",
                 label.c_str(), frames, keyframes,
                 static_cast<double>(bytes) / 1024.0 / (std::max)(frames, 1), lowQp, highQp,
                 changed ? "taken" : refused.c_str());
    encoder.stop();
}

/// The codecs @p luid's GPU encodes, and whether in 10 bits, as the probe
/// found them for its D3D11 path — the same silicon.
const GpuInfo* gpuFor(const Capabilities& caps, uint64_t luid)
{
    for (const GpuInfo& g : caps.gpus)
        if (g.nativeHandle == luid) return &g;
    return nullptr;
}

} // namespace
#endif

void run_vendor_encode12_tests()
{
    SECTION("Vendor encoders fed D3D12 pictures — NVENC and AMF (D3D12), on each GPU that has one");

#if !defined(_WIN32)
    std::fprintf(stderr, "  skipped: Windows only\n");
#else
    const Capabilities caps = NativeHost::probe();
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
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || !seen.insert(luid).second ||
            mw::native::platform::isIndirectDisplayOnly(desc.AdapterLuid))
            continue;
        const GpuInfo* gpu = gpuFor(caps, luid);
        if (!gpu || gpu->encoders.empty()) continue;
        const EncoderApi api = gpu->encoders.front();
        if (api != EncoderApi::Nvenc && api != EncoderApi::Amf) continue;
        std::string error;
        const std::shared_ptr<d3d12::D3d12Device> device =
            d3d12::D3d12Device::forAdapter(adapter.Get(), error);
        if (!device) {
            std::fprintf(stderr, "  %s: no D3D12 device: %s\n", gpu->name.c_str(), error.c_str());
            continue;
        }
        ++ran;
        for (Codec codec : {Codec::Hevc, Codec::H264, Codec::Av1}) {
            if (std::find(gpu->codecs.begin(), gpu->codecs.end(), codec) == gpu->codecs.end())
                continue;
            const int frames = codec == Codec::Hevc ? 40 : 20;
            if (api == EncoderApi::Nvenc) {
                NvencEncoder12 encoder;
                runOn(encoder, device, codec, false, frames);
                if (gpu->supports10Bit && codec != Codec::H264) {
                    NvencEncoder12 tenBit;
                    runOn(tenBit, device, codec, true, 8);
                }
            } else {
                // AMF's HDR is not done on either path (AmfConfig).
                AmfEncoder12 encoder;
                runOn(encoder, device, codec, false, frames);
            }
        }
    }
    if (ran == 0) std::fprintf(stderr, "  no NVENC or AMF GPU here — skipped\n");
#endif
}
