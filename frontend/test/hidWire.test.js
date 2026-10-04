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
    HidReportPump,
    REPEAT_MS,
    attachMessage,
    decodeReportFrame,
    detachMessage,
    encodeReportFrame,
    isGameDevice,
    plainCollection,
} from '../js/hid/hidWire.js';

// A WebHID collection keeps its fields on the prototype, as Chrome's do.
function protoObject(fields) {
    return Object.create(fields);
}

function g923Joystick() {
    const hat = protoObject({
        usages: [0x10039],
        reportSize: 4,
        reportCount: 1,
        logicalMinimum: 0,
        logicalMaximum: 7,
        hasNull: true,
        unitSystem: 'english-rotation',
        unitFactorLengthExponent: 1,
    });
    const wheel = protoObject({
        usages: [0x10030],
        reportSize: 16,
        reportCount: 1,
        logicalMinimum: 0,
        logicalMaximum: 65535,
    });
    const report = protoObject({ reportId: 1, items: [hat, wheel] });
    return protoObject({
        usagePage: 1,
        usage: 4,
        type: 1,
        children: [],
        inputReports: [report],
        outputReports: [],
        featureReports: [],
    });
}

describe('hidWire (HID passthrough, P2)', () => {
    it('turns WebHID collections into plain objects JSON can carry', () => {
        const plain = plainCollection(g923Joystick());
        const back = JSON.parse(JSON.stringify(plain));
        expect(back.usagePage).toBe(1);
        expect(back.inputReports[0].reportId).toBe(1);
        expect(back.inputReports[0].items[0]).toMatchObject({
            usages: [0x10039],
            hasNull: true,
            unitSystem: 'english-rotation',
        });
        expect(back.inputReports[0].items[1].logicalMaximum).toBe(65535);
        expect(back.outputReports).toEqual([]);
    });

    it('tells a game device from a keyboard or a vendor interface alone', () => {
        expect(isGameDevice([{ usagePage: 1, usage: 4 }])).toBe(true);
        expect(isGameDevice([{ usagePage: 2, usage: 0xc4 }])).toBe(true);
        expect(
            isGameDevice([
                { usagePage: 0xff43, usage: 0x602 },
                { usagePage: 1, usage: 5 },
            ]),
        ).toBe(true);
        expect(isGameDevice([{ usagePage: 1, usage: 6 }])).toBe(false);
        expect(isGameDevice([{ usagePage: 0xfffd, usage: 0xfd01 }])).toBe(false);
        expect(isGameDevice(undefined)).toBe(false);
    });

    it('writes the attach and detach messages', () => {
        const device = {
            vendorId: 0x046d,
            productId: 0xc26e,
            productName: 'G923',
            collections: [g923Joystick()],
        };
        const m = attachMessage(2, device);
        expect(m).toMatchObject({
            type: 'hidattach',
            slot: 2,
            vendorId: 0x046d,
            productId: 0xc26e,
            productName: 'G923',
        });
        expect(m.collections[0].inputReports[0].items).toHaveLength(2);
        expect(attachMessage(0, { vendorId: 1, productId: 2 })).toMatchObject({
            productName: '',
            collections: [],
        });
        expect(detachMessage(2)).toEqual({ type: 'hiddetach', slot: 2 });
    });

    it('frames a report as [slot][id][seq LE][bytes] and reads it back', () => {
        const f = encodeReportFrame(3, 17, 0x1234, new Uint8Array([8, 0, 0x80]));
        expect(Array.from(f)).toEqual([3, 17, 0x34, 0x12, 8, 0, 0x80]);
        const d = decodeReportFrame(f.buffer);
        expect(d).toMatchObject({ slot: 3, reportId: 17, seq: 0x1234 });
        expect(Array.from(d.bytes)).toEqual([8, 0, 0x80]);
        expect(decodeReportFrame(new Uint8Array([1, 2, 3]))).toBe(null);
    });

    it('numbers reports per slot and wraps at 16 bits', () => {
        const pump = new HidReportPump();
        expect(decodeReportFrame(pump.report(0, 1, [1], 0)).seq).toBe(0);
        expect(decodeReportFrame(pump.report(0, 1, [2], 1)).seq).toBe(1);
        expect(decodeReportFrame(pump.report(1, 0, [9], 1)).seq).toBe(0);
        let last;
        for (let i = 2; i <= 0xffff; i++) last = pump.report(0, 1, [3], 2);
        expect(decodeReportFrame(last).seq).toBe(0xffff);
        expect(decodeReportFrame(pump.report(0, 1, [3], 3)).seq).toBe(0);
    });

    it('repeats an unchanged report every REPEAT_MS, the last one only, until forgotten', () => {
        const pump = new HidReportPump();
        const bytes = new Uint8Array([5, 6]);
        pump.report(0, 1, bytes, 1000);
        bytes[0] = 99; // the caller's buffer is reused by WebHID: the pump kept its own copy
        expect(pump.due(1000 + REPEAT_MS - 1)).toHaveLength(0);
        const again = pump.due(1000 + REPEAT_MS);
        expect(again).toHaveLength(1);
        expect(Array.from(decodeReportFrame(again[0]).bytes)).toEqual([5, 6]);
        expect(pump.due(1000 + REPEAT_MS + 10)).toHaveLength(0); // not before another REPEAT_MS
        pump.report(0, 1, [7, 7], 1700); // a new report resets its clock
        expect(pump.due(1700 + REPEAT_MS - 1)).toHaveLength(0);
        pump.report(0, 2, [1], 1700);
        expect(pump.due(1700 + REPEAT_MS)).toHaveLength(2);
        pump.forget(0);
        expect(pump.due(9999)).toHaveLength(0);
    });
});
