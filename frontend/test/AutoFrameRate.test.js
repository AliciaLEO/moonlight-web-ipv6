/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * "Auto" frame rate: the stream runs at the rate of the screen it lands on.
 * One measurement answers for both halves — the frames per second the host is
 * asked for, and the cadence its virtual display is created at — so what is
 * worth pinning down here is the reading of that measurement: a panel that
 * runs at 164.8 Hz is 165 FPS, and a screen that could not be measured leaves
 * the choice to the caller rather than inventing a rate.
 */
import { describe, it, expect } from 'vitest';
import {
    autoFps,
    measuredFps,
    AUTO_FPS_MIN,
    AUTO_FPS_MAX,
    LOW_MEMORY_AUTO_FPS_MAX,
} from '../js/util/RefreshRate.js';

describe('measuredFps', () => {
    it('rounds the measured millihertz to whole frames per second', () => {
        expect(measuredFps(59940)).toBe(60);
        expect(measuredFps(60000)).toBe(60);
        expect(measuredFps(119880)).toBe(120);
        expect(measuredFps(143900)).toBe(144);
        expect(measuredFps(164800)).toBe(165);
    });

    it('says what the panel runs at, with no ceiling on it', () => {
        expect(measuredFps(240000)).toBe(240);
        expect(measuredFps(0)).toBe(0);
    });
});

describe('autoFps', () => {
    it('rounds the measured millihertz to whole frames per second', () => {
        expect(autoFps(59940)).toBe(60);
        expect(autoFps(60000)).toBe(60);
        expect(autoFps(119880)).toBe(120);
    });

    it('stops at the ceiling a choice made for the viewer stops at', () => {
        // The panel is 144 or 165; Auto asks for 120 and the viewer who wants
        // the rest picks it by name in the list.
        expect(autoFps(143900)).toBe(AUTO_FPS_MAX);
        expect(autoFps(164800)).toBe(AUTO_FPS_MAX);
        expect(AUTO_FPS_MAX).toBe(120);
    });

    it('holds a device of a gigabyte or less to 30, a slower screen to its own rate', () => {
        // The Mi TV case: 60 asked, 34 decoded and 146 ms; 30 asked, 54 ms.
        expect(LOW_MEMORY_AUTO_FPS_MAX).toBe(30);
        expect(autoFps(60000, true)).toBe(30);
        expect(autoFps(50000, true)).toBe(30);
        expect(autoFps(25000, true)).toBe(25);
        expect(autoFps(60000, false)).toBe(60);
    });

    it('answers 0 when nothing was measured, so the caller keeps its default', () => {
        expect(autoFps(0)).toBe(0);
        expect(autoFps(-1)).toBe(0);
        expect(autoFps(NaN)).toBe(0);
    });

    it('stays within what an encoder and a client will carry', () => {
        expect(autoFps(1000)).toBe(AUTO_FPS_MIN);
        expect(autoFps(500000)).toBe(AUTO_FPS_MAX);
    });

    it('reads the last measurement when given no argument, 0 before any', () => {
        expect(autoFps()).toBe(0);
    });
});
