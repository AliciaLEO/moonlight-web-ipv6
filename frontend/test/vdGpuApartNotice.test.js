/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach } from 'vitest';
import { takeVdGpuApartNotice } from '../js/util/vdGpuApartNotice.js';

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
