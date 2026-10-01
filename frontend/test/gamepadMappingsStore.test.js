/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, vi } from 'vitest';
import {
    getMapping,
    setMapping,
    removeMapping,
    listMappings,
    setKind,
    CHANGED_EVENT,
    _resetForTests,
} from '../js/util/gamepadMappingsStore.js';

describe('gamepadMappingsStore', () => {
    beforeEach(() => {
        localStorage.clear();
        _resetForTests();
    });

    it('keeps a mapping in this browser, under its own key', () => {
        setMapping('usb:1234:5678', 'Pad', { a: { t: 'b', i: 2 } });
        expect(getMapping('usb:1234:5678')).toMatchObject({
            name: 'Pad',
            bindings: { a: { t: 'b', i: 2 } },
        });
        // Survives a reload: read back from storage, not from memory.
        _resetForTests();
        expect(getMapping('usb:1234:5678').bindings.a).toEqual({ t: 'b', i: 2 });
        expect(JSON.parse(localStorage.getItem('mw-gamepad-mappings'))).toHaveProperty(
            'usb:1234:5678',
        );
        // Never inside the settings pushed to the server as defaults.
        expect(localStorage.getItem('mw-streaming-settings')).toBeNull();
    });

    it('announces every change', () => {
        const seen = vi.fn();
        window.addEventListener(CHANGED_EVENT, seen);
        setMapping('name:pad', 'Pad', {});
        removeMapping('name:pad');
        removeMapping('name:pad'); // nothing to remove: no event
        window.removeEventListener(CHANGED_EVENT, seen);
        expect(seen).toHaveBeenCalledTimes(2);
    });

    it('lists mappings newest first and forgets removed ones', () => {
        const now = vi.spyOn(Date, 'now');
        now.mockReturnValue(1000);
        setMapping('a', 'A', {});
        now.mockReturnValue(2000);
        setMapping('b', 'B', {});
        now.mockRestore();
        expect(listMappings().map((m) => m.key)).toEqual(['b', 'a']);
        removeMapping('b');
        expect(getMapping('b')).toBeNull();
        expect(listMappings().map((m) => m.key)).toEqual(['a']);
    });

    it('keeps the kind of device: alone, beside a layout, and through a new layout', () => {
        // A wheel laid out by a built-in profile: the kind alone, no layout.
        setKind('usb:046d:c24f', 'G29', 'wheel');
        expect(getMapping('usb:046d:c24f')).toMatchObject({ kind: 'wheel' });
        expect(getMapping('usb:046d:c24f').bindings).toBeUndefined();
        // Not a saved layout: Settings does not offer to forget it as one.
        expect(listMappings()).toEqual([]);
        // A layout saved later keeps the kind; a kind changed later keeps the layout.
        setMapping('usb:046d:c24f', 'G29', { leftx: { t: 'a', i: 0, s: 0 } });
        expect(getMapping('usb:046d:c24f')).toMatchObject({
            kind: 'wheel',
            bindings: { leftx: {} },
        });
        setKind('usb:046d:c24f', 'G29', 'gamepad');
        expect(getMapping('usb:046d:c24f')).toMatchObject({
            kind: 'gamepad',
            bindings: { leftx: {} },
        });
        setMapping('usb:1209:4f54', 'TX12', {}, 'rc');
        expect(getMapping('usb:1209:4f54').kind).toBe('rc');
    });

    it('reads an entry saved before kinds as a layout without one', () => {
        localStorage.setItem(
            'mw-gamepad-mappings',
            JSON.stringify({
                old: { name: 'Old', bindings: { a: { t: 'b', i: 0 } }, updatedAt: 1 },
            }),
        );
        _resetForTests();
        expect(getMapping('old')).toMatchObject({ name: 'Old', bindings: { a: { t: 'b', i: 0 } } });
        expect(getMapping('old').kind).toBeUndefined();
    });

    it('treats unreadable storage as empty', () => {
        localStorage.setItem('mw-gamepad-mappings', '{not json');
        _resetForTests();
        expect(getMapping('a')).toBeNull();
        expect(listMappings()).toEqual([]);
    });
});
