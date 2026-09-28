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

// `queues` — where the conversion waits (plan pipeline-video-d3d12-v2, C1.1).
//
// The product's own conversion — 1440p to 1080p through the Lanczos-2
// resample, the pointer drawn in — submitted at a steady rate, on:
//  - D3D11, as the product runs it today (ColorConvert, GPU thread priority 7);
//  - D3D12 pixel shaders on a DIRECT queue (ColorConvert12, the route built);
//  - D3D12 compute on a COMPUTE queue (ComputeConvert, the route G1 decides on);
//  - D3D12 Video Process on a VIDEO_PROCESS queue (plan §9-15): the driver's
//    own scaler and colour conversion — on Intel the fixed-function scaler of
//    the video engine (SFC), if the driver routes it there — alone, or with
//    the pointer composed as a second stream;
// the D3D12 ones at every queue priority, with the default CreatorID and with
// one of their own. Each submission is timed three ways: the wall clock from
// submit to completion, the GPU's own timestamps around the work, and the wait
// in the queue before the GPU started on it (timestamps put on the CPU clock).
// Run it under a game — or mw-gpu-load, whose --json it can read to report
// the load's own frame rate over each variant's window. The last picture of
// each variant is held to the pixel shaders' (PSNR per plane, plane means):
// with --picture, a real desktop rather than the synthetic one says what the
// video engine's scaler costs in quality against the product's Lanczos-2.

#include "Queues.h"

#include "ComputeConvert.h"
#include "Json.h"
#include "Lab.h"
#include "LabD3d12.h"

#include "capture/CaptureTypes.h"
#include "convert/windows/ColorConvert.h"
#include "convert/windows/d3d12/ColorConvert12.h"
#include "mw/native/NativeHost.h"
#include "platform/windows/StreamPriority.h"
#include "platform/windows/d3d12/D3d12Device.h"

#include <d3d11_4.h>
#include <d3d12.h>
#include <d3d12video.h>
#include <objbase.h>
#include <wincodec.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace lab {
namespace {

using namespace mw::native;

struct Options
{
    std::string adapter;
    int seconds = 30;
    int rate = 120;
    int warmup = 60;
    int sourceW = 2560, sourceH = 1440, outputW = 1920, outputH = 1080;
    convert::ScaleFilter filter = convert::ScaleFilter::Lanczos2;
    bool cursor = true;
    std::vector<std::string> variants = {"d3d11", "ps", "cs"};
    std::vector<std::string> priorities = {"normal", "high", "realtime"};
    std::vector<std::string> creators = {"default", "own"};
    std::string gpuClass = "auto";
    std::wstring loadJson;
    std::wstring jsonPath;
    bool json = true;
    /// A real picture (PNG, JPEG, BMP) as the desktop; its size is the source.
    std::wstring picture;
    /// Where the last picture of each variant goes, raw NV12 ("<dump>-<variant>.nv12").
    std::wstring dump;
    /// The Video Process variants without timestamps, for a driver that faults on them.
    bool vpUntimed = false;
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

bool has(const std::vector<std::string>& list, const char* item)
{
    return std::find(list.begin(), list.end(), item) != list.end();
}

double epochSeconds()
{
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int64_t qpc()
{
    LARGE_INTEGER c = {};
    ::QueryPerformanceCounter(&c);
    return c.QuadPart;
}

double qpcMs(int64_t ticks)
{
    static const double frequency = [] {
        LARGE_INTEGER f = {};
        ::QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    return static_cast<double>(ticks) * 1000.0 / frequency;
}

struct Stats
{
    size_t n = 0;
    double mean = 0, p50 = 0, p99 = 0, max = 0;
};

Stats stats(std::vector<double> v)
{
    Stats s;
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v)
        sum += x;
    s.n = v.size();
    s.mean = sum / v.size();
    s.p50 = v[v.size() / 2];
    s.p99 = v[std::min(v.size() - 1, static_cast<size_t>(v.size() * 0.99))];
    s.max = v.back();
    return s;
}

struct Result
{
    std::string name;
    std::string api;
    std::string queue;
    std::string refused;
    std::vector<double> wall, gpu, wait, record;
    int late = 0;
    int timeouts = 0;
    double start = 0, end = 0;
    double loadFps = -1;
    int loadSeconds = 0;
    /// The last frame against ColorConvert12's: the largest difference of a
    /// code, and how many codes differ by more than 2. -1 when not compared.
    int maxDiff = -1;
    size_t bigDiffs = 0;
    /// The same comparison as a PSNR per plane (99 when identical), and the
    /// last frame's plane means: a scaler moves the PSNR, a range or matrix
    /// mistake the means. -1 when not compared.
    double psnrY = -1, psnrUV = -1;
    double meanY = -1, meanU = -1, meanV = -1;
    /// Submissions left undone when the variant ran out of time (late ones
    /// stretch it: under load, past the load's own end).
    int cut = 0;
    /// The last frame, kept for --dump.
    std::vector<uint8_t> last;
};

/// The means of an NV12 picture read back by readNv12: luma first
/// (@p lumaBytes), then the interleaved chroma.
void means(const std::vector<uint8_t>& nv12, size_t lumaBytes, double& y, double& u, double& v)
{
    y = u = v = -1;
    if (lumaBytes == 0 || nv12.size() <= lumaBytes) return;
    double sumY = 0, sumU = 0, sumV = 0;
    for (size_t i = 0; i < lumaBytes; ++i)
        sumY += nv12[i];
    for (size_t i = lumaBytes; i + 1 < nv12.size(); i += 2) {
        sumU += nv12[i];
        sumV += nv12[i + 1];
    }
    const double pairs = static_cast<double>((nv12.size() - lumaBytes) / 2);
    y = sumY / static_cast<double>(lumaBytes);
    u = sumU / pairs;
    v = sumV / pairs;
}

double psnr(double squares, size_t n)
{
    if (n == 0) return -1;
    const double mse = squares / static_cast<double>(n);
    return mse <= 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

void compare(const std::vector<uint8_t>& reference, const std::vector<uint8_t>& got,
             size_t lumaBytes, Result& r)
{
    if (reference.empty() || got.size() != reference.size() || lumaBytes >= got.size()) return;
    r.maxDiff = 0;
    double squaresY = 0, squaresUV = 0;
    for (size_t i = 0; i < got.size(); ++i) {
        const int d = std::abs(static_cast<int>(got[i]) - static_cast<int>(reference[i]));
        r.maxDiff = std::max(r.maxDiff, d);
        if (d > 2) ++r.bigDiffs;
        (i < lumaBytes ? squaresY : squaresUV) += static_cast<double>(d) * d;
    }
    r.psnrY = psnr(squaresY, lumaBytes);
    r.psnrUV = psnr(squaresUV, got.size() - lumaBytes);
    means(got, lumaBytes, r.meanY, r.meanU, r.meanV);
}

/// A steady rate: each tick waits for its slot on a high-resolution timer; a
/// submission that overran its slot is counted late and the rate re-anchored
/// rather than caught up with a burst.
class Pacer
{
public:
    explicit Pacer(int rate)
        : m_PeriodUs(1000000 / std::max(1, rate))
    {
        m_Timer = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                           TIMER_ALL_ACCESS);
        m_Next = nowUs() + m_PeriodUs;
    }
    ~Pacer()
    {
        if (m_Timer) ::CloseHandle(m_Timer);
    }
    /// False when the slot had already passed.
    bool wait()
    {
        const int64_t now = nowUs();
        if (now >= m_Next) {
            m_Next = now + m_PeriodUs;
            return false;
        }
        const int64_t remaining = m_Next - now;
        if (m_Timer) {
            LARGE_INTEGER due = {};
            due.QuadPart = -remaining * 10;
            ::SetWaitableTimer(m_Timer, &due, 0, nullptr, nullptr, FALSE);
            ::WaitForSingleObject(m_Timer, INFINITE);
        } else {
            ::Sleep(static_cast<DWORD>(remaining / 1000));
        }
        m_Next += m_PeriodUs;
        return true;
    }

private:
    int64_t m_PeriodUs;
    int64_t m_Next = 0;
    HANDLE m_Timer = nullptr;
};

/// How long a variant may run once its warm-up is done: --seconds, and one
/// period for the timer's slack. A variant whose submissions come back late
/// stops there with fewer of them, instead of running on past a load that
/// stops at 60 s.
int64_t deadlineUs(const Options& o)
{
    return static_cast<int64_t>(o.seconds) * 1000000 + 1000000 / std::max(1, o.rate);
}

/// A picture with detail everywhere, so the resample and the conversion do
/// their real work.
std::vector<uint8_t> desktop(int w, int h)
{
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &out[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = static_cast<uint8_t>((x * 7 + y * 3) & 0xff);
            p[1] = static_cast<uint8_t>(((x ^ y) * 5) & 0xff);
            p[2] = static_cast<uint8_t>((x + y * 11) & 0xff);
            p[3] = 255;
        }
    }
    return out;
}

/// A picture from disk as BGRA rows, through WIC (PNG, JPEG, BMP...), made
/// opaque: a desktop has no transparency. COM must be up on this thread.
bool loadPicture(const std::wstring& path, int& width, int& height, std::vector<uint8_t>& pixels,
                 std::string& error)
{
    ComPtr<IWICImagingFactory> factory;
    HRESULT h = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&factory));
    ComPtr<IWICBitmapDecoder> decoder;
    if (SUCCEEDED(h))
        h = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                               WICDecodeMetadataCacheOnDemand, &decoder);
    ComPtr<IWICBitmapFrameDecode> frame;
    if (SUCCEEDED(h)) h = decoder->GetFrame(0, &frame);
    ComPtr<IWICFormatConverter> converter;
    if (SUCCEEDED(h)) h = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(h))
        h = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                                  WICBitmapDitherTypeNone, nullptr, 0.0,
                                  WICBitmapPaletteTypeCustom);
    UINT w = 0, ht = 0;
    if (SUCCEEDED(h)) h = converter->GetSize(&w, &ht);
    if (SUCCEEDED(h) && (w < 64 || ht < 64 || w > 16384 || ht > 16384)) h = E_INVALIDARG;
    if (SUCCEEDED(h)) {
        pixels.assign(static_cast<size_t>(w) * ht * 4, 0);
        h = converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size()), pixels.data());
    }
    if (FAILED(h)) {
        error = "could not read " + utf8(path.c_str()) + " (" + hr(h) + ")";
        return false;
    }
    for (size_t i = 3; i < pixels.size(); i += 4)
        pixels[i] = 255;
    width = static_cast<int>(w);
    height = static_cast<int>(ht);
    return true;
}

capture::CursorState pointer()
{
    capture::CursorState c;
    c.visible = true;
    c.x = 1000;
    c.y = 600;
    c.width = 32;
    c.height = 32;
    c.shapeVersion = 1;
    c.pixels.assign(32 * 32 * 4, 0);
    c.invert.assign(32 * 32, 0);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            if (x <= y && x < 20 && y < 28) {
                uint8_t* p = &c.pixels[(static_cast<size_t>(y) * 32 + x) * 4];
                p[0] = p[1] = p[2] = (x == 0 || x == y || y == 27) ? 0 : 255;
                p[3] = 255;
            }
            if (x >= 24 && y >= 4 && y < 28) c.invert[static_cast<size_t>(y) * 32 + x] = 255;
        }
    }
    return c;
}

// ── D3D11, as the product runs it ───────────────────────────────────────────

void runD3d11(const Adapter& a, const Options& o, const std::vector<uint8_t>& pixels,
              const capture::CursorState& cursor, Result& r)
{
    r.api = "d3d11";
    r.name = "d3d11 (immediate context, GPU thread priority 7)";
    r.queue = "D3D11 immediate context";
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT h = ::D3D11CreateDevice(a.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                    D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
                                    &device, nullptr, &context);
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext4> context4;
    if (SUCCEEDED(h)) h = device.As(&device5);
    if (SUCCEEDED(h)) h = context.As(&context4);
    if (FAILED(h)) {
        r.refused = "no D3D11.4 device " + hr(h);
        return;
    }
    StreamPriority::raiseDevice(device.Get(), "probe");

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(o.sourceW);
    desc.Height = static_cast<UINT>(o.sourceH);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA seed = {pixels.data(), static_cast<UINT>(o.sourceW * 4), 0};
    ComPtr<ID3D11Texture2D> source;
    if (FAILED(h = device->CreateTexture2D(&desc, &seed, &source))) {
        r.refused = "source texture " + hr(h);
        return;
    }
    convert::ColorConvert converter;
    std::string error;
    if (!converter.init(device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, o.sourceW, o.sourceH, o.outputW,
                        o.outputH, convert::ColorConvert::Chroma::C420, false, o.filter, error)) {
        r.refused = error;
        return;
    }
    ComPtr<ID3D11Fence> fence;
    HANDLE event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(h = device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) ||
        !event) {
        r.refused = "fence " + hr(h);
        if (event) ::CloseHandle(event);
        return;
    }
    D3D11_QUERY_DESC q = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    ComPtr<ID3D11Query> disjoint, begin, end;
    device->CreateQuery(&q, &disjoint);
    q.Query = D3D11_QUERY_TIMESTAMP;
    device->CreateQuery(&q, &begin);
    device->CreateQuery(&q, &end);

    const capture::CursorState none;
    const convert::CursorDraw draw;
    Pacer pacer(o.rate);
    UINT64 value = 0;
    const int total = o.warmup + o.seconds * o.rate;
    int64_t deadline = 0;
    for (int i = 0; i < total; ++i) {
        const bool onTime = pacer.wait();
        const bool counted = i >= o.warmup;
        if (counted && r.start == 0) {
            r.start = epochSeconds();
            deadline = nowUs() + deadlineUs(o);
        }
        if (counted && nowUs() >= deadline) {
            r.cut = total - i;
            break;
        }
        if (counted && !onTime) ++r.late;
        const int64_t t0 = qpc();
        context->Begin(disjoint.Get());
        context->End(begin.Get());
        converter.convert(source.Get(), o.cursor ? cursor : none, draw, error);
        context->End(end.Get());
        context->End(disjoint.Get());
        context4->Signal(fence.Get(), ++value);
        context->Flush();
        const int64_t t1 = qpc();
        fence->SetEventOnCompletion(value, event);
        const bool done = ::WaitForSingleObject(event, 1000) == WAIT_OBJECT_0;
        const int64_t t2 = qpc();
        if (!counted) continue;
        if (!done) {
            ++r.timeouts;
            continue;
        }
        r.wall.push_back(qpcMs(t2 - t0));
        r.record.push_back(qpcMs(t1 - t0));
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
        UINT64 b = 0, e = 0;
        if (context->GetData(disjoint.Get(), &dj, sizeof(dj), 0) == S_OK && !dj.Disjoint &&
            context->GetData(begin.Get(), &b, sizeof(b), 0) == S_OK &&
            context->GetData(end.Get(), &e, sizeof(e), 0) == S_OK && e >= b && dj.Frequency)
            r.gpu.push_back(static_cast<double>(e - b) * 1000.0 /
                            static_cast<double>(dj.Frequency));
    }
    r.end = epochSeconds();
    ::CloseHandle(event);
}

// ── D3D12, pixel shaders on DIRECT or compute on COMPUTE ────────────────────

int priorityValue(const std::string& name)
{
    if (name == "high") return D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    if (name == "realtime") return D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME;
    return D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
}

const char* priorityLabel(const std::string& name)
{
    if (name == "high") return "HIGH";
    if (name == "realtime") return "GLOBAL_REALTIME";
    return "NORMAL";
}

bool makeQueue(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type, int priority, bool own,
               ComPtr<ID3D12CommandQueue>& queue, std::string& why)
{
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = type;
    desc.Priority = priority;
    HRESULT h = E_FAIL;
    if (own) {
        ComPtr<ID3D12Device9> device9;
        GUID creator = {};
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device9))) ||
            FAILED(::CoCreateGuid(&creator))) {
            why = "no CreateCommandQueue1";
            return false;
        }
        h = device9->CreateCommandQueue1(&desc, creator, IID_PPV_ARGS(&queue));
    } else {
        h = device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue));
    }
    if (FAILED(h)) {
        why = hr(h);
        return false;
    }
    return true;
}

/// An NV12 texture (in COMMON) read back, rows packed: luma, then chroma.
std::vector<uint8_t> readNv12(ID3D12Device* device, ID3D12Resource* nv12, std::string& error)
{
    std::vector<uint8_t> out;
    ComPtr<ID3D12CommandQueue> queue;
    if (!makeQueue(device, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
                   false, queue, error))
        return out;
    const D3D12_RESOURCE_DESC desc = nv12->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT feet[2] = {};
    UINT rows[2] = {};
    UINT64 rowBytes[2] = {};
    UINT64 total = 0;
    device->GetCopyableFootprints(&desc, 0, 2, 0, feet, rows, rowBytes, &total);
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer = {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = total;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&readback))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                         nullptr, IID_PPV_ARGS(&list)))) {
        error = "readback resources";
        return out;
    }
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = nv12;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &barrier);
    for (UINT plane = 0; plane < 2; ++plane) {
        D3D12_TEXTURE_COPY_LOCATION to = {}, from = {};
        to.pResource = readback.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint = feet[plane];
        from.pResource = nv12;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.SubresourceIndex = plane;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(1, &barrier);
    list->Close();
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    d3d12::GpuFence fence;
    if (!fence.create(device, false, error)) return out;
    const uint64_t value = fence.signal(queue.Get(), error);
    if (!value || fence.wait(value, 5000, error) != d3d12::GpuFence::Wait::Done) return out;
    uint8_t* mapped = nullptr;
    const D3D12_RANGE range = {0, static_cast<SIZE_T>(total)};
    if (FAILED(readback->Map(0, &range, reinterpret_cast<void**>(&mapped)))) return out;
    for (UINT plane = 0; plane < 2; ++plane)
        for (UINT y = 0; y < rows[plane]; ++y) {
            const uint8_t* p = mapped + feet[plane].Offset +
                               static_cast<size_t>(y) * feet[plane].Footprint.RowPitch;
            out.insert(out.end(), p, p + rowBytes[plane]);
        }
    const D3D12_RANGE none = {0, 0};
    readback->Unmap(0, &none);
    return out;
}

/// BGRA rows as a D3D12 texture in COMMON, as the duplication's surface
/// arrives: the desktop, or the pointer the Video Process composes.
ComPtr<ID3D12Resource> uploadBgra(ID3D12Device* device, int width, int height,
                                  const std::vector<uint8_t>& pixels, std::string& error)
{
    ComPtr<ID3D12CommandQueue> queue;
    if (!makeQueue(device, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
                   false, queue, error))
        return nullptr;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = static_cast<UINT64>(width);
    desc.Height = static_cast<UINT>(height);
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> texture;
    HRESULT h = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&texture));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot = {};
    UINT64 total = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &foot, nullptr, nullptr, &total);
    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = total;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    ComPtr<ID3D12Resource> upload;
    if (SUCCEEDED(h))
        h = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                            IID_PPV_ARGS(&upload));
    uint8_t* mapped = nullptr;
    if (SUCCEEDED(h)) h = upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    if (FAILED(h)) {
        error = "BGRA upload " + hr(h);
        return nullptr;
    }
    for (int y = 0; y < height; ++y)
        std::memcpy(mapped + foot.Offset + y * static_cast<size_t>(foot.Footprint.RowPitch),
                    pixels.data() + static_cast<size_t>(y) * width * 4,
                    static_cast<size_t>(width) * 4);
    upload->Unmap(0, nullptr);
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                              IID_PPV_ARGS(&list));
    D3D12_TEXTURE_COPY_LOCATION to = {}, from = {};
    to.pResource = texture.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.pResource = upload.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint = foot;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    list->ResourceBarrier(1, &barrier);
    list->Close();
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    d3d12::GpuFence fence;
    if (!fence.create(device, false, error)) return nullptr;
    const uint64_t value = fence.signal(queue.Get(), error);
    if (!value || fence.wait(value, 5000, error) != d3d12::GpuFence::Wait::Done) return nullptr;
    return texture;
}

void runD3d12(ID3D12Device* device, ID3D12Resource* source, const std::string& api,
              const std::string& priority, bool own, const Options& o,
              const capture::CursorState& cursor, const std::vector<uint8_t>& reference, Result& r)
{
    const bool compute = api != "ps";
    const D3D12_COMMAND_LIST_TYPE type =
        compute ? D3D12_COMMAND_LIST_TYPE_COMPUTE : D3D12_COMMAND_LIST_TYPE_DIRECT;
    r.api = api;
    r.queue = std::string(compute ? "COMPUTE " : "DIRECT ") + priorityLabel(priority) +
              (own ? ", own CreatorID" : ", default CreatorID");
    r.name = api + " " + r.queue;

    ComPtr<ID3D12CommandQueue> queue;
    std::string why;
    if (!makeQueue(device, type, priorityValue(priority), own, queue, why)) {
        r.refused = why;
        return;
    }

    convert::ColorConvert12 pixel;
    ComputeConvert computeConverter;
    std::string error;
    if (!compute) {
        if (!pixel.init(device, queue.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, o.sourceW, o.sourceH,
                        o.outputW, o.outputH, 0, 0, false, o.filter, error)) {
            r.refused = error;
            return;
        }
    } else {
        // "cs": typed UAVs on the planes where the GPU has them, R8/R8G8 and a
        // copy elsewhere; "cs-copy": the copy everywhere.
        bool typed = api == "cs";
        if (typed && !computeConverter.init(device, o.sourceW, o.sourceH, o.outputW, o.outputH,
                                            o.filter, true, error))
            typed = false;
        if (!typed && !computeConverter.init(device, o.sourceW, o.sourceH, o.outputW, o.outputH,
                                             o.filter, false, error)) {
            r.refused = error;
            return;
        }
        r.queue += typed ? ", typed UAV on NV12" : ", R8/R8G8 + copy into NV12";
        r.name = api + " " + r.queue;
    }

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    HRESULT h = device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(h))
        h = device->CreateCommandList(0, type, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(h)) h = list->Close();
    d3d12::GpuFence fence;
    d3d12::QueueTimer timer;
    if (FAILED(h) || !fence.create(device, false, error)) {
        r.refused = FAILED(h) ? "command list " + hr(h) : error;
        return;
    }
    if (!timer.init(device, queue.Get(), type, error))
        say("    (no timestamps on this queue: %s)\n", error.c_str());

    const capture::CursorState none;
    const convert::CursorDraw draw;
    Pacer pacer(o.rate);
    bool cursorUploaded = false;
    const int total = o.warmup + o.seconds * o.rate;
    int64_t deadline = 0;
    for (int i = 0; i < total; ++i) {
        const bool onTime = pacer.wait();
        const bool counted = i >= o.warmup;
        if (counted && r.start == 0) {
            r.start = epochSeconds();
            deadline = nowUs() + deadlineUs(o);
        }
        if (counted && nowUs() >= deadline) {
            r.cut = total - i;
            break;
        }
        if (counted && !onTime) ++r.late;

        const int64_t r0 = qpc();
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
        const int slot = timer.ready() ? timer.begin(list.Get()) : -1;
        bool recorded = true;
        if (!compute) {
            recorded =
                pixel.recordConvert(list.Get(), source, o.cursor ? cursor : none, draw, error);
        } else {
            if (o.cursor && !cursorUploaded) {
                recorded = computeConverter.setCursor(list.Get(), cursor, error);
                cursorUploaded = true;
            }
            recorded = recorded && computeConverter.record(list.Get(), source,
                                                           o.cursor ? cursor : none, draw, error);
        }
        if (slot >= 0) timer.end(list.Get(), slot);
        if (!recorded || FAILED(list->Close())) {
            r.refused = "recording failed: " + error;
            return;
        }
        const int64_t t0 = qpc();
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        const uint64_t value = fence.signal(queue.Get(), error);
        const d3d12::GpuFence::Wait waited = fence.wait(value, 1000, error);
        const int64_t t1 = qpc();
        if (!compute) pixel.frameCompleted();
        if (waited == d3d12::GpuFence::Wait::DeviceRemoved) {
            r.refused = error;
            return;
        }
        d3d12::QueueTimer::Sample sample;
        const bool timed =
            slot >= 0 && waited == d3d12::GpuFence::Wait::Done && timer.read(slot, sample);
        if (!timed && slot >= 0) timer.cancel(slot);
        if (!counted) continue;
        if (waited != d3d12::GpuFence::Wait::Done) {
            ++r.timeouts;
            continue;
        }
        r.wall.push_back(qpcMs(t1 - t0));
        r.record.push_back(qpcMs(t0 - r0));
        if (timed) {
            r.gpu.push_back(static_cast<double>(sample.gpuUs) / 1000.0);
            if (sample.startQpc) r.wait.push_back(std::max(0.0, qpcMs(sample.startQpc - t0)));
        }
    }
    r.end = epochSeconds();

    // What was timed must be the same work: the last frame against the pixel
    // shaders' (the compute one differs by a code at most, the sRGB encode
    // of the resample being done in the shader there).
    std::string why2;
    std::vector<uint8_t> last =
        readNv12(device, compute ? computeConverter.output() : pixel.output(), why2);
    compare(reference, last, static_cast<size_t>(o.outputW) * o.outputH, r);
    if (!o.dump.empty()) r.last = std::move(last);
}

// ── D3D12 Video Process: the video engine's scaler (plan §9-15) ─────────────

/// The desktop as DDA delivers it, and the stream as the conversion writes it.
constexpr DXGI_COLOR_SPACE_TYPE kDesktopSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
constexpr DXGI_COLOR_SPACE_TYPE kStreamSpace = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;

/// A driver that faults inside the question is reported, not fatal (as in
/// `caps`, which met one on the N95's UHD Graphics).
HRESULT askGuarded(ID3D12VideoDevice* video, D3D12_FEATURE_VIDEO feature, void* data, UINT size)
{
    __try {
        return video->CheckFeatureSupport(feature, data, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return static_cast<HRESULT>(GetExceptionCode());
    }
}

std::string vpFeatures(D3D12_VIDEO_PROCESS_FEATURE_FLAGS flags)
{
    static const std::pair<D3D12_VIDEO_PROCESS_FEATURE_FLAGS, const char*> names[] = {
        {D3D12_VIDEO_PROCESS_FEATURE_FLAG_ALPHA_FILL, "alpha-fill"},
        {D3D12_VIDEO_PROCESS_FEATURE_FLAG_LUMA_KEY, "luma-key"},
        {D3D12_VIDEO_PROCESS_FEATURE_FLAG_STEREO, "stereo"},
        {D3D12_VIDEO_PROCESS_FEATURE_FLAG_ROTATION, "rotation"},
        {D3D12_VIDEO_PROCESS_FEATURE_FLAG_FLIP, "flip"},
        {D3D12_VIDEO_PROCESS_FEATURE_FLAG_ALPHA_BLENDING, "alpha-blending"},
        {D3D12_VIDEO_PROCESS_FEATURE_FLAG_PIXEL_ASPECT_RATIO, "pixel-aspect-ratio"},
    };
    std::string out;
    for (const auto& n : names)
        if (flags & n.first) out += (out.empty() ? "" : ",") + std::string(n.second);
    return out.empty() ? "none" : out;
}

/// What the driver answers for one conversion: "yes, ..." with the output
/// sizes it scales to and its features, "no", or the refusal. @p fits says
/// whether @p outW x @p outH is among the sizes.
std::string vpSupport(ID3D12VideoDevice* video, UINT inW, UINT inH, DXGI_FORMAT inFormat,
                      DXGI_COLOR_SPACE_TYPE inSpace, UINT outW, UINT outH, DXGI_FORMAT outFormat,
                      DXGI_COLOR_SPACE_TYPE outSpace, int rate, bool& fits)
{
    fits = false;
    D3D12_FEATURE_DATA_VIDEO_PROCESS_SUPPORT s = {};
    s.InputSample.Width = inW;
    s.InputSample.Height = inH;
    s.InputSample.Format = {inFormat, inSpace};
    s.InputFieldType = D3D12_VIDEO_FIELD_TYPE_NONE;
    s.InputStereoFormat = D3D12_VIDEO_FRAME_STEREO_FORMAT_NONE;
    s.InputFrameRate = {static_cast<UINT>(rate), 1};
    s.OutputFormat = {outFormat, outSpace};
    s.OutputStereoFormat = D3D12_VIDEO_FRAME_STEREO_FORMAT_NONE;
    s.OutputFrameRate = {static_cast<UINT>(rate), 1};
    const HRESULT h = askGuarded(video, D3D12_FEATURE_VIDEO_PROCESS_SUPPORT, &s, sizeof(s));
    if (FAILED(h)) return "refused " + hr(h);
    if (!(s.SupportFlags & D3D12_VIDEO_PROCESS_SUPPORT_FLAG_SUPPORTED)) return "no";
    const D3D12_VIDEO_SIZE_RANGE& range = s.ScaleSupport.OutputSizeRange;
    fits = outW >= range.MinWidth && outW <= range.MaxWidth && outH >= range.MinHeight &&
           outH <= range.MaxHeight;
    std::string out = "yes, output " + std::to_string(range.MinWidth) + "x" +
                      std::to_string(range.MinHeight) + " to " + std::to_string(range.MaxWidth) +
                      "x" + std::to_string(range.MaxHeight);
    if (s.ScaleSupport.Flags & D3D12_VIDEO_SCALE_SUPPORT_FLAG_POW2_ONLY)
        out += ", powers of two only";
    if (s.ScaleSupport.Flags & D3D12_VIDEO_SCALE_SUPPORT_FLAG_EVEN_DIMENSIONS_ONLY)
        out += ", even sizes only";
    if (!fits) out += " (" + std::to_string(outW) + "x" + std::to_string(outH) + " is not)";
    out += ", features " + vpFeatures(s.FeatureSupport);
    return out;
}

/// What the Video Process offers for the probe's conversion and for the
/// product's two others (HDR, and HDR tone-mapped into an SDR stream).
struct VpCaps
{
    std::string refused; ///< no ID3D12VideoDevice
    UINT maxInputStreams = 0;
    std::string sdr, hdr, toneMap;
    bool sdrFits = false;
};

VpCaps vpCaps(ID3D12Device* device, const Options& o)
{
    VpCaps caps;
    ComPtr<ID3D12VideoDevice> video;
    const HRESULT h = device->QueryInterface(IID_PPV_ARGS(&video));
    if (FAILED(h)) {
        caps.refused = "no ID3D12VideoDevice " + hr(h);
        return caps;
    }
    D3D12_FEATURE_DATA_VIDEO_PROCESS_MAX_INPUT_STREAMS streams = {};
    if (SUCCEEDED(askGuarded(video.Get(), D3D12_FEATURE_VIDEO_PROCESS_MAX_INPUT_STREAMS, &streams,
                             sizeof(streams))))
        caps.maxInputStreams = streams.MaxInputStreams;
    const UINT inW = static_cast<UINT>(o.sourceW), inH = static_cast<UINT>(o.sourceH);
    const UINT outW = static_cast<UINT>(o.outputW), outH = static_cast<UINT>(o.outputH);
    bool fits = false;
    caps.sdr = vpSupport(video.Get(), inW, inH, DXGI_FORMAT_B8G8R8A8_UNORM, kDesktopSpace, outW,
                         outH, DXGI_FORMAT_NV12, kStreamSpace, o.rate, caps.sdrFits);
    caps.hdr = vpSupport(video.Get(), inW, inH, DXGI_FORMAT_R16G16B16A16_FLOAT,
                         DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, outW, outH, DXGI_FORMAT_P010,
                         DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020, o.rate, fits);
    caps.toneMap = vpSupport(video.Get(), inW, inH, DXGI_FORMAT_R16G16B16A16_FLOAT,
                             DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, outW, outH, DXGI_FORMAT_NV12,
                             kStreamSpace, o.rate, fits);
    return caps;
}

/// The desktop scaled and converted by the Video Process, at a steady rate,
/// on a VIDEO_PROCESS queue; @p pointerTexture composed over it as a second
/// stream when given, where the conversion draws the pointer. Timed like the
/// others: the two timestamps of each submission resolve into a buffer of
/// their own (a video queue cannot write a readback heap), read back once at
/// the end and put on the CPU clock by calibrations taken along the run.
void runVp(ID3D12Device* device, ID3D12Resource* source, ID3D12Resource* pointerTexture,
           const std::string& priority, bool own, const Options& o,
           const capture::CursorState& cursor, const std::vector<uint8_t>& reference, Result& r)
{
    const bool withPointer = pointerTexture != nullptr;
    r.api = withPointer ? "vp-pointer" : "vp";
    r.queue = std::string("VIDEO_PROCESS ") + priorityLabel(priority) +
              (own ? ", own CreatorID" : ", default CreatorID");
    r.name = r.api + " " + r.queue;

    ComPtr<ID3D12VideoDevice> video;
    HRESULT h = device->QueryInterface(IID_PPV_ARGS(&video));
    if (FAILED(h)) {
        r.refused = "no ID3D12VideoDevice " + hr(h);
        return;
    }
    ComPtr<ID3D12CommandQueue> queue;
    std::string why;
    if (!makeQueue(device, D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS, priorityValue(priority), own,
                   queue, why)) {
        r.refused = why;
        return;
    }

    const UINT inW = static_cast<UINT>(o.sourceW), inH = static_cast<UINT>(o.sourceH);
    const UINT outW = static_cast<UINT>(o.outputW), outH = static_cast<UINT>(o.outputH);
    // The pointer where the conversion draws it: the picture's scale applied
    // to its place and its size.
    const double sx = static_cast<double>(outW) / inW, sy = static_cast<double>(outH) / inH;
    const RECT pointerRect = {std::lround(cursor.x * sx), std::lround(cursor.y * sy),
                              std::lround((cursor.x + cursor.width) * sx),
                              std::lround((cursor.y + cursor.height) * sy)};
    const UINT pointerW = static_cast<UINT>(std::max(1L, pointerRect.right - pointerRect.left));
    const UINT pointerH = static_cast<UINT>(std::max(1L, pointerRect.bottom - pointerRect.top));

    D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC in[2] = {};
    in[0].Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    in[0].ColorSpace = kDesktopSpace;
    in[0].SourceAspectRatio = {1, 1};
    in[0].DestinationAspectRatio = {1, 1};
    in[0].FrameRate = {static_cast<UINT>(o.rate), 1};
    in[0].SourceSizeRange = {inW, inH, inW, inH};
    in[0].DestinationSizeRange = {outW, outH, outW, outH};
    in[1] = in[0];
    in[1].SourceSizeRange = {static_cast<UINT>(cursor.width), static_cast<UINT>(cursor.height),
                             static_cast<UINT>(cursor.width), static_cast<UINT>(cursor.height)};
    in[1].DestinationSizeRange = {pointerW, pointerH, pointerW, pointerH};
    in[1].EnableAlphaBlending = TRUE;
    D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC out = {};
    out.Format = DXGI_FORMAT_NV12;
    out.ColorSpace = kStreamSpace;
    out.AlphaFillMode = D3D12_VIDEO_PROCESS_ALPHA_FILL_MODE_OPAQUE;
    out.FrameRate = {static_cast<UINT>(o.rate), 1};
    ComPtr<ID3D12VideoProcessor> processor;
    h = video->CreateVideoProcessor(0, &out, withPointer ? 2 : 1, in, IID_PPV_ARGS(&processor));
    if (FAILED(h)) {
        r.refused = "CreateVideoProcessor " + hr(h);
        return;
    }

    ComPtr<ID3D12Resource> output =
        makeTexture(device, outW, outH, 1, DXGI_FORMAT_NV12, D3D12_RESOURCE_FLAG_NONE);
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12VideoProcessCommandList> list;
    h = output ? device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS,
                                                IID_PPV_ARGS(&allocator))
               : E_OUTOFMEMORY;
    if (SUCCEEDED(h))
        h = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS, allocator.Get(),
                                      nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(h)) h = list->Close();
    d3d12::GpuFence fence;
    std::string error;
    if (FAILED(h) || !fence.create(device, false, error)) {
        r.refused = FAILED(h) ? "output or command list " + hr(h) : error;
        return;
    }

    const int total = o.warmup + o.seconds * o.rate;
    UINT64 frequency = 0;
    ComPtr<ID3D12QueryHeap> heap;
    ComPtr<ID3D12Resource> stamps;
    bool timed = !o.vpUntimed && SUCCEEDED(queue->GetTimestampFrequency(&frequency)) && frequency;
    if (timed) {
        D3D12_QUERY_HEAP_DESC hd = {};
        hd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        hd.Count = 2;
        timed = SUCCEEDED(device->CreateQueryHeap(&hd, IID_PPV_ARGS(&heap)));
    }
    if (timed) {
        stamps = makeBuffer(device, 16ull * total, D3D12_HEAP_TYPE_DEFAULT, false,
                            D3D12_RESOURCE_STATE_COMMON);
        timed = stamps != nullptr;
    }
    if (!timed) say("    (no timestamps on this queue)\n");
    struct Calibration
    {
        int64_t qpc;
        uint64_t gpu;
    };
    std::vector<Calibration> calibrations;
    const auto calibrate = [&] {
        UINT64 gpu = 0, cpu = 0;
        if (SUCCEEDED(queue->GetClockCalibration(&gpu, &cpu)))
            calibrations.push_back({static_cast<int64_t>(cpu), gpu});
    };
    if (timed) calibrate();
    std::vector<int64_t> submitted(static_cast<size_t>(total), 0);

    D3D12_RESOURCE_BARRIER before[4], after[4];
    UINT barriers = 0;
    const auto both = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES state) {
        before[barriers] = transition(resource, D3D12_RESOURCE_STATE_COMMON, state);
        after[barriers] = transition(resource, state, D3D12_RESOURCE_STATE_COMMON);
        ++barriers;
    };
    both(source, D3D12_RESOURCE_STATE_VIDEO_PROCESS_READ);
    both(output.Get(), D3D12_RESOURCE_STATE_VIDEO_PROCESS_WRITE);
    if (withPointer) both(pointerTexture, D3D12_RESOURCE_STATE_VIDEO_PROCESS_READ);
    // COPY_DEST, which the validation layer asks of a resolve's destination on
    // a video-process list too (VIDEO_PROCESS_WRITE is refused there).
    if (timed) both(stamps.Get(), D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_VIDEO_PROCESS_OUTPUT_STREAM_ARGUMENTS outArgs = {};
    outArgs.OutputStream[0].pTexture2D = output.Get();
    outArgs.TargetRectangle = {0, 0, static_cast<LONG>(outW), static_cast<LONG>(outH)};
    D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS inArgs[2] = {};
    inArgs[0].InputStream[0].pTexture2D = source;
    inArgs[0].Transform.SourceRectangle = {0, 0, static_cast<LONG>(inW), static_cast<LONG>(inH)};
    inArgs[0].Transform.DestinationRectangle = outArgs.TargetRectangle;
    inArgs[0].Transform.Orientation = D3D12_VIDEO_PROCESS_ORIENTATION_DEFAULT;
    inArgs[0].AlphaBlending = {FALSE, 1.0f};
    inArgs[1].InputStream[0].pTexture2D = pointerTexture;
    inArgs[1].Transform.SourceRectangle = {0, 0, cursor.width, cursor.height};
    inArgs[1].Transform.DestinationRectangle = pointerRect;
    inArgs[1].Transform.Orientation = D3D12_VIDEO_PROCESS_ORIENTATION_DEFAULT;
    inArgs[1].AlphaBlending = {TRUE, 1.0f};

    Pacer pacer(o.rate);
    int64_t deadline = 0;
    for (int i = 0; i < total; ++i) {
        const bool onTime = pacer.wait();
        const bool counted = i >= o.warmup;
        if (counted && r.start == 0) {
            r.start = epochSeconds();
            deadline = nowUs() + deadlineUs(o);
        }
        if (counted && nowUs() >= deadline) {
            r.cut = total - i;
            break;
        }
        if (counted && !onTime) ++r.late;
        if (timed && counted && (i - o.warmup) % (2 * o.rate) == 0) calibrate();

        const int64_t r0 = qpc();
        allocator->Reset();
        list->Reset(allocator.Get());
        list->ResourceBarrier(barriers, before);
        if (timed) list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        list->ProcessFrames(processor.Get(), &outArgs, withPointer ? 2 : 1, inArgs);
        if (timed) {
            list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, stamps.Get(),
                                   16ull * static_cast<UINT64>(i));
        }
        list->ResourceBarrier(barriers, after);
        h = list->Close();
        if (FAILED(h)) {
            r.refused = "recording failed " + hr(h);
            return;
        }
        const int64_t t0 = qpc();
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        const uint64_t value = fence.signal(queue.Get(), error);
        d3d12::GpuFence::Wait waited = fence.wait(value, 1000, error);
        const int64_t t1 = qpc();
        if (waited == d3d12::GpuFence::Wait::TimedOut) {
            // Not reused before it is done: the allocator is still the GPU's.
            if (counted) ++r.timeouts;
            waited = fence.wait(value, 10000, error);
            if (waited != d3d12::GpuFence::Wait::Done) {
                r.refused = "a submission did not complete: " + error;
                return;
            }
            continue;
        }
        if (waited != d3d12::GpuFence::Wait::Done) {
            r.refused = error;
            return;
        }
        if (!counted) continue;
        r.wall.push_back(qpcMs(t1 - t0));
        r.record.push_back(qpcMs(t0 - r0));
        submitted[static_cast<size_t>(i)] = t0;
    }
    r.end = epochSeconds();

    if (timed) {
        // Read back through a DIRECT queue, then each submission's stretch:
        // its GPU time, and its start on the CPU clock by the calibration
        // taken closest to it.
        ComPtr<ID3D12Resource> readback = makeBuffer(
            device, 16ull * total, D3D12_HEAP_TYPE_READBACK, false, D3D12_RESOURCE_STATE_COPY_DEST);
        ComPtr<ID3D12CommandQueue> copyQueue;
        ComPtr<ID3D12CommandAllocator> copyAllocator;
        ComPtr<ID3D12GraphicsCommandList> copy;
        d3d12::GpuFence copyFence;
        bool read = readback &&
                    makeQueue(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                              D3D12_COMMAND_QUEUE_PRIORITY_NORMAL, false, copyQueue, why) &&
                    SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                             IID_PPV_ARGS(&copyAllocator))) &&
                    SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                        copyAllocator.Get(), nullptr,
                                                        IID_PPV_ARGS(&copy))) &&
                    copyFence.create(device, false, why);
        if (read) {
            copy->CopyBufferRegion(readback.Get(), 0, stamps.Get(), 0, 16ull * total);
            copy->Close();
            ID3D12CommandList* copies[] = {copy.Get()};
            copyQueue->ExecuteCommandLists(1, copies);
            const uint64_t value = copyFence.signal(copyQueue.Get(), why);
            read = value && copyFence.wait(value, 5000, why) == d3d12::GpuFence::Wait::Done;
        }
        void* mapped = nullptr;
        const D3D12_RANGE range = {0, static_cast<SIZE_T>(16ull * total)};
        if (read && SUCCEEDED(readback->Map(0, &range, &mapped))) {
            const auto* s = static_cast<const uint64_t*>(mapped);
            LARGE_INTEGER qpcFrequency = {};
            ::QueryPerformanceFrequency(&qpcFrequency);
            for (int i = o.warmup; i < total; ++i) {
                const int64_t t0 = submitted[static_cast<size_t>(i)];
                const uint64_t start = s[2 * i], end = s[2 * i + 1];
                if (!t0 || !start || end < start) continue;
                r.gpu.push_back(static_cast<double>(end - start) * 1000.0 /
                                static_cast<double>(frequency));
                const Calibration* c = nullptr;
                for (const Calibration& k : calibrations)
                    if (!c || std::llabs(k.qpc - t0) < std::llabs(c->qpc - t0)) c = &k;
                if (!c) continue;
                const double seconds =
                    (static_cast<double>(start) - static_cast<double>(c->gpu)) / frequency;
                const int64_t startQpc =
                    c->qpc +
                    static_cast<int64_t>(seconds * static_cast<double>(qpcFrequency.QuadPart));
                r.wait.push_back(std::max(0.0, qpcMs(startQpc - t0)));
            }
            const D3D12_RANGE none = {0, 0};
            readback->Unmap(0, &none);
        } else {
            say("    (timestamps not read back: %s)\n", why.c_str());
        }
    }

    std::string why2;
    std::vector<uint8_t> last = readNv12(device, output.Get(), why2);
    compare(reference, last, static_cast<size_t>(outW) * outH, r);
    if (!o.dump.empty()) r.last = std::move(last);
}

/// One conversion by the pixel shaders, waited for and read back: the
/// picture every variant is held to.
std::vector<uint8_t> convertOnce(ID3D12Device* device, ID3D12Resource* source, const Options& o,
                                 convert::ScaleFilter filter, bool withPointer,
                                 const capture::CursorState& cursor, std::string& why)
{
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    convert::ColorConvert12 converter;
    d3d12::GpuFence fence;
    const convert::CursorDraw draw;
    const capture::CursorState none;
    if (makeQueue(device, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
                  false, queue, why) &&
        converter.init(device, queue.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, o.sourceW, o.sourceH,
                       o.outputW, o.outputH, 0, 0, false, filter, why) &&
        SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 IID_PPV_ARGS(&allocator))) &&
        SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                            nullptr, IID_PPV_ARGS(&list))) &&
        converter.recordConvert(list.Get(), source, withPointer ? cursor : none, draw, why) &&
        SUCCEEDED(list->Close()) && fence.create(device, false, why)) {
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        const uint64_t value = fence.signal(queue.Get(), why);
        if (value && fence.wait(value, 5000, why) == d3d12::GpuFence::Wait::Done)
            return readNv12(device, converter.output(), why);
    }
    return {};
}

void dumpNv12(const Options& o, const std::string& tag, const std::vector<uint8_t>& nv12)
{
    if (o.dump.empty() || nv12.empty()) return;
    std::string safe;
    for (char c : tag)
        safe += (std::isalnum(static_cast<unsigned char>(c)) || c == '-') ? c : '_';
    const std::wstring path = o.dump + L"-" + wide(safe) + L".nv12";
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(nv12.data()),
               static_cast<std::streamsize>(nv12.size()));
}

// ── The load's own frame rate ───────────────────────────────────────────────

double number(const std::string& line, const char* key, bool& found)
{
    const std::string k = std::string("\"") + key + "\":";
    const size_t at = line.find(k);
    found = at != std::string::npos;
    return found ? std::strtod(line.c_str() + at + k.size(), nullptr) : 0.0;
}

/// mw-gpu-load's --json: a "start" line with the local time, then one line a
/// second with "t" (seconds since start) and "fps".
void correlateLoad(const std::wstring& path, std::vector<Result>& results)
{
    std::ifstream file(path);
    if (!file) {
        say("load: could not read %s\n", utf8(path.c_str()).c_str());
        return;
    }
    struct Point
    {
        double at;
        double fps;
    };
    std::vector<Point> points;
    double start = 0;
    std::string line;
    while (std::getline(file, line)) {
        const size_t ts = line.find("\"ts\":\"");
        if (line.find("\"event\":\"start\"") != std::string::npos && ts != std::string::npos) {
            std::tm tm = {};
            if (sscanf_s(line.c_str() + ts + 6, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon,
                         &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
                tm.tm_year -= 1900;
                tm.tm_mon -= 1;
                tm.tm_isdst = -1;
                start = static_cast<double>(std::mktime(&tm));
            }
            continue;
        }
        bool hasT = false, hasFps = false;
        const double t = number(line, "t", hasT);
        const double fps = number(line, "fps", hasFps);
        if (hasT && hasFps && start > 0) points.push_back({start + t, fps});
    }
    for (Result& r : results) {
        double sum = 0;
        int n = 0;
        for (const Point& p : points)
            if (p.at >= r.start && p.at <= r.end) {
                sum += p.fps;
                ++n;
            }
        if (n > 0) {
            r.loadFps = sum / n;
            r.loadSeconds = n;
        }
    }
}

bool parse(int argc, wchar_t** argv, Options& o)
{
    for (int i = 0; i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? utf8(argv[++i]) : std::string(); };
        if (arg == L"--adapter") {
            o.adapter = next();
        } else if (arg == L"--seconds") {
            o.seconds = std::max(1, std::atoi(next().c_str()));
        } else if (arg == L"--rate") {
            o.rate = std::max(1, std::atoi(next().c_str()));
        } else if (arg == L"--warmup") {
            o.warmup = std::max(0, std::atoi(next().c_str()));
        } else if (arg == L"--size") {
            if (sscanf_s(next().c_str(), "%dx%d:%dx%d", &o.sourceW, &o.sourceH, &o.outputW,
                         &o.outputH) != 4)
                return false;
        } else if (arg == L"--filter") {
            const std::string f = next();
            if (f == "lanczos")
                o.filter = convert::ScaleFilter::Lanczos2;
            else if (f == "bilinear")
                o.filter = convert::ScaleFilter::Bilinear;
            else
                return false;
        } else if (arg == L"--no-cursor") {
            o.cursor = false;
        } else if (arg == L"--variants") {
            o.variants = split(next());
        } else if (arg == L"--priorities") {
            o.priorities = split(next());
        } else if (arg == L"--creators") {
            o.creators = split(next());
        } else if (arg == L"--class") {
            o.gpuClass = next();
        } else if (arg == L"--load-json") {
            o.loadJson = i + 1 < argc ? argv[++i] : L"";
        } else if (arg == L"--json") {
            o.jsonPath = i + 1 < argc ? argv[++i] : L"";
        } else if (arg == L"--no-json") {
            o.json = false;
        } else if (arg == L"--picture") {
            o.picture = i + 1 < argc ? argv[++i] : L"";
        } else if (arg == L"--dump") {
            o.dump = i + 1 < argc ? argv[++i] : L"";
        } else if (arg == L"--vp-untimed") {
            o.vpUntimed = true;
        } else {
            return false;
        }
    }
    return isGpuClassOption(o.gpuClass);
}

void jsonStats(Json& j, const char* key, const std::vector<double>& v)
{
    const Stats s = stats(v);
    j.key(key).beginObject();
    j.field("n", static_cast<unsigned long long>(s.n));
    j.field("meanMs", s.mean).field("p50Ms", s.p50).field("p99Ms", s.p99).field("maxMs", s.max);
    j.endObject();
}

} // namespace

void queuesUsage()
{
    std::puts(
        "mw-d3d12-lab queues [options]\n"
        "  Where the conversion waits: the product's 1440p->1080p conversion (Lanczos-2 +\n"
        "  pointer) at a steady rate on D3D11, on D3D12 DIRECT (pixel shaders) and on D3D12\n"
        "  COMPUTE, the D3D12 ones at each queue priority and CreatorID; and the driver's\n"
        "  own scaler, D3D12 Video Process on a VIDEO_PROCESS queue (Intel: the video\n"
        "  engine's SFC), alone or with the pointer as a second stream. Wall, GPU and\n"
        "  queue-wait times per submission. Run it under the load you want to measure.\n"
        "  --adapter <gpu>          a DXGI index or a piece of the name (RTX, Arc...)\n"
        "  --seconds <s>            per variant (default 30), after --warmup <n> (default 60)\n"
        "  --rate <n>               submissions per second (default 120)\n"
        "  --size WxH:wxh           source and stream (default 2560x1440:1920x1080)\n"
        "  --picture <file>         a real desktop (PNG, JPEG, BMP) instead of the synthetic\n"
        "                           one; its size is the source\n"
        "  --filter lanczos|bilinear\n"
        "  --no-cursor\n"
        "  --variants d3d11,ps,cs,cs-copy,vp,vp-pointer   (default d3d11,ps,cs)\n"
        "  --priorities normal,high,realtime --creators default,own\n"
        "  --class auto|high|normal the process's GPU scheduling class (auto = REALTIME\n"
        "                           where the token allows it, as the product asks)\n"
        "  --load-json <file>       mw-gpu-load's --json, for the load's frame rate\n"
        "  --dump <prefix>          each variant's last picture, raw NV12 "
        "(<prefix>-<variant>.nv12)\n"
        "  --vp-untimed             no timestamps on the VIDEO_PROCESS queue\n"
        "  --json <file> | --no-json");
}

int runQueues(int argc, wchar_t** argv)
{
    Options o;
    if (!parse(argc, argv, o)) {
        queuesUsage();
        return 2;
    }
    NativeHost::setLogSink([](int level, const std::string& message) {
        if (level >= 1) std::printf("    %s\n", message.c_str());
    });

    // A real desktop sets the source size before anything is made.
    std::vector<uint8_t> pixels;
    if (!o.picture.empty()) {
        ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::string why;
        if (!loadPicture(o.picture, o.sourceW, o.sourceH, pixels, why)) {
            say("%s\n", why.c_str());
            return 2;
        }
    }

    // The process's GPU class, the product's way.
    StreamPriority priority;
    const GpuScheduling scheduling = takeGpuClass(priority, o.gpuClass);

    const std::vector<Adapter> all = adapters(false);
    const Adapter* a = pickAdapter(all, o.adapter);
    if (!a) {
        say("no such adapter\n");
        return 2;
    }

    say("mw-d3d12-lab queues - %s on %s\n", nowText().c_str(), computerName().c_str());
    say("%s (LUID %s, HAGS %s), driver %s, Windows %s\n", a->name.c_str(),
        luidHex(a->desc.AdapterLuid).c_str(), hagsState(a->desc.AdapterLuid).c_str(),
        umdVersion(a->adapter.Get()).c_str(), osBuild().c_str());
    say("token %s, base-priority privilege %s, GPU class %s\n", scheduling.token.c_str(),
        scheduling.privilege ? "enabled" : "not held", scheduling.gpuClass.c_str());
    say("%dx%d -> %dx%d %s%s, %d submissions/s for %d s each (+%d warm-up)\n", o.sourceW, o.sourceH,
        o.outputW, o.outputH, o.filter == convert::ScaleFilter::Lanczos2 ? "Lanczos-2" : "bilinear",
        o.cursor ? " + pointer" : "", o.rate, o.seconds, o.warmup);
    say("desktop: %s\n\n",
        o.picture.empty() ? "synthetic (detail everywhere)" : utf8(o.picture.c_str()).c_str());

    if (pixels.empty()) pixels = desktop(o.sourceW, o.sourceH);
    const capture::CursorState cursor = pointer();
    std::vector<Result> results;

    if (has(o.variants, "d3d11")) {
        Result r;
        say("  running d3d11...\n");
        runD3d11(*a, o, pixels, cursor, r);
        results.push_back(std::move(r));
    }

    std::string error;
    std::shared_ptr<d3d12::D3d12Device> device =
        d3d12::D3d12Device::forAdapter(d3d12::luidValue(a->desc.AdapterLuid), error);
    ComPtr<ID3D12Resource> source;
    if (device) source = uploadBgra(device->device(), o.sourceW, o.sourceH, pixels, error);
    // The picture every D3D12 variant is held to: one conversion by the pixel
    // shaders, pointer included, read back.
    std::vector<uint8_t> reference;
    const bool vp = has(o.variants, "vp") || has(o.variants, "vp-pointer");
    // For the Video Process: the same without the pointer (what `vp` draws),
    // and the pixel shaders' bilinear against their Lanczos-2 — the yardstick
    // for how far a scaler's picture lies from the product's.
    std::vector<uint8_t> referencePlain, bilinear;
    Result yardstick;
    const size_t lumaBytes = static_cast<size_t>(o.outputW) * o.outputH;
    if (device && source) {
        std::string why;
        reference = convertOnce(device->device(), source.Get(), o, o.filter, o.cursor, cursor, why);
        if (reference.empty()) say("  (no reference picture: %s)\n", why.c_str());
        if (vp) {
            referencePlain = o.cursor ? convertOnce(device->device(), source.Get(), o, o.filter,
                                                    false, cursor, why)
                                      : reference;
            const bool scaled = o.sourceW != o.outputW || o.sourceH != o.outputH;
            if (scaled && o.filter == convert::ScaleFilter::Lanczos2) {
                bilinear = convertOnce(device->device(), source.Get(), o,
                                       convert::ScaleFilter::Bilinear, false, cursor, why);
                compare(referencePlain, bilinear, lumaBytes, yardstick);
            }
        }
    }

    VpCaps caps;
    if (vp && device) {
        caps = vpCaps(device->device(), o);
        if (!caps.refused.empty()) {
            say("  Video Process: %s\n", caps.refused.c_str());
        } else {
            say("  Video Process, up to %u input streams\n", caps.maxInputStreams);
            say("    BGRA sRGB -> NV12 BT.709 limited: %s\n", caps.sdr.c_str());
            say("    FP16 scRGB -> P010 BT.2020 PQ (HDR): %s\n", caps.hdr.c_str());
            say("    FP16 scRGB -> NV12 BT.709 limited (tone map): %s\n", caps.toneMap.c_str());
        }
        if (yardstick.psnrY >= 0)
            say("  yardstick: the pixel shaders' bilinear against their Lanczos-2, Y %.2f dB, "
                "UV %.2f dB\n",
                yardstick.psnrY, yardstick.psnrUV);
        say("\n");
    }

    if (!device || !source) {
        say("  no D3D12 variants: %s\n", error.c_str());
    } else {
        for (const char* api : {"ps", "cs", "cs-copy"}) {
            if (!has(o.variants, api)) continue;
            for (const std::string& p : o.priorities) {
                for (const std::string& c : o.creators) {
                    Result r;
                    say("  running %s %s %s...\n", api, p.c_str(), c.c_str());
                    runD3d12(device->device(), source.Get(), api, p, c == "own", o, cursor,
                             reference, r);
                    device->relayMessages();
                    if (!r.refused.empty()) say("    %s\n", r.refused.c_str());
                    results.push_back(std::move(r));
                    std::string gone;
                    if (device->removed(gone)) {
                        say("  %s - stopping\n", gone.c_str());
                        break;
                    }
                }
            }
        }
        ComPtr<ID3D12Resource> pointerTexture;
        if (has(o.variants, "vp-pointer") && o.cursor)
            pointerTexture =
                uploadBgra(device->device(), cursor.width, cursor.height, cursor.pixels, error);
        for (const char* api : {"vp", "vp-pointer"}) {
            if (!has(o.variants, api)) continue;
            const bool withPointer = std::string(api) == "vp-pointer";
            for (const std::string& p : o.priorities) {
                for (const std::string& c : o.creators) {
                    Result r;
                    say("  running %s %s %s...\n", api, p.c_str(), c.c_str());
                    if (!caps.refused.empty()) {
                        r.api = api;
                        r.name = std::string(api) + " (no Video Process)";
                        r.refused = caps.refused;
                    } else if (withPointer && (!o.cursor || caps.maxInputStreams < 2)) {
                        r.api = api;
                        r.name = std::string(api) + " " + p + " " + c;
                        r.refused =
                            !o.cursor ? "--no-cursor" : "the driver composes one input stream only";
                    } else {
                        runVp(device->device(), source.Get(),
                              withPointer ? pointerTexture.Get() : nullptr, p, c == "own", o,
                              cursor, withPointer ? reference : referencePlain, r);
                    }
                    device->relayMessages();
                    if (!r.refused.empty()) say("    %s\n", r.refused.c_str());
                    results.push_back(std::move(r));
                    std::string gone;
                    if (device->removed(gone)) {
                        say("  %s - stopping\n", gone.c_str());
                        break;
                    }
                }
            }
        }
    }

    if (!o.loadJson.empty()) correlateLoad(o.loadJson, results);

    say("\n%-62s %5s | %-27s | %-13s | %-13s | %6s | %4s | %-14s | %s\n", "variant", "n",
        "wall ms mean/p50/p99/max", "gpu mean/p99", "wait mean/p99", "record", "late",
        "vs PS Y dB/max", "load i/s");
    for (const Result& r : results) {
        if (!r.refused.empty()) {
            say("%-62s refused: %s\n", r.name.c_str(), r.refused.c_str());
            continue;
        }
        const Stats w = stats(r.wall), g = stats(r.gpu), q = stats(r.wait), rec = stats(r.record);
        char gpu[32] = "-", wait[32] = "-", same[32] = "-", load[32] = "-";
        if (g.n) std::snprintf(gpu, sizeof(gpu), "%5.2f %6.2f", g.mean, g.p99);
        if (q.n) std::snprintf(wait, sizeof(wait), "%5.2f %6.2f", q.mean, q.p99);
        if (r.maxDiff >= 0) std::snprintf(same, sizeof(same), "%5.2f <=%d", r.psnrY, r.maxDiff);
        if (r.loadFps >= 0) std::snprintf(load, sizeof(load), "%.1f", r.loadFps);
        say("%-62s %5zu | %5.2f %5.2f %6.2f %6.2f | %-13s | %-13s | %6.3f | %4d | %-14s | %s\n",
            r.name.c_str(), w.n, w.mean, w.p50, w.p99, w.max, gpu, wait, rec.mean, r.late, same,
            load);
        if (r.cut > 0) say("%-62s (out of time: %d submissions not made)\n", "", r.cut);
    }
    say("(vs PS: the last frame against ColorConvert12's, the luma PSNR (99 = identical) and\n"
        " the largest code difference; wait: GPU start minus submit, D3D12 only)\n");
    if (vp) {
        double y = -1, u = -1, v = -1;
        means(referencePlain, lumaBytes, y, u, v);
        say("\nplane means Y/U/V - pixel shaders %.2f/%.2f/%.2f\n", y, u, v);
        for (const Result& r : results)
            if (r.refused.empty() && r.meanY >= 0)
                say("  %-60s %.2f/%.2f/%.2f, UV %.2f dB\n", r.name.c_str(), r.meanY, r.meanU,
                    r.meanV, r.psnrUV);
    }

    dumpNv12(o, "reference", reference);
    if (o.cursor) dumpNv12(o, "reference-plain", referencePlain);
    dumpNv12(o, "bilinear", bilinear);
    for (const Result& r : results)
        dumpNv12(o, r.name, r.last);

    if (o.json) {
        Json j;
        j.beginObject();
        j.field("tool", "mw-d3d12-lab").field("command", "queues").field("schema", 1);
        j.field("date", nowText()).field("computer", computerName()).field("os", osBuild());
        j.field("gpu", a->name).field("luid", luidHex(a->desc.AdapterLuid));
        j.field("hags", hagsState(a->desc.AdapterLuid))
            .field("driver", umdVersion(a->adapter.Get()));
        j.field("token", scheduling.token).field("basePriorityPrivilege", scheduling.privilege);
        j.field("gpuClass", scheduling.gpuClass);
        j.field("source", std::to_string(o.sourceW) + "x" + std::to_string(o.sourceH));
        j.field("output", std::to_string(o.outputW) + "x" + std::to_string(o.outputH));
        j.field("filter", o.filter == convert::ScaleFilter::Lanczos2 ? "lanczos2" : "bilinear");
        j.field("cursor", o.cursor).field("rate", o.rate).field("seconds", o.seconds);
        j.field("picture", o.picture.empty() ? std::string("synthetic") : utf8(o.picture.c_str()));
        if (vp) {
            j.key("videoProcess").beginObject();
            if (!caps.refused.empty()) {
                j.field("refused", caps.refused);
            } else {
                j.field("maxInputStreams", static_cast<unsigned long long>(caps.maxInputStreams));
                j.field("sdr", caps.sdr).field("hdr", caps.hdr).field("toneMap", caps.toneMap);
            }
            j.endObject();
            double y = -1, u = -1, v = -1;
            means(referencePlain, lumaBytes, y, u, v);
            j.key("referenceMeans").beginObject();
            j.field("y", y).field("u", u).field("v", v);
            j.endObject();
            if (yardstick.psnrY >= 0)
                j.field("bilinearVsLanczosPsnrY", yardstick.psnrY)
                    .field("bilinearVsLanczosPsnrUV", yardstick.psnrUV);
        }
        j.key("variants").beginArray();
        for (const Result& r : results) {
            j.beginObject();
            j.field("name", r.name).field("api", r.api).field("queue", r.queue);
            if (!r.refused.empty()) {
                j.field("refused", r.refused);
            } else {
                jsonStats(j, "wall", r.wall);
                jsonStats(j, "gpu", r.gpu);
                jsonStats(j, "wait", r.wait);
                jsonStats(j, "record", r.record);
                j.field("late", r.late).field("timeouts", r.timeouts).field("cut", r.cut);
                j.field("start", r.start).field("end", r.end);
                if (r.maxDiff >= 0) {
                    j.field("maxDiffVsPs", r.maxDiff)
                        .field("diffsOver2VsPs", static_cast<unsigned long long>(r.bigDiffs));
                    j.field("psnrYVsPs", r.psnrY).field("psnrUvVsPs", r.psnrUV);
                    j.field("meanY", r.meanY).field("meanU", r.meanU).field("meanV", r.meanV);
                }
                if (r.loadFps >= 0)
                    j.field("loadFps", r.loadFps).field("loadSeconds", r.loadSeconds);
            }
            j.endObject();
        }
        j.endArray();
        j.endObject();
        std::wstring path = o.jsonPath;
        if (path.empty()) path = wide("queues-" + computerName() + "-" + nowStamp() + ".json");
        std::ofstream file(path, std::ios::binary);
        file << j.str() << '\n';
        say("\nJSON: %s\n", utf8(path.c_str()).c_str());
    }
    priority.release();
    return 0;
}

} // namespace lab
