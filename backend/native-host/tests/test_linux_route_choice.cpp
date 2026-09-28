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
    SECTION("LinuxRoute — the vendor table: VA-API everywhere; on AMD fed by Vulkan compute off "
            "the scanout (§9-20), by GL elsewhere");
    {
        for (uint32_t vendor : {0x1002u, 0x8086u, 0x10DEu, 0u})
            CHECK(autoLinuxPipeline(vendor) == VideoPipeline::Vaapi);
        CHECK(autoLinuxConversion(0x1002, false) == EncoderTuning::ConvertLinux::Vulkan);
        CHECK(autoLinuxConversion(0x1002, true) == EncoderTuning::ConvertLinux::Gl);
        for (uint32_t vendor : {0x8086u, 0x10DEu, 0u})
            for (bool portal : {false, true})
                CHECK(autoLinuxConversion(vendor, portal) == EncoderTuning::ConvertLinux::Gl);

        LinuxRoute r = chooseLinuxRoute(amd());
        CHECK(r.encoder == LinuxRoute::Encoder::Vaapi);
        CHECK(r.conversion == LinuxRoute::Conversion::Vulkan);
        CHECK(r.pipeline == VideoPipeline::Vaapi);
        CHECK_EQ(r.route, std::string("Vulkan compute → VA-API"));
        CHECK(!r.refused);
        CHECK(contains(r.reason, "vendor table has VA-API for AMD"));
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

    SECTION("LinuxRoute — AMD through the portal: GL, until the portal's buffers are imported on "
            "a bench (C13.3)");
    {
        LinuxRouteFacts f = amd();
        f.portal = true;
        LinuxRoute r = chooseLinuxRoute(f);
        CHECK(r.conversion == LinuxRoute::Conversion::Gl);
        CHECK_EQ(r.route, std::string("EGL → VA-API"));
        CHECK(!r.refused);
        CHECK(contains(r.reason, "converts with GL for AMD on the portal"));
        // The bench key still measures it there.
        f.convertKey = EncoderTuning::ConvertLinux::Vulkan;
        r = chooseLinuxRoute(f);
        CHECK(r.conversion == LinuxRoute::Conversion::Vulkan);
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

    SECTION("LinuxRoute — auto never takes Vulkan Video, even trusted: behind the setting until "
            "G5 (§9-21)");
    {
        for (uint32_t vendor : {0x1002u, 0x8086u, 0x10DEu, 0u}) {
            LinuxRouteFacts f = amdWithVulkanEncoder();
            f.vendorId = vendor;
            CHECK(!linuxRouteWantsVulkanVideo(f));
            const LinuxRoute r = chooseLinuxRoute(f);
            CHECK(r.encoder == LinuxRoute::Encoder::Vaapi);
            CHECK(r.pipeline == VideoPipeline::Vaapi);
            CHECK(!r.refused);
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
        // The vendor table has VA-API everywhere: nobody is made to prove.
        LinuxRouteFacts f = amdWithVulkanEncoder();
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

        // H.264 and AV1 come later (Phase 9): VA-API carries them meanwhile.
        f.codec = Codec::H264;
        r = chooseLinuxRoute(f);
        CHECK(r.encoder == LinuxRoute::Encoder::Vaapi);
        CHECK(contains(r.reason, "H.264 is not done by the Vulkan Video encoder yet"));
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

    SECTION("LinuxRoute — the portal's shared memory and a VA-API without parameter sets: the "
            "CPU, whatever was asked");
    {
        LinuxRouteFacts f = amdWithVulkanEncoder();
        f.sharedMemory = true;
        f.benchKey = VideoPipeline::Vulkan;
        f.convertKey = EncoderTuning::ConvertLinux::Vulkan;
        LinuxRoute r = chooseLinuxRoute(f);
        CHECK(r.encoder == LinuxRoute::Encoder::Cpu);
        CHECK(r.refused);
        CHECK(contains(r.reason, "shared memory"));

        f = amd();
        f.vaapiUnusable = true;
        r = chooseLinuxRoute(f);
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
