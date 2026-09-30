/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi } from 'vitest';
import { StreamView } from '../js/ui/StreamView.js';

/**
 * The guests of a native host share one feed. When a guest whose browser
 * decodes no HEVC joins, the whole feed goes H.264 and the one the others
 * watch ends: each of their pages is told (`feedcodec`) and comes back in
 * H.264 exactly as its own codec fallback would — the app's guest onQuit
 * relaunches on the flag.
 */
function guest(over = {}) {
    return {
        _playerMode: true,
        videoCodec: 'hevc',
        _codecFallbackRequested: false,
        _codecFallback: null,
        _quitting: false,
        _manualQuitting: false,
        quit: vi.fn(),
        _followFeedCodec: StreamView.prototype._followFeedCodec,
        ...over,
    };
}

const handle = (view, msg) => StreamView.prototype._handleStatsMessage.call(view, msg);

describe('StreamView — the shared feed changing codec', () => {
    it('comes back in H.264, the way a codec fallback does', () => {
        const v = guest();
        handle(v, { type: 'feedcodec', codec: 'h264' });
        expect(v._codecFallbackRequested).toBe(true);
        expect(v._codecFallback).toEqual({ codec: 'h264', hdr: false });
        expect(v.quit).toHaveBeenCalledOnce();
    });

    it('reads Auto as the HEVC it resolved to', () => {
        const v = guest({ videoCodec: 'auto' });
        handle(v, { type: 'feedcodec', codec: 'h264' });
        expect(v.quit).toHaveBeenCalledOnce();
    });

    it('stays put when already in H.264, or already on its way out', () => {
        for (const over of [
            { videoCodec: 'h264' },
            { _codecFallbackRequested: true },
            { _quitting: true },
            // Stop pressed: a fallback flag now would relaunch what was stopped.
            { _manualQuitting: true },
        ]) {
            const v = guest(over);
            handle(v, { type: 'feedcodec', codec: 'h264' });
            expect(v.quit).not.toHaveBeenCalled();
        }
    });

    it('is a guest message, and names H.264 only', () => {
        const owner = guest({ _playerMode: false });
        handle(owner, { type: 'feedcodec', codec: 'h264' });
        expect(owner.quit).not.toHaveBeenCalled();
        const other = guest();
        handle(other, { type: 'feedcodec', codec: 'av1' });
        expect(other.quit).not.toHaveBeenCalled();
        expect(other._codecFallbackRequested).toBe(false);
    });
});
