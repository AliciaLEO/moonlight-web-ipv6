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

#include "encode/windows/d3d12/VideoEncode12.h"

#include "core/Log.h"

#include <algorithm>
#include <cstring>

namespace mw::native::encode {

using Microsoft::WRL::ComPtr;

namespace {

/// A GPU that takes half a second over one picture is gone, not slow (plan
/// §3.1): the session goes back to D3D11 rather than freezing on it.
constexpr uint32_t kWaitMs = 500;

/// Pictures whose slice headers are read back with our SPS and PPS: the IDR
/// and the P pictures after it. HevcSliceParser is a sieve — a wrong PPS can
/// land on one header's alignment by chance, it does not land on six.
constexpr int kGuardedPictures = 6;

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after,
                                  UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = subresource;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

D3D12_RESOURCE_DESC bufferDesc(UINT64 size)
{
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return rd;
}

/// A buffer in system memory the GPU writes and the CPU reads in place — the
/// bitstream and the metadata: no copy on the GPU, and none waited for.
ComPtr<ID3D12Resource> systemBuffer(ID3D12Device* device, UINT64 size)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_CUSTOM;
    hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    const D3D12_RESOURCE_DESC rd = bufferDesc(size);
    ComPtr<ID3D12Resource> r;
    device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON,
                                    nullptr, IID_PPV_ARGS(&r));
    return r;
}

ComPtr<ID3D12Resource> videoBuffer(ID3D12Device* device, UINT64 size)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    const D3D12_RESOURCE_DESC rd = bufferDesc(size);
    ComPtr<ID3D12Resource> r;
    device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON,
                                    nullptr, IID_PPV_ARGS(&r));
    return r;
}

ComPtr<ID3D12Resource> texture(ID3D12Device* device, UINT width, UINT height, UINT16 slices,
                               DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = width;
    rd.Height = height;
    rd.DepthOrArraySize = slices;
    rd.MipLevels = 1;
    rd.Format = format;
    rd.SampleDesc.Count = 1;
    rd.Flags = flags;
    ComPtr<ID3D12Resource> r;
    device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON,
                                    nullptr, IID_PPV_ARGS(&r));
    return r;
}

std::string hex(uint64_t value)
{
    char text[24];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

std::string levelText(int idc)
{
    return std::to_string(idc / 30) + (idc % 30 ? "." + std::to_string(idc % 30 / 3) : "");
}

} // namespace

VideoEncode12::~VideoEncode12()
{
    stop();
}

bool VideoEncode12::init(const std::shared_ptr<d3d12::D3d12Device>& device, Codec codec, int width,
                         int height, int fps, int bitrateKbps, bool hdr, bool intraRefresh,
                         const EncoderTuning& tuning, std::string& error)
{
    stop();
    if (codec != Codec::Hevc) {
        error = std::string("D3D12 Video Encode does ") + toString(codec) + " later: HEVC only";
        return false;
    }
    if (!device || !device->device()) {
        error = "no D3D12 device";
        return false;
    }
    m_Device = device;
    if (FAILED(device->device()->QueryInterface(IID_PPV_ARGS(&m_Video)))) {
        error = "no ID3D12VideoDevice3 on " + device->name();
        stop();
        return false;
    }

    VideoEncodeCaps12 caps(m_Video.Get());
    HevcEncodeRequest request;
    request.width = static_cast<uint32_t>((std::max)(width, 0));
    request.height = static_cast<uint32_t>((std::max)(height, 0));
    request.fps = fps > 0 ? fps : 60;
    request.hdr = hdr;
    request.intraRefresh = intraRefresh;
    request.dpbFrames = tuning.dpbFrames;
    m_Setup = negotiateHevc(request, caps);
    if (!m_Setup.ok) {
        error = "D3D12 Video Encode takes no HEVC " + std::to_string(width) + "x" +
                std::to_string(height) + " on " + device->name() + ": " + m_Setup.reason;
        stop();
        return false;
    }
    m_Level = caps.suggestedLevel();
    m_Profile = VideoEncodeCaps12::profile(hdr);
    m_Config = VideoEncodeCaps12::configuration(m_Setup.blocks, m_Setup.blocksAnswer);
    // Endless, P only, the POC as wide as the SPS says.
    m_Gop = {0, 1, static_cast<UCHAR>(m_Setup.sequence.log2MaxPocLsb - 4)};
    m_Fps = request.fps;
    m_VbvFrames = tuning.vbvFrames;
    m_QueueRequest = d3d12::queueRequestFor(D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE, tuning,
                                            L"MoonlightWeb video encode");
    m_Format = hdr ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    m_Rate = std::make_unique<VideoEncodeCaps12::RateControl>(
        m_Setup.rate, m_Fps, static_cast<uint32_t>(bitrateKbps) * 1000u);
    m_Rate->setBitrate(static_cast<uint32_t>(bitrateKbps) * 1000u, m_Fps, m_VbvFrames);
    m_Dpb = HevcDpb(m_Setup.dpbCapacity, m_Fps);

    // The parameter sets, once for the session — and read back at once: a
    // writer that cannot read its own SPS has nothing to judge a slice with.
    m_Headers.clear();
    for (const auto& unit :
         {paramsets::hevcVps(m_Setup.sequence), paramsets::hevcSps(m_Setup.sequence),
          paramsets::hevcPps(m_Setup.sequence)})
        m_Headers.insert(m_Headers.end(), unit.begin(), unit.end());
    const std::vector<uint8_t> sps = paramsets::hevcSps(m_Setup.sequence);
    const std::vector<uint8_t> pps = paramsets::hevcPps(m_Setup.sequence);
    std::string unread = parseHevcSps(sps.data(), sps.size(), m_SpsFields);
    if (unread.empty()) unread = parseHevcPps(pps.data(), pps.size(), m_PpsFields);
    if (!unread.empty()) {
        error = "our own parameter sets do not read back: " + unread;
        stop();
        return false;
    }

    if (!createResources(error)) {
        stop();
        return false;
    }
    // The first encode costs what the next ones do not — the Arc spends
    // 12.7 ms on its first IDR, ~2.3 after (21/09/2026): paid here, on a blank
    // picture, and its slices are the first the guard reads, so a driver that
    // disagrees with our parameter sets is caught before the stream starts.
    if (!warmUp(error)) {
        error = "D3D12 Video Encode, first picture: " + error;
        stop();
        return false;
    }
    m_GuardLeft = kGuardedPictures;
    m_IntraRefreshIndex = 0;

    const HevcBlocks& b = m_Setup.blocks;
    log::info("[native] D3D12 Video Encode ready on " + device->name() + ": HEVC " +
              (hdr ? "Main 10 (BT.2020 PQ) " : "Main ") + std::to_string(width) + "x" +
              std::to_string(height) + " coded " + std::to_string(m_Setup.codedWidth) + "x" +
              std::to_string(m_Setup.codedHeight) + "@" + std::to_string(m_Fps) + ", " +
              m_Setup.rate.describe() + " " + std::to_string(bitrateKbps) + " kbps, level " +
              levelText(m_Setup.sequence.levelIdc) + ", blocks " +
              std::to_string(1 << b.log2MinCodingBlock) + ".." +
              std::to_string(1 << b.log2MaxCodingBlock) + " depth " + std::to_string(b.depth) +
              (m_Setup.blocksAnswer.ampRequired ? ", AMP" : "") +
              (m_Setup.blocksAnswer.pAsLowDelayB ? ", P as low-delay B" : "") + ", " +
              std::to_string(m_Dpb.capacity()) + " pictures kept (reach " +
              std::to_string(m_Dpb.reachFrames()) + " frames), " +
              (m_Setup.intraRefreshFrames > 0
                   ? "intra refresh over " + std::to_string(m_Setup.intraRefreshFrames) + " frames"
                   : std::string("keyframes on demand")) +
              ", " + m_Queue.description + (m_Setup.reason.empty() ? "" : " — " + m_Setup.reason));
    return true;
}

bool VideoEncode12::createResources(std::string& error)
{
    ID3D12Device* d = m_Device->device();
    D3D12_VIDEO_ENCODER_PROFILE_DESC profile = {};
    profile.DataSize = sizeof(m_Profile);
    profile.pHEVCProfile = &m_Profile;
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION config = {};
    config.DataSize = sizeof(m_Config);
    config.pHEVCConfig = &m_Config;
    D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC res = {m_Setup.codedWidth, m_Setup.codedHeight};

    D3D12_VIDEO_ENCODER_DESC ed = {};
    ed.EncodeCodec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
    ed.EncodeProfile = profile;
    ed.InputFormat = m_Format;
    ed.CodecConfiguration = config;
    ed.MaxMotionEstimationPrecision = D3D12_VIDEO_ENCODER_MOTION_ESTIMATION_PRECISION_MODE_MAXIMUM;
    HRESULT h = m_Video->CreateVideoEncoder(&ed, IID_PPV_ARGS(&m_Encoder));
    if (FAILED(h)) {
        error = "the driver refuses the encoder it said it takes (" + d3d12::hresultText(h) + ")";
        return false;
    }
    D3D12_VIDEO_ENCODER_LEVEL_SETTING level = {};
    level.DataSize = sizeof(m_Level);
    level.pHEVCLevelSetting = &m_Level;
    D3D12_VIDEO_ENCODER_HEAP_DESC hd = {};
    hd.EncodeCodec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
    hd.EncodeProfile = profile;
    hd.EncodeLevel = level;
    hd.ResolutionsListCount = 1;
    hd.pResolutionList = &res;
    h = m_Video->CreateVideoEncoderHeap(&hd, IID_PPV_ARGS(&m_Heap));
    if (FAILED(h)) {
        error = "the encoder heap was refused (" + d3d12::hresultText(h) + ")";
        return false;
    }

    if (!m_Device->createQueue(m_QueueRequest, m_Queue, error)) return false;
    if (FAILED(h = d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE,
                                             IID_PPV_ARGS(&m_Allocator))) ||
        FAILED(h = d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE, m_Allocator.Get(),
                                        nullptr, IID_PPV_ARGS(&m_List))) ||
        FAILED(h = m_List->Close())) {
        error = "the video encode command list was refused (" + d3d12::hresultText(h) + ")";
        return false;
    }
    if (!m_Encoded.create(d, false, error)) return false;

    D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOURCE_REQUIREMENTS req = {};
    req.Codec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
    req.Profile = profile;
    req.InputFormat = m_Format;
    req.PictureTargetResolution = res;
    if (FAILED(m_Video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_RESOURCE_REQUIREMENTS, &req,
                                            sizeof(req))) ||
        req.IsSupported == 0) {
        error = "no resource requirements for " + std::to_string(res.Width) + "x" +
                std::to_string(res.Height);
        return false;
    }

    // The reconstructed pictures: HevcDpb's pool, one array where the driver
    // wants one (the AMD iGPU), the encoder's alone unless it lets others read.
    m_ReconCount = m_Dpb.textures();
    const D3D12_RESOURCE_FLAGS reconFlags = m_Setup.support.reconReadable
                                                ? D3D12_RESOURCE_FLAG_NONE
                                                : D3D12_RESOURCE_FLAG_VIDEO_ENCODE_REFERENCE_ONLY |
                                                      D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    m_Recon.clear();
    if (m_Setup.support.reconTextureArray) {
        m_Recon.push_back(texture(d, res.Width, res.Height, static_cast<UINT16>(m_ReconCount),
                                  m_Format, reconFlags));
    } else {
        for (int i = 0; i < m_ReconCount; ++i)
            m_Recon.push_back(texture(d, res.Width, res.Height, 1, m_Format, reconFlags));
    }
    for (const auto& r : m_Recon)
        if (!r) {
            error = "the reconstructed pictures were refused";
            return false;
        }

    // The bitstream: room for the parameter sets, then the worst picture (an
    // IDR at a high bitrate on a busy desktop), the slices at an offset the
    // driver accepts. The parameter sets are written now, once, right in
    // front of that offset.
    const UINT64 alignment = std::max<UINT64>(1, req.CompressedBitstreamBufferAccessAlignment);
    m_SliceOffset = (m_Headers.size() + alignment - 1) / alignment * alignment;
    // A raw 4:2:0 picture: 1.5 bytes a pixel, twice that in P010.
    const UINT64 raw = static_cast<UINT64>(res.Width) * res.Height * 3 / 2;
    const UINT64 worst = raw * (m_Format == DXGI_FORMAT_P010 ? 2 : 1) + 65536;
    m_BitstreamSize = (m_SliceOffset + worst + 65535) / 65536 * 65536;
    m_Bitstream = systemBuffer(d, m_BitstreamSize);
    const UINT64 metadataSize = std::max<UINT64>(4096, req.MaxEncoderOutputMetadataBufferSize);
    m_HwMetadata = videoBuffer(d, metadataSize);
    m_Metadata = systemBuffer(d, 4096);
    if (!m_Bitstream || !m_HwMetadata || !m_Metadata ||
        FAILED(m_Bitstream->Map(0, nullptr, reinterpret_cast<void**>(&m_BitstreamCpu))) ||
        FAILED(m_Metadata->Map(0, nullptr, reinterpret_cast<void**>(&m_MetadataCpu)))) {
        error = "no system-memory buffers for the bitstream";
        return false;
    }
    std::memcpy(m_BitstreamCpu + m_SliceOffset - m_Headers.size(), m_Headers.data(),
                m_Headers.size());
    return true;
}

ID3D12Resource* VideoEncode12::reconResource(int texture) const
{
    return m_Setup.support.reconTextureArray ? m_Recon[0].Get()
                                             : m_Recon[static_cast<size_t>(texture)].Get();
}

UINT VideoEncode12::reconSubresource(int texture) const
{
    return m_Setup.support.reconTextureArray ? static_cast<UINT>(texture) : 0u;
}

D3D12_RESOURCE_BARRIER VideoEncode12::reconBarrier(int texture, UINT plane,
                                                   D3D12_RESOURCE_STATES before,
                                                   D3D12_RESOURCE_STATES after) const
{
    if (!m_Setup.support.reconTextureArray)
        return transition(reconResource(texture), before, after);
    // Slice `texture`, plane `plane` of the array (one mip level).
    return transition(m_Recon[0].Get(), before, after,
                      static_cast<UINT>(texture) + plane * static_cast<UINT>(m_ReconCount));
}

bool VideoEncode12::submit(ID3D12Resource* picture, ID3D12Fence* ready, uint64_t readyValue,
                           const HevcDpb::Plan& plan, std::string& error)
{
    HRESULT h = m_Allocator->Reset();
    if (SUCCEEDED(h)) h = m_List->Reset(m_Allocator.Get());
    if (FAILED(h)) {
        error = "the encode list could not be reset (" + d3d12::hresultText(h) + ")";
        return false;
    }

    std::vector<D3D12_RESOURCE_BARRIER> pre = {
        transition(picture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ),
        transition(m_Bitstream.Get(), D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE),
        transition(m_HwMetadata.Get(), D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE),
    };
    const UINT planes = m_Setup.support.reconTextureArray ? 2u : 1u;
    for (UINT p = 0; p < planes; ++p)
        pre.push_back(reconBarrier(plan.texture, p, D3D12_RESOURCE_STATE_COMMON,
                                   D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE));
    std::vector<ID3D12Resource*> references;
    std::vector<UINT> subresources;
    std::vector<D3D12_VIDEO_ENCODER_REFERENCE_PICTURE_DESCRIPTOR_HEVC> descriptors;
    for (size_t k = 0; k < plan.references.size(); ++k) {
        const HevcDpb::Reference& r = plan.references[k];
        for (UINT p = 0; p < planes; ++p)
            pre.push_back(reconBarrier(r.texture, p, D3D12_RESOURCE_STATE_COMMON,
                                       D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ));
        references.push_back(reconResource(r.texture));
        subresources.push_back(reconSubresource(r.texture));
        D3D12_VIDEO_ENCODER_REFERENCE_PICTURE_DESCRIPTOR_HEVC ref = {};
        ref.ReconstructedPictureResourceIndex = static_cast<UINT>(k);
        ref.IsRefUsedByCurrentPic = r.used;
        ref.IsLongTermReference = FALSE;
        ref.PictureOrderCountNumber = r.poc;
        descriptors.push_back(ref);
    }
    m_List->ResourceBarrier(static_cast<UINT>(pre.size()), pre.data());

    // The used reference is listed first: L0 = {0}.
    UINT list0[1] = {0};
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC pic = {};
    pic.FrameType = plan.idr ? D3D12_VIDEO_ENCODER_FRAME_TYPE_HEVC_IDR_FRAME
                             : D3D12_VIDEO_ENCODER_FRAME_TYPE_HEVC_P_FRAME;
    pic.PictureOrderCountNumber = plan.poc;
    if (!plan.idr) {
        pic.List0ReferenceFramesCount = 1;
        pic.pList0ReferenceFrames = list0;
        pic.ReferenceFramesReconPictureDescriptorsCount = static_cast<UINT>(descriptors.size());
        pic.pReferenceFramesReconPictureDescriptors = descriptors.data();
    }

    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE gop = {};
    gop.DataSize = sizeof(m_Gop);
    gop.pHEVCGroupOfPictures = &m_Gop;
    D3D12_VIDEO_ENCODER_ENCODEFRAME_INPUT_ARGUMENTS in = {};
    in.SequenceControlDesc.Flags =
        m_RateChanged ? D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_FLAG_RATE_CONTROL_CHANGE
                      : D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_FLAG_NONE;
    const UINT refresh = static_cast<UINT>(m_Setup.intraRefreshFrames);
    in.SequenceControlDesc.IntraRefreshConfig = {
        refresh > 0 ? D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_ROW_BASED
                    : D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_NONE,
        refresh};
    in.SequenceControlDesc.RateControl = m_Rate->desc;
    in.SequenceControlDesc.PictureTargetResolution = {m_Setup.codedWidth, m_Setup.codedHeight};
    in.SequenceControlDesc.SelectedLayoutMode =
        D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME;
    in.SequenceControlDesc.CodecGopSequence = gop;
    in.PictureControlDesc.IntraRefreshFrameIndex = refresh > 0 ? m_IntraRefreshIndex % refresh : 0;
    in.PictureControlDesc.Flags =
        D3D12_VIDEO_ENCODER_PICTURE_CONTROL_FLAG_USED_AS_REFERENCE_PICTURE;
    in.PictureControlDesc.PictureControlCodecData.DataSize = sizeof(pic);
    in.PictureControlDesc.PictureControlCodecData.pHEVCPicData = &pic;
    if (!plan.idr) {
        in.PictureControlDesc.ReferenceFrames.NumTexture2Ds = static_cast<UINT>(references.size());
        in.PictureControlDesc.ReferenceFrames.ppTexture2Ds = references.data();
        in.PictureControlDesc.ReferenceFrames.pSubresources =
            m_Setup.support.reconTextureArray ? subresources.data() : nullptr;
    }
    in.pInputFrame = picture;
    in.InputFrameSubresource = 0;
    D3D12_VIDEO_ENCODER_ENCODEFRAME_OUTPUT_ARGUMENTS out = {};
    out.Bitstream = {m_Bitstream.Get(), m_SliceOffset};
    out.ReconstructedPicture = {reconResource(plan.texture), reconSubresource(plan.texture)};
    out.EncoderOutputMetadata = {m_HwMetadata.Get(), 0};
    m_List->EncodeFrame(m_Encoder.Get(), m_Heap.Get(), &in, &out);

    D3D12_RESOURCE_BARRIER mid[2] = {
        transition(m_HwMetadata.Get(), D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE,
                   D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ),
        transition(m_Metadata.Get(), D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE),
    };
    m_List->ResourceBarrier(2, mid);
    D3D12_VIDEO_ENCODER_PROFILE_DESC profile = {};
    profile.DataSize = sizeof(m_Profile);
    profile.pHEVCProfile = &m_Profile;
    D3D12_VIDEO_ENCODER_RESOLVE_METADATA_INPUT_ARGUMENTS rin = {};
    rin.EncoderCodec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
    rin.EncoderProfile = profile;
    rin.EncoderInputFormat = m_Format;
    rin.EncodedPictureEffectiveResolution = {m_Setup.codedWidth, m_Setup.codedHeight};
    rin.HWLayoutMetadata = {m_HwMetadata.Get(), 0};
    D3D12_VIDEO_ENCODER_RESOLVE_METADATA_OUTPUT_ARGUMENTS rout = {};
    rout.ResolvedLayoutMetadata = {m_Metadata.Get(), 0};
    m_List->ResolveEncoderOutputMetadata(&rin, &rout);

    // Everything back to COMMON: the conversion's queue and the next picture
    // find them as they expect.
    std::vector<D3D12_RESOURCE_BARRIER> post;
    for (D3D12_RESOURCE_BARRIER b : pre) {
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        if (b.Transition.pResource == m_HwMetadata.Get())
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ;
        post.push_back(b);
    }
    post.push_back(transition(m_Metadata.Get(), D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE,
                              D3D12_RESOURCE_STATE_COMMON));
    m_List->ResourceBarrier(static_cast<UINT>(post.size()), post.data());
    if (FAILED(h = m_List->Close())) {
        error = "the encode list was refused (" + d3d12::hresultText(h) + ")";
        m_Device->relayMessages();
        return false;
    }

    ID3D12CommandQueue* queue = m_Queue.queue.Get();
    if (ready && FAILED(h = queue->Wait(ready, readyValue))) {
        error = "the encode queue cannot wait for the picture (" + d3d12::hresultText(h) + ")";
        return false;
    }
    ID3D12CommandList* lists[] = {m_List.Get()};
    queue->ExecuteCommandLists(1, lists);
    const uint64_t value = m_Encoded.signal(queue, error);
    if (value == 0) return false;
    const d3d12::GpuFence::Wait waited = m_Encoded.wait(value, kWaitMs, error);
    if (waited != d3d12::GpuFence::Wait::Done) {
        if (waited == d3d12::GpuFence::Wait::TimedOut)
            error = "the picture took over " + std::to_string(kWaitMs) + " ms to encode";
        return false;
    }
    return true;
}

bool VideoEncode12::encode(ID3D12Resource* picture, ID3D12Fence* ready, uint64_t readyValue,
                           bool forceKeyframe, uint32_t frameNumber, EncoderOutput& out,
                           std::string& error)
{
    if (!m_Encoder) {
        error = "the encoder is not initialized";
        return false;
    }
    if (m_OutputHeld) {
        error = "the previous frame was not released";
        return false;
    }
    if (!picture) {
        error = "no picture";
        return false;
    }
    const HevcDpb::Plan plan = m_Dpb.plan(frameNumber, forceKeyframe);
    if (plan.idr) m_IntraRefreshIndex = 0;
    uint64_t bytes = 0;
    if (!submit(picture, ready, readyValue, plan, error) || !written(bytes, error)) return false;
    const auto* metadata =
        reinterpret_cast<const D3D12_VIDEO_ENCODER_OUTPUT_METADATA*>(m_MetadataCpu);
    const uint8_t* slices = m_BitstreamCpu + m_SliceOffset;
    if (m_GuardLeft > 0) {
        if (!guard(slices, static_cast<size_t>(bytes), plan, error)) return false;
        --m_GuardLeft;
    }
    m_Dpb.encoded(plan);
    m_RateChanged = false;
    ++m_IntraRefreshIndex;

    out.data = plan.idr ? slices - m_Headers.size() : slices;
    out.size = static_cast<size_t>(bytes) + (plan.idr ? m_Headers.size() : 0);
    out.keyframe = plan.idr;
    out.avgQp = static_cast<int>(metadata->EncodeStats.AverageQP);
    m_OutputHeld = true;
    return true;
}

bool VideoEncode12::written(uint64_t& bytes, std::string& error) const
{
    const auto* metadata =
        reinterpret_cast<const D3D12_VIDEO_ENCODER_OUTPUT_METADATA*>(m_MetadataCpu);
    bytes = metadata->EncodedBitstreamWrittenBytesCount;
    if (metadata->EncodeErrorFlags != 0) {
        error = "the driver flagged the picture (" + hex(metadata->EncodeErrorFlags) + ")";
        return false;
    }
    if (bytes == 0 || bytes > m_BitstreamSize - m_SliceOffset) {
        error = "the driver says it wrote " + std::to_string(bytes) + " bytes";
        return false;
    }
    return true;
}

bool VideoEncode12::warmUp(std::string& error)
{
    // Committed resources come zeroed: a flat picture is all the driver needs
    // to set itself up.
    ComPtr<ID3D12Resource> blank =
        texture(m_Device->device(), m_Setup.codedWidth, m_Setup.codedHeight, 1, m_Format,
                D3D12_RESOURCE_FLAG_NONE);
    if (!blank) {
        error = "no picture to warm the encoder up with";
        return false;
    }
    // Not recorded in the DPB: the first real picture is an IDR all the same.
    const HevcDpb::Plan plan = m_Dpb.plan(0, true);
    uint64_t bytes = 0;
    return submit(blank.Get(), nullptr, 0, plan, error) && written(bytes, error) &&
           guard(m_BitstreamCpu + m_SliceOffset, static_cast<size_t>(bytes), plan, error);
}

bool VideoEncode12::guard(const uint8_t* data, size_t size, const HevcDpb::Plan& plan,
                          std::string& error)
{
    int slices = 0;
    for (const HevcNalUnit& unit : hevcNalUnits(data, size)) {
        if (unit.type() > 31) continue; // not a slice: an AUD or an SEI the driver adds
        HevcSliceFields f;
        const std::string wrong =
            parseHevcSliceHeader(unit.data, unit.size, m_SpsFields, m_PpsFields, f);
        if (!wrong.empty()) {
            error = "the driver's slices do not read with our parameter sets: " + wrong;
            return false;
        }
        // What we asked for, said back: the picture's place, and the one it
        // predicts from.
        const uint32_t pocMask = (1u << m_SpsFields.log2MaxPocLsb) - 1u;
        if (!plan.idr && f.pocLsb != (plan.poc & pocMask)) {
            error = "the driver numbered picture " + std::to_string(plan.poc) + " as " +
                    std::to_string(f.pocLsb);
            return false;
        }
        if (!plan.idr && !plan.references.empty()) {
            const int32_t wanted =
                static_cast<int32_t>(plan.references[0].poc) - static_cast<int32_t>(plan.poc);
            const bool found = std::any_of(f.shortTerm.begin(), f.shortTerm.end(),
                                           [&](const HevcSliceFields::Reference& r) {
                                               return r.used && r.deltaPoc == wanted;
                                           });
            if (!found) {
                error = "the driver's slice does not predict from the picture asked (POC delta " +
                        std::to_string(wanted) + ")";
                return false;
            }
        }
        ++slices;
    }
    if (slices == 0) {
        error = "no slice in the driver's output";
        return false;
    }
    return true;
}

bool VideoEncode12::invalidateReference(uint32_t frameNumber, std::string& error)
{
    if (!m_Encoder) {
        error = "the encoder is not initialized";
        return false;
    }
    if (!m_Dpb.invalidate(frameNumber)) {
        error = "no picture from before frame " + std::to_string(frameNumber) +
                " is kept: a keyframe is the only repair";
        return false;
    }
    return true;
}

bool VideoEncode12::setBitrate(int bitrateKbps, std::string& error)
{
    if (!m_Encoder || !m_Rate) {
        error = "the encoder is not initialized";
        return false;
    }
    if (!m_Setup.support.rateReconfigurable) {
        error = "this driver changes its bitrate only with a new sequence (our own rate control "
                "is still to come)";
        return false;
    }
    m_Rate->setBitrate(static_cast<uint32_t>((std::max)(bitrateKbps, 1)) * 1000u, m_Fps,
                       m_VbvFrames);
    m_RateChanged = true;
    return true;
}

void VideoEncode12::stop()
{
    if (m_Queue.queue && m_Encoded.fence()) {
        std::string ignored;
        const uint64_t value = m_Encoded.signal(m_Queue.queue.Get(), ignored);
        if (value) m_Encoded.wait(value, kWaitMs, ignored);
    }
    if (m_Bitstream && m_BitstreamCpu) m_Bitstream->Unmap(0, nullptr);
    if (m_Metadata && m_MetadataCpu) m_Metadata->Unmap(0, nullptr);
    m_BitstreamCpu = nullptr;
    m_MetadataCpu = nullptr;
    m_Bitstream.Reset();
    m_Metadata.Reset();
    m_HwMetadata.Reset();
    m_Recon.clear();
    m_ReconCount = 0;
    m_List.Reset();
    m_Allocator.Reset();
    m_Encoded.reset();
    m_Queue = d3d12::Queue();
    m_Heap.Reset();
    m_Encoder.Reset();
    m_Video.Reset();
    m_Device.reset();
    m_Rate.reset();
    m_Dpb.reset();
    m_Headers.clear();
    m_Setup = HevcEncodeSetup();
    m_GuardLeft = 0;
    m_RateChanged = false;
    m_OutputHeld = false;
}

} // namespace mw::native::encode
