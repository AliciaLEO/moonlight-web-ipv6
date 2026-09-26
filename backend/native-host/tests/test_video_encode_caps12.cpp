/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#if defined(_WIN32)
#include "encode/windows/d3d12/VideoEncodeCaps12.h"
#include "platform/windows/d3d12/D3d12Device.h"

#include <dxgi1_4.h>
#include <wrl/client.h>

#include <set>
#include <string>

using namespace mw::native;
using namespace mw::native::encode;
using Microsoft::WRL::ComPtr;

namespace {

std::string blocksText(const HevcBlocks& b)
{
    return std::to_string(1 << b.log2MinCodingBlock) + ".." +
           std::to_string(1 << b.log2MaxCodingBlock) + " depth " + std::to_string(b.depth);
}

} // namespace
#endif

// HevcEncodeNegotiation against the real drivers: every hardware adapter with
// D3D12 Video Encode is asked for HEVC 1080p60. On the three GPUs of DualRTX
// the answers must be the ones test_hevc_negotiation.cpp stands in for — the
// check that those tables still describe the drivers. Elsewhere, only that a
// setup comes out and holds together.
void run_video_encode_caps12_tests()
{
#if defined(_WIN32)
    SECTION("VideoEncodeCaps12 — the negotiation on each GPU's own driver");

    ComPtr<IDXGIFactory4> factory;
    if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        std::fprintf(stderr, "  no DXGI factory — skipped\n");
        return;
    }
    std::set<uint64_t> seen;
    int asked = 0;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        const uint64_t luid =
            (static_cast<uint64_t>(static_cast<uint32_t>(desc.AdapterLuid.HighPart)) << 32) |
            desc.AdapterLuid.LowPart;
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || !seen.insert(luid).second) continue;
        std::string error;
        const std::shared_ptr<d3d12::D3d12Device> device =
            d3d12::D3d12Device::forAdapter(adapter.Get(), error);
        ComPtr<ID3D12VideoDevice3> video;
        if (!device || FAILED(device->device()->QueryInterface(IID_PPV_ARGS(&video)))) continue;

        VideoEncodeCaps12 caps(video.Get());
        HevcEncodeRequest request;
        request.width = 1920;
        request.height = 1080;
        request.fps = 60;
        request.intraRefresh = true;
        const HevcEncodeSetup s = negotiateHevc(request, caps);
        ++asked;
        std::fprintf(stderr, "  %s: %s\n", device->name().c_str(),
                     s.ok ? (blocksText(s.blocks) + (s.blocksAnswer.ampRequired ? ", AMP" : "") +
                             (s.blocksAnswer.pAsLowDelayB ? ", P as B" : "") + ", " +
                             s.rate.describe() + ", level " + std::to_string(s.sequence.levelIdc) +
                             ", QP map " + std::to_string(s.support.qpMapRegion) +
                             " px, intra refresh " + std::to_string(s.intraRefreshFrames) + ", " +
                             std::to_string(s.queries) + " queries" +
                             (s.reason.empty() ? "" : " (" + s.reason + ")"))
                                .c_str()
                          : ("no setup: " + s.reason).c_str());
        if (!s.ok) continue;
        CHECK_EQ(s.codedHeight, 1088u);
        CHECK_EQ(s.dpbCapacity, 4);
        CHECK(s.sequence.dialect == paramsets::HevcDialect::D3d12);

        const bool rtx = desc.VendorId == 0x10DE && desc.DeviceId == 0x2D04;
        const bool amd = desc.VendorId == 0x1002 && desc.DeviceId == 0x13C0;
        const bool arc = desc.VendorId == 0x8086 && desc.DeviceId == 0x56A5;
        if (rtx) {
            CHECK(s.blocks == HevcBlocks({3, 5, 2, 5, 3}));
            CHECK(s.blocksAnswer.ampRequired && !s.blocksAnswer.pAsLowDelayB);
            CHECK(s.rate.describe() == "CBR1 + VBV + QP range");
            CHECK(s.support.rateReconfigurable && !s.support.reconTextureArray);
            CHECK_EQ(s.support.qpMapRegion, 32u);
            CHECK_EQ(s.intraRefreshFrames, 0);
        }
        if (amd) {
            CHECK(s.blocks == HevcBlocks({3, 6, 2, 5, 4}));
            CHECK(!s.blocksAnswer.ampRequired && !s.blocksAnswer.pAsLowDelayB);
            CHECK(s.rate.describe() == "CBR1 + VBV + QP range + frame size cap");
            CHECK(s.support.rateReconfigurable && s.support.reconTextureArray);
            CHECK_EQ(s.support.qpMapRegion, 64u);
            CHECK_EQ(s.intraRefreshFrames, 120);
        }
        if (arc) {
            CHECK(s.blocks == HevcBlocks({3, 6, 2, 5, 2}));
            CHECK(s.blocksAnswer.ampRequired && s.blocksAnswer.pAsLowDelayB);
            CHECK(s.rate.describe() == "CBR1 + VBV + QP range + frame size cap");
            CHECK(!s.support.rateReconfigurable && !s.support.reconTextureArray);
            CHECK_EQ(s.support.qpMapRegion, 32u);
            CHECK_EQ(s.intraRefreshFrames, 0);
        }
        if (rtx || amd || arc) CHECK_EQ(s.sequence.levelIdc, 123);
    }
    if (asked == 0) std::fprintf(stderr, "  no GPU with D3D12 Video Encode here — skipped\n");
#endif
}
