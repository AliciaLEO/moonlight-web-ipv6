/*
 * MoonlightWeb — the virtual display drawn by another GPU than the apps.
 * Copyright (C) 2026 Bruno Martin. GPLv3 — see repository LICENSE.
 */

/**
 * "MoonlightWeb Virtual Display" takes the primary role when it comes on, and
 * an app picks its GPU from the primary display when it starts. An app opened
 * before the stream therefore stays on the GPU of the display it was opened
 * on, and when the virtual display is drawn by another one, every picture that
 * app paints crosses from one GPU to the other. Seen 04/10/2026 on the
 * UM790Pro: Steam on the Radeon 780M, the virtual display on a GTX 1050, the
 * host's encode at 130 ms per frame while the pointer hovered Steam's cards —
 * and 5 ms once Steam was restarted. The host says when the two GPUs differ
 * (`vd_gpu_apart` in the launch result); this says it to the user, once per
 * host and pair of GPUs.
 */

const SEEN_KEY = 'mw_vd_gpu_apart_seen';
const SEEN_MAX = 20;

/**
 * The notice to show for this launch result, or null: none when the GPUs are
 * the same, or when this browser was already told about this host's pair.
 * Marks it as told.
 *
 * @param {{vd_gpu_apart?: {display?: string, apps?: string}}} result
 * @param {string} hostUuid
 * @returns {{display: string, apps: string} | null}
 */
export function takeVdGpuApartNotice(result, hostUuid) {
    const apart = result?.vd_gpu_apart;
    if (!apart || typeof apart.display !== 'string' || typeof apart.apps !== 'string') {
        return null;
    }
    if (!apart.display || !apart.apps) return null;
    const id = `${hostUuid}|${apart.display}|${apart.apps}`;
    let seen = [];
    try {
        const stored = JSON.parse(localStorage.getItem(SEEN_KEY) || '[]');
        if (Array.isArray(stored)) seen = stored;
    } catch {
        /* storage unavailable or corrupt: tell the user, it costs one toast */
    }
    if (seen.includes(id)) return null;
    try {
        localStorage.setItem(SEEN_KEY, JSON.stringify([...seen, id].slice(-SEEN_MAX)));
    } catch {
        /* told now, maybe again next time */
    }
    return { display: apart.display, apps: apart.apps };
}
