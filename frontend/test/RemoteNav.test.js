/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import {
    pickNext,
    navKey,
    arrowStaysNative,
    init,
    isActive,
    _setActiveForTest,
    _resetForTest,
    _settleForTest,
    _pollForTest,
    isRemoteBackKey,
} from '../js/ui/RemoteNav.js';

/** jsdom has no layout: give an element the box it would have on screen. */
function place(el, left, top, width, height) {
    el.getBoundingClientRect = () => ({
        left,
        top,
        right: left + width,
        bottom: top + height,
        width,
        height,
        x: left,
        y: top,
    });
    return el;
}

function rect(left, top, width, height) {
    return { left, top, right: left + width, bottom: top + height };
}

function button(id, left, top, width = 100, height = 60, parent = document.body) {
    const b = document.createElement('button');
    b.id = id;
    b.textContent = id;
    parent.appendChild(b);
    return place(b, left, top, width, height);
}

function press(key, target = document.activeElement || document.body) {
    const ev = new KeyboardEvent('keydown', { key, bubbles: true, cancelable: true });
    target.dispatchEvent(ev);
    return ev;
}

describe('RemoteNav.pickNext', () => {
    // Two rows of three, 100×60 with 20 px gaps.
    const grid = [];
    for (let row = 0; row < 2; row++) {
        for (let col = 0; col < 3; col++) {
            grid.push({ el: `${col},${row}`, rect: rect(col * 120, row * 80, 100, 60) });
        }
    }
    const from = (id) => grid.find((c) => c.el === id).rect;
    const others = (id) => grid.filter((c) => c.el !== id);

    it('moves one step in each direction across a grid', () => {
        expect(pickNext(from('1,0'), 'right', others('1,0'))).toBe('2,0');
        expect(pickNext(from('1,0'), 'left', others('1,0'))).toBe('0,0');
        expect(pickNext(from('1,0'), 'down', others('1,0'))).toBe('1,1');
        expect(pickNext(from('1,1'), 'up', others('1,1'))).toBe('1,0');
    });

    it('stops at the edge instead of wrapping around', () => {
        expect(pickNext(from('2,0'), 'right', others('2,0'))).toBe(null);
        expect(pickNext(from('0,0'), 'up', others('0,0'))).toBe(null);
    });

    it('prefers what lines up with the current element to a closer diagonal', () => {
        const list = [
            { el: 'diagonal', rect: rect(150, 90, 100, 60) },
            { el: 'below', rect: rect(0, 200, 100, 60) },
        ];
        expect(pickNext(rect(0, 0, 100, 60), 'down', list)).toBe('below');
    });

    it('reaches the Quit button inside a card and comes back to the card', () => {
        const card = rect(0, 0, 100, 140);
        const quit = rect(10, 110, 80, 20);
        const nextRow = rect(0, 160, 100, 140);
        expect(
            pickNext(card, 'down', [
                { el: 'quit', rect: quit },
                { el: 'next row', rect: nextRow },
            ]),
        ).toBe('quit');
        expect(pickNext(quit, 'up', [{ el: 'card', rect: card }])).toBe('card');
    });
});

describe('RemoteNav.navKey', () => {
    afterEach(() => {
        document.body.innerHTML = '';
    });

    it('names an element by its own key, else its id', () => {
        const a = document.createElement('div');
        a.setAttribute('data-nav-key', 'resume');
        a.id = 'x';
        expect(navKey(a)).toBe('resume');
        const b = document.createElement('button');
        b.id = 'btn-settings';
        expect(navKey(b)).toBe('#btn-settings');
    });

    it('names an app card the same way after it was rebuilt', () => {
        const make = () => {
            document.body.innerHTML =
                '<section id="main-content"><div class="host-card" data-uuid="h1">' +
                '<div class="app-card" data-app-id="7" tabindex="0"></div></div></section>';
            return document.querySelector('.app-card');
        };
        const first = navKey(make());
        const second = navKey(make());
        expect(first).toBe(second);
        expect(first).toContain('data-app-id=7');
        expect(first).toContain('data-uuid=h1');
    });
});

describe('RemoteNav.arrowStaysNative', () => {
    it('leaves the caret its arrows inside a text, but not at its ends', () => {
        const input = document.createElement('input');
        input.value = 'abcd';
        input.setSelectionRange(2, 2);
        expect(arrowStaysNative(input, 'ArrowLeft')).toBe(true);
        expect(arrowStaysNative(input, 'ArrowRight')).toBe(true);
        expect(arrowStaysNative(input, 'ArrowDown')).toBe(false);
        input.setSelectionRange(0, 0);
        expect(arrowStaysNative(input, 'ArrowLeft')).toBe(false);
    });

    it('leaves an armed slider its left and right, and a checkbox nothing', () => {
        const range = document.createElement('input');
        range.type = 'range';
        expect(arrowStaysNative(range, 'ArrowRight')).toBe(false);
        range.dataset.navArmed = '1';
        expect(arrowStaysNative(range, 'ArrowRight')).toBe(true);
        expect(arrowStaysNative(range, 'ArrowUp')).toBe(false);
        const box = document.createElement('input');
        box.type = 'checkbox';
        expect(arrowStaysNative(box, 'ArrowLeft')).toBe(false);
        expect(arrowStaysNative(document.createElement('button'), 'ArrowLeft')).toBe(false);
    });
});

describe('RemoteNav on a page', () => {
    beforeEach(() => {
        _resetForTest();
        document.body.innerHTML = '';
        document.body.className = '';
    });

    afterEach(() => {
        _resetForTest();
        document.body.innerHTML = '';
        document.body.className = '';
    });

    it('is off on a desktop unless asked', () => {
        expect(isActive()).toBe(false);
        init();
        const a = button('a', 0, 0);
        button('b', 120, 0);
        a.focus();
        const ev = press('ArrowRight');
        expect(ev.defaultPrevented).toBe(false);
        expect(document.activeElement).toBe(a);
    });

    it('moves the focus with the arrows and keeps the browser from moving it too', () => {
        _setActiveForTest(true);
        init();
        const a = button('a', 0, 0);
        const b = button('b', 120, 0);
        const c = button('c', 0, 80);
        a.focus();
        const ev = press('ArrowRight');
        expect(ev.defaultPrevented).toBe(true);
        expect(document.activeElement).toBe(b);
        press('ArrowLeft');
        press('ArrowDown');
        expect(document.activeElement).toBe(c);
    });

    it('puts the first press on an app card when nothing has the focus', () => {
        _setActiveForTest(true);
        init();
        button('settings', 0, 0);
        const main = document.createElement('section');
        main.id = 'main-content';
        document.body.appendChild(main);
        const card = place(document.createElement('div'), 0, 100, 100, 140);
        card.className = 'app-card';
        card.tabIndex = 0;
        main.appendChild(card);
        press('ArrowDown', document.body);
        expect(document.activeElement).toBe(card);
    });

    it('gives an opening dialog the focus and keeps the arrows inside it', () => {
        _setActiveForTest(true);
        init();
        const opener = button('open', 0, 0);
        opener.focus();
        const dialog = place(document.createElement('div'), 200, 200, 400, 200);
        dialog.className = 'pairing-overlay';
        document.body.appendChild(dialog);
        const cancel = button('cancel', 220, 300, 100, 60, dialog);
        const danger = button('remove', 340, 300, 100, 60, dialog);
        danger.className = 'btn-danger';
        _settleForTest();
        expect(document.activeElement).toBe(cancel);
        press('ArrowRight');
        expect(document.activeElement).toBe(danger);
        // Nowhere further right inside the dialog: the press stays there.
        const ev = press('ArrowRight');
        expect(ev.defaultPrevented).toBe(true);
        expect(document.activeElement).toBe(danger);
        // Closed: the focus goes back to the element that opened it.
        dialog.remove();
        _settleForTest();
        expect(document.activeElement).toBe(opener);
    });

    it('puts the focus back on a card the host list rebuilt', () => {
        _setActiveForTest(true);
        init();
        const main = document.createElement('section');
        main.id = 'main-content';
        document.body.appendChild(main);
        const draw = () => {
            main.innerHTML =
                '<div class="host-card" data-uuid="h1"><div class="app-card" data-app-id="3" tabindex="0"></div></div>';
            return place(main.querySelector('.app-card'), 0, 0, 100, 140);
        };
        draw().focus();
        const again = draw();
        expect(document.activeElement).not.toBe(again);
        _settleForTest();
        expect(document.activeElement).toBe(again);
    });

    it('leaves the keys to the stream, unless a menu is open over it', () => {
        _setActiveForTest(true);
        init();
        const a = button('a', 0, 0);
        button('b', 120, 0);
        a.focus();
        document.body.classList.add('streaming-active');
        expect(press('ArrowRight').defaultPrevented).toBe(false);
        expect(document.activeElement).toBe(a);
    });

    it("moves with a pad's direction pad and clicks with A", () => {
        _setActiveForTest(true);
        init();
        const a = button('a', 0, 0);
        const b = button('b', 120, 0);
        let clicked = 0;
        b.addEventListener('click', () => clicked++);
        a.focus();
        const buttons = Array.from({ length: 17 }, () => ({ pressed: false, value: 0 }));
        const pad = { index: 0, connected: true, buttons, axes: [0, 0, 0, 0] };
        Object.defineProperty(navigator, 'getGamepads', {
            value: () => [pad],
            configurable: true,
        });
        _pollForTest();
        buttons[15].pressed = true; // right
        _pollForTest();
        expect(document.activeElement).toBe(b);
        buttons[15].pressed = false;
        _pollForTest();
        buttons[0].pressed = true; // A
        _pollForTest();
        expect(clicked).toBe(1);
        delete (/** @type {any} */ (navigator).getGamepads);
    });

    it('takes the press that made a pad appear: Chromium shows a pad only once pressed', () => {
        _setActiveForTest(true);
        init();
        const a = button('a', 0, 0);
        const b = button('b', 120, 0);
        a.focus();
        const buttons = Array.from({ length: 17 }, (_, i) => ({ pressed: i === 15, value: 0 }));
        Object.defineProperty(navigator, 'getGamepads', {
            value: () => [{ index: 0, connected: true, buttons, axes: [0, 0, 0, 0] }],
            configurable: true,
        });
        _pollForTest();
        expect(document.activeElement).toBe(b);
        delete (/** @type {any} */ (navigator).getGamepads);
    });
});

describe('RemoteNav: at the edge of a long page', () => {
    const root = document.documentElement;
    let scrollBy;

    beforeEach(() => {
        _resetForTest();
        document.body.innerHTML = '';
        scrollBy = vi.fn();
        // jsdom has no layout: a document taller than the screen.
        Object.defineProperty(document, 'scrollingElement', { value: root, configurable: true });
        Object.defineProperty(root, 'scrollHeight', { value: 2000, configurable: true });
        Object.defineProperty(root, 'clientHeight', { value: 700, configurable: true });
        root.scrollBy = scrollBy;
    });
    afterEach(() => {
        _resetForTest();
        delete (/** @type {any} */ (document).scrollingElement);
        delete (/** @type {any} */ (root).scrollHeight);
        delete (/** @type {any} */ (root).clientHeight);
        delete (/** @type {any} */ (root).scrollBy);
    });

    it('on a TV the document itself scrolls: Down past the last card shows more', () => {
        _setActiveForTest(true);
        init();
        button('top', 0, 0);
        const last = button('last', 0, 600);
        last.focus();
        const ev = press('ArrowDown');
        expect(scrollBy).toHaveBeenCalledWith({ top: 700 * 0.6 });
        expect(ev.defaultPrevented).toBe(true);
    });

    it('a scrolling box around the focus is still scrolled first', () => {
        _setActiveForTest(true);
        init();
        const box = document.createElement('div');
        box.style.overflowY = 'auto';
        Object.defineProperty(box, 'scrollHeight', { value: 900 });
        Object.defineProperty(box, 'clientHeight', { value: 300 });
        box.scrollBy = vi.fn();
        document.body.appendChild(box);
        button('inside', 0, 0, 100, 60, box).focus();
        press('ArrowDown');
        expect(box.scrollBy).toHaveBeenCalledWith({ top: 300 * 0.6 });
        expect(scrollBy).not.toHaveBeenCalled();
    });
});

describe('RemoteNav: the ring follows the focus, not the cursor', () => {
    const html = document.documentElement;
    const tap = () => window.dispatchEvent(new Event('pointerdown'));

    beforeEach(() => {
        _resetForTest();
        document.body.innerHTML = '';
    });
    afterEach(() => _resetForTest());

    it("a tap of the browser's cursor turns the ring off, an arrow turns it back on", () => {
        _setActiveForTest(true);
        init();
        button('a', 0, 0).focus();
        tap();
        expect(html.classList.contains('nav-pointer')).toBe(true);
        press('ArrowRight');
        expect(html.classList.contains('nav-pointer')).toBe(false);
    });

    it('OK and Escape bring it back too; any other key does not', () => {
        _setActiveForTest(true);
        init();
        tap();
        press('a');
        expect(html.classList.contains('nav-pointer')).toBe(true);
        press('Enter');
        expect(html.classList.contains('nav-pointer')).toBe(false);
        tap();
        press('Escape');
        expect(html.classList.contains('nav-pointer')).toBe(false);
    });

    it("a pad's press brings it back", () => {
        _setActiveForTest(true);
        init();
        button('a', 0, 0).focus();
        const buttons = Array.from({ length: 17 }, () => ({ pressed: false, value: 0 }));
        Object.defineProperty(navigator, 'getGamepads', {
            value: () => [{ index: 0, connected: true, buttons, axes: [0, 0, 0, 0] }],
            configurable: true,
        });
        _pollForTest();
        tap();
        buttons[13].pressed = true;
        _pollForTest();
        expect(html.classList.contains('nav-pointer')).toBe(false);
        delete (/** @type {any} */ (navigator).getGamepads);
    });

    it('off a TV, a click changes nothing', () => {
        _setActiveForTest(false);
        init();
        tap();
        expect(html.classList.contains('nav-pointer')).toBe(false);
    });
});

describe('RemoteNav: a slider changes only once OK armed it', () => {
    beforeEach(() => {
        _resetForTest();
        document.body.innerHTML = '';
    });
    afterEach(() => _resetForTest());

    function slider(left, top) {
        const r = document.createElement('input');
        r.type = 'range';
        r.min = '1';
        r.max = '150';
        r.value = '20';
        document.body.appendChild(r);
        return place(r, left, top, 300, 20);
    }

    it('passing over it with Left/Right keeps its value', () => {
        _setActiveForTest(true);
        init();
        const r = slider(0, 0);
        r.focus();
        const ev = press('ArrowRight', r);
        expect(ev.defaultPrevented).toBe(true);
        expect(r.value).toBe('20');
    });

    it('OK arms it, OK again or leaving it disarms it', () => {
        _setActiveForTest(true);
        init();
        const r = slider(0, 0);
        const b = button('below', 0, 100);
        r.focus();
        press('Enter', r);
        expect(r.dataset.navArmed).toBe('1');
        expect(press('ArrowRight', r).defaultPrevented).toBe(false); // the browser steps it
        press('Enter', r);
        expect(r.dataset.navArmed).toBeUndefined();
        press('Enter', r);
        b.focus();
        expect(r.dataset.navArmed).toBeUndefined();
    });

    it('a pad: A arms it, then its right steps it', () => {
        _setActiveForTest(true);
        init();
        const r = slider(0, 0);
        r.focus();
        const buttons = Array.from({ length: 17 }, () => ({ pressed: false, value: 0 }));
        Object.defineProperty(navigator, 'getGamepads', {
            value: () => [{ index: 0, connected: true, buttons, axes: [0, 0, 0, 0] }],
            configurable: true,
        });
        _pollForTest();
        buttons[15].pressed = true;
        _pollForTest();
        expect(r.value).toBe('20');
        buttons[15].pressed = false;
        _pollForTest();
        buttons[0].pressed = true;
        _pollForTest();
        expect(r.dataset.navArmed).toBe('1');
        buttons[0].pressed = false;
        _pollForTest();
        buttons[15].pressed = true;
        _pollForTest();
        expect(r.value).toBe('21');
        delete (/** @type {any} */ (navigator).getGamepads);
    });
});

describe("RemoteNav: a remote's colour key is the way back", () => {
    beforeEach(() => {
        _resetForTest();
        document.body.innerHTML = '';
        document.body.className = '';
        _setActiveForTest(true);
        init();
    });

    afterEach(() => {
        _resetForTest();
        document.body.innerHTML = '';
        document.body.className = '';
    });

    // As a Mi TV's remote sends them (03/10/2026): a stray code, no keyCode.
    const colour = (type, extra = {}) =>
        new KeyboardEvent(type, {
            key: 'ColorF0Red',
            code: 'MediaStop',
            bubbles: true,
            cancelable: true,
            ...extra,
        });

    it('knows the four colour keys and nothing else', () => {
        for (const key of ['ColorF0Red', 'ColorF1Green', 'ColorF2Yellow', 'ColorF3Blue']) {
            expect(isRemoteBackKey({ key })).toBe(true);
        }
        for (const key of ['Escape', 'Enter', 'MediaStop', 'c', '']) {
            expect(isRemoteBackKey({ key })).toBe(false);
        }
        expect(isRemoteBackKey(null)).toBe(false);
    });

    it('closes the dialog on top, as Escape would', () => {
        const overlay = place(document.createElement('div'), 0, 0, 400, 300);
        overlay.className = 'share-popin-overlay';
        document.body.appendChild(overlay);
        const inside = button('ok', 10, 10, 100, 60, overlay);
        inside.focus();
        const escapes = [];
        overlay.addEventListener('keydown', (e) => escapes.push(e.key));
        const close = place(document.createElement('button'), 900, 0, 40, 40);
        close.className = 'view-close-btn';
        document.body.appendChild(close);
        const closed = vi.fn();
        close.addEventListener('click', closed);
        const ev = colour('keydown');
        inside.dispatchEvent(ev);
        expect(ev.defaultPrevented).toBe(true);
        expect(escapes).toEqual(['Escape']); // the colour key itself never got there
        expect(closed).not.toHaveBeenCalled(); // the view under the dialog stays
    });

    it("with nothing open, presses the view's ✕ (Settings, Admin)", () => {
        button('a', 0, 100).focus();
        const close = place(document.createElement('button'), 900, 0, 40, 40);
        close.className = 'view-close-btn';
        document.body.appendChild(close);
        const closed = vi.fn();
        close.addEventListener('click', closed);
        document.activeElement.dispatchEvent(colour('keydown'));
        expect(closed).toHaveBeenCalledTimes(1);
    });

    it('goes back once per press, however long it is held', () => {
        // Android repeats a held key without the repeat flag.
        button('a', 0, 100).focus();
        const close = place(document.createElement('button'), 900, 0, 40, 40);
        close.className = 'view-close-btn';
        document.body.appendChild(close);
        const closed = vi.fn();
        close.addEventListener('click', closed);
        for (let i = 0; i < 4; i++) document.body.dispatchEvent(colour('keydown'));
        expect(closed).toHaveBeenCalledTimes(1);
        document.body.dispatchEvent(colour('keyup'));
        document.body.dispatchEvent(colour('keydown'));
        expect(closed).toHaveBeenCalledTimes(2);
    });

    it('leaves it to the stream view over a bare stream', () => {
        document.body.classList.add('streaming-active');
        const seen = vi.fn();
        document.addEventListener('keydown', seen);
        const ev = colour('keydown');
        document.body.dispatchEvent(ev);
        expect(ev.defaultPrevented).toBe(false);
        expect(seen).toHaveBeenCalledTimes(1);
    });
});
