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

#include "core/DecodeCredit.h"
#include "native_test_framework.h"

#include <deque>

using mw::native::DecodeCredit;

namespace {

/// A host presenting at @p presentHz against a client decoding one frame per
/// @p decodeUs, @p oneWayUs apart each way, for two seconds. The client speaks
/// as frontend DecodeQueueSignal.js does: "full" at two frames waiting, again
/// every 50 ms while it lasts, "clear" back at one. @p guarded: the host reads
/// the credit (and sends the picture it held once it is back); otherwise it
/// sends every present, as cadence=host does.
struct CreditSim
{
    int sent = 0;
    int decoded = 0;
    int held = 0;
    int maxQueue = 0;
};

CreditSim simulate(bool guarded, int presentHz, int64_t decodeUs, int64_t oneWayUs)
{
    CreditSim sim;
    DecodeCredit credit;
    const int64_t presentStep = 1000000 / presentHz;
    int64_t nextPresentUs = 0;
    std::deque<int64_t> toClient;               // arrival times of frames on the wire
    std::deque<std::pair<int64_t, int>> toHost; // (arrival, depth) of the client's words
    int queue = 0;
    int64_t busyUntil = 0;
    bool decoding = false;
    bool full = false;
    int64_t saidAt = -1000000;
    bool heldPicture = false;
    int64_t lastTickUs = 0;

    const auto observe = [&](int64_t now) {
        if (queue >= DecodeCredit::kFullAt) {
            if (full && now - saidAt < 50000) return;
            full = true;
            saidAt = now;
            toHost.emplace_back(now + oneWayUs, queue);
        } else if (full && queue <= 1) {
            full = false;
            saidAt = now;
            toHost.emplace_back(now + oneWayUs, queue);
        }
    };
    const auto send = [&](int64_t now) {
        sim.sent++;
        toClient.push_back(now + oneWayUs);
    };

    for (int64_t t = 0; t < 2000000; t += 100) {
        // The client's words reach the host.
        while (!toHost.empty() && toHost.front().first <= t) {
            credit.note(toHost.front().second, t);
            toHost.pop_front();
        }
        // The host: a present, or the held picture once the credit is back.
        if (t >= nextPresentUs) {
            nextPresentUs += presentStep;
            if (guarded && credit.missing(t)) {
                sim.held++;
                heldPicture = true;
            } else {
                heldPicture = false;
                send(t);
            }
        } else if (heldPicture && t - lastTickUs >= 1000 && !credit.missing(t)) {
            heldPicture = false;
            send(t);
        }
        if (t - lastTickUs >= 1000) lastTickUs = t;
        // The client: arrivals join the queue, the decoder takes one at a time.
        while (!toClient.empty() && toClient.front() <= t) {
            toClient.pop_front();
            queue++;
            if (queue > sim.maxQueue) sim.maxQueue = queue;
            observe(t);
        }
        if (decoding && t >= busyUntil) {
            decoding = false;
            sim.decoded++;
            observe(t);
        }
        if (!decoding && queue > 0) {
            queue--;
            decoding = true;
            busyUntil = t + decodeUs;
        }
    }
    return sim;
}

} // namespace

void run_decode_credit_tests()
{
    SECTION("DecodeCredit — nothing heard, nothing held");
    {
        DecodeCredit c;
        CHECK(!c.missing(0));
        CHECK(!c.missing(1000000));
        CHECK_EQ(c.signals(), 0);
    }

    SECTION("DecodeCredit — missing at two frames waiting, back at one");
    {
        DecodeCredit c;
        c.note(1, 0);
        CHECK(!c.missing(10));
        c.note(2, 100);
        CHECK(c.missing(101));
        c.note(5, 200);
        CHECK(c.missing(300));
        c.note(1, 400);
        CHECK(!c.missing(401));
        c.note(0, 500);
        CHECK(!c.missing(501));
        CHECK_EQ(c.signals(), 5);
    }

    SECTION("DecodeCredit — a full queue not said again is not believed");
    {
        DecodeCredit c;
        c.note(3, 0);
        CHECK(c.missing(DecodeCredit::kStaleUs - 1));
        CHECK(!c.missing(DecodeCredit::kStaleUs));
        // Said again: believed again, from then on.
        c.note(3, 200000);
        CHECK(c.missing(400000));
        CHECK(!c.missing(200000 + DecodeCredit::kStaleUs));
    }

    SECTION("DecodeCredit — a depth from a page is bounded");
    {
        DecodeCredit c;
        c.note(-7, 0);
        CHECK(!c.missing(1));
        c.note(1 << 30, 10);
        CHECK(c.missing(11));
    }

    SECTION("DecodeCredit — a 500 Hz host against a 100 fps decoder, LAN");
    {
        // Without the credit (cadence=host) the queue grows by 400 frames a
        // second and never comes down.
        const CreditSim host = simulate(false, 500, 10000, 500);
        CHECK(host.maxQueue > 500);
        // With it, the queue stays at the edge of full — the frames in flight
        // when the word went out, at most — and the host sends about what the
        // decoder takes.
        const CreditSim guarded = simulate(true, 500, 10000, 500);
        CHECK(guarded.maxQueue <= 3);
        CHECK(guarded.decoded >= 190);
        CHECK(guarded.sent <= guarded.decoded + 4);
        CHECK(guarded.held > 700);
    }

    SECTION("DecodeCredit — a decoder that keeps up is never held back");
    {
        // 240 presents a second, 2 ms a decode: the queue never reaches two.
        const CreditSim s = simulate(true, 240, 2000, 500);
        CHECK_EQ(s.held, 0);
        CHECK(s.sent >= 479 && s.sent <= 481);
        CHECK(s.maxQueue <= 1);
    }

    SECTION("DecodeCredit — across a slower link, the queue stays bounded");
    {
        // 10 ms each way: the frames in flight while the word travels land in
        // the queue, then it drains — and the decoder waits a little for the
        // first frame sent after "clear", a round trip later: about nine
        // frames in ten decoded of what it could take (183 of 200 in the
        // model), against a queue of 800 without the credit.
        const CreditSim s = simulate(true, 500, 10000, 10000);
        CHECK(s.maxQueue <= 12);
        CHECK(s.decoded >= 170);
    }
}
