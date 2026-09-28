/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#if defined(_WIN32)
#include "d3d12_test_pictures.h"
#include "encode/H264SliceParser.h"
#include "encode/HevcDpb.h"
#include "encode/HevcSliceParser.h"
#include "encode/windows/d3d12/VideoEncode12.h"
#include "platform/windows/IndirectDisplay.h"
#include "platform/windows/d3d12/D3d12Device.h"

#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace mw::native;
using namespace mw::native::encode;
using Microsoft::WRL::ComPtr;

namespace {

using d3d12_test::picture;
using d3d12_test::Uploader;

/// The slices of @p out read with the encoder's parameter sets.
std::vector<HevcSliceFields> slicesOf(const EncoderOutput& out, const VideoEncode12& encoder,
                                      std::string& error)
{
    const std::vector<uint8_t>& headers = encoder.parameterSets();
    HevcSpsFields sps;
    HevcPpsFields pps;
    for (const HevcNalUnit& u : hevcNalUnits(headers.data(), headers.size())) {
        if (u.type() == 33) parseHevcSps(u.data, u.size, sps);
        if (u.type() == 34) parseHevcPps(u.data, u.size, pps);
    }
    std::vector<HevcSliceFields> slices;
    for (const HevcNalUnit& u : hevcNalUnits(out.data, out.size)) {
        if (u.type() > 31) continue;
        HevcSliceFields f;
        error = parseHevcSliceHeader(u.data, u.size, sps, pps, f);
        if (!error.empty()) return {};
        slices.push_back(f);
    }
    return slices;
}

void runOn(const std::shared_ptr<d3d12::D3d12Device>& device, bool tenBit)
{
    VideoEncode12 encoder;
    std::string error;
    EncoderTuning tuning;
    if (!encoder.init(device, Codec::Hevc, 1920, 1080, 60, 20000, tenBit, false, tuning, error)) {
        std::fprintf(stderr, "  %s: %s\n", device->name().c_str(), error.c_str());
        CHECK(false);
        return;
    }
    CHECK_EQ(encoder.codedWidth(), 1920);
    CHECK_EQ(encoder.codedHeight(), 1088);
    ComPtr<ID3D12Resource> input =
        picture(device->device(), encoder.codedWidth(), encoder.codedHeight(), tenBit);
    Uploader uploader;
    if (!input || !uploader.init(*device, input.Get(), error)) {
        std::fprintf(stderr, "  %s: the test's upload: %s\n", device->name().c_str(),
                     error.c_str());
        CHECK(false);
        return;
    }

    std::ofstream dump;
    if (const char* dir = std::getenv("MW_TEST_DUMP_HEVC")) {
        std::string name = device->name();
        for (char& c : name)
            if (!std::isalnum(static_cast<unsigned char>(c))) c = '-';
        dump.open(std::string(dir) + "\\ve12-" + name + (tenBit ? "-main10" : "") + ".hevc",
                  std::ios::binary);
    }

    // The same DPB, run beside the encoder: what the driver was asked for.
    HevcDpb expected(encoder.setup().dpbCapacity, 60);
    const int frames = tenBit ? 8 : 40;
    int keyframes = 0, errors = 0, wrongPoc = 0, wrongReference = 0, badQp = 0, unknownQp = 0;
    int lowQp = 99, highQp = -1;
    for (int n = 0; n < frames; ++n) {
        encoder.releaseOutput(); // the last picture's buffer, read below
        const bool force = n == 30;
        if (n == 20) {
            // Frame 18 never arrived: the next picture reaches back past it.
            std::string why;
            CHECK(encoder.invalidateReference(18, why));
            expected.invalidate(18);
        }
        const HevcDpb::Plan plan = expected.plan(static_cast<uint32_t>(n), force);
        const uint64_t ready = uploader.upload(input.Get(), n, tenBit, error);
        EncoderOutput out;
        if (!ready || !encoder.encode(input.Get(), uploader.fence.fence(), ready, force,
                                      static_cast<uint32_t>(n), out, error)) {
            std::fprintf(stderr, "  %s, frame %d: %s\n", device->name().c_str(), n, error.c_str());
            ++errors;
            break;
        }
        expected.encoded(plan);
        if (dump.is_open())
            dump.write(reinterpret_cast<const char*>(out.data),
                       static_cast<std::streamsize>(out.size));
        CHECK_EQ(out.keyframe, plan.idr);
        // The driver's average QP, its slice's, or -1 where neither says
        // (the Arc): never a number that is not a QP.
        if (out.avgQp == -1) {
            ++unknownQp;
        } else if (out.avgQp < 1 || out.avgQp > 51) {
            ++badQp;
        } else {
            lowQp = (std::min)(lowQp, out.avgQp);
            highQp = (std::max)(highQp, out.avgQp);
        }
        if (out.keyframe) {
            ++keyframes;
            // The parameter sets go out in front of the slices, in one run.
            const std::vector<uint8_t>& headers = encoder.parameterSets();
            CHECK(out.size > headers.size());
            CHECK(std::memcmp(out.data, headers.data(), headers.size()) == 0);
        }
        const std::vector<HevcSliceFields> slices = slicesOf(out, encoder, error);
        if (slices.empty()) {
            std::fprintf(stderr, "  %s, frame %d: %s\n", device->name().c_str(), n, error.c_str());
            ++errors;
            continue;
        }
        const HevcSliceFields& f = slices.front();
        if (!plan.idr && f.pocLsb != (plan.poc & 0xFF)) ++wrongPoc;
        if (!plan.idr) {
            const int32_t wanted =
                static_cast<int32_t>(plan.references[0].poc) - static_cast<int32_t>(plan.poc);
            bool found = false;
            for (const auto& r : f.shortTerm)
                found = found || (r.used && r.deltaPoc == wanted);
            if (!found) ++wrongReference;
            if (n == 20)
                std::fprintf(stderr,
                             "  %s: after the loss of frame 18, frame 20 predicts from %d "
                             "pictures back\n",
                             device->name().c_str(), -wanted);
        }
    }
    CHECK_EQ(errors, 0);
    CHECK_EQ(wrongPoc, 0);
    CHECK_EQ(wrongReference, 0);
    CHECK_EQ(badQp, 0);
    CHECK_EQ(keyframes, tenBit ? 1 : 2);
    CHECK_EQ(encoder.guardLeft(), 0);

    // The driver's rate control where it moves its target in flight, ours
    // where it does not: either way the bitrate changes.
    CHECK_EQ(encoder.ownRateControl(), !encoder.setup().support.rateReconfigurable);
    CHECK_EQ(encoder.qpNotFollowed(), 0);
    std::string refused;
    const bool changed = encoder.setBitrate(10000, refused);
    CHECK(changed);
    if (changed) {
        const uint64_t ready = uploader.upload(input.Get(), frames, tenBit, error);
        EncoderOutput out;
        encoder.releaseOutput();
        CHECK(encoder.encode(input.Get(), uploader.fence.fence(), ready, false,
                             static_cast<uint32_t>(frames), out, error));
    }
    encoder.releaseOutput();
    std::fprintf(
        stderr, "  %s%s: %d frames, %d keyframes, QP %d..%d (%d unknown), %s, bitrate change %s\n",
        device->name().c_str(), tenBit ? " (Main 10)" : "", frames, keyframes, lowQp, highQp,
        unknownQp, encoder.ownRateControl() ? "our rate control" : "the driver's rate control",
        changed ? "taken" : refused.c_str());
    encoder.stop();
}

/// Our rate control on a constant QP (plan §4.6), wherever the driver's own
/// could do (rc12=qp): the driver codes the QP asked; a bitrate change moves
/// it without a new sequence; a still picture — encoded again without a new
/// upload, as the still-screen passes do — is sharpened to QP 18 inside
/// RefineConvergence's cap. Then flat pictures, then movement: the picture far
/// over its budget is coded again — or sent as it is, at reencode=0.
void ownRateOn(const std::shared_ptr<d3d12::D3d12Device>& device)
{
    VideoEncode12 encoder;
    std::string error;
    EncoderTuning tuning;
    tuning.rc12 = EncoderTuning::RateControl12::Qp;
    if (!encoder.init(device, Codec::Hevc, 1920, 1080, 60, 20000, false, false, tuning, error)) {
        std::fprintf(stderr, "  %s, our rate control: %s\n", device->name().c_str(), error.c_str());
        CHECK(false);
        return;
    }
    CHECK(encoder.ownRateControl());
    ComPtr<ID3D12Resource> input =
        picture(device->device(), encoder.codedWidth(), encoder.codedHeight(), false);
    Uploader uploader;
    if (!input || !uploader.init(*device, input.Get(), error)) {
        std::fprintf(stderr, "  %s: the test's upload: %s\n", device->name().c_str(),
                     error.c_str());
        CHECK(false);
        return;
    }
    uint32_t n = 0;
    int failures = 0, outOfRange = 0, said = 0;
    // One picture, ready at @p ready: a new upload's value, or the last one's
    // again — the same picture, as the still-screen passes encode it.
    const auto encode = [&](uint64_t ready, EncoderOutput& out) {
        encoder.releaseOutput();
        if (!encoder.encode(input.Get(), uploader.fence.fence(), ready, false, n++, out, error)) {
            std::fprintf(stderr, "  %s, our rate control, frame %u: %s\n", device->name().c_str(),
                         n - 1, error.c_str());
            ++failures;
            return false;
        }
        if (out.avgQp < QpRateController::kMinQp || out.avgQp > QpRateController::kMaxQp)
            ++outOfRange;
        if (encoder.driverQp() >= 0) ++said;
        return true;
    };

    EncoderOutput out;
    int moving = -1;
    for (int i = 0; i < 30 && failures == 0; ++i)
        if (encode(uploader.upload(input.Get(), i, false, error), out)) moving = out.avgQp;

    // The still-screen burst: x3, the same picture pass after pass.
    std::string refused;
    CHECK(encoder.setBitrate(60000, refused));
    const uint64_t still = uploader.upload(input.Get(), 30, false, error);
    encode(still, out);
    RefineConvergence conv;
    RefineConvergence::Verdict verdict = RefineConvergence::Verdict::Continue;
    int passes = 0, lastQp = out.avgQp;
    size_t passBytes = 0;
    while (verdict == RefineConvergence::Verdict::Continue && failures == 0 && passes < 12) {
        if (!encode(still, out)) break;
        ++passes;
        passBytes += out.size;
        lastQp = out.avgQp;
        verdict = conv.notePass(out.size, out.avgQp);
    }
    CHECK(verdict == RefineConvergence::Verdict::Converged);
    CHECK_EQ(lastQp, QpRateController::kMinQp);
    CHECK_EQ(failures, 0);
    CHECK_EQ(outOfRange, 0);
    CHECK_EQ(encoder.qpNotFollowed(), 0);
    CHECK(said > 0); // all three say their QP under CQP, one way or another
    encoder.releaseOutput();
    std::fprintf(stderr,
                 "  %s, our rate control: moving at QP %d, the still picture sharpened to %d in "
                 "%d passes (%zu KB), the driver's QP said on %d of %u pictures\n",
                 device->name().c_str(), moving, lastQp, passes, passBytes / 1024, said, n);
    encoder.stop();

    // A flat run makes every new picture look cheap; the movement after it is
    // far over its budget: coded again under the overshoot line by default,
    // once at its budget at the bench's refit=0, sent as it is at reencode=0.
    struct Case
    {
        EncoderTuning::Choice reencode;
        EncoderTuning::Choice fit;
        const char* what;
    };
    for (const Case& c :
         {Case{EncoderTuning::Choice::Default, EncoderTuning::Choice::Default, "coded again"},
          Case{EncoderTuning::Choice::Default, EncoderTuning::Choice::Off, "refit=0"},
          Case{EncoderTuning::Choice::Off, EncoderTuning::Choice::Default, "reencode=0"}}) {
        const bool again = c.reencode != EncoderTuning::Choice::Off;
        const bool fit = again && c.fit != EncoderTuning::Choice::Off;
        const char* what = c.what;
        tuning.reencode12 = c.reencode;
        tuning.reencodeFit12 = c.fit;
        if (!encoder.init(device, Codec::Hevc, 1920, 1080, 60, 20000, false, false, tuning,
                          error)) {
            std::fprintf(stderr, "  %s, %s: %s\n", device->name().c_str(), what, error.c_str());
            CHECK(false);
            return;
        }
        n = 0;
        failures = 0;
        for (int i = 0; i < 10 && failures == 0; ++i)
            encode(uploader.upload(input.Get(), i, false, error, /*flat=*/true), out);
        size_t burst = 0;
        for (int i = 0; i < 5 && failures == 0; ++i) {
            encode(uploader.upload(input.Get(), i, false, error), out);
            burst = (std::max)(burst, out.size);
        }
        CHECK_EQ(failures, 0);
        CHECK_EQ(encoder.qpNotFollowed(), 0);
        CHECK_EQ(encoder.rateController().reencodeFit(), fit);
        if (again) {
            CHECK(encoder.reencoded() >= 1);
        } else {
            CHECK_EQ(encoder.reencoded(), 0);
            CHECK(encoder.rateController().strongOvershoots() >= 1);
        }
        if (!fit) CHECK_EQ(encoder.reencodedTwice(), 0);
        encoder.releaseOutput();
        std::fprintf(stderr,
                     "  %s, %s: %d picture(s) coded again (%d twice), %d sent far over budget, "
                     "largest %zu KB against %llu KB a frame\n",
                     device->name().c_str(), what, encoder.reencoded(), encoder.reencodedTwice(),
                     encoder.rateController().strongOvershoots(), burst / 1024,
                     static_cast<unsigned long long>(encoder.rateController().frameBits() / 8192));
        encoder.stop();
    }
    tuning.reencodeFit12 = EncoderTuning::Choice::Default;
}

std::string fileSafe(std::string name)
{
    for (char& c : name)
        if (!std::isalnum(static_cast<unsigned char>(c))) c = '-';
    return name;
}

/// H.264 (plan C9.1): the IDR with its SPS and PPS in front, P pictures
/// numbered in order, every one kept as the next one's reference; a loss has
/// no older picture to reach back to — the next picture is an IDR — and a
/// forced keyframe; the header guard passed, a bitrate change taken. The
/// stream is dumped beside its input pictures for the pixel proof.
void h264On(const std::shared_ptr<d3d12::D3d12Device>& device)
{
    VideoEncode12 encoder;
    std::string error;
    EncoderTuning tuning;
    if (!encoder.init(device, Codec::H264, 1920, 1080, 60, 20000, false, false, tuning, error)) {
        std::fprintf(stderr, "  %s, H.264: %s\n", device->name().c_str(), error.c_str());
        CHECK(false);
        return;
    }
    CHECK(encoder.codec() == Codec::H264);
    CHECK_EQ(encoder.codedWidth(), 1920);
    CHECK_EQ(encoder.codedHeight(), 1088);
    CHECK(!encoder.supportsReferenceInvalidation());
    // The parameter sets read back as they were written: 1088 coded lines,
    // 1080 shown, one picture kept.
    const std::vector<uint8_t>& headers = encoder.parameterSets();
    H264SpsFields sps;
    H264PpsFields pps;
    int units = 0;
    for (const HevcNalUnit& u : hevcNalUnits(headers.data(), headers.size())) {
        ++units;
        if (h264NalType(u) == 7) CHECK(parseH264Sps(u.data, u.size, sps).empty());
        if (h264NalType(u) == 8) CHECK(parseH264Pps(u.data, u.size, pps).empty());
    }
    CHECK_EQ(units, 2);
    CHECK_EQ(sps.profileIdc, 100u);
    CHECK_EQ(sps.pocType, 2u);
    CHECK_EQ(sps.log2MaxFrameNum, 16u);
    CHECK_EQ(sps.maxRefFrames, 1u);
    CHECK_EQ(sps.heightMbs, 68u);
    CHECK_EQ(pps.cabac, encoder.h264Setup().cabac);
    CHECK_EQ(pps.transform8x8, encoder.h264Setup().transform8x8);

    ComPtr<ID3D12Resource> input =
        picture(device->device(), encoder.codedWidth(), encoder.codedHeight(), false);
    Uploader uploader;
    if (!input || !uploader.init(*device, input.Get(), error)) {
        std::fprintf(stderr, "  %s: the test's upload: %s\n", device->name().c_str(),
                     error.c_str());
        CHECK(false);
        return;
    }
    std::ofstream dump;
    std::ofstream dumpInput;
    if (const char* dir = std::getenv("MW_TEST_DUMP_HEVC")) {
        const std::string base = std::string(dir) + "\\ve12-" + fileSafe(device->name()) + "-h264";
        dump.open(base + ".h264", std::ios::binary);
        dumpInput.open(base + ".nv12", std::ios::binary);
    }

    HevcDpb expected(1, 60);
    const int frames = 40;
    int keyframes = 0, errors = 0, wrongFrameNum = 0, notKept = 0, badQp = 0, unknownQp = 0;
    int lowQp = 99, highQp = -1;
    bool lossHealed = false;
    for (int n = 0; n < frames; ++n) {
        encoder.releaseOutput();
        const bool force = n == 30;
        if (n == 20) {
            // Frame 18 never arrived: with one picture kept, nothing older is
            // left, and the next picture is an IDR — asked or not.
            std::string why;
            CHECK(!encoder.invalidateReference(18, why));
            CHECK(!expected.invalidate(18));
        }
        const HevcDpb::Plan plan = expected.plan(static_cast<uint32_t>(n), force);
        if (n == 20) lossHealed = plan.idr;
        const uint64_t ready = uploader.upload(input.Get(), n, false, error);
        EncoderOutput out;
        if (!ready || !encoder.encode(input.Get(), uploader.fence.fence(), ready, force,
                                      static_cast<uint32_t>(n), out, error)) {
            std::fprintf(stderr, "  %s, H.264 frame %d: %s\n", device->name().c_str(), n,
                         error.c_str());
            ++errors;
            break;
        }
        expected.encoded(plan);
        if (dump.is_open())
            dump.write(reinterpret_cast<const char*>(out.data),
                       static_cast<std::streamsize>(out.size));
        if (dumpInput.is_open()) {
            const std::vector<uint8_t> nv12 = d3d12_test::nv12Frame(1920, 1080, n);
            dumpInput.write(reinterpret_cast<const char*>(nv12.data()),
                            static_cast<std::streamsize>(nv12.size()));
        }
        CHECK_EQ(out.keyframe, plan.idr);
        if (out.avgQp == -1) {
            ++unknownQp;
        } else if (out.avgQp < 1 || out.avgQp > 51) {
            ++badQp;
        } else {
            lowQp = (std::min)(lowQp, out.avgQp);
            highQp = (std::max)(highQp, out.avgQp);
        }
        if (out.keyframe) {
            ++keyframes;
            CHECK(out.size > headers.size());
            CHECK(std::memcmp(out.data, headers.data(), headers.size()) == 0);
        }
        for (const HevcNalUnit& u : hevcNalUnits(out.data, out.size)) {
            const uint32_t type = h264NalType(u);
            if (type != 1 && type != 5) continue;
            H264SliceFields f;
            const std::string wrong = parseH264SliceHeader(u.data, u.size, sps, pps, f);
            if (!wrong.empty()) {
                std::fprintf(stderr, "  %s, H.264 frame %d: %s\n", device->name().c_str(), n,
                             wrong.c_str());
                ++errors;
                break;
            }
            if (f.frameNum != (plan.poc & 0xFFFFu)) ++wrongFrameNum;
            if (f.nalRefIdc == 0 || f.adaptiveMarking) ++notKept;
        }
    }
    CHECK_EQ(errors, 0);
    CHECK_EQ(wrongFrameNum, 0);
    CHECK_EQ(notKept, 0);
    CHECK_EQ(badQp, 0);
    CHECK(lossHealed);
    CHECK_EQ(keyframes, 3); // the first, after the loss, the forced one
    CHECK_EQ(encoder.guardLeft(), 0);
    CHECK_EQ(encoder.ownRateControl(), !encoder.setup().support.rateReconfigurable);
    CHECK_EQ(encoder.qpNotFollowed(), 0);
    std::string refused;
    const bool changed = encoder.setBitrate(10000, refused);
    CHECK(changed);
    if (changed) {
        const uint64_t ready = uploader.upload(input.Get(), frames, false, error);
        EncoderOutput out;
        encoder.releaseOutput();
        CHECK(encoder.encode(input.Get(), uploader.fence.fence(), ready, false,
                             static_cast<uint32_t>(frames), out, error));
    }
    encoder.releaseOutput();
    std::fprintf(stderr,
                 "  %s, H.264: %d frames, %d keyframes, QP %d..%d (%d unknown), %s, %s%s, "
                 "bitrate change %s\n",
                 device->name().c_str(), frames, keyframes, lowQp, highQp, unknownQp,
                 encoder.ownRateControl() ? "our rate control" : "the driver's rate control",
                 encoder.h264Setup().cabac ? "CABAC" : "CAVLC",
                 encoder.h264Setup().transform8x8 ? ", 8x8 transform" : "",
                 changed ? "taken" : refused.c_str());
    encoder.stop();
}

/// 1440 lines end inside a coding tree block of 64 (the Arc's, the AMD
/// iGPU's): the driver takes the size in whole CTBs, and encodes at it.
void wholeBlocksOn(const std::shared_ptr<d3d12::D3d12Device>& device)
{
    VideoEncode12 encoder;
    std::string error;
    EncoderTuning tuning;
    if (!encoder.init(device, Codec::Hevc, 2560, 1440, 120, 20000, false, false, tuning, error)) {
        std::fprintf(stderr, "  %s, 2560x1440: %s\n", device->name().c_str(), error.c_str());
        CHECK(false);
        return;
    }
    const int ctb = 1 << encoder.setup().blocks.log2MaxCodingBlock;
    CHECK_EQ(encoder.codedWidth(), 2560);
    CHECK_EQ(encoder.codedHeight() % ctb, 0);
    CHECK(encoder.codedHeight() >= 1440 && encoder.codedHeight() < 1440 + ctb);
    ComPtr<ID3D12Resource> input =
        picture(device->device(), encoder.codedWidth(), encoder.codedHeight(), false);
    Uploader uploader;
    if (!input || !uploader.init(*device, input.Get(), error)) {
        std::fprintf(stderr, "  %s: the test's upload: %s\n", device->name().c_str(),
                     error.c_str());
        CHECK(false);
        return;
    }
    int encoded = 0;
    for (int n = 0; n < 4; ++n) {
        encoder.releaseOutput();
        const uint64_t ready = uploader.upload(input.Get(), n, false, error);
        EncoderOutput out;
        if (ready && encoder.encode(input.Get(), uploader.fence.fence(), ready, false,
                                    static_cast<uint32_t>(n), out, error))
            ++encoded;
        else
            std::fprintf(stderr, "  %s, 2560x1440 frame %d: %s\n", device->name().c_str(), n,
                         error.c_str());
    }
    CHECK_EQ(encoded, 4);
    encoder.releaseOutput();
    std::fprintf(stderr, "  %s: 2560x1440 coded %dx%d (CTB %d)\n", device->name().c_str(),
                 encoder.codedWidth(), encoder.codedHeight(), ctb);
    encoder.stop();
}

} // namespace
#endif

// D3D12 Video Encode as the product drives it (plan C4.6), on every GPU that
// has it: the IDR with its parameter sets in front, P pictures numbered in
// order and predicting from the picture HevcDpb chose — after a loss too —
// the header guard passed, a forced keyframe, a bitrate change (the driver's
// rate control or ours); a short Main 10 run; 2560x1440 in whole coding tree
// blocks; our rate control forced on, a still picture sharpened, an overshoot
// coded again; H.264, a loss healed by an IDR (C9.1). MW_TEST_DUMP_HEVC=<dir>
// writes the streams out for ffmpeg, H.264's beside its input pictures.
void run_video_encode12_tests()
{
#if defined(_WIN32)
    SECTION("VideoEncode12 — HEVC and H.264 through D3D12 Video Encode, on each GPU");

    ComPtr<IDXGIFactory4> factory;
    if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        std::fprintf(stderr, "  no DXGI factory — skipped\n");
        return;
    }
    std::set<uint64_t> seen;
    int ran = 0;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        const uint64_t luid = d3d12::luidValue(desc.AdapterLuid);
        // A virtual display driver's adapter is the GPU rendering for it,
        // listed again under a LUID of its own (IndirectDisplay.h).
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || !seen.insert(luid).second ||
            mw::native::platform::isIndirectDisplayOnly(desc.AdapterLuid))
            continue;
        std::string error;
        const std::shared_ptr<d3d12::D3d12Device> device =
            d3d12::D3d12Device::forAdapter(adapter.Get(), error);
        ComPtr<ID3D12VideoDevice3> video;
        if (!device || FAILED(device->device()->QueryInterface(IID_PPV_ARGS(&video)))) continue;
        ++ran;
        runOn(device, false);
        runOn(device, true);
        wholeBlocksOn(device);
        ownRateOn(device);
        h264On(device);
    }
    if (ran == 0) std::fprintf(stderr, "  no GPU with D3D12 Video Encode here — skipped\n");
#endif
}
