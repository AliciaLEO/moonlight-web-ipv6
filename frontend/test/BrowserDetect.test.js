/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import {
    decoderRidesOutGaps,
    detectPlatform,
    detectTv,
    isLowMemory,
    isIphone,
    noteDecoderCannotRideOut,
    physicalScreenSize,
    pickAutoEnhancer,
    PLATFORM_TYPE,
} from '../js/util/BrowserDetect.js';

const UA = {
    iphone: 'Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) Mobile',
    androidPhone: 'Mozilla/5.0 (Linux; Android 13; Pixel) AppleWebKit Mobile Safari',
    androidTablet: 'Mozilla/5.0 (Linux; Android 13; Tab) AppleWebKit Safari',
    ipad: 'Mozilla/5.0 (iPad; CPU OS 17_0 like Mac OS X) Safari',
    winTablet: 'Mozilla/5.0 (Windows NT 10.0; Touch) Edge',
    kindle: 'Mozilla/5.0 (Linux; Silk) Safari',
    blackberry: 'Mozilla/5.0 (BlackBerry; BB10) Mobile',
    winPhone: 'Mozilla/5.0 (Windows Phone 10) IEMobile',
    desktop: 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/120',
};

function withNavigator(extra) {
    vi.stubGlobal('navigator', { maxTouchPoints: 0, hardwareConcurrency: 4, ...extra });
}

describe('BrowserDetect.detectPlatform', () => {
    // jsdom defines `ontouchstart`, so every case here is a touch device: the
    // no-touch-screen branch cannot be staged and is left to the code.
    afterEach(() => vi.unstubAllGlobals());

    it('classifies phones as mobile', () => {
        withNavigator({ userAgent: UA.iphone });
        expect(detectPlatform().type).toBe('mobile');
        withNavigator({ userAgent: UA.androidPhone });
        expect(detectPlatform().type).toBe('mobile');
        withNavigator({ userAgent: UA.blackberry });
        expect(detectPlatform().type).toBe('mobile');
        withNavigator({ userAgent: UA.winPhone });
        expect(detectPlatform().type).toBe('mobile');
    });

    it('classifies tablets as tablet', () => {
        withNavigator({ userAgent: UA.androidTablet });
        expect(detectPlatform().type).toBe('tablet');
        withNavigator({ userAgent: UA.ipad });
        expect(detectPlatform().type).toBe('tablet');
        withNavigator({ userAgent: UA.winTablet });
        expect(detectPlatform().type).toBe('tablet');
        withNavigator({ userAgent: UA.kindle });
        expect(detectPlatform().type).toBe('tablet');
    });

    it('classifies a plain desktop as desktop, with touch detection', () => {
        withNavigator({ userAgent: UA.desktop, maxTouchPoints: 0 });
        const d = detectPlatform();
        expect(d.type).toBe('desktop');
        expect(typeof d.isTouchDevice).toBe('boolean');
        // A high maxTouchPoints flags a touchscreen laptop as touch-capable.
        withNavigator({ userAgent: UA.desktop, maxTouchPoints: 10 });
        expect(detectPlatform().isTouchDevice).toBe(true);
    });

    it('isIphone reflects the user agent', () => {
        withNavigator({ userAgent: UA.iphone });
        expect(isIphone()).toBe(true);
        withNavigator({ userAgent: UA.desktop });
        expect(isIphone()).toBe(false);
    });
});

// A TV's browser often says "Mobile": the screen tells it apart, not the agent.
describe('BrowserDetect.detectTv', () => {
    const MI_TV_WEBVIEW =
        'Mozilla/5.0 (Linux; Android 11; MiTV-MOSR4 Build/RTM5.220609.003; wv) AppleWebKit/537.36 ' +
        '(KHTML, like Gecko) Version/4.0 Chrome/153.0.8010.36 Mobile Safari/537.36';
    const CROMITE =
        'Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 Chrome/153.0.0.0 Mobile Safari/537.36';

    it('knows a TV by its missing touchscreen, whatever the agent says', () => {
        expect(detectTv({ userAgent: CROMITE, maxTouchPoints: 0 })).toBe(true);
        expect(detectTv({ userAgent: MI_TV_WEBVIEW, maxTouchPoints: 5 })).toBe(true);
        expect(
            detectTv({
                userAgent: 'Mozilla/5.0 (Linux; Android 9; AFTMM) Silk',
                maxTouchPoints: 5,
            }),
        ).toBe(true);
    });

    it('leaves phones, tablets and desktops alone', () => {
        expect(detectTv({ userAgent: UA.androidPhone, maxTouchPoints: 5 })).toBe(false);
        expect(detectTv({ userAgent: UA.androidTablet, maxTouchPoints: 10 })).toBe(false);
        expect(detectTv({ userAgent: UA.desktop, maxTouchPoints: 0 })).toBe(false);
        expect(detectTv({ userAgent: UA.iphone, maxTouchPoints: 5 })).toBe(false);
        // "After" in an agent is not a Fire TV model.
        expect(
            detectTv({ userAgent: 'Mozilla/5.0 (X11; Linux) After/1.0', maxTouchPoints: 0 }),
        ).toBe(false);
    });
});

describe('BrowserDetect.isLowMemory', () => {
    it('reads deviceMemory, and an unknown amount is not low', () => {
        expect(isLowMemory({ deviceMemory: 0.5 })).toBe(true);
        expect(isLowMemory({ deviceMemory: 1 })).toBe(true);
        expect(isLowMemory({ deviceMemory: 2 })).toBe(false);
        expect(isLowMemory({ deviceMemory: 4 })).toBe(false);
        expect(isLowMemory({})).toBe(false);
    });
});

// The ride-out bargain needs a decoder that patches over a missing reference.
// VideoToolbox fails on it instead (seen 28/09/2026 from a Mac): every Apple
// platform decodes there, whatever the browser, and nothing else says so.
describe('BrowserDetect.decoderRidesOutGaps', () => {
    const MAC_CHROME =
        'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36';
    const MAC_SAFARI =
        'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Safari/605.1.15';
    const MAC_FIREFOX =
        'Mozilla/5.0 (Macintosh; Intel Mac OS X 14.6; rv:131.0) Gecko/20100101 Firefox/131.0';
    const IPHONE_CHROME =
        'Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) CriOS/129.0 Mobile/15E148 Safari/604.1';
    const WIN_CHROME =
        'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36';
    const LINUX_CHROME =
        'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36';
    const CHROMEOS = 'Mozilla/5.0 (X11; CrOS x86_64 14541.0.0) AppleWebKit/537.36 Chrome/126.0.0.0';

    it('says no on every Apple platform, whatever the browser', () => {
        for (const ua of [MAC_CHROME, MAC_SAFARI, MAC_FIREFOX, IPHONE_CHROME, UA.iphone, UA.ipad]) {
            expect(decoderRidesOutGaps(ua)).toBe(false);
        }
    });

    it('says yes everywhere else — "AppleWebKit" alone is not Apple', () => {
        for (const ua of [WIN_CHROME, LINUX_CHROME, CHROMEOS, UA.androidPhone, UA.desktop]) {
            expect(decoderRidesOutGaps(ua)).toBe(true);
        }
    });

    it('reads the browser user agent when given none', () => {
        vi.stubGlobal('navigator', { userAgent: MAC_CHROME });
        expect(decoderRidesOutGaps()).toBe(false);
        vi.stubGlobal('navigator', { userAgent: WIN_CHROME });
        expect(decoderRidesOutGaps()).toBe(true);
        vi.unstubAllGlobals();
    });

    // The Freebox Player POP's decoder goes silent under the wave where the
    // Mi TV's rides it out, and nothing in their user agents says which: the
    // stream finds out, and the device keeps what it found.
    const FREEBOX =
        'Mozilla/5.0 (Linux; Android 10; Freebox Player POP Build/QTT8.201201.002; wv) AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 Chrome/153.0.8010.39 Mobile Safari/537.36';

    function memoryStore() {
        const m = new Map();
        return { getItem: (k) => (m.has(k) ? m.get(k) : null), setItem: (k, v) => m.set(k, v) };
    }

    it("says no on a device that kept its decoder's verdict, and only there", () => {
        const store = memoryStore();
        expect(decoderRidesOutGaps(FREEBOX, store)).toBe(true);
        noteDecoderCannotRideOut(store);
        expect(decoderRidesOutGaps(FREEBOX, store)).toBe(false);
        expect(decoderRidesOutGaps(FREEBOX, memoryStore())).toBe(true);
    });

    it('keeps the verdict in localStorage by default', () => {
        localStorage.removeItem('mw_ride_out');
        expect(decoderRidesOutGaps(WIN_CHROME)).toBe(true);
        noteDecoderCannotRideOut();
        expect(decoderRidesOutGaps(WIN_CHROME)).toBe(false);
        localStorage.removeItem('mw_ride_out');
    });

    it('rides out when the store cannot be read, and keeps quiet when it cannot be written', () => {
        const broken = {
            getItem() {
                throw new Error('denied');
            },
            setItem() {
                throw new Error('denied');
            },
        };
        expect(decoderRidesOutGaps(FREEBOX, broken)).toBe(true);
        expect(() => noteDecoderCannotRideOut(broken)).not.toThrow();
        expect(decoderRidesOutGaps(FREEBOX, null)).toBe(true);
    });
});

describe('BrowserDetect.physicalScreenSize', () => {
    afterEach(() => vi.unstubAllGlobals());

    it('returns physical pixels scaled by devicePixelRatio', () => {
        vi.stubGlobal('screen', { width: 1280, height: 720 });
        vi.stubGlobal('window', { devicePixelRatio: 2 });
        expect(physicalScreenSize()).toEqual({ short: 1440, long: 2560 });
    });
});

describe('BrowserDetect — enhancer choice + module constants', () => {
    // jsdom has no WebGL: answer "no context" quietly instead of logging its
    // not-implemented notice each time the GPU probe asks.
    beforeEach(() => {
        vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue(null);
    });
    afterEach(() => {
        vi.restoreAllMocks();
        vi.unstubAllGlobals();
    });

    it('PLATFORM_TYPE is a known value and desktop picks fsr1', () => {
        expect(['mobile', 'tablet', 'desktop']).toContain(PLATFORM_TYPE);
        expect(pickAutoEnhancer()).toBe('fsr1'); // jsdom default UA → desktop
    });

    it('a beefy 1080p+ Android phone still picks sgsr (re-imported with a stubbed UA)', async () => {
        // The platform class is the whole rule since 03/09/2026: a phone's GPU
        // budget goes to the decode, however many cores it has.
        vi.resetModules();
        vi.stubGlobal('navigator', {
            userAgent: UA.androidPhone,
            hardwareConcurrency: 8,
            maxTouchPoints: 5,
        });
        vi.stubGlobal('screen', { width: 1080, height: 2400 });
        const m = await import('../js/util/BrowserDetect.js');
        expect(m.IS_ANDROID).toBe(true);
        expect(m.PLATFORM_TYPE).toBe('mobile');
        expect(m.pickAutoEnhancer()).toBe('sgsr');
    });

    it('a weak Android phone picks sgsr', async () => {
        vi.resetModules();
        vi.stubGlobal('navigator', {
            userAgent: UA.androidPhone,
            hardwareConcurrency: 2,
            maxTouchPoints: 5,
        });
        vi.stubGlobal('screen', { width: 720, height: 1280 });
        const m = await import('../js/util/BrowserDetect.js');
        expect(m.pickAutoEnhancer()).toBe('sgsr');
    });
});

// Chrome on Windows-on-ARM says "Win64; x64" and mobile:false — the platform
// rule reads a Snapdragon laptop as a plain desktop. The GPU string is the one
// thing that tells, so SGSR (Qualcomm's upscaler) is keyed on it.
describe('BrowserDetect — Snapdragon picks SGSR whatever the form factor', () => {
    const ADRENO =
        'ANGLE (Qualcomm, Qualcomm(R) Adreno(TM) 618 GPU (0x41333830) Direct3D11 vs_5_0 ps_5_0, D3D11)';
    const NVIDIA =
        'ANGLE (NVIDIA, NVIDIA GeForce RTX 5060 Ti (0x00002D04) Direct3D11 vs_5_0 ps_5_0, D3D11)';

    /** Fake a WebGL context whose debug extension reports `renderer`. */
    function fakeWebGl(renderer) {
        const UNMASKED = 0x9246;
        const gl = {
            RENDERER: 0x1f01,
            getExtension: (name) =>
                name === 'WEBGL_debug_renderer_info' ? { UNMASKED_RENDERER_WEBGL: UNMASKED } : null,
            getParameter: (p) => (p === UNMASKED ? renderer : 'masked'),
        };
        return vi
            .spyOn(document, 'createElement')
            .mockImplementation(() => ({ getContext: () => (renderer === null ? null : gl) }));
    }

    async function freshModule(renderer) {
        vi.resetModules();
        vi.stubGlobal('navigator', { userAgent: UA.desktop, maxTouchPoints: 0 });
        const spy = fakeWebGl(renderer);
        const m = await import('../js/util/BrowserDetect.js');
        return { m, spy };
    }

    afterEach(() => {
        vi.restoreAllMocks();
        vi.unstubAllGlobals();
    });

    it('an Adreno under a desktop user agent picks sgsr', async () => {
        const { m } = await freshModule(ADRENO);
        expect(m.PLATFORM_TYPE).toBe('desktop');
        expect(m.isSnapdragonGpu()).toBe(true);
        expect(m.pickAutoEnhancer()).toBe('sgsr');
    });

    it('any other desktop GPU keeps fsr1', async () => {
        const { m } = await freshModule(NVIDIA);
        expect(m.isSnapdragonGpu()).toBe(false);
        expect(m.pickAutoEnhancer()).toBe('fsr1');
    });

    it('without WebGL the platform rule stands', async () => {
        const { m } = await freshModule(null);
        expect(m.isSnapdragonGpu()).toBe(false);
        expect(m.pickAutoEnhancer()).toBe('fsr1');
    });

    it('asks the GPU once per page', async () => {
        const { m, spy } = await freshModule(ADRENO);
        m.pickAutoEnhancer();
        m.pickAutoEnhancer();
        m.isSnapdragonGpu();
        expect(spy).toHaveBeenCalledTimes(1);
    });
});

describe('BrowserDetect.supportsGamingMode', () => {
    async function fresh(userAgent, maxTouchPoints, finePointer) {
        vi.resetModules();
        vi.stubGlobal('navigator', { userAgent, maxTouchPoints });
        vi.stubGlobal('matchMedia', (q) => ({
            matches: q === '(any-pointer: fine)' && finePointer,
        }));
        return import('../js/util/BrowserDetect.js');
    }

    afterEach(() => vi.unstubAllGlobals());

    it('a TV is no touch device, nor handheld, and keeps gaming mode out', async () => {
        const tv =
            'Mozilla/5.0 (Linux; Android 10; Freebox Player POP Build/QTT8.201201.002; wv) ' +
            'AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 Chrome/153.0.8010.39 Mobile Safari/537.36';
        const m = await fresh(tv, 0, false);
        expect(m.IS_TV).toBe(true);
        expect(m.IS_TOUCH_DEVICE).toBe(false);
        expect(m.IS_HANDHELD).toBe(false);
        expect(m.supportsGamingMode()).toBe(false);
        // A phone is still one.
        const phone = await fresh(UA.androidPhone, 5, false);
        expect(phone.IS_TOUCH_DEVICE).toBe(true);
    });

    it('a touchscreen PC with a mouse has it (Surface, issue #16)', async () => {
        const m = await fresh(UA.desktop, 10, true);
        expect(m.IS_TOUCH_DEVICE).toBe(true);
        expect(m.supportsGamingMode()).toBe(true);
    });

    it('a touchscreen PC with no mouse or trackpad does not', async () => {
        const m = await fresh(UA.desktop, 10, false);
        expect(m.supportsGamingMode()).toBe(false);
    });

    it('a phone never has it, even with a mouse paired', async () => {
        const m = await fresh(UA.androidPhone, 5, true);
        expect(m.supportsGamingMode()).toBe(false);
    });
});
