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
    unmixedChannel,
} from '../js/ui/GamepadRemapDialog.js';
import {
    BUTTON_TARGETS,
    AXIS_TARGETS,
    resolveMapping,
    snapshot,
} from '../js/stream/gamepadMapping.js';

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

/**
 * A radio sends its channels and nothing else: a switch its model does not
 * mix never reaches the browser. Bruno's TX12 (02/10/2026), on a model that
 * mixed only the sticks, gave the wizard nothing to catch past them.
 */
describe('remap wizard, a radio switch that stays silent', () => {
    /** A TX12 at rest as Chrome reports it: 8 channel axes, 24 buttons. */
    const radio = () => ({
        id: 'Radiomaster TX12 Joystick (Vendor: 1209 Product: 4f54)',
        mapping: '',
        connected: true,
        axes: new Array(8).fill(0),
        buttons: Array.from({ length: 24 }, () => ({ pressed: false, value: 0 })),
    });

    /** The dialog's own elements, for _renderStatic run on a stand-in. */
    function dialogEls() {
        const root = document.createElement('div');
        root.innerHTML = `
            <div class="stage"></div><div class="empty"></div>
            <span class="prompt"></span><span class="raw"></span>
            <div class="progress"><span></span></div>
            <p class="hint" hidden></p><div class="actions"></div>`;
        const q = (s) => root.querySelector(s);
        return {
            stage: q('.stage'),
            empty: q('.empty'),
            prompt: q('.prompt'),
            raw: q('.raw'),
            progress: q('.progress'),
            bar: q('.progress > span'),
            hint: q('.hint'),
            actions: q('.actions'),
        };
    }

    it('names the channel the radio profile reads each switch on', () => {
        const { bindings } = resolveMapping(radio(), { platform: 'win' });
        const sticks = ['leftx', 'lefty', 'rightx', 'righty'];
        for (const target of WIZARD_STEPS.rc) {
            const b = bindings[target];
            // CH1-8 are the axes 0-7, CH9 and up the buttons 0 and up.
            const ch = b.t === 'a' ? b.i + 1 : b.i + 9;
            expect([target, unmixedChannel('rc', target)]).toEqual([
                target,
                sticks.includes(target) ? 0 : ch,
            ]);
        }
        // Only a radio has switches to mix.
        expect(unmixedChannel('wheel', 'a')).toBe(0);
        expect(unmixedChannel('gamepad', 'lefttrigger')).toBe(0);
    });

    it('says which channel to mix after 4 s on a silent switch, once', () => {
        const gp = radio();
        const fake = wizardAt('a', {}, 'rc');
        Object.assign(fake._wiz, { waitRelease: false, caught: null, base: snapshot(gp) });
        const now = vi.spyOn(performance, 'now');
        const frameAt = (ms) => {
            now.mockReturnValue(ms);
            GamepadRemapDialog.prototype._wizardFrame.call(fake, gp);
        };
        try {
            frameAt(1000);
            frameAt(4900);
            expect(fake._wiz.silent).toBe(false);
            expect(fake._renderStatic).not.toHaveBeenCalled();
            frameAt(5100);
            expect(fake._wiz.silent).toBe(true);
            frameAt(9000);
            expect(fake._renderStatic).toHaveBeenCalledTimes(1);
            // The next step starts its own clock.
            GamepadRemapDialog.prototype._wizardSkip.call(fake);
            frameAt(9100);
            expect(fake._wiz.silent).toBe(false);
        } finally {
            now.mockRestore();
        }
    });

    it('never on a stick, nor on a device other than a radio', () => {
        const gp = radio();
        const now = vi.spyOn(performance, 'now');
        try {
            for (const [target, kind] of [
                ['leftx', 'rc'],
                ['lefttrigger', 'gamepad'],
                ['a', 'wheel'],
            ]) {
                const fake = wizardAt(target, {}, kind);
                Object.assign(fake._wiz, { waitRelease: false, base: snapshot(gp) });
                now.mockReturnValue(0);
                GamepadRemapDialog.prototype._wizardFrame.call(fake, gp);
                now.mockReturnValue(60000);
                GamepadRemapDialog.prototype._wizardFrame.call(fake, gp);
                expect([target, kind, fake._wiz.silent]).toEqual([target, kind, false]);
            }
        } finally {
            now.mockRestore();
        }
    });

    it('shows the hint under the step, and the mixing in Test', () => {
        const art = { setTarget: vi.fn(), setMapped: vi.fn() };
        const wizard = {
            _els: dialogEls(),
            _art: art,
            _view: 'wizard',
            _selectedPad: () => null,
            _wiz: { ...wizardAt('a', {}, 'rc')._wiz, waitRelease: false, caught: null },
        };
        wizard._wiz.stepIdx = wizard._wiz.idx;
        wizard._wiz.silent = true;
        GamepadRemapDialog.prototype._renderStatic.call(wizard);
        expect(wizard._els.hint.hidden).toBe(false);
        expect(wizard._els.hint.textContent).toBe('gamepad.remap.rcUnmixedHint');

        // Test reads the radio through its profile: a desktop browser's.
        const ua = vi
            .spyOn(navigator, 'userAgent', 'get')
            .mockReturnValue('Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/154.0');
        const gp = radio();
        const test = {
            _els: dialogEls(),
            _art: art,
            _view: 'test',
            _key: 'usb:1209:4f54',
            _selectedPad: () => gp,
        };
        try {
            GamepadRemapDialog.prototype._renderStatic.call(test);
        } finally {
            ua.mockRestore();
        }
        expect(test._els.hint.textContent).toBe('gamepad.remap.guessed gamepad.remap.rcMixHint');
    });
});
