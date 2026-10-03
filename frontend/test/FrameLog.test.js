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
 * FrameLog — each drawn frame's age on the host's clock, from the stamp the
 * host put on it to the end of its draw, and the per-frame log a bench fetches.
 */
import { describe, it, expect } from 'vitest';
import { FRAME_LOG_COLUMNS, FrameLog } from '../js/stream/FrameLog.js';

const WRAP = 2 ** 32;

/** The host's steady clock (µs) at this client's @p ms: 5 000 s ahead. */
const fixedHost = (ms) => ms * 1000 + 5e12;

/** One ping sent at @p t, answered @p up ms later, back @p down ms after that. */
function pong(log, t, host, up, down) {
    log.notePong({ type: 'pong', ts: t, host: host(t + up) }, t + up + down);
}

/** The 32-bit ms stamp the host puts on a frame it presents at client time @p ms. */
const stampAt = (host, ms) => Math.floor(host(ms) / 1000) % WRAP;

/** Pongs every 2 s, as StreamView pings, from @p from to @p to. */
function pings(log, host, from, to, up = 0.8, down = 0.8) {
    for (let t = from; t <= to; t += 2000) pong(log, t, host, up, down);
}

describe('FrameLog — the age of each drawn frame', () => {
    it('ages a frame from its stamp to the end of its draw, on the host clock', () => {
        const log = new FrameLog();
        pings(log, fixedHost, 0, 10000);
        // Presented on the host at 10 000 ms of this client's clock, drawn 30 ms later.
        const e2e = log.noteDrawn({
            backendTs: stampAt(fixedHost, 10000),
            drawnMs: 10030,
            arrivedMs: 10012,
            decodedMs: 10020,
            drawStartMs: 10027,
            bytes: 41000,
            key: false,
        });
        // The stamp is whole milliseconds, taken down: up to one more.
        expect(e2e).toBeGreaterThanOrEqual(29.9);
        expect(e2e).toBeLessThan(31.1);
        const [row] = log.rows();
        expect(Number.isInteger(row.hostMs)).toBe(true);
        expect(row.captureMs).toBeCloseTo(10030 - e2e, 6);
        expect(row.arrivedMs).toBe(10012);
        expect(row.bytes).toBe(41000);
        expect(row.key).toBe(0);
        expect(log.measured).toBe(1);
    });

    it('logs the frame but gives no age before the clock is known', () => {
        const log = new FrameLog();
        pong(log, 0, fixedHost, 1, 1);
        const e2e = log.noteDrawn({ backendTs: stampAt(fixedHost, 100), drawnMs: 120 });
        expect(e2e).toBeNaN();
        expect(log.length).toBe(1);
        expect(log.measured).toBe(0);
        const line = log.csv().split('\n')[1].split(',');
        expect(line[FRAME_LOG_COLUMNS.indexOf('e2eMs')]).toBe('');
        expect(line[FRAME_LOG_COLUMNS.indexOf('drawnMs')]).toBe('120.000');
    });

    it('follows a drift with pings every 2 s, through a jittery link', () => {
        const log = new FrameLog();
        // 50 ppm fast: 9 ms of offset over the three minutes.
        const host = (ms) => ms * 1000 * (1 + 50e-6) + 3e12;
        let seed = 7;
        const rand = () => (seed = (seed * 16807) % 2147483647) / 2147483647;
        for (let t = 0; t <= 180000; t += 2000) {
            // One pong in three queued behind a frame, one way or the other.
            const j = rand() < 0.33 ? 2 + rand() * 20 : 0;
            if (rand() < 0.5) pong(log, t, host, 0.8 + j, 0.8);
            else pong(log, t, host, 0.8, 0.8 + j);
        }
        const e2e = log.noteDrawn({ backendTs: stampAt(host, 180000), drawnMs: 180025 });
        expect(e2e).toBeGreaterThan(24.5);
        expect(e2e).toBeLessThan(26.5);
        expect(log.summary().clock.driftPpm).toBeGreaterThan(40);
        expect(log.summary().clock.driftPpm).toBeLessThan(60);
    });

    it('puts a stamp back across the 32-bit wrap', () => {
        const log = new FrameLog();
        // The host's ms clock crosses 2^32 a few ms after the frame below.
        const host = (ms) => (ms - 10010) * 1000 + WRAP * 1000;
        pings(log, host, 0, 10000);
        const stamp = stampAt(host, 10000);
        expect(stamp).toBeGreaterThan(WRAP - 20);
        const e2e = log.noteDrawn({ backendTs: stamp, drawnMs: 10020 });
        expect(e2e).toBeGreaterThanOrEqual(19.9);
        expect(e2e).toBeLessThan(21.1);
        // Drawn after the wrap, stamped before it.
        expect(log.rows()[0].hostMs).toBeLessThan(WRAP);
        expect(log.noteDrawn({ backendTs: stampAt(host, 10015), drawnMs: 10040 })).toBeLessThan(
            26.1,
        );
    });

    it('refuses a frame with no stamp, and an age no link could give', () => {
        const log = new FrameLog();
        pings(log, fixedHost, 0, 10000);
        expect(log.noteDrawn({ backendTs: 0, drawnMs: 10030 })).toBeNaN();
        expect(log.length).toBe(0);
        // A stamp from another clock: logged, never aged.
        expect(log.noteDrawn({ backendTs: 12345, drawnMs: 10030 })).toBeNaN();
        expect(log.length).toBe(1);
        expect(log.measured).toBe(0);
    });
});

describe('FrameLog — the log', () => {
    it('keeps the last frames of a full ring, oldest first, and writes them as CSV', () => {
        const log = new FrameLog({ frames: 4 });
        pings(log, fixedHost, 0, 10000);
        for (let k = 0; k < 6; k++)
            log.noteDrawn({
                backendTs: stampAt(fixedHost, 10000 + k * 10),
                drawnMs: 10020 + k * 10,
                bytes: 1000 + k,
                key: k === 0,
            });
        expect(log.length).toBe(4);
        expect(log.frames).toBe(6);
        expect(log.rows().map((r) => r.drawnMs)).toEqual([10040, 10050, 10060, 10070]);
        const lines = log.csv().split('\n');
        expect(lines[0]).toBe(FRAME_LOG_COLUMNS.join(','));
        expect(lines).toHaveLength(5);
        const last = lines[4].split(',');
        expect(last[FRAME_LOG_COLUMNS.indexOf('bytes')]).toBe('1005');
        expect(last[FRAME_LOG_COLUMNS.indexOf('arrivedMs')]).toBe('');
        log.clear();
        expect(log.rows()).toEqual([]);
        expect(log.csv()).toBe(FRAME_LOG_COLUMNS.join(','));
    });

    it('sums up the frames it holds', () => {
        const log = new FrameLog();
        pings(log, fixedHost, 0, 10000);
        for (let k = 0; k < 100; k++)
            log.noteDrawn({
                backendTs: stampAt(fixedHost, 10000 + k * 10),
                drawnMs: 10020 + k * 10 + (k === 99 ? 80 : 0),
            });
        const s = log.summary();
        expect(s.frames).toBe(100);
        expect(s.measured).toBe(100);
        expect(s.medianMs).toBeGreaterThanOrEqual(19.9);
        expect(s.medianMs).toBeLessThan(21.1);
        expect(s.p99Ms).toBeGreaterThan(99);
        expect(s.clock.samples).toBeGreaterThanOrEqual(3);
    });
});
