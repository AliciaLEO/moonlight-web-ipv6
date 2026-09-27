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

#include "encode/HevcEncodeNegotiation.h"

#include <d3d12video.h>
#include <wrl/client.h>

namespace mw::native::encode {

/// The questions HevcEncodeNegotiation asks, put to a D3D12 video device.
///
/// Every answer D3D12 types as a BOOL is read as "not zero": the Intel driver
/// says 128 where the others say 1 (26/09/2026). The structures an answer was
/// given for are built here too, by the same functions the encoder uses to
/// create itself — what was asked is what gets created.
class VideoEncodeCaps12 : public HevcDriverQueries
{
public:
    explicit VideoEncodeCaps12(ID3D12VideoDevice3* video);

    HevcDriverLimits limits(bool tenBit) override;
    HevcBlocksAnswer blocks(const HevcBlocks& blocks, bool tenBit) override;
    HevcSupportAnswer support(const HevcSupportQuestion& question) override;

    /// The level the driver suggested in its last "yes" to support().
    D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC suggestedLevel() const { return m_Level; }

    static D3D12_VIDEO_ENCODER_PROFILE_HEVC profile(bool tenBit);
    static D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC
    configuration(const HevcBlocks& blocks, const HevcBlocksAnswer& flags);
    /// 30 × the level: D3D12's enumeration in the SPS's terms.
    static int levelIdc(D3D12_VIDEO_ENCODER_LEVELS_HEVC level);

    /// The rate control of @p rate at @p fps, at a nominal bitrate (the answer
    /// does not depend on it): the structures D3D12 points to, kept together.
    struct RateControl
    {
        D3D12_VIDEO_ENCODER_RATE_CONTROL desc = {};
        D3D12_VIDEO_ENCODER_RATE_CONTROL_CBR cbr = {};
        D3D12_VIDEO_ENCODER_RATE_CONTROL_CBR1 cbr1 = {};
        D3D12_VIDEO_ENCODER_RATE_CONTROL_CQP cqp = {};
        D3D12_VIDEO_ENCODER_RATE_CONTROL_CQP1 cqp1 = {};

        RateControl(const HevcRate& rate, int fps, uint32_t bitsPerSecond);
        RateControl(const RateControl&) = delete;
        RateControl& operator=(const RateControl&) = delete;

        /// A new CBR target; the VBV by the rule every encoder follows
        /// (RateControl.h), or @p vbvFrames frames' worth when the bench asks.
        void setBitrate(uint32_t bitsPerSecond, int fps, int vbvFrames);
        /// A new constant QP, for every kind of picture: which one a driver
        /// reads for its P pictures is its own business (the Arc codes them
        /// as low-delay B).
        void setQp(UINT qp);
    };

private:
    Microsoft::WRL::ComPtr<ID3D12VideoDevice3> m_Video;
    D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC m_Level = {};
};

} // namespace mw::native::encode
