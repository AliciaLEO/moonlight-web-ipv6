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

// `interop` — what the DDA handshake costs, and what reading without it does
// (plan pipeline-video-d3d12-v2, C1.2).
//
// The captured display shows scripts/bench/content/scroll.html?band=1: every
// frame carries its number, coded at the top-left and the bottom-left. For
// each acquired frame the capture context copies both bands at once — the
// truth, ordered after the compositor's write by Desktop Duplication's keyed
// mutex — and D3D12 reads the same bands through DdaInterop, in one of three
// ways (ddasync): none (no fence at all), gpu (the pipeline's: fence A before
// the read, fence B before ReleaseFrame, on the GPU), cpu (the CPU waits for
// fence B before ReleaseFrame). A busy shader can be put in front of the D3D12
// read (--delays): the read then happens long after the release, which is the
// race the handshake exists for. A D3D12 read that differs from the truth read
// a frame too old or too new, or one caught half-written.

#include "Interop.h"

#include "Json.h"
#include "Lab.h"

#include "capture/windows/DxgiDuplication.h"
#include "mw/native/NativeHost.h"
#include "platform/windows/StreamPriority.h"
#include "platform/windows/d3d12/DdaInterop.h"
#include "platform/windows/d3d12/D3d12Device.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace lab {
namespace {

using namespace mw::native;

constexpr UINT kBandW = 192;
constexpr UINT kBandH = 16;
constexpr UINT kBandPitch = kBandW * 4; // 768, a multiple of 256
constexpr UINT64 kBottomOffset = kBandPitch * kBandH;

struct Options
{
    std::string display;
    int seconds = 15;
    std::vector<std::string> modes = {"none", "gpu", "cpu"};
    std::vector<int> delaysMs = {0, 8};
    std::string gpuClass = "auto";
    std::wstring jsonPath;
    bool json = true;
};

void say(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::fflush(stdout);
}

std::vector<std::string> split(const std::string& text)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t comma = text.find(',', start);
        const std::string item =
            text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!item.empty()) out.push_back(item);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

double ms(int64_t us)
{
    return static_cast<double>(us) / 1000.0;
}

/// One band: 24 blocks of 8x8, white for a 1, the complement 8 rows below.
/// @p rows points at the band's first row, BGRA, @p pitch bytes apart.
/// -1 when a block and its complement do not disagree clearly: half-written.
long long decode(const uint8_t* rows, size_t pitch)
{
    long long value = 0;
    for (int bit = 0; bit < 24; ++bit) {
        const size_t x = static_cast<size_t>(bit * 8 + 4) * 4;
        const int a = rows[4 * pitch + x + 1];  // green of the block's centre
        const int b = rows[12 * pitch + x + 1]; // and of its complement's
        if (std::abs(a - b) < 100) return -1;
        if (a > b) value |= 1ll << bit;
    }
    return value;
}

struct Frame
{
    long long truthTop = -1, truthBottom = -1, readTop = -1, readBottom = -1;
};

struct Run
{
    std::string mode;
    int delayMs = 0;
    std::string refused;
    int frames = 0, timeouts = 0;
    int truthInvalid = 0, truthTorn = 0;
    int readInvalid = 0, readDiffers = 0, readNewer = 0, readOlder = 0, readTorn = 0;
    /// A D3D12 read still queued 10 s after the release (not compared), and a
    /// release that went ahead before the read (cpu: fence B not in 1 s).
    int readLate = 0, releasedEarly = 0;
    std::vector<double> holdMs, releaseMs;
    double seconds = 0;
};

struct Stats
{
    double mean = 0, p99 = 0;
};

Stats stats(std::vector<double> v)
{
    Stats s;
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v)
        sum += x;
    s.mean = sum / v.size();
    s.p99 = v[std::min(v.size() - 1, static_cast<size_t>(v.size() * 0.99))];
    return s;
}

/// A shader that keeps its queue busy for a while and nothing else busy: one
/// group of 64 threads, a dependent loop, its length a root constant.
constexpr char kBusy[] = R"HLSL(
cbuffer Busy : register(b0) { uint Iterations; };
RWByteAddressBuffer Sink : register(u0);
[numthreads(64, 1, 1)]
void CsMain(uint3 id : SV_DispatchThreadID)
{
    float a = float(id.x);
    for (uint i = 0; i < Iterations; ++i) a = sin(a) * 1.0001 + 0.5;
    Sink.Store(id.x * 4, asuint(a));
}
)HLSL";

struct Busy
{
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12Resource> sink;
    UINT iterationsPerMs = 0;

    bool init(ID3D12Device* device, std::string& error)
    {
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = 1;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        D3D12_ROOT_SIGNATURE_DESC desc = {};
        desc.NumParameters = 2;
        desc.pParameters = params;
        ComPtr<ID3DBlob> blob, errors, cs;
        HRESULT h =
            ::D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
        if (SUCCEEDED(h))
            h = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                            IID_PPV_ARGS(&root));
        if (SUCCEEDED(h))
            h = ::D3DCompile(kBusy, sizeof(kBusy) - 1, "busy.hlsl", nullptr, nullptr, "CsMain",
                             "cs_5_0", 0, 0, &cs, &errors);
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = root.Get();
        if (SUCCEEDED(h)) {
            pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
            h = device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
        }
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC buffer = {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = 256;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (SUCCEEDED(h))
            h = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                IID_PPV_ARGS(&sink));
        if (FAILED(h)) {
            error = "busy shader " + hr(h);
            return false;
        }
        return true;
    }

    void record(ID3D12GraphicsCommandList* list, UINT iterations)
    {
        list->SetComputeRootSignature(root.Get());
        list->SetPipelineState(pso.Get());
        list->SetComputeRoot32BitConstant(0, iterations, 0);
        list->SetComputeRootUnorderedAccessView(1, sink->GetGPUVirtualAddress());
        list->Dispatch(1, 1, 1);
    }
};

bool parse(int argc, wchar_t** argv, Options& o)
{
    for (int i = 0; i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? utf8(argv[++i]) : std::string(); };
        if (arg == L"--display") {
            o.display = next();
        } else if (arg == L"--seconds") {
            o.seconds = std::max(1, std::atoi(next().c_str()));
        } else if (arg == L"--modes") {
            o.modes = split(next());
        } else if (arg == L"--delays") {
            o.delaysMs.clear();
            for (const std::string& d : split(next()))
                o.delaysMs.push_back(std::atoi(d.c_str()));
        } else if (arg == L"--class") {
            o.gpuClass = next();
        } else if (arg == L"--json") {
            o.jsonPath = i + 1 < argc ? argv[++i] : L"";
        } else if (arg == L"--no-json") {
            o.json = false;
        } else {
            return false;
        }
    }
    return isGpuClassOption(o.gpuClass);
}

d3d12::DdaSync syncOf(const std::string& mode)
{
    if (mode == "gpu") return d3d12::DdaSync::Gpu;
    if (mode == "cpu") return d3d12::DdaSync::Cpu;
    return d3d12::DdaSync::None;
}

} // namespace

void interopUsage()
{
    std::puts("mw-d3d12-lab interop [options]\n"
              "  What the DDA handshake costs and what reading without it does. The captured\n"
              "  display must show scripts/bench/content/scroll.html?band=1 (kiosk.ps1), SDR.\n"
              "  For each acquired frame D3D11 copies the frame-number bands (the truth) and\n"
              "  D3D12 reads them through DdaInterop; the two are compared.\n"
              "  --display <which>     'Display 3', a monitor name, or a piece of the GPU's name\n"
              "                        (Arc); default the primary\n"
              "  --seconds <s>         per run (default 15)\n"
              "  --modes none,gpu,cpu  ddasync variants (default all three)\n"
              "  --delays 0,8          ms of busy shader in front of the D3D12 read (default 0,8)\n"
              "  --class auto|high|normal  the process's GPU scheduling class (auto = REALTIME\n"
              "                        where the token allows it, as the product asks); the\n"
              "                        queue and the capture device follow it\n"
              "  --json <file> | --no-json");
}

int runInterop(int argc, wchar_t** argv)
{
    Options o;
    if (!parse(argc, argv, o)) {
        interopUsage();
        return 2;
    }
    NativeHost::setLogSink([](int level, const std::string& message) {
        if (level >= 2) std::printf("    %s\n", message.c_str());
    });

    // The process's GPU class, the product's way; the queue and the capture
    // device below follow it.
    StreamPriority priority;
    const GpuScheduling scheduling = takeGpuClass(priority, o.gpuClass);

    const Capabilities caps = NativeHost::probe();
    if (caps.displays.empty()) {
        say("no display\n");
        return 2;
    }
    const DisplayInfo* target = nullptr;
    // A label ("Display 3"), a monitor name, or a piece of the GPU's name
    // ("Arc"): the first display that answers.
    std::string wanted = o.display;
    for (char& ch : wanted)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    for (const DisplayInfo& d : caps.displays) {
        if (target) break;
        const GpuInfo* g = caps.gpuFor(d);
        std::string gpuName = g ? g->name : std::string();
        for (char& ch : gpuName)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (o.display.empty() ? d.primary
                              : (d.label.find(o.display) != std::string::npos ||
                                 d.detail.find(o.display) != std::string::npos ||
                                 gpuName.find(wanted) != std::string::npos))
            target = &d;
    }
    if (!target) target = &caps.displays.front();
    const GpuInfo* gpu = caps.gpuFor(*target);
    if (!gpu) {
        say("display %s names no GPU\n", target->label.c_str());
        return 2;
    }
    unsigned outputIndex = 0;
    for (const DisplayInfo& d : caps.displays) {
        if (d.id == target->id) break;
        if (d.gpuId == target->gpuId) ++outputIndex;
    }

    say("mw-d3d12-lab interop - %s on %s\n", nowText().c_str(), computerName().c_str());
    say("%s on %s, %d s per run\n", target->label.c_str(), gpu->name.c_str(), o.seconds);
    say("token %s, base-priority privilege %s, GPU class %s\n", scheduling.token.c_str(),
        scheduling.privilege ? "enabled" : "not held", scheduling.gpuClass.c_str());

    std::string error;
    std::shared_ptr<d3d12::D3d12Device> device =
        d3d12::D3d12Device::forAdapter(gpu->nativeHandle, error);
    if (!device) {
        say("no D3D12: %s\n", error.c_str());
        return 1;
    }
    d3d12::Queue queue;
    d3d12::QueueRequest request;
    request.priority = d3d12::QueuePriority::Auto;
    if (!device->createQueue(request, queue, error)) {
        say("%s\n", error.c_str());
        return 1;
    }
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Resource> readback;
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer = {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 2 * kBottomOffset;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT h = device->device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                         IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(h))
        h = device->device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                                nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(h)) h = list->Close();
    if (SUCCEEDED(h))
        h = device->device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                      IID_PPV_ARGS(&readback));
    Busy busy;
    if (FAILED(h) || !busy.init(device->device(), error)) {
        say("setup failed: %s %s\n", hr(h).c_str(), error.c_str());
        return 1;
    }

    // The busy shader's rate on this GPU: 2^16 iterations timed, the rest
    // scaled from it.
    {
        d3d12::QueueTimer timer;
        d3d12::GpuFence fence;
        if (timer.init(device->device(), queue.queue.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT,
                       error) &&
            fence.create(device->device(), false, error)) {
            double best = 0;
            for (int round = 0; round < 3; ++round) {
                allocator->Reset();
                list->Reset(allocator.Get(), nullptr);
                const int slot = timer.begin(list.Get());
                busy.record(list.Get(), 1u << 16);
                timer.end(list.Get(), slot);
                list->Close();
                ID3D12CommandList* lists[] = {list.Get()};
                queue.queue->ExecuteCommandLists(1, lists);
                const uint64_t v = fence.signal(queue.queue.Get(), error);
                fence.wait(v, 5000, error);
                d3d12::QueueTimer::Sample s;
                if (timer.read(slot, s) && s.gpuUs > 0)
                    best = std::max(best, 65536.0 * 1000.0 / static_cast<double>(s.gpuUs));
            }
            busy.iterationsPerMs = static_cast<UINT>(best);
        }
        say("busy shader: %u iterations per ms\n\n", busy.iterationsPerMs);
    }

    std::vector<Run> runs;
    for (const std::string& mode : o.modes) {
        for (int delay : o.delaysMs) {
            Run run;
            run.mode = mode;
            run.delayMs = delay;
            say("  ddasync=%s, %d ms in front of the read...\n", mode.c_str(), delay);

            capture::DxgiDuplication duplication(gpu->nativeHandle, outputIndex);
            if (!duplication.start(error)) {
                run.refused = "duplication: " + error;
                runs.push_back(run);
                continue;
            }
            StreamPriority::raiseDevice(duplication.device(), "capture");
            if (duplication.format() != DXGI_FORMAT_B8G8R8A8_UNORM) {
                run.refused = "the display is not SDR 8-bit";
                runs.push_back(run);
                continue;
            }
            if (runs.empty()) {
                const capture::DesktopRect r = duplication.desktopRect();
                say("    (the band page goes on %d,%d,%d,%d)\n", r.left, r.top, r.width(),
                    r.height());
            }
            d3d12::DdaInterop interop;
            std::string refusal;
            if (!interop.bind(duplication.device(), device, refusal)) {
                run.refused = refusal;
                runs.push_back(run);
                continue;
            }
            // The truth: both bands, copied on the capture context.
            D3D11_TEXTURE2D_DESC sd = {};
            sd.Width = kBandW;
            sd.Height = 2 * kBandH;
            sd.MipLevels = 1;
            sd.ArraySize = 1;
            sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            sd.SampleDesc.Count = 1;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;
            if (FAILED(duplication.device()->CreateTexture2D(&sd, nullptr, &staging))) {
                run.refused = "staging texture";
                runs.push_back(run);
                continue;
            }
            const UINT height = static_cast<UINT>(duplication.height());
            const D3D11_BOX top11 = {0, 0, 0, kBandW, kBandH, 1};
            const D3D11_BOX bottom11 = {0, height - kBandH, 0, kBandW, height, 1};
            const d3d12::DdaSync sync = syncOf(mode);

            const int64_t started = nowUs();
            while (nowUs() - started < static_cast<int64_t>(o.seconds) * 1000000) {
                capture::CapturedFrame frame;
                if (duplication.acquire(100, frame) != capture::AcquireStatus::Ok) {
                    ++run.timeouts;
                    continue;
                }
                const int64_t acquired = nowUs();
                ++run.frames;
                ID3D11DeviceContext* context = duplication.context();
                context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, frame.texture, 0, &top11);
                context->CopySubresourceRegion(staging.Get(), 0, 0, kBandH, 0, frame.texture, 0,
                                               &bottom11);
                ID3D12Resource* surface = interop.open(frame.texture, error);
                const uint64_t a = interop.signalAcquired(context, sync, error);
                if (sync == d3d12::DdaSync::None) context->Flush();

                allocator->Reset();
                list->Reset(allocator.Get(), nullptr);
                if (delay > 0 && busy.iterationsPerMs)
                    busy.record(list.Get(), busy.iterationsPerMs * static_cast<UINT>(delay));
                D3D12_RESOURCE_BARRIER barrier = {};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = surface;
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                if (surface) {
                    list->ResourceBarrier(1, &barrier);
                    for (int band = 0; band < 2; ++band) {
                        D3D12_TEXTURE_COPY_LOCATION to = {}, from = {};
                        to.pResource = readback.Get();
                        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                        to.PlacedFootprint.Offset = band ? kBottomOffset : 0;
                        to.PlacedFootprint.Footprint = {DXGI_FORMAT_B8G8R8A8_UNORM, kBandW, kBandH,
                                                        1, kBandPitch};
                        from.pResource = surface;
                        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                        const D3D12_BOX box = {0,      band ? height - kBandH : 0, 0,
                                               kBandW, band ? height : kBandH,     1};
                        list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
                    }
                    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
                    list->ResourceBarrier(1, &barrier);
                }
                list->Close();
                interop.conversionWaits(queue.queue.Get(), a, error);
                ID3D12CommandList* lists[] = {list.Get()};
                queue.queue->ExecuteCommandLists(1, lists);
                const uint64_t b = interop.conversionSignals(queue.queue.Get(), error);

                const int64_t releasing = nowUs();
                if (!interop.beforeRelease(context, b, sync, 1000, error)) ++run.releasedEarly;
                duplication.release();
                const int64_t released = nowUs();
                run.holdMs.push_back(ms(released - acquired));
                run.releaseMs.push_back(ms(released - releasing));

                // Both reads, now that both are done. A read not done in 10 s
                // is not compared (its buffer still holds the previous one),
                // and ends the run: its list may still be queued, so the
                // allocator cannot be reset under it.
                if (interop.converted().wait(b, 10000, error) != d3d12::GpuFence::Wait::Done) {
                    ++run.readLate;
                    break;
                }
                Frame f;
                uint8_t* mapped = nullptr;
                const D3D12_RANGE range = {0, static_cast<SIZE_T>(2 * kBottomOffset)};
                if (surface &&
                    SUCCEEDED(readback->Map(0, &range, reinterpret_cast<void**>(&mapped)))) {
                    f.readTop = decode(mapped, kBandPitch);
                    f.readBottom = decode(mapped + kBottomOffset, kBandPitch);
                    const D3D12_RANGE none = {0, 0};
                    readback->Unmap(0, &none);
                }
                D3D11_MAPPED_SUBRESOURCE m = {};
                if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                    const auto* rows = static_cast<const uint8_t*>(m.pData);
                    f.truthTop = decode(rows, m.RowPitch);
                    f.truthBottom =
                        decode(rows + static_cast<size_t>(kBandH) * m.RowPitch, m.RowPitch);
                    context->Unmap(staging.Get(), 0);
                }

                if (f.truthTop < 0 || f.truthBottom < 0) {
                    ++run.truthInvalid;
                    continue;
                }
                if (f.truthTop != f.truthBottom) ++run.truthTorn;
                if (f.readTop < 0 || f.readBottom < 0) {
                    ++run.readInvalid;
                    continue;
                }
                if (f.readTop != f.readBottom) ++run.readTorn;
                if (f.readTop != f.truthTop || f.readBottom != f.truthBottom) {
                    ++run.readDiffers;
                    if (f.readTop > f.truthTop) ++run.readNewer;
                    if (f.readTop < f.truthTop) ++run.readOlder;
                }
            }
            run.seconds = ms(nowUs() - started) / 1000.0;
            interop.unbind();
            duplication.stop();
            runs.push_back(run);
        }
    }

    say("\n%-24s %6s %6s | %-13s | %-9s %-9s %-9s %-7s %-10s | %-13s | %-13s\n", "run", "frames",
        "i/s", "truth bad/torn", "D3D12 bad", "differs", "new/old", "torn", "late/early",
        "hold mean/p99", "release mean/p99");
    for (const Run& r : runs) {
        char name[48];
        std::snprintf(name, sizeof(name), "ddasync=%s +%d ms", r.mode.c_str(), r.delayMs);
        if (!r.refused.empty()) {
            say("%-24s refused: %s\n", name, r.refused.c_str());
            continue;
        }
        const Stats hold = stats(r.holdMs), rel = stats(r.releaseMs);
        char truth[24], newOld[24], lateEarly[24];
        std::snprintf(truth, sizeof(truth), "%d/%d", r.truthInvalid, r.truthTorn);
        std::snprintf(newOld, sizeof(newOld), "%d/%d", r.readNewer, r.readOlder);
        std::snprintf(lateEarly, sizeof(lateEarly), "%d/%d", r.readLate, r.releasedEarly);
        say("%-24s %6d %6.1f | %-13s | %-9d %-9d %-9s %-7d %-10s | %5.2f %6.2f  | %5.2f %6.2f\n",
            name, r.frames, r.frames / std::max(0.001, r.seconds), truth, r.readInvalid,
            r.readDiffers, newOld, r.readTorn, lateEarly, hold.mean, hold.p99, rel.mean, rel.p99);
    }
    for (const Run& r : runs) {
        if (r.refused.empty() && r.truthInvalid > r.frames / 2) {
            say("(most frames carry no band: is scroll.html?band=1 on this display?)\n");
            break;
        }
    }

    if (o.json) {
        Json j;
        j.beginObject();
        j.field("tool", "mw-d3d12-lab").field("command", "interop").field("schema", 1);
        j.field("date", nowText()).field("computer", computerName()).field("os", osBuild());
        j.field("display", target->label).field("gpu", gpu->name);
        j.field("token", scheduling.token).field("basePriorityPrivilege", scheduling.privilege);
        j.field("gpuClass", scheduling.gpuClass);
        j.field("busyIterationsPerMs", busy.iterationsPerMs);
        j.key("runs").beginArray();
        for (const Run& r : runs) {
            j.beginObject();
            j.field("ddasync", r.mode).field("delayMs", r.delayMs);
            if (!r.refused.empty()) {
                j.field("refused", r.refused);
            } else {
                const Stats hold = stats(r.holdMs), rel = stats(r.releaseMs);
                j.field("frames", r.frames).field("seconds", r.seconds);
                j.field("truthInvalid", r.truthInvalid).field("truthTorn", r.truthTorn);
                j.field("readInvalid", r.readInvalid).field("readDiffers", r.readDiffers);
                j.field("readNewer", r.readNewer).field("readOlder", r.readOlder);
                j.field("readTorn", r.readTorn);
                j.field("readLate", r.readLate).field("releasedEarly", r.releasedEarly);
                j.field("holdMeanMs", hold.mean).field("holdP99Ms", hold.p99);
                j.field("releaseMeanMs", rel.mean).field("releaseP99Ms", rel.p99);
            }
            j.endObject();
        }
        j.endArray();
        j.endObject();
        std::wstring path = o.jsonPath;
        if (path.empty()) path = wide("interop-" + computerName() + "-" + nowStamp() + ".json");
        std::ofstream file(path, std::ios::binary);
        file << j.str() << '\n';
        say("JSON: %s\n", utf8(path.c_str()).c_str());
    }
    return 0;
}

} // namespace lab
