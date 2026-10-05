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
 * The video on an RTP track instead of the video DataChannel (POC Ultra U1.4,
 * docs/design/ultra-lan-poc.md). The host offers the track when its
 * `rtp_video` setting names the session's codec for its host type; the frames
 * are taken off it by Encoded Transform (rtpVideoTransformWorker.js), before
 * the browser's decoder, and go to the same decode path as the DataChannel's.
 * A browser without RTCRtpScriptTransform answers the track inactive and the
 * host keeps the video on the DataChannel.
 *
 * Track "video": the stream's codec, frames as the host's encoder made them
 * (Annex B for H.264 / HEVC, a temporal unit for AV1); the RTP timestamp is
 * the frame's backendTs (host steady clock, ms, mod 2^32). Track "ultra" (the
 * bench's synthetic Ultra stream): a 10-byte VP8 key-frame header, then the
 * Ultra DataChannel's chunk format for one chunk.
 */

export const ULTRA_RTP_PREFIX_BYTES = 10;

/** Whether this browser can take an RTP track's frames before its decoder. */
export function rtpVideoSupported() {
    return typeof globalThis.RTCRtpScriptTransform === 'function';
}

/**
 * Takes the frames of the RTP track @p event announced (an `ontrack` event).
 * @param {RTCTrackEvent} event
 * @param {{onVideo?: function(Uint8Array, boolean, number): void,
 *          onUltra?: function(ArrayBuffer, number): void,
 *          log?: function(string): void}} sinks
 * @returns {{mid: string, worker: Worker|null, stop: function(): void}}
 */
export function attachRtpVideo(event, { onVideo, onUltra, log = console.log } = {}) {
    const mid = event.transceiver?.mid || (event.track.label === 'ultra' ? 'ultra' : 'video');
    if (!rtpVideoSupported()) {
        // Answered inactive: the host keeps the video on the DataChannel.
        try {
            event.transceiver.direction = 'inactive';
        } catch {
            // An already stopped transceiver: nothing will flow on it.
        }
        log('[MW-RTP] no RTCRtpScriptTransform here: track ' + mid + ' refused');
        return { mid, worker: null, stop() {} };
    }
    const worker = new Worker(new URL('./rtpVideoTransformWorker.js', import.meta.url));
    let frames = 0;
    worker.onmessage = (msg) => {
        const m = msg.data;
        if (m.ready) {
            log('[MW-RTP] transform of ' + mid + ' running');
            return;
        }
        if (m.error) {
            log('[MW-RTP] transform of ' + mid + ' ended: ' + m.error);
            return;
        }
        frames++;
        // Worker → page hop, for the bench (U1.4): kept as a running figure.
        const hop = performance.timeOrigin + performance.now() - m.at;
        if (m.at) {
            const st = (globalThis.__mwRtp ||= { hops: [] });
            st.hops.push(hop);
            if (st.hops.length > 600) st.hops.shift();
        }
        if (frames === 1) log('[MW-RTP] first frame on ' + mid + (m.key ? ' (key)' : ''));
        if (m.mid === 'ultra') {
            if (onUltra && m.data.byteLength > ULTRA_RTP_PREFIX_BYTES)
                onUltra(m.data.slice(ULTRA_RTP_PREFIX_BYTES), performance.now());
            return;
        }
        if (onVideo) onVideo(new Uint8Array(m.data), m.key, m.ts);
    };
    // Nothing of the browser's own buffering is wanted before the transform.
    try {
        event.receiver.jitterBufferTarget = 0;
    } catch {
        // Not settable here: the transform sits before it anyway.
    }
    worker.onerror = (e) => log('[MW-RTP] transform worker of ' + mid + ' failed: ' + e.message);
    event.receiver.transform = new globalThis.RTCRtpScriptTransform(worker, { mid });
    log('[MW-RTP] taking the frames of RTP track ' + mid + ' by Encoded Transform');
    // The first seconds in figures: packets that came, frames assembled.
    let looks = 0;
    setTimeout(() => {
        try {
            const codecs = event.receiver.getParameters().codecs || [];
            log(
                '[MW-RTP] ' +
                    mid +
                    ' negotiated: ' +
                    codecs
                        .map((c) => c.payloadType + ' ' + c.mimeType + ' ' + (c.sdpFmtpLine || ''))
                        .join(' | '),
            );
        } catch (e) {
            log('[MW-RTP] ' + mid + ' parameters: ' + e.message);
        }
    }, 1000);
    const look = setInterval(() => {
        if (++looks > 5) clearInterval(look);
        event.receiver
            .getStats()
            .then((report) => {
                report.forEach((r) => {
                    if (r.type !== 'inbound-rtp') return;
                    log(
                        '[MW-RTP] ' +
                            mid +
                            ': packets ' +
                            r.packetsReceived +
                            ', lost ' +
                            r.packetsLost +
                            ', frames ' +
                            (r.framesReceived ?? '-') +
                            ', to us ' +
                            frames +
                            ', codec ' +
                            (r.codecId || '-'),
                    );
                });
            })
            .catch(() => {});
    }, 2000);
    return {
        mid,
        worker,
        get frames() {
            return frames;
        },
        stop() {
            clearInterval(look);
            worker.terminate();
        },
    };
}
