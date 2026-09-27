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
/// complexity × 2^(−(QP − 26)/6), times some noise. A pass over the same
/// picture codes what separates its QP from the one the picture already has —
/// the whole picture's information between the two, so a still picture costs
/// the same in total whatever the passes — and next to nothing at or above it.
struct Simulator
{
    double inter = 0.6e6;  ///< bits a new picture costs at QP 26
    double intra = 1.25e6; ///< bits an intra picture costs at QP 26
    double spread = 0.0;   ///< log2 of the noise's reach, either side
    double skipBits = 4000;
    uint32_t seed = 12345;
    int pictureQp = 51;

    static double at(double complexity, int qp) { return complexity * std::exp2(-(qp - 26) / 6.0); }

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
            bits = at(inter, p.qp) * noise();
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

/// A session: its first IDR, then @p frames new pictures.
std::vector<Frame> run(QpRateController& rc, Simulator& sim, int frames)
{
    std::vector<Frame> out;
    out.push_back(step(rc, sim, true, true));
    for (int i = 0; i < frames; ++i)
        out.push_back(step(rc, sim, false, true));
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

    SECTION("QpRateController — keystrokes do not lower the belief below a quarter of intra");
    {
        for (const uint32_t rate : {kRate, kRate / 4}) {
            const double budget = static_cast<double>(rate) / kFps;
            QpRateController rc;
            rc.start(rate, kFps, 0, kPixels);
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
