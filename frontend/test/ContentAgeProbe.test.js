/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

/**
 * ContentAgeProbe — the band the bench page draws, read back at any scale and
 * through compression; the clock estimate from ping/pong; and the age of what
 * was drawn.
 */
import { describe, it, expect, vi, afterEach } from 'vitest';
import {
    BAND_BITS,
    BAND_BLOCKS,
    bandCheck,
    bandRegion,
    decodeBand,
    unwrapBand,
    ClockEstimator,
    ContentAgeProbe,
    lumaOf,
    refreshCounts,
    shownAges,
} from '../js/stream/ContentAgeProbe.js';

/** Deterministic noise. */
function rng(seed) {
    let s = seed >>> 0;
    return () => {
        s = (s * 1664525 + 1013904223) >>> 0;
        return s / 2 ** 32;
    };
}

/**
 * The band as scroll.html?band=time draws it on a screen @p screenW device
 * pixels wide — block edges at round(i × W / 100) — as bits (1 = white) per
 * block, @p bitsOf giving each block's value.
 */
function pageBand(screenW, bitsOf) {
    const w = Math.round((BAND_BITS * screenW) / BAND_BLOCKS);
    const h = Math.round((2 * screenW) / BAND_BLOCKS);
    const img = new Float32Array(w * h);
    for (let bit = 0; bit < BAND_BITS; bit++) {
        const x0 = Math.round((bit * screenW) / BAND_BLOCKS);
        const x1 = Math.round(((bit + 1) * screenW) / BAND_BLOCKS);
        const one = bitsOf(bit);
        for (let row = 0; row < 2; row++) {
            const y0 = Math.round((row * screenW) / BAND_BLOCKS);
            const y1 = Math.round(((row + 1) * screenW) / BAND_BLOCKS);
            const white = row === 0 ? one : !one;
            for (let y = y0; y < y1; y++)
                for (let x = x0; x < x1; x++) img[y * w + x] = white ? 255 : 0;
        }
    }
    return { img, w, h };
}

/** Bits of a value and its check, as the page lays them out. */
function bitsOfValue(value) {
    const check = bandCheck(value);
    return (bit) =>
        bit < 32 ? Math.floor(value / 2 ** bit) % 2 === 1 : ((check >> (bit - 32)) & 1) === 1;
}

/**
 * What the client reads: the page's band scaled to a frame @p clientW wide
 * (bilinear), with @p noise of luma either way and an optional 3-pixel blur —
 * the region bandRegion() asks for, as RGBA.
 */
function clientStrip(band, screenW, clientW, { noise = 0, blur = false, seed = 1 } = {}) {
    const region = bandRegion(clientW);
    const scale = screenW / clientW;
    const rand = rng(seed);
    const at = (x, y) => {
        const cx = Math.min(band.w - 1, Math.max(0, x));
        const cy = Math.min(band.h - 1, Math.max(0, y));
        return band.img[cy * band.w + cx];
    };
    const sample = (x, y) => {
        // Pixel centre mapped back onto the page, bilinear.
        const sx = (x + 0.5) * scale - 0.5;
        const sy = (y + 0.5) * scale - 0.5;
        const x0 = Math.floor(sx);
        const y0 = Math.floor(sy);
        const fx = sx - x0;
        const fy = sy - y0;
        return (
            at(x0, y0) * (1 - fx) * (1 - fy) +
            at(x0 + 1, y0) * fx * (1 - fy) +
            at(x0, y0 + 1) * (1 - fx) * fy +
            at(x0 + 1, y0 + 1) * fx * fy
        );
    };
    let lum = new Float32Array(region.w * region.h);
    for (let y = 0; y < region.h; y++)
        for (let x = 0; x < region.w; x++) lum[y * region.w + x] = sample(x, y);
    if (blur) {
        const out = new Float32Array(lum.length);
        for (let y = 0; y < region.h; y++)
            for (let x = 0; x < region.w; x++) {
                let s = 0;
                let n = 0;
                for (let dy = -1; dy <= 1; dy++)
                    for (let dx = -1; dx <= 1; dx++) {
                        const xx = x + dx;
                        const yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= region.w || yy >= region.h) continue;
                        s += lum[yy * region.w + xx];
                        n++;
                    }
                out[y * region.w + x] = s / n;
            }
        lum = out;
    }
    const luma = new Uint8ClampedArray(region.w * region.h);
    for (let i = 0; i < lum.length; i++) luma[i] = lum[i] + (rand() * 2 - 1) * noise;
    return { luma, region };
}

function readAt(value, screenW, clientW, opts) {
    const { luma, region } = clientStrip(
        pageBand(screenW, bitsOfValue(value)),
        screenW,
        clientW,
        opts,
    );
    return decodeBand(luma, region.w, region.h, region.block);
}

describe('ContentAgeProbe — the band', () => {
    const values = [0, 1, 0x12345678, 0xffffffff, 0x80000001, 2863311530];

    it('reads back what the page coded, one for one', () => {
        for (const v of values) expect(readAt(v, 1920, 1920)).toEqual({ ok: true, value: v });
    });

    it('reads it back scaled down, and up, whatever the ratio', () => {
        for (const [screen, client] of [
            [2560, 1920],
            [2560, 1280],
            [3840, 1920],
            [2224, 1668],
            [1920, 1280],
            [1280, 1920],
            [1920, 1080],
        ]) {
            for (const v of values)
                expect(readAt(v, screen, client), `${screen}→${client} ${v}`).toEqual({
                    ok: true,
                    value: v,
                });
        }
    });

    it('reads it back through compression noise and blur', () => {
        for (let seed = 1; seed < 20; seed++)
            expect(readAt(0x9abcdef0, 2560, 1920, { noise: 45, blur: true, seed })).toEqual({
                ok: true,
                value: 0x9abcdef0,
            });
    });

    it('refuses a band caught half-written: two numbers, one check', () => {
        const a = 0x12345678;
        const b = 0x9abcdef0;
        const first = bitsOfValue(a);
        const second = bitsOfValue(b);
        // The left half from one frame, the right half — check included —
        // from the next.
        const mixed = (bit) => (bit < 20 ? first(bit) : second(bit));
        let mixedValue = 0;
        for (let bit = 0; bit < 32; bit++) if (mixed(bit)) mixedValue += 2 ** bit;
        expect(bandCheck(mixedValue)).not.toBe(bandCheck(b));
        const { luma, region } = clientStrip(pageBand(1920, mixed), 1920, 1920);
        expect(decodeBand(luma, region.w, region.h, region.block)).toEqual({
            ok: false,
            why: 'check',
        });
    });

    it('refuses a smeared block, and a strip too small to hold the band', () => {
        const { luma, region } = clientStrip(pageBand(1920, bitsOfValue(5)), 1920, 1920);
        // Grey out block 7's top row.
        for (let y = 0; y < region.block; y++)
            for (let x = Math.floor(7 * region.block); x < Math.floor(8 * region.block); x++)
                luma[y * region.w + x] = 128;
        expect(decodeBand(luma, region.w, region.h, region.block)).toEqual({
            ok: false,
            why: 'block',
        });
        expect(decodeBand(luma, 10, region.h, region.block).ok).toBe(false);
        expect(decodeBand(null, region.w, region.h, region.block).ok).toBe(false);
        // A page that is not there at all — plain text — is refused too.
        const white = new Uint8ClampedArray(region.w * region.h).fill(255);
        expect(decodeBand(white, region.w, region.h, region.block).ok).toBe(false);
    });

    it('puts the 32 bits back on the host clock, across the wrap', () => {
        const hostUs = 5e11 + 123450; // 5.8 days after boot
        const units = Math.floor(hostUs / 10) % 2 ** 32;
        expect(unwrapBand(units, hostUs)).toBe(hostUs - (hostUs % 10));
        // Read 30 ms later, and 5 ms "earlier" (a clock estimate slightly off).
        expect(unwrapBand(units, hostUs + 30000)).toBe(hostUs - (hostUs % 10));
        expect(unwrapBand(units, hostUs - 5000)).toBe(hostUs - (hostUs % 10));
        // Just across a wrap of the 32 bits.
        const wrapUs = 2 ** 32 * 10 * 3;
        const before = wrapUs - 20;
        expect(unwrapBand(Math.floor(before / 10) % 2 ** 32, wrapUs + 40000)).toBe(before);
    });
});

/**
 * Exchanges against a host clock `host(ms)`: sent at t, the host answers
 * after `up` ms, back after `down` more.
 */
function exchange(est, t, host, up, down) {
    return est.note(t, host(t + up), t + up + down);
}

describe('ContentAgeProbe — the clock estimate', () => {
    it('finds a fixed offset through jittery round trips', () => {
        const est = new ClockEstimator();
        const host = (ms) => ms * 1000 + 7.5e9;
        const rand = rng(3);
        for (let t = 0; t < 10000; t += 100) {
            const j = rand() * 4; // up to 4 ms of queueing, one way or the other
            if (rand() < 0.5) exchange(est, t, host, 0.4 + j, 0.4);
            else exchange(est, t, host, 0.4, 0.4 + j);
        }
        expect(est.ready).toBe(true);
        expect(Math.abs(est.toHostUs(10000) - host(10000))).toBeLessThan(200);
    });

    it('follows a drift between the two clocks', () => {
        const est = new ClockEstimator();
        const host = (ms) => ms * 1000 * (1 + 100e-6) + 1e9; // 100 ppm fast
        for (let t = 0; t < 60000; t += 100) exchange(est, t, host, 0.5, 0.5);
        expect(Math.abs(est.toHostUs(60000) - host(60000))).toBeLessThan(300);
        expect(est.summary.driftPpm).toBeGreaterThan(90);
        expect(est.summary.driftPpm).toBeLessThan(110);
    });

    it('is wrong by half the difference on a link slower one way', () => {
        const est = new ClockEstimator();
        const host = (ms) => ms * 1000 + 2e9;
        for (let t = 0; t < 10000; t += 100) exchange(est, t, host, 3, 1);
        // Answered 3 ms after sending, back 1 ms later: the answer is taken
        // for the middle of the trip, 1 ms before it really came, and the
        // estimate runs 1 ms ahead. No two-way exchange can see it.
        const err = est.toHostUs(10000) - host(10000);
        expect(err).toBeGreaterThan(900);
        expect(err).toBeLessThan(1100);
    });

    it('starts over when the host clock jumps, and ignores a lone outlier', () => {
        const est = new ClockEstimator();
        let jump = 0;
        const host = (ms) => ms * 1000 + 3e9 + jump;
        for (let t = 0; t < 5000; t += 100) exchange(est, t, host, 0.5, 0.5);
        // One exchange answered 900 ms late: its offset is off the line.
        expect(est.note(5000, host(5000) + 50000, 5001)).toBe(false);
        expect(est.summary.rejected).toBe(1);
        jump = 50000; // 50 ms
        for (let t = 5100; t < 8000; t += 100) exchange(est, t, host, 0.5, 0.5);
        expect(est.summary.jumps).toBe(1);
        expect(Math.abs(est.toHostUs(8000) - host(8000))).toBeLessThan(200);
    });

    it('refuses what cannot be an exchange', () => {
        const est = new ClockEstimator();
        expect(est.note(10, 1e9, 5)).toBe(false);
        expect(est.note(10, 0, 11)).toBe(false);
        expect(est.note(10, 1e9, 5000)).toBe(false);
        expect(est.ready).toBe(false);
    });
});

/**
 * A decoded frame as VideoFrame shows it to the probe: NV12, its band coded
 * for a host clock of `units()`, copied out asynchronously.
 */
function fakeFrame(timestamp, units, width = 1920, screenW = 2560) {
    return {
        timestamp,
        format: 'NV12',
        codedWidth: width,
        visibleRect: { x: 0, y: 0, width, height: Math.round((width * 9) / 16) },
        clone() {
            const value = units();
            return {
                format: 'NV12',
                allocationSize: ({ rect }) => rect.width * rect.height * 1.5,
                copyTo(buf, { rect }) {
                    const { luma, region } = clientStrip(
                        pageBand(screenW, bitsOfValue(value)),
                        screenW,
                        width,
                    );
                    expect([rect.x, rect.y, rect.width, rect.height]).toEqual([
                        region.x,
                        region.y,
                        region.w,
                        region.h,
                    ]);
                    buf.set(luma.subarray(0, rect.width * rect.height), 0);
                    return Promise.resolve([
                        { offset: 0, stride: rect.width },
                        { offset: rect.width * rect.height, stride: rect.width },
                    ]);
                },
                close() {},
            };
        },
    };
}

/** Let the copied strips come back. */
async function flush() {
    for (let i = 0; i < 4; i++) await Promise.resolve();
}

describe('ContentAgeProbe — the age of what was drawn', () => {
    afterEach(() => {
        vi.useRealTimers();
        vi.restoreAllMocks();
    });

    it('reads the band out of the decoded frame, dates it at its draw, on the host clock', async () => {
        vi.useFakeTimers();
        // The client's clock is the test's: performance.now() stands still
        // unless moved, so the ages are exactly what the frames carry.
        let clientMs = 5000;
        vi.spyOn(performance, 'now').mockImplementation(() => clientMs);
        const offsetUs = 4.2e9; // host steady = client ms * 1000 + offset
        const hostNowUs = () => clientMs * 1000 + offsetUs;
        const renderer = { kind: 'canvas2d', afterDraw: null };
        const pings = [];
        const results = [];
        const probe = new ContentAgeProbe({
            renderer: () => renderer,
            sendPing: (seq, ts) => pings.push({ seq, ts }),
            results,
        });
        probe.start({ every: 1 });
        expect(renderer.afterDraw).toBeTypeOf('function');
        // Ten pings a second; the host answers each 0.5 ms after it was sent.
        for (let i = 0; i < 10; i++) {
            clientMs += 100;
            vi.advanceTimersByTime(100);
            const p = pings[pings.length - 1];
            probe.notePong(
                { type: 'pong', seq: p.seq, ts: p.ts, host: p.ts * 1000 + offsetUs + 500 },
                p.ts + 1,
            );
        }
        expect(pings.length).toBe(10);
        for (let i = 0; i < 20; i++) {
            // The page drew this 8 + i/2 ms ago; the host captured it 3 ms ago.
            const contentAgeUs = 8000 + i * 500;
            const units = () => Math.floor((hostNowUs() - contentAgeUs) / 10) % 2 ** 32;
            const backendTs = Math.floor((hostNowUs() - 3000) / 1000) % 2 ** 32;
            probe.onDecoded(fakeFrame(1000 + i, units), backendTs);
            await flush();
            renderer.afterDraw(renderer, 1000 + i);
        }
        // Decoded, never drawn: not an age.
        probe.onDecoded(
            fakeFrame(5000, () => 1),
            0,
        );
        await flush();
        const s = probe.stop();
        expect(renderer.afterDraw).toBeNull();
        expect(s.ages).toBe(20);
        expect(s.invalid.undrawn).toBe(1);
        expect(s.minMs).toBeCloseTo(8, 0);
        expect(s.maxMs).toBeCloseTo(17.5, 0);
        expect(s.capture.medianMs).toBeGreaterThan(2.4);
        expect(s.capture.medianMs).toBeLessThan(4.1);
        expect(s.beforeCapture.medianMs).toBeGreaterThan(8.5);
        expect(s.beforeCapture.medianMs).toBeLessThan(10.6);
        expect(results).toEqual([s]);
        expect(s.samples.length).toBe(20);
        // Drawn all at one instant here: shown, each is as old as when drawn.
        expect(s.drawsPerSecond).toBeNull();
    });

    it('reads one frame in `every`, counts a band it cannot read, and ages nothing without a clock', async () => {
        const renderer = { kind: 'webgl', afterDraw: null };
        const probe = new ContentAgeProbe({ renderer: () => renderer, sendPing: () => {} });
        probe.start({ every: 3 });
        const grey = (timestamp) => ({
            timestamp,
            format: 'NV12',
            visibleRect: { x: 0, y: 0, width: 1280, height: 720 },
            clone: () => ({
                format: 'NV12',
                allocationSize: ({ rect }) => rect.width * rect.height * 1.5,
                copyTo: (buf, { rect }) => {
                    buf.fill(128);
                    return Promise.resolve([{ offset: 0, stride: rect.width }]);
                },
                close() {},
            }),
        });
        for (let i = 0; i < 9; i++) probe.onDecoded(grey(i), 0);
        await flush();
        for (let i = 0; i < 9; i++) renderer.afterDraw(renderer, i);
        // A readable band, the tenth frame decoded, before the clock is known.
        probe.onDecoded(
            fakeFrame(50, () => 12345),
            0,
        );
        await flush();
        renderer.afterDraw(renderer, 50);
        const s = probe.stop();
        expect(s.decoded).toBe(10);
        expect(s.read).toBe(4);
        expect(s.ages).toBe(0);
        expect(s.invalid.block).toBe(3);
        expect(s.invalid.clock).toBe(1);
    });

    it('takes luma from the first plane of a 4:2:0 frame, and weighs a packed RGB one', () => {
        const nv12 = new Uint8Array([9, 9, 1, 2, 9, 9, 3, 4, 77, 77]);
        expect(Array.from(lumaOf('NV12', nv12, [{ offset: 2, stride: 4 }], 2, 2))).toEqual([
            1, 2, 3, 4,
        ]);
        const bgra = new Uint8Array([0, 0, 255, 255, 255, 255, 255, 255]);
        const l = lumaOf('BGRA', bgra, [{ offset: 0, stride: 8 }], 2, 1);
        expect(l[0]).toBeCloseTo(0.299 * 255, 3);
        expect(l[1]).toBeCloseTo(255, 3);
        expect(lumaOf('P010', bgra, [{ offset: 0, stride: 8 }], 2, 1)).toBeNull();
    });
});

/**
 * A Worker that does what bandReadWorker.js does, a turn later: the copy is
 * its own, never the main thread's.
 */
function fakeWorkerClass(log) {
    return class {
        constructor(url, opts) {
            log.made.push([String(url), opts && opts.type]);
            this.onmessage = null;
            this.onerror = null;
        }
        postMessage(msg, transfer) {
            if (log.refuse)
                throw Object.assign(new Error('not transferable'), { name: 'DataCloneError' });
            log.posted.push({ msg, transfer });
            Promise.resolve().then(async () => {
                const { id, frame, rect } = msg;
                const buf = new Uint8Array(frame.allocationSize({ rect }));
                const layout = await frame.copyTo(buf, { rect });
                frame.close();
                if (log.crash) this.onerror({ type: 'error' });
                else this.onmessage({ data: { id, format: frame.format, layout, buf } });
            });
        }
        terminate() {
            log.terminated = true;
        }
    };
}

/** A frame whose clone counts its copies, to see where they ran. */
function countedFrame(timestamp, value, copies) {
    const f = fakeFrame(timestamp, () => value);
    const clone = f.clone.bind(f);
    f.clone = () => {
        const c = clone();
        const copyTo = c.copyTo.bind(c);
        c.copyTo = (buf, opts) => {
            copies.n++;
            return copyTo(buf, opts);
        };
        return c;
    };
    return f;
}

describe('ContentAgeProbe — the read, off the main thread', () => {
    afterEach(() => {
        vi.unstubAllGlobals();
    });

    it('hands the frame to a worker, which copies the strip out', async () => {
        const log = { made: [], posted: [] };
        vi.stubGlobal('Worker', fakeWorkerClass(log));
        const renderer = { kind: 'canvas2d', afterDraw: null };
        const probe = new ContentAgeProbe({ renderer: () => renderer, sendPing: () => {} });
        probe.start({ every: 1 });
        const copies = { n: 0 };
        for (let i = 0; i < 3; i++) {
            probe.onDecoded(countedFrame(i, 4242, copies), 0);
            // Nothing copied while the frame is still on its way to its draw.
            expect(copies.n).toBe(0);
            renderer.afterDraw(renderer, i);
        }
        await flush();
        await flush();
        const s = probe.stop();
        expect(log.made).toEqual([[expect.stringContaining('bandReadWorker.js'), 'module']]);
        expect(log.posted).toHaveLength(3);
        // The clone is transferred, not copied.
        expect(log.posted[0].transfer).toEqual([log.posted[0].msg.frame]);
        expect(copies.n).toBe(3);
        // Read, and refused an age only for want of a clock: the band was good.
        expect(s.reader).toBe('worker');
        expect(s.invalid.clock).toBe(3);
        expect(s.invalid.block + s.invalid.copy).toBe(0);
        expect(s.readCost.n).toBe(3);
    });

    it('copies on the main thread where a frame cannot be transferred', async () => {
        const log = { made: [], posted: [], refuse: true };
        vi.stubGlobal('Worker', fakeWorkerClass(log));
        const renderer = { kind: 'canvas2d', afterDraw: null };
        const probe = new ContentAgeProbe({ renderer: () => renderer, sendPing: () => {} });
        probe.start({ every: 1 });
        const copies = { n: 0 };
        probe.onDecoded(countedFrame(1, 4242, copies), 0);
        expect(copies.n).toBe(1);
        renderer.afterDraw(renderer, 1);
        probe.onDecoded(countedFrame(2, 4242, copies), 0);
        renderer.afterDraw(renderer, 2);
        await flush();
        const s = probe.stop();
        expect(log.made).toHaveLength(1);
        expect(s.reader).toBe('inline');
        expect(s.invalid.clock).toBe(2);
    });

    it('counts the reads a dying worker took with it, and goes on inline', async () => {
        const log = { made: [], posted: [], crash: true };
        vi.stubGlobal('Worker', fakeWorkerClass(log));
        const renderer = { kind: 'canvas2d', afterDraw: null };
        const probe = new ContentAgeProbe({ renderer: () => renderer, sendPing: () => {} });
        probe.start({ every: 1 });
        const copies = { n: 0 };
        probe.onDecoded(countedFrame(1, 4242, copies), 0);
        renderer.afterDraw(renderer, 1);
        await flush();
        await flush();
        expect(log.terminated).toBe(true);
        probe.onDecoded(countedFrame(2, 4242, copies), 0);
        renderer.afterDraw(renderer, 2);
        await flush();
        const s = probe.stop();
        expect(s.invalid.copy).toBe(1);
        expect(s.invalid.clock).toBe(1);
        expect(s.reader).toBe('inline');
    });

    it('reads on the main thread when asked to', async () => {
        const log = { made: [], posted: [] };
        vi.stubGlobal('Worker', fakeWorkerClass(log));
        const renderer = { kind: 'canvas2d', afterDraw: null };
        const probe = new ContentAgeProbe({
            renderer: () => renderer,
            sendPing: () => {},
            worker: false,
        });
        probe.start({ every: 1 });
        probe.onDecoded(countedFrame(1, 4242, { n: 0 }), 0);
        renderer.afterDraw(renderer, 1);
        await flush();
        expect(log.made).toHaveLength(0);
        expect(probe.stop().reader).toBe('inline');
    });
});

describe('ContentAgeProbe — the age of what is shown', () => {
    it('ages the frame on screen until the next one replaces it', () => {
        // Three frames drawn 10 ms apart, each 5 ms old when drawn.
        const draws = [
            [0, 1],
            [10, 2],
            [20, 3],
        ];
        const ageOf = new Map([
            [1, 5],
            [2, 5],
            [3, 5],
        ]);
        const grid = [];
        for (let t = 0; t <= 20; t += 0.5) grid.push(t);
        const shown = shownAges(draws, ageOf, grid);
        expect(shown[0]).toBe(5);
        expect(shown[19]).toBe(14.5); // 9.5 ms after the first draw
        expect(shown[20]).toBe(5); // the second one, just drawn
        const mean = shown.reduce((a, b) => a + b, 0) / shown.length;
        expect(mean).toBeGreaterThan(9.5);
        expect(mean).toBeLessThan(10.5);
    });

    it('leaves out a moment before the first draw, or whose frame was not read', () => {
        const draws = [
            [10, 1],
            [20, 2],
        ];
        const ageOf = new Map([[2, 3]]);
        expect(shownAges(draws, ageOf, [5, 15, 25])).toEqual([8]);
    });
});

describe('ContentAgeProbe — what the refreshes showed', () => {
    const ticks = (n, period, from = 0) => Array.from({ length: n }, (_, k) => from + k * period);

    it('one frame per refresh: nothing repeated, nothing unseen', () => {
        const draws = ticks(60, 8, 3).map((t, k) => [t, k]);
        expect(refreshCounts(draws, ticks(60, 8))).toEqual({
            repeats: 0,
            unseen: 0,
            refreshes: 58, // from the refresh after the one that showed the first frame
        });
    });

    it('a frame missing: one refresh shows the picture again', () => {
        const draws = ticks(60, 8, 3)
            .filter((_, k) => k !== 30)
            .map((t, k) => [t, k]);
        expect(refreshCounts(draws, ticks(60, 8)).repeats).toBe(1);
    });

    it('two frames a refresh: one of each pair is never at a refresh', () => {
        const draws = ticks(120, 4, 1).map((t, k) => [t, k]);
        const seen = refreshCounts(draws, ticks(60, 8));
        expect(seen.repeats).toBe(0);
        expect(seen.unseen).toBe(58);
    });

    it('counts from the first draw on', () => {
        const draws = [
            [50, 1],
            [58, 2],
        ];
        expect(refreshCounts(draws, ticks(10, 8))).toEqual({
            repeats: 1,
            unseen: 0,
            refreshes: 2,
        });
    });
});
