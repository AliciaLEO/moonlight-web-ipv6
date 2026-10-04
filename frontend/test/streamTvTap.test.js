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

function view(touchScreen, tvCursor = false) {
    const sent = [];
    const buttons = [];
    return {
        sent,
        buttons,
        _touchScreen: touchScreen,
        _tvCursor: tvCursor,
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
        handleTouchMove: P.handleTouchMove,
        _emitScroll: P._emitScroll,
        _curScrollScale: P._curScrollScale,
        _touchScrollScale: 1,
        _scrollScale: 1,
        _zoom: 1,
        _sendAbsTouch: P._sendAbsTouch,
        _tvTapOffPicture: P._tvTapOffPicture,
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

    it('a TV cursor off the picture neither moves the pointer nor clicks', () => {
        // The cursor on a letterbox bar or the header: pinned to the nearest
        // edge, the pointer leapt to a border and clicked there (Freebox).
        const v = view(true, true);
        v._mediaRect = () => ({ left: 100, top: 80, width: 1280, height: 720 });
        tap(v, 1500, 400); // right of the picture
        tap(v, 600, 20); // above it, over the header
        expect(v.sent).toEqual([]);
        expect(v.buttons).toEqual([]);
        tap(v, 1380, 800); // its very corner is still the picture
        expect(v.sent).toEqual([
            { type: 'mousemove', x: 1280, y: 720, referenceWidth: 1280, referenceHeight: 720 },
        ]);
        expect(v.buttons).toEqual([
            [1, true],
            [1, false],
        ]);
    });

    it('a TV cursor held off the picture does not start a drag', () => {
        vi.useFakeTimers();
        const v = view(true, true);
        v.handleTouchStart(touch('touchstart', 1500, 400, true));
        vi.advanceTimersByTime(600);
        v.handleTouchEnd(touch('touchend', 1500, 400, false));
        expect(v.sent).toEqual([]);
        expect(v.buttons).toEqual([]);
    });

    it("TV Bro's edge scroll, a swipe it fakes, neither moves the pointer nor scrolls", () => {
        // The cursor pushed past the picture's right edge: TV Bro swipes from
        // well inside the page, leftwards (Freebox, 04/10/2026). As a
        // touch-screen scroll it pulled the host's pointer to the swipe's
        // start and scrolled the window there.
        const v = view(true, true);
        v.handleTouchStart(touch('touchstart', 810, 357, true));
        for (let x = 772; x >= 525; x -= 21) v.handleTouchMove(touch('touchmove', x, 357, true));
        v.handleTouchEnd(touch('touchend', 525, 357, false));
        expect(v.sent).toEqual([]);
        expect(v.buttons).toEqual([]);
        // And the next OK on the picture is a tap again.
        tap(v, 634, 314);
        expect(v.sent).toEqual([
            { type: 'mousemove', x: 634, y: 314, referenceWidth: 1280, referenceHeight: 720 },
        ]);
        expect(v.buttons.length).toBe(2);
    });

    it('a touch the browser cancels is no tap', () => {
        // TV Bro held against an edge opens each fake swipe with a touchstart
        // it cancels at once: as taps, a click a dozen times a second.
        const v = view(true, true);
        for (let i = 0; i < 5; i++) {
            v.handleTouchStart(touch('touchstart', 810, 150, true));
            v.handleTouchEnd(touch('touchcancel', 810, 150, false));
        }
        expect(v.sent).toEqual([]);
        expect(v.buttons).toEqual([]);
    });

    it('a finger dragged on a phone still scrolls, from where it landed', () => {
        const v = view(true, false);
        v.handleTouchStart(touch('touchstart', 810, 357, true));
        v.handleTouchMove(touch('touchmove', 770, 357, true));
        expect(v.sent[0]).toEqual({
            type: 'mousemove',
            x: 810,
            y: 357,
            referenceWidth: 1280,
            referenceHeight: 720,
        });
        expect(v.sent.some((m) => m.type === 'mousehwheel')).toBe(true);
    });

    it('a finger off the picture on a phone keeps landing on the nearest edge', () => {
        const v = view(true, false);
        tap(v, 1400, 600);
        expect(v.sent).toEqual([
            { type: 'mousemove', x: 1280, y: 600, referenceWidth: 1280, referenceHeight: 720 },
        ]);
        expect(v.buttons.length).toBe(2);
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
