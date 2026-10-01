/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

// The admin's apps in gamescope (a Linux native host, plan « Idées Punktfunk »,
// chapter G): a card each on the host, a name and the command the host runs.
// Shown only where the server says gamescope is usable, the whole list saved
// at each change, and the list shown afterwards always the server's.
vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: { getStreamingSettings: vi.fn(), saveStreamingSettings: vi.fn() },
}));
vi.mock('../js/ui/Toast.js', () => ({
    Toast: { success: vi.fn(), error: vi.fn(), warning: vi.fn() },
}));

import { AdminView } from '../js/ui/AdminView.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { Toast } from '../js/ui/Toast.js';

describe('AdminView — apps in gamescope', () => {
    let view;

    const $ = (sel) => document.querySelector(sel);
    const mount = () => {
        document.body.innerHTML = `<div>${view._renderGamescopeApps()}</div>`;
        view.container = document.body;
        view._bindGamescopeApps();
    };
    const type = (sel, value) => {
        $(sel).value = value;
        $(sel).dispatchEvent(new Event('input'));
    };

    beforeEach(() => {
        document.body.innerHTML = '<div></div>';
        view = new AdminView(document.body, () => {});
        BackendClient.getStreamingSettings.mockReset();
        BackendClient.saveStreamingSettings.mockReset();
        Toast.success.mockReset();
        Toast.error.mockReset();
    });

    afterEach(() => {
        view.destroy();
    });

    it('reads the apps, the limit and whether this machine can run them', async () => {
        BackendClient.getStreamingSettings.mockResolvedValue({
            gamescope_apps: [{ name: 'RetroArch', command: 'retroarch' }],
            gamescope_apps_supported: true,
            gamescope_apps_max: 8,
        });
        await view._loadStreamingState();
        expect(view._gamescopeApps).toEqual([{ name: 'RetroArch', command: 'retroarch' }]);
        expect(view._gamescopeAppsSupported).toBe(true);
        expect(view._gamescopeAppsMax).toBe(8);
    });

    it('stays hidden when the server says nothing (an older backend, another OS)', async () => {
        BackendClient.getStreamingSettings.mockResolvedValue({});
        await view._loadStreamingState();
        expect(view._gamescopeAppsSupported).toBe(false);
        expect(view._gamescopeApps).toEqual([]);
    });

    it('lists each app, its name and command escaped', () => {
        view._gamescopeApps = [{ name: '<b>Emu</b>', command: 'emu --x "a&b"' }];
        mount();
        const row = $('.gamescope-app-row');
        expect(row.querySelector('.gamescope-app-name').textContent).toBe('<b>Emu</b>');
        expect(row.querySelector('.gamescope-app-command').textContent).toBe('emu --x "a&b"');
        expect(row.querySelector('b')).toBeNull();
    });

    it('says so when there is none yet', () => {
        mount();
        expect($('.gamescope-app-list')).toBeNull();
        expect(document.body.textContent).toContain('admin.gamescopeAppsEmpty');
    });

    it('adds an app once both fields are filled, saving the whole list', async () => {
        view._gamescopeApps = [{ name: 'A', command: 'a' }];
        const saved = [
            { name: 'A', command: 'a' },
            { name: 'Heroic', command: 'heroic' },
        ];
        BackendClient.saveStreamingSettings.mockResolvedValue({ gamescope_apps: saved });
        mount();
        expect($('#btn-gamescope-add').disabled).toBe(true);
        type('#gamescope-app-name', '  Heroic ');
        expect($('#btn-gamescope-add').disabled).toBe(true);
        type('#gamescope-app-command', 'heroic');
        expect($('#btn-gamescope-add').disabled).toBe(false);
        $('#btn-gamescope-add').click();
        await vi.waitFor(() => expect(Toast.success).toHaveBeenCalled());
        expect(BackendClient.saveStreamingSettings).toHaveBeenCalledWith({ gamescope_apps: saved });
        expect(view._gamescopeApps).toEqual(saved);
        // Drawn again from what the server kept, the form empty.
        expect(document.querySelectorAll('.gamescope-app-row').length).toBe(2);
        expect($('#gamescope-app-name').value).toBe('');
    });

    it('removes an app', async () => {
        view._gamescopeApps = [
            { name: 'A', command: 'a' },
            { name: 'B', command: 'b' },
        ];
        BackendClient.saveStreamingSettings.mockResolvedValue({
            gamescope_apps: [{ name: 'B', command: 'b' }],
        });
        mount();
        document.querySelector('[data-gamescope-remove="0"]').click();
        await vi.waitFor(() => expect(Toast.success).toHaveBeenCalled());
        expect(BackendClient.saveStreamingSettings).toHaveBeenCalledWith({
            gamescope_apps: [{ name: 'B', command: 'b' }],
        });
        expect(document.querySelectorAll('.gamescope-app-row').length).toBe(1);
    });

    it('keeps the list as it was when the server refuses it', async () => {
        view._gamescopeApps = [{ name: 'A', command: 'a' }];
        BackendClient.saveStreamingSettings.mockRejectedValue(new Error('16 apps at most'));
        mount();
        document.querySelector('[data-gamescope-remove="0"]').click();
        await vi.waitFor(() => expect(Toast.error).toHaveBeenCalled());
        expect(view._gamescopeApps).toEqual([{ name: 'A', command: 'a' }]);
        expect(document.querySelectorAll('.gamescope-app-row').length).toBe(1);
        expect(Toast.success).not.toHaveBeenCalled();
    });

    it('takes no more once the list is full', () => {
        view._gamescopeAppsMax = 2;
        view._gamescopeApps = [
            { name: 'A', command: 'a' },
            { name: 'B', command: 'b' },
        ];
        mount();
        expect($('#gamescope-app-name').disabled).toBe(true);
        expect($('#gamescope-app-command').disabled).toBe(true);
        expect(document.body.textContent).toContain('admin.gamescopeAppsFull');
    });
});
