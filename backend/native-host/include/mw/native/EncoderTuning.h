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

#pragma once

#include "VideoPipeline.h"

#include <string>

namespace mw::native {

/// The encoder knobs the engine decides for itself — exposed so the BENCH can
/// move them, one at a time, and measure what each one costs.
///
/// ── This is not configuration ───────────────────────────────────────────────
///
/// Nothing in the product sets any of these: the engine's own choice is what
/// every field's `Default` means, and a session started from a browser never
/// carries anything else (§28 of the mission — no preset, no tuning, no VBV in
/// front of a user). The struct exists because the choice has to be MEASURED
/// before it is made, on every vendor's silicon, and a benchmark that cannot
/// vary the setting under test is not one. `--native-bench` is the only caller
/// that fills it in.
///
/// Tri-state on purpose: "leave it to the preset" is a distinct answer from
/// "off", and the difference is exactly what the bench wants to see — what a
/// vendor's ultra-low-latency preset actually enables, and whether it was right.
struct EncoderTuning
{
    enum class Choice
    {
        Default,
        Off,
        On
    };

    /// NVENC: the P1 (fastest) … P7 (best quality) preset. 0 is the engine's
    /// own (P1 since the bench of 04/09/2026 — see NvencEncoder.cpp).
    int nvencPreset = 0;

    /// NVENC: which latency tuning the preset is fetched for.
    enum class Latency
    {
        Default,
        UltraLow,
        Low
    };
    Latency nvencTuning = Latency::Default;

    /// NVENC: a first pass at reduced or full resolution to steer the rate
    /// control of the second. Costs encode time; buys a steadier CBR.
    enum class MultiPass
    {
        Default,
        Off,
        QuarterRes,
        FullRes
    };
    MultiPass nvencMultiPass = MultiPass::Default;

    /// NVENC: the QP the rate control never goes below. 0 is the engine's own
    /// (18, qindex 32 for AV1 — see NvencEncoder.cpp), -1 none at all.
    int nvencMinQp = 0;
    /// AMF (H.264, HEVC): the same floor. 0 is the engine's own (18 — see
    /// AmfEncoder.cpp), -1 none at all.
    int amfMinQp = 0;

    /// NVENC intra-refresh: frames from one sweep's start to the next
    /// (intraRefreshPeriod) and frames the band takes to cross the picture
    /// (intraRefreshCnt). 0 is the engine's own for each.
    int nvencIntraRefreshPeriod = 0;
    int nvencIntraRefreshCount = 0;

    /// Spatial adaptive quantization: NVENC `enableAQ`, AMF VBAQ, AMF AV1 CAQ.
    Choice spatialAq = Choice::Default;
    /// NVENC only: temporal AQ.
    Choice temporalAq = Choice::Default;
    /// AMF only: the pre-analysis module ahead of the rate control.
    Choice preAnalysis = Choice::Default;

    /// AMF: the quality preset — speed, balanced, quality. Default is whatever
    /// the ultra-low-latency usage picks.
    enum class AmfQuality
    {
        Default,
        Speed,
        Balanced,
        Quality
    };
    AmfQuality amfQuality = AmfQuality::Default;

    /// AMF H.264/HEVC: the encoder's internal low-latency mode
    /// (`LowLatencyInternal`, which also puts H.264 in POC mode 2).
    ///
    /// Its own header says `default = false` — flatly, not "depends on USAGE"
    /// like every other knob here — so the ultra-low-latency usage is not
    /// documented to switch it on. Worth a measurement rather than a guess: it
    /// is the one AMD knob this engine has never touched. AV1 has no such
    /// property; it has an explicit latency mode, already set to its lowest.
    Choice amfLowLatency = Choice::Default;

    /// oneVPL: TargetUsage 1 (quality) … 7 (speed). 0 is the engine's own (7).
    int vplTargetUsage = 0;

    // ── The rest of what Intel exposes ──────────────────────────────────────
    //
    // Six knobs, none of which this engine sets on its own except the first,
    // and all of which the Intel matrix measures rather than assumes. They live
    // here for the same reason every other field does: a benchmark that cannot
    // vary the setting under test is not one.

    /// The fixed-function encode engine (VDENC) rather than the shader-based
    /// one. The engine's own answer is ON — measured, it halves the encode time
    /// — with an automatic fall-back when a generation has none.
    Choice vplLowPower = Choice::Default;
    /// Macroblock-level rate control: spends bits where the picture needs them
    /// rather than evenly. Intel's answer to spatial AQ.
    Choice vplMbBrc = Choice::Default;
    /// The alternative bitrate controller. Intel documents it as better on
    /// low-delay content, which is exactly this pipeline's content.
    Choice vplExtBrc = Choice::Default;
    /// The low-delay mode of the bitrate controller — one frame in, one frame
    /// out, no lookahead budgeting.
    Choice vplLowDelayBrc = Choice::Default;
    /// `ScenarioInfo = MFX_SCENARIO_REMOTE_GAMING`, a hint Intel added for this
    /// exact use. What the driver does with it is not documented, which is why
    /// it is measured.
    Choice vplGamingScenario = Choice::Default;
    /// Sliding-window rate cap, in frames: no window of this many frames may
    /// average more than the target. A burst limiter, priced in quality.
    int vplWinBrcFrames = 0;
    /// The bitrate controller itself. CBR spends its whole budget on every
    /// frame, a still one included; VBR under the same MaxKbps and the same
    /// buffer may spend less; QVBR aims at a quality (vplQvbrQuality, 1..51)
    /// under that same cap.
    enum class VplRateControl
    {
        Default,
        Cbr,
        Vbr,
        Qvbr
    };
    VplRateControl vplRateControl = VplRateControl::Default;
    int vplQvbrQuality = 0;
    /// QP offset of the intra-refresh band, oneVPL (IntRefQPDelta, -51..51).
    /// 0 is the engine's own: the band at the frame's own quality.
    int vplIntraRefreshQpDelta = 0;
    /// Frames between the starts of two intra-refresh cycles, oneVPL
    /// (IntRefCycleDist). 0 is the engine's own (encode::intraRefreshDistanceFrames,
    /// four periods); -1 is back to back, the engine's fallback for a runtime
    /// that refuses the gap.
    int vplIntraRefreshDist = 0;

    /// How many reference pictures the encoder keeps for healing a lost frame
    /// by a delta. NVENC: the decoded picture buffer's depth (engine's own: 4
    /// — see NvencEncoder). AMF: the number of long-term reference slots
    /// (engine's own: 4 — see AmfEncoder / ReferenceSlots). 0 is the engine's
    /// own; 1 is the bench's "before" — a single reference, no invalidation
    /// possible on either vendor — for the cost of the feature.
    int dpbFrames = 0;

    /// The link governor (encode::RateGovernor). Off, the encoder's target is
    /// the setting, moved both ways at once by setTargetBitrate: a bench has
    /// no receiver, so the governor would cut the rate for the silence and
    /// never pass a step back up. Every other value is the engine's own: on.
    Choice linkGovernor = Choice::Default;

    /// The delta the relay drops when the link stops draining (SendBacklog),
    /// named to the encoder like the sender's evictions (design §9.10.2): the
    /// next delta predicts from a frame the client has, where the stream
    /// otherwise waits for a keyframe or the refresh wave (plan §9-25). The
    /// engine's own is nameLinkDropsByDefault, below: on for NVENC, fed D3D11
    /// or D3D12 pictures, and for AMF through D3D11 since 29/09/2026, off
    /// elsewhere. A real session's (MW_NATIVE_TUNING): the relay reads it.
    Choice nameLinkDrops = Choice::Default;

    /// The VBV, in frames at the stream's own rate — exactly, with no floor.
    /// 0 is the engine's rule: one frame, never less than a sixtieth of a
    /// second's worth (RateControl.h says why). 1 and 2 are the two bounds the
    /// bench compares that rule against.
    int vbvFrames = 0;

    /// Bench only: pretend no GPU in this machine can encode, so the session
    /// lands on the fallback tier — and, optionally, on ONE named member of it.
    /// The only way to exercise Media Foundation or the CPU encoder on a bench
    /// that has NVENC, and to price them against it on the same content.
    enum class Fallback
    {
        None,                    ///< the engine's choice: GPUs first, the tier only without them
        Tier,                    ///< whatever the tier would pick on an encoder-less machine
        MediaFoundation,         ///< the Media Foundation transform, hardware or software
        MediaFoundationSoftware, ///< Microsoft's software transform even where hardware exists
        MediaFoundationCpuInput, ///< the hardware transform, fed through system memory
        Cpu                      ///< OpenH264
    };
    Fallback fallback = Fallback::None;

    // ── The D3D12 pipeline (plan pipeline-video-d3d12-v2) ───────────────────
    //
    // Each one defaults to the engine's own choice, like everything above;
    // none means anything to a session running D3D11.

    /// The chain, over the setting and the vendor table (VideoPipeline.h).
    VideoPipeline pipeline = VideoPipeline::Auto;
    /// The D3D12 conversion's queue: DIRECT with the pixel shaders D3D11 runs
    /// (the engine's own), or COMPUTE.
    enum class ConvertQueue12
    {
        Default,
        Direct,
        Compute
    };
    ConvertQueue12 conv12 = ConvertQueue12::Default;
    /// The D3D12 route's encoder: D3D12 Video Encode (the engine's own), or
    /// the vendor's SDK fed D3D12 pictures.
    enum class Encoder12
    {
        Default,
        VideoEncode,
        Nvenc,
        Amf
    };
    Encoder12 enc12 = Encoder12::Default;
    /// D3D12 Video Encode's rate control: the driver's CBR where it can move
    /// its target, the in-house QP controller where it cannot (Intel). Qp
    /// runs the in-house one everywhere, as a witness where it is not needed.
    enum class RateControl12
    {
        Default,
        Driver,
        Qp
    };
    RateControl12 rc12 = RateControl12::Default;
    /// The in-house rate control only: a picture far over its budget is coded
    /// again at the QP that fits it — an encode more on that picture, against
    /// a burst on the link. On is the engine's own since the bench of
    /// 27/09/2026 (plan §9-5): scrolling text went from 12 such pictures sent
    /// to 1, for the same mean host time. Off is the bench's "before".
    Choice reencode12 = Choice::Default;
    /// How that picture is coded again. On, the engine's own since
    /// 28/09/2026: at two budgets — under the overshoot line — by the slope
    /// learned, then by the textbook if it still lands far over. Off, the
    /// bench's "before": once, at its budget, by the textbook's slope. On
    /// scrolling text, on went from 0.68 to 0.83 of the target on the N95 and
    /// sent no picture far over on either the N95 or the Arc, for a third
    /// encode on 0.5 % of the pictures (bench §8n.9-§8n.10, plan §9-14).
    Choice reencodeFit12 = Choice::Default;
    /// The in-house rate control only: a new picture is never believed to
    /// cost under 1/2^k of an intra one. 0 is the engine's own — no floor,
    /// since 27/09/2026: a page of text scrolling costs a thirtieth of its
    /// intra picture, and a floor of a quarter held it at QP 38-45 on a 20
    /// Mbit/s stream; -1 no floor either.
    int interFloor12 = 0;
    /// The priority of the D3D12 queues. The engine's own follows the
    /// process's GPU class: GLOBAL_REALTIME under REALTIME, HIGH otherwise.
    enum class Priority12
    {
        Default,
        Normal,
        High,
        GlobalRealtime
    };
    Priority12 prio12 = Priority12::Default;
    /// A CreatorID of each queue's own (the engine's) rather than the
    /// runtime's shared one, whose priority hardware scheduling ignores.
    Choice ownCreator12 = Choice::Default;
    /// How the capture and the D3D12 read of its surface are ordered: fences
    /// on the GPU both ways (the engine's own), none, or the CPU waiting for
    /// the read before ReleaseFrame. Measured: without it, reads come out
    /// stale or from the next frame.
    enum class DdaSync
    {
        Default,
        None,
        Gpu,
        Cpu
    };
    DdaSync ddaSync = DdaSync::Default;
    /// Timestamps on the D3D12 queues, into EncodedFrame's GPU times. Off by
    /// default: a query pair per pass is not free.
    bool gpuTiming = false;
    /// A session asked to run D3D12 that has to run D3D11 ends instead: a
    /// bench row labelled D3D12 is never a D3D11 one.
    bool strict12 = false;
    /// Two pictures in flight (plan Phase 10): D3D12 Video Encode codes a
    /// picture on a thread of its own while the capture converts the next
    /// into a second output; a converted picture still waiting when a newer
    /// one is ready is dropped, never queued. The engine's own since
    /// 29/09/2026 (§9-26): on an Intel GPU with memory of its own, one
    /// picture at a time elsewhere (pipelinedByDefault, VideoPipelineChoice.h).
    Choice pipelined = Choice::Default;
    /// A capture restart (a mode change, a locked screen, a new desktop)
    /// keeps the D3D12 Video Encode encoder when the stream it codes is the
    /// same — codec, size, rate, HDR — rather than making it again (plan
    /// C11.4): the stream starts over on a keyframe either way. On, the
    /// engine's own since 29/09/2026 (§9-27): the keyframe after a mode
    /// change came 717 → 546 ms sooner on the Arc; keep12=0 makes it again.
    Choice keep12 = Choice::Default;

    // ── The Linux chain (plan pipeline-video-d3d12-v2, Phase 13) ────────────

    /// The conversion in front of VA-API: GL through EGL, or Vulkan on a
    /// compute queue — the split route (§9-17). The engine's own is the
    /// vendor table's (core/LinuxRouteChoice.h): Vulkan compute on AMD since
    /// §9-20 (the portal's buffers too since C13.3 bis), GL elsewhere and
    /// whenever VA-API is asked for by name.
    enum class ConvertLinux
    {
        Default,
        Gl,
        Vulkan
    };
    ConvertLinux convertLinux = ConvertLinux::Default;
    /// The Vulkan conversion's queue priority. The engine's own is HIGH when
    /// the process holds CAP_SYS_NICE, normal otherwise; Normal measures the
    /// route without it on a process that has it.
    enum class PriorityVk
    {
        Default,
        Normal,
        High
    };
    PriorityVk prioVk = PriorityVk::Default;
    /// DMA-BUF asked of the ScreenCast portal (PortalCapture::offerDmabuf,
    /// C13.3 bis). Off asks for shared memory only, what a compositor without
    /// DMA-BUF hands over — measured on one that has it (C13.10). The engine's
    /// own is on.
    Choice portalDmabuf = Choice::Default;

    bool isDefault() const
    {
        return nvencPreset == 0 && nvencTuning == Latency::Default &&
               nvencMultiPass == MultiPass::Default && nvencMinQp == 0 && amfMinQp == 0 &&
               nvencIntraRefreshPeriod == 0 && nvencIntraRefreshCount == 0 &&
               spatialAq == Choice::Default && temporalAq == Choice::Default &&
               preAnalysis == Choice::Default && amfQuality == AmfQuality::Default &&
               amfLowLatency == Choice::Default && vplTargetUsage == 0 &&
               vplLowPower == Choice::Default && vplMbBrc == Choice::Default &&
               vplExtBrc == Choice::Default && vplLowDelayBrc == Choice::Default &&
               vplGamingScenario == Choice::Default && vplWinBrcFrames == 0 &&
               vplRateControl == VplRateControl::Default && vplIntraRefreshQpDelta == 0 &&
               vplIntraRefreshDist == 0 && linkGovernor == Choice::Default &&
               nameLinkDrops == Choice::Default && vbvFrames == 0 && dpbFrames == 0 &&
               fallback == Fallback::None && pipeline == VideoPipeline::Auto &&
               conv12 == ConvertQueue12::Default && enc12 == Encoder12::Default &&
               rc12 == RateControl12::Default && reencode12 == Choice::Default &&
               reencodeFit12 == Choice::Default && interFloor12 == 0 &&
               prio12 == Priority12::Default && ownCreator12 == Choice::Default &&
               ddaSync == DdaSync::Default && !gpuTiming && !strict12 &&
               pipelined == Choice::Default && keep12 == Choice::Default &&
               convertLinux == ConvertLinux::Default && prioVk == PriorityVk::Default &&
               portalDmabuf == Choice::Default;
    }

    /// One line naming every field that is NOT at its default, for the log and
    /// the bench summary. Empty when nothing is.
    std::string describe() const
    {
        std::string s;
        auto add = [&s](const std::string& item) {
            if (!s.empty()) s += ' ';
            s += item;
        };
        auto choice = [](Choice c) { return c == Choice::On ? "on" : "off"; };
        if (nvencPreset > 0) add("preset=P" + std::to_string(nvencPreset));
        if (nvencTuning == Latency::UltraLow) add("tuning=ULL");
        if (nvencTuning == Latency::Low) add("tuning=LL");
        if (nvencMultiPass == MultiPass::Off) add("multipass=off");
        if (nvencMultiPass == MultiPass::QuarterRes) add("multipass=quarter");
        if (nvencMultiPass == MultiPass::FullRes) add("multipass=full");
        if (nvencMinQp != 0) add("nvminqp=" + std::to_string(nvencMinQp));
        if (amfMinQp != 0) add("amfminqp=" + std::to_string(amfMinQp));
        if (nvencIntraRefreshPeriod > 0)
            add("nvirperiod=" + std::to_string(nvencIntraRefreshPeriod));
        if (nvencIntraRefreshCount > 0) add("nvircnt=" + std::to_string(nvencIntraRefreshCount));
        if (spatialAq != Choice::Default) add(std::string("aq=") + choice(spatialAq));
        if (temporalAq != Choice::Default) add(std::string("taq=") + choice(temporalAq));
        if (preAnalysis != Choice::Default) add(std::string("preanalysis=") + choice(preAnalysis));
        if (amfQuality == AmfQuality::Speed) add("quality=speed");
        if (amfQuality == AmfQuality::Balanced) add("quality=balanced");
        if (amfQuality == AmfQuality::Quality) add("quality=quality");
        if (amfLowLatency != Choice::Default)
            add(std::string("lowlatency=") + choice(amfLowLatency));
        if (vplTargetUsage > 0) add("tu=" + std::to_string(vplTargetUsage));
        if (vplLowPower != Choice::Default) add(std::string("lowpower=") + choice(vplLowPower));
        if (vplMbBrc != Choice::Default) add(std::string("mbbrc=") + choice(vplMbBrc));
        if (vplExtBrc != Choice::Default) add(std::string("extbrc=") + choice(vplExtBrc));
        if (vplLowDelayBrc != Choice::Default)
            add(std::string("lowdelaybrc=") + choice(vplLowDelayBrc));
        if (vplGamingScenario != Choice::Default)
            add(std::string("gaming=") + choice(vplGamingScenario));
        if (vplWinBrcFrames > 0) add("winbrc=" + std::to_string(vplWinBrcFrames) + "f");
        if (vplRateControl == VplRateControl::Cbr) add("rc=cbr");
        if (vplRateControl == VplRateControl::Vbr) add("rc=vbr");
        if (vplRateControl == VplRateControl::Qvbr) add("rc=qvbr" + std::to_string(vplQvbrQuality));
        if (vplIntraRefreshQpDelta != 0) add("irqp=" + std::to_string(vplIntraRefreshQpDelta));
        if (vplIntraRefreshDist != 0) add("irdist=" + std::to_string(vplIntraRefreshDist));
        if (linkGovernor != Choice::Default) add(std::string("governor=") + choice(linkGovernor));
        if (nameLinkDrops != Choice::Default)
            add(std::string("namedrops=") + choice(nameLinkDrops));
        if (vbvFrames > 0) add("vbv=" + std::to_string(vbvFrames) + "f");
        if (dpbFrames > 0) add("dpb=" + std::to_string(dpbFrames));
        if (fallback == Fallback::Tier) add("fallback=1");
        if (fallback == Fallback::MediaFoundation) add("fallback=mf");
        if (fallback == Fallback::MediaFoundationSoftware) add("fallback=mfsw");
        if (fallback == Fallback::MediaFoundationCpuInput) add("fallback=mfcpu");
        if (fallback == Fallback::Cpu) add("fallback=cpu");
        if (pipeline != VideoPipeline::Auto) add(std::string("pipeline=") + toString(pipeline));
        if (conv12 == ConvertQueue12::Direct) add("conv12=direct");
        if (conv12 == ConvertQueue12::Compute) add("conv12=compute");
        if (enc12 == Encoder12::VideoEncode) add("enc12=ve");
        if (enc12 == Encoder12::Nvenc) add("enc12=nvenc");
        if (enc12 == Encoder12::Amf) add("enc12=amf");
        if (rc12 == RateControl12::Driver) add("rc12=driver");
        if (rc12 == RateControl12::Qp) add("rc12=qp");
        if (reencode12 != Choice::Default) add(std::string("reencode=") + choice(reencode12));
        if (reencodeFit12 != Choice::Default) add(std::string("refit=") + choice(reencodeFit12));
        if (interFloor12 < 0) add("interfloor=off");
        if (interFloor12 > 0) add("interfloor=" + std::to_string(interFloor12));
        if (prio12 == Priority12::Normal) add("prio12=normal");
        if (prio12 == Priority12::High) add("prio12=high");
        if (prio12 == Priority12::GlobalRealtime) add("prio12=realtime");
        if (ownCreator12 == Choice::On) add("creator12=own");
        if (ownCreator12 == Choice::Off) add("creator12=default");
        if (ddaSync == DdaSync::None) add("ddasync=none");
        if (ddaSync == DdaSync::Gpu) add("ddasync=gpu");
        if (ddaSync == DdaSync::Cpu) add("ddasync=cpu");
        if (gpuTiming) add("gputiming=1");
        if (strict12) add("strict12=1");
        if (pipelined != Choice::Default) add(std::string("pipelined=") + choice(pipelined));
        if (keep12 != Choice::Default) add(std::string("keep12=") + choice(keep12));
        if (convertLinux == ConvertLinux::Gl) add("convert=gl");
        if (convertLinux == ConvertLinux::Vulkan) add("convert=vulkan");
        if (prioVk == PriorityVk::Normal) add("priovk=normal");
        if (prioVk == PriorityVk::High) add("priovk=high");
        if (portalDmabuf != Choice::Default)
            add(std::string("portaldmabuf=") + choice(portalDmabuf));
        return s;
    }
};

/// EncoderTuning::nameLinkDrops when the key says nothing, for a session that
/// streams through @p pipeline with @p encoder — and, on the D3D12 route, the
/// encoder that route runs, @p encoder12: on for NVENC and AMF through D3D11
/// and for NVENC fed D3D12 pictures, off everywhere else (Bruno, 29/09/2026,
/// plan §9-25). On a link that stalled, the pictures a Chrome client showed
/// damaged fell by 85 to 93 % on NVENC, by 87 % on NVENC fed D3D12 pictures and
/// by 46 to 96 % on AMF, for the same freezes (bench §8n.27 to §8n.29). Never
/// oneVPL: it hung under it, three passes out of three. D3D12 Video Encode
/// gained nothing measurable, nor did AMF fed D3D12 pictures, which only a
/// bench key runs.
inline bool nameLinkDropsByDefault(VideoPipeline pipeline, EncoderApi encoder,
                                   EncoderTuning::Encoder12 encoder12)
{
    if (pipeline == VideoPipeline::D3d11)
        return encoder == EncoderApi::Nvenc || encoder == EncoderApi::Amf;
    if (pipeline == VideoPipeline::D3d12) return encoder12 == EncoderTuning::Encoder12::Nvenc;
    return false;
}

} // namespace mw::native
