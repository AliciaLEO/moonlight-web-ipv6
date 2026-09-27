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

#pragma once

#include "WindowsVideoPipeline.h"

#include "../../../convert/windows/d3d12/ColorConvert12.h"
#include "../../../encode/windows/d3d12/IVideoEncoder12.h"
#include "../d3d12/D3d12Device.h"
#include "../d3d12/DdaInterop.h"

#include <wrl/client.h>

#include <memory>

namespace mw::native {

/// The D3D12 chain (plan pipeline-video-d3d12-v2, §3.1). One picture, one
/// thread, no queue of frames:
///
///  1. the capture's D3D11 context signals fence A right after the acquire;
///  2. the conversion queue waits for it on the GPU, converts the surface
///     opened in D3D12 into the encoder's input, and signals fence B and the
///     pipeline's own fence;
///  3. the capture context waits for fence B on the GPU before ReleaseFrame,
///     so the compositor cannot write the surface under the read;
///  4. the encoder waits for the pipeline's fence on its own queue, and the
///     CPU waits for the bitstream — the frame's only CPU wait.
///
/// Anything that goes wrong once a session is streaming — a device removed,
/// a fence past its deadline, an encoder error, the header guard — makes
/// encode() answer Lost, and the session goes back to D3D11 for good. A
/// conversion that fails is kept for that answer rather than returned: the
/// session ends on a failed conversion, and D3D12 must never end a session
/// D3D11 could have carried.
class D3d12VideoPipeline final : public WindowsVideoPipeline
{
public:
    /// @p tuning: the bench's D3D12 knobs, fixed for the session (queues,
    /// handshake, GPU timing); the defaults are the engine's own.
    explicit D3d12VideoPipeline(const EncoderTuning& tuning);
    ~D3d12VideoPipeline() override;

    const char* kind() const override { return "d3d12"; }

    /// @p crossGpuCopy is refused (the bridge is D3D11's); the device is the
    /// one on @p encodeAdapterLuid, which is then the display's.
    bool open(bool crossGpuCopy, uint64_t encodeAdapterLuid, const std::string& encodeGpuName,
              std::string& error) override;
    void teardown(bool keepHeld) override;
    /// The queues were made at their priority in open(); the capture's device
    /// is the session's to raise.
    void raisePriority() override {}
    bool buildConverter(const capture::IWindowsCapture& capture, const ConverterBuild& build,
                        std::string& error) override;
    bool buildEncoder(const capture::IWindowsCapture& capture, const EncoderBuild& build,
                      std::string& error) override;
    void close() override;
    bool hasEncoder() const override { return m_Encoder != nullptr; }
    bool built() const override { return m_Converter && m_Encoder; }

    bool convert(capture::IWindowsCapture& capture, ID3D11Texture2D* captured,
                 const capture::CursorState& cursor, const convert::CursorDraw& draw, bool retain,
                 std::string& error) override;
    bool hasHeld() const override { return m_Converter && m_Converter->held() != nullptr; }
    bool convertHeld(capture::IWindowsCapture& capture, const capture::CursorState& cursor,
                     const convert::CursorDraw& draw, std::string& error) override;
    /// The black picture needs nothing from the capture: the converter clears
    /// its own output. Still false without a converter to clear.
    bool prepareBlank(const capture::IWindowsCapture* capture, std::string& error) override;
    bool hasBlank() const override { return m_BlankReady; }
    bool convertBlank(const convert::CursorDraw& draw, std::string& error) override;
    void releaseBlank() override { m_BlankReady = false; }
    void beforeCaptureRelease() override;

    EncodeResult encode(bool forceKeyframe, uint32_t frameNumber, encode::EncoderOutput& out,
                        std::string& error) override;
    void releaseOutput() override
    {
        if (m_Encoder) m_Encoder->releaseOutput();
    }
    bool setBitrate(int bitrateKbps, std::string& error) override;
    bool intraRefreshEnabled() const override
    {
        return m_Encoder && m_Encoder->intraRefreshEnabled();
    }
    int intraRefreshHorizonFrames() const override
    {
        return m_Encoder ? m_Encoder->intraRefreshHorizonFrames() : 0;
    }
    bool supportsReferenceInvalidation() const override
    {
        return m_Encoder && m_Encoder->supportsReferenceInvalidation();
    }
    bool invalidateReference(uint32_t frameNumber, std::string& error) override;

    int outputWidth() const override { return m_Converter ? m_Converter->outputWidth() : 0; }
    int outputHeight() const override { return m_Converter ? m_Converter->outputHeight() : 0; }
    /// The bitstream's read, as on D3D11: the surface is read in place.
    int copiesPerFrame() const override { return 1; }
    bool toneMapsToSdr() const override { return m_Converter && m_Converter->toneMapsToSdr(); }
    bool scRgbSource() const override { return m_Converter && m_Converter->scRgbSource(); }
    void setSdrWhite(float scRgbWhite) override;

    convert::ScaleFilter scaleFilter() const override;
    bool takeResampleCost(int64_t& costUs) override;
    bool dropResample() override;

    void logEndOfSession() const override;

    /// The converter's output, for the tests.
    ID3D12Resource* output() const { return m_Converter ? m_Converter->output() : nullptr; }

private:
    /// A converter at @p codedWidth × @p codedHeight, the held copy and the
    /// SDR white carried over from the one it replaces.
    bool makeConverter(int codedWidth, int codedHeight, std::string& error);
    /// A fresh list, its allocator reset: the previous one has completed (the
    /// encode that followed it waited for it).
    ID3D12GraphicsCommandList* beginList(std::string& error);
    /// Close and submit the list behind a wait for fence A (@p acquired, 0 for
    /// none), then signal fence B (captured pictures) and the pipeline's own.
    bool submit(uint64_t acquired, bool signalCapture, std::string& error);
    /// A failure kept for encode() to answer Lost with.
    void lose(const std::string& reason);
    /// The queue idle: nothing of ours still reads or writes a resource.
    void drain();

    EncoderTuning m_Tuning;
    std::shared_ptr<d3d12::D3d12Device> m_Device;
    d3d12::Queue m_Queue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_Allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_List;
    /// Signalled after each conversion: what the encoder waits for.
    d3d12::GpuFence m_Converted;
    uint64_t m_ConvertedValue = 0;
    d3d12::DdaInterop m_Interop;
    d3d12::DdaSync m_Sync = d3d12::DdaSync::Gpu;
    /// Fence B's value for the picture being released, 0 when none.
    uint64_t m_ReleaseAfter = 0;
    ID3D11DeviceContext* m_CaptureContext = nullptr;

    d3d12::QueueTimer m_Timer;
    bool m_Timing = false;
    int m_TimerSlot = -1;

    ConverterBuild m_ConverterBuild;
    DXGI_FORMAT m_SourceFormat = DXGI_FORMAT_UNKNOWN;
    int m_SourceWidth = 0;
    int m_SourceHeight = 0;
    std::unique_ptr<convert::ColorConvert12> m_Converter;
    /// The held desktop between a teardown that keeps it and the next build.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_KeptHeld;
    std::unique_ptr<encode::IVideoEncoder12> m_Encoder;
    bool m_BlankReady = false;
    /// Why the chain cannot go on, once it cannot: encode() answers Lost.
    std::string m_Lost;

    // End-of-session figures.
    uint64_t m_Frames = 0;
    uint64_t m_TimedFrames = 0;
    int64_t m_GpuConvertUs = 0;
};

} // namespace mw::native
