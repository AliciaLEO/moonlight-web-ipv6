/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The way back to the estimated bitrate. Any move of the slider makes the
 * value the user's own, and a stray click on its track (a TV's cursor,
 * 02/10/2026: 98 Mbps for a 720p30 stream) used to stay for good, with a
 * reset of every setting as the only undo. A "Back to auto" button now
 * appears once the estimate is left, and puts it back.
 */
import { describe, it, expect, beforeEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        getAuthStatus: vi.fn(async () => ({ has_session: false })),
        getStreamingSettings: vi.fn(async () => ({})),
        saveStreamingSettings: vi.fn(async () => ({})),
        getMetricsReporting: vi.fn(),
        setMetricsReporting: vi.fn(async () => ({ enabled: true })),
    },
}));
vi.mock('../js/ui/Toast.js', () => ({
    Toast: { success: vi.fn(), error: vi.fn(), warning: vi.fn(), info: vi.fn() },
}));
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key) => 'text:' + key,
    getLanguage: () => 'en',
    setLanguage: vi.fn(),
    AVAILABLE_LANGUAGES: [{ code: 'en', label: 'English' }],
}));

import { SettingsView } from '../js/ui/SettingsView.js';

describe('SettingsView: back to the estimated bitrate', () => {
    let view;
    const $ = (sel) => view.container.querySelector(sel);

    beforeEach(() => {
        localStorage.clear();
        document.body.innerHTML = '<div id="settings"></div>';
        vi.clearAllMocks();
        view = new SettingsView(document.getElementById('settings'), () => {});
        view.render();
        view.bindEvents();
    });

    it('is hidden while the bitrate is the estimate', () => {
        expect($('#settings-bitrate-back-to-auto').hidden).toBe(true);
    });

    it('shows once the slider is moved off it, and puts the estimate back', () => {
        const slider = $('#settings-stream-bitrate');
        const estimate = slider.value;
        slider.value = '98';
        slider.dispatchEvent(new Event('change'));
        expect(view._bitrateAuto).toBe(false);
        expect($('#settings-bitrate-back-to-auto').hidden).toBe(false);

        $('#settings-bitrate-back-to-auto').click();
        expect(view._bitrateAuto).toBe(true);
        expect(slider.value).toBe(estimate);
        expect($('#settings-bitrate-value').textContent).toBe(estimate);
        expect($('#settings-bitrate-back-to-auto').hidden).toBe(true);
        expect($('#settings-bitrate-auto').textContent).toBe('text:settings.bitrateAuto');
    });
});
