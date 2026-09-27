/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "encode/QpRateController.h"
#include "encode/RateControl.h"
#include "native_test_framework.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

using namespace mw::native::encode;
using Kind = QpRateController::Kind;

namespace {

/// An encoder as the model sees one (plan C6.1): a picture costs its
/// complexity × 2^(−(QP − 26)/6), times some noise — a new predicted picture
/// halving every interSlope of QP instead, for content whose curve is steeper
/// than the textbook's (text scrolling: ~2.4 on the Arc, 27/09/2026). A pass
/// over the same picture codes what separates its QP from the one the picture
/// already has — the whole picture's information between the two, so a still
/// picture costs the same in total whatever the passes — and next to nothing
/// at or above it.
struct Simulator
{
    double inter = 0.6e6;  ///< bits a new picture costs at QP 26
    double intra = 1.25e6; ///< bits an intra picture costs at QP 26
    double interSlope = 6.0;
    double spread = 0.0; ///< log2 of the noise's reach, either side
    double skipBits = 4000;
    uint32_t seed = 12345;
    int pictureQp = 51;

    static double at(double complexity, int qp, double slope = 6.0)
    {
        return complexity * std::exp2(-(qp - 26) / slope);
    }

    double noise()
    {
        seed = seed * 1664525u + 1013904223u;
        const double u = static_cast<double>(seed >> 8) / static_cast<double>(1u << 24);
        return std::exp2(spread * (2.0 * u - 1.0));
    }

    uint64_t encode(const QpRateController::Picture& p)
    {
        double bits = 0;
        switch (p.kind) {
        case Kind::Intra:
            bits = at(intra, p.qp) * noise();
            pictureQp = p.qp;
            break;
        case Kind::Inter:
            bits = at(inter, p.qp, interSlope) * noise();
            pictureQp = p.qp;
            break;
        case Kind::Pass:
            if (p.qp < pictureQp) bits = (at(intra, p.qp) - at(intra, pictureQp)) * noise();
            pictureQp = (std::min)(pictureQp, p.qp);
            break;
        }
        return static_cast<uint64_t>((std::max)(bits, skipBits));
    }
};

constexpr uint64_t kPixels = 1920ull * 1088ull;
constexpr uint32_t kRate = 20000000; // 20 Mbit/s
constexpr int kFps = 60;

struct Frame
{
    QpRateController::Picture picture;
    uint64_t bits = 0;
    bool again = false; ///< coded again, for a strong overshoot
};

/// One picture through the controller and the simulator.
Frame step(QpRateController& rc, Simulator& sim, bool intra, bool fresh)
{
    Frame f;
    f.picture = rc.plan(intra, fresh);
    f.bits = sim.encode(f.picture);
    rc.encoded(f.picture, f.bits);
    return f;
}

/// One picture — new (@p fresh) or the same one again — coded again where it
/// overshoots strongly, as VideoEncode12 does. The try was not sent: the
/// picture the simulator holds is the one before it.
Frame stepAgain(QpRateController& rc, Simulator& sim, bool fresh = true)
{
    Frame f;
    const int pictureQp = sim.pictureQp;
    f.picture = rc.plan(false, fresh);
    f.bits = sim.encode(f.picture);
    if (rc.strongOvershoot(f.picture, f.bits)) {
        rc.overshot(f.picture, f.bits);
        f.picture = rc.reencode(f.picture, f.bits);
        sim.pictureQp = pictureQp;
        f.bits = sim.encode(f.picture);
        f.again = true;
    }
    rc.encoded(f.picture, f.bits);
    return f;
}

/// A session: its first IDR, then @p frames new pictures.
std::vector<Frame> run(QpRateController& rc, Simulator& sim, int frames, bool again = false)
{
    std::vector<Frame> out;
    out.push_back(step(rc, sim, true, true));
    for (int i = 0; i < frames; ++i)
        out.push_back(again ? stepAgain(rc, sim) : step(rc, sim, false, true));
    return out;
}

/// Bits per second over frames [from, from + count).
double rateOver(const std::vector<Frame>& frames, size_t from, size_t count, int fps)
{
    double bits = 0;
    for (size_t i = from; i < from + count && i < frames.size(); ++i)
        bits += static_cast<double>(frames[i].bits);
    return bits * fps / static_cast<double>(count);
}

double percentile(std::vector<double> values, double q)
{
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[static_cast<size_t>(q * static_cast<double>(values.size() - 1))];
}

} // namespace

void run_qp_rate_controller_tests()
{
    const double frame = static_cast<double>(kRate) / kFps;

    SECTION("QpRateController — the first IDR: a conservative guess, then the measure");
    {
        QpRateController rc;
        rc.start(kRate, kFps, 0, kPixels);
        CHECK_EQ(rc.frameBits(), uint64_t(kRate / kFps));
        CHECK_EQ(rc.vbvCapacityBits(), uint64_t(kRate / kFps)); // one frame at 60 fps
        const QpRateController::Picture first = rc.plan(true, true);
        CHECK(first.kind == Kind::Intra);
        CHECK_EQ(first.budgetBits, rc.vbvCapacityBits());
        // 1 bit a pixel at QP 26 against a frame's worth: QP 42.
        CHECK(first.qp >= 40 && first.qp <= 44);
        // At 120 fps the VBV keeps a sixtieth of a second: two frames' worth.
        QpRateController fast;
        fast.start(kRate, 120, 0, kPixels);
        CHECK_EQ(fast.vbvCapacityBits(), uint64_t(kRate / 60));
        CHECK_EQ(fast.plan(true, true).budgetBits, fast.vbvCapacityBits());
    }

    SECTION("QpRateController — steady content holds the rate over every 2 s window");
    {
        QpRateController rc;
        rc.start(kRate, kFps, 0, kPixels);
        Simulator sim;
        sim.inter = 1.2e6;
        sim.intra = 3.0e6;
        sim.spread = 0.3; // ±23 % from one picture to the next
        const std::vector<Frame> frames = run(rc, sim, 1200);
        bool inside = true;
        for (size_t w = 1 + kFps; w + 2 * kFps <= frames.size(); w += 2 * kFps) {
            const double rate = rateOver(frames, w, 2 * kFps, kFps);
            if (std::fabs(rate - kRate) > 0.10 * kRate) inside = false;
        }
        CHECK(inside);
        std::vector<double> sizes;
        int biggestStep = 0;
        double qpSum = 0, qpSquares = 0;
        for (size_t i = 1 + kFps; i < frames.size(); ++i) {
            sizes.push_back(static_cast<double>(frames[i].bits));
            biggestStep =
                (std::max)(biggestStep, std::abs(frames[i].picture.qp - frames[i - 1].picture.qp));
            qpSum += frames[i].picture.qp;
            qpSquares += static_cast<double>(frames[i].picture.qp) * frames[i].picture.qp;
        }
        CHECK(percentile(sizes, 0.95) <= 2.0 * frame);
        // 1.2 Mbit at QP 26 against 333 kbit: QP 37, and no pumping around it
        // — white noise of ±23 % a picture moves it a step or two, no more.
        const double n = static_cast<double>(sizes.size());
        const double qpMean = qpSum / n;
        const double qpSpread = std::sqrt((std::max)(qpSquares / n - qpMean * qpMean, 0.0));
        CHECK(std::fabs(qpMean - 37.5) <= 1.0);
        CHECK(qpSpread <= 1.5);
        CHECK(biggestStep <= 4);
        CHECK_EQ(rc.strongOvershoots(), 0);
    }

    SECTION("QpRateController — complexity x4: one picture over, the backlog paid back");
    {
        QpRateController rc;
        rc.start(kRate, kFps, 0, kPixels);
        Simulator sim;
        std::vector<Frame> frames = run(rc, sim, 299);
        const int before = frames.back().picture.qp; // 0.6 Mbit at 26: QP 31
        CHECK(std::abs(before - 31) <= 1);
        sim.inter *= 4;
        for (int i = 0; i < 60; ++i)
            frames.push_back(step(rc, sim, false, true));
        const size_t jump = 300;
        // The model learns after the fact: the first picture is ~4x over...
        CHECK(frames[jump].bits > 3.0 * frame);
        // ...the next ones pay the backlog back, and within 6 pictures the
        // wire is down to less than a frame of it.
        bool drained = false;
        QpRateController replay;
        replay.start(kRate, kFps, 0, kPixels);
        for (size_t i = 0; i < frames.size() && !drained; ++i) {
            replay.encoded(frames[i].picture, frames[i].bits);
            if (i >= jump && i <= jump + 6 && replay.fullnessBits() <= frame) drained = true;
        }
        CHECK(drained);
        bool settled = true;
        for (size_t i = jump + 12; i < frames.size(); ++i)
            if (std::fabs(static_cast<double>(frames[i].bits) - frame) > 0.25 * frame)
                settled = false;
        CHECK(settled);
        CHECK(std::abs(frames.back().picture.qp - (before + 12)) <= 1);
    }

    SECTION("QpRateController — complexity /4: down 3 a picture, never over the budget");
    {
        QpRateController rc;
        rc.start(kRate, kFps, 0, kPixels);
        Simulator sim;
        sim.inter = 2.4e6;
        std::vector<Frame> frames = run(rc, sim, 299);
        const int before = frames.back().picture.qp; // QP 43
        sim.inter /= 4;
        for (int i = 0; i < 30; ++i)
            frames.push_back(step(rc, sim, false, true));
        bool braked = true, under = true;
        for (size_t i = 300; i < frames.size(); ++i) {
            // 3 a picture, and 1 of rounding.
            if (frames[i - 1].picture.qp - frames[i].picture.qp > 4) braked = false;
            if (static_cast<double>(frames[i].bits) > 1.05 * frame) under = false;
        }
        CHECK(braked);
        CHECK(under);
        CHECK(frames[300].picture.qp >= before - 1); // it did not know yet
        CHECK(std::abs(frames.back().picture.qp - (before - 12)) <= 1);
    }

    SECTION("QpRateController — a one-picture spike: counted, then paid back");
    {
        QpRateController rc;
        rc.start(kRate, kFps, 0, kPixels);
        Simulator sim;
        std::vector<Frame> frames = run(rc, sim, 299);
        const int steady = frames.back().picture.qp;
        sim.inter *= 8;
        frames.push_back(step(rc, sim, false, true));
        sim.inter /= 8;
        for (int i = 0; i < 40; ++i)
            frames.push_back(step(rc, sim, false, true));
        CHECK_EQ(rc.strongOvershoots(), 1);
        bool quiet = true;
        for (size_t i = 301; i < frames.size(); ++i)
            if (static_cast<double>(frames[i].bits) > 1.2 * frame) quiet = false;
        CHECK(quiet);
        CHECK(std::abs(frames.back().picture.qp - steady) <= 1);
    }

    SECTION("QpRateController — the governor's steps are followed on the next picture");
    {
        QpRateController rc;
        rc.start(kRate, kFps, 0, kPixels);
        Simulator sim;
        std::vector<Frame> frames = run(rc, sim, 299);
        const int steady = frames.back().picture.qp;
        rc.setBitrate(kRate / 2);
        const Frame down = step(rc, sim, false, true);
        CHECK(std::abs(down.picture.qp - (steady + 6)) <= 1);
        CHECK(std::fabs(static_cast<double>(down.bits) - frame / 2) <= 0.15 * frame / 2);
        for (int i = 0; i < 10; ++i)
            step(rc, sim, false, true);
        // Up: the budget is known exactly, so no brake — three pictures at most.
        rc.setBitrate(kRate);
        std::vector<Frame> up;
        for (int i = 0; i < 3; ++i)
            up.push_back(step(rc, sim, false, true));
        CHECK(std::abs(up.back().picture.qp - steady) <= 1);
        CHECK(std::fabs(static_cast<double>(up.back().bits) - frame) <= 0.15 * frame);
        // The still boost is a step like any other: x3, and back.
        rc.setBitrate(kRate * 3);
        const Frame boosted = step(rc, sim, false, true);
        CHECK(boosted.picture.qp <= steady - 8);
        rc.setBitrate(kRate);
        const Frame back = step(rc, sim, false, true);
        CHECK(std::abs(back.picture.qp - steady) <= 2);
    }

    SECTION("QpRateController — a still picture is sharpened to QP 18 inside the pass cap");
    {
        QpRateController rc;
        rc.start(kRate, kFps, 0, kPixels);
        Simulator sim;
        const Frame idr = step(rc, sim, true, true);
        CHECK(idr.picture.qp >= 38);
        // The session's still-screen burst: the ×3 budget, one pass at a time,
        // until RefineConvergence says the picture stopped improving.
        rc.setBitrate(static_cast<uint32_t>(stillBitrateKbps(kRate / 1000)) * 1000u);
        RefineConvergence conv;
        RefineConvergence::Verdict verdict = RefineConvergence::Verdict::Continue;
        int lastQp = idr.picture.qp;
        bool fits = true, falling = true;
        while (verdict == RefineConvergence::Verdict::Continue) {
            const Frame pass = step(rc, sim, false, false);
            CHECK(pass.picture.kind == Kind::Pass);
            if (static_cast<double>(pass.bits) > 2.0 * static_cast<double>(pass.picture.budgetBits))
                fits = false;
            if (pass.picture.qp > lastQp || lastQp - pass.picture.qp > QpRateController::kPassDrop)
                falling = false;
            lastQp = pass.picture.qp;
            verdict = conv.notePass(static_cast<size_t>(pass.bits / 8), pass.picture.qp);
        }
        CHECK(verdict == RefineConvergence::Verdict::Converged);
        CHECK_EQ(lastQp, QpRateController::kMinQp);
        CHECK_EQ(rc.pictureQp(), QpRateController::kMinQp);
        CHECK(fits);
        CHECK(falling);
        // The idle floor after it: the picture again, at its own QP, for next
        // to nothing.
        rc.setBitrate(kRate);
        const Frame floor = step(rc, sim, false, false);
        CHECK_EQ(floor.picture.qp, QpRateController::kMinQp);
        CHECK(floor.bits <= static_cast<uint64_t>(sim.skipBits));
    }

    SECTION("QpRateController — a heavy page: every pass sharpens, none stalls early");
    {
        // A page of text whose first picture is ~3x its budget at QP 42 (the
        // bench's still.html on the Arc, 27/09/2026): 1.9 MB whole at QP 18,
        // more than a burst of 8 passes of 125 KB buys.
        QpRateController rc;
        rc.start(kRate, kFps, 0, kPixels);
        Simulator sim;
        sim.intra = 6.0e6;
        const Frame idr = step(rc, sim, true, true);
        rc.setBitrate(static_cast<uint32_t>(stillBitrateKbps(kRate / 1000)) * 1000u);
        RefineConvergence conv;
        RefineConvergence::Verdict verdict = RefineConvergence::Verdict::Continue;
        int lastQp = idr.picture.qp, passes = 0;
        bool sharpened = true, fits = true;
        while (verdict == RefineConvergence::Verdict::Continue) {
            const Frame pass = step(rc, sim, false, false);
            ++passes;
            if (pass.picture.qp >= lastQp && lastQp > QpRateController::kMinQp) sharpened = false;
            if (static_cast<double>(pass.bits) > 1.5 * static_cast<double>(pass.picture.budgetBits))
                fits = false;
            lastQp = pass.picture.qp;
            verdict = conv.notePass(static_cast<size_t>(pass.bits / 8), pass.picture.qp);
        }
        CHECK(sharpened); // no "converged" at the first QP
        CHECK(fits);
        CHECK(lastQp <= idr.picture.qp - 16);
        // The idle floor, a frame's worth: a step down where it buys one —
        // here less than half of one, so the picture stays, for next to
        // nothing.
        rc.setBitrate(kRate);
        const Frame floor = step(rc, sim, false, false);
        CHECK(floor.picture.qp <= lastQp);
        CHECK(static_cast<double>(floor.bits) <= 1.5 * frame);
    }

    SECTION("QpRateController — a pass far over its budget, coded again, still sharpens");
    {
        // After movement, what the whole picture costs is known only through
        // the moving pictures: still at last, a page of dense text costs three
        // times the burst's budget on its first pass. Coded again by the rule
        // for new pictures, that pass went above the picture's QP, coded
        // nothing, taught nothing, and the next one tried the same step: the
        // page stayed at its moving QP, every pass coded twice (27/09/2026).
        QpRateController rc;
        rc.start(kRate, kFps, 0, kPixels);
        Simulator sim;
        sim.intra = 6.0e6;
        const std::vector<Frame> frames = run(rc, sim, 30, true);
        const int moving = frames.back().picture.qp;
        rc.setBitrate(static_cast<uint32_t>(stillBitrateKbps(kRate / 1000)) * 1000u);
        RefineConvergence conv;
        RefineConvergence::Verdict verdict = RefineConvergence::Verdict::Continue;
        int lastQp = moving, again = 0;
        bool sharpened = true, fits = true;
        while (verdict == RefineConvergence::Verdict::Continue) {
            const Frame pass = stepAgain(rc, sim, false);
            again += pass.again ? 1 : 0;
            if (pass.picture.qp >= lastQp) sharpened = false;
            if (rc.strongOvershoot(pass.picture, pass.bits)) fits = false;
            lastQp = pass.picture.qp;
            verdict = conv.notePass(static_cast<size_t>(pass.bits / 8), pass.picture.qp);
        }
        CHECK(again >= 1); // the case at hand
        CHECK(sharpened);
        CHECK(fits);
        CHECK(rc.pictureQp() <= moving - 8);
        // The idle floor carries on where a step fits a frame's worth, and is
        // never coded twice for nothing.
        rc.setBitrate(kRate);
        int idleAgain = 0;
        for (int i = 0; i < 10; ++i)
            idleAgain += stepAgain(rc, sim, false).again ? 1 : 0;
        CHECK_EQ(idleAgain, 0);
        CHECK(rc.pictureQp() <= lastQp);
    }

    SECTION("QpRateController — movement after a sharpened desktop is budgeted as movement");
    {
        QpRateController rc;
        rc.start(kRate, kFps, 0, kPixels);
        Simulator sim;
        std::vector<Frame> frames = run(rc, sim, 119);
        const int moving = frames.back().picture.qp;
        rc.setBitrate(kRate * 3);
        for (int i = 0; i < 8; ++i)
            step(rc, sim, false, false);
        CHECK_EQ(rc.pictureQp(), QpRateController::kMinQp);
        rc.setBitrate(kRate);
        const Frame next = step(rc, sim, false, true);
        CHECK(next.picture.kind == Kind::Inter);
        // What the passes cost said nothing about it: coded where movement was.
        CHECK(std::abs(next.picture.qp - moving) <= 1);
        CHECK(static_cast<double>(next.bits) <= 1.2 * frame);
    }

    SECTION("QpRateController — interfloor=2: the belief stays over a quarter of intra");
    {
        for (const uint32_t rate : {kRate, kRate / 4}) {
            const double budget = static_cast<double>(rate) / kFps;
            QpRateController rc;
            rc.start(rate, kFps, 0, kPixels, -2.0);
            Simulator sim;
            sim.intra = 1.25e6;
            step(rc, sim, true, true);
            // Every new picture a tiny change: the QP falls, but stops where a
            // picture of a quarter of the intra one would fill the budget.
            sim.inter = 1000;
            Frame last;
            for (int i = 0; i < 40; ++i)
                last = step(rc, sim, false, true);
            const double floorQp = 26 + 6 * (std::log2(1.25e6 / 4) - std::log2(budget));
            CHECK(std::abs(last.picture.qp -
                           static_cast<int>(std::lround((std::max)(floorQp, 18.0)))) <= 1);
            // The scroll after it — half the intra picture: over its budget,
            // not by the 4x (20 Mbit/s) or 18x (5) the bottom of the range
            // would have given it.
            sim.inter = 0.6e6;
            const QpRateController::Picture p = rc.plan(false, true);
            const uint64_t scroll = sim.encode(p);
            CHECK(!rc.strongOvershoot(p, scroll));
            CHECK(static_cast<double>(scroll) < 2.2 * budget);
        }
    }

    SECTION("QpRateController — an intra-refresh band over rows of text and blank");
    {
        QpRateController rc;
        rc.start(kRate, kFps, 0, kPixels);
        Simulator sim;
        std::vector<Frame> frames;
        frames.push_back(step(rc, sim, true, true));
        // The band crosses ten frames of text, then ten of blank, over a sweep.
        for (int i = 0; i < 1200; ++i) {
            sim.inter = (i / 10) % 2 ? 0.8e6 : 0.4e6;
            frames.push_back(step(rc, sim, false, true));
        }
        bool inside = true;
        for (size_t w = 1 + kFps; w + 2 * kFps <= frames.size(); w += 2 * kFps) {
            const double rate = rateOver(frames, w, 2 * kFps, kFps);
            if (std::fabs(rate - kRate) > 0.10 * kRate) inside = false;
        }
        CHECK(inside);
        std::vector<double> sizes;
        int lowest = 99, highest = 0;
        bool settled = true;
        for (size_t i = 1 + kFps; i < frames.size(); ++i) {
            sizes.push_back(static_cast<double>(frames[i].bits));
            lowest = (std::min)(lowest, frames[i].picture.qp);
            highest = (std::max)(highest, frames[i].picture.qp);
            // The last three pictures of each ten sit on their plateau: QP
            // 27.6 over blank rows, 33.6 over text.
            const int within = static_cast<int>((i - 1) % 10);
            const double plateau = ((i - 1) / 10) % 2 ? 33.6 : 27.6;
            if (within >= 7 && std::fabs(frames[i].picture.qp - plateau) > 1.5) settled = false;
        }
        CHECK(percentile(sizes, 0.95) <= 2.0 * frame);
        CHECK(settled);
        // Into text, the first picture is twice its budget — the model cannot
        // know before — and the QP goes a few steps past the plateau while
        // that is paid back; never below the blank rows' plateau.
        CHECK(lowest >= 27);
        CHECK(highest <= 39);
    }

    SECTION("QpRateController — coding a strong overshoot again lands it in its budget");
    {
        QpRateController rc;
        rc.start(kRate, kFps, 0, kPixels);
        Simulator sim;
        std::vector<Frame> frames = run(rc, sim, 299);
        sim.inter *= 4;
        const QpRateController::Picture p = rc.plan(false, true);
        const uint64_t first = sim.encode(p);
        CHECK(rc.strongOvershoot(p, first));
        rc.overshot(p, first);
        const QpRateController::Picture again = rc.reencode(p, first);
        CHECK(again.qp > p.qp);
        CHECK_EQ(again.budgetBits, p.budgetBits);
        const uint64_t second = sim.encode(again);
        CHECK(!rc.strongOvershoot(again, second));
        CHECK(static_cast<double>(second) <= static_cast<double>(p.budgetBits));
        rc.encoded(again, second);
        // Nothing to pay back, and the next picture is sized by what the
        // content now costs.
        CHECK(static_cast<double>(rc.fullnessBits()) < 0.1 * frame);
        const Frame next = step(rc, sim, false, true);
        CHECK(std::fabs(static_cast<double>(next.bits) - frame) <= 0.15 * frame);
        CHECK_EQ(rc.strongOvershoots(), 0);
        // A picture within its budget, or merely over it, is not one.
        CHECK(!rc.strongOvershoot(p, p.budgetBits * 2));
    }

    SECTION("QpRateController — a one-picture cut coded again: the next is back at once");
    {
        QpRateController rc;
        rc.start(kRate, kFps, 0, kPixels);
        Simulator sim;
        std::vector<Frame> frames = run(rc, sim, 299);
        const int steady = frames.back().picture.qp;
        // An alt-tab, a page wrapping round: one picture that costs an intra
        // one, coded again where it fits.
        const auto cut = [&]() {
            const QpRateController::Picture p = rc.plan(false, true);
            const uint64_t first = sim.encode(p);
            CHECK(rc.strongOvershoot(p, first));
            rc.overshot(p, first);
            const QpRateController::Picture again = rc.reencode(p, first);
            rc.encoded(again, sim.encode(again));
        };
        sim.inter *= 8;
        cut();
        sim.inter /= 8;
        // The picture after it cannot know yet, and is coded as dear as the
        // cut (under its budget); once it has said the pictures are back where
        // they were, the next is believed so at once — not braked down 3 a
        // picture from what the cut cost.
        const Frame after = step(rc, sim, false, true);
        CHECK(static_cast<double>(after.bits) <= frame);
        const Frame next = step(rc, sim, false, true);
        CHECK(std::abs(next.picture.qp - steady) <= 1);
        CHECK(std::fabs(static_cast<double>(next.bits) - frame) <= 0.15 * frame);
        // A new level is no spike: the second picture up keeps the belief up.
        sim.inter *= 8;
        cut();
        const Frame second = step(rc, sim, false, true);
        CHECK(second.picture.qp >= steady + 15);
        CHECK(static_cast<double>(second.bits) <= 1.2 * frame);
    }

    SECTION("QpRateController — a page of text scrolling: its steep curve is learned");
    {
        // Around its operating point the scroll halves every 2.4 of QP, not
        // 6: at QP 29 it fills the budget, at 24 it is four times over it,
        // at 36 next to nothing (the Arc, 27/09/2026).
        for (const bool again : {false, true}) {
            QpRateController rc;
            rc.start(kRate, kFps, 0, kPixels);
            Simulator sim;
            sim.intra = 6.0e6;
            sim.interSlope = 2.4;
            sim.inter = frame * std::exp2(3.0 / 2.4);
            sim.spread = 0.1;
            const std::vector<Frame> frames = run(rc, sim, 900, again);
            bool inside = true;
            for (size_t w = 1 + 2 * kFps; w + 2 * kFps <= frames.size(); w += 2 * kFps) {
                const double rate = rateOver(frames, w, 2 * kFps, kFps);
                if (std::fabs(rate - kRate) > 0.10 * kRate) inside = false;
            }
            CHECK(inside);
            std::vector<double> sizes;
            int lowest = 99, highest = 0;
            for (size_t i = 1 + 2 * kFps; i < frames.size(); ++i) {
                sizes.push_back(static_cast<double>(frames[i].bits));
                lowest = (std::min)(lowest, frames[i].picture.qp);
                highest = (std::max)(highest, frames[i].picture.qp);
            }
            CHECK(percentile(sizes, 0.95) <= 2.0 * frame);
            // Settled around 29, not swinging between 24 and 36.
            CHECK(lowest >= 27);
            CHECK(highest <= 31);
            CHECK(rc.slope() < 3.5);
        }
    }

    SECTION("QpRateController — the page wraps round: a spike coded again, then back");
    {
        QpRateController rc;
        rc.start(kRate, kFps, 0, kPixels);
        Simulator sim;
        sim.intra = 6.0e6;
        sim.interSlope = 2.4;
        sim.inter = frame * std::exp2(3.0 / 2.4);
        const double page = sim.inter;
        std::vector<Frame> frames = run(rc, sim, 300, true);
        // The first IDR went out at nearly three times its budget (nothing
        // codes an IDR again here): what counts is what comes after.
        const int before = rc.strongOvershoots();
        // A whole new page, every three seconds: one picture that costs an
        // intra one.
        int wraps = 0;
        bool back = true;
        for (int i = 0; i < 900; ++i) {
            const bool wrap = i % 180 == 90;
            sim.inter = wrap ? sim.intra : page;
            sim.interSlope = wrap ? 6.0 : 2.4;
            frames.push_back(stepAgain(rc, sim));
            if (wrap) ++wraps;
            // Two pictures after the wrap, the scroll is where it was.
            if (i % 180 == 92 && std::abs(frames.back().picture.qp - 29) > 1) back = false;
        }
        CHECK_EQ(wraps, 5);
        CHECK(back);
        CHECK_EQ(rc.strongOvershoots(), before);
        std::vector<double> sizes;
        for (size_t i = 301; i < frames.size(); ++i)
            sizes.push_back(static_cast<double>(frames[i].bits));
        CHECK(percentile(sizes, 0.95) <= 2.0 * frame);
    }

    SECTION("QpRateController — a spike sent whole, paid back far above it, is no one-off");
    {
        // Text scrolling at 120 fps, a page wrapping round every 3 s, no
        // picture coded again: the picture after a spike goes out at the
        // backlog's QP, far above the spike's, and next to nothing. Taken for
        // a return, it brought back the belief the spike had disproved, and
        // the QP sank to 18 with every other picture 12x its budget (the Arc,
        // 27/09/2026).
        const double budget = static_cast<double>(kRate) / 120;
        QpRateController rc;
        rc.start(kRate, 120, 0, kPixels);
        Simulator sim;
        sim.intra = 6.0e6;
        sim.interSlope = 2.4;
        sim.inter = budget * std::exp2(9.0 / 2.4); // fills the budget at QP 35
        // At 120 fps a scroll step is 5 px, or 10 when two presents fold into
        // one capture: at the same QP, one picture costs up to three times
        // the next.
        sim.spread = 0.8;
        const double page = sim.inter;
        std::vector<Frame> frames;
        frames.push_back(step(rc, sim, true, true));
        for (int i = 0; i < 2400; ++i) {
            const bool wrap = i % 360 == 180;
            // Every other capture folds two presents: twice the motion.
            sim.inter = wrap ? sim.intra : page * (i % 2 ? 2.5 : 1.0);
            sim.interSlope = wrap ? 6.0 : 2.4;
            frames.push_back(step(rc, sim, false, true));
        }
        int at18 = 0;
        std::vector<double> sizes;
        for (size_t i = 241; i < frames.size(); ++i) {
            at18 += frames[i].picture.qp == QpRateController::kMinQp ? 1 : 0;
            sizes.push_back(static_cast<double>(frames[i].bits) / budget);
        }
        CHECK(at18 < 20);
        CHECK(percentile(sizes, 0.95) <= 2.5);
        bool inside = true;
        for (size_t w = 241; w + 240 <= frames.size(); w += 240) {
            const double rate = rateOver(frames, w, 240, 120);
            if (std::fabs(rate - kRate) > 0.15 * kRate) inside = false;
        }
        CHECK(inside);
    }

    SECTION("QpRateController — a game's noise bends the slope a little, and no further");
    {
        // Read in a closed loop, the slope comes out low: the QP moves most
        // after the noisiest pictures, and their noise lands in the secant.
        // A game of the textbook's 6 reads 3.5 to 5.5 — harmless, the rate
        // holds (above) — and never wanders toward a steep content's 2.
        QpRateController rc;
        rc.start(kRate, kFps, 0, kPixels);
        Simulator sim;
        sim.inter = 1.2e6;
        sim.intra = 3.0e6;
        sim.spread = 0.3;
        double lowest = 9.0, highest = 0.0;
        step(rc, sim, true, true);
        for (int i = 0; i < 6000; ++i) {
            step(rc, sim, false, true);
            if (i < 120) continue;
            lowest = (std::min)(lowest, rc.slope());
            highest = (std::max)(highest, rc.slope());
        }
        CHECK(lowest > 3.0);
        CHECK(highest < 7.0);
    }

    SECTION("QpRateController — the range and the budget's floor hold, whatever the content");
    {
        QpRateController rc;
        rc.start(kRate, kFps, 0, kPixels);
        Simulator sim;
        sim.inter = 1e10;
        sim.intra = 1e10;
        std::vector<Frame> frames = run(rc, sim, 30);
        bool bounded = true, floored = true;
        for (const Frame& f : frames) {
            bounded = bounded && f.picture.qp >= QpRateController::kMinQp &&
                      f.picture.qp <= QpRateController::kMaxQp;
            floored = floored &&
                      f.picture.budgetBits >= rc.frameBits() / QpRateController::kMinBudgetShare;
        }
        CHECK(bounded);
        CHECK(floored);
        CHECK_EQ(frames.back().picture.qp, QpRateController::kMaxQp);
        // The bucket keeps its backlog through a change of rate.
        const uint64_t held = rc.fullnessBits();
        CHECK(held > 0);
        rc.setBitrate(kRate / 2);
        CHECK_EQ(rc.fullnessBits(), held);

        QpRateController easy;
        easy.start(kRate, kFps, 0, kPixels);
        Simulator flat;
        flat.inter = 100;
        flat.intra = 100;
        const std::vector<Frame> quiet = run(easy, flat, 30);
        CHECK_EQ(quiet.back().picture.qp, QpRateController::kMinQp);
        CHECK(easy.meanQp() >= QpRateController::kMinQp);
        CHECK_EQ(easy.pictures(), 31);
    }
}
