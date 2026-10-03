/*
 * MoonlightWeb — frontend TNR. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * A way out of the kebab menu. A TV remote's Back never reaches the page, and
 * RemoteNav keeps the arrows inside an open menu: the menu used to close only
 * on a click outside it, so a remote stayed locked in. Escape (a pad's B) and,
 * on a TV, a Close item of its own now close it, and the focus goes back to
 * the button that opened it.
 */
import { describe, it, expect, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: { getHosts: vi.fn(), getAppList: vi.fn() },
}));
vi.mock('../js/i18n/i18n.js', () => ({ t: (key) => key }));

import { HostListView } from '../js/ui/HostListView.js';

function build(menuOpen) {
    const container = document.createElement('div');
    document.body.innerHTML = '';
    document.body.appendChild(container);
    container.innerHTML = `<div class="host-card" data-uuid="h1">
        <div class="host-card-menu">
            <button class="btn-icon btn-host-menu" aria-expanded="${menuOpen}"></button>
            <div class="host-menu"${menuOpen ? '' : ' hidden'}>
                <button class="host-menu-item btn-share"></button>
                <button class="host-menu-item btn-menu-close"></button>
            </div>
        </div>
    </div>`;
    const view = Object.create(HostListView.prototype);
    view.container = container;
    view._staleCards = false;
    return { view, container };
}

describe('closing the kebab menu', () => {
    it('closes it and gives the focus back to the kebab', () => {
        const { view, container } = build(true);
        container.querySelector('.btn-share').focus();
        expect(view._closeMenuBackToButton()).toBe(true);
        expect(container.querySelector('.host-menu').hasAttribute('hidden')).toBe(true);
        const kebab = container.querySelector('.btn-host-menu');
        expect(kebab.getAttribute('aria-expanded')).toBe('false');
        expect(document.activeElement).toBe(kebab);
    });

    it('says so when no menu was open, so Escape is left to others', () => {
        const { view } = build(false);
        expect(view._closeMenuBackToButton()).toBe(false);
    });
});
