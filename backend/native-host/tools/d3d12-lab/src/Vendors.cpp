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

// `vendors` — whether the vendors' own encoders take a D3D12 picture (plan
// pipeline-video-d3d12-v2, C1.3b): NVENC through its D3D12 interface (a
// session on an ID3D12Device, registered NV12 textures and a readback
// bitstream buffer, ordered by fence points), AMF through InitDX12 and
// CreateSurfaceFromDX12Native (the resource's state and fence handed over as
// private data, core/D3D12AMF.h). HEVC, CBR with a one-frame VBV, the product's
// low-latency presets, the same synthetic NV12 as `encode`, so the times read
// against it and against the D3D11 path of the product.

#include "Vendors.h"

#include "Lab.h"
#include "LabD3d12.h"

#include "encode/windows/AmfApi.h"
#include "encode/windows/NvencApi.h"
#include "mw/native/NativeHost.h"
#include "platform/windows/d3d12/D3d12Device.h"

#include <components/VideoEncoderHEVC.h>
#include <core/D3D12AMF.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace lab {
namespace {

using namespace mw::native;

struct Options
{
    std::string adapter;
    int width = 1920, height = 1080;
    int fps = 60;
    int frames = 300;
    int kbps = 20000;
    bool nvenc = true;
    bool amf = true;
};

void say(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::fflush(stdout);
}

struct Result
{
    std::string name;
    std::string refused;
    std::vector<double> ms;
    size_t bytes = 0;
    int frames = 0;
    int errors = 0;
};

void print(const Result& r)
{
    if (!r.refused.empty()) {
        say("%-12s refused: %s\n", r.name.c_str(), r.refused.c_str());
        return;
    }
    std::vector<double> v = r.ms;
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v)
        sum += x;
    const auto at = [&](double q) {
        return v.empty() ? 0.0 : v[std::min(v.size() - 1, static_cast<size_t>(v.size() * q))];
    };
    say("%-12s %d frames, %d errors, %.1f KB/frame | ms per frame: mean %.2f  p50 %.2f  p99 "
        "%.2f  max %.2f\n",
        r.name.c_str(), r.frames, r.errors,
        r.frames ? static_cast<double>(r.bytes) / 1024.0 / r.frames : 0.0,
        v.empty() ? 0.0 : sum / v.size(), at(0.5), at(0.99), v.empty() ? 0.0 : v.back());
}

void pace(HANDLE timer, int64_t& next, int64_t periodUs)
{
    next += periodUs;
    const int64_t now = nowUs();
    if (next <= now) {
        next = now;
        return;
    }
    LARGE_INTEGER due = {};
    due.QuadPart = -(next - now) * 10;
    if (timer && ::SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
        ::WaitForSingleObject(timer, INFINITE);
}

// ── NVENC, D3D12 interface ──────────────────────────────────────────────────

void runNvenc(d3d12::D3d12Device& device, const std::vector<ComPtr<ID3D12Resource>>& inputs,
              const Options& o, Result& r)
{
    r.name = "NVENC-D3D12";
    const encode::NvencApi* api = encode::NvencApi::instance();
    if (!api || !api->available()) {
        r.refused = api ? api->unavailableReason() : "no NVENC";
        return;
    }
    const NV_ENCODE_API_FUNCTION_LIST& fn = api->fn();
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open = {};
    open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    open.device = device.device();
    open.apiVersion = NVENCAPI_VERSION;
    void* encoder = nullptr;
    NVENCSTATUS status = fn.nvEncOpenEncodeSessionEx(&open, &encoder);
    if (status != NV_ENC_SUCCESS) {
        r.refused =
            std::string("session on the D3D12 device: ") + encode::NvencApi::statusToString(status);
        return;
    }

    NV_ENC_PRESET_CONFIG preset = {};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    status = fn.nvEncGetEncodePresetConfigEx(encoder, NV_ENC_CODEC_HEVC_GUID, NV_ENC_PRESET_P1_GUID,
                                             NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset);
    NV_ENC_CONFIG config = preset.presetCfg;
    const uint32_t bps = static_cast<uint32_t>(o.kbps) * 1000u;
    config.gopLength = NVENC_INFINITE_GOPLENGTH;
    config.frameIntervalP = 1;
    config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    config.rcParams.averageBitRate = bps;
    config.rcParams.maxBitRate = bps;
    config.rcParams.vbvBufferSize = bps / static_cast<uint32_t>(o.fps);
    config.rcParams.vbvInitialDelay = config.rcParams.vbvBufferSize;
    config.encodeCodecConfig.hevcConfig.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    NV_ENC_INITIALIZE_PARAMS init = {};
    init.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID = NV_ENC_CODEC_HEVC_GUID;
    init.presetGUID = NV_ENC_PRESET_P1_GUID;
    init.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    init.encodeWidth = init.darWidth = init.maxEncodeWidth = static_cast<uint32_t>(o.width);
    init.encodeHeight = init.darHeight = init.maxEncodeHeight = static_cast<uint32_t>(o.height);
    init.frameRateNum = static_cast<uint32_t>(o.fps);
    init.frameRateDen = 1;
    init.enablePTD = 1;
    init.encodeConfig = &config;
    init.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
    if (status == NV_ENC_SUCCESS) status = fn.nvEncInitializeEncoder(encoder, &init);
    if (status != NV_ENC_SUCCESS) {
        r.refused =
            std::string("HEVC P1/ULL on D3D12: ") + encode::NvencApi::statusToString(status);
        fn.nvEncDestroyEncoder(encoder);
        return;
    }

    // The pictures and the bitstream buffer, registered and mapped once.
    std::vector<NV_ENC_REGISTERED_PTR> registered;
    std::vector<NV_ENC_INPUT_PTR> mapped;
    const auto registerResource = [&](ID3D12Resource* resource, uint32_t w, uint32_t h,
                                      NV_ENC_BUFFER_FORMAT format, NV_ENC_BUFFER_USAGE usage,
                                      NV_ENC_INPUT_PTR& out) {
        NV_ENC_REGISTER_RESOURCE reg = {};
        reg.version = NV_ENC_REGISTER_RESOURCE_VER;
        reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        reg.width = w;
        reg.height = h;
        reg.resourceToRegister = resource;
        reg.bufferFormat = format;
        reg.bufferUsage = usage;
        NVENCSTATUS s = fn.nvEncRegisterResource(encoder, &reg);
        if (s != NV_ENC_SUCCESS) return s;
        registered.push_back(reg.registeredResource);
        NV_ENC_MAP_INPUT_RESOURCE map = {};
        map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        map.registeredResource = reg.registeredResource;
        s = fn.nvEncMapInputResource(encoder, &map);
        if (s != NV_ENC_SUCCESS) return s;
        mapped.push_back(map.mappedResource);
        out = map.mappedResource;
        return NV_ENC_SUCCESS;
    };
    std::vector<NV_ENC_INPUT_PTR> pictures(inputs.size());
    for (size_t i = 0; i < inputs.size() && status == NV_ENC_SUCCESS; ++i)
        status = registerResource(inputs[i].Get(), static_cast<uint32_t>(o.width),
                                  static_cast<uint32_t>(o.height), NV_ENC_BUFFER_FORMAT_NV12,
                                  NV_ENC_INPUT_IMAGE, pictures[i]);
    const UINT64 outputSize = 4u << 20;
    ComPtr<ID3D12Resource> bitstream =
        makeBuffer(device.device(), outputSize, D3D12_HEAP_TYPE_READBACK, false,
                   D3D12_RESOURCE_STATE_COPY_DEST);
    NV_ENC_INPUT_PTR output = nullptr;
    if (status == NV_ENC_SUCCESS && bitstream)
        status = registerResource(bitstream.Get(), static_cast<uint32_t>(outputSize), 1,
                                  NV_ENC_BUFFER_FORMAT_U8, NV_ENC_OUTPUT_BITSTREAM, output);
    d3d12::GpuFence fence;
    std::string error;
    if (status != NV_ENC_SUCCESS || !bitstream || !fence.create(device.device(), false, error)) {
        r.refused = std::string("registering the D3D12 resources: ") +
                    encode::NvencApi::statusToString(status) + " " + error;
    } else {
        HANDLE timer = ::CreateWaitableTimerExW(
            nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        int64_t next = nowUs();
        uint64_t value = 0;
        for (int n = 0; n < o.frames; ++n) {
            pace(timer, next, 1000000 / o.fps);
            NV_ENC_INPUT_RESOURCE_D3D12 in = {};
            in.version = NV_ENC_INPUT_RESOURCE_D3D12_VER;
            in.pInputBuffer = pictures[n % pictures.size()];
            in.inputFencePoint.version = NV_ENC_FENCE_POINT_D3D12_VER;
            in.inputFencePoint.pFence = fence.fence();
            in.inputFencePoint.waitValue = value; // reached: the picture is ready
            in.inputFencePoint.bWait = 1;
            NV_ENC_OUTPUT_RESOURCE_D3D12 out = {};
            out.version = NV_ENC_OUTPUT_RESOURCE_D3D12_VER;
            out.pOutputBuffer = output;
            out.outputFencePoint.version = NV_ENC_FENCE_POINT_D3D12_VER;
            out.outputFencePoint.pFence = fence.fence();
            out.outputFencePoint.signalValue = ++value;
            out.outputFencePoint.bSignal = 1;
            NV_ENC_PIC_PARAMS pic = {};
            pic.version = NV_ENC_PIC_PARAMS_VER;
            pic.inputWidth = static_cast<uint32_t>(o.width);
            pic.inputHeight = static_cast<uint32_t>(o.height);
            pic.inputPitch = static_cast<uint32_t>(o.width);
            pic.inputBuffer = &in;
            pic.outputBitstream = &out;
            pic.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
            pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
            pic.inputTimeStamp = static_cast<uint64_t>(n);

            const int64_t started = nowUs();
            status = fn.nvEncEncodePicture(encoder, &pic);
            if (status != NV_ENC_SUCCESS) {
                if (r.errors++ < 3)
                    say("  NVENC frame %d: %s\n", n, encode::NvencApi::statusToString(status));
                continue;
            }
            // The output fence says the bitstream is written; the lock then
            // only reads the size.
            if (fence.wait(value, 1000, error) != d3d12::GpuFence::Wait::Done) {
                r.refused = "the output fence never came: " + error;
                break;
            }
            NV_ENC_LOCK_BITSTREAM lock = {};
            lock.version = NV_ENC_LOCK_BITSTREAM_VER;
            lock.outputBitstream = &out;
            status = fn.nvEncLockBitstream(encoder, &lock);
            if (status == NV_ENC_SUCCESS) {
                r.bytes += lock.bitstreamSizeInBytes;
                fn.nvEncUnlockBitstream(encoder, &out);
                r.ms.push_back(static_cast<double>(nowUs() - started) / 1000.0);
                ++r.frames;
            } else if (r.errors++ < 3) {
                say("  NVENC lock %d: %s\n", n, encode::NvencApi::statusToString(status));
            }
        }
        if (timer) ::CloseHandle(timer);
    }
    for (NV_ENC_INPUT_PTR m : mapped)
        fn.nvEncUnmapInputResource(encoder, m);
    for (NV_ENC_REGISTERED_PTR g : registered)
        fn.nvEncUnregisterResource(encoder, g);
    fn.nvEncDestroyEncoder(encoder);
}

// ── AMF, InitDX12 ───────────────────────────────────────────────────────────

void runAmf(d3d12::D3d12Device& device, const std::vector<ComPtr<ID3D12Resource>>& inputs,
            const Options& o, Result& r)
{
    r.name = "AMF-DX12";
    const encode::AmfApi* api = encode::AmfApi::instance();
    if (!api || !api->available()) {
        r.refused = api ? api->unavailableReason() : "no AMF";
        return;
    }
    amf::AMFContext* context = nullptr;
    AMF_RESULT result = api->factory()->CreateContext(&context);
    amf::AMFContext2* context2 = nullptr;
    if (result == AMF_OK)
        result =
            context->QueryInterface(amf::AMFContext2::IID(), reinterpret_cast<void**>(&context2));
    if (result == AMF_OK) result = context2->InitDX12(device.device(), amf::AMF_DX12);
    if (result != AMF_OK) {
        r.refused = "InitDX12: " + encode::AmfApi::resultToString(result);
        if (context2) context2->Release();
        if (context) context->Release();
        return;
    }
    amf::AMFComponent* encoder = nullptr;
    result = api->factory()->CreateComponent(context, AMFVideoEncoder_HEVC, &encoder);
    if (result == AMF_OK) {
        // USAGE first: it resets the rest.
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_USAGE,
                             amf_int64(AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY));
        const amf_int64 bps = static_cast<amf_int64>(o.kbps) * 1000;
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD,
                             amf_int64(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR));
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, bps);
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, bps);
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE, bps / o.fps);
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMESIZE,
                             ::AMFConstructSize(o.width, o.height));
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMERATE, ::AMFConstructRate(o.fps, 1));
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, amf_int64(1000000));
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_QUERY_TIMEOUT, amf_int64(50));
        result = encoder->Init(amf::AMF_SURFACE_NV12, o.width, o.height);
    }
    if (result != AMF_OK) {
        r.refused = "the HEVC encoder on a DX12 context: " + encode::AmfApi::resultToString(result);
    } else {
        // Each picture's state and fence, as core/D3D12AMF.h asks: COMMON,
        // and a fence already at the value to wait for.
        d3d12::GpuFence fence;
        std::string error;
        fence.create(device.device(), false, error);
        const UINT64 ready = 0;
        fence.fence()->SetPrivateData(AMFFenceValueGUID, sizeof(ready), &ready);
        for (const auto& t : inputs) {
            const UINT state = D3D12_RESOURCE_STATE_COMMON;
            t->SetPrivateData(AMFResourceStateGUID, sizeof(state), &state);
            t->SetPrivateDataInterface(AMFFenceGUID, fence.fence());
        }
        HANDLE timer = ::CreateWaitableTimerExW(
            nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        int64_t next = nowUs();
        for (int n = 0; n < o.frames; ++n) {
            pace(timer, next, 1000000 / o.fps);
            amf::AMFSurface* surface = nullptr;
            result = context2->CreateSurfaceFromDX12Native(inputs[n % inputs.size()].Get(),
                                                           &surface, nullptr);
            if (result != AMF_OK) {
                r.refused =
                    "CreateSurfaceFromDX12Native: " + encode::AmfApi::resultToString(result);
                break;
            }
            const int64_t started = nowUs();
            result = encoder->SubmitInput(surface);
            surface->Release();
            if (result != AMF_OK) {
                if (r.errors++ < 3)
                    say("  AMF submit %d: %s\n", n, encode::AmfApi::resultToString(result).c_str());
                continue;
            }
            amf::AMFData* data = nullptr;
            for (int tries = 0; tries < 20; ++tries) {
                result = encoder->QueryOutput(&data);
                if (result == AMF_OK && data) break;
            }
            if (!data) {
                if (r.errors++ < 3)
                    say("  AMF output %d: %s\n", n, encode::AmfApi::resultToString(result).c_str());
                continue;
            }
            amf::AMFBuffer* buffer = nullptr;
            if (data->QueryInterface(amf::AMFBuffer::IID(), reinterpret_cast<void**>(&buffer)) ==
                    AMF_OK &&
                buffer) {
                r.bytes += buffer->GetSize();
                buffer->Release();
            }
            data->Release();
            r.ms.push_back(static_cast<double>(nowUs() - started) / 1000.0);
            ++r.frames;
        }
        if (timer) ::CloseHandle(timer);
    }
    if (encoder) {
        encoder->Terminate();
        encoder->Release();
    }
    context2->Release();
    context->Terminate();
    context->Release();
}

bool parse(int argc, wchar_t** argv, Options& o)
{
    for (int i = 0; i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? utf8(argv[++i]) : std::string(); };
        if (arg == L"--adapter") {
            o.adapter = next();
        } else if (arg == L"--size") {
            if (sscanf_s(next().c_str(), "%dx%d", &o.width, &o.height) != 2) return false;
        } else if (arg == L"--fps") {
            o.fps = std::max(1, std::atoi(next().c_str()));
        } else if (arg == L"--frames") {
            o.frames = std::max(1, std::atoi(next().c_str()));
        } else if (arg == L"--kbps") {
            o.kbps = std::max(100, std::atoi(next().c_str()));
        } else if (arg == L"--only") {
            const std::string which = next();
            o.nvenc = which == "nvenc";
            o.amf = which == "amf";
        } else {
            return false;
        }
    }
    return true;
}

} // namespace

void vendorsUsage()
{
    std::puts("mw-d3d12-lab vendors [options]\n"
              "  Whether NVENC (D3D12 interface) and AMF (InitDX12) encode a D3D12 NV12\n"
              "  picture, and how fast: HEVC CBR, one-frame VBV, low-latency presets.\n"
              "  --adapter <gpu>      a DXGI index or a piece of the name (RTX, Arc...)\n"
              "  --size WxH --fps <n> --frames <n> --kbps <n>\n"
              "  --only nvenc|amf");
}

int runVendors(int argc, wchar_t** argv)
{
    Options o;
    if (!parse(argc, argv, o)) {
        vendorsUsage();
        return 2;
    }
    NativeHost::setLogSink([](int level, const std::string& message) {
        if (level >= 2) std::printf("    %s\n", message.c_str());
    });
    const std::vector<Adapter> all = adapters(false);
    const Adapter* a = pickAdapter(all, o.adapter);
    if (!a) {
        say("no such adapter\n");
        return 2;
    }
    std::string error;
    std::shared_ptr<d3d12::D3d12Device> device =
        d3d12::D3d12Device::forAdapter(d3d12::luidValue(a->desc.AdapterLuid), error);
    Direct direct;
    if (!device || !direct.init(*device, error)) {
        say("no D3D12: %s\n", error.c_str());
        return 1;
    }
    say("mw-d3d12-lab vendors - %s on %s\n%s, driver %s, %dx%d @ %d, %d kbps\n", nowText().c_str(),
        computerName().c_str(), a->name.c_str(), umdVersion(a->adapter.Get()).c_str(), o.width,
        o.height, o.fps, o.kbps);

    std::vector<ComPtr<ID3D12Resource>> inputs;
    for (int f = 0; f < 8; ++f) {
        ComPtr<ID3D12Resource> t =
            makeTexture(device->device(), static_cast<UINT>(o.width), static_cast<UINT>(o.height),
                        1, DXGI_FORMAT_NV12, D3D12_RESOURCE_FLAG_NONE);
        if (!t ||
            !upload(device->device(), direct, t.Get(), nv12Frame(o.width, o.height, f), error)) {
            say("input pictures: %s\n", error.c_str());
            return 1;
        }
        inputs.push_back(t);
    }

    const UINT vendor = a->desc.VendorId;
    if (o.nvenc && vendor == 0x10DE) {
        Result r;
        runNvenc(*device, inputs, o, r);
        print(r);
    }
    if (o.amf && vendor == 0x1002) {
        Result r;
        runAmf(*device, inputs, o, r);
        print(r);
    }
    if (vendor != 0x10DE && vendor != 0x1002)
        say("no vendor SDK with a D3D12 input on this GPU (Intel's oneVPL keeps D3D11)\n");
    return 0;
}

} // namespace lab
