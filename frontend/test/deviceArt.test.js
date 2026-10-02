/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect } from 'vitest';
import { GamepadArtView, gamepadArtSvg } from '../js/ui/GamepadArt.js';
import {
    rcArtSvg,
    flightstickArtSvg,
    wheelArtSvg,
    deviceArtSvg,
    kindIconSvg,
} from '../js/ui/DeviceArt.js';
import { WIZARD_STEPS } from '../js/ui/GamepadRemapDialog.js';
import { BUTTON_TARGETS } from '../js/stream/gamepadMapping.js';

/**
 * The radio, the flight stick and the wheel, drawn and driven by what the
 * game receives (readVirtualPad's standard pad): a rim that turns with the
 * steering, a switch lever that follows LT, a pedal pressed by RT.
 */
function mount(svg) {
    const root = document.createElement('div');
    root.innerHTML = svg;
    document.body.appendChild(root);
    return { root, view: new GamepadArtView(root) };
}

/** A standard pad: axes in W3C order, buttons by target name (0..1). */
function pad({ axes = [0, 0, 0, 0], buttons = {} } = {}) {
    return {
        axes,
        buttons: BUTTON_TARGETS.map((target) => {
            const v = buttons[target] ?? 0;
            return { pressed: v > 0.5, value: v };
        }),
    };
}

describe('device drawings', () => {
    it('turn the rim as far as a 900° wheel turns', () => {
        const { root, view } = mount(wheelArtSvg());
        const rim = root.querySelector('[data-rotate="leftx"]');
        // 180° to the right is 0.4 of the axis on G HUB's default 900°.
        view.render(pad({ axes: [0.4, 0, 0, 0] }), BUTTON_TARGETS);
        expect(rim.getAttribute('transform')).toBe('rotate(180.0 320 160)');
        // The left stop: a turn and a quarter, as the wheel itself.
        view.render(pad({ axes: [-1, 0, 0, 0] }), BUTTON_TARGETS);
        expect(rim.getAttribute('transform')).toBe('rotate(-450.0 320 160)');
    });

    it("move a radio switch's lever with LT: up, middle, down", () => {
        const { root, view } = mount(rcArtSvg());
        const lever = root.querySelector('[data-lever="lefttrigger"]');
        const knob = lever.querySelector('.gp-lever-knob');
        view.render(pad({ buttons: { lefttrigger: 0 } }), BUTTON_TARGETS);
        expect(knob.getAttribute('transform')).toBe('translate(0 -26.0)');
        view.render(pad({ buttons: { lefttrigger: 0.5 } }), BUTTON_TARGETS);
        expect(knob.getAttribute('transform')).toBe('translate(0 0.0)');
        view.render(pad({ buttons: { lefttrigger: 1 } }), BUTTON_TARGETS);
        expect(knob.getAttribute('transform')).toBe('translate(0 26.0)');
        expect(lever.querySelector('.gp-lever-stem').getAttribute('transform')).toContain(
            'scale(1 1.00)',
        );
        expect(
            root.querySelector('.gp-ctl[data-ctl="lefttrigger"]').classList.contains('is-pressed'),
        ).toBe(true);
    });

    it('press a pedal with its trigger, and slide the throttle with the left stick', () => {
        const wheel = mount(wheelArtSvg());
        wheel.view.render(pad({ buttons: { righttrigger: 0.5 } }), BUTTON_TARGETS);
        expect(
            wheel.root.querySelector('[data-press="righttrigger"]').getAttribute('transform'),
        ).toBe('translate(0.0 6.0)');
        // Full throttle is the left stick up: the handle and its buttons go up together.
        const stick = mount(flightstickArtSvg());
        stick.view.render(pad({ axes: [0, -1, 0, 0] }), BUTTON_TARGETS);
        const slides = stick.root.querySelectorAll('[data-press="lefty"]');
        expect(slides).toHaveLength(2);
        for (const s of slides) expect(s.getAttribute('transform')).toBe('translate(0.0 -76.0)');
    });

    it('move a gimbal by its own travel, and know it under both its axes', () => {
        const { root, view } = mount(rcArtSvg());
        view.render(pad({ axes: [1, 0, 0, 0] }), BUTTON_TARGETS);
        const gimbal = root.querySelector('[data-stick="left"]');
        expect(gimbal.querySelector('.gp-stick-cap').getAttribute('transform')).toBe(
            'translate(34.0 0.0)',
        );
        expect(gimbal.classList.contains('is-moved')).toBe(true);
        view.setTarget('lefty');
        expect(gimbal.classList.contains('is-target-y')).toBe(true);
    });

    it('come back to rest on clear(), and leave the pad drawn as it was', () => {
        const { root, view } = mount(wheelArtSvg());
        view.render(pad({ axes: [1, 0, 0, 0], buttons: { righttrigger: 1 } }), BUTTON_TARGETS);
        view.clear();
        expect(root.querySelector('[data-rotate="leftx"]').getAttribute('transform')).toBe(
            'rotate(0.0 320 160)',
        );
        expect(root.querySelector('[data-press="righttrigger"]').getAttribute('transform')).toBe(
            'translate(0.0 0.0)',
        );
        // The pad has none of the hooks: its sticks move as always.
        const plain = mount(gamepadArtSvg());
        plain.view.render(pad({ axes: [1, 1, 0, 0] }), BUTTON_TARGETS);
        expect(
            plain.root.querySelector('[data-stick="left"] .gp-stick-cap').getAttribute('transform'),
        ).toBe('translate(12.0 12.0)');
    });

    it("draw every control its kind's wizard asks for", () => {
        for (const kind of ['rc', 'flightstick', 'wheel']) {
            const { root } = mount(deviceArtSvg(kind));
            const drawn = new Set();
            for (const g of root.querySelectorAll('.gp-ctl')) {
                for (const name of g.getAttribute('data-ctl').split(/\s+/)) drawn.add(name);
            }
            const missing = WIZARD_STEPS[kind].filter((target) => !drawn.has(target));
            expect([kind, missing]).toEqual([kind, []]);
        }
    });

    it('give every kind a drawing and an icon, the pad for anything else', () => {
        for (const kind of ['gamepad', 'rc', 'flightstick', 'wheel']) {
            expect(deviceArtSvg(kind)).toContain('class="gp-art');
            expect(kindIconSvg(kind)).toContain('<svg');
        }
        // The pad's own svg is plain "gp-art"; the others add their kind.
        expect(deviceArtSvg('toaster')).toContain('<svg class="gp-art" ');
        expect(deviceArtSvg('wheel')).toContain('<svg class="gp-art gp-art-wheel" ');
    });
});
