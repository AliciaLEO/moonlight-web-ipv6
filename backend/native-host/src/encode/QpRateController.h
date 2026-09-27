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

#include "RateControl.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mw::native::encode {

/// Our own rate control (plan pipeline-video-d3d12-v2, §4.6): a QP for every
/// picture, handed to an encoder told a constant QP.
///
/// ── Why one of our own ──────────────────────────────────────────────────────
///
/// D3D12 Video Encode on the Arc takes its bitrate once, when the encoder is
/// made: the governor, the frame-rate scaling and the still-screen boost all
/// move the target through setBitrate(), and the driver refused every one of
/// those moves — a still screen stayed at the first bitrate's ~33 KB a pass
/// and never sharpened (27/09/2026, bench §8n.5). The delta-QP map that would
/// have carried a controller of ours is refused at EncodeFrame by the
/// runtime's second look (plan §4.6). What the Arc does do is code each
/// picture at a constant QP changed since the last one, without being told,
/// and say which QP it used: that is the lever, and this is what moves it.
/// Anywhere else it runs as a witness only (the bench's rc12=qp).
///
/// ── The model ───────────────────────────────────────────────────────────────
///
/// A picture's size halves for every 6 of QP — the quantizer's step doubles —
/// so bits = C · 2^(−(QP − 26) / 6), C being what the picture would cost at
/// QP 26: its complexity. In the log domain L = log2(bits) + (QP − 26) / 6 is
/// read off every picture that goes out, and the next one's QP follows from
/// its budget: QP = 26 + 6 · (L − log2(budget)). One L per kind of picture,
/// an intra picture costing several times an inter one of the same content.
/// Each is smoothed, half the old and half the new, so that one noisy picture
/// moves the QP by a step or two rather than six; a picture the model missed
/// by more than twice — a cut, a window opening — replaces it outright.
///
/// ── The budget ──────────────────────────────────────────────────────────────
///
/// A bucket, the VBV's: every picture pours its bits in, the link takes a
/// frame's worth (bitrate / fps) out before the next one, and it never holds
/// less than nothing — a link left idle is capacity gone, not saved. What is
/// in it is time the next picture waits behind on the wire. So an inter
/// picture's budget is a frame's worth less half of what is in the bucket (a
/// backlog is paid back in halves), and an intra picture's is the VBV itself,
/// by the rule every encoder follows (RateControl.h). Never below an eighth of
/// a frame's worth: there the QP is at the top of its range anyway.
///
/// ── Up at once, down in steps ──────────────────────────────────────────────
///
/// Latency first. A picture over its budget delays every picture behind it
/// and one under it delays nothing, so the two errors are not worth the same:
/// the QP rises as far as the model says at once, and falls by 3 at most per
/// picture when pictures got cheaper — a menu over a game, a camera at rest —
/// because the next one may well cost what they used to. The brake is on what
/// the model believes, not on the QP: a budget that grows (the governor, the
/// still-screen boost) is known exactly and is followed on the next picture.
/// And the belief never falls below a quarter of what an intra picture of the
/// content costs: the keystrokes of a sharpened desktop make every new picture
/// cost nearly nothing, and the scroll that follows them should not be coded
/// as if it did too. A page of text scrolling costs about half its intra
/// picture, so it lands at twice its budget at worst, not at the four to
/// eighteen times (20 and 5 Mbit/s) the bottom of the QP range would give it;
/// the price is keystrokes coded a few QP above where they could be, until
/// the still-screen passes sharpen them 150 ms after the last one.
///
/// ── The same picture again ─────────────────────────────────────────────────
///
/// The still-screen passes and the idle floor encode the picture that already
/// went out, and every pass codes what the one before it left out: its size
/// says what refining costs, not what the next new picture will. So passes
/// have an L of their own, started from the picture's and replaced by each
/// pass that refined, and they never move the one new pictures are sized by —
/// the first movement after a sharpened desktop is budgeted as movement, not
/// as the nothing the last pass cost. A pass goes down 6 at most (the step
/// halves) and never above the QP the picture already has: coded there, the
/// residual is under the quantizer and the pass costs next to nothing, which
/// is what an idle-floor frame should cost. From where a moving picture leaves
/// the QP (30 to 45), 18 is three to five passes away, inside the cap of
/// RefineConvergence with the two quiet passes it wants on top.
///
/// ── What it does not do ────────────────────────────────────────────────────
///
/// It sees whole pictures: a picture far over its budget has gone out before
/// anything can be done about it. reencode() gives the QP that would have
/// fitted, for an encoder that can afford to code the picture again (the
/// bench's reencode=, a decision the bench's numbers wait on — plan §9-5).
class QpRateController
{
public:
    /// The engine's floor, as on every encoder: below it a still picture
    /// spends bits on noise nobody sees.
    static constexpr int kMinQp = 18;
    static constexpr int kMaxQp = 51;
    /// Where complexity is measured: the model's pivot.
    static constexpr int kPivotQp = 26;
    /// The most the QP falls in one new picture, on the model's word alone.
    static constexpr int kNewPictureDrop = 3;
    /// The most one pass over the same picture sharpens it.
    static constexpr int kPassDrop = 6;
    /// log2 of how far a picture may miss before it replaces the estimate.
    static constexpr double kCut = 1.0;
    /// The budget's floor, as a share of a frame's worth.
    static constexpr int kMinBudgetShare = 8;
    /// How far a new picture's cost may be believed to fall below an intra
    /// picture's, log2: a quarter.
    static constexpr double kInterFloor = -2.0;
    /// A picture this many times over its budget — and over the VBV — is the
    /// overshoot the bench counts and reencode= codes again.
    static constexpr double kStrongOvershoot = 2.5;
    /// Before the first intra picture: bits per pixel at QP 26. A desktop of
    /// text sits at or below it — high on purpose: a first picture under its
    /// budget costs a pass of refinement, one over it a stall.
    static constexpr double kIntraBitsPerPixel = 1.0;
    /// Before the first new inter picture: a third of the intra one, log2.
    static constexpr double kInterShare = -1.585;

    enum class Kind
    {
        Intra, ///< an IDR, new picture or not
        Inter, ///< a new picture, predicted
        Pass,  ///< the same picture again: a still-screen pass, the idle floor
    };

    /// What one picture is to be coded at, and against what budget.
    struct Picture
    {
        Kind kind = Kind::Inter;
        int qp = kPivotQp;
        uint64_t budgetBits = 0;
    };

    /// A session starts: @p bitsPerSecond at @p fps, the VBV by the shared
    /// rule (or @p vbvFrames frames' worth, the bench's vbv=), for pictures
    /// of @p pixels.
    void start(uint32_t bitsPerSecond, int fps, int vbvFrames, uint64_t pixels)
    {
        *this = QpRateController{};
        m_Fps = fps > 0 ? fps : 60;
        m_VbvFrames = vbvFrames;
        setBitrate(bitsPerSecond);
        const double px = static_cast<double>(pixels > 0 ? pixels : 1);
        m_Intra = std::log2(px * kIntraBitsPerPixel);
        m_Inter = m_Intra + kInterShare;
        m_InterHeld = m_Inter;
    }

    /// A new target, from the next picture on — no new sequence, no IDR. The
    /// bucket keeps what it holds: those bits are on the wire whatever the
    /// rate now is.
    void setBitrate(uint32_t bitsPerSecond)
    {
        const uint32_t bps = bitsPerSecond > 0 ? bitsPerSecond : 1000;
        m_FrameBits = (std::max)(uint64_t(bps / static_cast<uint32_t>(m_Fps)), uint64_t(1));
        m_VbvBits = (std::max)(uint64_t(vbvBits(bps, m_Fps, m_VbvFrames)), m_FrameBits);
    }

    /// The QP of the next picture: an IDR (@p intra), or a predicted one —
    /// new (@p newPicture) or the same picture as the last one encoded. One
    /// call per picture encoded: the rounding of new pictures carries over.
    Picture plan(bool intra, bool newPicture)
    {
        Picture p;
        const double left = static_cast<double>(m_Fullness) / 2.0;
        if (intra) {
            p.kind = Kind::Intra;
            p.budgetBits = budget(static_cast<double>(m_VbvBits) - left);
            // An intra picture costs at least what a predicted one of the same
            // content does: the intra estimate may be from another screen.
            p.qp = clampQp(qpFor((std::max)(m_Intra, believedInter()), p.budgetBits));
            return p;
        }
        p.budgetBits = budget(static_cast<double>(m_FrameBits) - left);
        if (newPicture || !m_HavePicture) {
            p.kind = Kind::Inter;
            // The fraction rounding leaves goes to the next picture, so a
            // steady picture does not sit half a step off its budget for good:
            // the bucket catches a picture over it, nothing catches one under.
            const double wanted = qpFor(believedInter(), p.budgetBits) + m_Carry;
            p.qp = clampQp(wanted);
            m_Carry = (p.qp == kMinQp || p.qp == kMaxQp) ? 0.0 : wanted - p.qp;
            return p;
        }
        p.kind = Kind::Pass;
        const int sharpest = (std::max)(kMinQp, m_PictureQp - kPassDrop);
        p.qp = std::clamp(static_cast<int>(std::lround(qpFor(m_Pass, p.budgetBits))), sharpest,
                          (std::max)(sharpest, m_PictureQp));
        return p;
    }

    /// @p p went out at @p bits.
    void encoded(const Picture& p, uint64_t bits)
    {
        const double measured = complexity(p.qp, bits);
        const int64_t level = static_cast<int64_t>(m_Fullness) + static_cast<int64_t>(bits) -
                              static_cast<int64_t>(m_FrameBits);
        m_Fullness = level > 0 ? static_cast<uint64_t>(level) : 0;
        switch (p.kind) {
        case Kind::Intra:
            // The first measure replaces the guess, whatever the distance.
            m_Intra = m_IntraSeen ? learn(m_Intra, measured) : measured;
            m_IntraSeen = true;
            // Until a new picture has been measured, what one costs is only
            // known through the intra one: the guess follows it.
            if (!m_InterSeen) m_Inter = m_InterHeld = m_Intra + kInterShare;
            startPicture(p.qp, measured);
            break;
        case Kind::Inter:
            m_Inter = m_InterSeen ? learn(m_Inter, measured) : measured;
            m_InterSeen = true;
            m_InterHeld = m_Inter >= m_InterHeld
                              ? m_Inter
                              : (std::max)(m_Inter, m_InterHeld - kNewPictureDrop / 6.0);
            startPicture(p.qp, measured);
            break;
        case Kind::Pass:
            // A pass at the picture's own QP refined nothing, and its size
            // says nothing of what refining costs.
            if (p.qp < m_PictureQp) {
                m_Pass = measured;
                m_PictureQp = p.qp;
            }
            break;
        }
        ++m_Pictures;
        m_QpSum += static_cast<uint64_t>(p.qp);
        if (strongOvershoot(p, bits)) ++m_StrongOvershoots;
    }

    /// Whether @p bits is far enough over @p p's budget that coding the
    /// picture again would be worth it.
    bool strongOvershoot(const Picture& p, uint64_t bits) const
    {
        const double b = static_cast<double>(bits);
        return b > kStrongOvershoot * static_cast<double>(p.budgetBits) &&
               b > static_cast<double>(m_VbvBits);
    }

    /// @p p again, at the QP that would have fitted its budget, going by what
    /// it cost at its own.
    Picture reencode(const Picture& p, uint64_t bits) const
    {
        Picture again = p;
        const double over =
            static_cast<double>(bits) / static_cast<double>((std::max)(p.budgetBits, uint64_t(1)));
        const int raise = static_cast<int>(std::ceil(6.0 * std::log2((std::max)(over, 1.0))));
        again.qp = (std::min)(kMaxQp, p.qp + (std::max)(raise, 1));
        return again;
    }

    uint64_t frameBits() const { return m_FrameBits; }
    uint64_t vbvCapacityBits() const { return m_VbvBits; }
    /// Bits still on the wire before the next picture's.
    uint64_t fullnessBits() const { return m_Fullness; }
    /// The QP the picture on screen has been sharpened to.
    int pictureQp() const { return m_PictureQp; }

    int pictures() const { return m_Pictures; }
    int strongOvershoots() const { return m_StrongOvershoots; }
    double meanQp() const
    {
        return m_Pictures > 0 ? static_cast<double>(m_QpSum) / m_Pictures : 0.0;
    }

private:
    static double complexity(int qp, uint64_t bits)
    {
        return std::log2(static_cast<double>((std::max)(bits, uint64_t(1)))) +
               (qp - kPivotQp) / 6.0;
    }

    static double qpFor(double complexityLog2, uint64_t budgetBits)
    {
        return kPivotQp + 6.0 * (complexityLog2 - std::log2(static_cast<double>(
                                                      (std::max)(budgetBits, uint64_t(1)))));
    }

    static int clampQp(double qp)
    {
        return std::clamp(static_cast<int>(std::lround(qp)), kMinQp, kMaxQp);
    }

    static double learn(double estimate, double measured)
    {
        return std::fabs(measured - estimate) > kCut ? measured : (estimate + measured) / 2.0;
    }

    uint64_t budget(double bits) const
    {
        const double floor = static_cast<double>(m_FrameBits) / kMinBudgetShare;
        return static_cast<uint64_t>((std::max)(bits, floor));
    }

    /// What a new picture is taken to cost: the brake of kNewPictureDrop, and
    /// never below kInterFloor under the intra picture.
    double believedInter() const { return (std::max)(m_InterHeld, m_Intra + kInterFloor); }

    void startPicture(int qp, double measured)
    {
        m_PictureQp = qp;
        m_Pass = measured;
        m_HavePicture = true;
    }

    int m_Fps = 60;
    int m_VbvFrames = 0;
    uint64_t m_FrameBits = 1;
    uint64_t m_VbvBits = 1;
    uint64_t m_Fullness = 0;

    double m_Intra = 20.0;
    double m_Inter = 18.4;
    /// m_Inter as decisions use it: up at once, down by kNewPictureDrop.
    double m_InterHeld = 18.4;
    double m_Pass = 18.4;
    double m_Carry = 0.0;
    bool m_IntraSeen = false;
    bool m_InterSeen = false;
    bool m_HavePicture = false;
    int m_PictureQp = kMaxQp;

    int m_Pictures = 0;
    uint64_t m_QpSum = 0;
    int m_StrongOvershoots = 0;
};

} // namespace mw::native::encode
