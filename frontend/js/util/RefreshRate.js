/**
 * RefreshRate — what this window's screen refreshes at, measured.
 *
 * ── Why the host wants to know ──────────────────────────────────────────────
 *
 * A client that paints on its vsync (tearing off, or a browser that cannot
 * tear) shows one frame per refresh at most. A 60 fps stream on a 144 Hz
 * screen then lands on ticks 2.4 refreshes apart — some frames stay up two
 * refreshes, some three — and the eye reads it as judder although nothing was
 * lost. The native host can run the stream at a divisor of the client's
 * refresh instead (72 on 144 Hz, 55 on 165 Hz), IF it knows the rate. This
 * module is how it does: the rate goes into /start (`client_refresh_mhz`) and,
 * when the window changes screen mid-stream, into a `clientrefresh` message.
 *
 * ── How it is measured ──────────────────────────────────────────────────────
 *
 * No browser exposes the refresh rate directly. requestAnimationFrame fires
 * once per refresh while the tab is visible, with vsync-aligned timestamps in
 * Chromium, so the mean of ~90 deltas is the period to a tenth of a percent
 * (the timestamps are coarsened, so one delta alone is not). A median sorts
 * the frames the browser skipped under load (a doubled delta) out of that
 * mean — as long as fewer than half are skipped. In a hidden tab rAF does not
 * run at all, so the measurement is skipped and the last good value stands.
 *
 * The result is in millihertz, unrounded: the host multiplies the exact
 * measured period, and 164 800 mHz for a 165 Hz panel is a better number for
 * that than a snapped 165 000 would be if the panel really runs at 164.8.
 *
 * ── On a TV ─────────────────────────────────────────────────────────────────
 *
 * A TV is measured once: its first good reading stands for the page's
 * lifetime. It has one panel, and a stream fills its main thread — rAF then
 * ticks at the page's pace, with more than half the refreshes skipped, which
 * no median sorts out. Under a stream on 01/10/2026 the Mi TV's 60 Hz panel
 * read 34 Hz and the Freebox Player POP's 50 Hz read 29.4; the host took the
 * 34 for a new screen and ran a stream set to 30 at 34 fps, at the ceiling of
 * that TV's decoder.
 */

import { IS_LOW_MEMORY, IS_TV } from './BrowserDetect.js';

/** @type {number} Last good measurement, millihertz. 0 = none yet. */
let _milliHz = 0;
/** @type {Promise<number> | null} A measurement in flight, shared. */
let _inFlight = null;
/** @type {Set<(mhz: number) => void>} */
const _listeners = new Set();
let _monitorStarted = false;
let _resizeTimer = null;

/** A change under one percent is jitter, not a new screen. */
const CHANGE_THRESHOLD = 0.01;

/**
 * Measure the refresh rate now. Resolves with millihertz, or with the last
 * good value (possibly 0) when the tab is hidden or the sampling times out —
 * and on a TV, as soon as there is one ("On a TV" above).
 * Concurrent callers share one measurement.
 * @param {{frames?: number, timeoutMs?: number}} [opts]
 * @returns {Promise<number>}
 */
export function measureRefreshRate(opts = {}) {
    if (_inFlight) return _inFlight;
    if (IS_TV && _milliHz > 0) return Promise.resolve(_milliHz);
    const frames = opts.frames || 90;
    const timeoutMs = opts.timeoutMs || 2500;
    if (typeof document !== 'undefined' && document.hidden) return Promise.resolve(_milliHz);
    if (typeof requestAnimationFrame !== 'function') return Promise.resolve(_milliHz);

    _inFlight = new Promise((resolve) => {
        /** @type {number[]} */
        const stamps = [];
        let done = false;
        const finish = () => {
            if (done) return;
            done = true;
            _inFlight = null;
            clearTimeout(timer);
            resolve(_settle(stamps));
        };
        const timer = setTimeout(finish, timeoutMs);
        const tick = (now) => {
            if (done) return;
            stamps.push(now);
            if (stamps.length >= frames) finish();
            else requestAnimationFrame(tick);
        };
        requestAnimationFrame(tick);
    });
    return _inFlight;
}

/**
 * Turn rAF timestamps into millihertz and publish a change. The first three
 * deltas are dropped: the first callbacks after a request often land
 * mid-refresh. The median only sorts the good deltas from the skipped
 * frames; the period itself is the MEAN of the good ones, because the
 * timestamps are coarsened (to a millisecond, in a page without cross-origin
 * isolation) and a single delta of "6.000" cannot tell 165 Hz from 166.7 —
 * ninety of them can, to a tenth of a percent.
 * @param {number[]} stamps
 * @returns {number}
 */
function _settle(stamps) {
    const deltas = [];
    for (let i = 4; i < stamps.length; i++) {
        const d = stamps[i] - stamps[i - 1];
        if (d > 0) deltas.push(d);
    }
    if (deltas.length < 8) return _milliHz;
    const sorted = deltas.slice().sort((a, b) => a - b);
    const median = sorted[sorted.length >> 1];
    // 10 Hz to 1000 Hz: anything else is not a screen.
    if (!(median >= 1 && median <= 100)) return _milliHz;
    let sum = 0;
    let count = 0;
    for (const d of deltas) {
        // A delta half again the median is a skipped frame, not a refresh.
        if (d > median * 1.5 || d < median * 0.5) continue;
        sum += d;
        count++;
    }
    const period = count > 0 ? sum / count : median;
    const mhz = Math.round(1e6 / period);
    const before = _milliHz;
    _milliHz = mhz;
    if (before === 0 || Math.abs(mhz - before) > before * CHANGE_THRESHOLD) {
        for (const cb of _listeners) {
            try {
                cb(mhz);
            } catch (e) {
                /* a listener's problem */
            }
        }
    }
    return mhz;
}

/** The last measured refresh rate in millihertz, 0 when none was taken yet. */
export function currentRefreshMilliHz() {
    return _milliHz;
}

/** The lowest and highest frame rate "Auto" will ask a host for.
 *
 *  The ceiling is 120 and not the 165 or 240 a panel may run at: past 120 the
 *  smoothness a viewer can feel is bought with a frame every 8 ms across a
 *  capture, an encoder, a network and a decoder, and a choice made FOR the
 *  viewer does not spend that. A viewer who wants their 165 Hz panel fed says
 *  so in the list — a named rate is never held down. The rate that goes to
 *  the host for its vsync alignment is the UNCAPPED measurement
 *  (currentRefreshMilliHz): the grid the frames land on is the panel's, cap
 *  or no cap. */
export const AUTO_FPS_MIN = 24;
export const AUTO_FPS_MAX = 120;

/** Auto's ceiling on a device of a gigabyte of memory or less.
 *
 *  Measured on a Mi TV (MT5867, Mali-G31, 1.4 GB, 01/10/2026) streaming a
 *  moving 720p desktop: asked 60, its browser decoded 34 frames a second
 *  with 112 ms in the decoder and a quarter of the frames thrown away, for
 *  146 ms end to end; asked 30, 25 ms in the decoder and 54 ms end to end.
 *  The decoder alone does 60 and more (127 fps fed flat out); what gives way
 *  is the page around it — every decoded frame waits for the next refresh to
 *  be drawn, and MediaCodec does not hand out the next one meanwhile. The
 *  decode-rate governor finds the same answer, seconds later and after a
 *  burst of keyframes; a choice made FOR the viewer starts where it ends. */
export const LOW_MEMORY_AUTO_FPS_MAX = 30;

/** What a panel was actually measured at, whole frames per second, with no
 *  ceiling on it: what the Settings line says about the screen, and the floor
 *  the pixel budget reads (fitPixelBudget). 0 when nothing was measured. */
export function measuredFps(milliHz) {
    const mhz = milliHz === undefined ? _milliHz : milliHz;
    if (!(mhz > 0)) return 0;
    return Math.round(mhz / 1000);
}

/**
 * The frame rate "Auto" asks for: this screen's own, in whole frames per
 * second, up to AUTO_FPS_MAX. A stream at the screen's rate shows one frame
 * per refresh — no frame encoded for nothing, and no refresh showing the
 * previous one again.
 *
 * The measurement is unrounded on purpose (a 165 Hz panel may run at 164.8),
 * so it is rounded here and nowhere else; 0 — nothing measured yet, a hidden
 * tab at launch, a browser without rAF — answers 0 and the caller's default
 * stands.
 *
 * @param {number} [milliHz] defaults to the last measurement
 * @param {boolean} [lowMemory] a device of a gigabyte or less (BrowserDetect):
 *        the ceiling is then LOW_MEMORY_AUTO_FPS_MAX
 * @returns {number} frames per second, or 0 when unknown
 */
export function autoFps(milliHz, lowMemory = IS_LOW_MEMORY) {
    const fps = measuredFps(milliHz);
    if (!fps) return 0;
    const ceiling = lowMemory ? LOW_MEMORY_AUTO_FPS_MAX : AUTO_FPS_MAX;
    return Math.min(ceiling, Math.max(AUTO_FPS_MIN, fps));
}

/**
 * Be told when the rate changes by more than a percent — the window moved to
 * another screen, typically. Returns the unsubscribe function.
 * @param {(mhz: number) => void} cb
 * @returns {() => void}
 */
export function onRefreshRateChange(cb) {
    _listeners.add(cb);
    return () => {
        _listeners.delete(cb);
    };
}

/**
 * Measure once now and again whenever the window may have changed screen: a
 * resize (a move between monitors of different geometry always resizes; one
 * between identical monitors is caught by the screen change event where the
 * browser has it), a `Screen.change`, and the tab becoming visible again —
 * except on a TV, whose first good reading stands (measureRefreshRate).
 * Idempotent; the first call is enough for the page's lifetime.
 */
export function startRefreshRateMonitor() {
    if (_monitorStarted || typeof window === 'undefined') return;
    _monitorStarted = true;
    const remeasure = () => {
        clearTimeout(_resizeTimer);
        _resizeTimer = setTimeout(() => {
            measureRefreshRate().catch(() => {});
        }, 400);
    };
    window.addEventListener('resize', remeasure);
    document.addEventListener('visibilitychange', () => {
        if (!document.hidden) remeasure();
    });
    try {
        // Window Management API (Chromium): fires when the window's current
        // screen changes its properties — including which screen it is.
        const scr = /** @type {any} */ (window.screen);
        if (scr && typeof scr.addEventListener === 'function')
            scr.addEventListener('change', remeasure);
    } catch (e) {
        /* no screen events here */
    }
    measureRefreshRate().catch(() => {});
}
