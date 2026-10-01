/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#include "mw/native/NativeHost.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(MW_NATIVE_LINUX_AUDIO) && defined(MW_NATIVE_TESTS_HAVE_OPUS)
#include <opus.h>
#endif
#if defined(MW_NATIVE_LINUX_VULKAN)
#include "platform/linux/vulkan/VulkanDevice.h"
#endif

using namespace mw::native;

// A whole Linux session, the way the backend would run one: probe, select,
// create, stream for a moment, stop. The frames go to a file so the machine's
// own decoder can look at them afterwards — that is the only proof of
// orientation and colour this side of a browser. The audio packets are decoded
// back with libopus and their peak printed: a paced silence and a captured tone
// have the same cadence, and only the signal tells them apart.

void run_linux_session_tests()
{
    SECTION("Linux — a whole session through NativeHost, frames to a file");

#if !defined(MW_NATIVE_LINUX_GFX)
    std::fprintf(stderr, "  skipped: Linux graphics backend not built\n");
#else
    const Capabilities caps = NativeHost::probe();
    if (!caps.available) {
        std::fprintf(stderr, "  skipped: %s (%s)\n", toString(caps.reason),
                     caps.diagnostic.c_str());
        return;
    }
    // Either route is legitimate, and which one appears says something about
    // the machine: KMS when this process may read the scanout, PipeWire when it
    // may not and a portal answers instead — the AppImage's situation, and the
    // one a plain copy of this binary reproduces exactly (no file capability).
    const bool viaPortal = caps.capture == CaptureApi::PipeWire;
    CHECK((caps.capture == CaptureApi::Kms || viaPortal));
    CHECK(!caps.displays.empty());
    CHECK(!caps.gpus.empty());

    const DisplayInfo* display = nullptr;
    for (const DisplayInfo& d : caps.displays)
        if (d.primary) display = &d;
    if (!display) display = &caps.displays.front();
    const GpuInfo* gpu = caps.gpuFor(*display);
    CHECK(gpu != nullptr);
    if (!gpu || gpu->encoders.empty()) {
        std::fprintf(stderr, "  skipped: the display's GPU has no encoder\n");
        return;
    }
    std::fprintf(stderr, "  %s — %s on %s\n", display->label.c_str(), display->detail.c_str(),
                 gpu->name.c_str());

    SessionConfig config;
    config.displayId = display->id;
    config.fps = 60;
    config.bitrateKbps = 20000;
    config.clientCodecs = {Codec::H264};
    config.intraRefresh = true; // asked; the driver decides, and SessionInfo says

    // The portal route needs a consent. A real host stores the grant it was
    // given and replays it; a test has nowhere to store one, so it takes it
    // from the environment — and without it there is nothing to test but a
    // dialog nobody will answer, which would hang rather than fail.
    if (viaPortal) {
        const char* token = std::getenv("MW_PORTAL_RESTORE_TOKEN");
        if (!token || !*token) {
            std::fprintf(stderr, "  skipped: the portal would raise a consent dialog — set "
                                 "MW_PORTAL_RESTORE_TOKEN to a grant from an earlier run\n");
            return;
        }
        config.portalRestoreToken = token;
        std::fprintf(stderr, "  portal route, replaying a stored grant\n");
    }

    std::atomic<int> frames{0};
    std::atomic<int> keyframes{0};
    std::atomic<bool> firstWasKeyframe{false};
    std::atomic<bool> orderOk{true};
    std::atomic<uint32_t> lastNumber{0};
    std::atomic<int64_t> worstProcessingUs{0};
    std::ofstream out("/tmp/mw-linux-session.h264", std::ios::binary | std::ios::trunc);
    std::string ended;

    // The host's sound, through PipeWire. What is CHECKED is the cadence —
    // the relay advances the RTP clock by one frame per packet, so 200 a
    // second is the contract whether the host plays anything or not. What is
    // printed is the decoded peak, for the bench: a tone played into the
    // default output must show up here as a number well above zero.
    std::atomic<int> audioPackets{0};
    std::atomic<size_t> audioBytes{0};
    std::atomic<bool> audioFrameSizeOk{true};
    std::mutex audioMutex;
    std::vector<std::vector<uint8_t>> audioCopies;
    AudioCallback onAudio = [&](const AudioPacket& p) {
        audioPackets.fetch_add(1);
        audioBytes.fetch_add(p.size);
        if (p.samplesPerChannel != 240) audioFrameSizeOk.store(false);
        std::lock_guard<std::mutex> lock(audioMutex);
        if (audioCopies.size() < 1000) audioCopies.emplace_back(p.data, p.data + p.size);
    };

    std::string error;
    std::unique_ptr<Session> session = NativeHost::createSession(
        config,
        [&](const EncodedFrame& f) {
            const int n = frames.fetch_add(1);
            if (f.keyframe) keyframes.fetch_add(1);
            if (n == 0) firstWasKeyframe.store(f.keyframe);
            if (n > 0 && f.frameNumber != lastNumber.load() + 1) orderOk.store(false);
            lastNumber.store(f.frameNumber);
            if (f.processingUs() > worstProcessingUs.load())
                worstProcessingUs.store(f.processingUs());
            out.write(reinterpret_cast<const char*>(f.data), static_cast<std::streamsize>(f.size));
        },
        onAudio, nullptr, nullptr, [&](const std::string& reason) { ended = reason; }, error);
    if (!session) {
        std::fprintf(stderr, "  createSession failed: %s\n", error.c_str());
        CHECK(false);
        return;
    }
    // Registered before start(), which is where the grant happens. Nothing is
    // EXPECTED here: this run replays a stored token, so the portal raises no
    // dialog and has no new consent to hand back. What is checked is the other
    // half — that a session which asked for nothing does not report a grant,
    // because a host that trusted a spurious one would overwrite a token that
    // works with one that may not.
    std::atomic<int> grants{0};
    std::string grantedToken;
    session->setPortalGrantCallback([&](const std::string& token) {
        grants.fetch_add(1);
        grantedToken = token;
    });

    // The audio tap opens inside start(): its packets are counted against the
    // time from here to the end of stop().
    const auto startedAt = std::chrono::steady_clock::now();
    CHECK(session->start(error));
    if (!error.empty()) std::fprintf(stderr, "  %s\n", error.c_str());
    if (grants.load() > 0)
        std::fprintf(stderr, "  portal granted a NEW consent (%zu bytes)\n", grantedToken.size());
    CHECK_EQ(grants.load(), 0);

    const SessionInfo& info = session->info();
    std::fprintf(stderr, "  session: %dx%d %s via %s on %s, intra-refresh %s, capture %s\n",
                 info.width, info.height, toString(info.codec), toString(info.encoder),
                 info.gpuName.c_str(), info.intraRefresh ? "on" : "off", toString(info.capture));
    // The engine's own chain, nothing asked: on AMD off the scanout, the split
    // route since §9-20 — checked in its own section below.
    std::fprintf(stderr, "  route \"%s\"%s\n    reason: %s\n", info.videoRoute.c_str(),
                 info.videoPipelineRefused ? " (refused)" : "", info.videoPipelineReason.c_str());
    CHECK_EQ(info.width, display->width);
    CHECK_EQ(info.height, display->height);
    if (viaPortal) {
        // ⚠️ The portal route reports what it ENDED UP with, which is not what
        // the Selector chose: a compositor that hands over shared memory rather
        // than a DMA-BUF makes the GPU pair impossible, and the session falls to
        // the CPU one. GNOME 42 did exactly that on the bench, until the
        // session offered the GPU's modifiers (29/09/2026): GNOME 46 then
        // hands DMA-BUF, and the GPU pair runs.
        CHECK_EQ(static_cast<int>(info.capture), static_cast<int>(CaptureApi::PipeWire));
        // OpenH264 writes its own reference list, so a lost frame costs a
        // keyframe here — and SessionInfo says so rather than promising a
        // repair the client would wait for in vain.
        if (info.encoder == EncoderApi::Software) CHECK(!info.referenceInvalidation);
    } else {
        CHECK_EQ(static_cast<int>(info.capture), static_cast<int>(CaptureApi::Kms));
        CHECK_EQ(static_cast<int>(info.encoder), static_cast<int>(EncoderApi::VaApi));
        // VA-API hands the reference list to the application picture by picture,
        // so every encoder on this path can heal a named loss with a delta. What
        // the driver DOES with that list is a bench question (§19.13); that the
        // session offers it, and therefore that /start promises it to the
        // client, is this one's.
        CHECK(info.referenceInvalidation);
    }

    // Two seconds. A still desktop yields the first frame plus the floor at
    // 2 fps, plus whatever the refinement burst adds; anything moving yields
    // more. Either way there are frames, and the first is a keyframe.
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Name a frame as lost and check the stream does NOT answer with a
    // keyframe: the point of reference invalidation is that the repair is an
    // ordinary delta. The count is taken before and after, around a window long
    // enough for several pictures.
    const int keyframesBefore = keyframes.load();
    const uint32_t lost = lastNumber.load();
    session->invalidateReference(lost);
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    const int keyframesAfterInvalidation = keyframes.load() - keyframesBefore;
    std::fprintf(stderr, "  named frame %u as lost — %d keyframe(s) followed\n", lost,
                 keyframesAfterInvalidation);
    // Only where the session promised a delta repair. Where it did not — the
    // CPU pair — a keyframe is the correct answer and demanding zero would be
    // testing the opposite of what SessionInfo told the client.
    if (info.referenceInvalidation) CHECK_EQ(keyframesAfterInvalidation, 0);

    session->requestKeyframe();
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    session->stop();
    const double streamedS =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - startedAt).count();
    out.close();

    std::fprintf(stderr, "  %d frame(s), %d keyframe(s) in %.2f s, worst host latency %.2f ms%s\n",
                 frames.load(), keyframes.load(), streamedS, worstProcessingUs.load() / 1000.0,
                 ended.empty() ? "" : (", ended: " + ended).c_str());
    CHECK(ended.empty());
    CHECK(frames.load() >= 3);
    CHECK(keyframes.load() >= 2); // the first, and the one asked for
    CHECK(firstWasKeyframe.load());
    CHECK(orderOk.load());

#if defined(MW_NATIVE_LINUX_AUDIO)
    if (!info.audio) {
        // No PipeWire daemon for this user (a bare console, a CI runner): the
        // session said so in the log and streams silent. Nothing to check.
        std::fprintf(stderr, "  audio: skipped — the session has no audio (see the log above)\n");
    } else {
        // 200 packets a second over the time the session ran, start() and
        // stop() included — measured, not assumed: 2.7 s was assumed here
        // when the named loss added 0.6 s to the run, and a session that
        // took a little longer to start went over the bound (690 to 702
        // packets, 28/09/2026). The bounds leave room for the pacer's
        // start-up, and would catch a tap that fired in bursts or a pacer
        // that stalled.
        const double expected = 200.0 * streamedS;
        std::fprintf(stderr, "  audio: %d packet(s), %zu bytes (%.1f/s over %.2f s)\n",
                     audioPackets.load(), audioBytes.load(), audioPackets.load() / streamedS,
                     streamedS);
        CHECK(audioPackets.load() >= static_cast<int>(expected * 0.75));
        CHECK(audioPackets.load() <= static_cast<int>(expected * 1.1));
        CHECK(audioFrameSizeOk.load());

#if defined(MW_NATIVE_TESTS_HAVE_OPUS)
        // Decode what went out and look at it. A quiet host is legal (peak
        // ~0); a tone playing on the host must read well above zero here, and
        // a packet that is 3 bytes of silence while the host plays is the bug
        // §20.8 describes.
        int opusError = 0;
        ::OpusDecoder* decoder = opus_decoder_create(48000, 2, &opusError);
        if (decoder && opusError == OPUS_OK) {
            std::vector<float> pcm(240 * 2);
            float peak = 0.0f;
            double sumSquares = 0.0;
            size_t samples = 0;
            std::lock_guard<std::mutex> lock(audioMutex);
            for (const std::vector<uint8_t>& packet : audioCopies) {
                const int n =
                    opus_decode_float(decoder, packet.data(),
                                      static_cast<opus_int32>(packet.size()), pcm.data(), 240, 0);
                if (n <= 0) continue;
                for (int i = 0; i < n * 2; ++i) {
                    peak = std::max(peak, std::fabs(pcm[static_cast<size_t>(i)]));
                    sumSquares += static_cast<double>(pcm[static_cast<size_t>(i)]) *
                                  pcm[static_cast<size_t>(i)];
                }
                samples += static_cast<size_t>(n) * 2;
            }
            opus_decoder_destroy(decoder);
            std::fprintf(stderr,
                         "  audio decoded: %zu packet(s), peak %.3f, RMS %.4f — play a tone into "
                         "the default output to see it here\n",
                         audioCopies.size(), peak,
                         samples > 0 ? std::sqrt(sumSquares / static_cast<double>(samples)) : 0.0);
        }
#endif
    }
#endif

    std::fprintf(stderr, "  wrote /tmp/mw-linux-session.h264 — decode it to look at the picture\n");

    // Whether this machine's route can encode anything but H.264 at all. The
    // portal handing over shared memory forces the CPU pair, and OpenH264 does
    // H.264 only — so the codec sections below have nothing to test, and a
    // failure there would be the test disagreeing with what the session just
    // told the client.
    const bool h264Only = info.encoder == EncoderApi::Software;

    // ── And in AV1 ──────────────────────────────────────────────────────────
    {
        SECTION("Linux — the same session in AV1");
        const bool offersAv1 = !h264Only && std::find(gpu->codecs.begin(), gpu->codecs.end(),
                                                      Codec::Av1) != gpu->codecs.end();
        if (h264Only) {
            std::fprintf(stderr, "  skipped: this route encodes H.264 only (CPU pair)\n");
        } else if (!offersAv1) {
            std::fprintf(stderr, "  skipped: this GPU does not offer AV1\n");
        } else {
            SessionConfig av1Config = config;
            av1Config.clientCodecs = {Codec::Av1};
            std::atomic<int> av1Frames{0};
            std::atomic<int> av1Keyframes{0};
            std::ofstream av1Out("/tmp/mw-linux-session.av1", std::ios::binary | std::ios::trunc);
            std::string av1Ended;
            std::string av1Error;
            std::unique_ptr<Session> av1Session = NativeHost::createSession(
                av1Config,
                [&](const EncodedFrame& f) {
                    av1Frames.fetch_add(1);
                    if (f.keyframe) av1Keyframes.fetch_add(1);
                    av1Out.write(reinterpret_cast<const char*>(f.data),
                                 static_cast<std::streamsize>(f.size));
                },
                nullptr, nullptr, nullptr, [&](const std::string& reason) { av1Ended = reason; },
                av1Error);
            if (!av1Session) {
                std::fprintf(stderr, "  createSession failed: %s\n", av1Error.c_str());
                CHECK(false);
            } else if (!av1Session->start(av1Error)) {
                std::fprintf(stderr, "  start failed: %s\n", av1Error.c_str());
                CHECK(false);
            } else {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                av1Session->requestKeyframe();
                std::this_thread::sleep_for(std::chrono::milliseconds(700));
                av1Session->stop();
                av1Out.close();
                std::fprintf(stderr, "  %d frame(s), %d keyframe(s)%s\n", av1Frames.load(),
                             av1Keyframes.load(),
                             av1Ended.empty() ? "" : (", ended: " + av1Ended).c_str());
                CHECK(av1Ended.empty());
                CHECK(av1Frames.load() >= 3);
                std::fprintf(stderr, "  wrote /tmp/mw-linux-session.av1\n");
            }
        }
    }

    // ── The same session again, in HEVC ─────────────────────────────────────
    //
    // Only where the GPU claims it: the probe advertises HEVC exactly when
    // VaapiEncoder has a path for it, so if this section is skipped the codec
    // was never offered to a client either. What it proves is the part a unit
    // test can prove — that the driver accepts the sequence, picture and slice
    // parameters and returns a bitstream in decode order. Whether a browser
    // decodes the result is a bench question, not this one.
    {
        SECTION("Linux — the same session in HEVC");
        const bool offersHevc = !h264Only && std::find(gpu->codecs.begin(), gpu->codecs.end(),
                                                       Codec::Hevc) != gpu->codecs.end();
        if (h264Only) {
            // The GPU offers HEVC; the ROUTE cannot use it. A portal that hands
            // over shared memory keeps the frame out of EGL's reach, so the CPU
            // pair encodes — and it does H.264 only.
            //
            // So what is checked here is the DOWNGRADE, not HEVC: a browser
            // that prefers HEVC and also decodes H.264 (every browser) must get
            // a session in H.264, not a refusal. Left to itself the Selector
            // picks HEVC — the GPU really does offer it — and the CPU pair then
            // answers "OpenH264 encodes H.264 only" and the session dies before
            // a single frame. That is what happened on the first real browser
            // session through the portal, 08/09/2026.
            std::fprintf(stderr, "  the CPU pair encodes H.264 only — checking the downgrade\n");
            SessionConfig mixed = config;
            mixed.clientCodecs = {Codec::Hevc, Codec::H264};
            std::atomic<int> mixedFrames{0};
            std::string mixedError;
            std::unique_ptr<Session> mixedSession = NativeHost::createSession(
                mixed, [&](const EncodedFrame&) { mixedFrames.fetch_add(1); }, nullptr, nullptr,
                nullptr, nullptr, mixedError);
            CHECK(mixedSession != nullptr);
            if (mixedSession) {
                const bool started = mixedSession->start(mixedError);
                if (!started) std::fprintf(stderr, "  start failed: %s\n", mixedError.c_str());
                CHECK(started);
                // The client is told what it will really receive, never what
                // the GPU could have done.
                CHECK(mixedSession->info().codec == Codec::H264);
                CHECK(mixedSession->info().encoder == EncoderApi::Software);
                std::this_thread::sleep_for(std::chrono::seconds(2));
                std::fprintf(stderr, "  downgraded to %s, %d frame(s)\n",
                             toString(mixedSession->info().codec), mixedFrames.load());
                CHECK(mixedFrames.load() > 0);
                mixedSession->stop();
            }

            // And the honest refusal: a client that named HEVC and nothing else
            // cannot be served by this route, and must be told so rather than
            // sent a stream it cannot decode.
            SessionConfig hevcOnly = config;
            hevcOnly.clientCodecs = {Codec::Hevc};
            std::string refusal;
            std::unique_ptr<Session> refused = NativeHost::createSession(
                hevcOnly, [](const EncodedFrame&) {}, nullptr, nullptr, nullptr, nullptr, refusal);
            const bool refusedAtStart = refused && !refused->start(refusal);
            CHECK((refused == nullptr || refusedAtStart));
            std::fprintf(stderr, "  HEVC-only client refused: %s\n", refusal.c_str());
        } else if (!offersHevc) {
            std::fprintf(stderr, "  skipped: this GPU does not offer HEVC\n");
        } else {
            SessionConfig hevcConfig = config;
            hevcConfig.clientCodecs = {Codec::Hevc};

            std::atomic<int> hevcFrames{0};
            std::atomic<int> hevcKeyframes{0};
            std::atomic<bool> hevcFirstWasKeyframe{false};
            std::atomic<bool> hevcOrderOk{true};
            std::atomic<uint32_t> hevcLast{0};
            std::ofstream hevcOut("/tmp/mw-linux-session.hevc", std::ios::binary | std::ios::trunc);
            std::string hevcEnded;
            std::string hevcError;

            std::unique_ptr<Session> hevcSession = NativeHost::createSession(
                hevcConfig,
                [&](const EncodedFrame& f) {
                    const int n = hevcFrames.fetch_add(1);
                    if (f.keyframe) hevcKeyframes.fetch_add(1);
                    if (n == 0) hevcFirstWasKeyframe.store(f.keyframe);
                    if (n > 0 && f.frameNumber != hevcLast.load() + 1) hevcOrderOk.store(false);
                    hevcLast.store(f.frameNumber);
                    hevcOut.write(reinterpret_cast<const char*>(f.data),
                                  static_cast<std::streamsize>(f.size));
                },
                nullptr, nullptr, nullptr, [&](const std::string& reason) { hevcEnded = reason; },
                hevcError);
            if (!hevcSession) {
                std::fprintf(stderr, "  createSession failed: %s\n", hevcError.c_str());
                CHECK(false);
            } else {
                CHECK(hevcSession->start(hevcError));
                const SessionInfo& hevcInfo = hevcSession->info();
                std::fprintf(stderr, "  session: %dx%d %s via %s\n", hevcInfo.width,
                             hevcInfo.height, toString(hevcInfo.codec), toString(hevcInfo.encoder));
                CHECK_EQ(static_cast<int>(hevcInfo.codec), static_cast<int>(Codec::Hevc));
                std::this_thread::sleep_for(std::chrono::seconds(2));

                // The same named loss as in H.264. HEVC is where it is harder:
                // the decoder keeps only the pictures each slice's reference set
                // lists, so the repair works only if the older one was kept.
                // Drop the named frame from the file and the pictures after it
                // must decode to the same bytes as with it.
                const int hevcKeyframesBefore = hevcKeyframes.load();
                const uint32_t hevcLost = hevcLast.load();
                hevcSession->invalidateReference(hevcLost);
                std::this_thread::sleep_for(std::chrono::milliseconds(600));
                std::fprintf(stderr, "  named frame %u as lost — %d keyframe(s) followed\n",
                             hevcLost, hevcKeyframes.load() - hevcKeyframesBefore);
                if (hevcInfo.referenceInvalidation)
                    CHECK_EQ(hevcKeyframes.load() - hevcKeyframesBefore, 0);

                hevcSession->requestKeyframe();
                std::this_thread::sleep_for(std::chrono::milliseconds(700));
                hevcSession->stop();
                hevcOut.close();

                std::fprintf(stderr, "  %d frame(s), %d keyframe(s)%s\n", hevcFrames.load(),
                             hevcKeyframes.load(),
                             hevcEnded.empty() ? "" : (", ended: " + hevcEnded).c_str());
                CHECK(hevcEnded.empty());
                CHECK(hevcFrames.load() >= 3);
                CHECK(hevcKeyframes.load() >= 2);
                CHECK(hevcFirstWasKeyframe.load());
                CHECK(hevcOrderOk.load());
                std::fprintf(stderr, "  wrote /tmp/mw-linux-session.hevc\n");
            }
        }
    }

    // ── A smaller stream: the resample pass, and what it costs here ──
    //
    // 720p of a bigger display goes through the Lanczos-2 pass by default on
    // the GPU tier, and through the plain bilinear one under MW_SCALER=
    // bilinear. Both are run, through each conversion the build has — GL, and
    // Vulkan compute where it is built (AMD's own since §9-20), so that the
    // table's choice never leaves the other untested — and the conversion
    // time per frame is printed side by side: the figure that decides whether
    // the 780M keeps the default (design §28.3). Either pass ends in a wait
    // for the GPU, so convertedUs − submittedUs is the GPU's work too.
    if (display && display->height > 720) {
        SECTION("Linux — a 720p stream of a bigger display, Lanczos-2 then bilinear, through each "
                "conversion");
        struct Run
        {
            EncoderTuning::ConvertLinux conversion;
            const char* scaler;
        };
        const Run runs[] = {
            {EncoderTuning::ConvertLinux::Gl, "lanczos2"},
            {EncoderTuning::ConvertLinux::Gl, "bilinear"},
#if defined(MW_NATIVE_LINUX_VULKAN)
            {EncoderTuning::ConvertLinux::Vulkan, "lanczos2"},
            {EncoderTuning::ConvertLinux::Vulkan, "bilinear"},
#endif
        };
        for (const Run& run : runs) {
            const char* scaler = run.scaler;
            ::setenv("MW_SCALER", scaler, 1);
            SessionConfig small = config;
            small.clientCodecs = {Codec::H264};
            small.height = 720;
            small.width = 0;
            small.tuning.convertLinux = run.conversion;

            std::atomic<int> smallFrames{0};
            std::atomic<int64_t> convertSumUs{0};
            std::atomic<int64_t> convertWorstUs{0};
            std::atomic<int64_t> encodeSumUs{0};
            std::string smallEnded;
            std::string smallError;
            std::unique_ptr<Session> smallSession = NativeHost::createSession(
                small,
                [&](const EncodedFrame& f) {
                    smallFrames.fetch_add(1);
                    const int64_t convert = f.convertedUs - f.submittedUs;
                    convertSumUs.fetch_add(convert);
                    int64_t worst = convertWorstUs.load();
                    while (convert > worst &&
                           !convertWorstUs.compare_exchange_weak(worst, convert)) {}
                    encodeSumUs.fetch_add(f.encodedUs - f.convertedUs);
                },
                nullptr, nullptr, nullptr, [&](const std::string& reason) { smallEnded = reason; },
                smallError);
            if (!smallSession) {
                std::fprintf(stderr, "  createSession failed: %s\n", smallError.c_str());
                CHECK(false);
                continue;
            }
            CHECK(smallSession->start(smallError));
            const SessionInfo& smallInfo = smallSession->info();
            std::this_thread::sleep_for(std::chrono::seconds(2));
            smallSession->requestKeyframe();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            smallSession->stop();
            const int n = smallFrames.load();
            std::fprintf(stderr,
                         "  %s, %s: %dx%d, %d frame(s), convert mean %.2f ms worst %.2f ms, "
                         "encode mean %.2f ms%s\n",
                         smallInfo.videoRoute.c_str(), scaler, smallInfo.width, smallInfo.height, n,
                         n ? convertSumUs.load() / 1000.0 / n : 0.0, convertWorstUs.load() / 1000.0,
                         n ? encodeSumUs.load() / 1000.0 / n : 0.0,
                         smallEnded.empty() ? "" : (", ended: " + smallEnded).c_str());
            CHECK(smallEnded.empty());
            CHECK(n >= 2);
            CHECK_EQ(smallInfo.height, 720);
            CHECK(smallInfo.width < display->width);
        }
        ::unsetenv("MW_SCALER");
    }

    // ── The split route, and its way back to GL ──────────────────────────────
    //
    // Plan §9-17: Vulkan on a compute queue in front of VA-API — the bench's
    // key convert=vulkan, and since §9-20 (Bruno, 28/09/2026) the vendor
    // table's own on AMD, off the scanout. And Bruno's rule of the same day on
    // the same road: a Vulkan that cannot carry the stream is never forced —
    // the session converts through GL instead, and says so. Run four times:
    // the route under its key; the route with nothing asked, the table's on
    // AMD and GL elsewhere; the Vulkan conversion failing at its first
    // picture, as a lost device would (MW_VK_CONVERT_FAIL_AT=1), which must
    // leave a stream that goes on through GL; and a Vulkan conversion that
    // does not start, as on a machine without Vulkan (=0), which must leave a
    // stream that starts on GL. Not by hiding the drivers with
    // VK_DRIVER_FILES: the loader reads it with secure_getenv, which a binary
    // carrying file capabilities — this one, for the scanout — never sees.
#if defined(MW_NATIVE_LINUX_VULKAN)
    // Whether the portal handed over shared memory rather than a DMA-BUF — GNOME
    // does under X11 (the UM790Pro, 30/09/2026). GL cannot read it, so where
    // Vulkan gives up the next pair down is the CPU's, not GL's. Learnt from the
    // split route's first fall below, whose reason names it.
    bool portalSharedMemory = false;
    if (!h264Only) {
        SECTION("Linux — the split route: Vulkan compute into VA-API, AMD's own, GL when Vulkan "
                "gives up");
        struct Run
        {
            const char* what;
            const char* failAt; // MW_VK_CONVERT_FAIL_AT, or null
            bool keyed;         // convert=vulkan, or nothing asked
        };
        const Run runs[] = {
            {"split route, convert=vulkan", nullptr, true},
            {"nothing asked, the vendor table's", nullptr, false},
            {"Vulkan lost at its first picture", "1", true},
            {"no Vulkan conversion at all", "0", true},
        };
        const bool amdGpu = gpu->vendorId == 0x1002;
        for (const Run& run : runs) {
            if (run.failAt) ::setenv("MW_VK_CONVERT_FAIL_AT", run.failAt, 1);
            SessionConfig split = config;
            split.clientCodecs = {Codec::H264};
            if (run.keyed) split.tuning.convertLinux = EncoderTuning::ConvertLinux::Vulkan;
            std::atomic<int> splitFrames{0};
            std::atomic<int> splitKeyframes{0};
            std::atomic<int64_t> convertSumUs{0};
            std::string splitEnded;
            std::string splitError;
            const std::string failAt = run.failAt ? run.failAt : "";
            std::ofstream splitOut(std::string("/tmp/mw-linux-split-") +
                                       (failAt.empty() ? std::string(run.keyed ? "vulkan" : "auto")
                                                       : "fail" + failAt) +
                                       ".h264",
                                   std::ios::binary | std::ios::trunc);
            std::unique_ptr<Session> splitSession = NativeHost::createSession(
                split,
                [&](const EncodedFrame& f) {
                    splitFrames.fetch_add(1);
                    if (f.keyframe) splitKeyframes.fetch_add(1);
                    convertSumUs.fetch_add(f.convertedUs - f.submittedUs);
                    splitOut.write(reinterpret_cast<const char*>(f.data),
                                   static_cast<std::streamsize>(f.size));
                },
                nullptr, nullptr, nullptr, [&](const std::string& reason) { splitEnded = reason; },
                splitError);
            if (!splitSession) {
                std::fprintf(stderr, "  createSession failed: %s\n", splitError.c_str());
                CHECK(false);
                continue;
            }
            CHECK(splitSession->start(splitError));
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            splitSession->requestKeyframe();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            splitSession->stop();
            splitOut.close();
            ::unsetenv("MW_VK_CONVERT_FAIL_AT");
            const SessionInfo& splitInfo = splitSession->info();
            const int n = splitFrames.load();
            std::fprintf(stderr,
                         "  %s: route \"%s\"%s, %d frame(s), %d keyframe(s), convert mean %.2f "
                         "ms%s\n    reason: %s\n",
                         run.what, splitInfo.videoRoute.c_str(),
                         splitInfo.videoPipelineRefused ? " (refused)" : "", n,
                         splitKeyframes.load(), n ? convertSumUs.load() / 1000.0 / n : 0.0,
                         splitEnded.empty() ? "" : (", ended: " + splitEnded).c_str(),
                         splitInfo.videoPipelineReason.c_str());
            // Whatever Vulkan did, the stream went on: that is the rule.
            CHECK(splitEnded.empty());
            CHECK(n >= 2);
            CHECK(splitKeyframes.load() >= 1);
            if (failAt.empty() && (run.keyed || amdGpu)) {
                CHECK_EQ(splitInfo.videoRoute, std::string("Vulkan compute → VA-API"));
                CHECK(!splitInfo.videoPipelineRefused);
                if (!run.keyed)
                    CHECK(splitInfo.videoPipelineReason.find(
                              "the vendor table converts with Vulkan compute for AMD") !=
                          std::string::npos);
            } else if (failAt.empty()) {
                // Intel: the table's GL, which nobody refused.
                CHECK_EQ(splitInfo.videoRoute, std::string("EGL → VA-API"));
                CHECK(!splitInfo.videoPipelineRefused);
            } else {
                // Vulkan gone, the pair a step down: GL's — or, on the portal's
                // shared memory, which GL cannot read, the CPU's; the route's
                // reason says which, and only the portal can give it.
                const bool shm =
                    splitInfo.videoPipelineReason.find("shared memory") != std::string::npos;
                if (shm) portalSharedMemory = true;
                CHECK(!shm || viaPortal);
                CHECK_EQ(splitInfo.videoRoute,
                         std::string(shm ? "CPU → OpenH264" : "EGL → VA-API"));
                CHECK(splitInfo.videoPipelineRefused);
                CHECK(splitInfo.videoPipelineReason.find(
                          failAt == "0" ? "could not start" : "gave up while streaming") !=
                      std::string::npos);
            }
        }
    }

    // ── The Vulkan Video chain, and its ways back to VA-API ──────────────────
    //
    // C13.5, and Bruno's rule on its main road: the whole chain in Vulkan is
    // taken only where the pixel proof passed on this GPU, driver and firmware
    // (VulkanHevcProof). A driver that shows no encoder — Ubuntu 22.04's Mesa
    // 23.2 — or one that codes something other than its SPS says — the
    // witness, MW_VK_ENCODE_DEPTH=2, what coded wrong on the 780M — leaves
    // VA-API encoding, by name; a chain that does not start, or gives up while
    // streaming, leaves a stream that goes on through VA-API. The chain itself
    // needs a driver with an encoder: run as root, VK_DRIVER_FILES at such a
    // RADV (a binary with file capabilities never sees the variable).
    if (!h264Only) {
        SECTION("Linux — the Vulkan Video chain: taken on the pixel's word, VA-API otherwise");
        std::string node;
        vulkan::DeviceIdentity id;
        for (int minor = 128; minor < 136 && node.empty(); ++minor) {
            const std::string path = "/dev/dri/renderD" + std::to_string(minor);
            std::string why;
            if (vulkan::VulkanDevice::identify(path, id, why)) node = path;
        }
        const bool capable = !node.empty() && id.encodesHevc && id.decodesHevc;
        std::fprintf(stderr, "  %s: %s\n", node.empty() ? "no Vulkan device" : id.name.c_str(),
                     capable ? "a Vulkan Video encoder and decoder"
                             : "no Vulkan Video encoder to prove");
        struct Run
        {
            const char* what;
            const char* depth;  // MW_VK_ENCODE_DEPTH, or null
            const char* failAt; // MW_VK_CONVERT_FAIL_AT, or null
        };
        const Run runs[] = {
            {"Vulkan Video", nullptr, nullptr},
            {"the witness, transform depth 2", "2", nullptr},
            {"the chain lost at its first picture", nullptr, "1"},
            {"no Vulkan conversion at all", nullptr, "0"},
        };
        ::setenv("MW_VK_PROOF_CACHE", "0", 1);
        int index = 0;
        for (const Run& run : runs) {
            if (run.depth) ::setenv("MW_VK_ENCODE_DEPTH", run.depth, 1);
            if (run.failAt) ::setenv("MW_VK_CONVERT_FAIL_AT", run.failAt, 1);
            SessionConfig vk = config;
            vk.clientCodecs = {Codec::Hevc};
            vk.tuning.pipeline = VideoPipeline::Vulkan;
            std::atomic<int> vkFrames{0};
            std::atomic<int> vkKeyframes{0};
            std::atomic<int64_t> encodeSumUs{0};
            std::string vkEnded;
            std::string vkError;
            std::ofstream vkOut("/tmp/mw-linux-vkvideo-" + std::to_string(index++) + ".hevc",
                                std::ios::binary | std::ios::trunc);
            std::unique_ptr<Session> vkSession = NativeHost::createSession(
                vk,
                [&](const EncodedFrame& f) {
                    vkFrames.fetch_add(1);
                    if (f.keyframe) vkKeyframes.fetch_add(1);
                    encodeSumUs.fetch_add(f.encodedUs - f.convertedUs);
                    vkOut.write(reinterpret_cast<const char*>(f.data),
                                static_cast<std::streamsize>(f.size));
                },
                nullptr, nullptr, nullptr, [&](const std::string& reason) { vkEnded = reason; },
                vkError);
            if (!vkSession) {
                std::fprintf(stderr, "  createSession failed: %s\n", vkError.c_str());
                CHECK(false);
                continue;
            }
            // This client decodes HEVC and nothing else. On the portal's shared
            // memory, with Vulkan gone, the only pair left is the CPU's, which
            // is H.264's alone: the client is refused — at the start, or when
            // the chain falls mid-stream — and told why (LinuxSession::buildPair)
            // rather than sent a stream it cannot decode.
            const bool refusedForCodec = portalSharedMemory && run.failAt;
            const bool started = vkSession->start(vkError);
            if (!refusedForCodec) CHECK(started);
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            vkSession->requestKeyframe();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            vkSession->stop();
            vkOut.close();
            ::unsetenv("MW_VK_ENCODE_DEPTH");
            ::unsetenv("MW_VK_CONVERT_FAIL_AT");
            const SessionInfo& vkInfo = vkSession->info();
            const int n = vkFrames.load();
            std::fprintf(stderr,
                         "  %s: route \"%s\"%s, %d frame(s), %d keyframe(s), encode mean %.2f "
                         "ms%s\n    reason: %s\n",
                         run.what, vkInfo.videoRoute.c_str(),
                         vkInfo.videoPipelineRefused ? " (refused)" : "", n, vkKeyframes.load(),
                         n ? encodeSumUs.load() / 1000.0 / n : 0.0,
                         vkEnded.empty() ? "" : (", ended: " + vkEnded).c_str(),
                         vkInfo.videoPipelineReason.c_str());
            if (refusedForCodec) {
                const std::string& why = started ? vkEnded : vkError;
                std::fprintf(stderr, "    refused, as it must be: %s\n", why.c_str());
                CHECK(why.find("can only produce H.264") != std::string::npos);
                continue;
            }
            // Whatever Vulkan did, the stream went on: that is the rule.
            CHECK(vkEnded.empty());
            CHECK(n >= 2);
            CHECK(vkKeyframes.load() >= 1);
            const std::string& reason = vkInfo.videoPipelineReason;
            // Where VA-API takes over, the conversion in front of it is the
            // vendor table's (§9-20): Vulkan compute on AMD while a Vulkan
            // device converts, GL once the conversion gave up too — the fault
            // runs, where the chain falls twice, through the split route to
            // GL — or on another vendor.
            const std::string vaapiRoute = !node.empty() && id.vendorId == 0x1002 && !run.failAt
                                               ? "Vulkan compute → VA-API"
                                               : "EGL → VA-API";
            if (!capable) {
                CHECK_EQ(vkInfo.videoRoute, vaapiRoute);
                CHECK(vkInfo.videoPipelineRefused);
                // The proof's word — or, once the split route's conversion
                // gave up too (the fault runs), that failure's, which rules
                // the chain out before any proof is asked for again.
                const char* expected = !run.failAt ? "the pixel proof could not run"
                                       : std::string(run.failAt) == "0" ? "could not start"
                                                                        : "gave up while streaming";
                CHECK(reason.find(expected) != std::string::npos);
            } else if (run.depth) {
                // AMD's firmware codes the full depth whatever the SPS says.
                if (id.vendorId == 0x1002) {
                    CHECK_EQ(vkInfo.videoRoute, vaapiRoute);
                    CHECK(vkInfo.videoPipelineRefused);
                    CHECK(reason.find("the pixel proof failed") != std::string::npos);
                }
            } else if (run.failAt) {
                CHECK_EQ(vkInfo.videoRoute, vaapiRoute);
                CHECK(vkInfo.videoPipelineRefused);
                CHECK(reason.find(std::string(run.failAt) == "0"
                                      ? "could not start"
                                      : "gave up while streaming") != std::string::npos);
            } else {
                CHECK_EQ(vkInfo.videoRoute, std::string("Vulkan compute → Vulkan Video"));
                CHECK(!vkInfo.videoPipelineRefused);
            }
        }
        ::unsetenv("MW_VK_PROOF_CACHE");
    }
#endif
#endif
}

// The portal's VIRTUAL source: a monitor the compositor makes for the session
// at the size and cadence the stream asks for (GNOME 46+). What is checked is
// the contract the "virtual display" card rests on — the picture comes back
// at the client's size, not at any monitor's. Opt-in (MW_PORTAL_VIRTUAL=1):
// the portal may raise a dialog, and a test nobody watches must not hang on it.
void run_linux_virtual_display_tests()
{
    SECTION("Linux — the portal's virtual display, at the client's size");

#if !defined(MW_NATIVE_LINUX_GFX) || !defined(MW_NATIVE_LINUX_PORTAL)
    std::fprintf(stderr, "  skipped: the portal route is not built\n");
#else
    const Capabilities caps = NativeHost::probe();
    const DisplayInfo* virt = nullptr;
    for (const DisplayInfo& d : caps.displays)
        if (d.key == kPortalVirtualDisplayKey) virt = &d;
    if (!virt) {
        std::fprintf(stderr, "  skipped: the portal offers no virtual source here\n");
        return;
    }
    CHECK(virt->kind == DisplayKind::Virtual);
    CHECK(virt->capture == CaptureApi::PipeWire);
    CHECK(!virt->primary || caps.displays.size() == 1);
    std::fprintf(stderr, "  listed: %s — %s\n", virt->label.c_str(), virt->detail.c_str());
    const char* opt = std::getenv("MW_PORTAL_VIRTUAL");
    if (!opt || std::string(opt) != "1") {
        std::fprintf(stderr, "  session skipped: set MW_PORTAL_VIRTUAL=1 (the portal may ask)\n");
        return;
    }

    SessionConfig config;
    config.displayId = virt->id;
    config.width = 1600;
    config.height = 900;
    config.fps = 30;
    config.bitrateKbps = 10000;
    config.clientCodecs = {Codec::H264};
    // A grant from an earlier run replays silently; without one the portal
    // asks, and the grant it hands back is printed for the next run.
    const char* replay = std::getenv("MW_PORTAL_VIRTUAL_TOKEN");
    if (replay && *replay) config.portalRestoreToken = replay;

    std::atomic<int> frames{0};
    std::string ended;
    std::string error;
    std::unique_ptr<Session> session = NativeHost::createSession(
        config, [&](const EncodedFrame&) { frames.fetch_add(1); }, nullptr, nullptr, nullptr,
        [&](const std::string& reason) { ended = reason; }, error);
    if (!session) {
        std::fprintf(stderr, "  createSession failed: %s\n", error.c_str());
        CHECK(false);
        return;
    }
    std::atomic<int> grants{0};
    std::string granted;
    session->setPortalGrantCallback([&](const std::string& token) {
        grants.fetch_add(1);
        granted = token;
    });
    const bool started = session->start(error);
    CHECK(started);
    if (!started) {
        std::fprintf(stderr, "  start failed: %s\n", error.c_str());
        return;
    }
    const SessionInfo& info = session->info();
    std::fprintf(stderr, "  session: display %dx%d, stream %dx%d via %s, capture %s\n",
                 info.displayWidth, info.displayHeight, info.width, info.height,
                 toString(info.encoder), toString(info.capture));
    CHECK_EQ(info.displayWidth, 1600);
    CHECK_EQ(info.displayHeight, 900);
    CHECK_EQ(static_cast<int>(info.capture), static_cast<int>(CaptureApi::PipeWire));
    std::this_thread::sleep_for(std::chrono::seconds(3));
    session->stop();
    std::fprintf(stderr, "  %d frame(s)%s\n", frames.load(),
                 ended.empty() ? "" : (", ended: " + ended).c_str());
    CHECK(frames.load() >= 1);
    CHECK(ended.empty());
    if (!granted.empty())
        std::fprintf(stderr, "  new grant — MW_PORTAL_VIRTUAL_TOKEN=%s\n", granted.c_str());
    // A replayed grant raises no dialog, and so hands nothing back.
    if (replay && *replay) CHECK_EQ(grants.load(), 0);

    // "Match my screen" from a phone held upright, the monitor made at 240 Hz
    // under a 60 fps stream (C0 of plan Idées Punktfunk): the monitor takes the
    // phone's size to the pixel — a shape the nominal 1080p entry would have
    // refused — and the stream keeps its own cadence, the gate carrying the
    // first present of each interval ("cadence:" line). Replays the grant the
    // first session ended with.
    SessionConfig phone = config;
    phone.width = 1170;
    phone.height = 2532;
    phone.fps = 60;
    phone.fitRequestedBox = phone.allowUpscale = phone.matchClientDisplay = true;
    phone.virtualRefreshHz = 240;
    const std::string token = !granted.empty() ? granted : (replay ? replay : "");
    if (token.empty()) {
        std::fprintf(stderr, "  match session skipped: no grant to replay\n");
        return;
    }
    phone.portalRestoreToken = token;
    std::atomic<int> phoneFrames{0};
    std::string phoneEnded;
    std::unique_ptr<Session> phoneSession = NativeHost::createSession(
        phone, [&](const EncodedFrame&) { phoneFrames.fetch_add(1); }, nullptr, nullptr, nullptr,
        [&](const std::string& reason) { phoneEnded = reason; }, error);
    CHECK(phoneSession != nullptr);
    if (!phoneSession) return;
    const bool phoneStarted = phoneSession->start(error);
    CHECK(phoneStarted);
    if (!phoneStarted) {
        std::fprintf(stderr, "  match start failed: %s\n", error.c_str());
        return;
    }
    const SessionInfo& pinfo = phoneSession->info();
    std::fprintf(stderr, "  match session: display %dx%d, stream %dx%d@%d\n", pinfo.displayWidth,
                 pinfo.displayHeight, pinfo.width, pinfo.height, pinfo.fps);
    CHECK_EQ(pinfo.displayWidth, 1170);
    CHECK_EQ(pinfo.displayHeight, 2532);
    CHECK_EQ(pinfo.width, 1170);
    CHECK_EQ(pinfo.height, 2532);
    CHECK_EQ(pinfo.fps, 60);
    std::this_thread::sleep_for(std::chrono::seconds(3));
    phoneSession->stop();
    std::fprintf(stderr, "  %d frame(s)%s\n", phoneFrames.load(),
                 phoneEnded.empty() ? "" : (", ended: " + phoneEnded).c_str());
    CHECK(phoneFrames.load() >= 1);
    CHECK(phoneEnded.empty());
#endif
}
