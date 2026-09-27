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

#include "encode/HevcDpb.h"
#include "encode/HevcEncodeNegotiation.h"
#include "encode/HevcSliceParser.h"
#include "encode/QpRateController.h"
#include "encode/windows/d3d12/IVideoEncoder12.h"
#include "encode/windows/d3d12/VideoEncodeCaps12.h"
#include "platform/windows/d3d12/D3d12Device.h"

#include <d3d12video.h>
#include <wrl/client.h>

#include <memory>
#include <string>
#include <vector>

namespace mw::native::encode {

/// HEVC through D3D12 Video Encode: the driver writes the slices, we write the
/// rest.
///
/// ── One picture ─────────────────────────────────────────────────────────────
///
/// HevcDpb says what the picture is and what it keeps; one command list on the
/// VIDEO_ENCODE queue waits for the conversion's fence, encodes, resolves the
/// metadata, and signals; the CPU waits for that signal and nothing else (3 s
/// at most — beyond, the GPU is gone, not slow). The bitstream lands in system
/// memory, read in place: no copy on the GPU, one on the way out, as
/// everywhere.
///
/// ── The headers ─────────────────────────────────────────────────────────────
///
/// VPS, SPS and PPS never change during a session, so they are written once,
/// at init, right in front of the offset the driver writes its slices at: an
/// IDR goes out as one run, headers and slices, without a byte moved. The
/// first pictures' slice headers are read back with those very parameter sets
/// (HevcSliceParser) — a driver whose slices say something else sends the
/// session back to D3D11 before a viewer sees noise.
///
/// ── The rate control ────────────────────────────────────────────────────────
///
/// The driver's CBR where it can move its target in flight (the RTX, the AMD
/// iGPU); where it cannot (the Arc), a constant QP that our QpRateController
/// moves picture by picture — the bench's rc12= forces either. The driver is
/// held to it: the QP it says it coded must follow the one asked, at a
/// constant distance, or the session goes back to D3D11 (plan §4.6: the Arc
/// applies a QP changed without being told, which nothing documents). A
/// picture encoded again without a new conversion — the same fence value as
/// the last one — is the same picture: a still-screen pass or the idle floor,
/// which the controller sizes apart from new pictures.
///
/// ── What it does not do yet ────────────────────────────────────────────────
///
/// HEVC only (H.264 and AV1 are Phase 9).
class VideoEncode12 : public IVideoEncoder12
{
public:
    VideoEncode12() = default;
    ~VideoEncode12() override;

    bool init(const std::shared_ptr<d3d12::D3d12Device>& device, Codec codec, int width, int height,
              int fps, int bitrateKbps, bool hdr, bool intraRefresh, const EncoderTuning& tuning,
              std::string& error) override;

    int codedWidth() const override { return static_cast<int>(m_Setup.codedWidth); }
    int codedHeight() const override { return static_cast<int>(m_Setup.codedHeight); }

    bool encode(ID3D12Resource* picture, ID3D12Fence* ready, uint64_t readyValue,
                bool forceKeyframe, uint32_t frameNumber, EncoderOutput& out,
                std::string& error) override;

    bool supportsReferenceInvalidation() const override { return m_Dpb.capacity() > 1; }
    bool invalidateReference(uint32_t frameNumber, std::string& error) override;
    void releaseOutput() override { m_OutputHeld = false; }
    void stop() override;
    bool setBitrate(int bitrateKbps, std::string& error) override;
    bool intraRefreshEnabled() const override { return m_Setup.intraRefreshFrames > 0; }
    std::string describe() const override { return "D3D12 VE"; }

    /// What the negotiation settled, for the log and the tests.
    const HevcEncodeSetup& setup() const { return m_Setup; }
    const std::vector<uint8_t>& parameterSets() const { return m_Headers; }
    /// How many pictures' slice headers are still read back.
    int guardLeft() const { return m_GuardLeft; }
    /// Whether our rate control moves the QP (a CQP encoder), and its state.
    bool ownRateControl() const { return m_OwnRate; }
    const QpRateController& rateController() const { return m_Controller; }
    /// Pictures coded again for a strong overshoot (reencode=).
    int reencoded() const { return m_Reencoded; }
    /// Pictures whose QP, as the driver said it, did not follow the one asked.
    int qpNotFollowed() const { return m_QpNotFollowed; }
    /// The QP the driver said it coded the last picture at, -1 when it does
    /// not say.
    int driverQp() const { return m_DriverQp; }

private:
    bool createResources(std::string& error);
    /// One IDR of a blank picture, not kept: the driver's first-picture cost
    /// paid at build, and the header guard's first reading.
    bool warmUp(std::string& error);
    /// The size the driver wrote, or why the picture is not usable.
    bool written(uint64_t& bytes, std::string& error) const;
    /// Records and submits one picture, and waits for it.
    bool submit(ID3D12Resource* picture, ID3D12Fence* ready, uint64_t readyValue,
                const HevcDpb::Plan& plan, std::string& error);
    /// The slice headers of @p data read with our SPS and PPS, and checked
    /// against @p plan; false, with the reason, when they do not agree.
    bool guard(const uint8_t* data, size_t size, const HevcDpb::Plan& plan, std::string& error);
    /// The QP the driver says it coded the picture at: its average, or its
    /// first slice's once slices have been seen to move; -1 when neither says.
    int reportedQp(const uint8_t* slices, size_t size);
    /// The next picture's constant QP, said to a driver that takes a change
    /// in flight.
    void applyQp(int qp);
    /// Whether the driver coded @p asked, going by @p said; false, with the
    /// reason, once it has not for kQpMisses pictures in a row.
    bool qpFollowed(int asked, int said, std::string& error);
    D3D12_RESOURCE_BARRIER reconBarrier(int texture, UINT plane, D3D12_RESOURCE_STATES before,
                                        D3D12_RESOURCE_STATES after) const;
    ID3D12Resource* reconResource(int texture) const;
    UINT reconSubresource(int texture) const;

    std::shared_ptr<d3d12::D3d12Device> m_Device;
    Microsoft::WRL::ComPtr<ID3D12VideoDevice3> m_Video;
    Microsoft::WRL::ComPtr<ID3D12VideoEncoder> m_Encoder;
    Microsoft::WRL::ComPtr<ID3D12VideoEncoderHeap> m_Heap;
    /// The queue as the bench's knobs ask it (prio12, creator12).
    d3d12::QueueRequest m_QueueRequest;
    d3d12::Queue m_Queue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_Allocator;
    Microsoft::WRL::ComPtr<ID3D12VideoEncodeCommandList2> m_List;
    d3d12::GpuFence m_Encoded;

    HevcEncodeSetup m_Setup;
    D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC m_Level = {};
    D3D12_VIDEO_ENCODER_PROFILE_HEVC m_Profile = D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN;
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC m_Config = {};
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_HEVC m_Gop = {};
    std::unique_ptr<VideoEncodeCaps12::RateControl> m_Rate;
    bool m_RateChanged = false;
    int m_Fps = 60;
    int m_VbvFrames = 0; ///< the bench's vbv=, 0 for the shared rule
    DXGI_FORMAT m_Format = DXGI_FORMAT_NV12;

    /// Our rate control, on a CQP encoder (plan §4.6).
    bool m_OwnRate = false;
    bool m_Reencode = false; ///< the bench's reencode=
    QpRateController m_Controller;
    int m_SubmittedQp = 0; ///< the constant QP the encoder was last given
    /// The picture last encoded, by the fence value its conversion signalled.
    ID3D12Fence* m_LastReady = nullptr;
    uint64_t m_LastReadyValue = 0;
    /// How far the driver's QP sits from ours, once it has said one.
    bool m_QpOffsetKnown = false;
    int m_QpOffset = 0;
    int m_QpMisses = 0;
    int m_QpNotFollowed = 0;
    int m_DriverQp = -1;
    int m_Reencoded = 0;

    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_Recon;
    int m_ReconCount = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_Bitstream; ///< system memory, mapped
    uint8_t* m_BitstreamCpu = nullptr;
    uint64_t m_BitstreamSize = 0;
    uint64_t m_SliceOffset = 0; ///< where the driver writes; the headers end here
    Microsoft::WRL::ComPtr<ID3D12Resource> m_HwMetadata;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_Metadata; ///< system memory, mapped
    uint8_t* m_MetadataCpu = nullptr;

    HevcDpb m_Dpb;
    std::vector<uint8_t> m_Headers; ///< VPS, SPS, PPS, Annex-B
    HevcSpsFields m_SpsFields;
    HevcPpsFields m_PpsFields;
    int m_GuardLeft = 0;
    /// A slice QP has left the PPS's: the driver says its QP there (encode()).
    bool m_SliceQpMoves = false;
    uint32_t m_IntraRefreshIndex = 0;
    bool m_OutputHeld = false;
};

} // namespace mw::native::encode
