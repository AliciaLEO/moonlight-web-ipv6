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

#include "core/CadenceChoice.h"
#include "core/CadenceStep.h"
#include "native_test_framework.h"

#include <string>

using mw::native::CadenceChoice;
using mw::native::CadenceInputs;
using mw::native::EncodeTail;
using mw::native::FpsStep;
using mw::native::FrameCadence;
using mw::native::PresentRate;
using mw::native::StepInputs;

namespace {

using Verdict = FpsStep::Verdict;

/// A client at 120 Hz that tears, its stream at 120, on the product's virtual
/// display at 240 Hz, an encoder at 2 ms.
StepInputs ask(int fps)
{
    StepInputs in;
    in.askedFps = fps;
    in.baseFps = 120;
    in.displayMilliHz = 240000;
    in.encodeP95Us = 2000;
    return in;
}

/// The cadence for that client, with @p stepFps granted.
CadenceInputs stepped(int stepFps)
{
    CadenceInputs in;
    in.settingFps = 120;
    in.maxFps = 120; // Auto states its ceiling
    in.displayMilliHz = 240000;
    in.clientMilliHz = 120000;
    in.clientVsync = false;
    in.stepFps = stepFps;
    return in;
}

/// Presents at @p presentHz for one second; how many the gate lets through.
int admittedInOneSecond(FrameCadence gate, int presentHz)
{
    int admitted = 0;
    for (int i = 0; i < presentHz; ++i)
        if (gate.admit(static_cast<int64_t>(i) * 1000000 / presentHz)) admitted++;
    return admitted;
}

bool contains(const std::string& s, const char* part)
{
    return s.find(part) != std::string::npos;
}

} // namespace

void run_cadence_step_tests()
{
    SECTION("CadenceStep — a step the display and the encoder hold is applied");
    {
        const FpsStep s = mw::native::decideStep(ask(240));
        CHECK(s.verdict == Verdict::Applied);
        CHECK_EQ(s.fps, 240);
        CHECK_EQ(s.askedFps, 240);
        CHECK_EQ(std::string(s.why), std::string());
    }

    SECTION("CadenceStep — above the display's refresh, capped there");
    {
        // A 144 Hz client asks twice its rate; the virtual display stops at 240.
        StepInputs in = ask(288);
        in.baseFps = 144;
        const FpsStep s = mw::native::decideStep(in);
        CHECK(s.verdict == Verdict::Capped);
        CHECK_EQ(s.fps, 240);
        CHECK_EQ(s.askedFps, 288);
        CHECK(contains(s.why, "display"));
    }

    SECTION("CadenceStep — an encoder slower than two frames at that rate refuses it");
    {
        // 9 ms at the p95 against 8.33 ms, two frames at 240.
        StepInputs in = ask(240);
        in.encodeP95Us = 9000;
        const FpsStep s = mw::native::decideStep(in);
        CHECK(s.verdict == Verdict::Refused);
        CHECK_EQ(s.fps, 0);
        CHECK(contains(s.why, "encoder"));
        // The same encoder holds 120 for a 60 Hz client: 9 ms < 16.67.
        StepInputs half = ask(120);
        half.baseFps = 60;
        half.encodeP95Us = 9000;
        CHECK(mw::native::decideStep(half).verdict == Verdict::Applied);
        // Exactly two frames are held: 8333 µs × 240 < 2 s.
        in.encodeP95Us = 8333;
        CHECK(mw::native::decideStep(in).verdict == Verdict::Applied);
        // The Arc on the UA.3 bench: 6.9 ms, longer than a frame at 240,
        // and cadence=host-guarded streamed 238 a second through it.
        in.encodeP95Us = 6900;
        CHECK(mw::native::decideStep(in).verdict == Verdict::Applied);
    }

    SECTION("CadenceStep — capped, then weighed at the capped rate");
    {
        StepInputs in = ask(288);
        in.baseFps = 144;
        in.encodeP95Us = 9000; // two in flight hold 222 a second, not 240
        const FpsStep s = mw::native::decideStep(in);
        CHECK(s.verdict == Verdict::Refused);
        CHECK(contains(s.why, "encoder"));
    }

    SECTION("CadenceStep — a display no faster than the stream leaves nothing to step to");
    {
        StepInputs in = ask(120);
        in.baseFps = 60;
        in.displayMilliHz = 59940; // a laptop panel
        const FpsStep s = mw::native::decideStep(in);
        CHECK(s.verdict == Verdict::Refused);
        CHECK_EQ(s.fps, 0);
        CHECK(contains(s.why, "display"));
    }

    SECTION("CadenceStep — 0, or the stream's own rate, takes the step away");
    {
        const FpsStep zero = mw::native::decideStep(ask(0));
        CHECK(zero.verdict == Verdict::Base);
        CHECK_EQ(zero.fps, 0);
        const FpsStep same = mw::native::decideStep(ask(120));
        CHECK(same.verdict == Verdict::Base);
        CHECK_EQ(same.fps, 0);
        CHECK(mw::native::decideStep(ask(-5)).verdict == Verdict::Base);
    }

    SECTION("CadenceStep — refused under a bench cadence, a decoder cap, a vsync client");
    {
        StepInputs bench = ask(240);
        bench.benchCadence = true;
        CHECK(mw::native::decideStep(bench).verdict == Verdict::Refused);
        StepInputs capped = ask(240);
        capped.clientCapFps = 100;
        CHECK(mw::native::decideStep(capped).verdict == Verdict::Refused);
        CHECK(contains(mw::native::decideStep(capped).why, "decoder"));
        StepInputs vsync = ask(240);
        vsync.clientVsync = true;
        CHECK(mw::native::decideStep(vsync).verdict == Verdict::Refused);
        CHECK(contains(mw::native::decideStep(vsync).why, "vsync"));
        StepInputs early = ask(240);
        early.baseFps = 0; // the loop has not chosen a rate yet
        CHECK(mw::native::decideStep(early).verdict == Verdict::Refused);
    }

    SECTION("CadenceStep — an encoder not measured yet does not refuse");
    {
        StepInputs in = ask(240);
        in.encodeP95Us = 0;
        CHECK(mw::native::decideStep(in).verdict == Verdict::Applied);
        // Nor a display whose refresh is unknown: nothing to cap at.
        in.displayMilliHz = 0;
        const FpsStep s = mw::native::decideStep(in);
        CHECK(s.verdict == Verdict::Applied);
        CHECK_EQ(s.fps, 240);
    }

    SECTION("CadenceChoice — a step runs the stream above Auto's ceiling, as a ceiling gate");
    {
        const CadenceChoice c = mw::native::chooseCadence(stepped(240));
        CHECK_EQ(c.fps, 240);
        CHECK(c.gate.isCeiling());
        // Every refresh of the 240 Hz display goes through.
        CHECK_EQ(admittedInOneSecond(c.gate, 240), 240);
        CHECK_EQ(c.line, std::string("[native] cadence: 240 fps stream on a 240 Hz display — every "
                                     "refresh is encoded, presents beyond it no faster than the "
                                     "stream; a step above the client's 120 fps, asked by its "
                                     "detection; client at 120 Hz"));
    }

    SECTION("CadenceChoice — a step under the display's refresh is a grid at its rate");
    {
        CadenceInputs in = stepped(120);
        in.settingFps = 60;
        in.maxFps = 60;
        in.clientMilliHz = 60000;
        const CadenceChoice c = mw::native::chooseCadence(in);
        CHECK_EQ(c.fps, 120);
        CHECK(c.gate.enabled());
        CHECK(!c.gate.isCeiling());
        CHECK_EQ(admittedInOneSecond(c.gate, 240), 120);
        CHECK(contains(c.line, "a step above the client's 60 fps"));
    }

    SECTION("CadenceChoice — a step is capped at the display's refresh");
    {
        CadenceInputs in = stepped(288);
        in.settingFps = 144;
        in.maxFps = 120; // Auto's ceiling on a 144 Hz panel
        in.clientMilliHz = 144000;
        const CadenceChoice c = mw::native::chooseCadence(in);
        CHECK_EQ(c.fps, 240);
        CHECK(c.gate.isCeiling());
        CHECK(contains(c.line, "a step above the client's 120 fps"));
    }

    SECTION("CadenceChoice — no step, a decoder cap or a vsync client: today's cadence");
    {
        CadenceInputs none = stepped(0);
        CadenceInputs today = none;
        const CadenceChoice a = mw::native::chooseCadence(none);
        CHECK_EQ(a.fps, 120);
        CHECK_EQ(a.line, std::string("[native] cadence: 120 fps stream on a 240 Hz display — the "
                                     "first present of each interval is encoded, at once; client "
                                     "at 120 Hz, tearing — nothing to align on"));
        // A decoder that asked for fewer frames is never stepped.
        CadenceInputs capped = stepped(240);
        capped.clientCapFps = 100;
        today.clientCapFps = 100;
        const CadenceChoice b = mw::native::chooseCadence(capped);
        CHECK_EQ(b.fps, 100);
        CHECK_EQ(b.line, mw::native::chooseCadence(today).line);
        // Nor a client that paints on its vsync.
        CadenceInputs vsync = stepped(240);
        vsync.clientVsync = true;
        CadenceInputs vsyncToday = stepped(0);
        vsyncToday.clientVsync = true;
        CHECK_EQ(mw::native::chooseCadence(vsync).fps, mw::native::chooseCadence(vsyncToday).fps);
        CHECK_EQ(mw::native::chooseCadence(vsync).line, mw::native::chooseCadence(vsyncToday).line);
        // A step at or under the stream's own rate changes nothing either.
        CHECK_EQ(mw::native::chooseCadence(stepped(120)).line, a.line);
        // Nor does one under a bench cadence, which owns the rate.
        CadenceInputs bench = stepped(240);
        bench.mode = mw::native::EncoderTuning::Cadence::Host;
        CadenceInputs benchToday = stepped(0);
        benchToday.mode = mw::native::EncoderTuning::Cadence::Host;
        CHECK_EQ(mw::native::chooseCadence(bench).line, mw::native::chooseCadence(benchToday).line);
    }

    SECTION("PresentRate — whole one-second windows, folded presents counted");
    {
        PresentRate rate;
        CHECK_EQ(rate.perSecond(), 0);
        // 240 presents a second, every other one folded into the next acquire.
        for (int i = 0; i < 120; ++i)
            rate.note(2, static_cast<int64_t>(i) * 8333);
        CHECK_EQ(rate.perSecond(), 0); // the first window has not closed
        rate.note(2, 1000000);
        CHECK_EQ(rate.perSecond(), 240);
    }

    SECTION("PresentRate — a still screen reads 0 once a whole window passed without a present");
    {
        PresentRate rate;
        for (int i = 0; i <= 60; ++i)
            rate.note(1, static_cast<int64_t>(i) * 16667);
        CHECK(rate.perSecond() >= 59 && rate.perSecond() <= 61);
        // Timeouts every 100 ms, nothing presented: the window still open
        // holds the last present, the one after it holds none.
        for (int i = 1; i <= 21; ++i)
            rate.tick(1000000 + static_cast<int64_t>(i) * 100000);
        CHECK_EQ(rate.perSecond(), 0);
    }

    SECTION("PresentRate — a game at 50 frames a second reads 50");
    {
        PresentRate rate;
        for (int i = 0; i <= 150; ++i)
            rate.note(1, static_cast<int64_t>(i) * 20000);
        CHECK_EQ(rate.perSecond(), 50);
    }

    SECTION("EncodeTail — the p95 of the window, nearest rank");
    {
        EncodeTail tail;
        CHECK_EQ(tail.p95Us(), 0);
        // 100 pictures over 2 s: 1 ms each, five of them at 9 ms.
        bool moved = false;
        for (int i = 0; i < 100; ++i) {
            const int64_t us = i % 20 == 0 ? 9000 : 1000;
            moved = tail.note(us, static_cast<int64_t>(i) * 20000) || moved;
        }
        CHECK(!moved); // 1.98 s in
        CHECK(tail.note(1000, 2000000));
        // 101 samples, 5 of them at 9 ms: rank ceil(0.95 × 101) = 96 is the
        // last of the 1 ms ones.
        CHECK_EQ(tail.p95Us(), 1000);
    }

    SECTION("EncodeTail — a slow tail shows, a sparse window keeps the last figure");
    {
        EncodeTail tail;
        for (int i = 0; i <= 120; ++i)
            tail.note(i < 108 ? 2000 : 6000, static_cast<int64_t>(i) * 16667);
        // 121 samples, 13 at 6 ms: rank 115 is a 6 ms one.
        CHECK_EQ(tail.p95Us(), 6000);
        // Then a still screen: five pictures in the next window.
        for (int i = 1; i <= 5; ++i)
            tail.note(1000, 2000004 + static_cast<int64_t>(i) * 500000);
        CHECK_EQ(tail.p95Us(), 6000);
    }
}
