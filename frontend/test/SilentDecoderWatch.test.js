/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect } from 'vitest';
import { SilentDecoderWatch } from '../js/stream/SilentDecoderWatch.js';

/**
 * A decoder that takes every chunk and gives nothing back says nothing about
 * it: the watch reads it off the two counters, received and decoded.
 */
describe('SilentDecoderWatch', () => {
    /** Drive the watch on a 500 ms tick, `rate` chunks a second, none decoded. */
    function silentFor(watch, ms, rate = 50, start = { rx: 100, dec: 40, t: 0 }) {
        let hit = null;
        for (let t = 0; t <= ms; t += 500) {
            const r = watch.observe(
                start.rx + Math.round((t / 1000) * rate),
                start.dec,
                start.t + t,
            );
            if (r) hit = hit || { ...r, at: t };
        }
        return hit;
    }

    it('answers once the decoder has given nothing back for long enough while chunks went in', () => {
        const hit = silentFor(new SilentDecoderWatch(), 3000);
        expect(hit).not.toBe(null);
        expect(hit.at).toBe(1500);
        expect(hit.chunks).toBe(75);
        expect(hit.ms).toBe(1500);
    });

    it('gives a still host its time: few chunks are not a silence yet', () => {
        // Two frames a second: 30 chunks take 15 s, and only then does it count.
        const hit = silentFor(new SilentDecoderWatch(), 20000, 2);
        expect(hit.at).toBe(15000);
    });

    it('does not mistake a short hiccup for a decoder gone', () => {
        const w = new SilentDecoderWatch();
        expect(w.observe(0, 0, 0)).toBe(null);
        expect(w.observe(40, 0, 1000)).toBe(null);
        // A picture at 1.2 s: the count starts over from there.
        expect(w.observe(60, 1, 1200)).toBe(null);
        expect(w.observe(100, 1, 2500)).toBe(null);
        expect(w.observe(140, 1, 2800)).not.toBe(null);
    });

    it('answers once per stretch, and again only after as much again', () => {
        const w = new SilentDecoderWatch();
        w.observe(0, 5, 0);
        expect(w.observe(80, 5, 1600)).not.toBe(null);
        expect(w.observe(90, 5, 1800)).toBe(null);
        expect(w.observe(170, 5, 3200)).not.toBe(null);
    });

    it('starts over when the counters do (a new decoder), or when told to', () => {
        const w = new SilentDecoderWatch();
        w.observe(500, 200, 0);
        expect(w.observe(10, 200, 1600)).toBe(null);
        expect(w.observe(90, 200, 3200)).not.toBe(null);
        w.reset();
        expect(w.observe(200, 200, 3300)).toBe(null);
        expect(w.observe(240, 200, 4000)).toBe(null);
    });
});
