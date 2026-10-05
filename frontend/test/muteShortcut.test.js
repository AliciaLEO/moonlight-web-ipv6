/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi } from 'vitest';
import { StreamView } from '../js/ui/StreamView.js';

/**
 * Ctrl+Alt+Shift+S (Cmd+Option+Ctrl+S on a Mac client) turns the stream's
 * sound off and back on, like the volume button — issue #29. Like every
 * control combo, it never reaches the host as a keystroke.
 */
function keySink() {
    const sent = [];
    return {
        sent,
        host: null,
        webrtc: { send: (m) => sent.push(m) },
        _heldPhysKeys: new Map(),
        _metaTapCodes: new Set(),
        _swallowKeyUpCodes: new Set(),
        _appleKeyboard: false,
        _quitting: false,
        _kbdCapture: null,
        _locksSynced: true,
        _cssFullscreen: false,
        _clipboardEnabled: false,
        _layoutMap: null,
        _gamingMode: false,
        _pendingPasteKey: null,
        _suppressPasteKeyUpCode: null,
        toggleMute: vi.fn(() => true),
        handleKeyDown: StreamView.prototype.handleKeyDown,
        _sendKeyEvent: StreamView.prototype._sendKeyEvent,
        _reconcileModifiers: StreamView.prototype._reconcileModifiers,
        _forgetHeldKey: StreamView.prototype._forgetHeldKey,
        _holdsThroughStall: StreamView.prototype._holdsThroughStall,
        _releaseKeysHeldUnderMeta: StreamView.prototype._releaseKeysHeldUnderMeta,
        _sendPendingPasteKey: StreamView.prototype._sendPendingPasteKey,
        _syncLockState: () => {},
    };
}

function ev(code, key, mods = {}) {
    return {
        target: {},
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

const winCombo = { ctrlKey: true, altKey: true, shiftKey: true };

describe('the sound combo', () => {
    it('toggles the sound and sends the host nothing', () => {
        const v = keySink();
        v.handleKeyDown(ev('KeyS', 'S', winCombo));
        expect(v.toggleMute).toHaveBeenCalledTimes(1);
        expect(v.sent.filter((m) => m.type === 'keydown' && m.keyCode === 0x53)).toEqual([]);
    });

    it('toggles once for a held key, not once per auto-repeat', () => {
        const v = keySink();
        v.handleKeyDown(ev('KeyS', 'S', winCombo));
        v.handleKeyDown(ev('KeyS', 'S', { ...winCombo, repeat: true }));
        v.handleKeyDown(ev('KeyS', 'S', { ...winCombo, repeat: true }));
        expect(v.toggleMute).toHaveBeenCalledTimes(1);
    });

    it('leaves a plain S to the host', () => {
        const v = keySink();
        v.handleKeyDown(ev('KeyS', 's'));
        expect(v.toggleMute).not.toHaveBeenCalled();
    });
});
