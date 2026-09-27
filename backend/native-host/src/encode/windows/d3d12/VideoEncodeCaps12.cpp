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

#include "encode/windows/d3d12/VideoEncodeCaps12.h"

#include <vector>

namespace mw::native::encode {

namespace {

template <typename T> bool feature(ID3D12VideoDevice3* video, D3D12_FEATURE_VIDEO f, T& data)
{
    return SUCCEEDED(video->CheckFeatureSupport(f, &data, sizeof(data)));
}

D3D12_VIDEO_ENCODER_PROFILE_DESC profileDesc(D3D12_VIDEO_ENCODER_PROFILE_HEVC& profile)
{
    D3D12_VIDEO_ENCODER_PROFILE_DESC d = {};
    d.DataSize = sizeof(profile);
    d.pHEVCProfile = &profile;
    return d;
}

} // namespace

VideoEncodeCaps12::VideoEncodeCaps12(ID3D12VideoDevice3* video)
    : m_Video(video)
{}

D3D12_VIDEO_ENCODER_PROFILE_HEVC VideoEncodeCaps12::profile(bool tenBit)
{
    return tenBit ? D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN10 : D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN;
}

D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC
VideoEncodeCaps12::configuration(const HevcBlocks& blocks, const HevcBlocksAnswer& flags)
{
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC c = {};
    // AMP where the driver requires it, nothing else: the configurations
    // whose slices ffmpeg decoded without an error (ParameterSets.h).
    if (flags.ampRequired)
        c.ConfigurationFlags |=
            D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_USE_ASYMETRIC_MOTION_PARTITION;
    c.MinLumaCodingUnitSize = static_cast<D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE>(
        blocks.log2MinCodingBlock - 3);
    c.MaxLumaCodingUnitSize = static_cast<D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE>(
        blocks.log2MaxCodingBlock - 3);
    c.MinLumaTransformUnitSize = static_cast<D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE>(
        blocks.log2MinTransformBlock - 2);
    c.MaxLumaTransformUnitSize = static_cast<D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE>(
        blocks.log2MaxTransformBlock - 2);
    c.max_transform_hierarchy_depth_inter = static_cast<UCHAR>(blocks.depth);
    c.max_transform_hierarchy_depth_intra = static_cast<UCHAR>(blocks.depth);
    return c;
}

int VideoEncodeCaps12::levelIdc(D3D12_VIDEO_ENCODER_LEVELS_HEVC level)
{
    static const int kIdc[] = {30, 60, 63, 90, 93, 120, 123, 150, 153, 156, 180, 183, 186};
    const int i = static_cast<int>(level);
    return i >= 0 && i < 13 ? kIdc[i] : 0;
}

VideoEncodeCaps12::RateControl::RateControl(const HevcRate& rate, int fps, uint32_t bitsPerSecond)
{
    desc.TargetFrameRate = {static_cast<UINT>(fps > 0 ? fps : 60), 1};
    if (rate.mode == HevcRate::Mode::Cbr) {
        desc.Mode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CBR;
        // The engine's QP floor: below 18 a still picture spends bits on
        // noise the eye cannot see.
        cbr.InitialQP = 30;
        cbr.MinQP = 18;
        cbr.MaxQP = 51;
        cbr1.InitialQP = cbr.InitialQP;
        cbr1.MinQP = cbr.MinQP;
        cbr1.MaxQP = cbr.MaxQP;
        cbr1.QualityVsSpeed = 0; // the fastest end (measured 26/09/2026)
        setBitrate(bitsPerSecond, fps, 0);
        if (rate.qualityVsSpeed) {
            desc.ConfigParams.DataSize = sizeof(cbr1);
            desc.ConfigParams.pConfiguration_CBR1 = &cbr1;
        } else {
            desc.ConfigParams.DataSize = sizeof(cbr);
            desc.ConfigParams.pConfiguration_CBR = &cbr;
        }
    } else {
        desc.Mode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CQP;
        cqp = {30, 30, 30};
        cqp1.ConstantQP_FullIntracodedFrame = 30;
        cqp1.ConstantQP_InterPredictedFrame_PrevRefOnly = 30;
        cqp1.ConstantQP_InterPredictedFrame_BiDirectionalRef = 30;
        cqp1.QualityVsSpeed = 0;
        if (rate.qualityVsSpeed) {
            desc.ConfigParams.DataSize = sizeof(cqp1);
            desc.ConfigParams.pConfiguration_CQP1 = &cqp1;
        } else {
            desc.ConfigParams.DataSize = sizeof(cqp);
            desc.ConfigParams.pConfiguration_CQP = &cqp;
        }
    }
    if (rate.qualityVsSpeed)
        desc.Flags |= D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_EXTENSION1_SUPPORT |
                      D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_QUALITY_VS_SPEED;
    if (rate.vbv) desc.Flags |= D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_VBV_SIZES;
    if (rate.qpRange) desc.Flags |= D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_QP_RANGE;
    if (rate.frameSizeCap)
        desc.Flags |= D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_MAX_FRAME_SIZE;
}

void VideoEncodeCaps12::RateControl::setBitrate(uint32_t bitsPerSecond, int fps, int vbvFrames)
{
    cbr.TargetBitRate = bitsPerSecond;
    cbr.VBVCapacity = vbvBits(bitsPerSecond, fps, vbvFrames);
    cbr.InitialVBVFullness = cbr.VBVCapacity;
    cbr.MaxFrameBitSize = cbr.VBVCapacity;
    cbr1.TargetBitRate = cbr.TargetBitRate;
    cbr1.VBVCapacity = cbr.VBVCapacity;
    cbr1.InitialVBVFullness = cbr.InitialVBVFullness;
    cbr1.MaxFrameBitSize = cbr.MaxFrameBitSize;
}

void VideoEncodeCaps12::RateControl::setQp(UINT qp)
{
    cqp = {qp, qp, qp};
    cqp1.ConstantQP_FullIntracodedFrame = qp;
    cqp1.ConstantQP_InterPredictedFrame_PrevRefOnly = qp;
    cqp1.ConstantQP_InterPredictedFrame_BiDirectionalRef = qp;
}

HevcDriverLimits VideoEncodeCaps12::limits(bool tenBit)
{
    HevcDriverLimits l;
    if (!m_Video) return l;
    D3D12_VIDEO_ENCODER_PROFILE_HEVC prof = profile(tenBit);

    D3D12_FEATURE_DATA_VIDEO_ENCODER_OUTPUT_RESOLUTION_RATIOS_COUNT count = {};
    count.Codec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
    feature(m_Video.Get(), D3D12_FEATURE_VIDEO_ENCODER_OUTPUT_RESOLUTION_RATIOS_COUNT, count);
    std::vector<D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_RATIO_DESC> ratios(
        count.ResolutionRatiosCount ? count.ResolutionRatiosCount : 1);
    D3D12_FEATURE_DATA_VIDEO_ENCODER_OUTPUT_RESOLUTION res = {};
    res.Codec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
    res.ResolutionRatiosCount = count.ResolutionRatiosCount;
    res.pResolutionRatios = count.ResolutionRatiosCount ? ratios.data() : nullptr;
    if (feature(m_Video.Get(), D3D12_FEATURE_VIDEO_ENCODER_OUTPUT_RESOLUTION, res) &&
        res.IsSupported != 0) {
        l.minWidth = res.MinResolutionSupported.Width;
        l.minHeight = res.MinResolutionSupported.Height;
        l.maxWidth = res.MaxResolutionSupported.Width;
        l.maxHeight = res.MaxResolutionSupported.Height;
    }

    D3D12_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT_HEVC pc = {};
    D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT pcs = {};
    pcs.Codec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
    pcs.Profile = profileDesc(prof);
    pcs.PictureSupport.DataSize = sizeof(pc);
    pcs.PictureSupport.pHEVCSupport = &pc;
    if (feature(m_Video.Get(), D3D12_FEATURE_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT, pcs) &&
        pcs.IsSupported != 0) {
        l.maxL0ForP = pc.MaxL0ReferencesForP;
        l.maxDpb = pc.MaxDPBCapacity;
    }

    D3D12_VIDEO_ENCODER_PROFILE_HEVC main10 = D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN10;
    D3D12_FEATURE_DATA_VIDEO_ENCODER_INPUT_FORMAT input = {};
    input.Codec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
    input.Profile = profileDesc(main10);
    input.Format = DXGI_FORMAT_P010;
    l.main10 = feature(m_Video.Get(), D3D12_FEATURE_VIDEO_ENCODER_INPUT_FORMAT, input) &&
               input.IsSupported != 0;
    return l;
}

HevcBlocksAnswer VideoEncodeCaps12::blocks(const HevcBlocks& blocks, bool tenBit)
{
    HevcBlocksAnswer a;
    if (!m_Video) return a;
    D3D12_VIDEO_ENCODER_PROFILE_HEVC prof = profile(tenBit);
    const D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC proposal = configuration(blocks, a);
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC caps = {};
    caps.MinLumaCodingUnitSize = proposal.MinLumaCodingUnitSize;
    caps.MaxLumaCodingUnitSize = proposal.MaxLumaCodingUnitSize;
    caps.MinLumaTransformUnitSize = proposal.MinLumaTransformUnitSize;
    caps.MaxLumaTransformUnitSize = proposal.MaxLumaTransformUnitSize;
    caps.max_transform_hierarchy_depth_inter = proposal.max_transform_hierarchy_depth_inter;
    caps.max_transform_hierarchy_depth_intra = proposal.max_transform_hierarchy_depth_intra;
    D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT support = {};
    support.Codec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
    support.Profile = profileDesc(prof);
    support.CodecSupportLimits.DataSize = sizeof(caps);
    support.CodecSupportLimits.pHEVCSupport = &caps;
    if (!feature(m_Video.Get(), D3D12_FEATURE_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT, support) ||
        support.IsSupported == 0)
        return a;
    a.taken = true;
    a.ampRequired =
        (caps.SupportFlags &
         D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG_ASYMETRIC_MOTION_PARTITION_REQUIRED) !=
        0;
    a.pAsLowDelayB =
        (caps.SupportFlags &
         D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG_P_FRAMES_IMPLEMENTED_AS_LOW_DELAY_B_FRAMES) !=
        0;
    return a;
}

HevcSupportAnswer VideoEncodeCaps12::support(const HevcSupportQuestion& q)
{
    HevcSupportAnswer a;
    if (!m_Video) return a;
    // No profile in the question: the input format says Main or Main 10, and
    // the driver suggests the profile back.
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC config = configuration(q.blocks, q.blockFlags);
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION codec = {};
    codec.DataSize = sizeof(config);
    codec.pHEVCConfig = &config;
    // The product's sequence: endless, P only, an 8-bit POC (ParameterSets.h).
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_HEVC gop = {0, 1, 4};
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE gopDesc = {};
    gopDesc.DataSize = sizeof(gop);
    gopDesc.pHEVCGroupOfPictures = &gop;
    const RateControl rate(q.rate, q.fps, 20000000);
    D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC res = {q.codedWidth, q.codedHeight};

    D3D12_VIDEO_ENCODER_PROFILE_HEVC suggestedProfile = {};
    D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC suggestedLevel = {};
    D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_LIMITS limits = {};
    D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT1 s = {};
    s.Codec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
    s.InputFormat = q.tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    s.CodecConfiguration = codec;
    s.CodecGopSequence = gopDesc;
    s.RateControl = rate.desc;
    s.IntraRefresh = q.intraRefresh ? D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_ROW_BASED
                                    : D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_NONE;
    s.SubregionFrameEncoding = D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME;
    s.ResolutionsListCount = 1;
    s.pResolutionList = &res;
    s.MaxReferenceFramesInDPB = q.dpb;
    s.SuggestedProfile.DataSize = sizeof(suggestedProfile);
    s.SuggestedProfile.pHEVCProfile = &suggestedProfile;
    s.SuggestedLevel.DataSize = sizeof(suggestedLevel);
    s.SuggestedLevel.pHEVCLevelSetting = &suggestedLevel;
    s.pResolutionDependentSupport = &limits;
    // SUPPORT1 knows QualityVsSpeed; a runtime or driver without it takes the
    // original structure, which is SUPPORT1 without its last members.
    HRESULT asked =
        m_Video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_SUPPORT1, &s, sizeof(s));
    if (FAILED(asked) && !q.rate.qualityVsSpeed)
        asked = m_Video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_SUPPORT, &s,
                                             sizeof(D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT));
    if (FAILED(asked) ||
        (s.SupportFlags & D3D12_VIDEO_ENCODER_SUPPORT_FLAG_GENERAL_SUPPORT_OK) == 0)
        return a;
    a.ok = true;
    a.rateReconfigurable =
        (s.SupportFlags &
         D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RATE_CONTROL_RECONFIGURATION_AVAILABLE) != 0;
    a.reconTextureArray =
        (s.SupportFlags &
         D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RECONSTRUCTED_FRAMES_REQUIRE_TEXTURE_ARRAYS) != 0;
    a.reconReadable =
        (s.SupportFlags &
         D3D12_VIDEO_ENCODER_SUPPORT_FLAG_READABLE_RECONSTRUCTED_PICTURE_LAYOUT_AVAILABLE) != 0;
    a.suggestedLevelIdc = levelIdc(suggestedLevel.Level);
    a.qpMapRegion = limits.QPMapRegionPixelsSize;
    a.maxIntraRefreshFrames = limits.MaxIntraRefreshFrameDuration;
    a.maxQualityVsSpeed = s.MaxQualityVsSpeed;
    m_Level = suggestedLevel;
    return a;
}

} // namespace mw::native::encode
