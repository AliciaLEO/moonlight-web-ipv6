/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#if defined(_WIN32)
#include "convert/windows/ColorConvert.h"
#include "platform/windows/video/D3d11VideoPipeline.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace mw::native;
using Microsoft::WRL::ComPtr;

namespace {

/// A capture that holds nothing but a device: the pipeline only ever asks a
/// capture for its device, context, size and format — the texture it converts
/// comes from the caller. On WARP, so it runs on a machine with no GPU.
class FakeCapture final : public capture::IWindowsCapture
{
public:
    FakeCapture(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height)
        : m_Device(device)
        , m_Context(context)
        , m_Width(width)
        , m_Height(height)
    {}

    bool start(std::string&) override { return true; }
    capture::AcquireStatus acquire(int, capture::CapturedFrame&) override
    {
        return capture::AcquireStatus::Timeout;
    }
    void release() override {}
    void stop() override {}
    ID3D11Device* device() const override { return m_Device; }
    ID3D11DeviceContext* context() const override { return m_Context; }
    int width() const override { return m_Width; }
    int height() const override { return m_Height; }
    DXGI_FORMAT format() const override { return DXGI_FORMAT_B8G8R8A8_UNORM; }
    capture::DesktopRect desktopRect() const override { return {0, 0, m_Width, m_Height}; }
    const capture::CursorState& cursor() const override { return m_Cursor; }
    int cursorHotspotX() const override { return 0; }
    int cursorHotspotY() const override { return 0; }

private:
    ID3D11Device* m_Device;
    ID3D11DeviceContext* m_Context;
    int m_Width;
    int m_Height;
    capture::CursorState m_Cursor;
};

} // namespace
#endif

// The D3D11 path behind WindowsVideoPipeline (plan pipeline-video-d3d12-v2,
// C0.5-C0.6): moved out of WindowsSession without a change of behaviour, so
// what it writes must be what ColorConvert alone writes, byte for byte; the
// held desktop must come back with only the pointer's rectangle changed; the
// black of a restart must be limited-range black; and the encoder behind it
// must open on a keyframe and follow with deltas.
void run_d3d11_video_pipeline_tests()
{
#if defined(_WIN32)
    SECTION("D3D11 video pipeline — the same bytes as ColorConvert, on WARP");

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL wanted[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, wanted, 2, D3D11_SDK_VERSION,
                                   device.GetAddressOf(), nullptr, context.GetAddressOf()))) {
        std::fprintf(stderr, "  no WARP device here — skipped\n");
        return;
    }

    constexpr int kW = 64;
    constexpr int kH = 64;

    // A picture with something different in every pixel, so that a shifted
    // or mis-scaled conversion cannot pass for the right one.
    std::vector<uint8_t> pixels(static_cast<size_t>(kW) * kH * 4);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            uint8_t* p = &pixels[(static_cast<size_t>(y) * kW + x) * 4];
            p[0] = static_cast<uint8_t>(x * 4);
            p[1] = static_cast<uint8_t>(y * 4);
            p[2] = static_cast<uint8_t>((x + y) * 2);
            p[3] = 255;
        }
    }
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = kW;
    desc.Height = kH;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA seed = {};
    seed.pSysMem = pixels.data();
    seed.SysMemPitch = kW * 4;
    ComPtr<ID3D11Texture2D> source;
    CHECK(SUCCEEDED(device->CreateTexture2D(&desc, &seed, source.GetAddressOf())));
    if (!source) return;

    // An NV12 texture, read back whole: luma rows, then the chroma rows.
    auto readNv12 = [&](ID3D11Texture2D* nv12, std::vector<uint8_t>& out, UINT& pitch, int& h) {
        out.clear();
        if (!nv12) return false;
        D3D11_TEXTURE2D_DESC d = {};
        nv12->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device->CreateTexture2D(&d, nullptr, staging.GetAddressOf()))) return false;
        context->CopyResource(staging.Get(), nv12);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
        pitch = mapped.RowPitch;
        h = static_cast<int>(d.Height);
        const auto* bytes = static_cast<const uint8_t*>(mapped.pData);
        out.assign(bytes, bytes + static_cast<size_t>(mapped.RowPitch) * d.Height * 3 / 2);
        context->Unmap(staging.Get(), 0);
        return true;
    };

    FakeCapture capture(device.Get(), context.Get(), kW, kH);
    std::string error;

    for (const convert::ScaleFilter filter :
         {convert::ScaleFilter::Bilinear, convert::ScaleFilter::Lanczos2}) {
        // 1:1 for the plain pass, halved for the resample pass.
        const int outW = filter == convert::ScaleFilter::Bilinear ? kW : kW / 2;
        const int outH = filter == convert::ScaleFilter::Bilinear ? kH : kH / 2;

        convert::ColorConvert direct;
        CHECK(direct.init(device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, kW, kH, outW, outH,
                          convert::ColorConvert::Chroma::C420, false, filter, error));
        CHECK(direct.convert(source.Get(), capture::CursorState{}, convert::CursorDraw{}, error));

        D3d11VideoPipeline pipeline;
        CHECK(std::string(pipeline.kind()) == "d3d11");
        CHECK(pipeline.open(false, 0, "", error));
        pipeline.teardown(false);
        pipeline.raisePriority();
        WindowsVideoPipeline::ConverterBuild converter;
        converter.outputWidth = outW;
        converter.outputHeight = outH;
        converter.filter = filter;
        CHECK(pipeline.buildConverter(capture, converter, error));
        CHECK_EQ(pipeline.outputWidth(), outW);
        CHECK_EQ(pipeline.outputHeight(), outH);
        CHECK_EQ(pipeline.copiesPerFrame(), 1);
        CHECK(pipeline.scaleFilter() == direct.scaleFilter());
        CHECK(pipeline.convert(capture, source.Get(), capture::CursorState{}, convert::CursorDraw{},
                               false, error));
        CHECK(!pipeline.hasHeld());

        std::vector<uint8_t> a, b;
        UINT pitchA = 0, pitchB = 0;
        int hA = 0, hB = 0;
        CHECK(readNv12(direct.output(), a, pitchA, hA));
        CHECK(readNv12(pipeline.output(), b, pitchB, hB));
        CHECK_EQ(pitchA, pitchB);
        CHECK(!a.empty() && a == b);
    }

    SECTION("D3D11 video pipeline — the held desktop changes only under the pointer");

    {
        D3d11VideoPipeline pipeline;
        CHECK(pipeline.open(false, 0, "", error));
        WindowsVideoPipeline::ConverterBuild converter;
        CHECK(pipeline.buildConverter(capture, converter, error));

        // Retained on the way through: the copy the pointer-only path redraws.
        CHECK(pipeline.convert(capture, source.Get(), capture::CursorState{}, convert::CursorDraw{},
                               true, error));
        CHECK(pipeline.hasHeld());
        std::vector<uint8_t> before;
        UINT pitch = 0;
        int h = 0;
        CHECK(readNv12(pipeline.output(), before, pitch, h));

        // A white 8×8 pointer at (16, 16), redrawn over the held copy alone.
        capture::CursorState cursor;
        cursor.visible = true;
        cursor.x = 16;
        cursor.y = 16;
        cursor.width = 8;
        cursor.height = 8;
        cursor.inkWidth = 8;
        cursor.inkHeight = 8;
        cursor.pixels.assign(8 * 8 * 4, 255);
        cursor.invert.assign(8 * 8, 0);
        cursor.shapeVersion = 1;
        CHECK(pipeline.convertHeld(capture, cursor, convert::CursorDraw{}, error));
        std::vector<uint8_t> after;
        CHECK(readNv12(pipeline.output(), after, pitch, h));

        bool outsideSame = true;
        bool insideChanged = false;
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                const size_t i = static_cast<size_t>(y) * pitch + x;
                const bool under = x >= 16 && x < 24 && y >= 16 && y < 24;
                if (under && before[i] != after[i]) insideChanged = true;
                if (!under && before[i] != after[i]) outsideSame = false;
            }
        }
        CHECK(insideChanged);
        CHECK(outsideSame);

        // The load cap's rebuild keeps the copy; any other rebuild drops it.
        pipeline.teardown(true);
        CHECK(pipeline.hasHeld());
        CHECK(!pipeline.built());
        pipeline.teardown(false);
        CHECK(!pipeline.hasHeld());
    }

    SECTION("D3D11 video pipeline — the black of a restart, and the encoder behind it");

    {
        D3d11VideoPipeline pipeline;
        CHECK(pipeline.open(false, 0, "", error));
        WindowsVideoPipeline::ConverterBuild converter;
        CHECK(pipeline.buildConverter(capture, converter, error));

        // No capture, no black: said, not guessed.
        std::string why;
        CHECK(!pipeline.prepareBlank(nullptr, why));
        CHECK(why == "the capture has no device to make it on");

        CHECK(pipeline.prepareBlank(&capture, error));
        CHECK(pipeline.hasBlank());
        CHECK(pipeline.convertBlank(convert::CursorDraw{}, error));
        std::vector<uint8_t> black;
        UINT pitch = 0;
        int h = 0;
        CHECK(readNv12(pipeline.output(), black, pitch, h));
        bool lumaBlack = true;
        bool chromaNeutral = true;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < kW; ++x)
                lumaBlack = lumaBlack && black[static_cast<size_t>(y) * pitch + x] == 16;
        for (int y = 0; y < h / 2; ++y)
            for (int x = 0; x < kW; ++x)
                chromaNeutral =
                    chromaNeutral && black[static_cast<size_t>(h + y) * pitch + x] == 128;
        CHECK(lumaBlack);
        CHECK(chromaNeutral);
        pipeline.releaseBlank();
        CHECK(!pipeline.hasBlank());

        // The CPU encoder, the one tier every machine has: a keyframe first,
        // deltas after it.
        WindowsVideoPipeline::EncoderBuild encoder;
        encoder.encoder = EncoderApi::Software;
        encoder.codec = Codec::H264;
        encoder.fps = 60;
        encoder.bitrateKbps = 2000;
        CHECK(pipeline.buildEncoder(capture, encoder, error));
        if (!error.empty()) std::fprintf(stderr, "  %s\n", error.c_str());
        CHECK(pipeline.built());
        CHECK(!pipeline.intraRefreshEnabled());

        CHECK(pipeline.convert(capture, source.Get(), capture::CursorState{}, convert::CursorDraw{},
                               false, error));
        encode::EncoderOutput first;
        CHECK(pipeline.encode(true, 0, first, error) == WindowsVideoPipeline::EncodeResult::Ok);
        CHECK(first.keyframe);
        CHECK(first.size > 0);
        pipeline.releaseOutput();
        encode::EncoderOutput second;
        CHECK(pipeline.encode(false, 1, second, error) == WindowsVideoPipeline::EncodeResult::Ok);
        CHECK(!second.keyframe);
        pipeline.releaseOutput();
        CHECK(pipeline.setBitrate(1000, error));

        pipeline.close();
        CHECK(!pipeline.built());
        CHECK(!pipeline.hasEncoder());
        CHECK(!pipeline.hasHeld());
    }

    SECTION("D3D11 video pipeline — an encoder it does not know is refused by name");

    {
        D3d11VideoPipeline pipeline;
        CHECK(pipeline.open(false, 0, "", error));
        WindowsVideoPipeline::ConverterBuild converter;
        CHECK(pipeline.buildConverter(capture, converter, error));
        WindowsVideoPipeline::EncoderBuild encoder;
        encoder.encoder = EncoderApi::None;
        std::string why;
        CHECK(!pipeline.buildEncoder(capture, encoder, why));
        CHECK(why.find("no encoder implementation for") == 0);
    }
#endif
}
