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

// The choice of a Linux session's picture chain (plan Phase 13): what decides
// (bench key, setting, vendor table), what rules a chain out, and the rule that
// frames all of it — whatever cannot run reliably drops to the chain below and
// says why, it is never forced (Bruno, 28/09/2026).

#include "core/LinuxRouteChoice.h"
#include "native_test_framework.h"

#include <cstdio>
#include <string>

using namespace mw::native;

namespace {

/// The UM790Pro: a Radeon 780M encoding HEVC through VA-API off a KMS scanout,
/// the Vulkan conversion built in.
LinuxRouteFacts amd()
{
    LinuxRouteFacts f;
    f.encoder = EncoderApi::VaApi;
    f.codec = Codec::Hevc;
    f.vendorId = 0x1002;
    f.vulkanConvertBuilt = true;
    return f;
}

/// A Vulkan Video encoder built, and trusted on this GPU.
LinuxRouteFacts amdWithVulkanEncoder()
{
    LinuxRouteFacts f = amd();
    f.vulkanEncoderBuilt = true;
    return f;
}

bool contains(const std::string& text, const std::string& piece)
{
    return text.find(piece) != std::string::npos;
}

} // namespace

void run_linux_route_choice_tests()
{
    SECTION("LinuxRoute — the vendor table: Vulkan Video on AMD (§32.25), VA-API elsewhere; in "
            "front of VA-API, Vulkan compute on AMD (§9-20), GL elsewhere");
    {
        CHECK(autoLinuxPipeline(0x1002) == VideoPipeline::Vulkan);
        for (uint32_t vendor : {0x8086u, 0x10DEu, 0u})
            CHECK(autoLinuxPipeline(vendor) == VideoPipeline::Vaapi);
        CHECK(autoLinuxConversion(0x1002) == EncoderTuning::ConvertLinux::Vulkan);
        for (uint32_t vendor : {0x8086u, 0x10DEu, 0u})
            CHECK(autoLinuxConversion(vendor) == EncoderTuning::ConvertLinux::Gl);

        // A build without the Vulkan Video encoder: the table's VA-API, by
        // the table's own line — nothing refused.
        LinuxRoute r = chooseLinuxRoute(amd());
        CHECK(r.encoder == LinuxRoute::Encoder::Vaapi);
        CHECK(r.conversion == LinuxRoute::Conversion::Vulkan);
        CHECK(r.pipeline == VideoPipeline::Vaapi);
        CHECK_EQ(r.route, std::string("Vulkan compute → VA-API"));
        CHECK(!r.refused);
        CHECK(contains(r.reason, "vendor table has Vulkan Video for AMD, not built in here"));
        CHECK(contains(r.reason, "converts with Vulkan compute for AMD"));

        // Intel: GL, as it always was.
        LinuxRouteFacts intel = amd();
        intel.vendorId = 0x8086;
        r = chooseLinuxRoute(intel);
        CHECK(r.conversion == LinuxRoute::Conversion::Gl);
        CHECK_EQ(r.route, std::string("EGL → VA-API"));
        CHECK(!r.refused);
        CHECK(contains(r.reason, "converts with GL for Intel"));
    }

    SECTION("LinuxRoute — AMD in auto, a Vulkan that cannot convert: GL, and the reason says "
            "why");
    {
        LinuxRouteFacts f = amd();
        f.vulkanConvertBuilt = false;
        LinuxRoute r = chooseLinuxRoute(f);
        CHECK(r.encoder == LinuxRoute::Encoder::Vaapi);
        CHECK(r.conversion == LinuxRoute::Conversion::Gl);
        CHECK_EQ(r.route, std::string("EGL → VA-API"));
        CHECK(r.refused);
        CHECK(contains(r.reason, "the vendor table for AMD asks for Vulkan compute, GL converts: "
                                 "the Vulkan conversion is not built in"));

        // What the session learns — at the start, or while streaming — comes
        // back as a refusal, and GL converts for the rest of it.
        f.vulkanConvertBuilt = true;
        for (const char* learned :
             {"the Vulkan conversion could not start (no libvulkan.so.1)",
              "the Vulkan conversion gave up while streaming (VK_ERROR_DEVICE_LOST)"}) {
            f.vulkanConvertRefusal = learned;
            r = chooseLinuxRoute(f);
            CHECK(r.conversion == LinuxRoute::Conversion::Gl);
            CHECK_EQ(r.route, std::string("EGL → VA-API"));
            CHECK(r.refused);
            CHECK(contains(r.reason, std::string("GL converts: ") + learned));
        }
    }

    SECTION("LinuxRoute — AMD through the portal's DMA-BUF: the split route too, since its bench "
            "(C13.3 bis)");
    {
        LinuxRouteFacts f = amd();
        f.portal = true;
        LinuxRoute r = chooseLinuxRoute(f);
        CHECK(r.conversion == LinuxRoute::Conversion::Vulkan);
        CHECK_EQ(r.route, std::string("Vulkan compute → VA-API"));
        CHECK(!r.refused);
        CHECK(contains(r.reason, "converts with Vulkan compute for AMD on the portal"));
        // The bench key still measures GL there.
        f.convertKey = EncoderTuning::ConvertLinux::Gl;
        r = chooseLinuxRoute(f);
        CHECK(r.conversion == LinuxRoute::Conversion::Gl);
        CHECK(!r.refused);
    }

    SECTION("LinuxRoute — VA-API by name is the chain as it always ran, GL in front: the way "
            "back from the table");
    {
        for (bool bench : {false, true}) {
            LinuxRouteFacts f = amd();
            (bench ? f.benchKey : f.setting) = VideoPipeline::Vaapi;
            LinuxRoute r = chooseLinuxRoute(f);
            CHECK(r.encoder == LinuxRoute::Encoder::Vaapi);
            CHECK(r.conversion == LinuxRoute::Conversion::Gl);
            CHECK_EQ(r.route, std::string("EGL → VA-API"));
            CHECK(!r.refused);
            CHECK(contains(r.reason, bench ? "pipeline=vaapi" : "the setting (vaapi)"));
            CHECK(contains(r.reason, "VA-API by name converts with GL"));
            // convert= over it: the bench measures the split route either way.
            f.convertKey = EncoderTuning::ConvertLinux::Vulkan;
            r = chooseLinuxRoute(f);
            CHECK(r.conversion == LinuxRoute::Conversion::Vulkan);
            CHECK(contains(r.reason, "convert=vulkan"));
        }
    }

    SECTION("LinuxRoute — the bench key first: convert=gl over the table, pipeline= over the "
            "setting");
    {
        LinuxRouteFacts f = amd();
        f.convertKey = EncoderTuning::ConvertLinux::Gl;
        LinuxRoute r = chooseLinuxRoute(f);
        CHECK(r.conversion == LinuxRoute::Conversion::Gl);
        CHECK(!r.refused);
        CHECK(contains(r.reason, "convert=gl"));

        // pipeline=vulkan over a setting of vaapi: Vulkan Video, not built
        // here, refused — and that stream takes the table's conversion, for
        // nobody asked for VA-API by name.
        f = amd();
        f.setting = VideoPipeline::Vaapi;
        f.benchKey = VideoPipeline::Vulkan;
        r = chooseLinuxRoute(f);
        CHECK(r.encoder == LinuxRoute::Encoder::Vaapi);
        CHECK(r.conversion == LinuxRoute::Conversion::Vulkan);
        CHECK(r.refused);
        CHECK(contains(r.reason, "pipeline=vulkan asks for Vulkan Video"));
    }

    SECTION("LinuxRoute — auto takes Vulkan Video on AMD alone, trusted (Bruno, 05/10/2026, "
            "§32.25)");
    {
        for (uint32_t vendor : {0x8086u, 0x10DEu, 0u}) {
            LinuxRouteFacts f = amdWithVulkanEncoder();
            f.vendorId = vendor;
            CHECK(!linuxRouteWantsVulkanVideo(f));
            const LinuxRoute r = chooseLinuxRoute(f);
            CHECK(r.encoder == LinuxRoute::Encoder::Vaapi);
            CHECK(r.pipeline == VideoPipeline::Vaapi);
            CHECK(!r.refused);
        }

        LinuxRouteFacts f = amdWithVulkanEncoder();
        CHECK(linuxRouteWantsVulkanVideo(f));
        LinuxRoute r = chooseLinuxRoute(f);
        CHECK(r.encoder == LinuxRoute::Encoder::Vulkan);
        CHECK(r.conversion == LinuxRoute::Conversion::Vulkan);
        CHECK(r.pipeline == VideoPipeline::Vulkan);
        CHECK_EQ(r.route, std::string("Vulkan compute → Vulkan Video"));
        CHECK(!r.refused);
        CHECK(contains(r.reason, "auto: the vendor table has Vulkan Video for AMD"));

        // AV1 too, its only encoder here.
        f.codec = Codec::Av1;
        CHECK(linuxRouteWantsVulkanVideo(f));
        CHECK(chooseLinuxRoute(f).encoder == LinuxRoute::Encoder::Vulkan);

        // H.264, which the chain does not encode: VA-API by the table's own
        // line, behind the split route — no proof asked, nothing refused.
        f.codec = Codec::H264;
        CHECK(!linuxRouteWantsVulkanVideo(f));
        r = chooseLinuxRoute(f);
        CHECK(r.encoder == LinuxRoute::Encoder::Vaapi);
        CHECK_EQ(r.route, std::string("Vulkan compute → VA-API"));
        CHECK(!r.refused);
        CHECK(contains(r.reason, "Vulkan Video for AMD in HEVC and AV1, VA-API in H.264"));

        // The portal's shared memory: the chain converts in Vulkan, it reads it.
        f = amdWithVulkanEncoder();
        f.portal = true;
        f.sharedMemory = true;
        CHECK(chooseLinuxRoute(f).encoder == LinuxRoute::Encoder::Vulkan);

        // A VA-API that writes no parameter sets (the 610M): the chain, no CPU.
        f = amdWithVulkanEncoder();
        f.vaapiUnusable = true;
        CHECK(chooseLinuxRoute(f).encoder == LinuxRoute::Encoder::Vulkan);

        // The way back from the table: VA-API by name, GL in front, as always.
        f = amdWithVulkanEncoder();
        f.setting = VideoPipeline::Vaapi;
        CHECK(!linuxRouteWantsVulkanVideo(f));
        r = chooseLinuxRoute(f);
        CHECK_EQ(r.route, std::string("EGL → VA-API"));
        CHECK(!r.refused);
    }

    SECTION("LinuxRoute — AMD in auto, the chain not trusted: VA-API runs on its own, and the "
            "refusal is said");
    {
        for (const char* learned :
             {"the pixel proof failed: 4.9 dB after the first P picture",
              "the Vulkan Video chain gave up while streaming (device lost)"}) {
            LinuxRouteFacts f = amdWithVulkanEncoder();
            f.vulkanEncoderRefusal = learned;
            CHECK(!linuxRouteWantsVulkanVideo(f));
            LinuxRoute r = chooseLinuxRoute(f);
            CHECK(r.encoder == LinuxRoute::Encoder::Vaapi);
            CHECK(r.pipeline == VideoPipeline::Vaapi);
            // The table's conversion in front of it: nobody asked for VA-API by name.
            CHECK_EQ(r.route, std::string("Vulkan compute → VA-API"));
            CHECK(r.refused);
            CHECK(contains(r.reason, "auto: the vendor table for AMD asks for Vulkan Video, which "
                                     "cannot run: "));
            CHECK(contains(r.reason, learned));
            CHECK(contains(r.reason, "VA-API runs"));

            // And no VA-API either: the CPU, said.
            f.vaapiUnusable = true;
            r = chooseLinuxRoute(f);
            CHECK(r.encoder == LinuxRoute::Encoder::Cpu);
            CHECK(r.refused);
        }
    }

    SECTION("LinuxRoute — convert=vulkan: the split route, Vulkan compute into VA-API");
    {
        LinuxRouteFacts f = amd();
        f.convertKey = EncoderTuning::ConvertLinux::Vulkan;
        const LinuxRoute r = chooseLinuxRoute(f);
        CHECK(r.encoder == LinuxRoute::Encoder::Vaapi);
        CHECK(r.conversion == LinuxRoute::Conversion::Vulkan);
        CHECK_EQ(r.route, std::string("Vulkan compute → VA-API"));
        CHECK(!r.refused);
        CHECK(contains(r.reason, "convert=vulkan"));
    }

    SECTION("LinuxRoute — a Vulkan conversion that cannot run leaves GL converting, and says "
            "why");
    {
        LinuxRouteFacts f = amd();
        f.convertKey = EncoderTuning::ConvertLinux::Vulkan;
        f.vulkanConvertBuilt = false;
        LinuxRoute r = chooseLinuxRoute(f);
        CHECK(r.conversion == LinuxRoute::Conversion::Gl);
        CHECK_EQ(r.route, std::string("EGL → VA-API"));
        CHECK(r.refused);
        CHECK(contains(r.reason, "asks for Vulkan compute, GL converts: the Vulkan conversion "
                                 "is not built in"));

        f.vulkanConvertBuilt = true;
        f.vulkanConvertRefusal = "the Vulkan device was lost (VK_ERROR_DEVICE_LOST)";
        r = chooseLinuxRoute(f);
        CHECK(r.conversion == LinuxRoute::Conversion::Gl);
        CHECK(r.refused);
        CHECK(contains(r.reason, "VK_ERROR_DEVICE_LOST"));

        // Asked by nobody, the table's GL is no refusal, whatever Vulkan's state.
        f.convertKey = EncoderTuning::ConvertLinux::Default;
        f.vendorId = 0x8086;
        CHECK(!chooseLinuxRoute(f).refused);
    }

    SECTION("LinuxRoute — Vulkan Video asked for and not built: VA-API runs, said by name");
    {
        LinuxRouteFacts f = amd();
        f.setting = VideoPipeline::Vulkan;
        const LinuxRoute r = chooseLinuxRoute(f);
        CHECK(r.encoder == LinuxRoute::Encoder::Vaapi);
        CHECK(r.pipeline == VideoPipeline::Vaapi);
        CHECK(r.refused);
        CHECK(contains(r.reason, "the setting (vulkan) asks for Vulkan Video, which cannot run: "
                                 "the Vulkan Video encoder is not built in; VA-API runs"));
        // One step down, not two: the split route, the table's on AMD.
        CHECK(r.conversion == LinuxRoute::Conversion::Vulkan);
        CHECK_EQ(r.route, std::string("Vulkan compute → VA-API"));
    }

    SECTION("LinuxRoute — the Vulkan Video chain converts in Vulkan: what refused the conversion "
            "refuses the chain");
    {
        LinuxRouteFacts f = amdWithVulkanEncoder();
        f.setting = VideoPipeline::Vulkan;
        f.vulkanConvertRefusal = "the Vulkan conversion gave up while streaming (device lost)";
        LinuxRoute r = chooseLinuxRoute(f);
        CHECK(r.encoder == LinuxRoute::Encoder::Vaapi);
        CHECK(r.conversion == LinuxRoute::Conversion::Gl);
        CHECK(r.refused);
        CHECK(contains(r.reason, "gave up while streaming (device lost); VA-API runs"));

        f.vulkanConvertRefusal.clear();
        f.vulkanConvertBuilt = false;
        r = chooseLinuxRoute(f);
        CHECK(r.encoder == LinuxRoute::Encoder::Vaapi);
        CHECK(contains(r.reason, "the Vulkan conversion is not built in"));
    }

    SECTION("LinuxRoute — the pixel proof runs only where Vulkan Video would be taken");
    {
        // The vendor table has VA-API off AMD: nobody there is made to prove.
        LinuxRouteFacts f = amdWithVulkanEncoder();
        f.vendorId = 0x8086;
        CHECK(!linuxRouteWantsVulkanVideo(f));
        f.setting = VideoPipeline::Vulkan;
        CHECK(linuxRouteWantsVulkanVideo(f));
        f.setting = VideoPipeline::Auto;
        f.benchKey = VideoPipeline::Vulkan;
        CHECK(linuxRouteWantsVulkanVideo(f));
        // The bench key over the setting, as for the choice itself.
        f.setting = VideoPipeline::Vulkan;
        f.benchKey = VideoPipeline::Vaapi;
        CHECK(!linuxRouteWantsVulkanVideo(f));
        // Nothing to prove for a stream it would not carry, or once refused.
        f = amdWithVulkanEncoder();
        f.setting = VideoPipeline::Vulkan;
        f.codec = Codec::H264;
        CHECK(!linuxRouteWantsVulkanVideo(f));
        f.codec = Codec::Hevc;
        f.vulkanEncoderRefusal = "the pixel proof failed";
        CHECK(!linuxRouteWantsVulkanVideo(f));
        f = amd();
        f.setting = VideoPipeline::Vulkan;
        CHECK(!linuxRouteWantsVulkanVideo(f)); // not built in
    }

    SECTION("LinuxRoute — Bruno's rule: an encoder the firmware or the driver do not make "
            "reliable is never forced");
    {
        // What the 780M taught (bench §8o.3): the pixel proof is the verdict,
        // neither the firmware's number nor Mesa's version.
        const char* const verdicts[] = {
            "the pixel proof at its opening failed: 4.9 dB after the first P picture",
            "RADV exposes the encoder only through RADV_PERFTEST=video_encode here",
            "the driver has no encode queue for HEVC",
        };
        for (const char* verdict : verdicts) {
            for (bool bench : {false, true}) {
                LinuxRouteFacts f = amdWithVulkanEncoder();
                f.vulkanEncoderRefusal = verdict;
                (bench ? f.benchKey : f.setting) = VideoPipeline::Vulkan;
                const LinuxRoute r = chooseLinuxRoute(f);
                CHECK(r.encoder == LinuxRoute::Encoder::Vaapi);
                CHECK(r.pipeline == VideoPipeline::Vaapi);
                CHECK(r.refused);
                CHECK(contains(r.reason, verdict));
                CHECK(contains(r.reason, "VA-API runs"));
            }
        }
    }

    SECTION("LinuxRoute — Vulkan Video, trusted: the whole chain in Vulkan");
    {
        LinuxRouteFacts f = amdWithVulkanEncoder();
        f.benchKey = VideoPipeline::Vulkan;
        LinuxRoute r = chooseLinuxRoute(f);
        CHECK(r.encoder == LinuxRoute::Encoder::Vulkan);
        CHECK(r.conversion == LinuxRoute::Conversion::Vulkan);
        CHECK(r.pipeline == VideoPipeline::Vulkan);
        CHECK_EQ(r.route, std::string("Vulkan compute → Vulkan Video"));
        CHECK(!r.refused);

        // AV1 too (C13.12); H.264 comes later: VA-API carries it meanwhile.
        f.codec = Codec::Av1;
        r = chooseLinuxRoute(f);
        CHECK(r.encoder == LinuxRoute::Encoder::Vulkan);
        f.codec = Codec::H264;
        r = chooseLinuxRoute(f);
        CHECK(r.encoder == LinuxRoute::Encoder::Vaapi);
        CHECK(contains(r.reason, "H.264 is not done by the Vulkan Video encoder yet"));
    }

    SECTION("LinuxRoute — AV1 is Vulkan Video's alone, asked for by the bench key, the setting, "
            "or AMD's line of the vendor table");
    {
        CHECK(linuxVulkanOnlyCodec(Codec::Av1));
        CHECK(!linuxVulkanOnlyCodec(Codec::Hevc));
        CHECK(!linuxVulkanOnlyCodec(Codec::H264));
        CHECK(linuxWantedPipeline(VideoPipeline::Auto, VideoPipeline::Auto, 0x1002) ==
              VideoPipeline::Vulkan);
        CHECK(linuxWantedPipeline(VideoPipeline::Auto, VideoPipeline::Auto, 0x8086) ==
              VideoPipeline::Vaapi);
        CHECK(linuxWantedPipeline(VideoPipeline::Auto, VideoPipeline::Vaapi, 0x1002) ==
              VideoPipeline::Vaapi);
        CHECK(linuxWantedPipeline(VideoPipeline::Auto, VideoPipeline::Vulkan, 0x1002) ==
              VideoPipeline::Vulkan);
        CHECK(linuxWantedPipeline(VideoPipeline::Vaapi, VideoPipeline::Vulkan, 0x1002) ==
              VideoPipeline::Vaapi);
        CHECK(linuxWantedPipeline(VideoPipeline::Vulkan, VideoPipeline::Auto, 0x8086) ==
              VideoPipeline::Vulkan);
        // Windows' values are no opinion here: the table's.
        CHECK(linuxWantedPipeline(VideoPipeline::D3d12, VideoPipeline::Auto, 0x1002) ==
              VideoPipeline::Vulkan);
        CHECK(linuxWantedPipeline(VideoPipeline::D3d12, VideoPipeline::Auto, 0x8086) ==
              VideoPipeline::Vaapi);
    }

    SECTION("LinuxRoute — NVIDIA: no VA-API encoder, the CPU; Vulkan Video, once trusted, "
            "is its hardware encoder");
    {
        LinuxRouteFacts f;
        f.encoder = EncoderApi::Software;
        f.codec = Codec::Hevc;
        f.vendorId = 0x10DE;
        LinuxRoute r = chooseLinuxRoute(f);
        CHECK(r.encoder == LinuxRoute::Encoder::Cpu);
        CHECK(r.conversion == LinuxRoute::Conversion::Cpu);
        CHECK_EQ(r.route, std::string("CPU → OpenH264"));
        CHECK(!r.refused);
        CHECK(contains(r.reason, "no GPU encoder on this machine"));

        f.vulkanEncoderBuilt = true;
        f.vulkanConvertBuilt = true;
        f.setting = VideoPipeline::Vulkan;
        r = chooseLinuxRoute(f);
        CHECK(r.encoder == LinuxRoute::Encoder::Vulkan);

        f.vulkanEncoderRefusal = "the pixel proof at its opening failed";
        r = chooseLinuxRoute(f);
        CHECK(r.encoder == LinuxRoute::Encoder::Cpu);
        CHECK(r.refused);
        CHECK(contains(r.reason, "the pixel proof at its opening failed; the CPU runs"));
    }

    SECTION("LinuxRoute — the portal's shared memory: Vulkan compute where the table or the key "
            "asks for it (C13.10), the CPU where GL would convert");
    {
        // AMD: the table's Vulkan conversion reads it, into VA-API.
        LinuxRouteFacts f = amd();
        f.portal = true;
        f.sharedMemory = true;
        LinuxRoute r = chooseLinuxRoute(f);
        CHECK(r.encoder == LinuxRoute::Encoder::Vaapi);
        CHECK(r.conversion == LinuxRoute::Conversion::Vulkan);
        CHECK_EQ(r.route, std::string("Vulkan compute → VA-API"));
        CHECK(!r.refused);

        // The Vulkan Video chain as well, asked for and trusted: it converts
        // in Vulkan.
        f = amdWithVulkanEncoder();
        f.portal = true;
        f.sharedMemory = true;
        f.benchKey = VideoPipeline::Vulkan;
        r = chooseLinuxRoute(f);
        CHECK(r.encoder == LinuxRoute::Encoder::Vulkan);

        // A Vulkan conversion that cannot run: GL cannot read shared memory,
        // so the CPU runs — never GL.
        f = amd();
        f.portal = true;
        f.sharedMemory = true;
        f.vulkanConvertRefusal =
            "the Vulkan conversion gave up while streaming (VK_ERROR_DEVICE_LOST)";
        r = chooseLinuxRoute(f);
        CHECK(r.encoder == LinuxRoute::Encoder::Cpu);
        CHECK(r.refused);
        CHECK(contains(r.reason, "shared memory, which GL cannot read"));
        CHECK(contains(r.reason, "VK_ERROR_DEVICE_LOST"));

        // Intel: the table converts with GL — the CPU, as this route always
        // went, and no refusal; the bench key measures Vulkan there.
        f = amd();
        f.vendorId = 0x8086;
        f.portal = true;
        f.sharedMemory = true;
        r = chooseLinuxRoute(f);
        CHECK(r.encoder == LinuxRoute::Encoder::Cpu);
        CHECK(!r.refused);
        CHECK(contains(r.reason, "shared memory, which GL cannot read"));
        f.convertKey = EncoderTuning::ConvertLinux::Vulkan;
        r = chooseLinuxRoute(f);
        CHECK(r.conversion == LinuxRoute::Conversion::Vulkan);

        // VA-API by name converts with GL: the CPU here, and said as a refusal.
        f = amd();
        f.portal = true;
        f.sharedMemory = true;
        f.setting = VideoPipeline::Vaapi;
        r = chooseLinuxRoute(f);
        CHECK(r.encoder == LinuxRoute::Encoder::Cpu);
        CHECK(r.refused);
        CHECK(contains(r.reason, "VA-API by name converts with GL"));
    }

    SECTION("LinuxRoute — a VA-API without parameter sets: the CPU, whatever was asked");
    {
        LinuxRouteFacts f = amd();
        f.vaapiUnusable = true;
        LinuxRoute r = chooseLinuxRoute(f);
        CHECK(r.encoder == LinuxRoute::Encoder::Cpu);
        CHECK(!r.refused);
        CHECK(contains(r.reason, "no parameter sets"));

        f.benchKey = VideoPipeline::Vaapi;
        r = chooseLinuxRoute(f);
        CHECK(r.encoder == LinuxRoute::Encoder::Cpu);
        CHECK(r.refused);
        CHECK(contains(r.reason, "pipeline=vaapi asks for VA-API"));
    }

    SECTION("LinuxRoute — Windows' values are no opinion here");
    {
        LinuxRouteFacts f = amd();
        f.setting = VideoPipeline::D3d12;
        f.benchKey = VideoPipeline::D3d11;
        const LinuxRoute r = chooseLinuxRoute(f);
        CHECK(r.encoder == LinuxRoute::Encoder::Vaapi);
        CHECK(!r.refused);
        CHECK(contains(r.reason, "vendor table"));
    }
}
