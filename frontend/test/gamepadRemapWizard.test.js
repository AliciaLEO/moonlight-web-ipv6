/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi } from 'vitest';
import {
    GamepadRemapDialog,
    WIZARD_STEPS,
    stepPrompt,
    stepDir,
} from '../js/ui/GamepadRemapDialog.js';
import { BUTTON_TARGETS, AXIS_TARGETS } from '../js/stream/gamepadMapping.js';

/**
 * "Skip to the sticks": a device taken for a pad, with none of the seventeen
 * buttons the pad's wizard asks for first, used to be walked through them one
 * Skip at a time.
 */
function wizardAt(target, bindings = {}, kind = 'gamepad') {
    const steps = WIZARD_STEPS[kind];
    return {
        _wiz: {
            kind,
            steps,
            idx: steps.indexOf(target),
            bindings,
            caught: { t: 'b', i: 1 },
            waitRelease: true,
        },
        _renderStatic: vi.fn(),
    };
}

describe('remap wizard, skip to the sticks', () => {
    it('jumps to the first stick, keeping what is already bound', () => {
        const fake = wizardAt('x', { a: { t: 'b', i: 0 }, b: { t: 'b', i: 1 } });
        GamepadRemapDialog.prototype._wizardSkipToSticks.call(fake);
        expect(fake._wiz.steps[fake._wiz.idx]).toBe('leftx');
        expect(fake._wiz.bindings).toEqual({ a: { t: 'b', i: 0 }, b: { t: 'b', i: 1 } });
        expect(fake._wiz.waitRelease).toBe(false);
        expect(fake._wiz.caught).toBeNull();
        expect(fake._renderStatic).toHaveBeenCalledTimes(1);
    });

    it('does nothing once the sticks are reached', () => {
        const fake = wizardAt('lefty', { leftx: { t: 'a', i: 3, s: 0 } });
        GamepadRemapDialog.prototype._wizardSkipToSticks.call(fake);
        expect(fake._wiz.steps[fake._wiz.idx]).toBe('lefty');
        expect(fake._renderStatic).not.toHaveBeenCalled();
    });
});

describe('remap wizard, in the words of the device', () => {
    const targets = [...BUTTON_TARGETS, ...AXIS_TARGETS];

    it('walks each kind through Xbox controls only, each at most once', () => {
        for (const [kind, steps] of Object.entries(WIZARD_STEPS)) {
            expect(steps.every((s) => targets.includes(s))).toBe(true);
            expect(new Set(steps).size).toBe(steps.length);
            // Every kind maps the sticks it is held by.
            expect(steps).toContain('leftx');
            expect([kind, steps.length > 4]).toEqual([kind, true]);
        }
        // A radio starts with its sticks, a wheel with its rim.
        expect(WIZARD_STEPS.rc.slice(0, 4)).toEqual(['leftx', 'lefty', 'rightx', 'righty']);
        expect(WIZARD_STEPS.wheel.slice(0, 3)).toEqual(['leftx', 'righttrigger', 'lefttrigger']);
        expect(WIZARD_STEPS.flightstick.slice(0, 4)).toEqual([
            'rightx',
            'righty',
            'leftx',
            'lefty',
        ]);
    });

    it('asks a radio for its throttle and a wheel for its brake, a pad as before', () => {
        expect(stepPrompt('rc', 'lefty')).toBe('gamepad.remap.steps.rc.lefty');
        expect(stepPrompt('wheel', 'lefttrigger')).toBe('gamepad.remap.steps.wheel.lefttrigger');
        expect(stepPrompt('flightstick', 'dpup')).toBe('gamepad.remap.steps.flightstick.dpup');
        // A wheel's d-pad, and every pad step, keep the pad's words.
        expect(stepPrompt('wheel', 'dpup')).toBe('gamepad.remap.steps.dpup');
        expect(stepPrompt('gamepad', 'a')).toBe('gamepad.remap.steps.a');
        // The other buttons of a device: "the button that gives the game A".
        expect(stepPrompt('rc', 'a')).toBe('gamepad.remap.steps.button');
    });

    it('expects a throttle raised, every other stick pushed right or down', () => {
        expect(stepDir('rc', 'lefty')).toBe(-1);
        expect(stepDir('flightstick', 'lefty')).toBe(-1);
        expect(stepDir('gamepad', 'lefty')).toBe(1);
        expect(stepDir('wheel', 'leftx')).toBe(1);
        expect(stepDir('rc', 'righty')).toBe(1);
    });
});
