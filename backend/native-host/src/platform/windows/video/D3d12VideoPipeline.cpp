/*
 * MoonlightWeb — native capture & encoding engine.
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

#include "D3d12VideoPipeline.h"

#include "../../../convert/windows/ConvertShaders.h"
#include "../../../core/Log.h"
#include "../../../encode/windows/d3d12/VideoEncode12.h"

#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>

namespace mw::native {

namespace {

/// The adapters found without D3D12 Video Encode, and why (videoEncodeMissing).
std::mutex g_NoVideoEncodeLock;
std::map<uint64_t, std::string> g_NoVideoEncode;

/// The chains opened so far under MW_D3D12_FAULT=open@N: the Nth fails.
std::atomic<uint64_t> g_FaultedOpens{0};

/// MW_D3D12_FAULT, read when a chain is made (D3d12Fault).
D3d12Fault faultFromEnvironment()
{
    char value[32] = {};
    const DWORD n = ::GetEnvironmentVariableA("MW_D3D12_FAULT", value, sizeof(value));
    if (n == 0 || n >= sizeof(value)) return {};
    D3d12Fault fault;
    if (!parseD3d12Fault(value, fault)) {
        log::warning(std::string("[native] MW_D3D12_FAULT=") + value +
                     " is not a fault (open, convert, timeout, removed or encode, then @N) — "
                     "ignored");
        return {};
    }
    return fault;
}

/// The size the converter starts at: whole blocks of 16. The encoder settles
/// on whole coding tree blocks (32 or 64, HevcEncodeNegotiation.h), and the
/// converter is made again at that size.
int align16(int value)
{
    return (value + 15) & ~15;
}

d3d12::DdaSync syncOf(EncoderTuning::DdaSync sync)
{
    switch (sync) {
    case EncoderTuning::DdaSync::None: return d3d12::DdaSync::None;
    case EncoderTuning::DdaSync::Cpu: return d3d12::DdaSync::Cpu;
    default: return d3d12::DdaSync::Gpu;
    }
}

const char* syncName(d3d12::DdaSync sync)
{
    switch (sync) {
    case d3d12::DdaSync::None: return "none (bench)";
    case d3d12::DdaSync::Cpu: return "a CPU wait (bench)";
    default: return "fences on the GPU";
    }
}

/// The CPU's longest wait on the conversion queue: for the capture's release
/// under ddasync=cpu, and before a list is recorded again (d3d12::kGpuGoneMs).
constexpr uint32_t kWaitMs = d3d12::kGpuGoneMs;

} // namespace

D3d12VideoPipeline::D3d12VideoPipeline(const EncoderTuning& tuning)
    : m_Tuning(tuning)
    , m_Fault(faultFromEnvironment())
{}

D3d12VideoPipeline::~D3d12VideoPipeline()
{
    close();
}

std::string D3d12VideoPipeline::videoEncodeMissing(uint64_t encodeAdapterLuid)
{
    std::lock_guard<std::mutex> lock(g_NoVideoEncodeLock);
    const auto found = g_NoVideoEncode.find(encodeAdapterLuid);
    return found == g_NoVideoEncode.end() ? std::string() : found->second;
}

bool D3d12VideoPipeline::open(bool crossGpuCopy, uint64_t encodeAdapterLuid,
                              const std::string& encodeGpuName, std::string& error)
{
    close();
    if (crossGpuCopy) {
        error = "the frames cross to another GPU, over a D3D11 bridge";
        return false;
    }
    error = videoEncodeMissing(encodeAdapterLuid);
    if (!error.empty()) return false;
    m_Conversions = 0;
    if (m_Fault) {
        log::info("[native] " + describe(m_Fault) + " in effect: " + effect(m_Fault));
        if (m_Fault.kind == D3d12Fault::Kind::Open && ++g_FaultedOpens == m_Fault.at) {
            error = "fault injected (" + describe(m_Fault) + ")";
            return false;
        }
    }
    m_Device = d3d12::D3d12Device::forAdapter(encodeAdapterLuid, error);
    if (!m_Device) return false;
    ID3D12Device* device = m_Device->device();
    // The chain ends in D3D12 Video Encode: a device without it is refused
    // before its conversion is made, and remembered.
    Microsoft::WRL::ComPtr<ID3D12VideoDevice3> video;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&video)))) {
        error = "no D3D12 Video Encode on " + encodeGpuName +
                " (no ID3D12VideoDevice3: Windows 10, or a driver without it)";
        {
            std::lock_guard<std::mutex> lock(g_NoVideoEncodeLock);
            g_NoVideoEncode[encodeAdapterLuid] = error;
        }
        close();
        return false;
    }
    if (!m_Device->createQueue(d3d12::queueRequestFor(D3D12_COMMAND_LIST_TYPE_DIRECT, m_Tuning,
                                                      L"MoonlightWeb conversion"),
                               m_Queue, error)) {
        close();
        return false;
    }
    HRESULT h =
        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_Allocator));
    if (SUCCEEDED(h))
        h = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_Allocator.Get(), nullptr,
                                      IID_PPV_ARGS(&m_List));
    if (SUCCEEDED(h)) h = m_List->Close();
    if (FAILED(h)) {
        error = "the conversion's command list was refused (" + d3d12::hresultText(h) + ")";
        close();
        return false;
    }
    if (!m_Converted.create(device, false, error)) {
        close();
        return false;
    }
    m_Sync = syncOf(m_Tuning.ddaSync);
    m_Timing = false;
    if (m_Tuning.gpuTiming) {
        std::string why;
        if (m_Timer.init(device, m_Queue.queue.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, why))
            m_Timing = true;
        else
            log::warning("[native] D3D12 conversion: no GPU timing (" + why + ")");
    }
    log::info("[native] D3D12 chain on " + encodeGpuName + ": conversion on the " +
              m_Queue.description + ", capture handshake by " + syncName(m_Sync) +
              (m_Timing ? ", GPU-timed" : ""));
    return true;
}

void D3d12VideoPipeline::drain()
{
    if (!m_Queue.queue || !m_Converted) return;
    std::string ignored;
    const uint64_t value = m_Converted.signal(m_Queue.queue.Get(), ignored);
    if (value) m_Converted.wait(value, kWaitMs, ignored);
}

void D3d12VideoPipeline::teardown(bool keepHeld)
{
    // Nothing of ours may still read the surfaces, the held copy or the
    // encoder's input when they go — a stall injected on the queue included.
    releaseStall();
    drain();
    m_Stall.Reset();
    m_StallNext = false;
    m_EncodeFault = D3d12Fault::Kind::None;
    if (m_TimerSlot >= 0) {
        d3d12::QueueTimer::Sample unused;
        m_Timer.read(m_TimerSlot, unused);
        m_TimerSlot = -1;
    }
    m_KeptHeld.Reset();
    if (keepHeld && m_Converter) m_KeptHeld = m_Converter->takeHeld();
    // Released before the replacements are built, as on D3D11: an encoder
    // holds a hardware session.
    if (m_Encoder) m_Encoder->stop();
    m_Encoder.reset();
    m_Converter.reset();
    m_BlankReady = false;
    m_ReleaseAfter = 0;
    // A capture restart comes back on a new D3D11 device: bound again at the
    // next build.
    m_Interop.unbind();
    m_CaptureContext = nullptr;
}

bool D3d12VideoPipeline::buildConverter(const capture::IWindowsCapture& capture,
                                        const ConverterBuild& build, std::string& error)
{
    m_Lost.clear();
    if (!m_Device) {
        error = "the D3D12 chain was not opened";
        return false;
    }
    if (build.yuv444) {
        error = "4:4:4, which no D3D12 encoder takes";
        return false;
    }
    if (m_Tuning.conv12 == EncoderTuning::ConvertQueue12::Compute) {
        error = "the conversion on a COMPUTE queue is not built";
        return false;
    }
    std::string refusal;
    if (!m_Interop.bind(capture.device(), m_Device, refusal)) {
        error = "the capture cannot hand its pictures to D3D12 here: " + refusal;
        return false;
    }
    m_CaptureContext = capture.context();
    m_ConverterBuild = build;
    m_SourceFormat = capture.format();
    m_SourceWidth = capture.width();
    m_SourceHeight = capture.height();

    // The encoder settles its coded size once it is built, after this; the
    // converter starts at whole blocks of 16 and is made again in
    // buildEncoder if the encoder settles otherwise.
    convert::ConvertGeometry geometry;
    if (!convert::convertGeometry(m_SourceWidth, m_SourceHeight, build.outputWidth,
                                  build.outputHeight, build.filter, geometry, error))
        return false;
    return makeConverter(align16(geometry.outputWidth), align16(geometry.outputHeight), error);
}

bool D3d12VideoPipeline::makeConverter(int codedWidth, int codedHeight, std::string& error)
{
    // What the converter being replaced carries over: the held desktop (the
    // load cap's rebuild keeps it) and the SDR white the session set.
    Microsoft::WRL::ComPtr<ID3D12Resource> held = m_KeptHeld;
    m_KeptHeld.Reset();
    float white = 0.0f;
    if (m_Converter) {
        held = m_Converter->takeHeld();
        white = m_Converter->sdrWhite();
        m_Converter.reset();
    }
    auto converter = std::make_unique<convert::ColorConvert12>();
    if (!converter->init(m_Device->device(), m_Queue.queue.Get(), m_SourceFormat, m_SourceWidth,
                         m_SourceHeight, m_ConverterBuild.outputWidth,
                         m_ConverterBuild.outputHeight, codedWidth, codedHeight,
                         m_ConverterBuild.hdr, m_ConverterBuild.filter, error))
        return false;
    if (held) converter->setHeld(held);
    if (white > 0.0f) converter->setSdrWhite(white);
    m_Converter = std::move(converter);
    return true;
}

bool D3d12VideoPipeline::buildEncoder(const capture::IWindowsCapture& capture,
                                      const EncoderBuild& build, std::string& error)
{
    (void)capture;
    if (!m_Converter) {
        error = "no converter to encode from";
        return false;
    }
    if (build.tuning.enc12 == EncoderTuning::Encoder12::Nvenc ||
        build.tuning.enc12 == EncoderTuning::Encoder12::Amf) {
        error = "the vendors' SDKs fed D3D12 pictures are not built yet";
        return false;
    }
    if (build.yuv444) {
        error = "4:4:4, which no D3D12 encoder takes";
        return false;
    }
    auto encoder = std::make_unique<encode::VideoEncode12>();
    if (!encoder->init(m_Device, build.codec, m_Converter->outputWidth(),
                       m_Converter->outputHeight(), build.fps, build.bitrateKbps, build.hdr,
                       build.intraRefresh, build.tuning, error))
        return false;
    // The coded size is the encoder's to say; the converter writes it.
    if (encoder->codedWidth() != m_Converter->codedWidth() ||
        encoder->codedHeight() != m_Converter->codedHeight()) {
        log::info("[native] D3D12 conversion made again at the encoder's coded size " +
                  std::to_string(encoder->codedWidth()) + "x" +
                  std::to_string(encoder->codedHeight()));
        if (!makeConverter(encoder->codedWidth(), encoder->codedHeight(), error)) return false;
    }
    m_Encoder = std::move(encoder);
    return true;
}

void D3d12VideoPipeline::close()
{
    teardown(false);
    m_Timer.reset();
    m_Timing = false;
    m_Converted.reset();
    m_ConvertedValue = 0;
    m_List.Reset();
    m_Allocator.Reset();
    m_Queue = d3d12::Queue{};
    m_Device.reset();
    m_Lost.clear();
}

void D3d12VideoPipeline::lose(const std::string& reason)
{
    if (m_Lost.empty()) m_Lost = reason;
    if (m_TimerSlot >= 0) {
        m_Timer.cancel(m_TimerSlot);
        m_TimerSlot = -1;
    }
    // An injected stall lasts as long as the chain's patience, like a GPU
    // stuck behind a game that gets through once it can.
    releaseStall();
}

D3d12Fault::Kind D3d12VideoPipeline::nextConversion()
{
    ++m_Conversions;
    if (m_Fault.kind == D3d12Fault::Kind::Open || m_Conversions != m_Fault.at)
        return D3d12Fault::Kind::None;
    return m_Fault.kind;
}

bool D3d12VideoPipeline::armFault(D3d12Fault::Kind fault, std::string& error)
{
    switch (fault) {
    case D3d12Fault::Kind::Convert:
        error = "fault injected (" + describe(m_Fault) + ")";
        return false;
    case D3d12Fault::Kind::Timeout: m_StallNext = true; break;
    case D3d12Fault::Kind::Removed:
    case D3d12Fault::Kind::Encode: m_EncodeFault = fault; break;
    default: break;
    }
    return true;
}

bool D3d12VideoPipeline::stall(std::string& error)
{
    m_StallNext = false;
    HRESULT h = m_Device->device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_Stall));
    if (SUCCEEDED(h)) h = m_Queue.queue->Wait(m_Stall.Get(), 1);
    if (FAILED(h)) {
        error = "the injected stall was refused (" + d3d12::hresultText(h) + ")";
        m_Stall.Reset();
        return false;
    }
    log::info("[native] " + describe(m_Fault) + ": conversion " + std::to_string(m_Conversions) +
              " stalled on the GPU");
    return true;
}

void D3d12VideoPipeline::releaseStall()
{
    if (!m_Stall || m_Stall->GetCompletedValue() >= 1) return;
    // From the CPU: the queue goes on with what it holds.
    m_Stall->Signal(1);
    log::info("[native] " + describe(m_Fault) + ": the stalled conversion let go");
}

void D3d12VideoPipeline::removeDevice()
{
    Microsoft::WRL::ComPtr<ID3D12Device5> device5;
    if (FAILED(m_Device->device()->QueryInterface(IID_PPV_ARGS(&device5)))) {
        log::warning("[native] " + describe(m_Fault) +
                     ": no ID3D12Device5 to remove (Windows 10 before 1809) — nothing done");
        return;
    }
    log::info("[native] " + describe(m_Fault) + ": the D3D12 device removed before the picture " +
              "of conversion " + std::to_string(m_Conversions) + " is encoded");
    device5->RemoveDevice();
}

ID3D12GraphicsCommandList* D3d12VideoPipeline::beginList(std::string& error)
{
    // The list before this one has normally completed — the encode that
    // followed it waited for it — but a picture the cadence skipped was
    // converted and never encoded.
    if (m_ConvertedValue && m_Converted.completed() < m_ConvertedValue &&
        m_Converted.wait(m_ConvertedValue, kWaitMs, error) != d3d12::GpuFence::Wait::Done) {
        error = "the previous conversion did not finish: " + error;
        return nullptr;
    }
    HRESULT h = m_Allocator->Reset();
    if (SUCCEEDED(h)) h = m_List->Reset(m_Allocator.Get(), nullptr);
    if (FAILED(h)) {
        error = "the conversion's list could not be reset (" + d3d12::hresultText(h) + ")";
        return nullptr;
    }
    return m_List.Get();
}

bool D3d12VideoPipeline::submit(uint64_t acquired, bool signalCapture, std::string& error)
{
    const HRESULT h = m_List->Close();
    if (FAILED(h)) {
        error = "the conversion's list did not close (" + d3d12::hresultText(h) + ")";
        return false;
    }
    if (acquired && !m_Interop.conversionWaits(m_Queue.queue.Get(), acquired, error)) return false;
    if (m_StallNext && !stall(error)) return false;
    ID3D12CommandList* lists[] = {m_List.Get()};
    m_Queue.queue->ExecuteCommandLists(1, lists);
    if (signalCapture) {
        m_ReleaseAfter = m_Interop.conversionSignals(m_Queue.queue.Get(), error);
        if (m_ReleaseAfter == 0) return false;
    }
    m_ConvertedValue = m_Converted.signal(m_Queue.queue.Get(), error);
    return m_ConvertedValue != 0;
}

bool D3d12VideoPipeline::convert(capture::IWindowsCapture& capture, ID3D11Texture2D* captured,
                                 const capture::CursorState& cursor,
                                 const convert::CursorDraw& draw, bool retain, std::string& error)
{
    (void)error;
    m_ReleaseAfter = 0;
    if (!m_Lost.empty()) return true;
    const D3d12Fault::Kind fault = nextConversion();
    std::string why;
    ID3D12Resource* surface = m_Interop.open(captured, why);
    if (!surface) {
        lose("the captured surface does not open in D3D12: " + why);
        return true;
    }
    const uint64_t acquired = m_Interop.signalAcquired(capture.context(), m_Sync, why);
    if (m_Sync != d3d12::DdaSync::None && acquired == 0) {
        lose("fence A: " + why);
        return true;
    }
    // Without the handshake (a bench's none), what the acquire queued still
    // goes to the GPU now, as the lab's probe does it.
    if (m_Sync == d3d12::DdaSync::None) capture.context()->Flush();
    ID3D12GraphicsCommandList* list = beginList(why);
    if (!list) {
        lose(why);
        return true;
    }
    if (m_Timing) m_TimerSlot = m_Timer.begin(list);
    // The copy of the desktop for the pointer-only and still-screen paths, in
    // the same list: read before fence B, like the conversion.
    if (retain && !m_Converter->recordCopy(list, surface, why))
        log::warning("[native] could not keep a desktop copy: " + why);
    if (!m_Converter->recordConvert(list, surface, cursor, draw, why) || !armFault(fault, why)) {
        m_List->Close();
        lose("conversion: " + why);
        return true;
    }
    if (m_TimerSlot >= 0) m_Timer.end(list, m_TimerSlot);
    if (!submit(acquired, /*signalCapture=*/true, why)) lose(why);
    return true;
}

void D3d12VideoPipeline::beforeCaptureRelease()
{
    if (!m_CaptureContext || m_ReleaseAfter == 0) return;
    std::string why;
    if (!m_Interop.beforeRelease(m_CaptureContext, m_ReleaseAfter, m_Sync, kWaitMs, why))
        lose("the capture could not be held until the D3D12 read: " + why);
    m_ReleaseAfter = 0;
}

bool D3d12VideoPipeline::convertHeld(capture::IWindowsCapture& capture,
                                     const capture::CursorState& cursor,
                                     const convert::CursorDraw& draw, std::string& error)
{
    (void)capture;
    (void)error;
    m_ReleaseAfter = 0;
    if (!m_Lost.empty()) return true;
    if (!m_Converter || !m_Converter->held()) {
        lose("no desktop held to convert again");
        return true;
    }
    const D3d12Fault::Kind fault = nextConversion();
    std::string why;
    ID3D12GraphicsCommandList* list = beginList(why);
    if (!list) {
        lose(why);
        return true;
    }
    if (m_Timing) m_TimerSlot = m_Timer.begin(list);
    if (!m_Converter->recordConvert(list, m_Converter->held(), cursor, draw, why) ||
        !armFault(fault, why)) {
        m_List->Close();
        lose("conversion: " + why);
        return true;
    }
    if (m_TimerSlot >= 0) m_Timer.end(list, m_TimerSlot);
    if (!submit(0, /*signalCapture=*/false, why)) lose(why);
    return true;
}

bool D3d12VideoPipeline::prepareBlank(const capture::IWindowsCapture* capture, std::string& error)
{
    (void)capture;
    m_BlankReady = false;
    if (!m_Converter) {
        error = "no converter to clear";
        return false;
    }
    m_BlankReady = true;
    return true;
}

bool D3d12VideoPipeline::convertBlank(const convert::CursorDraw& draw, std::string& error)
{
    (void)draw;
    (void)error;
    if (!m_Lost.empty()) return true;
    const D3d12Fault::Kind fault = nextConversion();
    std::string why;
    ID3D12GraphicsCommandList* list = beginList(why);
    if (!list) {
        lose(why);
        return true;
    }
    if (!m_Converter->recordClearBlack(list, why) || !armFault(fault, why)) {
        m_List->Close();
        lose("black picture: " + why);
        return true;
    }
    if (!submit(0, /*signalCapture=*/false, why)) lose(why);
    return true;
}

WindowsVideoPipeline::EncodeResult D3d12VideoPipeline::encode(bool forceKeyframe,
                                                              uint32_t frameNumber,
                                                              encode::EncoderOutput& out,
                                                              std::string& error)
{
    if (!m_Lost.empty()) {
        error = m_Lost;
        return EncodeResult::Lost;
    }
    if (!m_Encoder || !m_Converter) {
        error = "the D3D12 chain is not built";
        return EncodeResult::Lost;
    }
    const D3d12Fault::Kind fault = m_EncodeFault;
    m_EncodeFault = D3d12Fault::Kind::None;
    if (fault == D3d12Fault::Kind::Removed) removeDevice();
    if (!m_Encoder->encode(m_Converter->output(), m_Converted.fence(), m_ConvertedValue,
                           forceKeyframe, frameNumber, out, error)) {
        std::string reason;
        if (m_Device->removed(reason)) error += " (the device is gone: " + reason + ")";
        lose(error);
        return EncodeResult::Lost;
    }
    // A picture the GPU coded after all is not taken from a device that is
    // gone; one the fault flags is thrown away, as a driver's error would be.
    std::string gone;
    if ((fault == D3d12Fault::Kind::Removed && m_Device->removed(gone)) ||
        fault == D3d12Fault::Kind::Encode) {
        m_Encoder->releaseOutput();
        error = fault == D3d12Fault::Kind::Encode
                    ? "fault injected (" + describe(m_Fault) + ")"
                    : "the picture came back from a device that is gone (" + gone + ")";
        lose(error);
        return EncodeResult::Lost;
    }
    // The conversion behind this picture is done — the encoder waited for it
    // on the GPU, the CPU for the bitstream — so its timings can be read. A
    // re-sent picture has none: nothing was converted for it.
    m_Converter->frameCompleted();
    ++m_Frames;
    if (m_TimerSlot >= 0) {
        d3d12::QueueTimer::Sample sample;
        if (m_Timer.read(m_TimerSlot, sample)) {
            out.gpuConvertUs = sample.gpuUs;
            m_GpuConvertUs += sample.gpuUs;
            ++m_TimedFrames;
        }
        m_TimerSlot = -1;
    }
    return EncodeResult::Ok;
}

bool D3d12VideoPipeline::setBitrate(int bitrateKbps, std::string& error)
{
    if (!m_Encoder) {
        error = "no encoder";
        return false;
    }
    return m_Encoder->setBitrate(bitrateKbps, error);
}

bool D3d12VideoPipeline::invalidateReference(uint32_t frameNumber, std::string& error)
{
    if (!m_Encoder) {
        error = "no encoder";
        return false;
    }
    return m_Encoder->invalidateReference(frameNumber, error);
}

void D3d12VideoPipeline::setSdrWhite(float scRgbWhite)
{
    if (m_Converter) m_Converter->setSdrWhite(scRgbWhite);
}

convert::ScaleFilter D3d12VideoPipeline::scaleFilter() const
{
    return m_Converter ? m_Converter->scaleFilter() : convert::ScaleFilter::Bilinear;
}

bool D3d12VideoPipeline::takeResampleCost(int64_t& costUs)
{
    return m_Converter && m_Converter->takeResampleCost(costUs);
}

bool D3d12VideoPipeline::dropResample()
{
    return m_Converter && m_Converter->dropResample();
}

void D3d12VideoPipeline::logEndOfSession() const
{
    if (m_TimedFrames == 0) return;
    char mean[32];
    std::snprintf(mean, sizeof(mean), "%.2f",
                  static_cast<double>(m_GpuConvertUs) / static_cast<double>(m_TimedFrames) /
                      1000.0);
    log::info("[native] D3D12 conversion: " + std::to_string(m_Frames) + " pictures encoded, " +
              mean + " ms of GPU each on average over " + std::to_string(m_TimedFrames) + " timed");
}

} // namespace mw::native
