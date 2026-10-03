/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi, afterEach } from 'vitest';
import { StreamView, firedByTouch } from '../js/ui/StreamView.js';

/**
 * A TV browser's cursor in a stream.
 *
 * TV Bro and TCL Browser draw their own cursor and tap where it points. As a
 * trackpad tap that clicked wherever the host's pointer already was (03/10/2026,
 * a Mi TV: pointer at the top, click at the bottom). A TV streams in the
 * touch-screen model instead: the tap moves the host's pointer to the point,
 * then clicks there.
 */

const P = StreamView.prototype;

function view(touchScreen) {
    const sent = [];
    const buttons = [];
    return {
        sent,
        buttons,
        _touchScreen: touchScreen,
        _scrollSamples: [],
        _panSamples: [],
        _touchLongPressMs: 500,
        _touchTapThreshold: 15,
        _touchTapTimeThreshold: 300,
        _touchActive: false,
        webrtc: { send: (m) => sent.push(m) },
        _mediaRect: () => ({ left: 0, top: 0, width: 1280, height: 720 }),
        _clientCursorPlacedAt() {},
        _stopScrollMomentum() {},
        _stopPanMomentum() {},
        _startScrollMomentum() {},
        _sendMouseButton: (b, down) => buttons.push([b, down]),
        handleTouchStart: P.handleTouchStart,
        handleTouchEnd: P.handleTouchEnd,
        _sendAbsTouch: P._sendAbsTouch,
        _clearLongPress: P._clearLongPress,
    };
}

function touch(type, x, y, left) {
    const t = { clientX: x, clientY: y };
    return {
        type,
        target: document.body,
        touches: left ? [t] : [],
        changedTouches: [t],
        preventDefault() {},
    };
}

function tap(v, x, y) {
    v.handleTouchStart(touch('touchstart', x, y, true));
    v.handleTouchEnd(touch('touchend', x, y, false));
}

afterEach(() => {
    vi.useRealTimers();
});

describe('a TV cursor tap in a stream', () => {
    it('touch-screen model: the pointer goes to the tap, then the click', () => {
        const v = view(true);
        tap(v, 900, 600);
        expect(v.sent).toEqual([
            { type: 'mousemove', x: 900, y: 600, referenceWidth: 1280, referenceHeight: 720 },
        ]);
        expect(v.buttons).toEqual([
            [1, true],
            [1, false],
        ]);
    });

    it('trackpad model (a phone without the option): the click alone, where the pointer is', () => {
        const v = view(false);
        tap(v, 900, 600);
        expect(v.sent).toEqual([]);
        expect(v.buttons).toEqual([
            [1, true],
            [1, false],
        ]);
    });
});

describe('firedByTouch', () => {
    it('is true for the mouse events a browser makes up after a tap', () => {
        expect(firedByTouch({ sourceCapabilities: { firesTouchEvents: true } })).toBe(true);
    });

    it('is false for a real mouse, or a browser that does not say', () => {
        expect(firedByTouch({ sourceCapabilities: { firesTouchEvents: false } })).toBe(false);
        expect(firedByTouch({ sourceCapabilities: null })).toBe(false);
        expect(firedByTouch({})).toBe(false);
        expect(firedByTouch(null)).toBe(false);
    });
});
