/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach } from 'vitest';
import { VdGpuApartWatch, takeVdGpuApartNotice } from '../js/util/vdGpuApartNotice.js';

const apart = { vd_gpu_apart: { display: 'NVIDIA GeForce GTX 1050', apps: 'AMD Radeon 780M' } };

describe('takeVdGpuApartNotice', () => {
    beforeEach(() => localStorage.clear());

    it('says nothing when the host reports no split', () => {
        expect(takeVdGpuApartNotice({ status: 'streaming' }, 'h1')).toBeNull();
        expect(takeVdGpuApartNotice({ vd_gpu_apart: { display: 'A' } }, 'h1')).toBeNull();
    });

    it('tells once per host and pair of GPUs', () => {
        expect(takeVdGpuApartNotice(apart, 'h1')).toEqual({
            display: 'NVIDIA GeForce GTX 1050',
            apps: 'AMD Radeon 780M',
        });
        expect(takeVdGpuApartNotice(apart, 'h1')).toBeNull();
        // Another host, or another pair on the same host, is news.
        expect(takeVdGpuApartNotice(apart, 'h2')).not.toBeNull();
        expect(
            takeVdGpuApartNotice({ vd_gpu_apart: { display: 'X', apps: 'Y' } }, 'h1'),
        ).not.toBeNull();
    });
});

describe('VdGpuApartWatch', () => {
    const win = (ms) => ({ encode: { n: 10, avg: ms * 1000 } });

    it('fires once the encode stage stays stalled two windows in a row', () => {
        const w = new VdGpuApartWatch();
        // The UM790Pro: 5 ms normally, 130 ms while Steam's cards were hovered.
        expect(w.note(win(5))).toBe(false);
        expect(w.note(win(130))).toBe(false);
        expect(w.note(win(140))).toBe(true);
        // Once only.
        expect(w.note(win(150))).toBe(false);
        expect(w.note(win(150))).toBe(false);
    });

    it('ignores a single slow window, and windows without frames', () => {
        const w = new VdGpuApartWatch();
        expect(w.note(win(130))).toBe(false);
        expect(w.note(win(5))).toBe(false);
        expect(w.note(win(130))).toBe(false);
        expect(w.note({ encode: { n: 0, avg: 0 } })).toBe(false);
        expect(w.note(undefined)).toBe(false);
        expect(w.note(win(12))).toBe(false);
    });
});
