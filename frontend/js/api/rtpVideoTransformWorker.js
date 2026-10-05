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

self.onrtctransform = (event) => {
    const transformer = event.transformer;
    const mid = (transformer.options && transformer.options.mid) || 'video';
    const reader = transformer.readable.getReader();
    self.postMessage({ mid, ready: true });
    const pump = () =>
        reader.read().then(({ value: frame, done }) => {
            if (done || !frame) return;
            let ts = frame.timestamp;
            try {
                const meta = frame.getMetadata();
                if (meta && typeof meta.rtpTimestamp === 'number') ts = meta.rtpTimestamp;
            } catch {
                // Older engines: the frame's own timestamp is the RTP one.
            }
            const data = frame.data;
            const at = performance.timeOrigin + performance.now();
            self.postMessage({ mid, data, key: frame.type === 'key', ts: ts >>> 0, at }, [data]);
            return pump();
        });
    pump().catch((e) => self.postMessage({ mid, error: String(e && e.message ? e.message : e) }));
};
