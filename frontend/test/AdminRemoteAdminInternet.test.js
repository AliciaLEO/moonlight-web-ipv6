/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

// The internet side of the remote admin door: shut until the owner opens it,
// offered only while the LAN's door is open, saved the moment the box
// changes, and a remote admin who closes it on itself is sent back to a page
// it can still use.
vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: { getAuthStatus: vi.fn(), saveRemoteAdmin: vi.fn() },
}));
vi.mock('../js/ui/Toast.js', () => ({
    Toast: { success: vi.fn(), error: vi.fn(), warning: vi.fn() },
}));

import { AdminView } from '../js/ui/AdminView.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { Toast } from '../js/ui/Toast.js';

describe('AdminView — remote admin from the internet', () => {
    let view;
    let original;
    let reload;

    const box = () => document.querySelector('#chk-remote-admin-internet');
    const reply = (internet) => ({
        status: 'ok',
        remote_admin_enabled: true,
        admin_password_set: true,
        remote_admin_internet: internet,
    });

    beforeEach(() => {
        vi.useFakeTimers();
        document.body.innerHTML = '<div></div>';
        view = new AdminView(document.body, () => {});
        // The whole page is rebuilt after a save; these tests look at the
        // section and the calls, not at the page.
        view.render = vi.fn();
        view.bindEvents = vi.fn();
        BackendClient.getAuthStatus.mockReset();
        BackendClient.saveRemoteAdmin.mockReset();
        Toast.success.mockReset();
        Toast.error.mockReset();
        reload = vi.fn();
        original = window.location;
        Object.defineProperty(window, 'location', { value: { reload }, configurable: true });
    });

    afterEach(() => {
        Object.defineProperty(window, 'location', { value: original, configurable: true });
        vi.useRealTimers();
        view.destroy();
    });

    it('is shut until the server says otherwise, an older backend included', async () => {
        expect(view._remoteAdminInternet).toBe(false);
        BackendClient.getAuthStatus.mockResolvedValue({
            is_localhost: true,
            remote_admin_enabled: true,
            admin_password_set: true,
        });
        await view._loadAuthStatus();
        expect(view._remoteAdminInternet).toBe(false);

        BackendClient.getAuthStatus.mockResolvedValue({
            is_localhost: true,
            remote_admin_enabled: true,
            admin_password_set: true,
            remote_admin_internet: true,
        });
        await view._loadAuthStatus();
        expect(view._remoteAdminInternet).toBe(true);
    });

    it('is offered under the LAN door, never without it', () => {
        view._remoteAdminEnabled = true;
        view._remoteAdminInternet = true;
        document.body.innerHTML = view._renderRemoteAdmin();
        expect(box()).not.toBeNull();
        expect(box().checked).toBe(true);

        view._remoteAdminInternet = false;
        document.body.innerHTML = view._renderRemoteAdmin();
        expect(box().checked).toBe(false);

        view._remoteAdminEnabled = false;
        document.body.innerHTML = view._renderRemoteAdmin();
        expect(box()).toBeNull();
    });

    it('opens the internet side with one call and keeps the page', async () => {
        BackendClient.saveRemoteAdmin.mockResolvedValue(reply(true));
        await view._toggleRemoteAdminInternet(true);
        expect(BackendClient.saveRemoteAdmin).toHaveBeenCalledWith({ internet: true });
        expect(view._remoteAdminInternet).toBe(true);
        expect(Toast.success).toHaveBeenCalled();
        vi.runAllTimers();
        expect(reload).not.toHaveBeenCalled();
        expect(view.render).toHaveBeenCalled();
    });

    it('sends a remote admin that closes it back to a page it can use', async () => {
        view._isRealHostMachine = false;
        BackendClient.saveRemoteAdmin.mockResolvedValue(reply(false));
        await view._toggleRemoteAdminInternet(false);
        expect(BackendClient.saveRemoteAdmin).toHaveBeenCalledWith({ internet: false });
        vi.runAllTimers();
        expect(reload).toHaveBeenCalled();
    });

    it('lets the host machine close it without leaving the page', async () => {
        view._isRealHostMachine = true;
        BackendClient.saveRemoteAdmin.mockResolvedValue(reply(false));
        await view._toggleRemoteAdminInternet(false);
        vi.runAllTimers();
        expect(reload).not.toHaveBeenCalled();
        expect(view._remoteAdminInternet).toBe(false);
    });

    it('shows what the server kept when it refuses', async () => {
        BackendClient.saveRemoteAdmin.mockRejectedValue(new Error('forbidden'));
        await view._toggleRemoteAdminInternet(true);
        expect(Toast.error).toHaveBeenCalled();
        expect(view._remoteAdminInternet).toBe(false);
        expect(view.render).toHaveBeenCalled();
    });
});
