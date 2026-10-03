/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi, afterEach } from 'vitest';

vi.mock('../js/stream/gamepadMapping.js', async (importOriginal) => {
    const real = await importOriginal();
    // No database to fetch in a test.
    return { ...real, loadGamepadDb: () => Promise.resolve(null) };
});

const { isTvRemoteId, padKind, PAD_KINDS, BUTTON_TARGETS } =
    await import('../js/stream/gamepadMapping.js');
const { isTvRemotePad } = await import('../js/stream/GamepadManager.js');
const { remoteArtSvg, deviceArtSvg, kindIconSvg } = await import('../js/ui/DeviceArt.js');
const { GamepadArtView } = await import('../js/ui/GamepadArt.js');
const { GamepadRemapDialog, remoteKeyCtl } = await import('../js/ui/GamepadRemapDialog.js');

/**
 * A TV remote in the controllers' dialog.
 *
 * Android calls a TV's remote a joystick (a volume axis is enough), so it
 * shows up among the pads — drawn as an Xbox pad, with a "Remap" that has
 * nothing to remap (Freebox B16C and Mi TV, 03/10/2026). It is drawn as the
 * remote it is, with what each key does in MoonlightWeb.
 */

const XIAOMI = 'Xiaomi RC Consumer Control';
const FREEBOX = 'B16C (Vendor: 7545 Product: 0183)';

/** A remote as the Gamepad API shows it: its arrows are buttons 12-15. */
function remotePad(id = XIAOMI, held = []) {
    return {
        id,
        index: 0,
        connected: true,
        mapping: '',
        timestamp: 1,
        axes: [0, 0],
        buttons: Array.from({ length: 17 }, (_, i) => ({
            pressed: held.includes(i),
            value: held.includes(i) ? 1 : 0,
        })),
    };
}

describe('a TV remote is told apart', () => {
    it('by its name, on both TVs', () => {
        expect(isTvRemoteId(XIAOMI)).toBe(true);
        expect(isTvRemoteId(FREEBOX)).toBe(true);
        expect(isTvRemoteId('SHIELD Remote')).toBe(true);
        expect(isTvRemoteId('Xbox Wireless Controller')).toBe(false);
        expect(isTvRemoteId('EdgeTX RadioMaster TX16S Joystick')).toBe(false);
        expect(isTvRemoteId(null)).toBe(false);
        // The stream reads the same rule.
        expect(isTvRemotePad({ id: XIAOMI })).toBe(true);
    });

    it("is a 'remote', whatever kind was saved for it, and not a kind to pick", () => {
        expect(padKind({ id: XIAOMI })).toBe('remote');
        expect(padKind(FREEBOX, { kind: 'gamepad' })).toBe('remote');
        expect(padKind({ id: 'Xbox Wireless Controller' })).toBe('gamepad');
        expect(PAD_KINDS).not.toContain('remote');
    });
});

describe('the drawing of a remote', () => {
    it('has the keys the stream uses, and names what they do', () => {
        const root = document.createElement('div');
        root.innerHTML = deviceArtSvg('remote');
        for (const ctl of [
            'dpup',
            'dpdown',
            'dpleft',
            'dpright',
            'ok',
            'chup',
            'chdown',
            'colour',
            'digits',
        ]) {
            expect(root.querySelector(`[data-ctl="${ctl}"]`), ctl).not.toBe(null);
        }
        expect(root.querySelector('svg.gp-art-remote')).not.toBe(null);
        // Back, Home, the volume, the power key: the TV's, greyed.
        expect(root.querySelectorAll('.gp-unbound').length).toBeGreaterThanOrEqual(4);
        expect(root.querySelectorAll('.gp-label').length).toBeGreaterThanOrEqual(6);
    });

    it('lights its arrows from the pad the browser shows', () => {
        const root = document.createElement('div');
        root.innerHTML = remoteArtSvg();
        const view = new GamepadArtView(root);
        view.render(remotePad(XIAOMI, [13]), BUTTON_TARGETS);
        expect(root.querySelector('[data-ctl="dpdown"]').classList.contains('is-pressed')).toBe(
            true,
        );
        expect(root.querySelector('[data-ctl="dpup"]').classList.contains('is-pressed')).toBe(
            false,
        );
    });

    it('has an icon of its own for the list', () => {
        expect(kindIconSvg('remote')).not.toBe(kindIconSvg('gamepad'));
    });
});

describe('the keys a remote sends as keys', () => {
    it('are named after the part of the drawing they light', () => {
        expect(remoteKeyCtl({ key: 'Enter', code: '' })).toBe('ok');
        // A keyboard's Enter has a code: not the remote's OK.
        expect(remoteKeyCtl({ key: 'Enter', code: 'Enter' })).toBe(null);
        expect(remoteKeyCtl({ key: 'ChannelUp' })).toBe('chup');
        expect(remoteKeyCtl({ key: 'ChannelDown' })).toBe('chdown');
        expect(remoteKeyCtl({ key: 'ColorF2Yellow', code: '' })).toBe('colour');
        expect(remoteKeyCtl({ key: '7', code: 'Digit7' })).toBe('digits');
        expect(remoteKeyCtl({ key: 'ArrowLeft', code: 'ArrowLeft' })).toBe('dpleft');
        expect(remoteKeyCtl({ key: 'a', code: 'KeyA' })).toBe(null);
        expect(remoteKeyCtl(null)).toBe(null);
    });
});

describe('the controllers dialog with a TV remote', () => {
    let dialog = null;
    let pads = [];

    afterEach(() => {
        if (dialog) dialog.close();
        dialog = null;
        document.body.innerHTML = '';
        vi.restoreAllMocks();
    });

    function open(mode, pad) {
        pads = [pad];
        vi.spyOn(navigator, 'getGamepads').mockImplementation(() => pads);
        dialog = new GamepadRemapDialog({ mode });
        dialog.open();
        return document.querySelector('.gamepad-remap');
    }

    // jsdom has no getGamepads of its own to spy on.
    if (!navigator.getGamepads) navigator.getGamepads = () => [];

    it('draws the remote, with no kind to pick, no layout badge and nothing to remap', () => {
        const el = open('auto', remotePad());
        expect(el.querySelector('svg.gp-art-remote')).not.toBe(null);
        expect(el.querySelector('.gamepad-remap-kind-select')).toBe(null);
        expect(el.querySelector('.gp-badge, [class*="gp-badge"]')).toBe(null);
        expect(el.querySelector('.gp-remap')).toBe(null);
        expect(el.querySelector('.gp-close')).not.toBe(null);
        expect(el.querySelector('.gamepad-remap-title').textContent).toBe(
            'gamepad.remap.kind.remote',
        );
        expect(el.querySelector('.gamepad-remap-hint').hidden).toBe(false);
    });

    it('never starts the wizard for it, even when asked', () => {
        const el = open('wizard', remotePad(FREEBOX));
        expect(el.querySelector('.gamepad-remap-progress').hidden).toBe(true);
        expect(el.querySelector('.gp-save, .gp-skip')).toBe(null);
    });

    it('lights OK while it is held', () => {
        const el = open('test', remotePad());
        const ok = el.querySelector('[data-ctl="ok"]');
        document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', code: '' }));
        dialog._drawRemoteKeys();
        expect(ok.classList.contains('is-pressed')).toBe(true);
        document.dispatchEvent(new KeyboardEvent('keyup', { key: 'Enter', code: '' }));
        dialog._drawRemoteKeys();
        expect(ok.classList.contains('is-pressed')).toBe(false);
    });

    it('keeps a real pad as it was', () => {
        const el = open('test', { ...remotePad('Xbox Wireless Controller'), mapping: 'standard' });
        expect(el.querySelector('svg.gp-art-remote')).toBe(null);
        expect(el.querySelector('.gamepad-remap-kind-select')).not.toBe(null);
        expect(el.querySelector('.gp-remap')).not.toBe(null);
    });
});
