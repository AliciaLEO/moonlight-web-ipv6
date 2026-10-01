/*
 * MoonlightWeb — Browser-based Moonlight streaming client.
 * Copyright (C) 2026 Bruno Martin.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * GamepadRemapDialog — check a controller, and map it control by control.
 *
 * Two modes over the same drawn pad (GamepadArt):
 *   - test:   the pad as MoonlightWeb reads it right now (user mapping,
 *             standard layout, or a guessed one), lit live;
 *   - wizard: one control at a time — "Press A", "Pull LT", "Push the left
 *             stick right" — each answered by the first raw input that moves
 *             (gamepadMapping.detectInput), then saved for this pad in this
 *             browser (gamepadMappingsStore).
 *
 * The dialog reads the pad itself; a stream running underneath is paused by
 * the caller (onOpen / onClose) so mapping "A" does not press A in the game.
 */

import { t } from '../i18n/i18n.js';
import { escapeHtml } from '../util/escapeHtml.js';
import { Toast } from './Toast.js';
import { gamepadArtSvg, GamepadArtView } from './GamepadArt.js';
import {
    BUTTON_TARGETS,
    resolveMapping,
    readVirtualPad,
    loadGamepadDb,
    padKey,
    padName,
    snapshot,
    detectInput,
    isAtRest,
    describeBinding,
    isAutoMapped,
} from '../stream/gamepadMapping.js';
import { getMapping, setMapping, removeMapping } from '../util/gamepadMappingsStore.js';

/** Wizard order: face, shoulders, the small buttons, clicks, d-pad, sticks. */
export const WIZARD_STEPS = [
    'a',
    'b',
    'x',
    'y',
    'leftshoulder',
    'rightshoulder',
    'lefttrigger',
    'righttrigger',
    'back',
    'start',
    'guide',
    'leftstick',
    'rightstick',
    'dpup',
    'dpright',
    'dpdown',
    'dpleft',
    'leftx',
    'lefty',
    'rightx',
    'righty',
];

/** Badge class + label for a resolution source. */
export function sourceBadge(source) {
    const k = source === 'pending' ? 'db' : source || 'none';
    return `<span class="gp-badge gp-badge-${k}">${escapeHtml(t(`gamepad.remap.source.${k}`))}</span>`;
}

/** Connected pads, in browser order. */
export function connectedPads() {
    const list = navigator.getGamepads ? Array.from(navigator.getGamepads()) : [];
    return list.filter((gp) => gp && gp.connected !== false);
}

let dbCache; // undefined until loadGamepadDb() answers

/** How MoonlightWeb reads `gp` right now (database loaded on demand). */
export function resolvePad(gp) {
    const res = resolveMapping(gp, { user: getMapping, db: dbCache });
    if (res.source === 'pending') {
        loadGamepadDb().then((db) => {
            dbCache = db;
        });
    }
    return res;
}

export class GamepadRemapDialog {
    /**
     * @param {{ container?: HTMLElement, padKey?: string|null,
     *           mode?: 'test'|'wizard'|'auto', required?: boolean,
     *           onOpen?: () => void, onClose?: () => void,
     *           onSaved?: (key: string) => void,
     *           onPadChange?: (key: string) => void }} [opts]
     *   `mode: 'auto'` opens the wizard for a pad nothing maps, the test view
     *   otherwise. `required` (a guest who cannot join unmapped) closes the
     *   dialog as soon as the mapping is saved.
     */
    constructor(opts = {}) {
        this._container = opts.container || document.body;
        this._key = opts.padKey || null;
        this._mode = opts.mode || 'auto';
        this._required = opts.required === true;
        this._onOpen = opts.onOpen || null;
        this._onClose = opts.onClose || null;
        this._onSaved = opts.onSaved || null;
        this._onPadChange = opts.onPadChange || null;
        this._overlay = null;
        this._raf = null;
        this._padsSig = '';
        this._wiz = null;
        this._onKey = (e) => {
            if (e.key !== 'Escape') return;
            e.preventDefault();
            e.stopPropagation();
            this.close();
        };
    }

    open() {
        if (this._overlay) return;
        if (document.pointerLockElement) document.exitPointerLock();
        loadGamepadDb().then((db) => {
            dbCache = db;
        });

        const overlay = document.createElement('div');
        overlay.className = 'gamepad-remap-overlay';
        overlay.innerHTML = `
            <div class="gamepad-remap" role="dialog" aria-modal="true" aria-labelledby="gamepad-remap-title">
                <div class="gamepad-remap-head">
                    <h3 class="gamepad-remap-title" id="gamepad-remap-title">${escapeHtml(t('gamepad.remap.title'))}</h3>
                    <div class="gamepad-remap-pad"></div>
                </div>
                <div class="gamepad-remap-stage">
                    ${gamepadArtSvg()}
                    <div class="gamepad-remap-empty" hidden>${escapeHtml(t('gamepad.remap.empty'))}</div>
                </div>
                <div class="gamepad-remap-status">
                    <span class="gamepad-remap-prompt"></span>
                    <span class="gamepad-remap-raw"></span>
                </div>
                <div class="gamepad-remap-progress" hidden><span></span></div>
                <p class="gamepad-remap-hint" hidden></p>
                <div class="gamepad-remap-actions"></div>
            </div>`;
        this._container.appendChild(overlay);
        this._overlay = overlay;
        this._art = new GamepadArtView(overlay.querySelector('.gamepad-remap-stage'));
        /** @type {Object<string, HTMLElement>} */
        this._els = {
            pad: overlay.querySelector('.gamepad-remap-pad'),
            stage: overlay.querySelector('.gamepad-remap-stage'),
            empty: overlay.querySelector('.gamepad-remap-empty'),
            prompt: overlay.querySelector('.gamepad-remap-prompt'),
            raw: overlay.querySelector('.gamepad-remap-raw'),
            progress: overlay.querySelector('.gamepad-remap-progress'),
            bar: overlay.querySelector('.gamepad-remap-progress > span'),
            hint: overlay.querySelector('.gamepad-remap-hint'),
            actions: overlay.querySelector('.gamepad-remap-actions'),
        };
        overlay.addEventListener('click', (e) => {
            if (e.target === overlay) this.close();
        });
        document.addEventListener('keydown', this._onKey, true);
        if (this._onOpen) this._onOpen();

        this._syncPads(true);
        this._loop();
    }

    close() {
        if (!this._overlay) return;
        if (this._raf !== null) cancelAnimationFrame(this._raf);
        this._raf = null;
        document.removeEventListener('keydown', this._onKey, true);
        this._overlay.remove();
        this._overlay = null;
        if (this._onClose) this._onClose();
    }

    get isOpen() {
        return !!this._overlay;
    }

    // ── Pads ─────────────────────────────────────────────────────────────

    _selectedPad() {
        const pads = connectedPads();
        if (!this._key) return null;
        return pads.find((gp) => padKey(gp) === this._key) || null;
    }

    /** Rebuild the pad picker when pads come and go. */
    _syncPads(force = false) {
        const pads = connectedPads();
        const sig = pads.map((gp) => `${gp.index}:${gp.id}`).join('|');
        if (!force && sig === this._padsSig) return;
        this._padsSig = sig;

        if (!this._key || !pads.some((gp) => padKey(gp) === this._key)) {
            const first = pads[0];
            const changed = first && padKey(first) !== this._key;
            this._key = first ? padKey(first) : this._key;
            if (changed && this._onPadChange) this._onPadChange(this._key);
            this._wiz = null;
        }

        const res = pads.length ? resolvePad(this._selectedPad() || pads[0]) : null;
        if (pads.length > 1) {
            this._els.pad.innerHTML = `
                <select class="gamepad-remap-select" aria-label="${escapeHtml(t('gamepad.remap.pick'))}">
                    ${pads
                        .map((gp) => {
                            const k = padKey(gp);
                            return `<option value="${escapeHtml(k)}" ${k === this._key ? 'selected' : ''}>${escapeHtml(padName(gp))}</option>`;
                        })
                        .join('')}
                </select>
                ${res ? sourceBadge(res.source) : ''}`;
            this._els.pad.querySelector('select').addEventListener('change', (e) => {
                this._key = /** @type {HTMLSelectElement} */ (e.target).value;
                this._wiz = null;
                if (this._onPadChange) this._onPadChange(this._key);
                this._syncPads(true);
            });
        } else if (pads.length === 1) {
            this._els.pad.innerHTML = `
                <span class="gamepad-remap-name">${escapeHtml(padName(pads[0]))}</span>
                ${sourceBadge(res.source)}`;
        } else {
            this._els.pad.innerHTML = '';
        }
        this._enterMode();
    }

    /** Pick the view for the selected pad, and draw its buttons. */
    _enterMode() {
        const gp = this._selectedPad();
        if (!gp) {
            this._view = 'empty';
        } else if (this._wiz) {
            this._view = 'wizard';
        } else {
            const res = resolvePad(gp);
            const unmapped = res.source === null;
            if (this._mode === 'wizard' || (this._mode === 'auto' && unmapped)) {
                this._startWizard(gp);
                return;
            }
            this._view = 'test';
        }
        this._renderStatic();
    }

    // ── Wizard ───────────────────────────────────────────────────────────

    _startWizard(gp) {
        this._wiz = {
            idx: 0,
            bindings: {},
            base: snapshot(gp),
            // Whatever is held when the wizard starts must be let go first.
            waitRelease: false,
            caught: null,
        };
        this._view = 'wizard';
        this._art.clearMarks();
        this._renderStatic();
    }

    _wizardFrame(gp) {
        const w = this._wiz;
        if (w.idx >= WIZARD_STEPS.length) return;
        const target = WIZARD_STEPS[w.idx];
        if (w.waitRelease) {
            if (isAtRest(w.base, gp)) {
                w.waitRelease = false;
                this._renderStatic();
            }
            return;
        }
        const exclude = Object.values(w.bindings);
        const b = detectInput(w.base, gp, target, exclude);
        if (!b) return;
        w.bindings[target] = b;
        w.caught = b;
        this._art.setMapped(target, true);
        w.idx++;
        w.waitRelease = true;
        this._renderStatic();
    }

    _wizardBack() {
        const w = this._wiz;
        if (!w || w.idx === 0) return;
        w.idx--;
        const target = WIZARD_STEPS[w.idx];
        delete w.bindings[target];
        this._art.setMapped(target, false);
        w.caught = null;
        w.waitRelease = false;
        this._renderStatic();
    }

    _wizardSkip() {
        const w = this._wiz;
        if (!w || w.idx >= WIZARD_STEPS.length) return;
        delete w.bindings[WIZARD_STEPS[w.idx]];
        w.idx++;
        w.caught = null;
        w.waitRelease = false;
        this._renderStatic();
    }

    _save() {
        const gp = this._selectedPad();
        const w = this._wiz;
        if (!gp || !w) return;
        setMapping(this._key, padName(gp), w.bindings);
        this._wiz = null;
        this._art.clearMarks();
        Toast.success(t('gamepad.remap.saved', { name: padName(gp) }));
        if (this._onSaved) this._onSaved(this._key);
        if (this._required) {
            this.close();
            return;
        }
        this._mode = 'test';
        this._syncPads(true);
    }

    _reset() {
        if (!this._key) return;
        removeMapping(this._key);
        this._wiz = null;
        this._mode = 'auto';
        if (this._onSaved) this._onSaved(this._key);
        this._syncPads(true);
    }

    // ── Rendering ────────────────────────────────────────────────────────

    /** Prompt, hint and buttons: redrawn on each change of step or view. */
    _renderStatic() {
        const els = this._els;
        const gp = this._selectedPad();
        els.stage.classList.toggle('is-empty', this._view === 'empty');
        els.empty.hidden = this._view !== 'empty';
        els.progress.hidden = this._view !== 'wizard';
        els.raw.textContent = '';
        els.raw.classList.remove('is-caught');
        this._art.setTarget(null);

        const btn = (cls, key, variant = 'btn-secondary') =>
            `<button type="button" class="btn ${variant} ${cls}">${escapeHtml(t(key))}</button>`;
        let hint = '';
        let actions = '';

        if (this._view === 'empty') {
            els.prompt.textContent = t('gamepad.remap.empty');
            actions = `<span class="gp-spacer"></span>${btn('gp-close', 'gamepad.remap.close')}`;
        } else if (this._view === 'test') {
            const res = resolvePad(gp);
            els.prompt.textContent = t(
                res.source ? 'gamepad.remap.testPrompt' : 'gamepad.remap.unknown',
            );
            if (isAutoMapped(res.source)) hint = t('gamepad.remap.guessed');
            actions =
                (res.source === 'user' ? btn('gp-reset', 'gamepad.remap.reset') : '') +
                `<span class="gp-spacer"></span>` +
                btn('gp-close', 'gamepad.remap.close') +
                btn(
                    'gp-remap',
                    res.source ? 'gamepad.remap.remap' : 'gamepad.remap.start',
                    'btn-save',
                );
        } else {
            const w = this._wiz;
            const done = w.idx >= WIZARD_STEPS.length;
            els.bar.style.width = `${Math.round((w.idx / WIZARD_STEPS.length) * 100)}%`;
            if (done) {
                els.prompt.textContent = t('gamepad.remap.done');
            } else if (w.waitRelease) {
                els.prompt.textContent = t('gamepad.remap.release');
            } else {
                const target = WIZARD_STEPS[w.idx];
                els.prompt.innerHTML = `<span class="gamepad-remap-step">${w.idx + 1}/${WIZARD_STEPS.length}</span> · ${escapeHtml(t(`gamepad.remap.steps.${target}`))}`;
                this._art.setTarget(target);
                if (target === 'guide') hint = t('gamepad.remap.guideHint');
            }
            if (w.caught) {
                els.raw.textContent = describeBinding(w.caught);
                els.raw.classList.add('is-caught');
            }
            actions =
                btn('gp-back', 'gamepad.remap.back') +
                (done ? '' : btn('gp-skip', 'gamepad.remap.skip')) +
                `<span class="gp-spacer"></span>` +
                btn('gp-close', 'gamepad.remap.cancel') +
                (done ? btn('gp-save', 'gamepad.remap.save', 'btn-save') : '');
        }

        els.hint.hidden = !hint;
        els.hint.textContent = hint;
        els.actions.innerHTML = actions;
        const on = (cls, fn) => {
            const b = els.actions.querySelector(`.${cls}`);
            if (b) b.addEventListener('click', fn);
        };
        on('gp-close', () => this.close());
        on('gp-remap', () => gp && this._startWizard(gp));
        on('gp-reset', () => this._reset());
        on('gp-back', () => this._wizardBack());
        on('gp-skip', () => this._wizardSkip());
        on('gp-save', () => this._save());
        const back = els.actions.querySelector('.gp-back');
        if (back)
            /** @type {HTMLButtonElement} */ (back).disabled = !this._wiz || this._wiz.idx === 0;
    }

    _loop() {
        if (!this._overlay) return;
        this._syncPads();
        const gp = this._selectedPad();
        if (gp && this._view === 'wizard' && this._wiz) {
            this._wizardFrame(gp);
            this._art.render(readVirtualPad(gp, this._wiz.bindings), BUTTON_TARGETS);
        } else if (gp && this._view === 'test') {
            const res = resolvePad(gp);
            if (res.source !== this._lastSource) {
                // The database answered, or the mapping changed elsewhere.
                this._lastSource = res.source;
                this._syncPads(true);
            }
            if (res.source === 'pending') {
                this._art.clear();
            } else if (res.bindings) {
                this._art.render(readVirtualPad(gp, res.bindings), BUTTON_TARGETS);
            } else if (res.source) {
                this._art.render(gp, BUTTON_TARGETS);
            } else {
                this._art.clear();
            }
        } else if (gp && this._view === 'empty') {
            this._syncPads(true);
        } else {
            this._art.clear();
        }
        this._raf = requestAnimationFrame(() => this._loop());
    }
}
