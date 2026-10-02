/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The refresh rate a TV tells its host (util/RefreshRate.js, "On a TV"). A
 * stream fills a TV's main thread and its rAF then ticks at the page's pace: a
 * reading taken then is not the panel's, and must not replace the one taken
 * before the stream. A desktop, whose window can move to another screen, is
 * measured again.
 */
import { describe, it, expect, afterEach, vi } from 'vitest';

/** RefreshRate.js afresh, on a device that is a TV or not. */
async function load(tv) {
    vi.resetModules();
    vi.doMock('../js/util/BrowserDetect.js', async () => ({
        ...(await vi.importActual('../js/util/BrowserDetect.js')),
        IS_TV: tv,
    }));
    return import('../js/util/RefreshRate.js');
}

/** A screen whose rAF ticks every `periodMs`. Returns the rAF spy. */
function ticksEvery(periodMs) {
    let now = 1000;
    const raf = vi.fn((cb) => setTimeout(() => cb((now += periodMs)), 0));
    vi.stubGlobal('requestAnimationFrame', raf);
    return raf;
}

function pageHidden(hidden) {
    Object.defineProperty(document, 'hidden', { value: hidden, configurable: true });
}

describe('RefreshRate on a TV', () => {
    afterEach(() => {
        vi.unstubAllGlobals();
        vi.doUnmock('../js/util/BrowserDetect.js');
        delete document.hidden;
    });

    it('keeps the panel it read before the stream, not the page under one', async () => {
        // The Mi TV: a 60 Hz panel, read 34 Hz under a 720p30 stream (01/10/2026).
        const rate = await load(true);
        pageHidden(false);
        ticksEvery(1000 / 60);
        expect(await rate.measureRefreshRate({ frames: 20 })).toBe(60000);

        const underStream = ticksEvery(1000 / 34);
        expect(await rate.measureRefreshRate({ frames: 20 })).toBe(60000);
        expect(underStream).not.toHaveBeenCalled();
        expect(rate.currentRefreshMilliHz()).toBe(60000);
    });

    it('still measures a TV that had no good reading yet', async () => {
        // The Freebox Player POP, its page hidden at load: nothing read, then
        // its 50 Hz once visible — and that stands under a stream (29.4 Hz).
        const rate = await load(true);
        pageHidden(true);
        expect(await rate.measureRefreshRate({ frames: 20 })).toBe(0);

        pageHidden(false);
        ticksEvery(20);
        expect(await rate.measureRefreshRate({ frames: 20 })).toBe(50000);

        const underStream = ticksEvery(34);
        expect(await rate.measureRefreshRate({ frames: 20 })).toBe(50000);
        expect(underStream).not.toHaveBeenCalled();
    });

    it('measures a desktop again: its window may have moved to another screen', async () => {
        const rate = await load(false);
        pageHidden(false);
        ticksEvery(1000 / 60);
        expect(await rate.measureRefreshRate({ frames: 20 })).toBe(60000);

        ticksEvery(1000 / 144);
        expect(await rate.measureRefreshRate({ frames: 20 })).toBe(144000);
    });
});
