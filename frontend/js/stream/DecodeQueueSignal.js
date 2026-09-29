/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * DecodeQueueSignal — tells a native host when the decoder has more than one
 * frame waiting, and when it is back to one, so that a host streaming at its
 * own display's rate skips presents instead of queuing them here.
 *
 * A host at 240 or 500 Hz can send frames faster than a phone decodes them.
 * On this side nothing can be done about a frame once it has arrived: an HEVC
 * delta can only be dropped by paying a keyframe for the ones after it. The
 * host, on the other hand, skips a present for free — the next one it sends
 * is simply a delta against the last one it did. DecodeRateGovernor answers a
 * queue that STANDS, over seconds, with a lower rate; this answers the first
 * frame too many, within one trip across the link (cadence=host-guarded,
 * design §33). A host that does not run that cadence ignores the message.
 *
 * The message says the queue's depth. The host stops at two and resumes at
 * one or less; while the queue stays full the signal is said again every
 * REFRESH_MS, because the host stops believing a "full" it has not heard
 * repeated — a lost "clear", or a page gone, must not stop the stream.
 *
 * Pure: the caller reads VideoDecoder.decodeQueueSize and the clock.
 */

/** Frames waiting at the decoder input that make it full. */
export const QUEUE_FULL = 2;
/** …and the depth it has to be back to before the host may send again. */
export const QUEUE_CLEAR = 1;
/** How often a queue that stays full is said again (the host forgets it after five times this). */
export const REFRESH_MS = 50;

export class DecodeQueueSignal {
    constructor() {
        this._full = false;
        this._sentMs = 0;
    }

    /** True while the host has last been told the queue is full. */
    get full() {
        return this._full;
    }

    /**
     * The decoder's queue as it stands now.
     * @param {number} queued VideoDecoder.decodeQueueSize
     * @param {number} now performance.now()
     * @returns {{type: string, depth: number}|null} the message to send, or null
     */
    observe(queued, now) {
        const depth = queued > 0 ? queued : 0;
        if (depth >= QUEUE_FULL) {
            if (this._full && now - this._sentMs < REFRESH_MS) return null;
            this._full = true;
            this._sentMs = now;
            return { type: 'decodequeue', depth };
        }
        if (this._full && depth <= QUEUE_CLEAR) {
            this._full = false;
            this._sentMs = now;
            return { type: 'decodequeue', depth };
        }
        return null;
    }
}
