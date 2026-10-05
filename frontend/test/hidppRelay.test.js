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
import { HidppRelay, hidppInputIds, isHidppReport } from '../js/hid/hidppRelay.js';
import { HidPassthrough } from '../js/hid/HidPassthrough.js';

const FFB_INDEX = 0x0b;
const DFU_INDEX = 0x0c;
const KERNEL_SWID = 0x01;

const empty = () => ({ children: [], inputReports: [], outputReports: [], featureReports: [] });

/**
 * A G923 as far as HID++ goes, answering on 0x12 as the real one does:
 * 0x8123 at index 0x0B and a firmware-update feature (0x00D0) at 0x0C.
 * `others` are reports another reader (G HUB) gets in between.
 */
function fakeWheel({ silent = false } = {}) {
    const listeners = new Set();
    const sent = [];
    const emit = (reportId, bytes) => {
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
                ...empty(),
                usagePage: 1,
                usage: 4,
                type: 1,
                inputReports: [{ reportId: 1, items: [] }],
            },
            {
                ...empty(),
                usagePage: 0xff43,
                usage: 0x0602,
                type: 1,
                inputReports: [{ reportId: 0x11, items: [] }],
                outputReports: [{ reportId: 0x11, items: [] }],
            },
            {
                ...empty(),
                usagePage: 0xff43,
                usage: 0x0604,
                type: 1,
                inputReports: [{ reportId: 0x12, items: [] }],
                outputReports: [{ reportId: 0x12, items: [] }],
            },
        ],
        sent,
        emit,
        async open() {
            this.opened = true;
        },
        addEventListener: (t, fn) => t === 'inputreport' && listeners.add(fn),
        removeEventListener: (t, fn) => t === 'inputreport' && listeners.delete(fn),
        async sendReport(reportId, data) {
            const b = Array.from(data);
            sent.push({ reportId, bytes: b });
            if (silent) return;
            const [dev, index, fs, p0, p1] = b;
            // Someone else's answer first: never taken for ours.
            emit(0x12, [dev, index, (fs & 0xf0) | 0x02, 0x77]);
            if (index === 0 && fs >> 4 === 0) {
                const id = (p0 << 8) | p1;
                const at = id === 0x8123 ? FFB_INDEX : id === 0x00d0 ? DFU_INDEX : 0;
                emit(0x12, [dev, 0, fs, at, 0, 1]);
            } else if (index === FFB_INDEX && fs >> 4 === 2) {
                emit(0x12, [dev, index, fs, 3]);
            } else {
                emit(0x12, [dev, index, fs, 4, 2]);
            }
        },
    };
}

/** A HID++ request from the kernel, as the host hands it (19 bytes, no id). */
const req = (index, func, ...params) => {
    const b = new Uint8Array(19);
    b.set([0xff, index, (func << 4) | KERNEL_SWID, ...params]);
    return b;
};

const settle = async () => {
    for (let i = 0; i < 6; i++) await new Promise((r) => globalThis.setTimeout(r, 0));
};

function relayOf(wheel, o = {}) {
    const replies = [];
    let resets = 0;
    const relay = new HidppRelay(wheel, {
        reply: (reportId, bytes) => replies.push({ reportId, bytes: Array.from(bytes) }),
        resetMotor: async () => {
            resets++;
        },
        ...o,
    });
    return { relay, replies, resets: () => resets };
}

describe('hidppRelay — helpers', () => {
    it('knows HID++ reports and a device’s HID++ input ids', () => {
        expect(isHidppReport(0x11)).toBe(true);
        expect(isHidppReport(0x12)).toBe(true);
        expect(isHidppReport(1)).toBe(false);
        expect([...hidppInputIds(fakeWheel())]).toEqual([0x11, 0x12]);
    });
});

describe('hidppRelay — the filter', () => {
    it('relays the probe and the force feedback feature, answers on the wheel’s report', async () => {
        const w = fakeWheel();
        const { relay, replies } = relayOf(w);
        relay.handle(0x11, req(0, 1)); // protocol version
        relay.handle(0x11, req(0, 0, 0x81, 0x23)); // getFeature(0x8123)
        relay.handle(0x11, req(FFB_INDEX, 2, 0, 0x80)); // DownloadEffect
        await settle();
        expect(w.sent.map((s) => s.bytes[1])).toEqual([0, 0, FFB_INDEX]);
        expect(replies).toHaveLength(3);
        expect(replies.every((r) => r.reportId === 0x12)).toBe(true);
        expect(replies[1].bytes.slice(0, 4)).toEqual([0xff, 0, 0x01, FFB_INDEX]);
        expect(replies[2].bytes.slice(0, 4)).toEqual([0xff, FFB_INDEX, 0x21, 3]);
        expect(relay.relayed).toBe(3);
    });

    it('tells the host a feature off the list is absent, never asking the wheel', async () => {
        const w = fakeWheel();
        const { relay, replies } = relayOf(w);
        relay.handle(0x11, req(0, 0, 0x00, 0xd0)); // getFeature(DFU)
        await settle();
        expect(w.sent).toEqual([]);
        expect(replies).toHaveLength(1);
        expect(replies[0].reportId).toBe(0x11);
        expect(replies[0].bytes.slice(0, 6)).toEqual([0xff, 0, 0x01, 0, 0, 0]);
    });

    it('refuses an index the host was never given, and functions off the list', async () => {
        const w = fakeWheel();
        const { relay, replies } = relayOf(w);
        relay.handle(0x11, req(DFU_INDEX, 1)); // guessed index
        relay.handle(0x11, req(0, 2)); // root function 2
        await settle();
        expect(w.sent).toEqual([]);
        expect(replies[0].bytes.slice(0, 5)).toEqual([0xff, 0xff, DFU_INDEX, 0x11, 0x06]);
        expect(replies[1].bytes.slice(0, 5)).toEqual([0xff, 0xff, 0, 0x21, 0x07]);
        expect(relay.refused).toBe(2);
    });

    it('sends nothing back when the wheel stays silent (the host times out)', async () => {
        const w = fakeWheel({ silent: true });
        const { relay, replies } = relayOf(w, { answerMs: 5 });
        relay.handle(0x11, req(0, 1));
        await new Promise((r) => globalThis.setTimeout(r, 20));
        expect(w.sent).toHaveLength(1);
        expect(replies).toEqual([]);
    });

    it('resets the motor on close only if the host drove it', async () => {
        const quiet = relayOf(fakeWheel());
        quiet.relay.handle(0x11, req(0, 1));
        await settle();
        await quiet.relay.close();
        expect(quiet.resets()).toBe(0);

        const driven = relayOf(fakeWheel());
        driven.relay.handle(0x11, req(0, 0, 0x81, 0x23));
        driven.relay.handle(0x11, req(FFB_INDEX, 2, 0, 0x80));
        await settle();
        await driven.relay.close();
        expect(driven.resets()).toBe(1);
        driven.relay.handle(0x11, req(0, 1));
        await settle();
        expect(driven.replies).toHaveLength(2); // nothing after close
    });
});

describe('hidppRelay — in the passthrough', () => {
    it('relays hidrequest under a Linux host and keeps HID++ off the hid channel', async () => {
        const w = fakeWheel();
        const sent = [];
        const frames = [];
        const storage = new Map([['mw_hid_forward', JSON.stringify(['046d:c26e'])]]);
        const listeners = [];
        const realAdd = w.addEventListener;
        w.addEventListener = (t, fn) => {
            listeners.push(fn);
            realAdd(t, fn);
        };
        const hp = new HidPassthrough({
            hid: { addEventListener() {}, removeEventListener() {}, getDevices: async () => [w] },
            send: (m) => sent.push(m),
            sendFrame: (f) => frames.push(f),
            storage: {
                getItem: (k) => storage.get(k) ?? null,
                setItem: (k, v) => storage.set(k, v),
            },
            setTimer: () => 1,
            clearTimer: () => {},
        });
        hp.handleMessage({ type: 'hidcaps', available: true, ffb: false, hidpp: true });
        await settle();
        const attach = sent.find((m) => m.type === 'hidattach');
        expect(attach.forceFeedback).toBeUndefined();
        hp.handleMessage({ type: 'hidattached', slot: attach.slot, ok: true });
        // The kernel's request as Linux hands it: report id first, 20 bytes.
        const withId = Uint8Array.from([0x11, ...req(0, 1)]);
        hp.handleMessage({
            type: 'hidrequest',
            slot: attach.slot,
            kind: 'output',
            reportId: 0x11,
            data: globalThis.btoa(String.fromCharCode(...withId)),
        });
        await settle();
        expect(w.sent.at(-1).bytes).toHaveLength(19);
        const reply = sent.find((m) => m.type === 'hidreply');
        expect(reply).toMatchObject({ slot: attach.slot, reportId: 0x12 });
        const bytes = Uint8Array.from(globalThis.atob(reply.data), (c) => c.charCodeAt(0));
        expect(Array.from(bytes.slice(0, 3))).toEqual([0xff, 0, 0x11]);
        // The joystick report goes on the channel, the wheel's HID++ never.
        const frameIds = () => frames.map((f) => f[1]);
        const wheelListener = listeners[0];
        wheelListener({ reportId: 1, data: new DataView(new Uint8Array(10).buffer) });
        wheelListener({ reportId: 0x12, data: new DataView(new Uint8Array(63).buffer) });
        expect(frameIds()).toEqual([1]);
        hp.stop();
    });
});
