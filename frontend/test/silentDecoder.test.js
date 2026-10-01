/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { StreamView } from '../js/ui/StreamView.js';
import { SilentDecoderWatch } from '../js/stream/SilentDecoderWatch.js';
import { decoderRidesOutGaps, decoderTakesReferenceRepairs } from '../js/util/BrowserDetect.js';

/**
 * A decoder gone silent under the refresh wave (Freebox Player POP, 01/10/2026):
 * every chunk taken, no picture back, no error. The stream sees it on its
 * counters, the device keeps the verdict, and the session comes back on
 * keyframes — the same app, the same codec, relaunched as a codec fallback is.
 */
const P = StreamView.prototype;

function view(over = {}) {
    return {
        // A decoder that worked: it gave pictures before going silent.
        stats: { received: 0, decoded: 40, recoveries: 0 },
        webrtc: { rideOutLoss: true },
        videoCodec: 'hevc',
        _hdrEnabled: false,
        _playerMode: false,
        _standby: false,
        _videoWorker: null,
        _quitting: false,
        _manualQuitting: false,
        _codecFallbackRequested: false,
        _codecFallback: null,
        _decoderRecovering: false,
        _silentWatch: new SilentDecoderWatch(),
        quit: vi.fn(),
        _requestIdr: vi.fn(),
        _handleDecoderError: vi.fn(),
        _checkSilentDecoder: P._checkSilentDecoder,
        _relaunchOnKeyframes: P._relaunchOnKeyframes,
        ...over,
    };
}

/** Two seconds of a decoder that takes 50 chunks a second and gives nothing back. */
function goSilent(v) {
    for (let t = 0; t <= 2000; t += 500) {
        v.stats.received = 100 + t / 20;
        v._checkSilentDecoder(t);
    }
}

describe('StreamView — a decoder gone silent', () => {
    beforeEach(() => {
        localStorage.removeItem('mw_ride_out');
        localStorage.removeItem('mw_ref_repairs');
    });
    afterEach(() => {
        localStorage.removeItem('mw_ride_out');
        localStorage.removeItem('mw_ref_repairs');
        vi.unstubAllGlobals();
    });

    it('under the wave: keeps the verdict and comes back on keyframes, same app, same codec', () => {
        const v = view({ stats: { received: 100, decoded: 71 } });
        goSilent(v);
        expect(decoderRidesOutGaps('Linux; Android 10')).toBe(false);
        expect(v._codecFallbackRequested).toBe(true);
        expect(v._codecFallback).toEqual({ codec: 'hevc', hdr: false, reason: 'silent-decoder' });
        expect(v.quit).toHaveBeenCalledOnce();
        expect(v._handleDecoderError).not.toHaveBeenCalled();
    });

    it('reads Auto as the HEVC it resolved to, and keeps HDR as it was', () => {
        const v = view({ videoCodec: 'auto', _hdrEnabled: true });
        goSilent(v);
        expect(v._codecFallback).toEqual({ codec: 'hevc', hdr: true, reason: 'silent-decoder' });
    });

    it("a guest keeps the verdict but stays: the feed and its wave are the owner's", () => {
        const v = view({ _playerMode: true });
        goSilent(v);
        expect(decoderRidesOutGaps('Linux; Android 10')).toBe(false);
        expect(v.quit).not.toHaveBeenCalled();
        expect(v._handleDecoderError).toHaveBeenCalledOnce();
    });

    it("on keyframes, under the host's reference repairs: the second verdict, and back again", () => {
        // The Freebox at 30 fps: AMF marks a long-term reference every frame.
        const v = view({ webrtc: { rideOutLoss: false }, _refInvalidation: true });
        goSilent(v);
        expect(decoderTakesReferenceRepairs()).toBe(false);
        expect(decoderRidesOutGaps('Linux; Android 10')).toBe(true);
        expect(v._codecFallback).toEqual({ codec: 'hevc', hdr: false, reason: 'silent-decoder' });
        expect(v.quit).toHaveBeenCalledOnce();
        expect(v._handleDecoderError).not.toHaveBeenCalled();
    });

    it('under the wave, the wave alone is blamed: one verdict a relaunch', () => {
        const v = view({ _refInvalidation: true });
        goSilent(v);
        expect(decoderRidesOutGaps('Linux; Android 10')).toBe(false);
        expect(decoderTakesReferenceRepairs()).toBe(true);
        expect(v.quit).toHaveBeenCalledOnce();
    });

    it('a guest keeps the repairs verdict too, and stays', () => {
        const v = view({
            webrtc: { rideOutLoss: false },
            _refInvalidation: true,
            _playerMode: true,
        });
        goSilent(v);
        expect(decoderTakesReferenceRepairs()).toBe(false);
        expect(v.quit).not.toHaveBeenCalled();
        expect(v._handleDecoderError).toHaveBeenCalledOnce();
    });

    it('on keyframes already: a new decoder on the main thread, a keyframe from the worker', () => {
        const main = view({ webrtc: { rideOutLoss: false } });
        goSilent(main);
        expect(main._handleDecoderError).toHaveBeenCalledOnce();
        expect(main.quit).not.toHaveBeenCalled();
        const worker = view({ webrtc: { rideOutLoss: false }, _videoWorker: {} });
        goSilent(worker);
        expect(worker._requestIdr).toHaveBeenCalledWith('decoder silent');
        expect(worker._handleDecoderError).not.toHaveBeenCalled();
        // Neither says anything about riding out, nor about the repairs: the
        // host made none (NVENC with dpb=1, a GameStream host).
        expect(decoderRidesOutGaps('Linux; Android 10')).toBe(true);
        expect(decoderTakesReferenceRepairs()).toBe(true);
    });

    // The Mi TV's decoder, stuck in "Decoding error" until a reboot (01/10/2026),
    // blamed the wave and the repairs that way: a failing decoder is no verdict.
    it('keeps no verdict for a decoder that failed, or never gave a picture', () => {
        const failed = view({ webrtc: { rideOutLoss: true }, _refInvalidation: true });
        failed._checkSilentDecoder(0);
        failed.stats.recoveries = 3; // errors and recoveries in the stretch
        goSilent(failed);
        const never = view({ stats: { received: 0, decoded: 0, recoveries: 0 } });
        goSilent(never);
        const recovering = view({ _recoveryStartedPerf: 123 });
        goSilent(recovering);
        expect(decoderRidesOutGaps('Linux; Android 10')).toBe(true);
        expect(decoderTakesReferenceRepairs()).toBe(true);
        for (const v of [failed, never, recovering]) {
            expect(v.quit).not.toHaveBeenCalled();
            expect(v._handleDecoderError).toHaveBeenCalledOnce();
        }
    });

    it('says nothing while pictures come out', () => {
        const v = view();
        for (let t = 0; t <= 4000; t += 500) {
            v.stats.received += 25;
            v.stats.decoded += 25;
            v._checkSilentDecoder(t);
        }
        expect(v.quit).not.toHaveBeenCalled();
        expect(v._handleDecoderError).not.toHaveBeenCalled();
    });

    it('counts nothing while the page is hidden, nor on the way out or in a recovery', () => {
        Object.defineProperty(document, 'hidden', { value: true, configurable: true });
        const hidden = view();
        goSilent(hidden);
        expect(hidden.quit).not.toHaveBeenCalled();
        delete (/** @type {any} */ (document).hidden);
        for (const over of [
            { _quitting: true },
            { _manualQuitting: true },
            { _codecFallbackRequested: true },
            { _decoderRecovering: true },
            { _standby: true },
        ]) {
            const v = view(over);
            goSilent(v);
            expect(v.quit).not.toHaveBeenCalled();
            expect(v._handleDecoderError).not.toHaveBeenCalled();
            expect(v._requestIdr).not.toHaveBeenCalled();
        }
    });
});
