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
    HidppFfbPlayer,
    HidppTransport,
    effectParams,
    findFfbFeature,
    hasHidpp,
} from '../js/hid/hidppFfb.js';
import { HidPassthrough } from '../js/hid/HidPassthrough.js';

const FFB_INDEX = 0x0b;

/**
 * A G923 in PC mode as far as HID++ goes: 0x8123 at index 0x0B, slots handed
 * out from 2 (1 being the firmware's), every answer on report 0x12 as the
 * real one gives it. Calls are kept for the test to read.
 */
function fakeWheel({ withFfb = true, silent = false } = {}) {
    const listeners = new Set();
    const calls = [];
    let nextSlot = 2;
    const reply = (reportId, bytes) => {
        const data = new DataView(Uint8Array.from(bytes).buffer);
        globalThis.queueMicrotask(() => listeners.forEach((fn) => fn({ reportId, data })));
    };
    return {
        vendorId: 0x046d,
        productId: 0xc26e,
        productName: 'G923 Racing Wheel for Xbox One and PC',
        opened: false,
        collections: [
            {
                usagePage: 1,
                usage: 4,
                type: 1,
                children: [],
                inputReports: [],
                outputReports: [],
                featureReports: [],
            },
            {
                usagePage: 0xff43,
                usage: 0x0602,
                type: 1,
                children: [],
                inputReports: [],
                outputReports: [],
                featureReports: [],
            },
        ],
        calls,
        async open() {
            this.opened = true;
        },
        addEventListener: (t, fn) => t === 'inputreport' && listeners.add(fn),
        removeEventListener: (t, fn) => t === 'inputreport' && listeners.delete(fn),
        async sendReport(reportId, data) {
            const b = Array.from(data);
            const feature = b[1];
            const func = b[2] >> 4;
            const params = b.slice(3);
            calls.push({ reportId, feature, func, params });
            if (silent) return;
            const head = [0xff, feature, b[2]];
            if (feature === 0 && func === 0) {
                const id = (params[0] << 8) | params[1];
                reply(0x12, [...head, id === 0x8123 && withFfb ? FFB_INDEX : 0, 0, 1]);
            } else if (feature === FFB_INDEX && func === 2) {
                reply(0x12, [...head, params[0] || nextSlot++]);
            } else {
                reply(0x12, [...head]);
            }
        },
        listening: () => listeners.size,
    };
}

const settle = () => new Promise((r) => globalThis.setTimeout(r, 0));
const ffbCalls = (w, func) => w.calls.filter((c) => c.feature === FFB_INDEX && c.func === func);
const s16 = (hi, lo) => (((hi << 8) | lo) << 16) >> 16;

describe('hidppFfb — the wheel and its feature', () => {
    it('knows a Logitech wheel with a HID++ interface', () => {
        expect(hasHidpp(fakeWheel())).toBe(true);
        expect(hasHidpp({ vendorId: 0x1209, collections: [{ usagePage: 0xff43 }] })).toBe(false);
        expect(hasHidpp({ vendorId: 0x046d, collections: [{ usagePage: 1 }] })).toBe(false);
    });

    it('finds 0x8123 through the Root feature, 0 when the wheel lacks it', async () => {
        const w = fakeWheel();
        expect(await findFfbFeature(new HidppTransport(w))).toBe(FFB_INDEX);
        expect(w.calls[0]).toMatchObject({ reportId: 0x11, feature: 0, func: 0 });
        expect(w.calls[0].params.slice(0, 2)).toEqual([0x81, 0x23]);
        expect(await findFfbFeature(new HidppTransport(fakeWheel({ withFfb: false })))).toBe(0);
    });

    it('times out on a wheel that never answers', async () => {
        const t = new HidppTransport(fakeWheel({ silent: true }), { timeoutMs: 5 });
        await expect(t.call(FFB_INDEX, 1)).rejects.toThrow('timeout');
    });
});

describe('hidppFfb — effect parameters', () => {
    it('a constant force along X, gain and sign kept, big-endian', () => {
        const p = effectParams({
            kind: 'constant',
            duration: -1,
            delay: 20,
            gain: 255,
            axes: 1,
            magnitude: -5000,
        });
        expect(p[0]).toBe(0x00);
        expect(p.slice(1, 3)).toEqual([0, 0]); // infinite
        expect(p.slice(3, 5)).toEqual([0, 20]);
        expect(s16(p[5], p[6])).toBe(-16383); // -16383.5 rounded
        expect(p.length).toBe(13);
    });

    it('a polar direction projects onto the wheel: 90° right, 270° left', () => {
        const right = effectParams({
            kind: 'constant',
            directionEnable: 1,
            direction: 9000,
            magnitude: 10000,
        });
        const left = effectParams({
            kind: 'constant',
            directionEnable: 1,
            direction: 27000,
            magnitude: 10000,
        });
        expect(s16(right[5], right[6])).toBe(32767);
        expect(s16(left[5], left[6])).toBe(-32767);
    });

    it('half the effect gain halves the force', () => {
        const p = effectParams({ kind: 'constant', gain: 128, magnitude: 10000 });
        expect(s16(p[5], p[6])).toBeCloseTo(16448, -1);
    });

    it('a spring: left saturation and coefficient, dead band, centre, right', () => {
        const p = effectParams({
            kind: 'spring',
            condition: {
                negativeSaturation: 10000,
                negativeCoefficient: 5000,
                deadBand: 0,
                offset: 0,
                positiveCoefficient: 5000,
                positiveSaturation: 10000,
            },
        });
        expect(p[0]).toBe(0x06);
        expect(p.length).toBe(17);
        expect((p[5] << 8) | p[6]).toBe(0x7fff);
        expect(s16(p[7], p[8])).toBe(16384);
        expect(s16(p[13], p[14])).toBe(16384);
        expect((p[15] << 8) | p[16]).toBe(0x7fff);
    });

    it('an unknown kind gives nothing', () => {
        expect(effectParams({ kind: 'custom' })).toBeNull();
    });
});

describe('hidppFfb — the player', () => {
    async function player() {
        const w = fakeWheel();
        const p = new HidppFfbPlayer(new HidppTransport(w), FFB_INDEX);
        await p.init();
        w.calls.length = 0;
        return { w, p };
    }

    it('init resets the wheel, cancels its centring and sets full gain', async () => {
        const w = fakeWheel();
        await new HidppFfbPlayer(new HidppTransport(w), FFB_INDEX).init();
        expect(w.calls.map((c) => c.func)).toEqual([1, 2, 8]);
        expect(w.calls[1].params.slice(0, 2)).toEqual([0, 0x86]); // new zero spring, autostart
    });

    it('a started effect is downloaded with autostart, then updated in its slot', async () => {
        const { w, p } = await player();
        p.apply({ op: 'effect', effect: 3, kind: 'constant', duration: -1, gain: 255, axes: 1 });
        p.apply({ op: 'constant', effect: 3, magnitude: 2000 });
        expect(w.calls.length).toBe(0); // nothing before it starts
        p.apply({ op: 'start', effect: 3, loops: 1 });
        await settle();
        await settle();
        const first = ffbCalls(w, 2)[0];
        expect(first.params[0]).toBe(0); // new slot
        expect(first.params[1]).toBe(0x80);
        p.apply({ op: 'constant', effect: 3, magnitude: 4000 });
        await settle();
        await settle();
        const update = ffbCalls(w, 2)[1];
        expect(update.params[0]).toBe(3); // the slot the wheel gave (2: init's zero spring)
        expect(s16(update.params[6], update.params[7])).toBe(force(4000));
    });

    it('a burst of updates while busy sends only the newest', async () => {
        const { w, p } = await player();
        p.apply({ op: 'effect', effect: 1, kind: 'constant', gain: 255, axes: 1 });
        p.apply({ op: 'start', effect: 1 });
        for (let m = 1; m <= 50; m++) p.apply({ op: 'constant', effect: 1, magnitude: m * 100 });
        for (let i = 0; i < 6; i++) await settle();
        const downloads = ffbCalls(w, 2);
        expect(downloads.length).toBeLessThanOrEqual(3);
        const last = downloads[downloads.length - 1];
        expect(s16(last.params[6], last.params[7])).toBe(force(5000));
    });

    it('stop, free, gain and reset reach the wheel', async () => {
        const { w, p } = await player();
        p.apply({ op: 'effect', effect: 2, kind: 'damper', gain: 255 });
        p.apply({ op: 'start', effect: 2 });
        p.apply({ op: 'stop', effect: 2 });
        p.apply({ op: 'gain', effect: 0, gain: 255 });
        p.apply({ op: 'free', effect: 2 });
        p.apply({ op: 'control', effect: 0, kind: 'reset' });
        for (let i = 0; i < 10; i++) await settle();
        const funcs = w.calls.map((c) => c.func);
        expect(funcs).toEqual([2, 3, 8, 4, 1, 2]);
        expect(ffbCalls(w, 3)[0].params.slice(0, 2)).toEqual([3, 1]); // its slot, stop
        expect(ffbCalls(w, 8)[0].params.slice(0, 2)).toEqual([0xff, 0xff]);
        expect(ffbCalls(w, 4)[0].params[0]).toBe(3);
    });

    it('close resets and frees the wheel, then plays nothing more', async () => {
        const { w, p } = await player();
        await p.close();
        expect(w.calls.map((c) => c.func)).toEqual([1, 2]);
        p.apply({ op: 'effect', effect: 1, kind: 'constant' });
        p.apply({ op: 'start', effect: 1 });
        await settle();
        expect(w.calls.length).toBe(2);
    });
});

function force(level) {
    return Math.round((level * 32767) / 10000);
}

describe('hidppFfb — in the passthrough', () => {
    function setup(caps) {
        const wheel = fakeWheel();
        const sent = [];
        const hp = new HidPassthrough({
            hid: {
                getDevices: async () => [wheel],
                addEventListener() {},
                removeEventListener() {},
            },
            send: (m) => sent.push(m),
            sendFrame: () => true,
            storage: {
                getItem: () => JSON.stringify(['046d:c26e']),
                setItem() {},
            },
            setTimer: () => 1,
            clearTimer() {},
        });
        hp.handleMessage({ type: 'hidcaps', available: true, ...caps });
        return { wheel, sent, hp };
    }

    async function attached(caps) {
        const s = setup(caps);
        for (let i = 0; i < 6; i++) await settle();
        return s;
    }

    it('asks for force feedback when the host can and the wheel has 0x8123', async () => {
        const { sent } = await attached({ ffb: true });
        const attach = sent.find((m) => m.type === 'hidattach');
        expect(attach.forceFeedback).toBe(true);
    });

    it('does not, and never talks HID++, when the host cannot', async () => {
        const { sent, wheel } = await attached({ ffb: false });
        expect(sent.find((m) => m.type === 'hidattach').forceFeedback).toBeUndefined();
        expect(wheel.calls.length).toBe(0);
    });

    it('plays hidffb once attached, and resets the wheel when it stops', async () => {
        const { wheel, hp } = await attached({ ffb: true });
        hp.handleMessage({ type: 'hidattached', slot: 0, ok: true });
        for (let i = 0; i < 6; i++) await settle();
        wheel.calls.length = 0;
        hp.handleMessage({
            type: 'hidffb',
            slot: 0,
            op: 'effect',
            effect: 1,
            kind: 'constant',
            gain: 255,
            axes: 1,
        });
        hp.handleMessage({ type: 'hidffb', slot: 0, op: 'constant', effect: 1, magnitude: 3000 });
        hp.handleMessage({ type: 'hidffb', slot: 0, op: 'start', effect: 1 });
        for (let i = 0; i < 6; i++) await settle();
        expect(ffbCalls(wheel, 2).length).toBe(1);
        wheel.calls.length = 0;
        hp.stop();
        for (let i = 0; i < 6; i++) await settle();
        expect(wheel.calls.map((c) => c.func)).toEqual([1, 2]);
        expect(wheel.listening()).toBe(0);
    });
});
