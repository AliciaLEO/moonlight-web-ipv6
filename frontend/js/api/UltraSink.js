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
 * UltraSink — the client's half of the POC Ultra transport lab (U1.1,
 * docs/design/ultra-lan-poc.md): it times a synthetic Ultra stream without
 * decoding it.
 *
 * The native host, with `ultra=synthetic:<KiB>` in its tuning, sends after
 * every video frame a train of chunks of that many KiB on the negotiated
 * channel id 5, in the video's own chunk format:
 *
 *     [frame_id:4][chunk_index:2][total_chunks:2][is_keyframe:1][payload_size:4][backend_ts:4]
 *
 * all big-endian, backend_ts being the video frame's stamp (host steady clock,
 * ms). Off unless this browser's localStorage holds `mw_ultra_sink`: "1" for
 * the video's reliability (ordered, given up on after 500 ms, the host's
 * default) or "unordered" (no retransmission, the host's
 * `ultrachannel=unordered`). Either side alone does nothing.
 *
 * Counted per second: frames complete, frames lost (a frame id never seen, or
 * a frame still missing chunks a second after its first), Mbit/s, the spread
 * of each frame (first chunk → last chunk, what the wire's rate and SCTP's
 * window make of a big frame) and its extra delay (last chunk against the
 * frame's stamp, over the session's smallest: the two clocks' offset cancels
 * out). Logged as `[MW-ULTRA] {…}` and kept in `globalThis.__mwUltra`.
 */

export const ULTRA_CHANNEL_ID = 5;
export const ULTRA_HEADER_BYTES = 17;
/** A frame still incomplete this long after its first chunk is counted lost. */
export const ULTRA_GIVE_UP_MS = 1000;

/** The sink's channel reliability asked for here, or null when off. */
export function ultraSinkMode(storage = globalThis.localStorage) {
    try {
        const value = storage?.getItem('mw_ultra_sink');
        if (!value || value === '0') return null;
        return value === 'unordered' ? 'unordered' : 'video';
    } catch {
        return null;
    }
}

/** The negotiated channel matching the host's (id 5). */
export function ultraChannelInit(mode) {
    return mode === 'unordered'
        ? { negotiated: true, id: ULTRA_CHANNEL_ID, ordered: false, maxRetransmits: 0 }
        : { negotiated: true, id: ULTRA_CHANNEL_ID, ordered: true, maxPacketLifeTime: 500 };
}

function percentile(sorted, p) {
    if (sorted.length === 0) return 0;
    return sorted[Math.min(sorted.length - 1, Math.floor(p * sorted.length))];
}

export class UltraSink {
    constructor() {
        this.totals = { frames: 0, lost: 0, chunks: 0, bytes: 0, foreign: 0 };
        /** frame id → {first, last, got, total, stamp} */
        this._open = new Map();
        this._highest = -1;
        this._minRelMs = Infinity;
        this._startWindow();
    }

    _startWindow() {
        this._w = { frames: 0, lost: 0, bytes: 0, spreads: [], delays: [], handlerMs: 0 };
    }

    /**
     * One chunk. @param {ArrayBuffer} buf @param {number} arrivalMs on the
     * page's clock (performance.now()).
     */
    onMessage(buf, arrivalMs) {
        if (!(buf instanceof ArrayBuffer) || buf.byteLength < ULTRA_HEADER_BYTES) {
            this.totals.foreign++;
            return;
        }
        const view = new DataView(buf);
        const id = view.getUint32(0);
        const total = view.getUint16(6);
        const stamp = view.getUint32(13);
        if (total === 0) {
            this.totals.foreign++;
            return;
        }
        this.totals.chunks++;
        this.totals.bytes += buf.byteLength;
        this._w.bytes += buf.byteLength;
        let f = this._open.get(id);
        if (!f) {
            // Frame ids skipped over entirely: frames that never came.
            if (this._highest >= 0 && id > this._highest + 1) this._lose(id - this._highest - 1);
            if (id > this._highest) this._highest = id;
            f = { first: arrivalMs, last: arrivalMs, got: 0, total, stamp };
            this._open.set(id, f);
        }
        f.got++;
        // Chunks may land out of order (an unordered channel, a retransmission):
        // the frame spans from its earliest to its latest arrival.
        if (arrivalMs < f.first) f.first = arrivalMs;
        if (arrivalMs > f.last) f.last = arrivalMs;
        if (f.got >= f.total) {
            this._open.delete(id);
            this._complete(f);
        }
        this._expire(arrivalMs);
    }

    _complete(f) {
        this.totals.frames++;
        this._w.frames++;
        this._w.spreads.push(f.last - f.first);
        // The stamp is the host's steady clock in ms, mod 2^32; the page's
        // performance.now() is another clock: only the difference to the
        // session's smallest means anything.
        const rel = f.last - f.stamp;
        if (rel < this._minRelMs) this._minRelMs = rel;
        this._w.delays.push(rel);
    }

    _lose(n) {
        this.totals.lost += n;
        this._w.lost += n;
    }

    /** Frames still missing chunks ULTRA_GIVE_UP_MS after their first: lost. */
    _expire(nowMs) {
        for (const [id, f] of this._open) {
            if (nowMs - f.first < ULTRA_GIVE_UP_MS) continue;
            this._open.delete(id);
            this._lose(1);
        }
    }

    /** The second just ended, in figures; a new one starts. */
    closeWindow(seconds = 1, nowMs = globalThis.performance?.now() ?? 0) {
        this._expire(nowMs);
        const w = this._w;
        const spreads = w.spreads.slice().sort((a, b) => a - b);
        const delays = w.delays.map((d) => d - this._minRelMs).sort((a, b) => a - b);
        const out = {
            framesPerSec: Math.round(w.frames / seconds),
            mbps: +((w.bytes * 8) / 1e6 / seconds).toFixed(1),
            lost: w.lost,
            spreadMs: {
                p50: +percentile(spreads, 0.5).toFixed(1),
                p95: +percentile(spreads, 0.95).toFixed(1),
                max: +(spreads.length ? spreads[spreads.length - 1] : 0).toFixed(1),
            },
            extraDelayMs: {
                p50: +percentile(delays, 0.5).toFixed(1),
                p95: +percentile(delays, 0.95).toFixed(1),
                max: +(delays.length ? delays[delays.length - 1] : 0).toFixed(1),
            },
            handlerMs: +w.handlerMs.toFixed(2),
        };
        this._startWindow();
        return out;
    }
}

/**
 * Open the Ultra channel on @p pc and time what lands on it, once a second.
 * Returns a handle whose stop() ends the counting.
 */
export function attachUltraSink(pc, mode, { log = console.log, intervalMs = 1000 } = {}) {
    const dc = pc.createDataChannel('ultra', ultraChannelInit(mode));
    dc.binaryType = 'arraybuffer';
    const sink = new UltraSink();
    dc.onmessage = (event) => {
        const t0 = performance.now();
        sink.onMessage(event.data, t0);
        sink._w.handlerMs += performance.now() - t0;
    };
    const history = [];
    const timer = setInterval(() => {
        const second = sink.closeWindow(intervalMs / 1000);
        history.push(second);
        if (history.length > 600) history.shift();
        globalThis.__mwUltra = { mode, totals: { ...sink.totals }, last: second, history };
        log('[MW-ULTRA] ' + JSON.stringify(second));
    }, intervalMs);
    log('[MW-ULTRA] timing the host synthetic Ultra stream on DC#5 (' + mode + ')');
    return {
        dc,
        sink,
        stop() {
            clearInterval(timer);
        },
    };
}
