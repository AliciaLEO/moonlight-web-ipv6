/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
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

/**
 * CadenceStepper — "Auto" with detection, the client's half (design §33.10).
 *
 * ── Why ──────────────────────────────────────────────────────────────────────
 *
 * "Auto" streams at this screen's refresh rate. When what the host shows
 * changes faster — a page scrolling at 240 Hz on the virtual display, a game at
 * 80 frames a second for a 60 Hz screen — the host's gate carries one present
 * per refresh of this screen, and what a skipped present showed waits for the
 * next one. A canvas that tears paints each frame the moment it is decoded, so
 * a faster stream shows it younger pictures: 3 to 6 ms on the bench, on a
 * client that keeps up (the UM790Pro on Ethernet, the Mac). On one that does
 * not — the N95 on Wi-Fi, a weak iGPU — the frames queue somewhere between the
 * host and the screen and the picture gets OLDER, by tens or hundreds of
 * milliseconds. Which of the two a client is cannot be known in advance: this
 * measures it, a step at a time, and gives up the moment it costs.
 *
 * ── The ladder ───────────────────────────────────────────────────────────────
 *
 * The stream's own rate (Auto's), twice it, then the captured display's
 * refresh (240 on the product's virtual display), each capped at that refresh
 * (ladder()). A step is tried only while the host says its display presents
 * clearly faster than the current one — `stats.cadence.presents` above FASTER
 * times it: a game at 50 frames a second on a 60 Hz client never starts one.
 *
 * ── A trial ──────────────────────────────────────────────────────────────────
 *
 * BASE_MS at the current step, then the next one is asked for (`fpsstep`) and
 * the host answers at once: applied, capped, refused. SETTLE_MS after the
 * answer, TRIAL_MS are measured. A window's age is what the viewer looks at:
 * the median of capture → painted on the host's clock (ClockEstimator, fed by
 * the pongs), plus half the interval between two painted frames — a picture
 * stays up until the next one. The step is kept when the age drops by GAIN_MS
 * or more and the client paints PAINTED_MIN of what it receives; otherwise the
 * stream goes back to the step before, and the next trial waits BACKOFF_MS —
 * 30 s, 1, 2, 4 min… — reset when the content or the link changes. A kept step
 * is the base of the next trial, if there is one and the content asks for it.
 *
 * ── The net ──────────────────────────────────────────────────────────────────
 *
 * Armed whenever the stream runs above this screen's rate, trials included:
 * capture → painted over the last FILET_WINDOW_MS above its reference — the
 * same, measured at the client's own rate — by half a refresh, or QUEUE_FULL
 * frames at the decoder for QUEUE_HOLD_MS, and the stream is back at the
 * client's rate in one round trip, whatever the cause (Wi-Fi, main thread,
 * decoder).
 *
 * ── Where it runs ────────────────────────────────────────────────────────────
 *
 * StreamView decides: a native host, a rate left to Auto, a canvas that tears
 * (Chromium on a desktop, its default), frames decoded and drawn on the main
 * thread. A client on vsync shows one frame per refresh and keeps its rate.
 * Pure apart from its two timers: the caller feeds the frames, the pongs, the
 * stats and the host's answers.
 */

import { ClockEstimator } from '../util/ClockEstimator.js';

/** After the first painted frame: a stream's first second is start-up traffic. */
export const WARMUP_MS = 1500;
/** The window a trial is compared with, at the current step. */
export const BASE_MS = 2500;
/** After the host's answer, before the trial is measured. */
export const SETTLE_MS = 400;
/** The trial's measured window. */
export const TRIAL_MS = 2500;
/** No answer by then: the step is taken back. */
export const REPLY_TIMEOUT_MS = 2000;
/** The age must drop at least this much for a step to be kept, ms. */
export const GAIN_MS = 1;
/** …and the client must paint at least this share of what it receives. */
export const PAINTED_MIN = 0.9;
/** Content counts as faster than a step above this many times its rate. */
export const FASTER = 1.15;
/** Fewest painted frames a window is judged on. */
export const MIN_FRAMES = 20;
/** The net looks at this much of the latest capture → painted… */
export const FILET_WINDOW_MS = 250;
/** …once it holds this many frames. */
export const FILET_MIN_FRAMES = 4;
/** Frames waiting at the decoder that make a queue… */
export const QUEUE_FULL = 2;
/** …once it has stood this long. */
export const QUEUE_HOLD_MS = 150;
/** Waits before the next trial, after each failed one. */
export const BACKOFF_MS = [30000, 60000, 120000, 240000, 480000, 960000];
/** Stats in a row before the content counts as changed. */
export const CONTENT_STABLE = 2;
/** Stats older than this say nothing about the content. */
export const STATS_STALE_MS = 3000;
/** Stats in a row before a step the host no longer runs is let go. */
export const HOST_MISMATCH = 2;
/** The state machine's clock, and the pings for the host's clock. */
export const TICK_MS = 100;
export const PING_EVERY_MS = 500;
/** How long frames are kept: a base window, an answer, a settle, a trial. */
export const KEEP_MS = 9000;
/** How many decisions the bench can read back. */
const EVENTS_KEPT = 200;

const WRAP_MS = 2 ** 32;

/**
 * Whether the detection runs: the switch `mw_autostep` in localStorage, '1'
 * for on. Off otherwise until the bench has judged it (plan, porte UA).
 * @returns {boolean}
 */
export function autostepEnabled() {
    try {
        return localStorage.getItem('mw_autostep') === '1';
    } catch (e) {
        return false;
    }
}

/**
 * The stream's steps: its own rate, twice it, then the display's refresh,
 * each capped at that refresh and above the one before. [] without a rate.
 * @param {number} baseFps
 * @param {number} displayHz 0 when unknown: no cap, no display step
 * @returns {number[]}
 */
export function ladder(baseFps, displayHz) {
    if (!(baseFps > 0)) return [];
    const levels = [Math.round(baseFps)];
    const add = (fps) => {
        const capped = displayHz > 0 ? Math.min(fps, displayHz) : fps;
        if (capped > levels[levels.length - 1]) levels.push(capped);
    };
    add(levels[0] * 2);
    if (displayHz > 0) add(Math.round(displayHz));
    return levels;
}

/** The median of an ascending array. */
function median(sorted) {
    const n = sorted.length;
    if (!n) return NaN;
    return n % 2 ? sorted[n >> 1] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2;
}

export class CadenceStepper {
    /**
     * @param {object} deps
     * @param {number} deps.baseFps the stream's own rate, Auto's choice
     * @param {(msg: object) => void} deps.send a control message to the host
     * @param {(seq: number, ts: number) => void} [deps.sendPing] a `ping` the
     *        host answers with a `pong` carrying `host` (its steady µs)
     * @param {() => number} [deps.now] this client's clock, ms
     * @param {(line: string) => void} [deps.log]
     * @param {{every: (fn: () => void, ms: number) => any, stop: (id: any) => void}} [deps.timers]
     */
    constructor({ baseFps, send, sendPing, now, log, timers }) {
        this._send = send;
        this._sendPing = sendPing || (() => {});
        this._now = now || (() => performance.now());
        this._log = log || ((line) => console.log(line));
        this._timers = timers || {
            every: (fn, ms) => setInterval(fn, ms),
            stop: (id) => clearInterval(id),
        };
        this._clock = new ClockEstimator();
        this._base = baseFps > 0 ? Math.round(baseFps) : 0;
        this._display = 0;
        this._levels = ladder(this._base, 0);
        this._presents = 0;
        this._statsAt = -Infinity;
        this._hostStep = 0;
        this._mismatch = 0;
        this._level = 0; // the step kept: an index into _levels
        this._phase = 'idle'; // idle | base | asking | settling | trial
        this._try = 0; // the step being tried
        this._trialFps = 0; // …and the rate the host granted for it
        this._baseFrom = 0;
        this._baseTo = 0;
        this._askedAt = 0;
        this._trialFrom = 0;
        /** @type {{at: number, ts: number}[]} painted frames: when, and their capture (host ms) */
        this._painted = [];
        /** @type {number[]} when frames arrived */
        this._received = [];
        this._firstPaintAt = 0;
        this._reference = null; // capture → painted at the client's own rate, ms
        this._queueSince = 0;
        this._backoff = 0;
        this._nextTrialAt = 0;
        this._band = 0;
        this._bandSeen = 0;
        this._bandCount = 0;
        this._capped = false;
        this._running = false;
        this._tickTimer = null;
        this._pingTimer = null;
        this._seq = 1 << 22; // apart from the view's, the grid's and the probe's pings
        this.trials = 0;
        this.kept = 0;
        this.rejected = 0;
        this.refused = 0;
        this.trips = 0;
        /** @type {object[]} what was decided, and on what — for the bench */
        this.events = [];
    }

    get running() {
        return this._running;
    }

    /** The rate the stream is asked to run at: 0 for its own. */
    get stepFps() {
        return this._level > 0 ? this._levels[this._level] : 0;
    }

    start() {
        if (this._running) return;
        this._running = true;
        this._pingTimer = this._timers.every(
            () => this._sendPing(this._seq++, this._now()),
            PING_EVERY_MS,
        );
        this._tickTimer = this._timers.every(() => this.tick(this._now()), TICK_MS);
    }

    /** Stops measuring; a step in force is taken back. */
    stop() {
        if (!this._running) return;
        this._running = false;
        this._timers.stop(this._pingTimer);
        this._timers.stop(this._tickTimer);
        this._pingTimer = null;
        this._tickTimer = null;
        if (this._stepped()) this._ask(0);
        this._level = 0;
        this._phase = 'idle';
    }

    /** A pong: the host's clock. */
    notePong(msg, recvMs) {
        if (msg && typeof msg.host === 'number' && typeof msg.ts === 'number')
            this._clock.note(msg.ts, msg.host, recvMs);
    }

    /**
     * The host's word on its cadence, once a second (`stats.cadence`): its
     * display's presents per second, the stream's own rate, the step it
     * runs, its display's refresh.
     */
    noteStats(cadence, now) {
        if (!cadence || typeof cadence !== 'object') return;
        this._statsAt = now;
        this._presents = cadence.presents > 0 ? cadence.presents : 0;
        this._hostStep = cadence.step > 0 ? cadence.step : 0;
        const base = cadence.base > 0 ? Math.round(cadence.base) : 0;
        const display = cadence.display > 0 ? Math.round(cadence.display) : 0;
        if (base > 0 && (base !== this._base || display !== this._display)) {
            const moved = this._base > 0 && base !== this._base;
            this._base = base;
            this._display = display;
            this._levels = ladder(base, display);
            if (moved) this._restart("the stream's own rate moved", now);
            else if (this._level >= this._levels.length) this._fallBack('no such step', now);
        }
        this._noteBand(now);
        this._reconcile(now);
    }

    /** The host's answer to an `fpsstep`. */
    noteReply(msg, now) {
        if (this._phase !== 'asking' || !msg) return;
        const fps = msg.fps > 0 ? msg.fps : 0;
        const ok = (msg.verdict === 'applied' || msg.verdict === 'capped') && fps > this._rate();
        if (!ok) {
            // The step in force before, if any, stays (the host keeps it).
            this.refused++;
            this._event('refused', now, {
                asked: msg.asked,
                why: msg.why || msg.verdict,
                fps,
            });
            this._log(
                `[CadenceStepper] ${msg.asked} fps refused by the host` +
                    (msg.why ? ': ' + msg.why : '') +
                    ` — staying at ${this._rate()} fps`,
            );
            this._phase = 'idle';
            this._delay(now);
            return;
        }
        this._phase = 'settling';
        this._trialFrom = now + SETTLE_MS;
        this._trialFps = fps;
        this._event('trial', now, { from: this._rate(), to: fps, verdict: msg.verdict });
    }

    /** A frame arrived, whole. */
    noteReceived(at) {
        this._received.push(at);
    }

    /** A frame was painted at @p paintedMs; @p backendTs its capture, host ms (32 bits). */
    notePainted(backendTs, paintedMs) {
        if (!(backendTs > 0)) return;
        if (!this._firstPaintAt) this._firstPaintAt = paintedMs;
        this._painted.push({ at: paintedMs, ts: backendTs });
        this._checkNet(paintedMs);
    }

    /** VideoDecoder.decodeQueueSize, as it stands at @p now. */
    noteDecodeQueue(depth, now) {
        if (depth >= QUEUE_FULL) {
            if (!this._queueSince) this._queueSince = now;
            this._checkNet(now);
        } else {
            this._queueSince = 0;
        }
    }

    /** The decoder asked the host for fewer frames (`clientfpscap`): no step meanwhile. */
    setDecoderCapped(capped, now) {
        this._capped = capped === true;
        if (this._capped && (this._stepped() || this._phase !== 'idle'))
            this._fallBack('the decoder asked for fewer frames', now);
    }

    /** The link or the screen changed under the stream: start over. */
    linkChanged(why, now) {
        this._restart(why || 'the link changed', now);
    }

    /** The state machine; on its own timer once started. */
    tick(now) {
        if (!this._running) return;
        this._prune(now);
        switch (this._phase) {
            case 'idle':
                if (this._mayTry(now)) {
                    this._phase = 'base';
                    this._baseFrom = now;
                }
                break;
            case 'base':
                if (now - this._baseFrom >= BASE_MS) this._askNext(now);
                break;
            case 'asking':
                if (now - this._askedAt >= REPLY_TIMEOUT_MS) {
                    this._log('[CadenceStepper] no answer from the host — step taken back');
                    this._event('timeout', now, {});
                    this._ask(this.stepFps);
                    this._phase = 'idle';
                    this._delay(now);
                }
                break;
            case 'settling':
            case 'trial':
                if (now >= this._trialFrom) this._phase = 'trial';
                if (now >= this._trialFrom + TRIAL_MS) this._judge(now);
                break;
        }
    }

    // ── The steps ───────────────────────────────────────────────────────────

    /** The rate the stream runs at now: the kept step's. */
    _rate() {
        return this._levels.length ? this._levels[this._level] : this._base;
    }

    /** Whether the host may be running above this screen's rate right now. */
    _stepped() {
        return (
            this._level > 0 ||
            this._phase === 'asking' ||
            this._phase === 'settling' ||
            this._phase === 'trial'
        );
    }

    _mayTry(now) {
        if (this._capped || this._level + 1 >= this._levels.length) return false;
        if (now < this._nextTrialAt) return false;
        if (!this._clock.ready || !this._firstPaintAt) return false;
        if (now - this._firstPaintAt < WARMUP_MS) return false;
        if (now - this._statsAt > STATS_STALE_MS) return false;
        return this._presents > this._rate() * FASTER;
    }

    /** Close the base window and ask for the next step. */
    _askNext(now) {
        const base = this._window(this._baseFrom, now);
        if (!base) {
            // Too few frames to judge (the content stopped): look again soon.
            this._phase = 'idle';
            this._nextTrialAt = now + 1000;
            return;
        }
        if (this._level === 0) this._reference = base.latency;
        this._baseTo = now;
        this._try = this._level + 1;
        this._phase = 'asking';
        this._askedAt = now;
        this.trials++;
        this._ask(this._levels[this._try]);
        this._log(
            `[CadenceStepper] trying ${this._levels[this._try]} fps (from ${this._rate()}): ` +
                `content at ${this._presents}/s, age ${base.age.toFixed(1)} ms here`,
        );
    }

    /** The trial's window is in: keep the step, or go back. */
    _judge(now) {
        const base = this._window(this._baseFrom, this._baseTo);
        const trial = this._window(this._trialFrom, now);
        if (!base || !trial) {
            // The content stopped meanwhile: nothing to judge, no blame.
            this._event('inconclusive', now, {});
            this._ask(this.stepFps);
            this._phase = 'idle';
            this._nextTrialAt = now + 1000;
            return;
        }
        const gain = base.age - trial.age;
        const keep = gain >= GAIN_MS && trial.ratio >= PAINTED_MIN;
        const facts = {
            from: this._rate(),
            to: this._trialFps,
            baseAge: base.age,
            trialAge: trial.age,
            gain,
            ratio: trial.ratio,
            baseLatency: base.latency,
            trialLatency: trial.latency,
        };
        this._log(
            `[CadenceStepper] ${this._trialFps} fps ${keep ? 'kept' : 'given up'}: age ` +
                `${base.age.toFixed(1)} → ${trial.age.toFixed(1)} ms, ` +
                `${Math.round(trial.ratio * 100)} % of the frames painted`,
        );
        if (!keep) {
            this.rejected++;
            this._event('rejected', now, facts);
            this._ask(this.stepFps);
            this._phase = 'idle';
            this._delay(now);
            return;
        }
        this.kept++;
        this._event('kept', now, facts);
        // The step is what the host granted: capped below the rate asked when
        // the ladder was built before the display's refresh was known.
        if (this._trialFps !== this._levels[this._try]) {
            const above = this._levels.slice(this._try + 1).filter((f) => f > this._trialFps);
            this._levels = this._levels.slice(0, this._try).concat([this._trialFps], above);
        }
        this._level = this._try;
        this._backoff = 0;
        this._phase = 'idle';
        // The next step, at once, from this window, when the content asks for it.
        if (this._mayTry(now)) {
            this._baseFrom = this._trialFrom;
            this._askNext(now);
        }
    }

    /** Tell the host: the stream at @p fps, 0 for its own rate. */
    _ask(fps) {
        this._send({ type: 'fpsstep', fps: fps > 0 ? fps : 0 });
    }

    /** The next trial waits, longer each time it is called. */
    _delay(now) {
        this._nextTrialAt = now + BACKOFF_MS[Math.min(this._backoff, BACKOFF_MS.length - 1)];
        this._backoff++;
    }

    /** Back to the client's own rate at once, and wait before trying again. */
    _fallBack(why, now) {
        if (this._stepped()) this._ask(0);
        this._event('fallback', now, { why, from: this.stepFps || this._trialFps || 0 });
        this._log(`[CadenceStepper] back to ${this._base} fps: ${why}`);
        this._level = 0;
        this._phase = 'idle';
        this._queueSince = 0;
        this._delay(now);
    }

    /** Start over: back to the client's rate, the backoff forgotten. */
    _restart(why, now) {
        if (this._stepped()) this._ask(0);
        this._event('restart', now, { why });
        this._level = 0;
        this._phase = 'idle';
        this._reference = null;
        this._backoff = 0;
        this._nextTrialAt = now + WARMUP_MS;
    }

    // ── The net ─────────────────────────────────────────────────────────────

    _checkNet(now) {
        if (!this._stepped()) return;
        if (this._queueSince && now - this._queueSince >= QUEUE_HOLD_MS) {
            this.trips++;
            this._fallBack('a decode queue that holds', now);
            return;
        }
        if (this._reference === null || !this._clock.ready) return;
        const recent = [];
        for (let i = this._painted.length - 1; i >= 0; i--) {
            const s = this._painted[i];
            if (s.at < now - FILET_WINDOW_MS) break;
            const ms = this._latency(s);
            if (ms !== null) recent.push(ms);
        }
        if (recent.length < FILET_MIN_FRAMES) return;
        const latency = median(recent.sort((a, b) => a - b));
        const half = 500 / this._base;
        if (latency > this._reference + half) {
            this.trips++;
            this._fallBack(
                `capture → painted ${latency.toFixed(1)} ms, over ` +
                    `${this._reference.toFixed(1)} + ${half.toFixed(1)} at the client's rate`,
                now,
            );
        }
    }

    // ── The content ─────────────────────────────────────────────────────────

    /**
     * How many steps the content could use; when that changes and holds, the
     * content changed and the backoff is forgotten.
     */
    _noteBand(now) {
        let band = 0;
        for (let i = 1; i < this._levels.length; i++)
            if (this._presents > this._levels[i - 1] * FASTER) band = i;
        if (band === this._band) {
            this._bandCount = 0;
            return;
        }
        if (band === this._bandSeen) this._bandCount++;
        else {
            this._bandSeen = band;
            this._bandCount = 1;
        }
        if (this._bandCount < CONTENT_STABLE) return;
        this._band = band;
        this._bandCount = 0;
        this._backoff = 0;
        if (this._phase === 'idle') this._nextTrialAt = Math.min(this._nextTrialAt, now);
        this._event('content', now, { presents: this._presents, band });
    }

    /** The host's step against ours: a step it dropped (a decoder cap) is let go. */
    _reconcile(now) {
        if (this._phase !== 'idle') {
            this._mismatch = 0;
            return;
        }
        const ours = this.stepFps;
        if (ours > 0 === this._hostStep > 0) {
            this._mismatch = 0;
            return;
        }
        if (++this._mismatch < HOST_MISMATCH) return;
        this._mismatch = 0;
        if (ours > 0) this._fallBack('the host no longer runs the step', now);
        else this._ask(0);
    }

    // ── The measure ─────────────────────────────────────────────────────────

    /** Capture → painted of one frame on the host's clock, ms; null when unknown. */
    _latency(s) {
        if (!this._clock.ready) return null;
        const paintedMs = this._clock.toHostUs(s.at) / 1000;
        const captureMs = s.ts + Math.round((paintedMs - s.ts) / WRAP_MS) * WRAP_MS;
        const ms = paintedMs - captureMs;
        return ms > 0 && ms < 1000 ? ms : null;
    }

    /**
     * What a viewer looked at between @p from and @p to: capture → painted
     * (median), the interval between painted frames, their sum as the age of
     * what is shown, and the share of the frames received that were painted.
     * Null under MIN_FRAMES.
     */
    _window(from, to) {
        const latencies = [];
        let first = Infinity;
        let last = -Infinity;
        for (const s of this._painted) {
            if (s.at < from || s.at > to) continue;
            const ms = this._latency(s);
            if (ms === null) continue;
            latencies.push(ms);
            if (s.at < first) first = s.at;
            if (s.at > last) last = s.at;
        }
        const painted = latencies.length;
        if (painted < MIN_FRAMES) return null;
        latencies.sort((a, b) => a - b);
        const latency = median(latencies);
        const interval = (last - first) / (painted - 1);
        let received = 0;
        for (const at of this._received) if (at >= from && at <= to) received++;
        return {
            latency,
            interval,
            age: latency + interval / 2,
            painted,
            received,
            ratio: received > 0 ? Math.min(1, painted / received) : 1,
        };
    }

    _prune(now) {
        const horizon = now - KEEP_MS;
        let i = 0;
        while (i < this._painted.length && this._painted[i].at < horizon) i++;
        if (i) this._painted.splice(0, i);
        let j = 0;
        while (j < this._received.length && this._received[j] < horizon) j++;
        if (j) this._received.splice(0, j);
    }

    _event(what, now, facts) {
        this.events.push({ at: Math.round(now), what, ...facts });
        if (this.events.length > EVENTS_KEPT) this.events.shift();
    }

    /** Where the detection stands, for the bench and the console. */
    get summary() {
        return {
            running: this._running,
            base: this._base,
            display: this._display,
            presents: this._presents,
            levels: this._levels.slice(),
            stepFps: this.stepFps,
            hostStep: this._hostStep,
            phase: this._phase,
            referenceMs: this._reference,
            trials: this.trials,
            kept: this.kept,
            rejected: this.rejected,
            refused: this.refused,
            trips: this.trips,
            backoff: this._backoff,
            nextTrialInMs: Math.max(0, Math.round(this._nextTrialAt - this._now())),
            clock: this._clock.summary,
        };
    }
}
