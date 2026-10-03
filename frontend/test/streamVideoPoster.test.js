/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect } from 'vitest';
import { BLANK_POSTER } from '../js/ui/StreamView.js';

/**
 * The stream <video> carries a poster of its own: without one, Android's
 * WebView paints a big grey "play" picture over it until the first frame
 * (seen on a Mi TV, behind the startup steps).
 */
describe('stream video poster', () => {
    it('is one transparent pixel, inline', () => {
        expect(BLANK_POSTER.startsWith('data:image/gif;base64,')).toBe(true);
        const bytes = Uint8Array.from(atob(BLANK_POSTER.split(',')[1]), (c) => c.charCodeAt(0));
        expect(String.fromCharCode(...bytes.slice(0, 6))).toBe('GIF89a');
        // Logical screen: 1 × 1.
        expect(bytes[6] | (bytes[7] << 8)).toBe(1);
        expect(bytes[8] | (bytes[9] << 8)).toBe(1);
        // A graphic control extension that marks colour 0 transparent.
        const gce = bytes.findIndex((b, i) => b === 0x21 && bytes[i + 1] === 0xf9);
        expect(gce).toBeGreaterThan(0);
        expect(bytes[gce + 3] & 1).toBe(1);
    });
});
