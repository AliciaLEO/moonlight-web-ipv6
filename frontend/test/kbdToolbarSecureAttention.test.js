/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * Ctrl+Alt+Suppr from the touch keyboard bar.
 *
 * No injected key can forge it: Windows reserves the combination in the
 * kernel. So the bar's Del, with Ctrl and Alt down, sends it as a message of
 * its own, which the host turns into a real SAS. The bar has no key of its own
 * for it (29/09/2026): Ctrl and Alt, then Del, as on a keyboard.
 */
import { describe, it, expect, vi } from 'vitest';

vi.mock('../js/ui/Toast.js', () => ({ Toast: { warning: () => {} } }));
vi.mock('../js/i18n/i18n.js', () => ({ t: (key) => key, applyTranslations: () => {} }));

const { StreamViewKeyboard } = await import('../js/ui/StreamViewKeyboard.js');

/** The bar, built on a stand-in for the StreamView the mixin runs against. */
function view() {
    const v = Object.create(StreamViewKeyboard.prototype);
    v._rootEl = document.createElement('div');
    v.sent = [];
    v.webrtc = { send: (m) => v.sent.push(m) };
    v._sendKeyEvent = (m) => v.sent.push(m);
    v._buildKbToolbar();
    return v;
}

const key = (v, label) =>
    [...v._kbToolbar.querySelectorAll('button')].find((b) => b.textContent === label);
const press = (btn) =>
    btn.dispatchEvent(new Event('pointerdown', { bubbles: true, cancelable: true }));
const lift = (btn) =>
    btn.dispatchEvent(new Event('pointerup', { bubbles: true, cancelable: true }));
const DEL = 0x2e;

describe('touch keyboard bar: Ctrl+Alt+Suppr', () => {
    it('has no key of its own', () => {
        const labels = [...view()._kbToolbar.querySelectorAll('button')].map((b) => b.textContent);
        expect(labels).toContain('Del');
        expect(labels.some((l) => l.startsWith('C+A'))).toBe(false);
    });

    it('Ctrl and Alt latched, then Del: the message, never the Delete key', () => {
        const v = view();
        press(key(v, 'Ctrl'));
        press(key(v, 'Alt'));
        v.sent.length = 0;
        press(key(v, 'Del'));
        lift(key(v, 'Del'));
        expect(v.sent.filter((m) => m.type === 'secureattention')).toHaveLength(1);
        expect(v.sent.some((m) => m.keyCode === DEL)).toBe(false);
        // Both modifiers let go before the host switches desktops.
        const ups = v.sent.filter((m) => m.type === 'keyup').map((m) => m.keyCode);
        expect(ups).toEqual(expect.arrayContaining([0x11, 0x12]));
        expect(v.sent.findIndex((m) => m.type === 'secureattention')).toBe(v.sent.length - 1);
        expect(key(v, 'Ctrl').classList.contains('active')).toBe(false);
        expect(key(v, 'Alt').classList.contains('active')).toBe(false);
    });

    it('lets go of a locked modifier too', () => {
        const v = view();
        press(key(v, 'Ctrl'));
        press(key(v, 'Ctrl')); // a fast second tap locks it
        expect(key(v, 'Ctrl').classList.contains('locked')).toBe(true);
        press(key(v, 'Alt'));
        v.sent.length = 0;
        press(key(v, 'Del'));
        expect(v.sent.some((m) => m.type === 'secureattention')).toBe(true);
        expect(v.sent.some((m) => m.type === 'keyup' && m.keyCode === 0x11)).toBe(true);
        expect(key(v, 'Ctrl').classList.contains('locked')).toBe(false);
        expect(v._heldMods.ctrl || v._lockedMods.ctrl).toBe(false);
    });

    it('Del alone, or with Ctrl only, is the Delete key', () => {
        const v = view();
        press(key(v, 'Del'));
        lift(key(v, 'Del'));
        expect(v.sent.map((m) => [m.type, m.keyCode])).toEqual([
            ['keydown', DEL],
            ['keyup', DEL],
        ]);

        v.sent.length = 0;
        press(key(v, 'Ctrl'));
        press(key(v, 'Del'));
        lift(key(v, 'Del'));
        expect(v.sent.some((m) => m.type === 'secureattention')).toBe(false);
        expect(v.sent.some((m) => m.type === 'keydown' && m.keyCode === DEL && m.ctrlKey)).toBe(
            true,
        );
    });
});
