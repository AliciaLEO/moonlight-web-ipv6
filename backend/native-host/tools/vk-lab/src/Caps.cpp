/*
 * MoonlightWeb — native capture & encoding engine: Vulkan lab.
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

// `caps` — what every Vulkan device of this machine answers, before anything
// of the Linux chain is built on it (plan pipeline-video-d3d12-v2, Phase 13,
// C13.1). The Windows counterpart is `mw-d3d12-lab caps` (C0.2); this one asks
// what a Vulkan chain depends on:
//
//  - the encoders: per codec and profile, the Vulkan Video capabilities —
//    coded sizes and their granularity, CTB and transform sizes (the coded
//    size is a whole number of CTBs, the lesson of C5.3), DPB slots and active
//    references, the rate control modes (DISABLED is the constant QP the
//    in-house controller drives), the QP range, the quality levels and what
//    each prefers, intra-refresh and QP maps when the driver has those
//    extensions, what the encode feedback query reports, and the formats and
//    usages an input picture may have — STORAGE or COLOR_ATTACHMENT on the
//    encoder's own input is what lets the conversion write straight into it;
//  - the import: which DRM format modifiers each capture format imports with,
//    in how many planes (AMD's displayable DCC is three), and what the KMS
//    planes scan out right now;
//  - the queues: which families there are, which priorities they announce,
//    and which a device actually gets at each — the kernel refuses HIGH and
//    REALTIME to a process without CAP_SYS_NICE (amdgpu), the way Windows
//    refuses GLOBAL_REALTIME to a limited token; whether each family can be
//    timestamped, and against which CPU clocks;
//  - the fences: sync_file import and export on a semaphore, the way the
//    kernel's implicit fences of a KMS or a portal buffer come in.
//
// The JSON written next to the text is what later probes, and the engine's
// negotiation tests, start from.

#include "Caps.h"

#include "Json.h"
#include "Lab.h"
#include "Vk.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace lab {

namespace {

struct Options
{
    std::string jsonPath;
    bool json = true;
    bool software = false;
    bool priority = true;
    std::string device;
};

// ── Names ───────────────────────────────────────────────────────────────────

const std::initializer_list<FlagName> kQueueFlags = {
    {VK_QUEUE_GRAPHICS_BIT, "graphics"},
    {VK_QUEUE_COMPUTE_BIT, "compute"},
    {VK_QUEUE_TRANSFER_BIT, "transfer"},
    {VK_QUEUE_SPARSE_BINDING_BIT, "sparse"},
    {VK_QUEUE_PROTECTED_BIT, "protected"},
    {VK_QUEUE_VIDEO_DECODE_BIT_KHR, "video-decode"},
    {VK_QUEUE_VIDEO_ENCODE_BIT_KHR, "video-encode"},
    {VK_QUEUE_OPTICAL_FLOW_BIT_NV, "optical-flow"},
};

const std::initializer_list<FlagName> kCodecOps = {
    {VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, "encode-h264"},
    {VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, "encode-h265"},
    {VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR, "encode-av1"},
    {VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR, "decode-h264"},
    {VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR, "decode-h265"},
    {VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR, "decode-av1"},
    {VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR, "decode-vp9"},
};

const std::initializer_list<FlagName> kVideoCapFlags = {
    {VK_VIDEO_CAPABILITY_PROTECTED_CONTENT_BIT_KHR, "protected"},
    {VK_VIDEO_CAPABILITY_SEPARATE_REFERENCE_IMAGES_BIT_KHR, "separate-reference-images"},
};

const std::initializer_list<FlagName> kEncodeCapFlags = {
    {VK_VIDEO_ENCODE_CAPABILITY_PRECEDING_EXTERNALLY_ENCODED_BYTES_BIT_KHR,
     "preceding-external-bytes"},
    {VK_VIDEO_ENCODE_CAPABILITY_INSUFFICIENT_BITSTREAM_BUFFER_RANGE_DETECTION_BIT_KHR,
     "overflow-detection"},
    {VK_VIDEO_ENCODE_CAPABILITY_QUANTIZATION_DELTA_MAP_BIT_KHR, "qp-delta-map"},
    {VK_VIDEO_ENCODE_CAPABILITY_EMPHASIS_MAP_BIT_KHR, "emphasis-map"},
};

const std::initializer_list<FlagName> kRateControlModes = {
    {VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR, "disabled(cqp)"},
    {VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR, "cbr"},
    {VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR, "vbr"},
};

const std::initializer_list<FlagName> kFeedbackFlags = {
    {VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BUFFER_OFFSET_BIT_KHR, "offset"},
    {VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BYTES_WRITTEN_BIT_KHR, "bytes-written"},
    {VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_HAS_OVERRIDES_BIT_KHR, "has-overrides"},
    {VK_VIDEO_ENCODE_FEEDBACK_AVERAGE_QUANTIZATION_BIT_KHR, "average-qp"},
    {VK_VIDEO_ENCODE_FEEDBACK_MIN_QUANTIZATION_BIT_KHR, "min-qp"},
    {VK_VIDEO_ENCODE_FEEDBACK_MAX_QUANTIZATION_BIT_KHR, "max-qp"},
    {VK_VIDEO_ENCODE_FEEDBACK_INTRA_PIXELS_BIT_KHR, "intra-pixels"},
    {VK_VIDEO_ENCODE_FEEDBACK_INTER_PIXELS_BIT_KHR, "inter-pixels"},
    {VK_VIDEO_ENCODE_FEEDBACK_SKIPPED_PIXELS_BIT_KHR, "skipped-pixels"},
    {VK_VIDEO_ENCODE_FEEDBACK_PICTURE_PARTITION_COUNT_BIT_KHR, "partition-count"},
};

const std::initializer_list<FlagName> kH265CapFlags = {
    {VK_VIDEO_ENCODE_H265_CAPABILITY_HRD_COMPLIANCE_BIT_KHR, "hrd-compliance"},
    {VK_VIDEO_ENCODE_H265_CAPABILITY_PREDICTION_WEIGHT_TABLE_GENERATED_BIT_KHR,
     "weight-table-generated"},
    {VK_VIDEO_ENCODE_H265_CAPABILITY_ROW_UNALIGNED_SLICE_SEGMENT_BIT_KHR,
     "row-unaligned-slice-segment"},
    {VK_VIDEO_ENCODE_H265_CAPABILITY_DIFFERENT_SLICE_SEGMENT_TYPE_BIT_KHR,
     "different-slice-segment-type"},
    {VK_VIDEO_ENCODE_H265_CAPABILITY_B_FRAME_IN_L0_LIST_BIT_KHR, "b-in-l0"},
    {VK_VIDEO_ENCODE_H265_CAPABILITY_B_FRAME_IN_L1_LIST_BIT_KHR, "b-in-l1"},
    {VK_VIDEO_ENCODE_H265_CAPABILITY_PER_PICTURE_TYPE_MIN_MAX_QP_BIT_KHR,
     "per-picture-type-min-max-qp"},
    {VK_VIDEO_ENCODE_H265_CAPABILITY_PER_SLICE_SEGMENT_CONSTANT_QP_BIT_KHR, "per-slice-segment-qp"},
    {VK_VIDEO_ENCODE_H265_CAPABILITY_MULTIPLE_TILES_PER_SLICE_SEGMENT_BIT_KHR,
     "tiles-per-slice-segment"},
    {VK_VIDEO_ENCODE_H265_CAPABILITY_MULTIPLE_SLICE_SEGMENTS_PER_TILE_BIT_KHR,
     "slice-segments-per-tile"},
    {VK_VIDEO_ENCODE_H265_CAPABILITY_CU_QP_DIFF_WRAPAROUND_BIT_KHR, "cu-qp-diff-wraparound"},
    {VK_VIDEO_ENCODE_H265_CAPABILITY_B_PICTURE_INTRA_REFRESH_BIT_KHR, "b-picture-intra-refresh"},
};

const std::initializer_list<FlagName> kH265StdFlags = {
    {VK_VIDEO_ENCODE_H265_STD_SEPARATE_COLOR_PLANE_FLAG_SET_BIT_KHR, "separate_colour_plane"},
    {VK_VIDEO_ENCODE_H265_STD_SAMPLE_ADAPTIVE_OFFSET_ENABLED_FLAG_SET_BIT_KHR, "sao"},
    {VK_VIDEO_ENCODE_H265_STD_SCALING_LIST_DATA_PRESENT_FLAG_SET_BIT_KHR, "scaling_list"},
    {VK_VIDEO_ENCODE_H265_STD_PCM_ENABLED_FLAG_SET_BIT_KHR, "pcm"},
    {VK_VIDEO_ENCODE_H265_STD_SPS_TEMPORAL_MVP_ENABLED_FLAG_SET_BIT_KHR, "temporal_mvp"},
    {VK_VIDEO_ENCODE_H265_STD_INIT_QP_MINUS26_BIT_KHR, "init_qp_minus26"},
    {VK_VIDEO_ENCODE_H265_STD_WEIGHTED_PRED_FLAG_SET_BIT_KHR, "weighted_pred"},
    {VK_VIDEO_ENCODE_H265_STD_WEIGHTED_BIPRED_FLAG_SET_BIT_KHR, "weighted_bipred"},
    {VK_VIDEO_ENCODE_H265_STD_LOG2_PARALLEL_MERGE_LEVEL_MINUS2_BIT_KHR, "parallel_merge_level"},
    {VK_VIDEO_ENCODE_H265_STD_SIGN_DATA_HIDING_ENABLED_FLAG_SET_BIT_KHR, "sign_data_hiding"},
    {VK_VIDEO_ENCODE_H265_STD_TRANSFORM_SKIP_ENABLED_FLAG_SET_BIT_KHR, "transform_skip=1"},
    {VK_VIDEO_ENCODE_H265_STD_TRANSFORM_SKIP_ENABLED_FLAG_UNSET_BIT_KHR, "transform_skip=0"},
    {VK_VIDEO_ENCODE_H265_STD_PPS_SLICE_CHROMA_QP_OFFSETS_PRESENT_FLAG_SET_BIT_KHR,
     "slice_chroma_qp_offsets"},
    {VK_VIDEO_ENCODE_H265_STD_TRANSQUANT_BYPASS_ENABLED_FLAG_SET_BIT_KHR, "transquant_bypass"},
    {VK_VIDEO_ENCODE_H265_STD_CONSTRAINED_INTRA_PRED_FLAG_SET_BIT_KHR, "constrained_intra_pred"},
    {VK_VIDEO_ENCODE_H265_STD_ENTROPY_CODING_SYNC_ENABLED_FLAG_SET_BIT_KHR, "entropy_coding_sync"},
    {VK_VIDEO_ENCODE_H265_STD_DEBLOCKING_FILTER_OVERRIDE_ENABLED_FLAG_SET_BIT_KHR,
     "deblocking_override"},
    {VK_VIDEO_ENCODE_H265_STD_DEPENDENT_SLICE_SEGMENTS_ENABLED_FLAG_SET_BIT_KHR,
     "dependent_slice_segments_enabled"},
    {VK_VIDEO_ENCODE_H265_STD_DEPENDENT_SLICE_SEGMENT_FLAG_SET_BIT_KHR, "dependent_slice_segment"},
    {VK_VIDEO_ENCODE_H265_STD_SLICE_QP_DELTA_BIT_KHR, "slice_qp_delta"},
    {VK_VIDEO_ENCODE_H265_STD_DIFFERENT_SLICE_QP_DELTA_BIT_KHR, "different_slice_qp_delta"},
};

const std::initializer_list<FlagName> kH264CapFlags = {
    {VK_VIDEO_ENCODE_H264_CAPABILITY_HRD_COMPLIANCE_BIT_KHR, "hrd-compliance"},
    {VK_VIDEO_ENCODE_H264_CAPABILITY_PREDICTION_WEIGHT_TABLE_GENERATED_BIT_KHR,
     "weight-table-generated"},
    {VK_VIDEO_ENCODE_H264_CAPABILITY_ROW_UNALIGNED_SLICE_BIT_KHR, "row-unaligned-slice"},
    {VK_VIDEO_ENCODE_H264_CAPABILITY_DIFFERENT_SLICE_TYPE_BIT_KHR, "different-slice-type"},
    {VK_VIDEO_ENCODE_H264_CAPABILITY_B_FRAME_IN_L0_LIST_BIT_KHR, "b-in-l0"},
    {VK_VIDEO_ENCODE_H264_CAPABILITY_B_FRAME_IN_L1_LIST_BIT_KHR, "b-in-l1"},
    {VK_VIDEO_ENCODE_H264_CAPABILITY_PER_PICTURE_TYPE_MIN_MAX_QP_BIT_KHR,
     "per-picture-type-min-max-qp"},
    {VK_VIDEO_ENCODE_H264_CAPABILITY_PER_SLICE_CONSTANT_QP_BIT_KHR, "per-slice-qp"},
    {VK_VIDEO_ENCODE_H264_CAPABILITY_GENERATE_PREFIX_NALU_BIT_KHR, "prefix-nalu"},
    {VK_VIDEO_ENCODE_H264_CAPABILITY_MB_QP_DIFF_WRAPAROUND_BIT_KHR, "mb-qp-diff-wraparound"},
    {VK_VIDEO_ENCODE_H264_CAPABILITY_B_PICTURE_INTRA_REFRESH_BIT_KHR, "b-picture-intra-refresh"},
};

const std::initializer_list<FlagName> kH264StdFlags = {
    {VK_VIDEO_ENCODE_H264_STD_SEPARATE_COLOR_PLANE_FLAG_SET_BIT_KHR, "separate_colour_plane"},
    {VK_VIDEO_ENCODE_H264_STD_QPPRIME_Y_ZERO_TRANSFORM_BYPASS_FLAG_SET_BIT_KHR,
     "qpprime_y_zero_transform_bypass"},
    {VK_VIDEO_ENCODE_H264_STD_SCALING_MATRIX_PRESENT_FLAG_SET_BIT_KHR, "scaling_matrix"},
    {VK_VIDEO_ENCODE_H264_STD_CHROMA_QP_INDEX_OFFSET_BIT_KHR, "chroma_qp_index_offset"},
    {VK_VIDEO_ENCODE_H264_STD_SECOND_CHROMA_QP_INDEX_OFFSET_BIT_KHR, "second_chroma_qp_offset"},
    {VK_VIDEO_ENCODE_H264_STD_PIC_INIT_QP_MINUS26_BIT_KHR, "pic_init_qp_minus26"},
    {VK_VIDEO_ENCODE_H264_STD_WEIGHTED_PRED_FLAG_SET_BIT_KHR, "weighted_pred"},
    {VK_VIDEO_ENCODE_H264_STD_WEIGHTED_BIPRED_IDC_EXPLICIT_BIT_KHR, "weighted_bipred_explicit"},
    {VK_VIDEO_ENCODE_H264_STD_WEIGHTED_BIPRED_IDC_IMPLICIT_BIT_KHR, "weighted_bipred_implicit"},
    {VK_VIDEO_ENCODE_H264_STD_TRANSFORM_8X8_MODE_FLAG_SET_BIT_KHR, "transform_8x8"},
    {VK_VIDEO_ENCODE_H264_STD_DIRECT_SPATIAL_MV_PRED_FLAG_UNSET_BIT_KHR, "direct_spatial_mv=0"},
    {VK_VIDEO_ENCODE_H264_STD_ENTROPY_CODING_MODE_FLAG_UNSET_BIT_KHR, "cavlc"},
    {VK_VIDEO_ENCODE_H264_STD_ENTROPY_CODING_MODE_FLAG_SET_BIT_KHR, "cabac"},
    {VK_VIDEO_ENCODE_H264_STD_DIRECT_8X8_INFERENCE_FLAG_UNSET_BIT_KHR, "direct_8x8_inference=0"},
    {VK_VIDEO_ENCODE_H264_STD_CONSTRAINED_INTRA_PRED_FLAG_SET_BIT_KHR, "constrained_intra_pred"},
    {VK_VIDEO_ENCODE_H264_STD_DEBLOCKING_FILTER_DISABLED_BIT_KHR, "deblocking_disabled"},
    {VK_VIDEO_ENCODE_H264_STD_DEBLOCKING_FILTER_ENABLED_BIT_KHR, "deblocking_enabled"},
    {VK_VIDEO_ENCODE_H264_STD_DEBLOCKING_FILTER_PARTIAL_BIT_KHR, "deblocking_partial"},
    {VK_VIDEO_ENCODE_H264_STD_SLICE_QP_DELTA_BIT_KHR, "slice_qp_delta"},
    {VK_VIDEO_ENCODE_H264_STD_DIFFERENT_SLICE_QP_DELTA_BIT_KHR, "different_slice_qp_delta"},
};

const std::initializer_list<FlagName> kAv1CapFlags = {
    {VK_VIDEO_ENCODE_AV1_CAPABILITY_PER_RATE_CONTROL_GROUP_MIN_MAX_Q_INDEX_BIT_KHR,
     "per-group-min-max-q-index"},
    {VK_VIDEO_ENCODE_AV1_CAPABILITY_GENERATE_OBU_EXTENSION_HEADER_BIT_KHR, "obu-extension-header"},
    {VK_VIDEO_ENCODE_AV1_CAPABILITY_PRIMARY_REFERENCE_CDF_ONLY_BIT_KHR,
     "primary-reference-cdf-only"},
    {VK_VIDEO_ENCODE_AV1_CAPABILITY_FRAME_SIZE_OVERRIDE_BIT_KHR, "frame-size-override"},
    {VK_VIDEO_ENCODE_AV1_CAPABILITY_MOTION_VECTOR_SCALING_BIT_KHR, "motion-vector-scaling"},
    {VK_VIDEO_ENCODE_AV1_CAPABILITY_COMPOUND_PREDICTION_INTRA_REFRESH_BIT_KHR,
     "compound-prediction-intra-refresh"},
};

const std::initializer_list<FlagName> kIntraRefreshModes = {
    {VK_VIDEO_ENCODE_INTRA_REFRESH_MODE_PER_PICTURE_PARTITION_BIT_KHR, "per-partition"},
    {VK_VIDEO_ENCODE_INTRA_REFRESH_MODE_BLOCK_BASED_BIT_KHR, "blocks"},
    {VK_VIDEO_ENCODE_INTRA_REFRESH_MODE_BLOCK_ROW_BASED_BIT_KHR, "rows"},
    {VK_VIDEO_ENCODE_INTRA_REFRESH_MODE_BLOCK_COLUMN_BASED_BIT_KHR, "columns"},
};

const std::initializer_list<FlagName> kH265RateControlFlags = {
    {VK_VIDEO_ENCODE_H265_RATE_CONTROL_ATTEMPT_HRD_COMPLIANCE_BIT_KHR, "hrd"},
    {VK_VIDEO_ENCODE_H265_RATE_CONTROL_REGULAR_GOP_BIT_KHR, "regular-gop"},
    {VK_VIDEO_ENCODE_H265_RATE_CONTROL_REFERENCE_PATTERN_FLAT_BIT_KHR, "flat"},
    {VK_VIDEO_ENCODE_H265_RATE_CONTROL_REFERENCE_PATTERN_DYADIC_BIT_KHR, "dyadic"},
    {VK_VIDEO_ENCODE_H265_RATE_CONTROL_TEMPORAL_SUB_LAYER_PATTERN_DYADIC_BIT_KHR,
     "dyadic-sub-layers"},
};

const std::initializer_list<FlagName> kImageUsage = {
    {VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "transfer-src"},
    {VK_IMAGE_USAGE_TRANSFER_DST_BIT, "transfer-dst"},
    {VK_IMAGE_USAGE_SAMPLED_BIT, "sampled"},
    {VK_IMAGE_USAGE_STORAGE_BIT, "storage"},
    {VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, "color-attachment"},
    {VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR, "decode-dst"},
    {VK_IMAGE_USAGE_VIDEO_DECODE_SRC_BIT_KHR, "decode-src"},
    {VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR, "decode-dpb"},
    {VK_IMAGE_USAGE_VIDEO_ENCODE_DST_BIT_KHR, "encode-dst"},
    {VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR, "encode-src"},
    {VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR, "encode-dpb"},
    {VK_IMAGE_USAGE_VIDEO_ENCODE_QUANTIZATION_DELTA_MAP_BIT_KHR, "qp-delta-map"},
    {VK_IMAGE_USAGE_VIDEO_ENCODE_EMPHASIS_MAP_BIT_KHR, "emphasis-map"},
};

const std::initializer_list<FlagName> kImageCreate = {
    {VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, "mutable-format"},
    {VK_IMAGE_CREATE_EXTENDED_USAGE_BIT, "extended-usage"},
    {VK_IMAGE_CREATE_DISJOINT_BIT, "disjoint"},
    {VK_IMAGE_CREATE_ALIAS_BIT, "alias"},
    {VK_IMAGE_CREATE_VIDEO_PROFILE_INDEPENDENT_BIT_KHR, "profile-independent"},
    {VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT, "2d-array-compatible"},
};

const std::initializer_list<FlagName> kFormatFeatures2 = {
    {VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT, "sampled"},
    {VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT, "storage"},
    {VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT, "color-attachment"},
    {VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT, "transfer-src"},
    {VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT, "transfer-dst"},
    {VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT, "linear-filter"},
    {VK_FORMAT_FEATURE_2_DISJOINT_BIT, "disjoint"},
    {VK_FORMAT_FEATURE_2_VIDEO_ENCODE_INPUT_BIT_KHR, "encode-input"},
    {VK_FORMAT_FEATURE_2_VIDEO_ENCODE_DPB_BIT_KHR, "encode-dpb"},
    {VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT, "storage-write-without-format"},
};

const char* deviceTypeName(VkPhysicalDeviceType type)
{
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
    default: return "other";
    }
}

const char* priorityName(VkQueueGlobalPriority p)
{
    switch (p) {
    case VK_QUEUE_GLOBAL_PRIORITY_LOW: return "low";
    case VK_QUEUE_GLOBAL_PRIORITY_MEDIUM: return "medium";
    case VK_QUEUE_GLOBAL_PRIORITY_HIGH: return "high";
    case VK_QUEUE_GLOBAL_PRIORITY_REALTIME: return "realtime";
    default: return "?";
    }
}

const char* timeDomainName(VkTimeDomainKHR d)
{
    switch (d) {
    case VK_TIME_DOMAIN_DEVICE_KHR: return "device";
    case VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR: return "clock-monotonic";
    case VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_KHR: return "clock-monotonic-raw";
    case VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR: return "qpc";
    default: return "?";
    }
}

std::string h265Level(int idc)
{
    static const char* const kNames[] = {"1.0", "2.0", "2.1", "3.0", "3.1", "4.0", "4.1",
                                         "5.0", "5.1", "5.2", "6.0", "6.1", "6.2"};
    if (idc >= 0 && idc < static_cast<int>(sizeof(kNames) / sizeof(kNames[0]))) return kNames[idc];
    return "idc " + std::to_string(idc);
}

std::string h264Level(int idc)
{
    static const char* const kNames[] = {"1.0", "1.1", "1.2", "1.3", "2.0", "2.1", "2.2",
                                         "3.0", "3.1", "3.2", "4.0", "4.1", "4.2", "5.0",
                                         "5.1", "5.2", "6.0", "6.1", "6.2"};
    if (idc >= 0 && idc < static_cast<int>(sizeof(kNames) / sizeof(kNames[0]))) return kNames[idc];
    return "idc " + std::to_string(idc);
}

std::string av1Level(int level)
{
    if (level < 0 || level > 23) return "level " + std::to_string(level);
    return std::to_string(2 + level / 4) + "." + std::to_string(level % 4);
}

std::string sizes(unsigned flags, std::initializer_list<std::pair<unsigned, int>> named)
{
    std::string out;
    for (const auto& n : named) {
        if (!(flags & n.first)) continue;
        if (!out.empty()) out += '|';
        out += std::to_string(n.second);
    }
    return out.empty() ? "none" : out;
}

std::string extent(const VkExtent2D& e)
{
    return std::to_string(e.width) + "x" + std::to_string(e.height);
}

/// "XR24" from a DRM fourcc.
std::string fourcc(uint32_t code)
{
    std::string s(4, ' ');
    for (int i = 0; i < 4; ++i) {
        const char c = static_cast<char>((code >> (8 * i)) & 0xff);
        s[i] = (c >= 32 && c < 127) ? c : '?';
    }
    return s;
}

/// libdrm's name for @p modifier ("AMD_GFX11,GFX9_64K_R_X,DCC,…"), or hex.
std::string modifierName(uint64_t modifier)
{
    if (modifier == 0) return "LINEAR";
    if (modifier == 0x00ffffffffffffffULL) return "INVALID";
    char* name = drmGetFormatModifierName(modifier);
    std::string out = name ? name : hex(modifier);
    std::free(name);
    return out;
}

/// The capture formats the KMS planes and the portal hand out, as Vulkan
/// formats: XR24/AR24 and XB24/AB24, and the 10-bit ones a HDR desktop or
/// KWin (which scans out XRGB2101010, 15/09) use.
struct CaptureFormat
{
    VkFormat format;
    const char* drm;
    uint32_t fourccA;
    uint32_t fourccX;
};

constexpr uint32_t fourccCode(char a, char b, char c, char d)
{
    return static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8) |
           (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24);
}

const CaptureFormat kCaptureFormats[] = {
    {VK_FORMAT_B8G8R8A8_UNORM, "XRGB8888/ARGB8888", fourccCode('A', 'R', '2', '4'),
     fourccCode('X', 'R', '2', '4')},
    {VK_FORMAT_R8G8B8A8_UNORM, "XBGR8888/ABGR8888", fourccCode('A', 'B', '2', '4'),
     fourccCode('X', 'B', '2', '4')},
    {VK_FORMAT_A2R10G10B10_UNORM_PACK32, "XRGB2101010/ARGB2101010", fourccCode('A', 'R', '3', '0'),
     fourccCode('X', 'R', '3', '0')},
    {VK_FORMAT_A2B10G10R10_UNORM_PACK32, "XBGR2101010/ABGR2101010", fourccCode('A', 'B', '3', '0'),
     fourccCode('X', 'B', '3', '0')},
};

// ── The encoder profiles asked about ────────────────────────────────────────

struct Profile
{
    const char* name;
    const char* key;
    VkVideoCodecOperationFlagBitsKHR op;
    const char* extension;
    int stdProfile;
    VkVideoComponentBitDepthFlagsKHR depth;
};

const Profile kProfiles[] = {
    {"HEVC Main", "hevc-main", VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR,
     VK_KHR_VIDEO_ENCODE_H265_EXTENSION_NAME, STD_VIDEO_H265_PROFILE_IDC_MAIN,
     VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR},
    {"HEVC Main 10", "hevc-main10", VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR,
     VK_KHR_VIDEO_ENCODE_H265_EXTENSION_NAME, STD_VIDEO_H265_PROFILE_IDC_MAIN_10,
     VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR},
    {"H.264 High", "h264-high", VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
     VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME, STD_VIDEO_H264_PROFILE_IDC_HIGH,
     VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR},
    {"H.264 Constrained Baseline", "h264-cbp", VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
     VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME, STD_VIDEO_H264_PROFILE_IDC_BASELINE,
     VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR},
    {"AV1 Main", "av1-main", VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR,
     VK_KHR_VIDEO_ENCODE_AV1_EXTENSION_NAME, STD_VIDEO_AV1_PROFILE_MAIN,
     VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR},
    {"AV1 Main 10-bit", "av1-main10", VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR,
     VK_KHR_VIDEO_ENCODE_AV1_EXTENSION_NAME, STD_VIDEO_AV1_PROFILE_MAIN,
     VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR},
};

// ── What one device holds while it is probed ────────────────────────────────

struct Device
{
    const Vulkan& vk;
    VkPhysicalDevice pd;
    std::vector<std::string> extensions;
    uint32_t apiVersion = 0;
    bool has(const char* name) const { return hasExtension(extensions, name); }
    bool video() const { return has(VK_KHR_VIDEO_QUEUE_EXTENSION_NAME); }
    bool globalPriority() const
    {
        return has(VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME) ||
               has(VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME);
    }
    bool globalPriorityQuery() const
    {
        return has(VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME) ||
               has(VK_EXT_GLOBAL_PRIORITY_QUERY_EXTENSION_NAME);
    }
};

// ── Queues ──────────────────────────────────────────────────────────────────

struct Family
{
    VkQueueFamilyProperties props = {};
    VkVideoCodecOperationFlagsKHR codecs = 0;
    bool resultStatus = false;
    std::vector<VkQueueGlobalPriority> priorities;
};

std::vector<Family> queueFamilies(const Device& d)
{
    uint32_t count = 0;
    d.vk.vkGetPhysicalDeviceQueueFamilyProperties2(d.pd, &count, nullptr);
    std::vector<VkQueueFamilyProperties2> props(count);
    std::vector<VkQueueFamilyVideoPropertiesKHR> video(count);
    std::vector<VkQueueFamilyQueryResultStatusPropertiesKHR> status(count);
    std::vector<VkQueueFamilyGlobalPriorityProperties> priorities(count);
    for (uint32_t i = 0; i < count; ++i) {
        void* head = nullptr;
        if (d.video()) {
            video[i] = {};
            video[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
            pushNext(head, video[i]);
            status[i] = {};
            status[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_QUERY_RESULT_STATUS_PROPERTIES_KHR;
            pushNext(head, status[i]);
        }
        if (d.globalPriorityQuery()) {
            priorities[i] = {};
            priorities[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_GLOBAL_PRIORITY_PROPERTIES;
            pushNext(head, priorities[i]);
        }
        props[i] = {};
        props[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
        props[i].pNext = head;
    }
    d.vk.vkGetPhysicalDeviceQueueFamilyProperties2(d.pd, &count, props.data());
    std::vector<Family> out(count);
    for (uint32_t i = 0; i < count; ++i) {
        out[i].props = props[i].queueFamilyProperties;
        if (d.video()) {
            out[i].codecs = video[i].videoCodecOperations;
            out[i].resultStatus = status[i].queryResultStatusSupport == VK_TRUE;
        }
        if (d.globalPriorityQuery()) {
            for (uint32_t p = 0; p < priorities[i].priorityCount; ++p)
                out[i].priorities.push_back(priorities[i].priorities[p]);
        }
    }
    return out;
}

/// What vkCreateDevice answers for one queue of @p family at @p priority —
/// the kernel is asked then (amdgpu creates the context at device creation).
VkResult tryPriority(const Device& d, uint32_t family, const Family& f,
                     VkQueueGlobalPriority priority)
{
    std::vector<const char*> enable;
    if (d.has(VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME))
        enable.push_back(VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME);
    else
        enable.push_back(VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME);
    if (f.props.queueFlags & (VK_QUEUE_VIDEO_ENCODE_BIT_KHR | VK_QUEUE_VIDEO_DECODE_BIT_KHR)) {
        enable.push_back(VK_KHR_VIDEO_QUEUE_EXTENSION_NAME);
        if ((f.props.queueFlags & VK_QUEUE_VIDEO_ENCODE_BIT_KHR) &&
            d.has(VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME))
            enable.push_back(VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME);
    }
    VkDeviceQueueGlobalPriorityCreateInfo prio = {};
    prio.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO;
    prio.globalPriority = priority;
    const float one = 1.0f;
    VkDeviceQueueCreateInfo queue = {};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.pNext = &prio;
    queue.queueFamilyIndex = family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &one;
    VkDeviceCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = static_cast<uint32_t>(enable.size());
    info.ppEnabledExtensionNames = enable.data();
    VkDevice device = VK_NULL_HANDLE;
    const VkResult result = d.vk.vkCreateDevice(d.pd, &info, nullptr, &device);
    if (result == VK_SUCCESS) d.vk.vkDestroyDevice(device, nullptr);
    return result;
}

// ── Encoders ────────────────────────────────────────────────────────────────

void formatsFor(const Device& d, const EncodeProfileChain& chain, VkImageUsageFlags usage,
                const char* label, Json& j)
{
    VkVideoProfileListInfoKHR list = {};
    list.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
    list.profileCount = 1;
    list.pProfiles = &chain.info;
    VkPhysicalDeviceVideoFormatInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR;
    info.pNext = &list;
    info.imageUsage = usage;
    uint32_t count = 0;
    VkResult result =
        d.vk.vkGetPhysicalDeviceVideoFormatPropertiesKHR(d.pd, &info, &count, nullptr);
    std::vector<VkVideoFormatPropertiesKHR> props(count);
    for (auto& p : props) {
        p = {};
        p.sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
    }
    if (result == VK_SUCCESS && count)
        result =
            d.vk.vkGetPhysicalDeviceVideoFormatPropertiesKHR(d.pd, &info, &count, props.data());
    j.key(label).beginObject();
    j.field("result", vkResultName(result));
    j.key("formats").beginArray();
    if (result != VK_SUCCESS) {
        say("      %s formats: %s\n", label, vkResultName(result).c_str());
    } else {
        for (uint32_t i = 0; i < count; ++i) {
            const VkVideoFormatPropertiesKHR& p = props[i];
            say("      %s: %s, %s, usage %s, create %s\n", label, vkFormatName(p.format).c_str(),
                p.imageTiling == VK_IMAGE_TILING_OPTIMAL  ? "optimal"
                : p.imageTiling == VK_IMAGE_TILING_LINEAR ? "linear"
                                                          : "other tiling",
                decode(p.imageUsageFlags, kImageUsage).c_str(),
                decode(p.imageCreateFlags, kImageCreate).c_str());
            j.beginObject();
            j.field("format", vkFormatName(p.format));
            j.field("formatId", static_cast<int>(p.format));
            j.field("tiling", static_cast<int>(p.imageTiling));
            j.field("usage", decode(p.imageUsageFlags, kImageUsage));
            j.field("create", decode(p.imageCreateFlags, kImageCreate));
            j.endObject();
        }
    }
    j.endArray();
    j.endObject();
}

/// Whether an input picture of @p format can be made with @p usage (and
/// @p flags) for this profile: what the conversion's way of writing needs.
VkResult inputImage(const Device& d, const EncodeProfileChain& chain, VkFormat format,
                    VkImageUsageFlags usage, VkImageCreateFlags flags)
{
    VkVideoProfileListInfoKHR list = {};
    list.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
    list.profileCount = 1;
    list.pProfiles = &chain.info;
    VkPhysicalDeviceImageFormatInfo2 info = {};
    info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    info.pNext = &list;
    info.format = format;
    info.type = VK_IMAGE_TYPE_2D;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.flags = flags;
    VkImageFormatProperties2 props = {};
    props.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    return d.vk.vkGetPhysicalDeviceImageFormatProperties2(d.pd, &info, &props);
}

void encodeProfile(const Device& d, const Profile& p, Json& j)
{
    j.beginObject();
    j.field("profile", p.key);
    j.field("name", p.name);
    if (!d.has(p.extension) || !d.has(VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME) ||
        !d.vk.vkGetPhysicalDeviceVideoCapabilitiesKHR) {
        say("  %s: no %s\n", p.name, p.extension);
        j.field("supported", false);
        j.field("why", std::string("no ") + p.extension);
        j.endObject();
        return;
    }
    const EncodeProfileChain chain(p.op, p.stdProfile, p.depth);
    const bool h264 = p.op == VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
    const bool h265 = p.op == VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
    const bool av1 = p.op == VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR;
    const bool qpMapExt = d.has(VK_KHR_VIDEO_ENCODE_QUANTIZATION_MAP_EXTENSION_NAME);
    const bool intraRefreshExt = d.has(VK_KHR_VIDEO_ENCODE_INTRA_REFRESH_EXTENSION_NAME);

    VkVideoEncodeH264CapabilitiesKHR c264 = {};
    c264.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_KHR;
    VkVideoEncodeH265CapabilitiesKHR c265 = {};
    c265.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_CAPABILITIES_KHR;
    VkVideoEncodeAV1CapabilitiesKHR cav1 = {};
    cav1.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_CAPABILITIES_KHR;
    VkVideoEncodeQuantizationMapCapabilitiesKHR qmap = {};
    qmap.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUANTIZATION_MAP_CAPABILITIES_KHR;
    VkVideoEncodeH264QuantizationMapCapabilitiesKHR qmap264 = {};
    qmap264.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_QUANTIZATION_MAP_CAPABILITIES_KHR;
    VkVideoEncodeH265QuantizationMapCapabilitiesKHR qmap265 = {};
    qmap265.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_QUANTIZATION_MAP_CAPABILITIES_KHR;
    VkVideoEncodeIntraRefreshCapabilitiesKHR refresh = {};
    refresh.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_INTRA_REFRESH_CAPABILITIES_KHR;
    VkVideoEncodeCapabilitiesKHR enc = {};
    enc.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR;
    VkVideoCapabilitiesKHR caps = {};
    caps.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;

    void* head = nullptr;
    if (h264) pushNext(head, c264);
    if (h265) pushNext(head, c265);
    if (av1) pushNext(head, cav1);
    if (qpMapExt) {
        pushNext(head, qmap);
        if (h264) pushNext(head, qmap264);
        if (h265) pushNext(head, qmap265);
    }
    if (intraRefreshExt) pushNext(head, refresh);
    pushNext(head, enc);
    caps.pNext = head;

    const VkResult result = d.vk.vkGetPhysicalDeviceVideoCapabilitiesKHR(d.pd, &chain.info, &caps);
    j.field("result", vkResultName(result));
    j.field("supported", result == VK_SUCCESS);
    if (result != VK_SUCCESS) {
        say("  %s: %s\n", p.name, vkResultName(result).c_str());
        j.endObject();
        return;
    }

    say("  %s (streaming, desktop|rendered, ultra-low-latency):\n", p.name);
    say("    video: coded %s..%s, picture granularity %s, DPB %u, active refs %u, "
        "bitstream offset/size alignment %llu/%llu, %s, std %s v%s\n",
        extent(caps.minCodedExtent).c_str(), extent(caps.maxCodedExtent).c_str(),
        extent(caps.pictureAccessGranularity).c_str(), caps.maxDpbSlots,
        caps.maxActiveReferencePictures,
        static_cast<unsigned long long>(caps.minBitstreamBufferOffsetAlignment),
        static_cast<unsigned long long>(caps.minBitstreamBufferSizeAlignment),
        decode(caps.flags, kVideoCapFlags).c_str(), caps.stdHeaderVersion.extensionName,
        vkVersionText(caps.stdHeaderVersion.specVersion).c_str());
    say("    encode: %s, rate control %s, %u layer(s), max %llu b/s, %u quality level(s), "
        "input granularity %s, feedback %s\n",
        decode(enc.flags, kEncodeCapFlags).c_str(),
        decode(enc.rateControlModes, kRateControlModes).c_str(), enc.maxRateControlLayers,
        static_cast<unsigned long long>(enc.maxBitrate), enc.maxQualityLevels,
        extent(enc.encodeInputPictureGranularity).c_str(),
        decode(enc.supportedEncodeFeedbackFlags, kFeedbackFlags).c_str());
    j.key("video").beginObject();
    j.field("minCoded", extent(caps.minCodedExtent));
    j.field("maxCoded", extent(caps.maxCodedExtent));
    j.field("pictureAccessGranularity", extent(caps.pictureAccessGranularity));
    j.field("maxDpbSlots", caps.maxDpbSlots);
    j.field("maxActiveReferencePictures", caps.maxActiveReferencePictures);
    j.field("bitstreamOffsetAlignment",
            static_cast<unsigned long long>(caps.minBitstreamBufferOffsetAlignment));
    j.field("bitstreamSizeAlignment",
            static_cast<unsigned long long>(caps.minBitstreamBufferSizeAlignment));
    j.field("flags", decode(caps.flags, kVideoCapFlags));
    j.field("stdHeader", std::string(caps.stdHeaderVersion.extensionName));
    j.field("stdHeaderVersion", vkVersionText(caps.stdHeaderVersion.specVersion));
    j.endObject();
    j.key("encode").beginObject();
    j.field("flags", decode(enc.flags, kEncodeCapFlags));
    j.field("rateControlModes", decode(enc.rateControlModes, kRateControlModes));
    j.field("maxRateControlLayers", enc.maxRateControlLayers);
    j.field("maxBitrate", static_cast<unsigned long long>(enc.maxBitrate));
    j.field("maxQualityLevels", enc.maxQualityLevels);
    j.field("inputGranularity", extent(enc.encodeInputPictureGranularity));
    j.field("feedback", decode(enc.supportedEncodeFeedbackFlags, kFeedbackFlags));
    j.endObject();

    if (h265) {
        say("    H.265: %s, level %s, %u slice segment(s), tiles %s, CTB %s, TB %s, refs P %u "
            "B %u L1 %u, sub-layers %u, QP %d..%d, GOP remaining %s/%s\n",
            decode(c265.flags, kH265CapFlags).c_str(), h265Level(c265.maxLevelIdc).c_str(),
            c265.maxSliceSegmentCount, extent(c265.maxTiles).c_str(),
            sizes(c265.ctbSizes, {{VK_VIDEO_ENCODE_H265_CTB_SIZE_16_BIT_KHR, 16},
                                  {VK_VIDEO_ENCODE_H265_CTB_SIZE_32_BIT_KHR, 32},
                                  {VK_VIDEO_ENCODE_H265_CTB_SIZE_64_BIT_KHR, 64}})
                .c_str(),
            sizes(c265.transformBlockSizes,
                  {{VK_VIDEO_ENCODE_H265_TRANSFORM_BLOCK_SIZE_4_BIT_KHR, 4},
                   {VK_VIDEO_ENCODE_H265_TRANSFORM_BLOCK_SIZE_8_BIT_KHR, 8},
                   {VK_VIDEO_ENCODE_H265_TRANSFORM_BLOCK_SIZE_16_BIT_KHR, 16},
                   {VK_VIDEO_ENCODE_H265_TRANSFORM_BLOCK_SIZE_32_BIT_KHR, 32}})
                .c_str(),
            c265.maxPPictureL0ReferenceCount, c265.maxBPictureL0ReferenceCount,
            c265.maxL1ReferenceCount, c265.maxSubLayerCount, c265.minQp, c265.maxQp,
            c265.prefersGopRemainingFrames ? "preferred" : "-",
            c265.requiresGopRemainingFrames ? "required" : "-");
        say("    H.265 std syntax honoured: %s\n",
            decode(c265.stdSyntaxFlags, kH265StdFlags).c_str());
        j.key("h265").beginObject();
        j.field("flags", decode(c265.flags, kH265CapFlags));
        j.field("maxLevel", h265Level(c265.maxLevelIdc));
        j.field("maxSliceSegmentCount", c265.maxSliceSegmentCount);
        j.field("maxTiles", extent(c265.maxTiles));
        j.field("ctbSizes", sizes(c265.ctbSizes, {{VK_VIDEO_ENCODE_H265_CTB_SIZE_16_BIT_KHR, 16},
                                                  {VK_VIDEO_ENCODE_H265_CTB_SIZE_32_BIT_KHR, 32},
                                                  {VK_VIDEO_ENCODE_H265_CTB_SIZE_64_BIT_KHR, 64}}));
        j.field("transformBlockSizes",
                sizes(c265.transformBlockSizes,
                      {{VK_VIDEO_ENCODE_H265_TRANSFORM_BLOCK_SIZE_4_BIT_KHR, 4},
                       {VK_VIDEO_ENCODE_H265_TRANSFORM_BLOCK_SIZE_8_BIT_KHR, 8},
                       {VK_VIDEO_ENCODE_H265_TRANSFORM_BLOCK_SIZE_16_BIT_KHR, 16},
                       {VK_VIDEO_ENCODE_H265_TRANSFORM_BLOCK_SIZE_32_BIT_KHR, 32}}));
        j.field("maxPL0", c265.maxPPictureL0ReferenceCount);
        j.field("maxBL0", c265.maxBPictureL0ReferenceCount);
        j.field("maxL1", c265.maxL1ReferenceCount);
        j.field("maxSubLayers", c265.maxSubLayerCount);
        j.field("minQp", c265.minQp);
        j.field("maxQp", c265.maxQp);
        j.field("prefersGopRemainingFrames", c265.prefersGopRemainingFrames == VK_TRUE);
        j.field("requiresGopRemainingFrames", c265.requiresGopRemainingFrames == VK_TRUE);
        j.field("stdSyntax", decode(c265.stdSyntaxFlags, kH265StdFlags));
        j.endObject();
    }
    if (h264) {
        say("    H.264: %s, level %s, %u slice(s), refs P %u B %u L1 %u, temporal layers %u, "
            "QP %d..%d, GOP remaining %s/%s\n",
            decode(c264.flags, kH264CapFlags).c_str(), h264Level(c264.maxLevelIdc).c_str(),
            c264.maxSliceCount, c264.maxPPictureL0ReferenceCount, c264.maxBPictureL0ReferenceCount,
            c264.maxL1ReferenceCount, c264.maxTemporalLayerCount, c264.minQp, c264.maxQp,
            c264.prefersGopRemainingFrames ? "preferred" : "-",
            c264.requiresGopRemainingFrames ? "required" : "-");
        say("    H.264 std syntax honoured: %s\n",
            decode(c264.stdSyntaxFlags, kH264StdFlags).c_str());
        j.key("h264").beginObject();
        j.field("flags", decode(c264.flags, kH264CapFlags));
        j.field("maxLevel", h264Level(c264.maxLevelIdc));
        j.field("maxSliceCount", c264.maxSliceCount);
        j.field("maxPL0", c264.maxPPictureL0ReferenceCount);
        j.field("maxBL0", c264.maxBPictureL0ReferenceCount);
        j.field("maxL1", c264.maxL1ReferenceCount);
        j.field("maxTemporalLayers", c264.maxTemporalLayerCount);
        j.field("minQp", c264.minQp);
        j.field("maxQp", c264.maxQp);
        j.field("stdSyntax", decode(c264.stdSyntaxFlags, kH264StdFlags));
        j.endObject();
    }
    if (av1) {
        say("    AV1: %s, level %s, alignment %s, tiles %s, superblock %s, single refs %u, "
            "q-index %u..%u\n",
            decode(cav1.flags, kAv1CapFlags).c_str(), av1Level(cav1.maxLevel).c_str(),
            extent(cav1.codedPictureAlignment).c_str(), extent(cav1.maxTiles).c_str(),
            sizes(cav1.superblockSizes, {{VK_VIDEO_ENCODE_AV1_SUPERBLOCK_SIZE_64_BIT_KHR, 64},
                                         {VK_VIDEO_ENCODE_AV1_SUPERBLOCK_SIZE_128_BIT_KHR, 128}})
                .c_str(),
            cav1.maxSingleReferenceCount, cav1.minQIndex, cav1.maxQIndex);
        j.key("av1").beginObject();
        j.field("flags", decode(cav1.flags, kAv1CapFlags));
        j.field("maxLevel", av1Level(cav1.maxLevel));
        j.field("codedPictureAlignment", extent(cav1.codedPictureAlignment));
        j.field("maxTiles", extent(cav1.maxTiles));
        j.field("maxSingleReferenceCount", cav1.maxSingleReferenceCount);
        j.field("minQIndex", cav1.minQIndex);
        j.field("maxQIndex", cav1.maxQIndex);
        j.endObject();
    }
    if (qpMapExt) {
        say("    QP map: up to %s", extent(qmap.maxQuantizationMapExtent).c_str());
        if (h265) say(", delta %d..%d", qmap265.minQpDelta, qmap265.maxQpDelta);
        if (h264) say(", delta %d..%d", qmap264.minQpDelta, qmap264.maxQpDelta);
        say("\n");
        j.key("qpMap").beginObject();
        j.field("maxExtent", extent(qmap.maxQuantizationMapExtent));
        if (h265) {
            j.field("minQpDelta", qmap265.minQpDelta);
            j.field("maxQpDelta", qmap265.maxQpDelta);
        }
        if (h264) {
            j.field("minQpDelta", qmap264.minQpDelta);
            j.field("maxQpDelta", qmap264.maxQpDelta);
        }
        j.endObject();
    } else {
        say("    QP map: no %s\n", VK_KHR_VIDEO_ENCODE_QUANTIZATION_MAP_EXTENSION_NAME);
        j.key("qpMap").null();
    }
    if (intraRefreshExt) {
        say("    intra-refresh: %s, cycle up to %u, %u active ref(s)%s%s\n",
            decode(refresh.intraRefreshModes, kIntraRefreshModes).c_str(),
            refresh.maxIntraRefreshCycleDuration, refresh.maxIntraRefreshActiveReferencePictures,
            refresh.partitionIndependentIntraRefreshRegions ? ", independent regions" : "",
            refresh.nonRectangularIntraRefreshRegions ? ", non-rectangular" : "");
        j.key("intraRefresh").beginObject();
        j.field("modes", decode(refresh.intraRefreshModes, kIntraRefreshModes));
        j.field("maxCycle", refresh.maxIntraRefreshCycleDuration);
        j.field("maxActiveReferences", refresh.maxIntraRefreshActiveReferencePictures);
        j.field("independentRegions", refresh.partitionIndependentIntraRefreshRegions == VK_TRUE);
        j.field("nonRectangular", refresh.nonRectangularIntraRefreshRegions == VK_TRUE);
        j.endObject();
    } else {
        say("    intra-refresh: no %s\n", VK_KHR_VIDEO_ENCODE_INTRA_REFRESH_EXTENSION_NAME);
        j.key("intraRefresh").null();
    }

    // What every quality level prefers: the rate control mode, and for HEVC
    // the constant QP and references it would pick on its own.
    j.key("qualityLevels").beginArray();
    for (uint32_t level = 0; level < enc.maxQualityLevels &&
                             d.vk.vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR;
         ++level) {
        VkPhysicalDeviceVideoEncodeQualityLevelInfoKHR info = {};
        info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_ENCODE_QUALITY_LEVEL_INFO_KHR;
        info.pVideoProfile = &chain.info;
        info.qualityLevel = level;
        VkVideoEncodeH265QualityLevelPropertiesKHR q265 = {};
        q265.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_QUALITY_LEVEL_PROPERTIES_KHR;
        VkVideoEncodeQualityLevelPropertiesKHR q = {};
        q.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUALITY_LEVEL_PROPERTIES_KHR;
        if (h265) q.pNext = &q265;
        const VkResult r =
            d.vk.vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR(d.pd, &info, &q);
        j.beginObject();
        j.field("level", level);
        j.field("result", vkResultName(r));
        if (r == VK_SUCCESS) {
            const std::string mode =
                q.preferredRateControlMode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR
                    ? std::string("default")
                    : decode(q.preferredRateControlMode, kRateControlModes);
            say("    quality %u: prefers %s", level, mode.c_str());
            j.field("preferredRateControl", mode);
            j.field("preferredLayers", q.preferredRateControlLayerCount);
            if (h265) {
                say(", %s, GOP %u, IDR %u, B %u, QP I/P/B %d/%d/%d, refs L0 %u L1 %u",
                    decode(q265.preferredRateControlFlags, kH265RateControlFlags).c_str(),
                    q265.preferredGopFrameCount, q265.preferredIdrPeriod,
                    q265.preferredConsecutiveBFrameCount, q265.preferredConstantQp.qpI,
                    q265.preferredConstantQp.qpP, q265.preferredConstantQp.qpB,
                    q265.preferredMaxL0ReferenceCount, q265.preferredMaxL1ReferenceCount);
                j.field("rateControlFlags",
                        decode(q265.preferredRateControlFlags, kH265RateControlFlags));
                j.field("gop", q265.preferredGopFrameCount);
                j.field("idrPeriod", q265.preferredIdrPeriod);
                j.field("bFrames", q265.preferredConsecutiveBFrameCount);
                j.field("qpI", q265.preferredConstantQp.qpI);
                j.field("qpP", q265.preferredConstantQp.qpP);
                j.field("qpB", q265.preferredConstantQp.qpB);
                j.field("maxL0", q265.preferredMaxL0ReferenceCount);
                j.field("maxL1", q265.preferredMaxL1ReferenceCount);
            }
            say("\n");
        } else {
            say("    quality %u: %s\n", level, vkResultName(r).c_str());
        }
        j.endObject();
    }
    j.endArray();

    // The pictures: what the encoder's input and its references may be.
    if (d.vk.vkGetPhysicalDeviceVideoFormatPropertiesKHR) {
        formatsFor(d, chain, VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR, "input", j);
        formatsFor(d, chain, VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR, "dpb", j);
    }

    // How the conversion could write the input: straight into it (compute
    // through a storage view of each plane, or a render pass per plane as
    // the D3D12 chain does), or into a picture of its own and then copied.
    const VkFormat input = p.depth == VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR
                               ? VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16
                               : VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    const VkImageCreateFlags perPlane =
        VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    struct Way
    {
        const char* name;
        VkImageUsageFlags usage;
        VkImageCreateFlags flags;
    };
    const Way ways[] = {
        {"storage-per-plane", VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_STORAGE_BIT,
         perPlane},
        {"render-per-plane",
         VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, perPlane},
        {"copied-in", VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0},
    };
    std::string line;
    j.key("inputWrites").beginObject();
    for (const Way& w : ways) {
        const VkResult r = inputImage(d, chain, input, w.usage, w.flags);
        if (!line.empty()) line += ", ";
        line += std::string(w.name) + " " + (r == VK_SUCCESS ? "yes" : vkResultName(r));
        j.field(w.name, vkResultName(r));
    }
    j.endObject();
    say("    writing the input: %s\n", line.c_str());
    j.endObject();
}

// ── Formats: planes of the encoder's input, capture formats to import ───────

VkFormatFeatureFlags2 optimalFeatures(const Device& d, VkFormat format)
{
    VkFormatProperties3 f3 = {};
    f3.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3;
    VkFormatProperties2 f2 = {};
    f2.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    f2.pNext = &f3;
    d.vk.vkGetPhysicalDeviceFormatProperties2(d.pd, format, &f2);
    return f3.optimalTilingFeatures;
}

void planeFormats(const Device& d, Json& j)
{
    const VkFormat formats[] = {VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
                                VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,
                                VK_FORMAT_R8_UNORM,
                                VK_FORMAT_R8G8_UNORM,
                                VK_FORMAT_R16_UNORM,
                                VK_FORMAT_R16G16_UNORM};
    say("  format features (optimal tiling):\n");
    j.key("formatFeatures").beginObject();
    for (const VkFormat f : formats) {
        const VkFormatFeatureFlags2 features = optimalFeatures(d, f);
        say("    %s: %s\n", vkFormatName(f).c_str(), decode(features, kFormatFeatures2).c_str());
        j.field(vkFormatName(f), decode(features, kFormatFeatures2));
    }
    j.endObject();
}

struct Importable
{
    uint64_t modifier = 0;
    uint32_t planes = 0;
    VkFormatFeatureFlags2 features = 0;
    VkResult sampled = VK_ERROR_FORMAT_NOT_SUPPORTED;
    VkExternalMemoryFeatureFlags memory = 0;
    VkExtent3D maxExtent = {};
};

std::vector<Importable> importable(const Device& d, VkFormat format)
{
    std::vector<Importable> out;
    VkDrmFormatModifierPropertiesList2EXT list = {};
    list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT;
    VkFormatProperties2 f2 = {};
    f2.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    f2.pNext = &list;
    d.vk.vkGetPhysicalDeviceFormatProperties2(d.pd, format, &f2);
    std::vector<VkDrmFormatModifierProperties2EXT> mods(list.drmFormatModifierCount);
    list.pDrmFormatModifierProperties = mods.data();
    d.vk.vkGetPhysicalDeviceFormatProperties2(d.pd, format, &f2);
    for (const VkDrmFormatModifierProperties2EXT& m : mods) {
        Importable imp;
        imp.modifier = m.drmFormatModifier;
        imp.planes = m.drmFormatModifierPlaneCount;
        imp.features = m.drmFormatModifierTilingFeatures;
        // Imported to be read by the conversion: sampled, or copied out.
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT modInfo = {};
        modInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
        modInfo.drmFormatModifier = m.drmFormatModifier;
        modInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkPhysicalDeviceExternalImageFormatInfo ext = {};
        ext.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
        ext.pNext = &modInfo;
        ext.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkPhysicalDeviceImageFormatInfo2 info = {};
        info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
        info.pNext = &ext;
        info.format = format;
        info.type = VK_IMAGE_TYPE_2D;
        info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VkExternalImageFormatProperties extProps = {};
        extProps.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
        VkImageFormatProperties2 props = {};
        props.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
        props.pNext = &extProps;
        imp.sampled = d.vk.vkGetPhysicalDeviceImageFormatProperties2(d.pd, &info, &props);
        if (imp.sampled == VK_SUCCESS) {
            imp.memory = extProps.externalMemoryProperties.externalMemoryFeatures;
            imp.maxExtent = props.imageFormatProperties.maxExtent;
        }
        out.push_back(imp);
    }
    return out;
}

bool importsFor(const Importable& imp)
{
    return imp.sampled == VK_SUCCESS && (imp.memory & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT);
}

void dmabuf(const Device& d, std::vector<std::vector<Importable>>& tables, Json& j)
{
    j.key("dmabuf").beginArray();
    if (!d.has(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME) ||
        !d.has(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) ||
        d.apiVersion < VK_API_VERSION_1_3) {
        say("  DMA-BUF import: no %s / %s (or Vulkan < 1.3)\n",
            VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
            VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
        j.endArray();
        tables.assign(sizeof(kCaptureFormats) / sizeof(kCaptureFormats[0]), {});
        return;
    }
    say("  DMA-BUF import (sampled|transfer-src, by modifier):\n");
    for (const CaptureFormat& cf : kCaptureFormats) {
        tables.push_back(importable(d, cf.format));
        const std::vector<Importable>& mods = tables.back();
        size_t ok = 0;
        for (const Importable& imp : mods)
            ok += importsFor(imp) ? 1 : 0;
        say("    %s (%s): %zu modifier(s), %zu importable\n", vkFormatName(cf.format).c_str(),
            cf.drm, mods.size(), ok);
        j.beginObject();
        j.field("format", vkFormatName(cf.format));
        j.field("drm", cf.drm);
        j.key("modifiers").beginArray();
        for (const Importable& imp : mods) {
            say("      %-18s %u plane(s) %-9s %s\n", hex(imp.modifier).c_str(), imp.planes,
                importsFor(imp) ? "import" : vkResultName(imp.sampled).c_str(),
                modifierName(imp.modifier).c_str());
            j.beginObject();
            j.field("modifier", hex(imp.modifier));
            j.field("name", modifierName(imp.modifier));
            j.field("planes", imp.planes);
            j.field("features", decode(imp.features, kFormatFeatures2));
            j.field("sampledImport", vkResultName(imp.sampled));
            j.field("importable", importsFor(imp));
            if (imp.sampled == VK_SUCCESS)
                j.field("maxExtent", std::to_string(imp.maxExtent.width) + "x" +
                                         std::to_string(imp.maxExtent.height));
            j.endObject();
        }
        j.endArray();
        j.endObject();
    }
    j.endArray();
}

// ── KMS: what the planes scan out now ───────────────────────────────────────

/// The DRM primary node whose device numbers are @p major:@p minor.
std::string cardFor(int64_t wantMajor, int64_t wantMinor)
{
    for (int i = 0; i < 16; ++i) {
        const std::string path = "/dev/dri/card" + std::to_string(i);
        struct stat st = {};
        if (::stat(path.c_str(), &st) != 0) continue;
        if (static_cast<int64_t>(major(st.st_rdev)) == wantMajor &&
            static_cast<int64_t>(minor(st.st_rdev)) == wantMinor)
            return path;
    }
    return "";
}

uint64_t planeType(int fd, uint32_t planeId)
{
    uint64_t type = ~0ULL;
    drmModeObjectProperties* props = drmModeObjectGetProperties(fd, planeId, DRM_MODE_OBJECT_PLANE);
    for (uint32_t i = 0; props && i < props->count_props; ++i) {
        drmModePropertyRes* prop = drmModeGetProperty(fd, props->props[i]);
        if (prop && std::strcmp(prop->name, "type") == 0) type = props->prop_values[i];
        drmModeFreeProperty(prop);
    }
    drmModeFreeObjectProperties(props);
    return type;
}

void kms(const std::string& card, const std::vector<std::vector<Importable>>& tables, Json& j)
{
    j.key("kms").beginObject();
    j.field("card", card);
    const int fd = card.empty() ? -1 : ::open(card.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        say("  KMS: cannot open %s\n", card.empty() ? "(no primary node)" : card.c_str());
        j.field("error", "cannot open");
        j.endObject();
        return;
    }
    drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmModePlaneRes* planes = drmModeGetPlaneResources(fd);
    say("  KMS %s — the planes that show something now:\n", card.c_str());
    j.key("planes").beginArray();
    for (uint32_t i = 0; planes && i < planes->count_planes; ++i) {
        drmModePlane* p = drmModeGetPlane(fd, planes->planes[i]);
        if (!p) continue;
        if (!p->fb_id) {
            drmModeFreePlane(p);
            continue;
        }
        const uint64_t type = planeType(fd, p->plane_id);
        const char* typeName = type == DRM_PLANE_TYPE_PRIMARY   ? "primary"
                               : type == DRM_PLANE_TYPE_CURSOR  ? "cursor"
                               : type == DRM_PLANE_TYPE_OVERLAY ? "overlay"
                                                                : "?";
        drmModeFB2* fb = drmModeGetFB2(fd, p->fb_id);
        j.beginObject();
        j.field("plane", p->plane_id);
        j.field("type", typeName);
        j.field("crtc", p->crtc_id);
        if (fb) {
            const uint64_t modifier = (fb->flags & DRM_MODE_FB_MODIFIERS) ? fb->modifier : 0;
            // Does the chain import it — the format's table, this modifier?
            std::string verdict = "not a capture format";
            for (size_t f = 0; f < sizeof(kCaptureFormats) / sizeof(kCaptureFormats[0]); ++f) {
                const CaptureFormat& cf = kCaptureFormats[f];
                if (fb->pixel_format != cf.fourccA && fb->pixel_format != cf.fourccX) continue;
                verdict = "not in the driver's list";
                if (f < tables.size()) {
                    for (const Importable& imp : tables[f]) {
                        if (imp.modifier != modifier) continue;
                        verdict = importsFor(imp)
                                      ? "imports, " + std::to_string(imp.planes) + " plane(s)"
                                      : "listed, refused (" + vkResultName(imp.sampled) + ")";
                    }
                }
            }
            say("    plane %u (%s) on CRTC %u: %ux%u %s %s %s — %s\n", p->plane_id, typeName,
                p->crtc_id, fb->width, fb->height, fourcc(fb->pixel_format).c_str(),
                hex(modifier).c_str(), modifierName(modifier).c_str(), verdict.c_str());
            j.field("width", fb->width);
            j.field("height", fb->height);
            j.field("fourcc", fourcc(fb->pixel_format));
            j.field("modifier", hex(modifier));
            j.field("modifierName", modifierName(modifier));
            j.field("vulkan", verdict);
            drmModeFreeFB2(fb);
        } else {
            say("    plane %u (%s): GETFB2 refused\n", p->plane_id, typeName);
            j.field("error", "GETFB2 refused");
        }
        j.endObject();
        drmModeFreePlane(p);
    }
    j.endArray();
    if (planes) drmModeFreePlaneResources(planes);
    ::close(fd);
    j.endObject();
}

// ── Semaphores ──────────────────────────────────────────────────────────────

void semaphores(const Device& d, Json& j)
{
    auto ask = [&](VkExternalSemaphoreHandleTypeFlagBits type, bool timeline) {
        VkSemaphoreTypeCreateInfo kind = {};
        kind.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        kind.semaphoreType = timeline ? VK_SEMAPHORE_TYPE_TIMELINE : VK_SEMAPHORE_TYPE_BINARY;
        VkPhysicalDeviceExternalSemaphoreInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO;
        info.pNext = &kind;
        info.handleType = type;
        VkExternalSemaphoreProperties props = {};
        props.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES;
        d.vk.vkGetPhysicalDeviceExternalSemaphoreProperties(d.pd, &info, &props);
        return decode(props.externalSemaphoreFeatures,
                      {{VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT, "export"},
                       {VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT, "import"}});
    };
    const std::string syncFd = ask(VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, false);
    const std::string opaque = ask(VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, true);
    say("  semaphores: sync_file (binary) %s, opaque fd (timeline) %s, %s %s\n", syncFd.c_str(),
        opaque.c_str(), VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        d.has(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME) ? "present" : "absent");
    j.key("semaphores").beginObject();
    j.field("syncFdBinary", syncFd);
    j.field("opaqueFdTimeline", opaque);
    j.field("externalSemaphoreFd", d.has(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME));
    j.endObject();
}

// ── One device ──────────────────────────────────────────────────────────────

const char* const kWatchedExtensions[] = {
    VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
    VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME,
    VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME,
    VK_KHR_VIDEO_ENCODE_H265_EXTENSION_NAME,
    VK_KHR_VIDEO_ENCODE_AV1_EXTENSION_NAME,
    VK_KHR_VIDEO_ENCODE_QUANTIZATION_MAP_EXTENSION_NAME,
    VK_KHR_VIDEO_ENCODE_INTRA_REFRESH_EXTENSION_NAME,
    "VK_KHR_video_maintenance1",
    "VK_KHR_video_maintenance2",
    VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME,
    VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME,
    VK_EXT_GLOBAL_PRIORITY_QUERY_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
    "VK_KHR_external_memory_fd",
    VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME,
    VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME,
    VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME,
};

void probeDevice(const Vulkan& vk, VkPhysicalDevice pd, uint32_t index, const Options& o, Json& j)
{
    Device d{vk, pd, deviceExtensions(vk, pd), 0};

    VkPhysicalDeviceDriverProperties driver = {};
    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    VkPhysicalDeviceDrmPropertiesEXT drm = {};
    drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
    VkPhysicalDeviceProperties2 props = {};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    void* head = nullptr;
    pushNext(head, driver);
    const bool hasDrm = d.has(VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME);
    if (hasDrm) pushNext(head, drm);
    props.pNext = head;
    vk.vkGetPhysicalDeviceProperties2(pd, &props);
    const VkPhysicalDeviceProperties& p = props.properties;
    d.apiVersion = p.apiVersion;

    say("\nGPU %u: %s — %s, %s, API %s, %04x:%04x, %s\n", index, p.deviceName, driver.driverName,
        driver.driverInfo, vkVersionText(p.apiVersion).c_str(), p.vendorID, p.deviceID,
        deviceTypeName(p.deviceType));
    j.beginObject();
    j.field("index", index);
    j.field("name", p.deviceName);
    j.field("driverName", driver.driverName);
    j.field("driverInfo", driver.driverInfo);
    j.field("driverId", static_cast<int>(driver.driverID));
    j.field("apiVersion", vkVersionText(p.apiVersion));
    j.field("driverVersion", hex(p.driverVersion));
    j.field("vendorId", hex(p.vendorID));
    j.field("deviceId", hex(p.deviceID));
    j.field("type", deviceTypeName(p.deviceType));
    std::string card;
    if (hasDrm) {
        if (drm.hasPrimary) card = cardFor(drm.primaryMajor, drm.primaryMinor);
        say("  DRM: primary %lld:%lld (%s), render %lld:%lld\n",
            static_cast<long long>(drm.primaryMajor), static_cast<long long>(drm.primaryMinor),
            card.empty() ? "?" : card.c_str(), static_cast<long long>(drm.renderMajor),
            static_cast<long long>(drm.renderMinor));
        j.key("drm").beginObject();
        j.field("primary",
                std::to_string(drm.primaryMajor) + ":" + std::to_string(drm.primaryMinor));
        j.field("render", std::to_string(drm.renderMajor) + ":" + std::to_string(drm.renderMinor));
        j.field("card", card);
        j.endObject();
    }

    std::string present, absent;
    j.key("extensions").beginObject();
    for (const char* name : kWatchedExtensions) {
        const bool has = d.has(name);
        std::string& list = has ? present : absent;
        if (!list.empty()) list += ' ';
        list += name + 3; // without "VK_"
        j.field(name, has);
    }
    j.endObject();
    j.field("extensionCount", static_cast<unsigned long long>(d.extensions.size()));
    say("  has: %s\n  lacks: %s\n", present.c_str(), absent.empty() ? "-" : absent.c_str());

    // Queues, their priorities and clocks.
    const std::vector<Family> families = queueFamilies(d);
    say("  queues (timestamp period %.3f ns):\n", p.limits.timestampPeriod);
    j.field("timestampPeriodNs", static_cast<double>(p.limits.timestampPeriod));
    j.key("queueFamilies").beginArray();
    for (uint32_t f = 0; f < families.size(); ++f) {
        const Family& fam = families[f];
        std::string prios;
        for (const VkQueueGlobalPriority prio : fam.priorities) {
            if (!prios.empty()) prios += '|';
            prios += priorityName(prio);
        }
        say("    family %u: %s x%u, timestamps %u bits%s%s%s%s\n", f,
            decode(fam.props.queueFlags, kQueueFlags).c_str(), fam.props.queueCount,
            fam.props.timestampValidBits, fam.codecs ? ", " : "",
            fam.codecs ? decode(fam.codecs, kCodecOps).c_str() : "",
            fam.resultStatus ? ", result-status queries" : "",
            prios.empty() ? "" : (", priorities " + prios).c_str());
        j.beginObject();
        j.field("index", f);
        j.field("flags", decode(fam.props.queueFlags, kQueueFlags));
        j.field("count", fam.props.queueCount);
        j.field("timestampBits", fam.props.timestampValidBits);
        j.field("codecs", decode(fam.codecs, kCodecOps));
        j.field("resultStatus", fam.resultStatus);
        j.field("priorities", prios);
        j.endObject();
    }
    j.endArray();

    std::string domains;
    j.key("timeDomains").beginArray();
    PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR getDomains =
        d.has(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME)
            ? vk.vkGetPhysicalDeviceCalibrateableTimeDomainsKHR
            : (d.has(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME)
                   ? vk.vkGetPhysicalDeviceCalibrateableTimeDomainsEXT
                   : nullptr);
    if (getDomains) {
        uint32_t count = 0;
        getDomains(pd, &count, nullptr);
        std::vector<VkTimeDomainKHR> list(count);
        getDomains(pd, &count, list.data());
        for (const VkTimeDomainKHR t : list) {
            if (!domains.empty()) domains += ", ";
            domains += timeDomainName(t);
            j.value(timeDomainName(t));
        }
    }
    j.endArray();
    say("  calibrateable clocks: %s\n", domains.empty() ? "none" : domains.c_str());

    // What a device actually gets, priority by priority.
    j.key("priorityGrants").beginArray();
    if (o.priority && d.globalPriority()) {
        say("  priorities granted (vkCreateDevice, one queue; CAP_SYS_NICE %s):\n",
            capEffective(kCapSysNice) ? "effective" : "not effective");
        const VkQueueGlobalPriority all[] = {
            VK_QUEUE_GLOBAL_PRIORITY_LOW, VK_QUEUE_GLOBAL_PRIORITY_MEDIUM,
            VK_QUEUE_GLOBAL_PRIORITY_HIGH, VK_QUEUE_GLOBAL_PRIORITY_REALTIME};
        for (uint32_t f = 0; f < families.size(); ++f) {
            const Family& fam = families[f];
            if (!(fam.props.queueFlags &
                  (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_VIDEO_ENCODE_BIT_KHR)))
                continue;
            std::string line;
            j.beginObject();
            j.field("family", f);
            for (const VkQueueGlobalPriority prio : all) {
                const VkResult r = tryPriority(d, f, fam, prio);
                if (!line.empty()) line += ", ";
                line += std::string(priorityName(prio)) + " " +
                        (r == VK_SUCCESS ? "ok" : vkResultName(r));
                j.field(priorityName(prio), vkResultName(r));
            }
            j.endObject();
            say("    family %u (%s): %s\n", f, decode(fam.props.queueFlags, kQueueFlags).c_str(),
                line.c_str());
        }
    } else {
        say("  priorities granted: %s\n", o.priority ? "no global priority extension" : "skipped");
    }
    j.endArray();

    // The encoders.
    say("  encoders:\n");
    j.key("encode").beginArray();
    for (const Profile& prof : kProfiles)
        encodeProfile(d, prof, j);
    j.endArray();

    planeFormats(d, j);

    std::vector<std::vector<Importable>> tables;
    dmabuf(d, tables, j);
    if (hasDrm) kms(card, tables, j);
    semaphores(d, j);
    j.endObject();
}

bool parse(int argc, char** argv, Options& o)
{
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string& out) {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        if (a == "--json") {
            if (!next(o.jsonPath)) return false;
        } else if (a == "--no-json") {
            o.json = false;
        } else if (a == "--software") {
            o.software = true;
        } else if (a == "--no-priority") {
            o.priority = false;
        } else if (a == "--device") {
            if (!next(o.device)) return false;
        } else {
            say("unknown option %s\n", a.c_str());
            return false;
        }
    }
    return true;
}

} // namespace

void capsUsage()
{
    say("mw-vk-lab caps [--device <index|name>] [--software] [--no-priority]\n"
        "               [--json <path> | --no-json]\n"
        "  What every Vulkan device answers: Vulkan Video encode capabilities per codec,\n"
        "  queue families, the global priorities granted (run it as a user and as root, or\n"
        "  with CAP_SYS_NICE, to see the difference), timestamps, DMA-BUF import by\n"
        "  modifier and what the KMS planes scan out, sync_file semaphores.\n"
        "  --software  also probe CPU devices (lavapipe)\n"
        "  The JSON goes to vk-caps-<host>-<date>.json unless --json says otherwise.\n"
        "  Mesa's RADV hides its encoder behind RADV_PERFTEST=video_encode when the VCN\n"
        "  firmware is older than it trusts; the environment is recorded.\n");
}

int runCaps(int argc, char** argv)
{
    Options o;
    if (!parse(argc, argv, o)) {
        capsUsage();
        return 2;
    }
    Vulkan vk;
    std::string error;
    if (!vk.open(VK_API_VERSION_1_4, error)) {
        say("mw-vk-lab caps: %s\n", error.c_str());
        return 1;
    }

    const char* const watchedEnv[] = {
        "VK_DRIVER_FILES", "VK_ICD_FILENAMES", "VK_LOADER_LAYERS_ENABLE", "RADV_PERFTEST",
        "RADV_DEBUG",      "ANV_DEBUG",        "MESA_VK_DEVICE_SELECT",   "LD_LIBRARY_PATH"};
    Json j;
    j.beginObject();
    j.field("tool", "mw-vk-lab caps");
    j.field("date", nowText());
    j.field("host", hostName());
    j.field("kernel", kernelRelease());
    j.field("os", osName());
    j.field("user", userText());
    j.field("capSysNiceEffective", capEffective(kCapSysNice));
    j.field("capSysNicePermitted", capPermitted(kCapSysNice));
    j.field("capSysAdminEffective", capEffective(kCapSysAdmin));
    j.key("env").beginObject();
    std::string envLine;
    for (const char* name : watchedEnv) {
        const std::string value = env(name);
        if (value.empty()) continue;
        j.field(name, value);
        envLine += std::string("\n  ") + name + "=" + value;
    }
    j.endObject();
    j.key("loader").beginObject();
    j.field("path", vk.libraryPath());
    j.field("version", vkVersionText(vk.loaderVersion()));
    j.field("instanceVersion", vkVersionText(vk.instanceVersion()));
    j.field("headers", vkVersionText(VK_HEADER_VERSION_COMPLETE));
    j.endObject();

    say("mw-vk-lab caps — %s, %s\n", hostName().c_str(), nowText().c_str());
    say("  %s, kernel %s, %s; CAP_SYS_NICE %s, CAP_SYS_ADMIN %s\n", osName().c_str(),
        kernelRelease().c_str(), userText().c_str(),
        capEffective(kCapSysNice)   ? "effective"
        : capPermitted(kCapSysNice) ? "permitted only"
                                    : "no",
        capEffective(kCapSysAdmin)   ? "effective"
        : capPermitted(kCapSysAdmin) ? "permitted only"
                                     : "no");
    say("  loader %s (%s), instance %s, headers %s%s\n", vkVersionText(vk.loaderVersion()).c_str(),
        vk.libraryPath().c_str(), vkVersionText(vk.instanceVersion()).c_str(),
        vkVersionText(VK_HEADER_VERSION_COMPLETE).c_str(), envLine.c_str());

    uint32_t count = 0;
    vk.vkEnumeratePhysicalDevices(vk.instance(), &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vk.vkEnumeratePhysicalDevices(vk.instance(), &count, devices.data());
    j.key("devices").beginArray();
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties2 props = {};
        props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        vk.vkGetPhysicalDeviceProperties2(devices[i], &props);
        if (props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU && !o.software) {
            say("\nGPU %u: %s — CPU device, skipped (--software)\n", i,
                props.properties.deviceName);
            continue;
        }
        if (!matchesDevice(o.device, i, props.properties.deviceName)) continue;
        probeDevice(vk, devices[i], i, o, j);
    }
    j.endArray();
    j.endObject();

    if (o.json) {
        const std::string path =
            o.jsonPath.empty() ? "vk-caps-" + hostName() + "-" + nowStamp() + ".json" : o.jsonPath;
        std::ofstream out(path);
        out << j.str() << '\n';
        say("\nJSON: %s%s\n", path.c_str(), out ? "" : " (NOT WRITTEN)");
    }
    return 0;
}

} // namespace lab
