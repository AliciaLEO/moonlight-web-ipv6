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

/**
 * The bench's other road (U1.4 ter, track "vaudio"): each frame comes cut in
 * Opus packets — 'M', flags (1 = key), frame seq, index, count (u16, big
 * endian), then the frame's bytes — and is put back together here. A frame
 * that never completes is skipped, and the next one says so (`lost`).
 */
function audioRoad(reader) {
    const open = new Map();
    let lastDone = -1;
    let lost = false;
    const pump = () =>
        reader.read().then(({ value: frame, done }) => {
            if (done || !frame) return;
            const buf = frame.data;
            const dv = new DataView(buf);
            if (buf.byteLength < 8 || dv.getUint8(0) !== 0x4d) return pump();
            const seq = dv.getUint16(2);
            const idx = dv.getUint16(4);
            const count = dv.getUint16(6);
            // Older than the last frame given: its time is gone.
            if (lastDone >= 0 && ((seq - lastDone) & 0xffff) >= 0x8000) return pump();
            let f = open.get(seq);
            if (!f) {
                f = { parts: new Array(count), got: 0, bytes: 0, key: (dv.getUint8(1) & 1) === 1 };
                open.set(seq, f);
            }
            if (!f.parts[idx]) {
                f.parts[idx] = new Uint8Array(buf, 8);
                f.got++;
                f.bytes += buf.byteLength - 8;
            }
            if (f.got < count) return pump();
            let held = -1;
            let ts = frame.timestamp;
            try {
                const meta = frame.getMetadata();
                if (meta && typeof meta.rtpTimestamp === 'number') ts = meta.rtpTimestamp;
                if (meta && typeof meta.receiveTime === 'number')
                    held = performance.now() - meta.receiveTime;
            } catch {
                // No metadata: the figures stay empty.
            }
            const data = new Uint8Array(f.bytes);
            let at = 0;
            for (const p of f.parts) {
                data.set(p, at);
                at += p.byteLength;
            }
            if (lastDone >= 0 && ((seq - lastDone) & 0xffff) !== 1) lost = true;
            for (const s of open.keys()) if (((seq - s) & 0xffff) < 0x8000) open.delete(s);
            lastDone = seq;
            const now = performance.timeOrigin + performance.now();
            self.postMessage(
                { mid: 'video', data: data.buffer, key: f.key, ts: ts >>> 0, at: now, held, lost },
                [data.buffer],
            );
            lost = false;
            return pump();
        });
    return pump();
}

self.onrtctransform = (event) => {
    const transformer = event.transformer;
    const mid = (transformer.options && transformer.options.mid) || 'video';
    const reader = transformer.readable.getReader();
    self.postMessage({ mid, ready: true });
    if (mid === 'vaudio') {
        audioRoad(reader).catch((e) =>
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
