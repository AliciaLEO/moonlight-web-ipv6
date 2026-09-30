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
#include <vector>

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

    SECTION("VideoPipeline — the vendor table: D3D12 on Intel (§9-23), D3D11 for the rest");
    {
        CHECK(autoVideoPipeline(EncoderApi::Vpl) == VideoPipeline::D3d12);
        for (EncoderApi api : {EncoderApi::None, EncoderApi::Nvenc, EncoderApi::Amf,
                               EncoderApi::MediaFoundation, EncoderApi::Software})
            CHECK(autoVideoPipeline(api) == VideoPipeline::D3d11);

        VideoPipelineChoice c = chooseVideoPipeline(arc());
        CHECK(c.pipeline == VideoPipeline::D3d12);
        CHECK_EQ(c.route, std::string("DIRECT conversion → D3D12 Video Encode HEVC"));
        CHECK_EQ(c.encoder, std::string("D3D12 VE"));
        CHECK(!c.refused);
        CHECK(contains(c.reason, "auto: the vendor table has D3D12 for oneVPL"));

        VideoPipelineFacts rtx = arc();
        rtx.encoder = EncoderApi::Nvenc;
        c = chooseVideoPipeline(rtx);
        CHECK(c.pipeline == VideoPipeline::D3d11);
        CHECK_EQ(c.route, std::string("D3D11"));
        CHECK(c.encoder.empty()); // the Selector's encoder names itself
        CHECK(!c.refused);
        CHECK(contains(c.reason, "auto: the vendor table has D3D11 for NVENC"));

        // The way back from the table's D3D12, from the admin page.
        VideoPipelineFacts back = arc();
        back.setting = VideoPipeline::D3d11;
        c = chooseVideoPipeline(back);
        CHECK(c.pipeline == VideoPipeline::D3d11);
        CHECK(!c.refused);
        CHECK(contains(c.reason, "the setting (d3d11)"));
    }

    SECTION("VideoPipeline — the table's D3D12 is HEVC's: H.264 keeps D3D11 on Auto, the setting "
            "reaches D3D12 Video Encode's");
    {
        VideoPipelineFacts f = arc();
        f.codec = Codec::H264; // a browser that decodes no HEVC
        VideoPipelineChoice c = chooseVideoPipeline(f);
        CHECK(c.pipeline == VideoPipeline::D3d11);
        CHECK(!c.refused); // the table has D3D11 for it: nothing was refused
        CHECK(c.encoder.empty());
        CHECK(contains(c.reason, "auto: the vendor table has D3D12 for oneVPL in HEVC, D3D11 in "
                                 "H.264 (not measured on D3D12)"));
        CHECK(autoD3d12Codec(Codec::Hevc));
        CHECK(!autoD3d12Codec(Codec::H264));
        CHECK(!autoD3d12Codec(Codec::Av1));

        f.setting = VideoPipeline::D3d12;
        c = chooseVideoPipeline(f);
        CHECK(c.pipeline == VideoPipeline::D3d12);
        CHECK_EQ(c.route, std::string("DIRECT conversion → D3D12 Video Encode H.264"));
        CHECK_EQ(c.encoder, std::string("D3D12 VE"));
    }

    SECTION("VideoPipeline — the table's D3D12 refused for a build: D3D11 runs, and says the table "
            "asked");
    {
        // A GPU a first build found without D3D12 Video Encode (Windows 10):
        // the builds after it choose D3D11 from the start.
        VideoPipelineFacts f = arc();
        f.videoEncode12 = false;
        VideoPipelineChoice c = chooseVideoPipeline(f);
        CHECK(c.pipeline == VideoPipeline::D3d11);
        CHECK(c.refused); // the overlay reads "oneVPL (D3D11)"
        CHECK(c.encoder.empty());
        CHECK(contains(c.reason, "auto: the vendor table for oneVPL asks for D3D12, D3D11 runs: "
                                 "this GPU's D3D12 Video Encode does not take HEVC"));
    }

    SECTION("VideoPipeline — intra-refresh required: a D3D12 route that grants none gives way to "
            "D3D11, whoever asked for it");
    {
        // The guests' shared feed of a native host (plan « flux commun des
        // invités », S1): taken for granted until a build finds out.
        VideoPipelineFacts f = arc();
        f.intraRefreshRequired = true;
        VideoPipelineChoice c = chooseVideoPipeline(f);
        CHECK(c.pipeline == VideoPipeline::D3d12);
        CHECK(!c.refused);
        // The Arc's D3D12 Video Encode sweeps one frame at most, which is no
        // wave: once a build has seen it, D3D11 (oneVPL) from the start.
        f.d3d12IntraRefresh = false;
        c = chooseVideoPipeline(f);
        CHECK(c.pipeline == VideoPipeline::D3d11);
        CHECK(c.refused); // the overlay reads "oneVPL (D3D11)"
        CHECK(c.encoder.empty());
        CHECK(contains(c.reason, "auto: the vendor table for oneVPL asks for D3D12, D3D11 runs: "
                                 "the stream must refresh by intra-refresh, which the D3D12 "
                                 "route does not grant on this GPU"));
        // The setting's D3D12 as well: the feed cannot do without its wave.
        f.setting = VideoPipeline::D3d12;
        c = chooseVideoPipeline(f);
        CHECK(c.pipeline == VideoPipeline::D3d11);
        CHECK(contains(c.reason, "the setting (d3d12) asks for D3D12, D3D11 runs"));
        // Any other stream: the same GPU keeps its D3D12.
        f.intraRefreshRequired = false;
        c = chooseVideoPipeline(f);
        CHECK(c.pipeline == VideoPipeline::D3d12);
        CHECK(!c.refused);
        // And a GPU the table keeps on D3D11 is not touched either way.
        VideoPipelineFacts rtx = arc();
        rtx.encoder = EncoderApi::Nvenc;
        rtx.intraRefreshRequired = true;
        rtx.d3d12IntraRefresh = false;
        c = chooseVideoPipeline(rtx);
        CHECK(c.pipeline == VideoPipeline::D3d11);
        CHECK(!c.refused);
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

    SECTION(
        "VideoPipeline — the D3D12 route's encoder: NVENC on NVIDIA (G1), VE elsewhere until G4");
    {
        CHECK(defaultEncoder12(EncoderApi::Nvenc) == EncoderTuning::Encoder12::Nvenc);
        for (EncoderApi api : {EncoderApi::Amf, EncoderApi::Vpl, EncoderApi::None})
            CHECK(defaultEncoder12(api) == EncoderTuning::Encoder12::VideoEncode);

        // D3D12 asked for on an RTX, no bench key: NVENC takes the picture.
        VideoPipelineFacts rtx = forcedD3d12();
        rtx.encoder = EncoderApi::Nvenc;
        rtx.nvenc12 = true;
        VideoPipelineChoice c = chooseVideoPipeline(rtx);
        CHECK(c.pipeline == VideoPipeline::D3d12);
        CHECK(c.encoder12 == EncoderTuning::Encoder12::Nvenc);
        CHECK_EQ(c.route, std::string("DIRECT conversion → NVENC (D3D12) HEVC"));
        CHECK_EQ(c.encoder, std::string("NVENC (D3D12)"));

        // The vendor's SDK codes what its D3D11 path codes; VE, the three
        // codecs where the GPU's driver does (its encoder asks).
        rtx.codec = Codec::H264;
        c = chooseVideoPipeline(rtx);
        CHECK(c.pipeline == VideoPipeline::D3d12);
        CHECK_EQ(c.route, std::string("DIRECT conversion → NVENC (D3D12) H.264"));
        rtx.codec = Codec::Av1;
        CHECK(chooseVideoPipeline(rtx).pipeline == VideoPipeline::D3d12);
        rtx.codec = Codec::Hevc;

        // The bench can still put VE on the RTX, for G2's comparison.
        rtx.enc12 = EncoderTuning::Encoder12::VideoEncode;
        c = chooseVideoPipeline(rtx);
        CHECK(c.pipeline == VideoPipeline::D3d12);
        CHECK(c.encoder12 == EncoderTuning::Encoder12::VideoEncode);
        CHECK_EQ(c.encoder, std::string("D3D12 VE"));
        rtx.codec = Codec::H264;
        CHECK_EQ(chooseVideoPipeline(rtx).route,
                 std::string("DIRECT conversion → D3D12 Video Encode H.264"));
        rtx.codec = Codec::Av1;
        CHECK_EQ(chooseVideoPipeline(rtx).route,
                 std::string("DIRECT conversion → D3D12 Video Encode AV1"));

        // No NVENC runtime able to take D3D12 pictures: D3D11, and why.
        VideoPipelineFacts old = forcedD3d12();
        old.encoder = EncoderApi::Nvenc;
        old.nvenc12 = false;
        c = chooseVideoPipeline(old);
        CHECK(c.pipeline == VideoPipeline::D3d11);
        CHECK(c.refused);
        CHECK(contains(c.reason, "NVENC takes no D3D12 picture on this machine"));

        // AMD keeps VE until G4 has been measured and Bruno has decided.
        VideoPipelineFacts amd = forcedD3d12();
        amd.encoder = EncoderApi::Amf;
        c = chooseVideoPipeline(amd);
        CHECK(c.pipeline == VideoPipeline::D3d12);
        CHECK(c.encoder12 == EncoderTuning::Encoder12::VideoEncode);

        // A D3D11 choice names no D3D12 encoder.
        CHECK(chooseVideoPipeline(arc()).encoder12 == EncoderTuning::Encoder12::VideoEncode);
        VideoPipelineFacts auto11 = arc();
        auto11.encoder = EncoderApi::Nvenc;
        CHECK(chooseVideoPipeline(auto11).pipeline == VideoPipeline::D3d11);
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
            {"no VE", [](VideoPipelineFacts& f) { f.videoEncode12 = false; },
             "D3D12 Video Encode does not take HEVC"},
            {"NVENC on Intel",
             [](VideoPipelineFacts& f) { f.enc12 = EncoderTuning::Encoder12::Nvenc; },
             "enc12=nvenc on a GPU NVENC does not drive"},
            {"NVENC12 unavailable",
             [](VideoPipelineFacts& f) {
                 f.encoder = EncoderApi::Nvenc;
                 f.enc12 = EncoderTuning::Encoder12::Nvenc;
             },
             "NVENC takes no D3D12 picture on this machine"},
            {"AMF12 unavailable",
             [](VideoPipelineFacts& f) {
                 f.encoder = EncoderApi::Amf;
                 f.enc12 = EncoderTuning::Encoder12::Amf;
             },
             "AMF takes no D3D12 picture on this machine"},
            {"excluded driver",
             [](VideoPipelineFacts& f) { f.driverExcluded = "driver 1.2.3.4: a fault"; },
             "the D3D12 route stays off driver 1.2.3.4: a fault"},
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

    SECTION("VideoPipeline — drivers the D3D12 route stays off: none yet, each range read whole");
    {
        CHECK_EQ(driverVersionText(driverVersionOf(32, 0, 101, 7088)),
                 std::string("32.0.101.7088"));
        CHECK_EQ(driverVersionText(driverVersionOf(65535, 1, 0, 65535)),
                 std::string("65535.1.0.65535"));
        CHECK_EQ(driverVersionText(0), std::string());
        // Packed, the versions compare as Windows orders them.
        CHECK(driverVersionOf(32, 0, 101, 7088) > driverVersionOf(32, 0, 101, 6987));
        CHECK(driverVersionOf(32, 0, 102, 0) > driverVersionOf(32, 0, 101, 65535));
        CHECK(driverVersionOf(33, 0, 0, 0) > driverVersionOf(32, 65535, 65535, 65535));

        // The product's list: empty, so nothing is kept off.
        CHECK(d3d12DriverExclusions().empty());
        CHECK(d3d12DriverExcluded(0x8086, driverVersionOf(32, 0, 101, 7088)).empty());

        const std::vector<D3d12DriverExclusion> list = {
            {0x8086, driverVersionOf(32, 0, 101, 6000), driverVersionOf(32, 0, 101, 6999),
             "a fault seen on the bench"},
        };
        CHECK_EQ(d3d12DriverExcluded(0x8086, driverVersionOf(32, 0, 101, 6500), list),
                 std::string("driver 32.0.101.6500: a fault seen on the bench"));
        // Both ends are in the range, the next version out of it.
        CHECK(!d3d12DriverExcluded(0x8086, driverVersionOf(32, 0, 101, 6000), list).empty());
        CHECK(!d3d12DriverExcluded(0x8086, driverVersionOf(32, 0, 101, 6999), list).empty());
        CHECK(d3d12DriverExcluded(0x8086, driverVersionOf(32, 0, 101, 7000), list).empty());
        CHECK(d3d12DriverExcluded(0x8086, driverVersionOf(32, 0, 101, 5999), list).empty());
        // Another vendor's driver of the same number, and a version nobody gave.
        CHECK(d3d12DriverExcluded(0x1002, driverVersionOf(32, 0, 101, 6500), list).empty());
        CHECK(d3d12DriverExcluded(0x8086, 0, list).empty());

        // Refused like any other reason, and the table's D3D12 with it.
        VideoPipelineFacts f = arc();
        f.driverExcluded = d3d12DriverExcluded(0x8086, driverVersionOf(32, 0, 101, 6500), list);
        const VideoPipelineChoice c = chooseVideoPipeline(f);
        CHECK(c.pipeline == VideoPipeline::D3d11);
        CHECK(c.refused);
        CHECK_EQ(c.reason, std::string("auto: the vendor table for oneVPL asks for D3D12, D3D11 "
                                       "runs: the D3D12 route stays off driver 32.0.101.6500: a "
                                       "fault seen on the bench"));
    }

    SECTION("VideoPipeline — two pictures in flight by default on an Intel GPU of its own memory");
    {
        CHECK(pipelinedByDefault(0x8086, false));  // the Arc
        CHECK(!pipelinedByDefault(0x8086, true));  // an iGPU: the N95, UHD, Xe
        CHECK(!pipelinedByDefault(0x10DE, false)); // NVIDIA streams through NVENC
        CHECK(!pipelinedByDefault(0x1002, false));
        CHECK(!pipelinedByDefault(0x1002, true));
        CHECK(!pipelinedByDefault(0, false)); // a GPU nobody named
    }

    SECTION("EncoderTuning — link drops named by default for NVENC, and for AMF through D3D11");
    {
        using E12 = EncoderTuning::Encoder12;
        CHECK(nameLinkDropsByDefault(VideoPipeline::D3d11, EncoderApi::Nvenc, E12::Default));
        CHECK(nameLinkDropsByDefault(VideoPipeline::D3d11, EncoderApi::Amf, E12::Default));
        // oneVPL hung under it (§9-25); the fallbacks heal no loss by a delta.
        for (EncoderApi api :
             {EncoderApi::Vpl, EncoderApi::MediaFoundation, EncoderApi::Software, EncoderApi::None})
            CHECK(!nameLinkDropsByDefault(VideoPipeline::D3d11, api, E12::Default));
        // The D3D12 route: NVENC fed D3D12 pictures, as through D3D11 (bench
        // §8n.29) — the route the setting gives an NVIDIA GPU.
        CHECK(nameLinkDropsByDefault(VideoPipeline::D3d12, EncoderApi::Nvenc, E12::Nvenc));
        // AMF fed D3D12 pictures gained nothing, and D3D12 Video Encode, on
        // whichever GPU, neither.
        CHECK(!nameLinkDropsByDefault(VideoPipeline::D3d12, EncoderApi::Amf, E12::Amf));
        for (EncoderApi api : {EncoderApi::Nvenc, EncoderApi::Amf, EncoderApi::Vpl})
            CHECK(!nameLinkDropsByDefault(VideoPipeline::D3d12, api, E12::VideoEncode));
        // Keyed on the route's encoder, not on the GPU's SDK: a D3D12 chain
        // that names none is off.
        CHECK(!nameLinkDropsByDefault(VideoPipeline::D3d12, EncoderApi::Nvenc, E12::Default));
        // Linux and macOS, not measured.
        CHECK(!nameLinkDropsByDefault(VideoPipeline::Vaapi, EncoderApi::VaApi, E12::Default));
        CHECK(!nameLinkDropsByDefault(VideoPipeline::Vulkan, EncoderApi::VaApi, E12::Default));
        CHECK(!nameLinkDropsByDefault(VideoPipeline::Auto, EncoderApi::VideoToolbox, E12::Default));
    }

    SECTION("VideoPipeline — a refusal only matters when D3D12 was asked for");
    {
        VideoPipelineFacts f = arc();
        f.encoder = EncoderApi::Nvenc; // whose line is D3D11
        f.capture = CaptureApi::WindowsGraphicsCapture;
        f.yuv444 = true;
        const VideoPipelineChoice c = chooseVideoPipeline(f);
        CHECK(c.pipeline == VideoPipeline::D3d11);
        CHECK(!c.refused);
        f.encoder = EncoderApi::Vpl; // whose line is D3D12, asked for D3D11 by name
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
        // table, as Auto does: D3D12 for Intel, D3D11 for NVIDIA.
        VideoPipelineFacts f = arc();
        f.setting = VideoPipeline::Vulkan;
        f.benchKey = VideoPipeline::Vaapi;
        VideoPipelineChoice c = chooseVideoPipeline(f);
        CHECK(c.pipeline == VideoPipeline::D3d12);
        CHECK(!c.refused);
        CHECK(contains(c.reason, "vendor table"));
        f.encoder = EncoderApi::Nvenc;
        c = chooseVideoPipeline(f);
        CHECK(c.pipeline == VideoPipeline::D3d11);
        CHECK(!c.refused);
        CHECK(contains(c.reason, "vendor table"));
    }
}
