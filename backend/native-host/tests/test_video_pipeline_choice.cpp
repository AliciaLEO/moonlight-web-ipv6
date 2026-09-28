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

// The choice of a Windows session's picture chain: what decides (bench key,
// setting, vendor table), what rules D3D12 out, and what the log is told.

#include "core/VideoPipelineChoice.h"
#include "native_test_framework.h"

#include <cstdio>
#include <string>

using namespace mw::native;

namespace {

/// An Arc streaming HEVC over Desktop Duplication, D3D12 Video Encode there:
/// everything a D3D12 route needs.
VideoPipelineFacts arc()
{
    VideoPipelineFacts f;
    f.encoder = EncoderApi::Vpl;
    f.codec = Codec::Hevc;
    f.videoEncode12 = true;
    return f;
}

VideoPipelineFacts forcedD3d12()
{
    VideoPipelineFacts f = arc();
    f.setting = VideoPipeline::D3d12;
    return f;
}

bool contains(const std::string& text, const std::string& piece)
{
    return text.find(piece) != std::string::npos;
}

} // namespace

void run_video_pipeline_choice_tests()
{
    SECTION("VideoPipeline — names read back, in any case, and nothing else is taken");
    {
        for (VideoPipeline p : {VideoPipeline::Auto, VideoPipeline::D3d11, VideoPipeline::D3d12}) {
            VideoPipeline back = VideoPipeline::Auto;
            CHECK(parseVideoPipeline(toString(p), back));
            CHECK(back == p);
        }
        VideoPipeline p = VideoPipeline::D3d11;
        CHECK(parseVideoPipeline("D3D12", p));
        CHECK(p == VideoPipeline::D3d12);
        CHECK(!parseVideoPipeline("d3d13", p));
        CHECK(!parseVideoPipeline("", p));
        CHECK(p == VideoPipeline::D3d12); // untouched by a refusal
    }

    SECTION("VideoPipeline — the vendor table is D3D11 everywhere until Bruno moves a line");
    {
        for (EncoderApi api : {EncoderApi::None, EncoderApi::Nvenc, EncoderApi::Amf,
                               EncoderApi::Vpl, EncoderApi::MediaFoundation, EncoderApi::Software})
            CHECK(autoVideoPipeline(api) == VideoPipeline::D3d11);
        const VideoPipelineChoice c = chooseVideoPipeline(arc());
        CHECK(c.pipeline == VideoPipeline::D3d11);
        CHECK_EQ(c.route, std::string("D3D11"));
        CHECK(c.encoder.empty()); // the Selector's encoder names itself
        CHECK(!c.refused);
        CHECK(contains(c.reason, "vendor table"));
        CHECK(contains(c.reason, "oneVPL"));
    }

    SECTION("VideoPipeline — the bench key over the setting over the table");
    {
        VideoPipelineFacts f = arc();
        f.setting = VideoPipeline::D3d12;
        VideoPipelineChoice c = chooseVideoPipeline(f);
        CHECK(c.pipeline == VideoPipeline::D3d12);
        CHECK(contains(c.reason, "setting"));

        f.benchKey = VideoPipeline::D3d11;
        c = chooseVideoPipeline(f);
        CHECK(c.pipeline == VideoPipeline::D3d11);
        CHECK(!c.refused); // asked for D3D11: nothing was refused
        CHECK(contains(c.reason, "pipeline=d3d11"));

        f.setting = VideoPipeline::D3d11;
        f.benchKey = VideoPipeline::D3d12;
        c = chooseVideoPipeline(f);
        CHECK(c.pipeline == VideoPipeline::D3d12);
        CHECK(contains(c.reason, "pipeline=d3d12"));
    }

    SECTION("VideoPipeline — the route names the queue, the encoder and the codec; the overlay, "
            "the encoder alone");
    {
        VideoPipelineFacts f = forcedD3d12();
        CHECK_EQ(chooseVideoPipeline(f).route,
                 std::string("DIRECT conversion → D3D12 Video Encode HEVC"));
        CHECK_EQ(chooseVideoPipeline(f).encoder, std::string("D3D12 VE"));
        f.conv12 = EncoderTuning::ConvertQueue12::Compute;
        CHECK_EQ(chooseVideoPipeline(f).route,
                 std::string("COMPUTE conversion → D3D12 Video Encode HEVC"));

        VideoPipelineFacts rtx = forcedD3d12();
        rtx.encoder = EncoderApi::Nvenc;
        rtx.enc12 = EncoderTuning::Encoder12::Nvenc;
        rtx.nvenc12 = true;
        CHECK_EQ(chooseVideoPipeline(rtx).route,
                 std::string("DIRECT conversion → NVENC (D3D12) HEVC"));
        CHECK_EQ(chooseVideoPipeline(rtx).encoder, std::string("NVENC (D3D12)"));
        VideoPipelineFacts amd = forcedD3d12();
        amd.encoder = EncoderApi::Amf;
        amd.enc12 = EncoderTuning::Encoder12::Amf;
        amd.amf12 = true;
        CHECK_EQ(chooseVideoPipeline(amd).route,
                 std::string("DIRECT conversion → AMF (D3D12) HEVC"));
        CHECK_EQ(chooseVideoPipeline(amd).encoder, std::string("AMF (D3D12)"));
    }

    SECTION("VideoPipeline — what rules D3D12 out for a build, each one named");
    {
        struct Case
        {
            const char* what;
            void (*spoil)(VideoPipelineFacts&);
            const char* named;
        };
        const Case cases[] = {
            {"WGC", [](VideoPipelineFacts& f) { f.capture = CaptureApi::WindowsGraphicsCapture; },
             "Windows.Graphics.Capture"},
            {"bridge", [](VideoPipelineFacts& f) { f.crossGpuCopy = true; }, "another GPU"},
            {"MF", [](VideoPipelineFacts& f) { f.encoder = EncoderApi::MediaFoundation; },
             "Media Foundation has no D3D12 route"},
            {"software", [](VideoPipelineFacts& f) { f.encoder = EncoderApi::Software; },
             "software has no D3D12 route"},
            {"4:4:4", [](VideoPipelineFacts& f) { f.yuv444 = true; }, "4:4:4"},
            {"H.264", [](VideoPipelineFacts& f) { f.codec = Codec::H264; }, "H.264 is not done"},
            {"AV1", [](VideoPipelineFacts& f) { f.codec = Codec::Av1; }, "AV1 is not done"},
            {"no VE", [](VideoPipelineFacts& f) { f.videoEncode12 = false; },
             "D3D12 Video Encode does not take HEVC"},
            {"NVENC on Intel",
             [](VideoPipelineFacts& f) { f.enc12 = EncoderTuning::Encoder12::Nvenc; },
             "enc12=nvenc on a GPU NVENC does not drive"},
            {"NVENC12 not built",
             [](VideoPipelineFacts& f) {
                 f.encoder = EncoderApi::Nvenc;
                 f.enc12 = EncoderTuning::Encoder12::Nvenc;
             },
             "NVENC fed D3D12 pictures is not built yet"},
            {"AMF12 not built",
             [](VideoPipelineFacts& f) {
                 f.encoder = EncoderApi::Amf;
                 f.enc12 = EncoderTuning::Encoder12::Amf;
             },
             "AMF fed D3D12 pictures is not built yet"},
        };
        for (const Case& k : cases) {
            VideoPipelineFacts f = forcedD3d12();
            k.spoil(f);
            const VideoPipelineChoice c = chooseVideoPipeline(f);
            CHECK(c.pipeline == VideoPipeline::D3d11);
            CHECK(c.refused);
            CHECK_EQ(c.route, std::string("D3D11"));
            CHECK(c.encoder.empty());
            CHECK(contains(c.reason, "the setting (d3d12) asks for D3D12, D3D11 runs: "));
            if (!contains(c.reason, k.named))
                std::fprintf(stderr, "  %s: \"%s\"\n", k.what, c.reason.c_str());
            CHECK(contains(c.reason, k.named));
        }
    }

    SECTION("VideoPipeline — a refusal only matters when D3D12 was asked for");
    {
        VideoPipelineFacts f = arc();
        f.capture = CaptureApi::WindowsGraphicsCapture;
        f.yuv444 = true;
        const VideoPipelineChoice c = chooseVideoPipeline(f);
        CHECK(c.pipeline == VideoPipeline::D3d11);
        CHECK(!c.refused);
        f.setting = VideoPipeline::D3d11;
        CHECK(!chooseVideoPipeline(f).refused);
    }

    SECTION("VideoPipeline — Linux's chains read back, and are no opinion on Windows");
    {
        for (VideoPipeline p : {VideoPipeline::Vaapi, VideoPipeline::Vulkan}) {
            VideoPipeline back = VideoPipeline::Auto;
            CHECK(parseVideoPipeline(toString(p), back));
            CHECK(back == p);
            CHECK(!isWindowsPipeline(p));
        }
        CHECK(autoVideoPipeline(EncoderApi::VaApi) == VideoPipeline::Vaapi);
        // A bench key or a setting naming a Linux chain leaves Windows on its
        // table, as Auto does: nothing asked of D3D12, nothing refused.
        VideoPipelineFacts f = arc();
        f.setting = VideoPipeline::Vulkan;
        f.benchKey = VideoPipeline::Vaapi;
        const VideoPipelineChoice c = chooseVideoPipeline(f);
        CHECK(c.pipeline == VideoPipeline::D3d11);
        CHECK(!c.refused);
        CHECK(contains(c.reason, "vendor table"));
    }
}
