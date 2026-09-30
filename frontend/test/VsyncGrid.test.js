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
 * VsyncGrid — the refresh grid fitted from requestAnimationFrame, put on the
 * host's clock, the lead from capture → ready, and the margin the misses set.
 */
import { describe, it, expect, vi, afterEach } from 'vitest';
import {
    fitGrid,
    VsyncGrid,
    MARGIN_CALM_MS,
    MARGIN_DOWN_MS,
    MARGIN_FLOOR_MS,
    MARGIN_UP_MS,
    SEND_EVERY_MS,
    STEADY_FOR_MS,
    TRANSIT_WINDOW_MS,
} from '../js/stream/VsyncGrid.js';

/** Deterministic noise. */
function rng(seed) {
    let s = seed >>> 0;
    return () => {
        s = (s * 1664525 + 1013904223) >>> 0;
        return s / 2 ** 32;
    };
}

const PERIOD = 1000 / 120;
const OFFSET_US = 5e9; // the host's clock, ahead of this one
const hostUs = (ms) => ms * 1000 + OFFSET_US;

describe('fitGrid', () => {
    it('finds the period and the latest refresh under timestamp jitter', () => {
        const rand = rng(1);
        const ticks = [];
        for (let k = 0; k < 240; k++) ticks.push(1000 + k * PERIOD + (rand() - 0.5) * 0.1);
        const fit = fitGrid(ticks);
        expect(fit.periodMs).toBeCloseTo(PERIOD, 2);
        expect(Math.abs(fit.phaseMs - (1000 + 239 * PERIOD))).toBeLessThan(0.05);
    });

    it('counts the refreshes the browser skipped as whole periods', () => {
        const ticks = [];
        for (let k = 0; k < 240; k++) if (k % 7 !== 3) ticks.push(k * PERIOD);
        const fit = fitGrid(ticks);
        expect(fit.periodMs).toBeCloseTo(PERIOD, 6);
        expect(fit.phaseMs).toBeCloseTo(239 * PERIOD, 6);
    });

    it('leaves a callback that ran late out of the fit', () => {
        const ticks = [];
        for (let k = 0; k < 120; k++) ticks.push(k * PERIOD + (k === 60 ? 3 : 0));
        const fit = fitGrid(ticks);
        expect(fit.periodMs).toBeCloseTo(PERIOD, 6);
        expect(fit.phaseMs).toBeCloseTo(119 * PERIOD, 6);
    });

    it('needs enough refreshes', () => {
        expect(fitGrid([0, PERIOD, 2 * PERIOD])).toBeNull();
    });
});

/**
 * A grid wired to a fake host whose display refreshes at 240 Hz: pongs
 * answered after 0.5 ms each way, frames ready @p transitMs (plus @p jitter)
 * after their capture. Without a deadline the host captures at any phase;
 * with one it captures, for each refresh, at the last of its own refreshes
 * that the last grid's lead allows.
 */
function setup({ transitMs = 10, jitter = () => 0, seed = 7 } = {}) {
    vi.useFakeTimers();
    const rand = rng(seed);
    const HOST_TICK_US = 1e6 / 240;
    let now = 0;
    const sent = [];
    const pending = [];
    const grid = new VsyncGrid({
        send: (msg) => sent.push(msg),
        sendPing: () => {},
        now: () => now,
        onFrame: () => {},
    });
    grid.start();
    const pong = (deadline) => {
        const msg = { type: 'pong', ts: now, host: hostUs(now + 0.5) };
        if (deadline) msg.deadline = { presentUs: HOST_TICK_US, aimed: deadline !== 'tearing' };
        grid.notePong(msg, now + 1);
    };
    /** Refreshes up to @p untilMs, a pong every 100 ms, a frame per refresh. */
    const run = (untilMs, deadline = false) => {
        for (let k = Math.floor(now / PERIOD) + 1; k * PERIOD <= untilMs; k++) {
            now = k * PERIOD;
            if (Math.floor(now / 100) !== Math.floor((now - PERIOD) / 100)) pong(deadline);
            pending.sort((a, b) => a.ready - b.ready);
            while (pending.length && pending[0].ready <= now) {
                const f = pending.shift();
                grid.noteReady(f.stamp, f.ready);
            }
            grid.noteRefresh(now);
            // The host's frame for the refresh four ahead.
            const target = (k + 4) * PERIOD;
            let captureUs;
            if (deadline && sent.length) {
                const s = sent[sent.length - 1];
                const n = Math.round((hostUs(target) - s.phaseUs) / s.periodUs);
                const refreshUs = s.phaseUs + n * s.periodUs;
                captureUs = Math.floor((refreshUs - s.leadUs) / HOST_TICK_US) * HOST_TICK_US;
            } else {
                // Any phase, on a whole host millisecond: the 32-bit stamp is exact.
                captureUs =
                    Math.floor(
                        (hostUs(target) - transitMs * 1000 - rand() * PERIOD * 1000) / 1000,
                    ) * 1000;
            }
            const ready = (captureUs - OFFSET_US) / 1000 + transitMs + jitter();
            pending.push({ stamp: Math.floor(captureUs / 1000) % 2 ** 32, ready });
        }
    };
    return {
        grid,
        sent,
        run,
        pong,
        at: (ms) => {
            now = ms;
        },
    };
}

describe('VsyncGrid', () => {
    afterEach(() => vi.useRealTimers());

    it('says nothing until it knows the host clock and a lead', () => {
        const { grid, sent } = setup();
        for (let k = 0; k < 100; k++) grid.noteRefresh(k * PERIOD);
        expect(sent).toHaveLength(0);
        grid.stop();
    });

    it('sends the grid on the host clock, the lead from capture → ready', () => {
        const { grid, sent, run } = setup();
        run(3000);
        expect(sent.length).toBeGreaterThan(2);
        const last = sent[sent.length - 1];
        expect(last.type).toBe('vsyncgrid');
        expect(last.periodUs).toBeCloseTo(PERIOD * 1000, 0);
        // The phase is a refresh, on the host's clock.
        const phaseClientMs = (last.phaseUs - OFFSET_US) / 1000;
        const k = Math.round(phaseClientMs / PERIOD);
        expect(Math.abs(phaseClientMs - k * PERIOD)).toBeLessThan(0.05);
        // 10 ms of transit plus the first margin, a quarter period.
        expect(last.leadUs / 1000).toBeCloseTo(10 + PERIOD / 4, 1);
        // No more than one every SEND_EVERY_MS.
        expect(sent.length).toBeLessThanOrEqual(Math.ceil(3000 / SEND_EVERY_MS));
        grid.stop();
    });

    it('says whether the canvas tears, and how many frames it takes', () => {
        vi.useFakeTimers();
        const sent = [];
        let now = 0;
        const grid = new VsyncGrid({
            send: (msg) => sent.push(msg),
            sendPing: () => {},
            now: () => now,
            onFrame: () => {},
            tearing: () => true,
            budgetFactor: 2,
        });
        grid.start();
        for (let k = 0; k * PERIOD < 3000; k++) {
            now = k * PERIOD;
            if (k % 12 === 0)
                grid.notePong({ type: 'pong', ts: now, host: hostUs(now + 0.5) }, now + 1);
            grid.noteRefresh(now);
            const captureMs = Math.floor(hostUs(now - 10) / 1000);
            grid.noteReady(captureMs % 2 ** 32, (captureMs * 1000 - OFFSET_US) / 1000 + 10);
        }
        const last = sent[sent.length - 1];
        expect(last.tearing).toBe(true);
        expect(last.budgetFps).toBeCloseTo(240, 1);
        grid.stop();
    });

    it('counts no miss while the host sends frames as they come', () => {
        const { grid, run } = setup();
        run(3000, 'tearing');
        expect(grid.followed).toBe(true);
        expect(grid.aimed).toBe(false);
        expect(grid.frames).toBe(0);
        grid.stop();
    });

    it('counts no miss while the host is not following', () => {
        const { grid, run } = setup();
        run(3000);
        expect(grid.followed).toBe(false);
        expect(grid.frames).toBe(0);
        expect(grid.misses).toBe(0);
        grid.stop();
    });

    /**
     * A frame captured for the refresh @p ahead refreshes after the last
     * grid's phase, ready @p lateMs after it (negative: before).
     */
    function aimed(grid, sent, ahead, lateMs) {
        const s = sent[sent.length - 1];
        const refreshUs = s.phaseUs + ahead * s.periodUs;
        const captureMs = Math.floor((refreshUs - s.leadUs - 1000) / 1000);
        const refreshClientMs = (refreshUs - OFFSET_US) / 1000;
        grid.noteReady(captureMs % 2 ** 32, refreshClientMs + lateMs);
    }

    it('widens the margin on a miss and narrows it after a calm spell', () => {
        const { grid, sent, run, pong, at } = setup();
        run(3000, true);
        expect(grid.followed).toBe(true);
        const start = grid.marginMs;
        expect(start).toBeCloseTo(PERIOD / 4, 6);
        const missesBefore = grid.misses;
        at(3001);
        pong(true);
        aimed(grid, sent, 2, 0.4);
        expect(grid.misses).toBe(missesBefore + 1);
        expect(grid.marginMs).toBeCloseTo(start + MARGIN_UP_MS, 6);
        // On time for longer than the calm spell: one step back.
        // Under a millisecond to spare: the step is the smallest one.
        aimed(grid, sent, 3, -0.9);
        const calmReady = (sent[sent.length - 1].phaseUs - OFFSET_US) / 1000 + MARGIN_CALM_MS + 50;
        at(calmReady - 10);
        pong(true);
        const s = sent[sent.length - 1];
        const ahead = Math.ceil(((calmReady - (s.phaseUs - OFFSET_US) / 1000) * 1000) / s.periodUs);
        aimed(grid, sent, ahead, -0.9);
        expect(grid.marginMs).toBeCloseTo(start + MARGIN_UP_MS - MARGIN_DOWN_MS, 6);
        grid.stop();
    });

    it('narrows faster when the tightest frames had time to spare', () => {
        const { grid, sent, run } = setup();
        run(3000, true);
        const start = grid.marginMs;
        const s = sent[sent.length - 1];
        const ahead = Math.ceil(((MARGIN_CALM_MS + 10) * 1000) / s.periodUs);
        // 2.5 ms to spare at worst: half of what lies past the floor goes.
        aimed(grid, sent, ahead, -2.5);
        expect(grid.marginMs).toBeCloseTo(
            Math.max(MARGIN_FLOOR_MS, start - (2.5 - MARGIN_FLOOR_MS) / 2),
            3,
        );
        grid.stop();
    });

    it('never narrows the margin under its floor', () => {
        const { grid, sent, run } = setup();
        run(3000, true);
        for (let i = 1; i <= 200; i++) {
            const s = sent[sent.length - 1];
            const ahead = Math.ceil((i * (MARGIN_CALM_MS + 10) * 1000) / s.periodUs);
            aimed(grid, sent, ahead, -1);
        }
        expect(grid.marginMs).toBe(MARGIN_FLOOR_MS);
        grid.stop();
    });

    it('stays within half a percent of misses on a jittery link', () => {
        const rand = rng(11);
        // Up to 4 ms more on the way, rarely: a long tail past the p95.
        const { grid, run } = setup({ jitter: () => rand() ** 8 * 4 });
        run(20000, true);
        const { missRate, followed, frames } = grid.summary;
        expect(followed).toBe(true);
        expect(frames).toBeGreaterThan(2000);
        expect(missRate).toBeLessThanOrEqual(0.005);
        // A 4 ms tail at 120 Hz is still steady enough to aim through.
        expect(grid.steady).toBe(true);
        grid.stop();
    });

    it('says the link is uneven when capture → ready wanders by over half a refresh', () => {
        const rand = rng(5);
        // Anywhere within 20 ms more: an N95 on Wi-Fi.
        const { grid, sent, run } = setup({ jitter: () => rand() * 20 });
        run(4000, true);
        expect(grid.steady).toBe(false);
        expect(grid.unsteadySpells).toBe(1);
        expect(sent[sent.length - 1].steady).toBe(false);
        expect(grid.summary.spreadMs).toBeGreaterThan(PERIOD / 2);
        grid.stop();
    });

    it('aims again once the link has held steady for a while', () => {
        const rand = rng(9);
        let wide = true;
        const { grid, sent, run } = setup({ jitter: () => (wide ? rand() * 20 : rand() * 0.5) });
        run(4000, true);
        expect(grid.steady).toBe(false);
        wide = false;
        // The wide samples leave the window, then the calm has to last.
        run(4000 + TRANSIT_WINDOW_MS + STEADY_FOR_MS / 2, true);
        expect(grid.steady).toBe(false);
        run(4000 + TRANSIT_WINDOW_MS + STEADY_FOR_MS + 2 * SEND_EVERY_MS, true);
        expect(grid.steady).toBe(true);
        expect(sent[sent.length - 1].steady).toBe(true);
        // The margin started afresh from the calm link: its tail and a
        // quarter period.
        expect(grid.marginMs).toBeLessThan(PERIOD / 4 + 1);
        grid.stop();
    });

    it('says the link is uneven once the margin has grown to a whole refresh', () => {
        const { grid, sent, run } = setup();
        run(3000, true);
        expect(grid.steady).toBe(true);
        for (let i = 0; i < 10; i++) aimed(grid, sent, 2 + i, 0.5);
        expect(grid.marginMs).toBeCloseTo(PERIOD, 3);
        run(3000 + 2 * SEND_EVERY_MS, true);
        expect(grid.steady).toBe(false);
        grid.stop();
    });
});
