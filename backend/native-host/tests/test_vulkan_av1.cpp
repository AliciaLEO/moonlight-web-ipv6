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

// AV1 through Vulkan Video and its pixel proof (plan Phase 13, C13.12): the
// HEVC chain's rules (test_vulkan_hevc.cpp) for the one AV1 encoder Linux has.
// Real GPU: skipped, and said, where the driver shows no AV1 encoder — which
// the verdict must then refuse by name. MW_AV1_DUMP=<file> writes the stream
// of the first section out, for ffmpeg or dav1d to read on the bench.

#include "native_test_framework.h"

#if defined(MW_NATIVE_LINUX_VULKAN)
#include "encode/Av1Obu.h"
#include "encode/linux/Dav1dDecoder.h"
#include "encode/linux/VulkanAv1Encoder.h"
#include "encode/linux/VulkanHevcProof.h"
#include "platform/linux/vulkan/VulkanDevice.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#endif

#include <cstdio>
#include <string>
#include <vector>

#if defined(MW_NATIVE_LINUX_VULKAN)
namespace {

using namespace mw::native;

bool findRenderNode(std::string& node, vulkan::DeviceIdentity& id)
{
    for (int minor = 128; minor < 136; ++minor) {
        const std::string path = "/dev/dri/renderD" + std::to_string(minor);
        std::string error;
        if (vulkan::VulkanDevice::identify(path, id, error)) {
            node = path;
            return true;
        }
    }
    return false;
}

bool contains(const std::string& text, const char* piece)
{
    return text.find(piece) != std::string::npos;
}

/// The types of the OBUs of one picture, "2 1 6": what a receiver is handed.
std::string obuTypes(const uint8_t* data, size_t size)
{
    std::string out;
    for (const encode::av1::Obu& u : encode::av1::obus(data, size))
        out += (out.empty() ? "" : " ") + std::to_string(static_cast<int>(u.type));
    return out;
}

/// The frame header of a picture, read with @p seq.
bool frameOf(const uint8_t* data, size_t size, const encode::av1::Sequence& seq,
             encode::av1::FrameHeader& header, std::string& why)
{
    for (const encode::av1::Obu& u : encode::av1::obus(data, size))
        if (u.type == encode::av1::ObuType::Frame || u.type == encode::av1::ObuType::FrameHeader) {
            why = encode::av1::parseFrameHeader(u.payload, u.size, seq, header);
            return why.empty();
        }
    why = "no frame OBU";
    return false;
}

} // namespace
#endif

void run_vulkan_av1_tests()
{
#if !defined(MW_NATIVE_LINUX_VULKAN)
    SECTION("Vulkan Video — AV1 through Vulkan and its pixel proof");
    std::fprintf(stderr, "  skipped: the Vulkan chain is not built\n");
#else
    SECTION("Vulkan Video AV1 — the driver says whether it has an encoder, and a refusal says why");
    std::string node;
    vulkan::DeviceIdentity id;
    if (!findRenderNode(node, id)) {
        std::fprintf(stderr, "  skipped: no Vulkan device behind any render node\n");
        return;
    }
    std::fprintf(stderr, "  %s: %s, AV1 encoder %s, dav1d %s\n", node.c_str(), id.name.c_str(),
                 id.encodesAv1 ? "yes" : "no",
                 encode::Dav1dDecoder::built() ? "built in" : "not built in");
    ::setenv("MW_VK_PROOF_CACHE", "0", 1);
    EncoderTuning tuning;
    if (!id.encodesAv1) {
        const encode::VulkanHevcProof proof =
            encode::vulkanAv1Verdict(node, 1920, 1080, 60, tuning);
        CHECK(!proof.ran);
        CHECK(!proof.passed);
        CHECK(contains(proof.summary, "shows no Vulkan Video AV1 encoder"));
        std::fprintf(stderr, "  refused: %s\n", proof.summary.c_str());
        ::unsetenv("MW_VK_PROOF_CACHE");
        return;
    }

    SECTION("VulkanAv1Encoder — the driver's sequence header, a key frame that carries it, "
            "inter frames on the slot asked, a repair, a new bitrate");
    {
        vulkan::DeviceOptions options;
        options.wantHigh = false;
        options.encodeAv1 = true;
        std::string error;
        std::shared_ptr<vulkan::VulkanDevice> device =
            vulkan::VulkanDevice::open(node, options, error);
        if (!device) std::fprintf(stderr, "  device: %s\n", error.c_str());
        CHECK(device != nullptr);
        if (device) {
            encode::VulkanAv1Encoder encoder;
            const bool ok = encoder.init(device, Codec::Av1, 1920, 1080, 60, 20000,
                                         /*intraRefresh=*/false, tuning, error);
            if (!ok) std::fprintf(stderr, "  init: %s\n", error.c_str());
            CHECK(ok);
            if (ok) {
                const encode::av1::Sequence& seq = encoder.sequence();
                std::fprintf(stderr,
                             "  sequence: %ux%u, level idx %d, sb%s, order hints %d, cdef %d, "
                             "restoration %d, %zu bytes\n",
                             seq.width, seq.height, seq.levelIdx, seq.sb128 ? "128" : "64",
                             seq.orderHintBits, seq.cdef ? 1 : 0, seq.restoration ? 1 : 0,
                             encoder.sequenceHeader().size());
                CHECK(seq.width >= 1920u && seq.height >= 1080u);
                CHECK_EQ(seq.orderHintBits, 8);
                CHECK(encoder.input().image != VK_NULL_HANDLE);
                const char* dumpPath = std::getenv("MW_AV1_DUMP");
                std::ofstream dump;
                if (dumpPath && *dumpPath) dump.open(dumpPath, std::ios::binary | std::ios::trunc);
                std::vector<uint8_t> picture(1920 * 1080 * 3 / 2, 128);
                auto paint = [&](uint32_t n) {
                    for (size_t i = 0; i < 1920u * 1080u; ++i)
                        picture[i] = static_cast<uint8_t>(16 + (i % 1920 + i / 1920 + n * 7) % 200);
                };
                encode::EncoderOutput out;
                std::string why;
                // 0: the key frame, with the sequence header behind its
                // temporal delimiter.
                paint(0);
                CHECK(encoder.upload(picture.data(), error));
                CHECK(encoder.encode(false, 0, out, error));
                CHECK(out.keyframe);
                const std::string keyTypes = obuTypes(out.data, out.size);
                std::fprintf(stderr, "  key frame: %zu bytes, OBUs %s, qindex %d\n", out.size,
                             keyTypes.c_str(), out.avgQp);
                CHECK(keyTypes.rfind("2 1 ", 0) == 0);
                encode::av1::FrameHeader header;
                CHECK(frameOf(out.data, out.size, seq, header, why));
                CHECK_EQ(header.start.frameType, 0);
                if (dump) dump.write(reinterpret_cast<const char*>(out.data), out.size);
                encoder.releaseOutput();
                // 1, 2: inter frames, a temporal delimiter each and no
                // sequence header, predicting from the previous picture.
                for (uint32_t n = 1; n <= 2; ++n) {
                    paint(n);
                    CHECK(encoder.upload(picture.data(), error));
                    CHECK(encoder.encode(false, n, out, error));
                    CHECK(!out.keyframe);
                    const std::string types = obuTypes(out.data, out.size);
                    CHECK(types.rfind("2 ", 0) == 0);
                    CHECK(!contains(" " + types + " ", " 1 "));
                    CHECK(!contains(" " + types + " ", " 15 ")); // no padding out
                    CHECK(frameOf(out.data, out.size, seq, header, why));
                    CHECK_EQ(header.start.frameType, 1);
                    CHECK_EQ(header.start.orderHint, n);
                    std::fprintf(stderr, "  inter %u: %zu bytes, qindex %d, refs %d\n", n, out.size,
                                 out.avgQp, header.start.refFrameIdx[0]);
                    if (dump) dump.write(reinterpret_cast<const char*>(out.data), out.size);
                    encoder.releaseOutput();
                }
                // 2 lost: 3 predicts from 1, still a delta.
                CHECK(encoder.supportsReferenceInvalidation());
                CHECK(encoder.invalidateReference(2, error));
                paint(3);
                CHECK(encoder.upload(picture.data(), error));
                CHECK(encoder.encode(false, 3, out, error));
                CHECK(!out.keyframe);
                if (dump) dump.write(reinterpret_cast<const char*>(out.data), out.size);
                encoder.releaseOutput();
                CHECK(encoder.setBitrate(10000, error));
                paint(4);
                CHECK(encoder.upload(picture.data(), error));
                CHECK(encoder.encode(false, 4, out, error));
                if (dump) dump.write(reinterpret_cast<const char*>(out.data), out.size);
                encoder.releaseOutput();
                // An unchanged picture: what a still desktop costs.
                CHECK(encoder.upload(picture.data(), error));
                CHECK(encoder.encode(false, 5, out, error));
                std::fprintf(stderr, "  an unchanged picture: %zu bytes, qindex %d\n", out.size,
                             out.avgQp);
                if (dump) dump.write(reinterpret_cast<const char*>(out.data), out.size);
                encoder.releaseOutput();
                std::fprintf(stderr, "  %s\n", encoder.describe().c_str());
                CHECK(!encoder.lost());
                CHECK(!encoder.intraRefreshEnabled());
            }
        }
    }

    SECTION("VulkanAv1Encoder — the rate: a picture full of detail at 3 Mbit/s keeps to it, "
            "and to 1.5 when told in flight");
    {
        vulkan::DeviceOptions options;
        options.wantHigh = false;
        options.encodeAv1 = true;
        std::string error;
        std::shared_ptr<vulkan::VulkanDevice> device =
            vulkan::VulkanDevice::open(node, options, error);
        encode::VulkanAv1Encoder encoder;
        const bool ok =
            device && encoder.init(device, Codec::Av1, 1920, 1080, 60, 3000, false, tuning, error);
        if (!ok) std::fprintf(stderr, "  init: %s\n", error.c_str());
        CHECK(ok);
        if (ok) {
            // A ramp that moves under a little noise: more than 3 Mbit/s can
            // carry, so the rate control decides each picture's size. (Heavy
            // noise beats RADV's VBR: 4.5 Mbit/s where qindex 255 alone gives
            // 2.6 — measured 04/10/2026, bench §8o.18.)
            std::vector<uint8_t> picture(1920 * 1080 * 3 / 2, 128);
            uint32_t seed = 1;
            size_t bytes[2] = {0, 0};
            for (uint32_t n = 0; n < 120; ++n) {
                for (size_t i = 0; i < 1920u * 1080u; ++i) {
                    seed = seed * 1103515245u + 12345u;
                    picture[i] =
                        static_cast<uint8_t>(16 + ((i % 1920) + n * 13 + ((seed >> 16) & 3)) % 200);
                }
                if (n == 60) CHECK(encoder.setBitrate(1500, error));
                CHECK(encoder.upload(picture.data(), error));
                encode::EncoderOutput out;
                const bool encoded = encoder.encode(false, n, out, error);
                CHECK(encoded);
                if (!encoded) break;
                // Past the key frame and the first half second of each rate.
                if (n >= 30 && n < 60) bytes[0] += out.size;
                if (n >= 90) bytes[1] += out.size;
                encoder.releaseOutput();
            }
            const double first = bytes[0] * 8.0 * 60 / 30 / 1e6;
            const double second = bytes[1] * 8.0 * 60 / 30 / 1e6;
            std::fprintf(stderr, "  %.2f Mbit/s asked 3, then %.2f asked 1.5\n", first, second);
            CHECK(first > 1.5 && first < 4.5);
            CHECK(second > 0.75 && second < 2.25);
            CHECK(second < first);
        }
    }

    SECTION("VulkanAv1Encoder — intra refresh: the stream's sweeps, and a repair in one");
    {
        vulkan::DeviceOptions options;
        options.wantHigh = false;
        options.encodeAv1 = true;
        std::string error;
        std::shared_ptr<vulkan::VulkanDevice> device =
            vulkan::VulkanDevice::open(node, options, error);
        CHECK(device != nullptr);
        if (device && !device->encodesIntraRefresh()) {
            std::fprintf(stderr,
                         "  skipped: this driver has no VK_KHR_video_encode_intra_refresh\n");
        } else if (device) {
            encode::VulkanAv1Encoder::Witness witness;
            witness.sweepPictures = 4;
            encode::VulkanAv1Encoder encoder;
            const bool ok = encoder.init(device, Codec::Av1, 1280, 720, 60, 20000, true, tuning,
                                         error, witness);
            if (!ok) std::fprintf(stderr, "  init: %s\n", error.c_str());
            CHECK(ok);
            if (ok) {
                std::fprintf(stderr, "  sweeps: %s\n",
                             encoder.intraRefreshEnabled() ? "yes" : "no (keyframes on demand)");
                std::vector<uint8_t> picture(1280 * 720 * 3 / 2, 128);
                int keyframes = 0;
                for (uint32_t n = 0; n < 14; ++n) {
                    for (size_t i = 0; i < 1280 * 720; ++i)
                        picture[i] = static_cast<uint8_t>(16 + (i / 1280 + n * 9) % 200);
                    CHECK(encoder.upload(picture.data(), error));
                    if (n == 7) CHECK(encoder.invalidateReference(5, error));
                    encode::EncoderOutput out;
                    const bool encoded = encoder.encode(false, n, out, error);
                    if (!encoded) std::fprintf(stderr, "  picture %u: %s\n", n, error.c_str());
                    CHECK(encoded);
                    if (!encoded) break;
                    keyframes += out.keyframe ? 1 : 0;
                    encoder.releaseOutput();
                }
                CHECK_EQ(keyframes, 1);
                CHECK(!encoder.lost());
            }
        }
    }

    SECTION("Vulkan Video AV1 pixel proof — the product's encoder decodes back to what went in");
    if (!encode::Dav1dDecoder::built()) {
        const encode::VulkanHevcProof proof =
            encode::vulkanAv1Verdict(node, 1920, 1080, 60, tuning);
        CHECK(!proof.passed);
        CHECK(contains(proof.summary, "no AV1 decoder for the pixel proof"));
        std::fprintf(stderr, "  refused: %s\n", proof.summary.c_str());
    } else {
        for (const auto& size : {std::pair<int, int>{1920, 1080}, std::pair<int, int>{1280, 720}}) {
            const encode::VulkanHevcProof proof =
                encode::proveVulkanAv1(node, size.first, size.second, 60, tuning);
            std::fprintf(stderr, "  %dx%d: %s, %s in %lld ms — %s\n", size.first, size.second,
                         proof.ran ? "ran" : "did not run", proof.passed ? "passed" : "FAILED",
                         static_cast<long long>(proof.tookMs), proof.summary.c_str());
            CHECK(proof.ran);
            CHECK(proof.passed);
            CHECK_EQ(proof.pictures, 10); // twelve encoded, two lost on the way
        }

        SECTION("Vulkan Video AV1 pixel proof — the coarsest constant qindex, for the record: "
                "how far the floors sit under a stream that is merely poor");
        encode::VulkanEncodeWitness coarse;
        coarse.constantQp = 255;
        const encode::VulkanHevcProof poor =
            encode::proveVulkanAv1(node, 1280, 720, 60, tuning, coarse);
        std::fprintf(stderr, "  qindex 255: %s — %s\n", poor.passed ? "passed" : "failed",
                     poor.summary.c_str());

        SECTION("Vulkan Video AV1 pixel proof — the verdict kept in the user's cache, apart from "
                "HEVC's");
        char dir[] = "/tmp/mw-vk-av1-proof-XXXXXX";
        if (::mkdtemp(dir)) {
            ::setenv("XDG_CACHE_HOME", dir, 1);
            ::unsetenv("MW_VK_PROOF_CACHE");
            const encode::VulkanHevcProof first =
                encode::vulkanAv1Verdict(node, 1920, 1080, 60, tuning);
            const encode::VulkanHevcProof second =
                encode::vulkanAv1Verdict(node, 1920, 1080, 60, tuning);
            CHECK(!first.cached);
            CHECK(second.cached);
            CHECK_EQ(second.passed, first.passed);
            const std::string file = std::string(dir) + "/MoonlightWeb/vulkan-video-proofs.txt";
            std::ifstream in(file);
            std::string line;
            bool av1Key = false;
            while (std::getline(in, line))
                av1Key = av1Key || line.rfind("av1|", 0) == 0;
            CHECK(av1Key);
            ::unlink(file.c_str());
            ::rmdir((std::string(dir) + "/MoonlightWeb").c_str());
            ::rmdir(dir);
            ::unsetenv("XDG_CACHE_HOME");
        }
    }
    ::unsetenv("MW_VK_PROOF_CACHE");
#endif
}
