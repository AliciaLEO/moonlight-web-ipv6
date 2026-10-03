/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { StreamView } from '../js/ui/StreamView.js';

/**
 * Leaving a stream always gets the page back.
 *
 * 02/10/2026, a Mi TV at 1080p60: the "Disconnecting" screen stayed up for
 * good. quit() had no finally — a throw in its teardown skipped destroy(),
 * which is what removes that screen, while the keys were already unbound and
 * every later quit() stopped at its re-entry guard.
 */

const P = StreamView.prototype;

function view(overrides = {}) {
    const root = document.createElement('div');
    root.className = 'stream-overlay';
    document.body.appendChild(root);
    document.body.classList.add('streaming-active');
    // onQuit fires once and is cleared: the spy is kept aside to be read.
    const quitSpy = vi.fn();
    return {
        quitSpy,
        _rootEl: root,
        _quitting: false,
        _quitFinished: false,
        _manualQuitting: false,
        _takenOver: false,
        onQuit: quitSpy,
        destroy: vi.fn(function () {
            this._rootEl.remove();
            document.body.classList.remove('streaming-active');
        }),
        hasActiveShare: () => false,
        _quitAppOnStop: false,
        _playPowerOff: (done) => done(),
        quit: P.quit,
        _quitTeardown: vi.fn(async () => {}),
        _quitDone: P._quitDone,
        _handleManualQuit: P._handleManualQuit,
        ...overrides,
    };
}

beforeEach(() => {
    document.body.innerHTML = '';
    document.body.className = '';
});

afterEach(() => {
    vi.useRealTimers();
});

describe('quit() always hands the page back', () => {
    it('tears down, then destroys and calls onQuit once', async () => {
        const v = view();
        await v.quit();
        expect(v._quitTeardown).toHaveBeenCalledTimes(1);
        expect(v.destroy).toHaveBeenCalledTimes(1);
        expect(v.quitSpy).toHaveBeenCalledTimes(1);
        expect(document.querySelector('.stream-overlay')).toBeNull();
    });

    it('a teardown that throws still removes the stream and calls onQuit', async () => {
        const err = vi.spyOn(console, 'error').mockImplementation(() => {});
        const v = view({
            _quitTeardown: vi.fn(async () => {
                throw new Error('renderer gone');
            }),
        });
        await v.quit();
        expect(v.destroy).toHaveBeenCalledTimes(1);
        expect(v.quitSpy).toHaveBeenCalledTimes(1);
        expect(document.body.classList.contains('streaming-active')).toBe(false);
        err.mockRestore();
    });

    it('a destroy() that throws still takes the screen away', async () => {
        const err = vi.spyOn(console, 'error').mockImplementation(() => {});
        const v = view({
            destroy: vi.fn(() => {
                throw new Error('half torn');
            }),
        });
        await v.quit();
        expect(document.querySelector('.stream-overlay')).toBeNull();
        expect(document.body.classList.contains('streaming-active')).toBe(false);
        expect(v.quitSpy).toHaveBeenCalledTimes(1);
        err.mockRestore();
    });

    it('a second quit() does nothing', async () => {
        const v = view();
        await v.quit();
        await v.quit();
        expect(v._quitTeardown).toHaveBeenCalledTimes(1);
        expect(v.quitSpy).toHaveBeenCalledTimes(1);
    });

    it('passes the options through to the teardown', async () => {
        const v = view();
        await v.quit({ silent: true, quitApp: true });
        expect(v._quitTeardown).toHaveBeenCalledWith({
            silent: true,
            takenOver: false,
            retire: false,
            keepHostSession: false,
            quitApp: true,
        });
    });
});

describe('the exit screen has a watchdog', () => {
    it('a quit() stuck forever is overtaken: the page goes back to the list', async () => {
        vi.useFakeTimers();
        const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
        const v = view({ quit: vi.fn(() => new Promise(() => {})) });
        v._handleManualQuit();
        expect(document.querySelector('.stream-takeover-overlay.is-quit')).not.toBeNull();
        await vi.advanceTimersByTimeAsync(1200);
        expect(v.quit).toHaveBeenCalledTimes(1);
        expect(v.quitSpy).not.toHaveBeenCalled();
        await vi.advanceTimersByTimeAsync(StreamView.QUIT_WATCHDOG_MS);
        expect(v.destroy).toHaveBeenCalledTimes(1);
        expect(v.quitSpy).toHaveBeenCalledTimes(1);
        expect(document.querySelector('.stream-takeover-overlay')).toBeNull();
        warn.mockRestore();
    });

    it('a quit that finishes in time disarms it', async () => {
        vi.useFakeTimers();
        const v = view();
        v._handleManualQuit();
        await vi.advanceTimersByTimeAsync(1200);
        expect(v.quitSpy).toHaveBeenCalledTimes(1);
        await vi.advanceTimersByTimeAsync(StreamView.QUIT_WATCHDOG_MS);
        expect(v.destroy).toHaveBeenCalledTimes(1);
        expect(v.quitSpy).toHaveBeenCalledTimes(1);
    });
});
