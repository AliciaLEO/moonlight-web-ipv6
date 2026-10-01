/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi } from 'vitest';
import { GamepadRemapDialog, WIZARD_STEPS } from '../js/ui/GamepadRemapDialog.js';

/**
 * "Skip to the sticks": a radio or a joystick has none of the seventeen
 * buttons the wizard asks for first, and used to be walked through them one
 * Skip at a time.
 */
function wizardAt(target, bindings = {}) {
    return {
        _wiz: {
            idx: WIZARD_STEPS.indexOf(target),
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
        expect(WIZARD_STEPS[fake._wiz.idx]).toBe('leftx');
        expect(fake._wiz.bindings).toEqual({ a: { t: 'b', i: 0 }, b: { t: 'b', i: 1 } });
        expect(fake._wiz.waitRelease).toBe(false);
        expect(fake._wiz.caught).toBeNull();
        expect(fake._renderStatic).toHaveBeenCalledTimes(1);
    });

    it('does nothing once the sticks are reached', () => {
        const fake = wizardAt('lefty', { leftx: { t: 'a', i: 3, s: 0 } });
        GamepadRemapDialog.prototype._wizardSkipToSticks.call(fake);
        expect(WIZARD_STEPS[fake._wiz.idx]).toBe('lefty');
        expect(fake._renderStatic).not.toHaveBeenCalled();
    });
});
