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

#include "HevcHeaders.h"

namespace lab {
namespace {

struct BitWriter
{
    std::vector<uint8_t> bytes;
    int used = 0; // bits used in the last byte, 0 = none started

    void u(int n, uint64_t v)
    {
        for (int i = n - 1; i >= 0; --i) {
            if (used == 0) bytes.push_back(0);
            if ((v >> i) & 1u) bytes.back() |= static_cast<uint8_t>(0x80u >> used);
            used = (used + 1) & 7;
        }
    }
    void ue(uint32_t v)
    {
        const uint64_t x = static_cast<uint64_t>(v) + 1;
        int length = 0;
        while ((x >> length) > 1)
            ++length;
        u(length, 0);
        u(length + 1, x);
    }
    void se(int32_t v)
    {
        ue(v > 0 ? static_cast<uint32_t>(2 * v - 1) : static_cast<uint32_t>(-2 * v));
    }
};

// Emulation prevention: no 00 00 0x (x <= 3) in the payload.
void escapeInto(const std::vector<uint8_t>& rbsp, std::vector<uint8_t>& out)
{
    int zeros = 0;
    for (uint8_t b : rbsp) {
        if (zeros >= 2 && b <= 3) {
            out.push_back(3);
            zeros = 0;
        }
        out.push_back(b);
        zeros = b == 0 ? zeros + 1 : 0;
    }
}

std::vector<uint8_t> nal(uint8_t type, BitWriter& w)
{
    w.u(1, 1); // rbsp_stop_one_bit, then the byte's zeros
    std::vector<uint8_t> out = {static_cast<uint8_t>(type << 1), 1};
    escapeInto(w.bytes, out);
    return out;
}

// profile_tier_level( 1, 0 ), Main, no sub-layers.
void profileTierLevel(BitWriter& w, const HevcShape& s)
{
    w.u(2, 0); // general_profile_space
    w.u(1, 0); // general_tier_flag
    w.u(5, 1); // Main
    for (uint32_t j = 0; j < 32; ++j)
        w.u(1, (j == 1 || j == 2) ? 1u : 0u);
    w.u(1, 1); // progressive
    w.u(1, 0); // interlaced
    w.u(1, 1); // non-packed
    w.u(1, 1); // frame only
    w.u(32, 0);
    w.u(12, 0);
    w.u(8, static_cast<uint32_t>(s.levelIdc));
}

void orderingInfo(BitWriter& w, const HevcShape& s)
{
    w.ue(static_cast<uint32_t>(s.decodedPictureBuffer - 1));
    w.ue(0); // no reordering
    w.ue(0);
}

std::vector<uint8_t> vps(const HevcShape& s)
{
    BitWriter w;
    w.u(4, 0);
    w.u(1, 1);
    w.u(1, 1);
    w.u(6, 0);
    w.u(3, 0);
    w.u(1, 1);
    w.u(16, 0xFFFF);
    profileTierLevel(w, s);
    w.u(1, 0);
    orderingInfo(w, s);
    w.u(6, 0);
    w.ue(0);
    w.u(1, 0);
    w.u(1, 0);
    return nal(32, w);
}

std::vector<uint8_t> sps(const HevcShape& s)
{
    BitWriter w;
    w.u(4, 0);
    w.u(3, 0);
    w.u(1, 1);
    profileTierLevel(w, s);
    w.ue(0); // sps id
    w.ue(1); // 4:2:0
    w.ue(static_cast<uint32_t>(s.codedWidth));
    w.ue(static_cast<uint32_t>(s.codedHeight));
    const int cropRight = (s.codedWidth - s.width) / 2;
    const int cropBottom = (s.codedHeight - s.height) / 2;
    const bool cropped = cropRight > 0 || cropBottom > 0;
    w.u(1, cropped ? 1u : 0u);
    if (cropped) {
        w.ue(0);
        w.ue(static_cast<uint32_t>(cropRight));
        w.ue(0);
        w.ue(static_cast<uint32_t>(cropBottom));
    }
    w.ue(0); // bit depth luma - 8
    w.ue(0); // chroma
    w.ue(static_cast<uint32_t>(s.log2MaxPicOrderCntLsb - 4));
    w.u(1, 0);
    orderingInfo(w, s);
    w.ue(static_cast<uint32_t>(s.log2MinCodingBlock - 3));
    w.ue(static_cast<uint32_t>(s.log2MaxCodingBlock - s.log2MinCodingBlock));
    w.ue(static_cast<uint32_t>(s.log2MinTransformBlock - 2));
    w.ue(static_cast<uint32_t>(s.log2MaxTransformBlock - s.log2MinTransformBlock));
    w.ue(static_cast<uint32_t>(s.transformDepthInter));
    w.ue(static_cast<uint32_t>(s.transformDepthIntra));
    w.u(1, 0); // scaling lists
    w.u(1, s.asymmetricMotionPartitions ? 1u : 0u);
    w.u(1, s.sampleAdaptiveOffset ? 1u : 0u);
    w.u(1, 0); // pcm
    w.ue(0);   // short-term RPS in the slice headers
    w.u(1, 0); // long-term refs
    w.u(1, 0); // temporal MVP
    w.u(1, 0); // strong intra smoothing
    w.u(1, 1); // VUI
    w.u(1, 0);
    w.u(1, 0);
    w.u(1, 1); // video signal type
    w.u(3, 5);
    w.u(1, 0); // limited range
    w.u(1, 1);
    w.u(8, 1); // BT.709
    w.u(8, 1);
    w.u(8, 1);
    w.u(1, 0);
    w.u(1, 0);
    w.u(1, 0);
    w.u(1, 0);
    w.u(1, 0);
    w.u(1, 0);
    w.u(1, 0);
    w.u(1, 0); // sps extension
    return nal(33, w);
}

std::vector<uint8_t> pps(const HevcShape& s)
{
    BitWriter w;
    w.ue(0);
    w.ue(0);
    w.u(1, 0); // dependent slices
    w.u(1, 0); // output flag
    w.u(3, 0); // extra slice header bits
    w.u(1, 0); // sign data hiding
    w.u(1, 1); // cabac_init_present
    w.ue(static_cast<uint32_t>(s.defaultActiveReferences - 1));
    w.ue(0);
    w.se(0);   // init_qp_minus26
    w.u(1, 0); // constrained intra
    w.u(1, 0); // transform skip
    w.u(1, 1); // cu_qp_delta_enabled
    w.ue(0);
    w.se(0);
    w.se(0);
    w.u(1, 1); // slice chroma QP offsets present
    w.u(1, 0);
    w.u(1, 0);
    w.u(1, 0);
    w.u(1, 0); // tiles
    w.u(1, 0); // entropy sync
    w.u(1, 1); // loop filter across slices
    w.u(1, 1); // deblocking control present
    w.u(1, 0);
    w.u(1, 0);
    w.se(0);
    w.se(0);
    w.u(1, 0); // scaling list data
    w.u(1, 0); // lists modification
    w.ue(0);
    w.u(1, 0);
    w.u(1, 0);
    return nal(34, w);
}

} // namespace

std::vector<uint8_t> hevcParameterSets(const HevcShape& shape)
{
    std::vector<uint8_t> out;
    for (const std::vector<uint8_t>& unit : {vps(shape), sps(shape), pps(shape)}) {
        out.insert(out.end(), {0, 0, 0, 1});
        out.insert(out.end(), unit.begin(), unit.end());
    }
    return out;
}

} // namespace lab
