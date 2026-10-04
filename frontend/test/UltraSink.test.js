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
import {
    ULTRA_CHANNEL_ID,
    ULTRA_GIVE_UP_MS,
    ULTRA_HEADER_BYTES,
    UltraSink,
    ultraChannelInit,
    ultraSinkMode,
} from '../js/api/UltraSink.js';

/** One chunk in the host's format (FrameSender::writeHeader). */
function chunk(frameId, index, total, stampMs, payload = 1000) {
    const buf = new ArrayBuffer(ULTRA_HEADER_BYTES + payload);
    const view = new DataView(buf);
    view.setUint32(0, frameId);
    view.setUint16(4, index);
    view.setUint16(6, total);
    view.setUint8(8, 0);
    view.setUint32(9, payload);
    view.setUint32(13, stampMs);
    return buf;
}

describe('UltraSink (POC Ultra U1.1, the transport lab)', () => {
    it('is off unless localStorage asks, and matches the host channel', () => {
        expect(ultraSinkMode({ getItem: () => null })).toBe(null);
        expect(ultraSinkMode({ getItem: () => '0' })).toBe(null);
        expect(ultraSinkMode({ getItem: () => '1' })).toBe('video');
        expect(ultraSinkMode({ getItem: () => 'unordered' })).toBe('unordered');
        expect(
            ultraSinkMode({
                getItem: () => {
                    throw new Error('blocked');
                },
            }),
        ).toBe(null);
        expect(ultraChannelInit('video')).toEqual({
            negotiated: true,
            id: ULTRA_CHANNEL_ID,
            ordered: true,
            maxPacketLifeTime: 500,
        });
        expect(ultraChannelInit('unordered')).toEqual({
            negotiated: true,
            id: 5,
            ordered: false,
            maxRetransmits: 0,
        });
    });

    it('times each frame: its spread and its delay over the smallest', () => {
        const s = new UltraSink();
        // Frame 0, stamped at host 1000 ms: 3 chunks from 50 to 54 on the page clock.
        s.onMessage(chunk(0, 0, 3, 1000), 50);
        s.onMessage(chunk(0, 1, 3, 1000), 52);
        s.onMessage(chunk(0, 2, 3, 1000), 54);
        // Frame 1, 10 ms later on the host, 2 chunks, arriving 6 ms later than frame 0 did.
        s.onMessage(chunk(1, 0, 2, 1010), 68);
        s.onMessage(chunk(1, 1, 2, 1010), 70);
        const w = s.closeWindow(1, 71);
        expect(w.framesPerSec).toBe(2);
        expect(w.lost).toBe(0);
        // Spreads 4 and 2 ms; delays 54-1000 and 70-1010, i.e. 0 and +6 over the smallest.
        expect(w.spreadMs.max).toBe(4);
        expect(w.extraDelayMs.max).toBe(6);
        expect(w.extraDelayMs.p50).toBe(6);
        expect(s.totals.chunks).toBe(5);
        expect(s.totals.bytes).toBe(5 * (ULTRA_HEADER_BYTES + 1000));
    });

    it('counts a frame id never seen, and a frame left incomplete, as lost', () => {
        const s = new UltraSink();
        s.onMessage(chunk(0, 0, 1, 0), 10);
        // Frame 1 never comes; frame 2 loses its second chunk.
        s.onMessage(chunk(2, 0, 2, 20), 30);
        s.onMessage(chunk(3, 0, 1, 30), 40);
        expect(s.totals.lost).toBe(1);
        const w = s.closeWindow(1, 30 + ULTRA_GIVE_UP_MS + 1);
        expect(w.lost).toBe(2);
        expect(s.totals.frames).toBe(2);
    });

    it('takes unordered chunks in any order', () => {
        const s = new UltraSink();
        s.onMessage(chunk(0, 1, 2, 0), 12);
        s.onMessage(chunk(0, 0, 2, 0), 10);
        expect(s.totals.frames).toBe(1);
        expect(s.closeWindow(1, 20).spreadMs.max).toBe(2);
    });

    it('ignores what is not one of its chunks', () => {
        const s = new UltraSink();
        s.onMessage(new ArrayBuffer(4), 1);
        s.onMessage('text', 1);
        s.onMessage(chunk(0, 0, 0, 0), 1);
        expect(s.totals.foreign).toBe(3);
        expect(s.totals.frames).toBe(0);
    });
});
