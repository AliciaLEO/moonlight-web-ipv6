import { afterEach, describe, expect, it, vi } from 'vitest';
import { attachRtpVideo, rtpVideoSupported, ULTRA_RTP_PREFIX_BYTES } from '../js/api/RtpVideo.js';

function trackEvent(mid) {
    return {
        transceiver: { mid, direction: 'recvonly' },
        track: { kind: 'video', label: mid },
        receiver: {},
    };
}

describe('RtpVideo', () => {
    afterEach(() => {
        delete globalThis.RTCRtpScriptTransform;
        vi.unstubAllGlobals();
    });

    it('refuses the track where there is no Encoded Transform', () => {
        delete globalThis.RTCRtpScriptTransform;
        expect(rtpVideoSupported()).toBe(false);
        const event = trackEvent('video');
        const handle = attachRtpVideo(event, { log: () => {} });
        expect(event.transceiver.direction).toBe('inactive');
        expect(handle.worker).toBeNull();
    });

    it('hands video frames on as they are and strips the Ultra prefix', () => {
        const workers = [];
        vi.stubGlobal(
            'Worker',
            class {
                constructor() {
                    workers.push(this);
                }
                terminate() {}
            },
        );
        globalThis.RTCRtpScriptTransform = class {
            constructor(worker, options) {
                this.worker = worker;
                this.options = options;
            }
        };
        const onVideo = vi.fn();
        const onUltra = vi.fn();
        const video = trackEvent('video');
        attachRtpVideo(video, { onVideo, onUltra, log: () => {} });
        expect(video.receiver.transform.options).toEqual({ mid: 'video' });
        const data = new Uint8Array([0, 0, 0, 1, 0x65]).buffer;
        workers[0].onmessage({ data: { mid: 'video', data, key: true, ts: 123456 } });
        expect(onVideo).toHaveBeenCalledWith(new Uint8Array([0, 0, 0, 1, 0x65]), true, 123456);

        const ultra = trackEvent('ultra');
        attachRtpVideo(ultra, { onVideo, onUltra, log: () => {} });
        const frame = new Uint8Array(ULTRA_RTP_PREFIX_BYTES + 20).fill(7);
        workers[1].onmessage({ data: { mid: 'ultra', data: frame.buffer, key: true, ts: 1 } });
        expect(onUltra).toHaveBeenCalledTimes(1);
        expect(onUltra.mock.calls[0][0].byteLength).toBe(20);
        expect(onVideo).toHaveBeenCalledTimes(1);
    });
});
