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
 * DecodeQueueSignal — when the host is told the decoder is full, when it is
 * told it is clear, and when it is told nothing.
 */
import { describe, it, expect } from 'vitest';
import { DecodeQueueSignal, REFRESH_MS } from '../js/stream/DecodeQueueSignal.js';

describe('DecodeQueueSignal', () => {
    it('says nothing while the queue stays at one frame or none', () => {
        const s = new DecodeQueueSignal();
        const sent = [];
        for (let t = 0; t < 2000; t += 4) {
            const m = s.observe(t % 8 === 0 ? 1 : 0, t);
            if (m) sent.push(m);
        }
        expect(sent).toEqual([]);
        expect(s.full).toBe(false);
    });

    it('says "full" at the second frame waiting, and "clear" back at one', () => {
        const s = new DecodeQueueSignal();
        expect(s.observe(1, 0)).toBeNull();
        expect(s.observe(2, 4)).toEqual({ type: 'decodequeue', depth: 2 });
        expect(s.full).toBe(true);
        // Deeper, soon after: already said.
        expect(s.observe(3, 8)).toBeNull();
        expect(s.observe(1, 12)).toEqual({ type: 'decodequeue', depth: 1 });
        expect(s.full).toBe(false);
        // Clear once, not again.
        expect(s.observe(0, 16)).toBeNull();
        expect(s.observe(1, 20)).toBeNull();
    });

    it('says a queue that stays full again, no more often than REFRESH_MS', () => {
        const s = new DecodeQueueSignal();
        const sent = [];
        for (let t = 0; t <= 200; t += 2) {
            const m = s.observe(3, t);
            if (m) sent.push(t);
        }
        expect(sent[0]).toBe(0);
        for (let i = 1; i < sent.length; i++)
            expect(sent[i] - sent[i - 1]).toBeGreaterThanOrEqual(REFRESH_MS);
        expect(sent.length).toBe(Math.floor(200 / REFRESH_MS) + 1);
    });

    it('takes a flushed decoder as clear', () => {
        const s = new DecodeQueueSignal();
        s.observe(5, 0);
        expect(s.observe(0, 1)).toEqual({ type: 'decodequeue', depth: 0 });
    });

    it('reads a junk depth as an empty queue', () => {
        const s = new DecodeQueueSignal();
        expect(s.observe(undefined, 0)).toBeNull();
        expect(s.observe(-3, 1)).toBeNull();
        expect(s.observe(NaN, 2)).toBeNull();
    });
});
