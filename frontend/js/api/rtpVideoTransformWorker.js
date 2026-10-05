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
 * The Encoded Transform of the RTP video track (POC Ultra U1.4, RtpVideo.js):
 * each frame the browser has assembled from its RTP packets is posted to the
 * page as is, with its key flag and RTP timestamp (the host puts the frame's
 * backendTs there, in ms), and never handed back — the browser's own decoder
 * never sees it, ours decodes it.
 */

/** How long a complete video frame waits for an older one asked again. */
const GIVE_UP_MS = 15;

/**
 * The bench's other road (U1.4 ter, tracks "vaudio" / "uaudio"): each frame
 * comes cut in Opus packets — 'M', flags (1 = key), frame seq, index, count
 * (u16, big endian), then the frame's bytes — and is put back together here.
 * Chrome asks nothing again on this road: the chunks seen missing (a hole in a
 * frame's indexes, or a newer frame started) are asked for by `nack` messages,
 * which the page sends on the input channel (U1.4 quater). Video frames go out
 * in order, since a delta needs the one before; one that waits more than
 * GIVE_UP_MS for an older one goes anyway, marked `lost`. Ultra frames stand
 * alone and go as they complete.
 */
function audioRoad(reader, outMid) {
    const ordered = outMid === 'video';
    const tag = ordered ? 'v' : 'u';
    const open = new Map(); // seq → frame being put together
    const ready = new Map(); // seq → complete video frame waiting for an older one
    let lastDone = -1;
    let lost = false;
    let gapTimer = 0;
    // Whether seq a comes after seq b, on the 16-bit wheel.
    const after = (a, b) => {
        const d = (a - b) & 0xffff;
        return d !== 0 && d < 0x8000;
    };
    const ask = (seq, f, from, to) => {
        const want = [];
        for (let k = from; k < to; k++)
            if (!f.parts[k] && !f.asked.has(k)) {
                f.asked.add(k);
                want.push(k);
            }
        if (want.length) self.postMessage({ mid: outMid, nack: { t: tag, s: seq, i: want } });
    };
    const post = (seq, f) => {
        const data = new Uint8Array(f.bytes);
        let at = 0;
        for (const p of f.parts) {
            data.set(p, at);
            at += p.byteLength;
        }
        lastDone = seq;
        for (const s of open.keys()) if (!after(s, seq)) open.delete(s);
        const now = performance.timeOrigin + performance.now();
        self.postMessage(
            {
                mid: outMid,
                data: data.buffer,
                key: f.key,
                ts: f.ts >>> 0,
                at: now,
                held: f.held,
                lost,
            },
            [data.buffer],
        );
        lost = false;
    };
    const flush = () => {
        for (;;) {
            const next = (lastDone + 1) & 0xffff;
            const f = ready.get(next);
            if (!f) break;
            ready.delete(next);
            post(next, f);
        }
        clearTimeout(gapTimer);
        gapTimer = ready.size ? setTimeout(giveUp, GIVE_UP_MS) : 0;
    };
    const giveUp = () => {
        gapTimer = 0;
        let oldest = -1;
        for (const s of ready.keys()) if (oldest < 0 || after(oldest, s)) oldest = s;
        if (oldest < 0) return;
        lost = true;
        lastDone = (oldest - 1) & 0xffff;
        flush();
    };
    const pump = () =>
        reader.read().then(({ value: frame, done }) => {
            if (done || !frame) return;
            const buf = frame.data;
            const dv = new DataView(buf);
            if (buf.byteLength < 8 || dv.getUint8(0) !== 0x4d) return pump();
            const seq = dv.getUint16(2);
            const idx = dv.getUint16(4);
            const count = dv.getUint16(6);
            // Not newer than the last frame given: its time is gone.
            if (lastDone >= 0 && !after(seq, lastDone)) return pump();
            if (ready.has(seq)) return pump();
            let f = open.get(seq);
            if (!f) {
                f = {
                    parts: new Array(count),
                    got: 0,
                    bytes: 0,
                    hi: -1,
                    asked: new Set(),
                    key: (dv.getUint8(1) & 1) === 1,
                };
                open.set(seq, f);
                // A newer frame started: what an older one still lacks is lost.
                for (const [s, o] of open)
                    if (o !== f && after(seq, s)) ask(s, o, 0, o.parts.length);
            }
            if (!f.parts[idx]) {
                f.parts[idx] = new Uint8Array(buf, 8);
                f.got++;
                f.bytes += buf.byteLength - 8;
            }
            if (idx > f.hi + 1) ask(seq, f, f.hi + 1, idx);
            if (idx > f.hi) f.hi = idx;
            if (f.got < count) return pump();
            open.delete(seq);
            f.held = -1;
            f.ts = frame.timestamp;
            try {
                const meta = frame.getMetadata();
                if (meta && typeof meta.rtpTimestamp === 'number') f.ts = meta.rtpTimestamp;
                if (meta && typeof meta.receiveTime === 'number')
                    f.held = performance.now() - meta.receiveTime;
            } catch {
                // No metadata: the figures stay empty.
            }
            if (!ordered || lastDone < 0 || seq === ((lastDone + 1) & 0xffff)) {
                post(seq, f);
                if (ordered) flush();
            } else {
                ready.set(seq, f);
                if (!gapTimer) gapTimer = setTimeout(giveUp, GIVE_UP_MS);
            }
            return pump();
        });
    return pump();
}

self.onrtctransform = (event) => {
    const transformer = event.transformer;
    const mid = (transformer.options && transformer.options.mid) || 'video';
    const reader = transformer.readable.getReader();
    self.postMessage({ mid, ready: true });
    // "uaudio": the bench's Ultra stream on the same road, posted without
    // the VP8 header of its video track.
    if (mid === 'vaudio' || mid === 'uaudio') {
        audioRoad(reader, mid === 'vaudio' ? 'video' : 'ultraraw').catch((e) =>
            self.postMessage({ mid, error: String(e && e.message ? e.message : e) }),
        );
        return;
    }
    const pump = () =>
        reader.read().then(({ value: frame, done }) => {
            if (done || !frame) return;
            let ts = frame.timestamp;
            let held = -1;
            try {
                const meta = frame.getMetadata();
                if (meta && typeof meta.rtpTimestamp === 'number') ts = meta.rtpTimestamp;
                // The browser's own wait, last packet in to this transform.
                if (meta && typeof meta.receiveTime === 'number')
                    held = performance.now() - meta.receiveTime;
            } catch {
                // Older engines: the frame's own timestamp is the RTP one.
            }
            const data = frame.data;
            const at = performance.timeOrigin + performance.now();
            self.postMessage({ mid, data, key: frame.type === 'key', ts: ts >>> 0, at, held }, [
                data,
            ]);
            return pump();
        });
    pump().catch((e) => self.postMessage({ mid, error: String(e && e.message ? e.message : e) }));
};
