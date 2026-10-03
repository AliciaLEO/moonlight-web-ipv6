/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { StreamView } from '../js/ui/StreamView.js';
import * as RemoteNav from '../js/ui/RemoteNav.js';
import { RemotePointer } from '../js/stream/RemotePointer.js';

/**
 * A TV remote in a stream.
 *
 * Its every key goes to the host, and Back never reaches the page (the TV
 * browser keeps it), so a long press of OK is the way out: it opens the
 * stream's own menu, and nothing reaches the host while that menu is up. A
 * short press must still be an Enter for the host.
 */

const P = StreamView.prototype;

/** The keyboard path of a StreamView, and what the menu touches. */
function view(overrides = {}) {
    const sent = [];
    const pad = {
        paused: false,
        setPaused(p) {
            this.paused = p;
        },
    };
    return {
        sent,
        pad,
        webrtc: { send: (m) => sent.push(m) },
        _heldPhysKeys: new Map(),
        _heldMouseButtons: new Set(),
        _metaTapCodes: new Set(),
        _swallowKeyUpCodes: new Set(),
        _appleKeyboard: false,
        _quitting: false,
        _manualQuitting: false,
        _pendingClipboardWrite: null,
        _kbdCapture: null,
        _locksSynced: true,
        _cssFullscreen: false,
        _clipboardEnabled: false,
        _layoutMap: null,
        _gamingMode: false,
        _pendingPasteKey: null,
        _suppressPasteKeyUpCode: null,
        _remoteMenu: null,
        _okHold: null,
        _remoteMenuClosedAt: 0,
        _padHolds: new Set(),
        _gamepadManager: pad,
        _rootEl: document.body,
        _showPerfStats: false,
        _statsClosed: false,
        _overlayEl: null,
        _updateOverlay() {},
        _handleManualQuit: vi.fn(),
        handleKeyDown: P.handleKeyDown,
        handleKeyUp: P.handleKeyUp,
        _sendKeyEvent: P._sendKeyEvent,
        _reconcileModifiers: P._reconcileModifiers,
        _forgetHeldKey: P._forgetHeldKey,
        _holdsThroughStall: P._holdsThroughStall,
        _releaseAllPhysKeys: P._releaseAllPhysKeys,
        _remoteOkDown: P._remoteOkDown,
        _remoteOkUp: P._remoteOkUp,
        _openRemoteMenu: P._openRemoteMenu,
        _closeRemoteMenu: P._closeRemoteMenu,
        _toggleStatsFromMenu: P._toggleStatsFromMenu,
        _closeOverlayEl: P._closeOverlayEl,
        _holdPads: P._holdPads,
        _releasePads: P._releasePads,
        // The menu's "Mouse".
        _remotePointerOn: false,
        _remotePointer: null,
        _wheelAccum: 0,
        _wheelAccumX: 0,
        _sendToHost: (m) => sent.push(m),
        _sendMouseButton: (button, down) =>
            sent.push({ type: down ? 'mousedown' : 'mouseup', button }),
        _clientCursorSteers: () => false,
        _sendWheel: P._sendWheel,
        _remoteArrow: P._remoteArrow,
        _remotePointerActive: P._remotePointerActive,
        _remotePointerKeyDown: P._remotePointerKeyDown,
        _remotePointerKeyUp: P._remotePointerKeyUp,
        _toggleRemotePointer: P._toggleRemotePointer,
        _pointerOfRemote: P._pointerOfRemote,
        _remotePointerMove: P._remotePointerMove,
        ...overrides,
    };
}

function ev(code, key, mods = {}) {
    return {
        target: document.body,
        code,
        key,
        keyCode: 0,
        repeat: false,
        ctrlKey: false,
        shiftKey: false,
        altKey: false,
        metaKey: false,
        preventDefault() {},
        ...mods,
    };
}

/** A remote's OK: Enter, with no code. */
const ok = (mods) => ev('', 'Enter', { keyCode: 13, ...mods });
const menuEl = () => document.querySelector('.stream-remote-menu');
const act = (name) => menuEl().querySelector(`[data-act="${name}"]`).click();

describe('a TV remote in a stream', () => {
    beforeEach(() => {
        vi.useFakeTimers();
        RemoteNav._setActiveForTest(true);
    });

    afterEach(() => {
        RemoteNav._setActiveForTest(null);
        vi.useRealTimers();
        document.body.innerHTML = '';
    });

    it('turns a short OK into an Enter press and release for the host', () => {
        const v = view();
        v.handleKeyDown(ok());
        expect(v.sent).toEqual([]);
        v.handleKeyUp(ok());
        expect(v.sent.map((m) => [m.type, m.keyCode])).toEqual([
            ['keydown', 13],
            ['keyup', 13],
        ]);
        expect(menuEl()).toBe(null);
    });

    it('opens the menu on OK held, and the host hears nothing of it', () => {
        const v = view();
        v.handleKeyDown(ok());
        v.handleKeyDown(ok({ repeat: true }));
        vi.advanceTimersByTime(StreamView.REMOTE_MENU_HOLD_MS);
        expect(menuEl()).not.toBe(null);
        expect(v.pad.paused).toBe(true);
        v.handleKeyUp(ok());
        expect(v.sent).toEqual([]);
    });

    it("does not let the held OK's repeats press the menu's first button", () => {
        // Android repeats a held key without the repeat flag, and the menu's
        // Resume takes the focus as it opens: the next repeat landed on it.
        const v = view();
        v.handleKeyDown(ok());
        vi.advanceTimersByTime(StreamView.REMOTE_MENU_HOLD_MS);
        const resume = menuEl().querySelector('[data-act="resume"]');
        let pressed = 0;
        const repeat = ok({ target: resume, preventDefault: () => pressed-- });
        v.handleKeyDown(repeat);
        v.handleKeyDown(repeat);
        expect(pressed).toBe(-2); // both default actions (the button's press) cancelled
        v.handleKeyUp(ok({ target: resume }));
        expect(menuEl()).not.toBe(null);
        expect(v._okHold).toBe(null);
        expect(v.sent).toEqual([]);
    });

    it('lets go of what was held on the host when the menu opens', () => {
        const v = view();
        v.handleKeyDown(ev('KeyW', 'w', { keyCode: 0x57 }));
        v.handleKeyDown(ok());
        vi.advanceTimersByTime(StreamView.REMOTE_MENU_HOLD_MS);
        expect(v.sent.map((m) => [m.type, m.code])).toEqual([
            ['keydown', 'KeyW'],
            ['keyup', 'KeyW'],
        ]);
    });

    it('sends no key at all while the menu is up', () => {
        const v = view();
        v._openRemoteMenu();
        v.handleKeyDown(ev('ArrowDown', 'ArrowDown', { keyCode: 40 }));
        v.handleKeyUp(ev('ArrowDown', 'ArrowDown', { keyCode: 40 }));
        expect(v.sent).toEqual([]);
    });

    it('resumes: the menu goes, the pads come back, the stray release of its OK stays here', () => {
        const v = view();
        v._openRemoteMenu();
        act('resume');
        expect(menuEl()).toBe(null);
        expect(v.pad.paused).toBe(false);
        v.handleKeyUp(ok());
        expect(v.sent).toEqual([]);
    });

    it('keeps the release of the OK that chose Stop here', () => {
        // "Leave" in the guests question, minutes after the menu closed: the
        // stream is going, and the host never had that key down.
        const v = view({ _manualQuitting: true, _remoteMenuClosedAt: -1e9 });
        v.handleKeyUp(ok());
        expect(v.sent).toEqual([]);
    });

    it('stops through the normal Stop, guests question included', () => {
        const v = view();
        v._openRemoteMenu();
        act('stop');
        expect(v._handleManualQuit).toHaveBeenCalledTimes(1);
        expect(menuEl()).toBe(null);
    });

    it('turns the stats card on and off', () => {
        const overlay = document.createElement('div');
        const v = view({ _overlayEl: overlay });
        v._openRemoteMenu();
        act('stats');
        expect(v._showPerfStats && !v._statsClosed).toBe(true);
        act('stats');
        expect(v._statsClosed).toBe(true);
    });

    it('shows the stats card again on a third press', () => {
        // Hidden by the second press, the card must come back: the refresh
        // tick never unhides it on its own.
        const overlay = document.createElement('div');
        const v = view({
            _overlayEl: overlay,
            _firstFrameRendered: true,
            _positionStatsOverlay: vi.fn(),
        });
        v._openRemoteMenu();
        act('stats');
        act('stats');
        expect(overlay.style.display).toBe('none');
        act('stats');
        expect(overlay.style.display).toBe('');
        expect(v._positionStatsOverlay).toHaveBeenCalled();
        expect(menuEl().querySelector('[data-act="stats"]').getAttribute('aria-pressed')).toBe(
            'true',
        );
    });

    it('keeps the pads held for the remap dialog when the menu closes', () => {
        const v = view();
        v._holdPads('remap');
        v._openRemoteMenu();
        v._closeRemoteMenu();
        expect(v.pad.paused).toBe(true);
        v._releasePads('remap');
        expect(v.pad.paused).toBe(false);
    });
});

describe('keys the host has no code for', () => {
    it('are not forwarded, whatever the device', () => {
        const v = view();
        v.handleKeyDown(ev('', 'Unidentified', { keyCode: 0 }));
        v.handleKeyDown(ev('', 'Process', { keyCode: 229 }));
        v.handleKeyUp(ev('', 'Unidentified', { keyCode: 0 }));
        expect(v.sent).toEqual([]);
    });
});

describe('an Enter with no code on a desktop', () => {
    it('goes to the host at once, as before', () => {
        const v = view();
        v.handleKeyDown(ok());
        expect(v.sent.map((m) => [m.type, m.keyCode])).toEqual([['keydown', 13]]);
    });
});

describe('the arrows of a remote that reaches the page as a pad', () => {
    it('press, repeat and release the arrow keys on the host', () => {
        const v = view({ _remoteArrow: P._remoteArrow });
        v._remoteArrow('down', true, false);
        v._remoteArrow('down', true, true);
        v._remoteArrow('down', false, false);
        expect(v.sent.map((m) => [m.type, m.code, m.keyCode])).toEqual([
            ['keydown', 'ArrowDown', 0x28],
            ['keydown', 'ArrowDown', 0x28],
            ['keyup', 'ArrowDown', 0x28],
        ]);
    });

    it('send no release for an arrow the host never got', () => {
        const v = view({ _remoteArrow: P._remoteArrow });
        v._remoteArrow('left', false, false);
        v._remoteArrow('left', true, true);
        expect(v.sent).toEqual([]);
    });
});

describe("the menu's Mouse: a remote steering the host's pointer", () => {
    beforeEach(() => {
        vi.useFakeTimers();
        RemoteNav._setActiveForTest(true);
        localStorage.removeItem('mw_remote_pointer');
    });

    afterEach(() => {
        RemoteNav._setActiveForTest(null);
        vi.useRealTimers();
        document.body.innerHTML = '';
    });

    /** A view in mouse mode, its pointer on the fake clock. */
    function mouseView() {
        const v = view({ _remotePointerOn: true });
        v._remotePointer = new RemotePointer({
            move: (dx, dy) => v._remotePointerMove(dx, dy),
            now: () => Date.now(),
            schedule: (cb) => setTimeout(cb, 16),
            cancel: (id) => clearTimeout(id),
        });
        return v;
    }
    const moves = (v) => v.sent.filter((m) => m.type === 'mousemove');

    it('an arrow moves the pointer, held it keeps moving, and no key reaches the host', () => {
        const v = mouseView();
        v.handleKeyDown(ev('ArrowRight', 'ArrowRight', { keyCode: 0x27 }));
        expect(moves(v)).toEqual([{ type: 'mousemove', dx: 4, dy: 0 }]);
        vi.advanceTimersByTime(160);
        const n = moves(v).length;
        expect(n).toBeGreaterThan(3);
        v.handleKeyUp(ev('ArrowRight', 'ArrowRight', { keyCode: 0x27 }));
        vi.advanceTimersByTime(160);
        expect(moves(v)).toHaveLength(n);
        expect(v.sent.filter((m) => m.type === 'keydown' || m.type === 'keyup')).toEqual([]);
    });

    it('OK clicks where the pointer is, instead of an Enter', () => {
        const v = mouseView();
        v.handleKeyDown(ok());
        v.handleKeyUp(ok());
        expect(v.sent).toEqual([
            { type: 'mousedown', button: 1 },
            { type: 'mouseup', button: 1 },
        ]);
    });

    it('OK held still opens the menu', () => {
        const v = mouseView();
        v.handleKeyDown(ok());
        vi.advanceTimersByTime(StreamView.REMOTE_MENU_HOLD_MS);
        expect(menuEl()).not.toBe(null);
        v.handleKeyUp(ok());
        expect(v.sent).toEqual([]);
    });

    it('Ch+ and Ch− scroll the wheel', () => {
        const v = mouseView();
        v.handleKeyDown(ev('', 'ChannelUp', { keyCode: 33 }));
        v.handleKeyUp(ev('', 'ChannelUp', { keyCode: 33 }));
        v.handleKeyDown(ev('', 'ChannelDown', { keyCode: 34 }));
        expect(v.sent).toEqual([
            { type: 'mousewheel', delta: 120 },
            { type: 'mousewheel', delta: -120 },
        ]);
    });

    it("a pad remote's arrows steer it too", () => {
        const v = mouseView();
        v._remoteArrow('down', true, false);
        v._remoteArrow('down', true, true);
        expect(moves(v)).toEqual([{ type: 'mousemove', dx: 0, dy: 4 }]);
        v._remoteArrow('down', false, false);
        expect(v._remotePointer.moving).toBe(false);
        expect(v.sent.filter((m) => m.type === 'keydown')).toEqual([]);
    });

    it('the menu turns it on — kept by the device — and goes back to the stream', () => {
        const v = view();
        v._openRemoteMenu();
        const btn = menuEl().querySelector('[data-act="pointer"]');
        expect(btn.getAttribute('aria-pressed')).toBe('false');
        btn.click();
        expect(v._remotePointerOn).toBe(true);
        expect(localStorage.getItem('mw_remote_pointer')).toBe('1');
        expect(menuEl()).toBe(null);
        expect(StreamView.readRemotePointerPref()).toBe(true);
    });

    it('turned off, the arrows are arrows on the host again', () => {
        const v = mouseView();
        v._toggleRemotePointer();
        v.handleKeyDown(ev('ArrowLeft', 'ArrowLeft', { keyCode: 0x25 }));
        expect(v.sent.map((m) => [m.type, m.code])).toEqual([['keydown', 'ArrowLeft']]);
        expect(localStorage.getItem('mw_remote_pointer')).toBe('0');
    });
});
