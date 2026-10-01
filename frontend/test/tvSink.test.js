/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi, afterEach } from 'vitest';
import { StreamView } from '../js/ui/StreamView.js';
import { tvPresentsThroughSink } from '../js/stream/renderers/videoSink.js';

/**
 * A TV presents through the <video> sink, fed on decode: a Freebox Player POP
 * showed 49.6 frames a second that way where its canvas showed 28 (01/10/2026).
 */
describe('tvPresentsThroughSink', () => {
    const tv = { tv: true, hdr: false, algo: 'off' };

    it('takes the sink on a TV, SDR, with no enhancer', () => {
        expect(tvPresentsThroughSink(tv)).toBe(true);
    });

    it('leaves every other screen, and every other choice, as it was', () => {
        expect(tvPresentsThroughSink({ ...tv, tv: false })).toBe(false);
        // HDR has its own routing; an enhancer needs a shader stage.
        expect(tvPresentsThroughSink({ ...tv, hdr: true })).toBe(false);
        expect(tvPresentsThroughSink({ ...tv, algo: 'gl-sgsr' })).toBe(false);
        // A bench reaches the canvas still.
        expect(tvPresentsThroughSink({ ...tv, forceCanvas2d: true })).toBe(false);
        expect(tvPresentsThroughSink({ ...tv, devCanvas2d: true })).toBe(false);
    });
});

describe('StreamView.startRenderLoop — what paces the frames', () => {
    afterEach(() => vi.useRealTimers());

    function loopOf(over) {
        vi.useFakeTimers();
        const v = { _useWorker: false, _tearing: false, renderRunning: false, ...over };
        StreamView.prototype.startRenderLoop.call(v);
        v.renderRunning = false; // the rAF loop stops at its next tick
        return v;
    }

    it("feeds a TV's sink as frames are decoded: the compositor paces the <video>", () => {
        expect(loopOf({ _tvSink: true })._immediateRender).toBe(true);
    });

    it('keeps a canvas on the rAF, and tearing on decode, as before', () => {
        expect(loopOf({})._immediateRender).toBe(false);
        expect(loopOf({ _tvSink: false })._immediateRender).toBe(false);
        expect(loopOf({ _tearing: true })._immediateRender).toBe(true);
    });
});
