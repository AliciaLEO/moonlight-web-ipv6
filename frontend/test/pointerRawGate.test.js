/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import { StreamView } from '../js/ui/StreamView.js';

/**
 * Which hosts get the mouse at the device's own report rate (pointerrawupdate)
 * instead of once a frame: the native host by default, a GameStream host only
 * under the test key mw_raw_pointer = '1' — the A/B for Sunshine and Wolf.
 * '0' holds the native host to the frame cadence, as it always did.
 */
function view({ nativeHost, gaming = true }) {
    const sent = [];
    const v = {
        _nativeHost: nativeHost,
        _gamingMode: gaming,
        _mouseFocused: true,
        _mouseSensitivity: 1,
        _mouseCarryX: 0,
        _mouseCarryY: 0,
        _mainThreadProbe: null,
        inputEl: document.createElement('div'),
        _sendToHost: (msg) => sent.push(msg),
        _sendRelativeMouse: StreamView.prototype._sendRelativeMouse,
        _noteMouseSent: StreamView.prototype._noteMouseSent,
    };
    StreamView.prototype._bindPointerRaw.call(v);
    return { v, sent };
}

/** One raw mouse report, as Chromium dispatches it. */
function rawMove(el, dx, dy) {
    const e = new window.Event('pointerrawupdate');
    Object.assign(e, { pointerType: 'mouse', movementX: dx, movementY: dy });
    el.dispatchEvent(e);
}

describe('mouse at the report rate', () => {
    let log;
    beforeEach(() => {
        // jsdom has no pointerrawupdate; Chromium does.
        window.onpointerrawupdate = null;
        localStorage.removeItem('mw_raw_pointer');
        log = vi.spyOn(console, 'log').mockImplementation(() => {});
    });
    afterEach(() => {
        delete window.onpointerrawupdate;
        localStorage.removeItem('mw_raw_pointer');
        log.mockRestore();
    });

    it('leaves a GameStream host at the frame cadence by default', () => {
        const { v, sent } = view({ nativeHost: false });
        expect(v._rawPointer).toBe(false);
        rawMove(v.inputEl, 3, -2);
        expect(sent).toEqual([]);
    });

    it('gives a GameStream host every report under mw_raw_pointer=1, and says so', () => {
        localStorage.setItem('mw_raw_pointer', '1');
        const { v, sent } = view({ nativeHost: false });
        expect(v._rawPointer).toBe(true);
        rawMove(v.inputEl, 3, -2);
        rawMove(v.inputEl, 1, 0);
        expect(sent).toEqual([
            { type: 'mousemove', dx: 3, dy: -2 },
            { type: 'mousemove', dx: 1, dy: 0 },
        ]);
        expect(log).toHaveBeenCalledWith(
            '[StreamView] Mouse: raw report rate (pointerrawupdate, GameStream host, mw_raw_pointer=1)',
        );
    });

    it('gives the native host every report, unless mw_raw_pointer=0 holds it off', () => {
        expect(view({ nativeHost: true }).v._rawPointer).toBe(true);
        localStorage.setItem('mw_raw_pointer', '0');
        const { v, sent } = view({ nativeHost: true });
        expect(v._rawPointer).toBe(false);
        rawMove(v.inputEl, 3, -2);
        expect(sent).toEqual([]);
    });

    it('keeps the key meaning "frame cadence" for a GameStream host too', () => {
        localStorage.setItem('mw_raw_pointer', '0');
        expect(view({ nativeHost: false }).v._rawPointer).toBe(false);
    });
});
