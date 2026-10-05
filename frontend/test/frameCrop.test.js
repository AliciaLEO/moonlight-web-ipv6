/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi, afterEach } from 'vitest';
import { MAX_PAD, cropRect, cropToAnnounced } from '../js/stream/FrameCrop.js';

/**
 * A decoded AV1 frame padded past its picture (the 780M's 1920x1088 for 1080p,
 * 05/10/2026) is cut back to the size the native host announces; every other
 * frame goes through untouched.
 */

/** A stand-in for WebCodecs' VideoFrame: the init's visibleRect is kept. */
class FakeFrame {
    constructor(source, init = {}) {
        if (source instanceof FakeFrame) {
            this.codedWidth = source.codedWidth;
            this.codedHeight = source.codedHeight;
            this.timestamp = source.timestamp;
        } else {
            this.codedWidth = init.codedWidth;
            this.codedHeight = init.codedHeight;
            this.timestamp = init.timestamp;
        }
        const r = init.visibleRect || {
            x: 0,
            y: 0,
            width: this.codedWidth,
            height: this.codedHeight,
        };
        this.visibleRect = { ...r };
        this.displayWidth = init.displayWidth || r.width;
        this.displayHeight = init.displayHeight || r.height;
        this.closed = false;
    }
    close() {
        this.closed = true;
    }
}

function decoded(width, height) {
    return new FakeFrame(null, { codedWidth: width, codedHeight: height, timestamp: 42 });
}

const rect = (width, height) => ({ x: 0, y: 0, width, height });

describe('cropRect', () => {
    it('cuts 1920x1088 to the 1920x1080 announced', () => {
        expect(cropRect(rect(1920, 1088), { width: 1920, height: 1080 })).toEqual(rect(1920, 1080));
    });

    it('cuts a frame padded on both sides', () => {
        expect(cropRect(rect(1408, 784), { width: 1366, height: 768 })).toEqual(rect(1366, 768));
    });

    it('leaves a frame at the announced size whole', () => {
        expect(cropRect(rect(1920, 1080), { width: 1920, height: 1080 })).toBeNull();
    });

    it('leaves a frame whole when nothing is announced', () => {
        expect(cropRect(rect(1920, 1088), null)).toBeNull();
        expect(cropRect(rect(1920, 1088), undefined)).toBeNull();
        expect(cropRect(rect(1920, 1088), { width: 0, height: 1080 })).toBeNull();
    });

    it('leaves a frame of another size whole: a change not told yet', () => {
        // Smaller than announced (the display shrank, the message is late).
        expect(cropRect(rect(1280, 720), { width: 1920, height: 1080 })).toBeNull();
        // Larger by a superblock or more: not this picture padded.
        expect(cropRect(rect(2560, 1440), { width: 1920, height: 1080 })).toBeNull();
        expect(cropRect(rect(1920, 1080 + MAX_PAD), { width: 1920, height: 1080 })).toBeNull();
    });

    it('keeps the frame origin', () => {
        expect(
            cropRect({ x: 2, y: 4, width: 1920, height: 1088 }, { width: 1920, height: 1080 }),
        ).toEqual({
            x: 2,
            y: 4,
            width: 1920,
            height: 1080,
        });
    });
});

describe('cropToAnnounced', () => {
    afterEach(() => {
        vi.unstubAllGlobals();
    });

    it('hands back a cut frame and closes the decoded one', () => {
        vi.stubGlobal('VideoFrame', FakeFrame);
        const frame = decoded(1920, 1088);
        const out = cropToAnnounced(/** @type {any} */ (frame), { width: 1920, height: 1080 });
        expect(out).not.toBe(frame);
        expect(frame.closed).toBe(true);
        expect(out.visibleRect).toEqual(rect(1920, 1080));
        expect(out.displayWidth).toBe(1920);
        expect(out.displayHeight).toBe(1080);
        expect(out.timestamp).toBe(42);
    });

    it('hands the same frame back when there is nothing to cut', () => {
        vi.stubGlobal('VideoFrame', FakeFrame);
        const frame = decoded(1920, 1080);
        expect(cropToAnnounced(/** @type {any} */ (frame), { width: 1920, height: 1080 })).toBe(
            frame,
        );
        expect(frame.closed).toBe(false);
    });

    it('hands the decoded frame back, open, when the browser refuses the cut', () => {
        vi.stubGlobal(
            'VideoFrame',
            class {
                constructor() {
                    throw new TypeError('visibleRect is not supported');
                }
            },
        );
        const frame = decoded(1920, 1088);
        expect(cropToAnnounced(/** @type {any} */ (frame), { width: 1920, height: 1080 })).toBe(
            frame,
        );
        expect(frame.closed).toBe(false);
    });
});

describe('canCropFrames', () => {
    afterEach(() => {
        vi.unstubAllGlobals();
        vi.resetModules();
    });

    async function fresh() {
        vi.resetModules();
        return (await import('../js/stream/FrameCrop.js')).canCropFrames;
    }

    it('says no where there is no VideoFrame', async () => {
        vi.stubGlobal('VideoFrame', undefined);
        expect((await fresh())()).toBe(false);
    });

    it('says yes where a frame can be cut', async () => {
        vi.stubGlobal('VideoFrame', FakeFrame);
        expect((await fresh())()).toBe(true);
    });

    it('says no where the cut is refused', async () => {
        let made = 0;
        vi.stubGlobal(
            'VideoFrame',
            class extends FakeFrame {
                constructor(source, init) {
                    if (made++ > 0) throw new TypeError('no visibleRect');
                    super(source, init);
                }
            },
        );
        expect((await fresh())()).toBe(false);
    });
});
