/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect } from 'vitest';
import {
    parsePadId,
    padKey,
    padName,
    detectPlatform,
    parseSdlMapping,
    readVirtualPad,
    resolveMapping,
    hatBits,
    snapshot,
    detectInput,
    isAtRest,
    sameBinding,
    describeBinding,
    loadGamepadDb,
    isAutoMapped,
} from '../js/stream/gamepadMapping.js';

function pad({ id = 'Pad', mapping = '', buttons = 16, axes = [0, 0, 0, 0] } = {}) {
    return {
        id,
        mapping,
        buttons: Array.from({ length: buttons }, () => ({ pressed: false, value: 0 })),
        axes: axes.slice(),
    };
}
const press = (gp, i, v = 1) => (gp.buttons[i] = { pressed: v > 0.5, value: v });

// Hat position k (0 = up, clockwise) as Chrome reports it on one axis.
const hat = (k) => -1 + (k * 2) / 7;
const HAT_CENTRED = 9 / 7;

describe('parsePadId', () => {
    it('reads Chrome desktop ids', () => {
        expect(
            parsePadId('Wireless Controller (STANDARD GAMEPAD Vendor: 054c Product: 09cc)'),
        ).toEqual({ name: 'Wireless Controller', vid: '054c', pid: '09cc' });
        expect(parsePadId('GameSir G4 (Vendor: 5ac Product: 22d)')).toEqual({
            name: 'GameSir G4',
            vid: '05ac',
            pid: '022d',
        });
    });

    it('reads Firefox and Safari ids, padding the hex', () => {
        expect(parsePadId('054c-09cc-Wireless Controller')).toMatchObject({
            vid: '054c',
            pid: '09cc',
        });
        expect(parsePadId('46d-c216-Logitech Dual Action')).toEqual({
            name: 'Logitech Dual Action',
            vid: '046d',
            pid: 'c216',
        });
    });

    it('keeps a bare Android name, and strips a lone Chrome suffix', () => {
        expect(parsePadId('GameSir-G8+')).toEqual({ name: 'GameSir-G8+', vid: null, pid: null });
        expect(parsePadId('Pad (STANDARD GAMEPAD)')).toMatchObject({ name: 'Pad', vid: null });
    });

    it('keys pads by USB ids when there are some, by name otherwise', () => {
        expect(padKey('054c-09cc-X')).toBe('usb:054c:09cc');
        expect(padKey({ id: 'X (Vendor: 054c Product: 09cc)' })).toBe('usb:054c:09cc');
        expect(padKey('GameSir-G8+')).toBe('name:gamesir-g8+');
        expect(padName('054c-09cc-Wireless')).toBe('Wireless');
        expect(padName('')).toBe('?');
    });

    it('names a pad Windows reports under a generic name from its USB ids', () => {
        expect(
            padName('HID-compliant game controller (STANDARD GAMEPAD Vendor: 045e Product: 02e0)'),
        ).toBe('Xbox One S Controller');
        expect(padName('HID-compliant game controller (Vendor: 1234 Product: abcd)')).toBe(
            'Controller 1234:abcd',
        );
        // No ids to go on: the generic name is all there is.
        expect(padName('HID-compliant game controller (STANDARD GAMEPAD)')).toBe(
            'HID-compliant game controller',
        );
        // A real product name is never replaced.
        expect(padName('Wireless Controller (Vendor: 054c Product: 09cc)')).toBe(
            'Wireless Controller',
        );
    });
});

describe('detectPlatform', () => {
    it('tells the platforms apart from the user agent', () => {
        expect(detectPlatform('Mozilla/5.0 (Linux; Android 14; Pixel 8) Chrome/128')).toBe(
            'android',
        );
        expect(detectPlatform('Mozilla/5.0 (iPhone; CPU iPhone OS 17_0)')).toBe('ios');
        expect(detectPlatform('Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/128')).toBe('win');
        expect(detectPlatform('Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7)')).toBe('mac');
        expect(detectPlatform('Mozilla/5.0 (X11; Linux x86_64)')).toBe('linux');
        expect(detectPlatform('curl/8')).toBe('other');
    });
});

describe('hatBits', () => {
    it('decodes a packed hat, diagonals included, and centre as nothing', () => {
        expect(hatBits(hat(0))).toBe(1);
        expect(hatBits(hat(1))).toBe(3);
        expect(hatBits(hat(2))).toBe(2);
        expect(hatBits(hat(4))).toBe(4);
        expect(hatBits(hat(6))).toBe(8);
        expect(hatBits(hat(7))).toBe(9);
        expect(hatBits(HAT_CENTRED)).toBe(0);
        expect(hatBits(undefined)).toBe(0);
    });
});

describe('parseSdlMapping', () => {
    // GameSir G4, SDL Windows entry.
    const G4 =
        'a:b0,b:b1,back:b10,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,leftshoulder:b6,' +
        'leftstick:b13,lefttrigger:b8,leftx:a0,lefty:a1,rightshoulder:b7,rightstick:b14,' +
        'righttrigger:b9,rightx:a2,righty:a5,start:b11,x:b3,y:b4';

    it('keeps buttons, axes that exist, and packs the hat on axis 9 (Windows)', () => {
        const b = parseSdlMapping(G4, { platform: 'win', axesLength: 10 });
        expect(b.a).toEqual({ t: 'b', i: 0 });
        expect(b.righty).toEqual({ t: 'a', i: 5, s: 0 });
        expect(b.dpup).toEqual({ t: 'h', i: 9, bit: 1 });
        expect(b.dpleft).toEqual({ t: 'h', i: 9, bit: 8 });
    });

    it('drops an axis the browser does not have', () => {
        const b = parseSdlMapping(G4, { platform: 'win', axesLength: 4 });
        expect(b.righty).toBeUndefined();
        expect(b.dpup).toBeUndefined(); // no axis 9 either
        expect(b.rightx).toEqual({ t: 'a', i: 2, s: 0 });
    });

    it('puts a Linux hat on the last two axes', () => {
        const b = parseSdlMapping(G4, { platform: 'linux', axesLength: 8 });
        expect(b.dpup).toEqual({ t: 'a', i: 7, s: -1 });
        expect(b.dpdown).toEqual({ t: 'a', i: 7, s: 1 });
        expect(b.dpleft).toEqual({ t: 'a', i: 6, s: -1 });
        expect(b.dpright).toEqual({ t: 'a', i: 6, s: 1 });
    });

    it('reads half axes and inversions, and ignores what it cannot place', () => {
        const b = parseSdlMapping('dpup:-a1,dpdown:+a1,lefty:a3~,misc1:b5,dpleft:h1.8,bogus', {
            platform: 'win',
            axesLength: 4,
        });
        expect(b.dpup).toEqual({ t: 'a', i: 1, s: -1 });
        expect(b.dpdown).toEqual({ t: 'a', i: 1, s: 1 });
        expect(b.lefty).toEqual({ t: 'a', i: 3, s: 0, inv: true });
        expect(b.misc1).toBeUndefined();
        expect(b.dpleft).toBeUndefined();
    });
});

describe('readVirtualPad', () => {
    it('reads buttons, triggers on axes, sticks and a packed hat into standard order', () => {
        const gp = pad({ axes: [0.5, -0.25, 0, 0, -1, 1, 0, 0, 0, HAT_CENTRED] });
        const bindings = {
            a: { t: 'b', i: 2 },
            lefttrigger: { t: 'a', i: 4, s: 0 },
            righttrigger: { t: 'a', i: 5, s: 0 },
            leftx: { t: 'a', i: 0, s: 0 },
            lefty: { t: 'a', i: 1, s: 0, inv: true },
            dpup: { t: 'h', i: 9, bit: 1 },
            dpright: { t: 'h', i: 9, bit: 2 },
        };
        press(gp, 2);
        let v = readVirtualPad(gp, bindings);
        expect(v.buttons).toHaveLength(17);
        expect(v.buttons[0].pressed).toBe(true); // A
        expect(v.buttons[6]).toEqual({ pressed: false, value: 0 }); // LT at rest
        expect(v.buttons[7]).toEqual({ pressed: true, value: 1 }); // RT full
        expect(v.axes).toEqual([0.5, 0.25, 0, 0]);
        expect(v.buttons[12].pressed).toBe(false);

        gp.axes[9] = hat(1); // up-right
        v = readVirtualPad(gp, bindings);
        expect(v.buttons[12].pressed).toBe(true);
        expect(v.buttons[15].pressed).toBe(true);
    });

    it('reads half axes as buttons and a button as a stick direction', () => {
        const gp = pad({ axes: [0, 0.8, 0, 0] });
        const v = readVirtualPad(gp, {
            dpdown: { t: 'a', i: 1, s: 1 },
            dpup: { t: 'a', i: 1, s: -1 },
            rightx: { t: 'b', i: 3 },
            righty: { t: 'a', i: 1, s: 1 },
        });
        expect(v.buttons[13].pressed).toBe(true);
        expect(v.buttons[12].pressed).toBe(false);
        expect(v.axes[2]).toBe(0);
        expect(v.axes[3]).toBeCloseTo(0.8);
        press(gp, 3);
        expect(readVirtualPad(gp, { rightx: { t: 'b', i: 3 } }).axes[2]).toBe(1);
    });

    it('reads missing inputs as rest', () => {
        const v = readVirtualPad(pad({ buttons: 2, axes: [] }), {
            a: { t: 'b', i: 9 },
            leftx: { t: 'a', i: 7, s: 0 },
            dpup: { t: 'h', i: 9, bit: 1 },
        });
        expect(v.buttons[0].pressed).toBe(false);
        expect(v.axes[0]).toBe(0);
        expect(v.buttons[12].pressed).toBe(false);
    });
});

describe('resolveMapping', () => {
    const db = {
        win: { '05ac:022d': [['GameSir G4', 'a:b0,b:b1,leftx:a0']] },
        linux: {
            '05ac:022d': [
                ['Other', 'a:b5'],
                ['GameSir G4', 'a:b1'],
            ],
        },
    };
    const G4 = 'GameSir G4 (Vendor: 05ac Product: 022d)';

    it('prefers the user mapping, even over a standard pad', () => {
        const user = (k) => (k === 'usb:045e:0b13' ? { bindings: { a: { t: 'b', i: 1 } } } : null);
        const gp = pad({
            id: 'Xbox (STANDARD GAMEPAD Vendor: 045e Product: 0b13)',
            mapping: 'standard',
        });
        expect(resolveMapping(gp, { user, platform: 'win' })).toMatchObject({
            source: 'user',
            key: 'usb:045e:0b13',
        });
        expect(resolveMapping(gp, { platform: 'win' })).toMatchObject({
            source: 'standard',
            bindings: null,
        });
    });

    it('reads an unknown Chrome Android pad as standard', () => {
        const res = resolveMapping(pad({ id: 'GameSir-G8+' }), { platform: 'android' });
        expect(res).toMatchObject({ source: 'android', bindings: null, name: 'GameSir-G8+' });
    });

    it('waits for the database, then maps from it, the name breaking ties', () => {
        const gp = pad({ id: G4, axes: [0, 0, 0, 0] });
        expect(resolveMapping(gp, { platform: 'win', db: undefined }).source).toBe('pending');
        const win = resolveMapping(gp, { platform: 'win', db });
        expect(win).toMatchObject({ source: 'db', dbName: 'GameSir G4' });
        expect(win.bindings.a).toEqual({ t: 'b', i: 0 });
        expect(resolveMapping(gp, { platform: 'linux', db }).bindings.a).toEqual({ t: 'b', i: 1 });
    });

    // An EdgeTX radio as each desktop browser names it (T0 relevé, 01/10/2026).
    const TX12_CHROME = 'Radiomaster TX12 Joystick (Vendor: 1209 Product: 4f54)';
    const TX12_SAFARI = '1209-4f54-Radiomaster TX12 Joystick';
    const radio = (id = TX12_CHROME) =>
        pad({ id, buttons: 24, axes: [0.02, -0.01, -1, 0.01, 0, 0, 0] });

    it('lays an EdgeTX / OpenTX radio out from its built-in profile, on every desktop', () => {
        for (const platform of ['win', 'mac', 'linux']) {
            const res = resolveMapping(radio(), { platform, db: undefined });
            expect(res).toMatchObject({ source: 'builtin', key: 'usb:1209:4f54' });
            expect(res.bindings.lefty).toEqual({ t: 'a', i: 2, s: 0, inv: true });
        }
        // Safari's id names the same pad, and keys it the same.
        const safari = resolveMapping(radio(TX12_SAFARI), { platform: 'mac', db: null });
        expect(safari).toMatchObject({ source: 'builtin', key: 'usb:1209:4f54' });
    });

    it("lets the user's own mapping win over the built-in one, and Android keep its own", () => {
        const user = (k) => (k === 'usb:1209:4f54' ? { bindings: { a: { t: 'b', i: 3 } } } : null);
        expect(resolveMapping(radio(), { user, platform: 'win' }).source).toBe('user');
        expect(
            resolveMapping(radio('Radiomaster TX12 Joystick'), { platform: 'android' }).source,
        ).toBe('android');
    });

    it('hands out a copy of the built-in profile', () => {
        resolveMapping(radio(), { platform: 'win' }).bindings.lefty.i = 9;
        expect(resolveMapping(radio(), { platform: 'win' }).bindings.lefty.i).toBe(2);
    });

    it('gives up on a pad nothing maps', () => {
        expect(
            resolveMapping(pad({ id: 'Mystery (Vendor: 1234 Product: 5678)' }), {
                platform: 'win',
                db,
            }).source,
        ).toBeNull();
        expect(resolveMapping(pad({ id: G4 }), { platform: 'mac', db }).source).toBeNull();
        expect(resolveMapping(pad({ id: 'Siri Remote' }), { platform: 'ios' }).source).toBeNull();
        expect(resolveMapping(pad({ id: G4 }), { platform: 'win', db: null }).source).toBeNull();
    });
});

describe('an EdgeTX / OpenTX radio, read through its built-in profile', () => {
    // Raw axes as Chrome gives them (T0): roll, pitch, throttle, yaw, CH5, CH6, CH7/8.
    const read = (axes, pressed = []) => {
        const gp = pad({
            id: 'Radiomaster TX12 Joystick (Vendor: 1209 Product: 4f54)',
            buttons: 24,
            axes,
        });
        for (const i of pressed) press(gp, i);
        const { bindings } = resolveMapping(gp, { platform: 'win' });
        const v = readVirtualPad(gp, bindings);
        // A flipped centred axis reads -0, which the host gets as 0.
        return { ...v, axes: v.axes.map((x) => x + 0) };
    };

    it('puts the sticks where a Mode 2 pilot holds them, up being up', () => {
        // Throttle at the bottom is the left stick held down; at the top, up.
        expect(read([0, 0, -1, 0, 0, 0, 0]).axes).toEqual([0, 1, 0, 0]);
        expect(read([0, 0, 1, 0, 0, 0, 0]).axes).toEqual([0, -1, 0, 0]);
        // Yaw left, roll right, pitch up.
        expect(read([1, 1, 0, -1, 0, 0, 0]).axes).toEqual([-1, 0, 1, -1]);
    });

    it('reads CH5 and CH6 as whole-axis triggers: 0, half, full', () => {
        expect(read([0, 0, -1, 0, -1, 1, 0]).buttons[6].value).toBe(0);
        expect(read([0, 0, -1, 0, 0, 1, 0]).buttons[6].value).toBe(0.5);
        expect(read([0, 0, -1, 0, 1, 1, 0]).buttons[6].value).toBe(1);
        expect(read([0, 0, -1, 0, -1, 1, 0]).buttons[7].value).toBe(1);
    });

    it('presses A to Start from channels 9-16, and sends CH7-8 nowhere', () => {
        const v = read([0, 0, -1, 0, -1, -1, 1], [0, 3, 7, 8]);
        const pressed = v.buttons.map((b, i) => (b.pressed ? i : null)).filter((i) => i !== null);
        // A, Y, Start; raw button 8 (CH17) has no place.
        expect(pressed).toEqual([0, 3, 9]);
        expect(v.axes).toEqual([0, 1, 0, 0]);
    });

    it('is named by its ids when Windows only gives a generic name', () => {
        expect(padName('HID-compliant game controller (Vendor: 1209 Product: 4f54)')).toBe(
            'EdgeTX / OpenTX radio',
        );
    });

    it('counts as laid out without the user, like a guess', () => {
        expect(['builtin', 'android', 'db'].every(isAutoMapped)).toBe(true);
        expect([null, 'user', 'standard', 'pending'].some(isAutoMapped)).toBe(false);
    });
});

describe('detectInput (wizard)', () => {
    it('takes the first button pressed, skipping ones already bound', () => {
        const gp = pad();
        const base = snapshot(gp);
        expect(detectInput(base, gp, 'a')).toBeNull();
        press(gp, 3);
        expect(detectInput(base, gp, 'a')).toEqual({ t: 'b', i: 3 });
        expect(detectInput(base, gp, 'b', [{ t: 'b', i: 3 }])).toBeNull();
    });

    it('takes a trigger resting at -1 as a whole axis, preferring it to its button', () => {
        const gp = pad({ axes: [0, 0, 0, 0, -1] });
        const base = snapshot(gp);
        gp.axes[4] = 0.9;
        press(gp, 6, 0.9);
        expect(detectInput(base, gp, 'lefttrigger')).toEqual({ t: 'a', i: 4, s: 0 });
        // For a plain button the button wins.
        expect(detectInput(base, gp, 'leftshoulder')).toEqual({ t: 'b', i: 6 });
    });

    it('takes a trigger resting at +1 inverted, and a centred axis by its half', () => {
        const gp = pad({ axes: [0, 0, 1] });
        const base = snapshot(gp);
        gp.axes[2] = -0.9;
        expect(detectInput(base, gp, 'righttrigger')).toEqual({ t: 'a', i: 2, s: 0, inv: true });
        gp.axes[2] = 1;
        gp.axes[1] = -0.9;
        expect(detectInput(base, gp, 'dpup')).toEqual({ t: 'a', i: 1, s: -1 });
    });

    it('takes a stick push as the whole axis, inverted when pushed the other way', () => {
        const gp = pad();
        const base = snapshot(gp);
        gp.axes[0] = 0.9;
        expect(detectInput(base, gp, 'leftx')).toEqual({ t: 'a', i: 0, s: 0 });
        gp.axes[0] = 0;
        gp.axes[3] = -0.9;
        expect(detectInput(base, gp, 'righty')).toEqual({ t: 'a', i: 3, s: 0, inv: true });
        // A stick target ignores buttons.
        gp.axes[3] = 0;
        press(gp, 0);
        expect(detectInput(base, gp, 'leftx')).toBeNull();
    });

    it('reads a parked stick by where it rests, not by the push', () => {
        // A radio's throttle rests at the bottom, -1, and can only go up — while
        // the step asks for "down". Its rest IS down: flipped, so up reads up.
        const radio = pad({ axes: [0, 0, -1, 0] });
        const atRest = snapshot(radio);
        radio.axes[2] = 1;
        expect(detectInput(atRest, radio, 'lefty')).toEqual({ t: 'a', i: 2, s: 0, inv: true });
        // A device whose bottom reads +1 is left as it is.
        const other = pad({ axes: [0, 0, 1, 0] });
        const otherRest = snapshot(other);
        other.axes[2] = -1;
        expect(detectInput(otherRest, other, 'righty')).toEqual({ t: 'a', i: 2, s: 0 });
        // A horizontal parked at an end rests at the left: -1 as it is, +1 flipped.
        const left = pad({ axes: [-1, 0, 0, 0] });
        const leftRest = snapshot(left);
        left.axes[0] = 1;
        expect(detectInput(leftRest, left, 'leftx')).toEqual({ t: 'a', i: 0, s: 0 });
        const flipped = pad({ axes: [1, 0, 0, 0] });
        const flippedRest = snapshot(flipped);
        flipped.axes[0] = -1;
        expect(detectInput(flippedRest, flipped, 'rightx')).toEqual({
            t: 'a',
            i: 0,
            s: 0,
            inv: true,
        });
    });

    it('reads a packed hat by its clean directions only', () => {
        const gp = pad({ axes: [0, 0, 0, 0, 0, 0, 0, 0, 0, HAT_CENTRED] });
        const base = snapshot(gp);
        gp.axes[9] = hat(2);
        expect(detectInput(base, gp, 'dpright')).toEqual({ t: 'h', i: 9, bit: 2 });
        gp.axes[9] = hat(3); // diagonal: says nothing
        expect(detectInput(base, gp, 'dpdown')).toBeNull();
    });

    it('knows when the pad is back at rest', () => {
        const gp = pad({ axes: [0, 0, -1] });
        const base = snapshot(gp);
        expect(isAtRest(base, gp)).toBe(true);
        press(gp, 1);
        expect(isAtRest(base, gp)).toBe(false);
        press(gp, 1, 0);
        gp.axes[2] = 0.5;
        expect(isAtRest(base, gp)).toBe(false);
        gp.axes[2] = -0.9;
        expect(isAtRest(base, gp)).toBe(true);
    });
});

describe('sameBinding / describeBinding', () => {
    it('compares raw inputs', () => {
        expect(sameBinding({ t: 'b', i: 1 }, { t: 'b', i: 1 })).toBe(true);
        expect(sameBinding({ t: 'b', i: 1 }, { t: 'b', i: 2 })).toBe(false);
        expect(sameBinding({ t: 'a', i: 1, s: 0 }, { t: 'a', i: 1, s: 1 })).toBe(true);
        expect(sameBinding({ t: 'a', i: 1, s: -1 }, { t: 'a', i: 1, s: 1 })).toBe(false);
        expect(sameBinding({ t: 'h', i: 9, bit: 1 }, { t: 'h', i: 9, bit: 2 })).toBe(false);
        expect(sameBinding(null, { t: 'b', i: 1 })).toBe(false);
    });

    it('labels raw inputs', () => {
        expect(describeBinding({ t: 'b', i: 7 })).toBe('B7');
        expect(describeBinding({ t: 'a', i: 2, s: 1 })).toBe('A2+');
        expect(describeBinding({ t: 'a', i: 5, s: 0, inv: true })).toBe('A5~');
        expect(describeBinding({ t: 'h', i: 9, bit: 4 })).toBe('H9↓');
        expect(describeBinding(null)).toBe('—');
    });
});

describe('loadGamepadDb', () => {
    it('loads the generated database once', async () => {
        const db = await loadGamepadDb();
        expect(db).toBeTruthy();
        expect(Object.keys(db)).toEqual(['win', 'mac', 'linux']);
        expect(await loadGamepadDb()).toBe(db);
        // Keys are vendor:product, entries [name, mapping].
        const [key, entries] = Object.entries(db.win)[0];
        expect(key).toMatch(/^[0-9a-f]{4}:[0-9a-f]{4}$/);
        expect(entries[0]).toHaveLength(2);
    });
});
