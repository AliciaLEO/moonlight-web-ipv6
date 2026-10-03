/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi, afterEach } from 'vitest';
import {
    VideoElementRenderer,
    READY_WAIT_MS,
} from '../js/stream/renderers/VideoElementRenderer.js';

/**
 * The <video> sink of a TV (and of HDR): a sink that stops taking frames must
 * not hold the render path, and letting go of it must free everything.
 */

function frame() {
    return { close: vi.fn() };
}

function renderer({ desiredSize = 1, ready = Promise.resolve() } = {}) {
    const r = new VideoElementRenderer();
    r._disposed = false;
    r._writer = {
        desiredSize,
        ready,
        write: vi.fn(async () => {}),
        abort: vi.fn(async () => {}),
    };
    r._track = { stop: vi.fn() };
    r.videoEl = { pause: vi.fn(), srcObject: {}, paused: false };
    return r;
}

afterEach(() => {
    vi.useRealTimers();
});

describe('VideoElementRenderer.draw', () => {
    it('writes a frame when the sink has room', async () => {
        const r = renderer();
        const f = frame();
        await r.draw(f);
        expect(r._writer.write).toHaveBeenCalledWith(f);
        expect(f.close).not.toHaveBeenCalled();
    });

    it('waits for a sink that is full, then writes', async () => {
        let free;
        const r = renderer({ desiredSize: 0, ready: new Promise((res) => (free = res)) });
        const f = frame();
        const drawn = r.draw(f);
        free();
        await drawn;
        expect(r._writer.write).toHaveBeenCalledWith(f);
    });

    it('a sink that never frees drops the frame instead of holding the draw', async () => {
        vi.useFakeTimers();
        const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
        const r = renderer({ desiredSize: 0, ready: new Promise(() => {}) });
        const f = frame();
        const drawn = r.draw(f);
        await vi.advanceTimersByTimeAsync(READY_WAIT_MS);
        await drawn;
        expect(r._writer.write).not.toHaveBeenCalled();
        expect(f.close).toHaveBeenCalledTimes(1);
        expect(warn).toHaveBeenCalledTimes(1);
        // Said once, not once per frame.
        const g = frame();
        const again = r.draw(g);
        await vi.advanceTimersByTimeAsync(READY_WAIT_MS);
        await again;
        expect(g.close).toHaveBeenCalledTimes(1);
        expect(warn).toHaveBeenCalledTimes(1);
        warn.mockRestore();
    });

    it('a renderer disposed during the wait closes the frame', async () => {
        let free;
        const r = renderer({ desiredSize: 0, ready: new Promise((res) => (free = res)) });
        const f = frame();
        const drawn = r.draw(f);
        r.dispose();
        free();
        await drawn;
        expect(f.close).toHaveBeenCalledTimes(1);
    });
});

describe('VideoElementRenderer.dispose', () => {
    it('stops the <video>, aborts the writer and ends the track', () => {
        const r = renderer();
        const { videoEl, _writer: writer, _track: track } = r;
        r.dispose();
        expect(videoEl.pause).toHaveBeenCalled();
        expect(videoEl.srcObject).toBeNull();
        expect(writer.abort).toHaveBeenCalled();
        expect(track.stop).toHaveBeenCalled();
        expect(r._writer).toBeNull();
    });

    it('a writer whose abort rejects does not throw', async () => {
        const r = renderer();
        r._writer.abort = vi.fn(() => Promise.reject(new Error('errored')));
        expect(() => r.dispose()).not.toThrow();
        await Promise.resolve();
    });

    it('a frame drawn after dispose is closed, not written', async () => {
        const r = renderer();
        const writer = r._writer;
        r.dispose();
        const f = frame();
        await r.draw(f);
        expect(writer.write).not.toHaveBeenCalled();
        expect(f.close).toHaveBeenCalledTimes(1);
    });
});
