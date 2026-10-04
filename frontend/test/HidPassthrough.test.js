/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

import { describe, expect, it, vi } from 'vitest';
import {
    FORWARD_KEY,
    HidPassthrough,
    deviceKey,
    loadForwarded,
    padKeyOf,
    saveForwarded,
} from '../js/hid/HidPassthrough.js';
import { decodeReportFrame } from '../js/hid/hidWire.js';

function fakeDevice({
    vendorId = 0x046d,
    productId = 0xc26e,
    usagePage = 1,
    usage = 4,
    openFails = false,
} = {}) {
    const listeners = new Set();
    return {
        vendorId,
        productId,
        productName: 'G923',
        opened: false,
        collections: [
            {
                usagePage,
                usage,
                type: 1,
                children: [],
                inputReports: [],
                outputReports: [],
                featureReports: [],
            },
        ],
        async open() {
            if (openFails) throw new Error('busy');
            this.opened = true;
        },
        addEventListener: (t, fn) => t === 'inputreport' && listeners.add(fn),
        removeEventListener: (t, fn) => t === 'inputreport' && listeners.delete(fn),
        report(reportId, bytes) {
            const data = new DataView(Uint8Array.from(bytes).buffer);
            for (const fn of listeners) fn({ reportId, data });
        },
        listening: () => listeners.size,
    };
}

function fakeHid(devices) {
    const listeners = new Set();
    return {
        getDevices: async () => devices,
        requestDevice: vi.fn(async () => devices.slice(0, 1)),
        addEventListener: (t, fn) => t === 'disconnect' && listeners.add(fn),
        removeEventListener: (t, fn) => t === 'disconnect' && listeners.delete(fn),
        unplug: (device) => listeners.forEach((fn) => fn({ device })),
    };
}

function memoryStorage(initial = {}) {
    const m = new Map(Object.entries(initial));
    return { getItem: (k) => (m.has(k) ? m.get(k) : null), setItem: (k, v) => m.set(k, v) };
}

function setup(devices, wanted = []) {
    const hid = fakeHid(devices);
    const sent = [];
    const frames = [];
    let tick = null;
    let now = 0;
    const storage = memoryStorage({ [FORWARD_KEY]: JSON.stringify(wanted) });
    const onChange = vi.fn();
    const onResult = vi.fn();
    const hp = new HidPassthrough({
        hid,
        send: (m) => sent.push(m),
        sendFrame: (f) => frames.push(f) && true,
        storage,
        now: () => now,
        setTimer: (fn) => (tick = fn),
        clearTimer: () => (tick = null),
        onChange,
        onResult,
    });
    return {
        hp,
        onResult,
        hid,
        sent,
        frames,
        storage,
        onChange,
        advance: (ms) => {
            now += ms;
            if (tick) tick();
        },
        ticking: () => !!tick,
    };
}

describe('HidPassthrough (HID passthrough in the stream, P2)', () => {
    it('remembers switched-on devices by vid:pid, and maps them to pad keys', () => {
        const d = fakeDevice();
        expect(deviceKey(d)).toBe('046d:c26e');
        expect(padKeyOf(d)).toBe('usb:046d:c26e');
        const storage = memoryStorage();
        saveForwarded(new Set(['046d:c26e']), storage);
        expect(Array.from(loadForwarded(storage))).toEqual(['046d:c26e']);
        expect(loadForwarded(memoryStorage({ [FORWARD_KEY]: 'not json' })).size).toBe(0);
        expect(
            loadForwarded({
                getItem: () => {
                    throw new Error('blocked');
                },
            }).size,
        ).toBe(0);
        saveForwarded(new Set(), {
            setItem: () => {
                throw new Error('blocked');
            },
        }); // no throw
    });

    it('attaches the wanted devices once the host can, then streams their reports', async () => {
        const wheel = fakeDevice();
        const t = setup([wheel], ['046d:c26e']);
        expect(t.hp.supported).toBe(true);
        t.hp.handleMessage({ type: 'hidcaps', available: true, why: '' });
        await vi.waitFor(() => expect(t.sent).toHaveLength(1));
        expect(t.sent[0]).toMatchObject({
            type: 'hidattach',
            slot: 0,
            vendorId: 0x046d,
            productId: 0xc26e,
        });
        expect(wheel.opened).toBe(true);
        expect(t.hp.excludedKeys()).toEqual(['usb:046d:c26e']);

        t.hp.handleMessage({ type: 'hidattached', slot: 0, ok: true });
        expect((await t.hp.devices())[0]).toMatchObject({
            key: '046d:c26e',
            wanted: true,
            state: 'on',
        });

        wheel.report(1, [8, 0, 0x80]);
        expect(t.frames).toHaveLength(1);
        expect(decodeReportFrame(t.frames[0])).toMatchObject({ slot: 0, reportId: 1, seq: 0 });
        // An unchanged report goes again after 500 ms.
        t.advance(400);
        expect(t.frames).toHaveLength(1);
        t.advance(100);
        expect(t.frames).toHaveLength(2);
    });

    it('does nothing for a host that cannot, and lets go when it says it no longer can', async () => {
        const wheel = fakeDevice();
        const t = setup([wheel], ['046d:c26e']);
        t.hp.handleMessage({ type: 'hidcaps', available: false, why: 'needs the native host' });
        await Promise.resolve();
        expect(t.sent).toHaveLength(0);
        expect(t.hp.caps).toEqual({ available: false, why: 'needs the native host' });

        t.hp.handleMessage({ type: 'hidcaps', available: true });
        await vi.waitFor(() => expect(t.sent).toHaveLength(1));
        t.hp.handleMessage({ type: 'hidcaps', available: false, why: 'gone' });
        expect(t.sent[1]).toEqual({ type: 'hiddetach', slot: 0 });
        expect(wheel.listening()).toBe(0);
        expect(t.ticking()).toBe(false);
    });

    it('shows a refusal by the device, and frees its slot', async () => {
        const wheel = fakeDevice();
        const t = setup([wheel], ['046d:c26e']);
        t.hp.handleMessage({ type: 'hidcaps', available: true });
        await vi.waitFor(() => expect(t.sent).toHaveLength(1));
        t.hp.handleMessage({ type: 'hidattached', slot: 0, ok: false, why: 'not a game device' });
        expect(t.onResult).toHaveBeenCalledWith(wheel, false, 'not a game device');
        const [d] = await t.hp.devices();
        expect(d).toMatchObject({ state: 'refused', why: 'not a game device' });
        expect(t.hp.excludedKeys()).toEqual([]);
        expect(wheel.listening()).toBe(0);
        expect(t.hp.handleMessage({ type: 'hidattached', slot: 5, ok: true })).toBe(true); // unknown slot
    });

    it('switches a device on and off for this viewer', async () => {
        const wheel = fakeDevice();
        const radio = fakeDevice({ vendorId: 0x1209, productId: 0x4f54, usage: 5 });
        const t = setup([wheel, radio]);
        t.hp.handleMessage({ type: 'hidcaps', available: true });
        await Promise.resolve();
        expect(t.sent).toHaveLength(0);
        await t.hp.setWanted(radio, true);
        expect(t.sent[0]).toMatchObject({ type: 'hidattach', slot: 0, vendorId: 0x1209 });
        await t.hp.setWanted(wheel, true);
        expect(t.sent[1]).toMatchObject({ type: 'hidattach', slot: 1 });
        expect(Array.from(loadForwarded(t.storage))).toEqual(['1209:4f54', '046d:c26e']);
        await t.hp.setWanted(radio, false);
        expect(t.sent[2]).toEqual({ type: 'hiddetach', slot: 0 });
        expect(Array.from(loadForwarded(t.storage))).toEqual(['046d:c26e']);
        expect(t.onChange).toHaveBeenCalled();
    });

    it('drops an unplugged device, keeps keyboards out, and survives a device it cannot open', async () => {
        const wheel = fakeDevice();
        const keyboard = fakeDevice({ vendorId: 1, productId: 2, usage: 6 });
        const busy = fakeDevice({ vendorId: 3, productId: 4, openFails: true });
        const t = setup([wheel, keyboard, busy], ['046d:c26e', '0003:0004', '0001:0002']);
        expect((await t.hp.devices()).map((d) => d.key)).toEqual(['046d:c26e', '0003:0004']);
        t.hp.handleMessage({ type: 'hidcaps', available: true });
        await vi.waitFor(() => expect(t.sent).toHaveLength(1));
        const states = await t.hp.devices();
        expect(states[1]).toMatchObject({ state: 'refused', why: 'busy' });
        t.hid.unplug(wheel);
        expect(t.sent[1]).toEqual({ type: 'hiddetach', slot: 0 });
        expect((await t.hp.devices())[0]).toMatchObject({ state: 'off', why: 'unplugged' });
    });

    it('counts host requests, ignores other messages, opens the chooser and stops cleanly', async () => {
        const wheel = fakeDevice();
        const t = setup([wheel], ['046d:c26e']);
        expect(t.hp.handleMessage({ type: 'hidrequest', slot: 0, kind: 'output' })).toBe(true);
        expect(t.hp.requests).toBe(1);
        expect(t.hp.handleMessage({ type: 'rumble' })).toBe(false);
        expect(t.hp.handleMessage(null)).toBe(false);
        expect(await t.hp.choose()).toEqual([wheel]);
        t.hp.handleMessage({ type: 'hidcaps', available: true });
        await vi.waitFor(() => expect(t.sent).toHaveLength(1));
        t.hp.stop();
        expect(t.sent[1]).toEqual({ type: 'hiddetach', slot: 0 });
    });

    it('says WebHID is missing rather than failing', async () => {
        const hp = new HidPassthrough({
            hid: null,
            send: () => {},
            sendFrame: () => true,
            storage: memoryStorage(),
        });
        expect(hp.supported).toBe(false);
        expect(await hp.devices()).toEqual([]);
        expect(await hp.choose()).toEqual([]);
        await hp.applyWanted();
        hp.stop();
    });
});
