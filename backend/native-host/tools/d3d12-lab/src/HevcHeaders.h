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

#pragma once

// The VPS, SPS and PPS a D3D12 Video Encode stream needs, for the `encode`
// probe's dumps only. D3D12 writes slices; the parameter sets are the
// application's, and nothing checks that the two agree. This is the first
// attempt's writer (HevcParamSets, 21/09, checked with ffmpeg on the Arc's
// stream), kept in the lab: the product's own writer comes with Phase 4, as a
// "dialect" of ParameterSets.h.

#include <cstdint>
#include <vector>

namespace lab {

struct HevcShape
{
    int width = 0;
    int height = 0;
    int codedWidth = 0;
    int codedHeight = 0;
    int levelIdc = 153;
    int log2MinCodingBlock = 3;
    int log2MaxCodingBlock = 6;
    int log2MinTransformBlock = 2;
    int log2MaxTransformBlock = 5;
    int transformDepthInter = 2;
    int transformDepthIntra = 2;
    bool asymmetricMotionPartitions = false;
    bool sampleAdaptiveOffset = false;
    int log2MaxPicOrderCntLsb = 8;
    int decodedPictureBuffer = 2;
    int defaultActiveReferences = 1;
};

/// VPS, SPS and PPS as Annex-B, what goes ahead of every IDR.
std::vector<uint8_t> hevcParameterSets(const HevcShape& shape);

} // namespace lab
