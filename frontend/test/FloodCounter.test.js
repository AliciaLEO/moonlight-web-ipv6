/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

import { describe, expect, it } from 'vitest';
import { FLOOD_MAGIC, FloodCounter, floodChannelInit, floodMode } from '../js/api/FloodCounter.js';

function message(seq, sendUs, bytes = 1100) {
    const buf = new ArrayBuffer(bytes);
    const view = new DataView(buf);
    view.setUint32(0, FLOOD_MAGIC);
    view.setUint32(4, seq);
    view.setBigUint64(8, BigInt(sendUs));
    return buf;
}

describe('FloodCounter (bench, plan Idées Punktfunk A0)', () => {
    it('is off unless localStorage asks, and matches the host channel', () => {
        expect(floodMode({ getItem: () => null })).toBe(null);
        expect(floodMode({ getItem: () => '1' })).toBe('fec');
        expect(floodMode({ getItem: () => 'video' })).toBe('video');
        expect(
            floodMode({
                getItem: () => {
                    throw new Error('blocked');
                },
            }),
        ).toBe(null);
        expect(floodChannelInit('fec')).toEqual({
            negotiated: true,
            id: 3,
            ordered: false,
            maxRetransmits: 0,
        });
        expect(floodChannelInit('video')).toMatchObject({ id: 3, ordered: true });
    });

    it('counts throughput, losses and late messages', () => {
        const c = new FloodCounter();
        // seq 0, 1, 2, then 5 (3 and 4 missing), then 4 late.
        c.onMessage(message(0, 1_000_000), 1000.0);
        c.onMessage(message(1, 1_001_000), 1001.0);
        c.onMessage(message(2, 1_002_000), 1002.5); // 0.5 ms later than the best
        c.onMessage(message(5, 1_005_000), 1005.0);
        c.onMessage(message(4, 1_004_000), 1006.0); // late: 2 ms over the best
        const w = c.closeWindow(1);
        expect(w.msgsPerSec).toBe(5);
        expect(w.kbps).toBe(44); // 5 × 1100 bytes
        expect(w.lost).toBe(1); // 3 only: 4 came back
        expect(w.reordered).toBe(1);
        expect(w.extraDelayMs.max).toBe(2);
        expect(c.totals).toMatchObject({ msgs: 5, lost: 1, reordered: 1, foreign: 0 });
        // A new window starts empty.
        expect(c.closeWindow(1).msgsPerSec).toBe(0);
    });

    it('ignores what is not a flood message', () => {
        const c = new FloodCounter();
        c.onMessage(new ArrayBuffer(8), 0);
        c.onMessage(new ArrayBuffer(32), 0);
        c.onMessage('text', 0);
        expect(c.totals.foreign).toBe(3);
        expect(c.totals.msgs).toBe(0);
    });
});
