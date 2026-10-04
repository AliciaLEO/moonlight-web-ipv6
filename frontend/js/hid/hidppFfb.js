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
 * Force feedback on a Logitech wheel through HID++ 2.0 feature 0x8123
 * (FORCE_FEEDBACK), the way the G920 and the G923 for Xbox take it (plan
 * « Passthrough HID », P4; checked on Bruno's G923 on 04/10 from Chrome, with
 * G HUB open beside it).
 *
 * The host decodes what the game asks of the recreated wheel (its PID block)
 * into `hidffb` operations; this file plays them on the real wheel:
 *  - each game effect gets one of the wheel's effect slots (DownloadEffect,
 *    slot 0 = a new one, the wheel answers with its slot), later uploads of
 *    the same effect rewrite that slot in place;
 *  - start, stop and free map to SetEffectState and DestroyEffect;
 *  - one command at a time, as the wheel answers each: while one is in
 *    flight, newer parameters for an effect replace the older ones waiting,
 *    so a game streaming its constant force at 300 Hz never builds a queue.
 *
 * Frames: report 0x11 (19 bytes) or 0x12 (63 bytes), as WebHID sends them,
 * without the id: [0xFF device][feature index][function << 4 | software id]
 * [parameters…], multi-byte values big-endian. The software id tells our
 * answers from G HUB's (it talks to the wheel at the same time with its own).
 */

export const FEATURE_FFB = 0x8123;
const LONG = 0x11;
const VERY_LONG = 0x12;
const SWID = 0x0e;
const TIMEOUT_MS = 300;

const TYPES = {
    constant: 0x00,
    sine: 0x01,
    square: 0x02,
    triangle: 0x03,
    sawtoothUp: 0x04,
    sawtoothDown: 0x05,
    spring: 0x06,
    damper: 0x07,
    friction: 0x08,
    inertia: 0x09,
    ramp: 0x0a,
};
const AUTOSTART = 0x80;
const STATE_STOP = 1;
const STATE_PLAY = 2;

const fn = { getInfo: 0, resetAll: 1, download: 2, setState: 3, destroy: 4, setGains: 8 };

/** A Logitech device with a HID++ interface the page can talk to. */
export function hasHidpp(device) {
    return (
        device?.vendorId === 0x046d &&
        (device.collections || []).some((c) => c.usagePage === 0xff43)
    );
}

const be16 = (v) => [(v >> 8) & 0xff, v & 0xff];
const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
/** DirectInput's -10000..10000 to a signed 16-bit force, scaled by a 0..1 gain. */
const force16 = (v, gain = 1) => clamp(Math.round((v * gain * 32767) / 10000), -32767, 32767);
/** 0..10000 to the 15-bit saturations and dead band the wheel takes. */
const level15 = (v) => clamp(Math.round((v * 0x7fff) / 10000), 0, 0x7fff);
/** 0..10000 to an envelope level byte. */
const level8 = (v) => clamp(Math.round((v * 255) / 10000), 0, 255);
const ms16 = (v) => clamp(Math.round(v), 0, 0xffff);

/**
 * HID++ calls on one device, one at a time. Replies come back as input
 * reports 0x11 or 0x12 (the G923 answers a 0x11 request on 0x12).
 */
export class HidppTransport {
    constructor(device, { timeoutMs = TIMEOUT_MS } = {}) {
        this._device = device;
        this._timeoutMs = timeoutMs;
        this._pending = null;
        this._chain = Promise.resolve();
        this._listener = (e) => {
            if (e.reportId !== LONG && e.reportId !== VERY_LONG) return;
            const p = this._pending;
            if (!p) return;
            const b = new Uint8Array(e.data.buffer, e.data.byteOffset, e.data.byteLength);
            if (b[1] === p.feature && b[2] === p.function) p.resolve(b.slice(3));
            else if (b[1] === 0xff && b[2] === p.feature && b[3] === p.function)
                p.reject(new Error(`HID++ error ${b[4]}`));
        };
        device.addEventListener('inputreport', this._listener);
    }

    /** Resolves with the reply's parameters; rejects on an error or a timeout. */
    call(feature, func, params = []) {
        const run = () => this._call(feature, func, params);
        const p = this._chain.then(run, run);
        this._chain = p.catch(() => {});
        return p;
    }

    close() {
        this._device.removeEventListener('inputreport', this._listener);
        if (this._pending) this._pending.reject(new Error('closed'));
    }

    _call(feature, func, params) {
        const reportId = params.length > 16 ? VERY_LONG : LONG;
        const data = new Uint8Array(reportId === VERY_LONG ? 63 : 19);
        data[0] = 0xff;
        data[1] = feature;
        data[2] = (func << 4) | SWID;
        data.set(params.slice(0, data.length - 3), 3);
        return new Promise((resolve, reject) => {
            const timer = setTimeout(
                () => done(reject, new Error('HID++ timeout')),
                this._timeoutMs,
            );
            const done = (fnDone, v) => {
                clearTimeout(timer);
                this._pending = null;
                fnDone(v);
            };
            this._pending = {
                feature,
                function: data[2],
                resolve: (v) => done(resolve, v),
                reject: (err) => done(reject, err),
            };
            this._device.sendReport(reportId, data).catch((err) => this._pending?.reject(err));
        });
    }
}

/** The index of feature 0x8123 on this device, or 0 when it has none. */
export async function findFfbFeature(transport) {
    try {
        const r = await transport.call(0x00, 0, be16(FEATURE_FFB));
        return r[0] || 0;
    } catch {
        return 0;
    }
}

/** The DownloadEffect parameters for one effect, without its slot byte. */
export function effectParams(e) {
    const type = TYPES[e.kind];
    if (type === undefined) return null;
    const gain = (e.gain ?? 255) / 255;
    // Along the wheel's one axis: the polar direction's X component when the
    // game gave one, the level as is when it enabled X alone.
    const dir =
        e.directionEnable && e.direction !== undefined
            ? Math.sin((e.direction * Math.PI) / 18000)
            : (e.axes ?? 1) & 1
              ? 1
              : 0;
    const env = e.envelope || {};
    const envelope = [
        level8(env.attackLevel || 0),
        ...be16(ms16(env.attackTime || 0)),
        level8(env.fadeLevel || 0),
        ...be16(ms16(env.fadeTime || 0)),
    ];
    const head = [
        type,
        ...be16(e.duration < 0 ? 0 : ms16(e.duration || 0)),
        ...be16(ms16(e.delay || 0)),
    ];
    const s16 = (v) => be16(v & 0xffff);
    if (e.kind === 'constant')
        return [...head, ...s16(force16((e.magnitude || 0) * dir, gain)), ...envelope];
    if (e.kind === 'ramp')
        return [
            ...head,
            ...s16(force16((e.start || 0) * dir, gain)),
            ...s16(force16((e.end || 0) * dir, gain)),
            ...envelope,
        ];
    if (TYPES[e.kind] <= TYPES.sawtoothDown) {
        const sign = dir < 0 ? -1 : 1;
        return [
            ...head,
            ...s16(force16((e.magnitude || 0) * sign, gain)),
            ...s16(force16(e.offset || 0, gain)),
            ...be16(ms16(e.period || 0)),
            ...be16(0), // phase: no scale known for this wheel, starts at 0
            ...envelope,
        ];
    }
    // Conditions: spring, damper, friction, inertia. Left is the negative side.
    const c = e.condition || {};
    return [
        ...head,
        ...be16(level15(c.negativeSaturation ?? 10000)),
        ...s16(force16(c.negativeCoefficient || 0, gain)),
        ...be16(level15(c.deadBand || 0)),
        ...s16(force16(c.offset || 0)),
        ...s16(force16(c.positiveCoefficient || 0, gain)),
        ...be16(level15(c.positiveSaturation ?? 10000)),
    ];
}

/**
 * The `hidffb` operations of one recreated wheel, played on the real one.
 * `onError` hears about a failed command (logged by the caller, never thrown).
 */
export class HidppFfbPlayer {
    constructor(transport, featureIndex, { onError = null } = {}) {
        this._t = transport;
        this._index = featureIndex;
        this._onError = onError;
        this._effects = new Map(); // game effect block -> state
        this._queue = []; // { key, run }
        this._busy = false;
        this._closed = false;
        this._frees = 0;
    }

    /** Clears the wheel: no effect, and its own centring spring cancelled. */
    async init() {
        await this._t.call(this._index, fn.resetAll);
        await this._freeWheel();
        await this._t.call(this._index, fn.setGains, [0xff, 0xff, 0, 0]);
    }

    /** One `hidffb` message from the host. */
    apply(msg) {
        if (this._closed || !msg) return;
        const id = msg.effect | 0;
        const e = id ? this._effect(id) : null;
        switch (msg.op) {
            case 'effect':
                Object.assign(e, {
                    kind: msg.kind,
                    duration: msg.duration,
                    delay: msg.delay,
                    gain: msg.gain,
                    direction: msg.direction,
                    directionEnable: msg.directionEnable,
                    axes: msg.axes,
                });
                this._upload(id);
                break;
            case 'envelope':
                e.envelope = msg;
                this._upload(id);
                break;
            case 'condition':
                // One condition per axis: the wheel has one, the first.
                if ((msg.axis | 0) === 0) e.condition = msg;
                this._upload(id);
                break;
            case 'periodic':
                Object.assign(e, {
                    magnitude: msg.magnitude,
                    offset: msg.offset,
                    period: msg.period,
                });
                this._upload(id);
                break;
            case 'constant':
                e.magnitude = msg.magnitude;
                this._upload(id);
                break;
            case 'ramp':
                Object.assign(e, { start: msg.start, end: msg.end });
                this._upload(id);
                break;
            case 'solo':
                for (const [other, o] of this._effects)
                    if (other !== id && o.playing) this._setPlaying(other, false);
                this._setPlaying(id, true);
                break;
            case 'start':
                this._setPlaying(id, true);
                break;
            case 'stop':
                this._setPlaying(id, false);
                break;
            case 'free':
                // Out of the table at once: the game may reuse the block for a
                // new effect before the wheel has answered. The slot is read
                // when the task runs, after any upload still in flight set it.
                this._effects.delete(id);
                this._enqueue(`free:${id}:${++this._frees}`, async () => {
                    if (e.slot) await this._t.call(this._index, fn.destroy, [e.slot]);
                });
                break;
            case 'gain': {
                const g = clamp(Math.round(((msg.gain | 0) * 0xffff) / 255), 0, 0xffff);
                this._enqueue('gain', () =>
                    this._t.call(this._index, fn.setGains, [...be16(g), 0, 0]),
                );
                break;
            }
            case 'control':
                if (msg.kind === 'reset' || msg.kind === 'stopAll' || msg.kind === 'disable') {
                    if (msg.kind === 'reset') this._effects.clear();
                    else for (const o of this._effects.values()) o.playing = false;
                    this._enqueue('reset', async () => {
                        await this._t.call(this._index, fn.resetAll);
                        for (const o of this._effects.values()) o.slot = 0;
                        await this._freeWheel();
                    });
                } else if (msg.kind === 'pause') {
                    for (const [other, o] of this._effects)
                        if (o.playing) this._state(other, STATE_STOP);
                } else if (msg.kind === 'continue') {
                    for (const [other, o] of this._effects)
                        if (o.playing) this._state(other, STATE_PLAY);
                }
                break;
            default:
                break;
        }
    }

    /** Leaves the wheel free and silent: the stream ended or the device left. */
    async close() {
        if (this._closed) return;
        this._closed = true;
        this._queue = [];
        try {
            await this._t.call(this._index, fn.resetAll);
            await this._freeWheel();
        } catch (err) {
            this._error(err);
        }
    }

    _effect(id) {
        let e = this._effects.get(id);
        if (!e) {
            e = { slot: 0, playing: false };
            this._effects.set(id, e);
        }
        return e;
    }

    // A zero spring, started: the firmware's own centring survives a reset.
    _freeWheel() {
        return this._t.call(this._index, fn.download, [
            0,
            TYPES.spring | AUTOSTART,
            ...new Array(16).fill(0),
        ]);
    }

    _upload(id) {
        const e = this._effects.get(id);
        // Parameters before the header (or for a wheel slot not made yet and
        // not playing) wait for the effect to start.
        if (!e || !e.kind || (!e.slot && !e.playing)) return;
        this._enqueue(`up:${id}`, () => this._send(id));
    }

    async _send(id) {
        const e = this._effects.get(id);
        if (!e || !e.kind) return;
        const p = effectParams(e);
        if (!p) return;
        p[0] |= e.playing ? AUTOSTART : 0;
        const r = await this._t.call(this._index, fn.download, [e.slot || 0, ...p]);
        if (!e.slot) e.slot = r[0] || 0;
    }

    _setPlaying(id, on) {
        const e = this._effect(id);
        e.playing = on;
        if (on && !e.slot) this._enqueue(`up:${id}`, () => this._send(id));
        else this._state(id, on ? STATE_PLAY : STATE_STOP);
    }

    // The effect is taken now: a free right after must not lose its stop. Its
    // slot is read when the task runs, after an upload in flight set it.
    _state(id, state) {
        const e = this._effects.get(id);
        if (!e) return;
        this._enqueue(`state:${id}`, async () => {
            if (e.slot) await this._t.call(this._index, fn.setState, [e.slot, state]);
        });
    }

    // Keyed: a task already waiting under the same key is replaced in place,
    // keeping its turn, so the newest parameters go out and nothing piles up.
    _enqueue(key, run) {
        const waiting = this._queue.find((q) => q.key === key);
        if (waiting) waiting.run = run;
        else this._queue.push({ key, run });
        this._pump();
    }

    async _pump() {
        if (this._busy) return;
        this._busy = true;
        while (this._queue.length && !this._closed) {
            const { run } = this._queue.shift();
            try {
                await run();
            } catch (err) {
                this._error(err);
            }
        }
        this._busy = false;
    }

    _error(err) {
        if (this._onError) this._onError(err);
    }
}
