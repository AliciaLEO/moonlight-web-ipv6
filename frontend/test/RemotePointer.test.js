/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect } from 'vitest';
import {
    RemotePointer,
    POINTER_MIN_SPEED,
    POINTER_MAX_SPEED,
    POINTER_RAMP_MS,
    POINTER_NUDGE,
} from '../js/stream/RemotePointer.js';

/**
 * A TV remote's arrows steering the host's mouse: a small step at the press,
 * then every frame, faster the longer the arrow is held.
 */

/** A pointer on a fake clock and a hand-cranked frame loop. */
function rig() {
    const moves = [];
    let t = 0;
    let pending = null;
    const p = new RemotePointer({
        move: (dx, dy) => moves.push([dx, dy]),
        now: () => t,
        schedule: (cb) => (pending = cb),
        cancel: () => (pending = null),
    });
    const frames = (n, ms = 16) => {
        for (let i = 0; i < n && pending; i++) {
            t += ms;
            const cb = pending;
            pending = null;
            cb();
        }
    };
    const total = () => moves.reduce((a, [dx, dy]) => [a[0] + dx, a[1] + dy], [0, 0]);
    return { p, moves, frames, total, running: () => pending !== null };
}

describe('RemotePointer', () => {
    it('a press steps at once, a quick release stops there', () => {
        const r = rig();
        r.p.press('right');
        expect(r.moves).toEqual([[POINTER_NUDGE, 0]]);
        r.p.release('right');
        expect(r.running()).toBe(false);
        r.frames(5);
        expect(r.moves).toHaveLength(1);
    });

    it('held, it moves every frame, slow at first', () => {
        const r = rig();
        r.p.press('down');
        r.frames(10);
        const [, y] = r.total();
        // ~160 ms at the slow speed, plus the step.
        expect(y).toBeGreaterThan(POINTER_NUDGE + POINTER_MIN_SPEED * 0.16 * 0.8);
        expect(y).toBeLessThan(POINTER_NUDGE + POINTER_MIN_SPEED * 0.16 * 2);
    });

    it('speeds up to the fast speed after the ramp', () => {
        const r = rig();
        r.p.press('left');
        r.frames(Math.ceil(POINTER_RAMP_MS / 16) + 2);
        const before = r.total()[0];
        r.frames(10);
        const perFrame = (before - r.total()[0]) / 10;
        expect(perFrame).toBeCloseTo((POINTER_MAX_SPEED * 16) / 1000, 0);
    });

    it('two arrows go diagonally, at the same speed', () => {
        const r = rig();
        r.p.press('right');
        r.p.press('down');
        r.moves.length = 0;
        r.frames(20);
        const [x, y] = r.total();
        expect(x).toBeGreaterThan(0);
        expect(Math.abs(x - y)).toBeLessThanOrEqual(1);
        const straight = rig();
        straight.p.press('right');
        straight.moves.length = 0;
        straight.frames(20);
        expect(Math.hypot(x, y)).toBeCloseTo(straight.total()[0], -1);
    });

    it('a repeat of a held arrow changes nothing', () => {
        const r = rig();
        r.p.press('up');
        r.p.press('up');
        expect(r.moves).toHaveLength(1);
    });

    it('releaseAll stops everything', () => {
        const r = rig();
        r.p.press('up');
        r.p.press('left');
        r.p.releaseAll();
        expect(r.p.moving).toBe(false);
        expect(r.running()).toBe(false);
    });

    it('a late frame moves at most 50 ms worth', () => {
        const r = rig();
        r.p.press('right');
        r.moves.length = 0;
        r.frames(1, 2000);
        expect(r.total()[0]).toBeLessThanOrEqual(Math.ceil((POINTER_MAX_SPEED * 50) / 1000));
    });
});
