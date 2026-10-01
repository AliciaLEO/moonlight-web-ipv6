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
 * DeviceArt — the devices that are not pads: what they look like, small and
 * large (gamepadMapping.PAD_KINDS). The pad itself is GamepadArt.
 */

/** Line icons of each kind, 20×20, drawn in the text colour. */
const KIND_ICONS = {
    gamepad:
        '<path d="M6 7.5h8a4 4 0 0 1 3.9 4.9l-.6 2.4a1.8 1.8 0 0 1-3.1.8L12.5 13.5h-5l-1.7 2.1a1.8 1.8 0 0 1-3.1-.8l-.6-2.4A4 4 0 0 1 6 7.5Z"/>' +
        '<path d="M6.5 9.5v2.5M5.25 10.75h2.5"/><path d="M13.5 10h.01M15 11.5h.01"/>',
    rc:
        '<rect x="3" y="7.5" width="14" height="9.5" rx="2"/><path d="M10 7.5V2.5"/>' +
        '<circle cx="7" cy="12" r="1.7"/><circle cx="13" cy="12" r="1.7"/>',
    flightstick:
        '<path d="M8.5 2.5h3a1 1 0 0 1 1 1V10a2.5 2.5 0 0 1-5 0V3.5a1 1 0 0 1 1-1Z"/>' +
        '<path d="M10 12.5V15.5"/><rect x="4" y="15.5" width="12" height="2.5" rx="1"/>',
    wheel:
        '<circle cx="10" cy="10" r="7.5"/><circle cx="10" cy="10" r="2"/>' +
        '<path d="M2.6 8.6 8 9.5M17.4 8.6 12 9.5M10 12v5.5"/>',
};

/** A small icon of a device kind, for lists (Settings → Controllers). */
export function kindIconSvg(kind) {
    return (
        '<svg class="pad-kind-icon" viewBox="0 0 20 20" width="18" height="18" aria-hidden="true" ' +
        'fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round">' +
        (KIND_ICONS[kind] || KIND_ICONS.gamepad) +
        '</svg>'
    );
}
