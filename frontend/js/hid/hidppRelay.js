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
 * Logitech HID++ between a Linux host and the real wheel (plan « Passthrough
 * HID », P4 under Linux; Bruno's choice B of 05/10).
 *
 * On a Linux host the kernel's hid-logitech-hidpp binds to the recreated
 * G923 by its VID/PID and talks HID++ to it at probe (protocol version,
 * feature lookup, name). Unanswered, the probe held the wheel's input for
 * about 15 s (H2 bench, 04/10). The host sends those requests to the page
 * (`hidrequest`); this file passes them to the real wheel and sends its
 * answers back (`hidreply`), injected once into the recreated wheel: the
 * probe then ends within a second (05/10).
 *
 * Force feedback does not follow: the driver's 0x8123 setup refuses any
 * device that is not usbhid ("device is not USB"), and a uhid device never
 * is. 0x8123 stays on the list for its probe questions.
 *
 * Filtered, because the wheel's HID++ also reaches its firmware updates and
 * on-board settings: a host must not reprogram the wheel in the viewer's
 * hands. Only these go through:
 *  - root (index 0): getFeature, and only for an allowed feature (any other
 *    is answered "not present" here), and the protocol version / ping;
 *  - feature 0x8123 (force feedback): every function;
 *  - 0x0003 (firmware info) and 0x0005 (device name): their read functions.
 * The kernel learns feature indexes from getFeature only, so a feature it
 * was told is absent never gets an index to call. Anything else is answered
 * here with a HID++ 2.0 error. When relaying stops, a wheel whose motor was
 * driven is reset and left free.
 *
 * Frames as WebHID gives them, without the report id: [device index][feature
 * index][function << 4 | software id][parameters…]. The kernel's software id
 * is its own, distinct from G HUB's and from this page's (hidppFfb.js), so
 * the wheel's answers are told apart by (feature index, function, software
 * id), or (0xFF, feature index, function, software id) for an error.
 */

export const HIDPP_LONG = 0x11;
export const HIDPP_VERY_LONG = 0x12;
const ERROR = 0xff;
const ERR_INVALID_FEATURE_INDEX = 0x06;
const ERR_INVALID_FUNCTION_ID = 0x07;
const ANSWER_MS = 1000;
const MAX_WAITING = 32;

/** Feature id → the functions that may go through (null: all). */
const ALLOWED = new Map([
    [0x0000, [0, 1]],
    [0x0003, [0, 1]],
    [0x0005, [0, 1, 2]],
    [0x8123, null],
]);
const FFB = 0x8123;

/** True for a report id this file relays. */
export function isHidppReport(reportId) {
    return reportId === HIDPP_LONG || reportId === HIDPP_VERY_LONG;
}

/** The input report ids of a device's HID++ collections (page 0xFF43). */
export function hidppInputIds(device) {
    const ids = new Set();
    for (const c of device?.collections || [])
        if (c.usagePage === 0xff43) for (const r of c.inputReports || []) ids.add(r.reportId);
    return ids;
}

export class HidppRelay {
    /**
     * @param {HIDDevice} device the real wheel, opened
     * @param {object} o
     * @param {(reportId: number, bytes: Uint8Array) => void} o.reply to the host
     * @param {() => Promise<void>} [o.resetMotor] leaves the wheel free (called
     *     on close when the host drove the motor)
     */
    constructor(device, { reply, resetMotor = null, answerMs = ANSWER_MS }) {
        this._device = device;
        this._reply = reply;
        this._resetMotor = resetMotor;
        this._answerMs = answerMs;
        this._features = new Map([[0, 0x0000]]); // feature index → id, as the wheel told the host
        this._chain = Promise.resolve();
        this._waiting = 0;
        this._pending = null; // { match, resolve }
        this._motor = false;
        this._closed = false;
        this.relayed = 0;
        this.refused = 0;
        this._onReport = (e) => this._input(e);
        device.addEventListener('inputreport', this._onReport);
    }

    /** One `hidrequest` output report from the host: answered, relayed or refused. */
    handle(reportId, bytes) {
        if (this._closed || !isHidppReport(reportId) || bytes.length < 4) return;
        if (this._waiting >= MAX_WAITING) return; // the host's own timeout answers
        this._waiting++;
        const req = Uint8Array.from(bytes);
        this._chain = this._chain
            .then(() => this._one(reportId, req))
            .catch(() => {})
            .finally(() => this._waiting--);
    }

    /** Stops relaying; resets the motor if the host drove it. */
    async close() {
        if (this._closed) return;
        this._closed = true;
        this._device.removeEventListener('inputreport', this._onReport);
        if (this._pending) this._pending.resolve(null);
        if (this._motor && this._resetMotor) {
            try {
                await this._resetMotor();
            } catch {
                /* the wheel went away */
            }
        }
    }

    async _one(reportId, req) {
        const index = req[1];
        const func = req[2] >> 4;
        const feature = this._features.get(index);
        if (feature === undefined) return this._error(reportId, req, ERR_INVALID_FEATURE_INDEX);
        const fns = ALLOWED.get(feature);
        if (fns === undefined) return this._error(reportId, req, ERR_INVALID_FEATURE_INDEX);
        if (fns && !fns.includes(func)) return this._error(reportId, req, ERR_INVALID_FUNCTION_ID);
        let asked = 0;
        if (feature === 0x0000 && func === 0) {
            asked = (req[3] << 8) | req[4];
            if (!ALLOWED.has(asked)) {
                // "Not present": index 0, no type, no version.
                const answer = new Uint8Array(req.length);
                answer.set(req.subarray(0, 3));
                this.refused++;
                return this._reply(reportId, answer);
            }
        }
        if (feature === FFB) this._motor = true;
        const answer = await this._ask(reportId, req);
        if (!answer || this._closed) return;
        if (asked && answer.bytes[1] === index && answer.bytes[3])
            this._features.set(answer.bytes[3], asked);
        this.relayed++;
        this._reply(answer.reportId, answer.bytes);
    }

    // Sends the request to the wheel and waits for its answer (or its error).
    _ask(reportId, req) {
        const [dev, index, fs] = req;
        return new Promise((resolve) => {
            const timer = globalThis.setTimeout(() => done(null), this._answerMs);
            const done = (v) => {
                globalThis.clearTimeout(timer);
                this._pending = null;
                resolve(v);
            };
            this._pending = {
                match: (b) =>
                    b[0] === dev &&
                    ((b[1] === index && b[2] === fs) ||
                        (b[1] === ERROR && b[2] === index && b[3] === fs)),
                resolve: done,
            };
            this._device.sendReport(reportId, req).catch(() => done(null));
        });
    }

    _input(e) {
        if (!this._pending || !isHidppReport(e.reportId)) return;
        const b = new Uint8Array(e.data.buffer, e.data.byteOffset, e.data.byteLength);
        if (this._pending.match(b))
            this._pending.resolve({ reportId: e.reportId, bytes: Uint8Array.from(b) });
    }

    _error(reportId, req, code) {
        const answer = new Uint8Array(req.length);
        answer[0] = req[0];
        answer[1] = ERROR;
        answer[2] = req[1];
        answer[3] = req[2];
        answer[4] = code;
        this.refused++;
        this._reply(reportId, answer);
    }
}
