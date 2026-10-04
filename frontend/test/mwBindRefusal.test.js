/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi } from 'vitest';
import { WebRtcDataChannel } from '../js/api/WebRtcDataChannel.js';
import { WebRtcMedia } from '../js/api/WebRtcMedia.js';
import { IDENTITY_REFUSED, isBindRefusedClose } from '../js/util/pairingCrypto.js';

/**
 * A host that fails MW-BIND must end the launch with IDENTITY_REFUSED — the one
 * reason MoonlightApp._onTransportFailed treats as final. Any other message there
 * is an ordinary transport failure, and the chain then walks down to wss, which
 * carries the stream with no DTLS and so nothing to bind: the refusal would be
 * walked around. Seen on the bench 21/09/2026 — three refused offers, then a
 * stream over wss, and no word to the user.
 */
describe.each([
    ['WebRtcDataChannel', () => new WebRtcDataChannel('ws://test.invalid')],
    ['WebRtcMedia', () => new WebRtcMedia('ws://test.invalid')],
])('%s — an offer that fails MW-BIND', (_name, make) => {
    it('reports IDENTITY_REFUSED and never negotiates', async () => {
        const t = make();
        const onError = vi.fn();
        t.onError = onError;
        const createPc = vi.spyOn(t, '_createPeerConnection').mockImplementation(() => {});
        // A paired identity, and an offer that names another host: refused
        // before the signature is even looked at.
        t._mwBind = {
            hostPublicKey: 'AAAA',
            hostId: 'host-a',
            keyId: 'k',
            nonceB: new Uint8Array(32),
        };

        await t._handleSdpOffer({
            type: 'offer',
            sdp: 'v=0\r\na=fingerprint:sha-256 AA:BB\r\n',
            host_id: 'host-b',
            nonce: 'bm9uY2U=',
            sig: 'c2ln',
        });

        expect(onError).toHaveBeenCalledTimes(1);
        expect(onError.mock.calls[0][0].message).toBe(IDENTITY_REFUSED);
        // No DTLS state was created for the peer that failed to prove itself.
        expect(createPc).not.toHaveBeenCalled();
        expect(t.pc).toBeNull();
    });
});

/**
 * The other half: the HOST refusing our key closes the signalling socket with
 * 1008. Seen 04/10/2026 on the UM790Pro's DEV after it changed port — a session
 * cookie (cookies ignore the port) bound to the key of the old origin. Every
 * WebRTC rung was refused, and the stream came up over wss with a doubled
 * pointer and no word to the user.
 */
describe('isBindRefusedClose', () => {
    it("is the host's MW-BIND refusal, and nothing else", () => {
        expect(isBindRefusedClose({ code: 1008, reason: 'pairing verification failed' })).toBe(
            true,
        );
        expect(isBindRefusedClose({ code: 1008, reason: 'something else' })).toBe(false);
        expect(isBindRefusedClose({ code: 1006, reason: '' })).toBe(false);
        expect(isBindRefusedClose(undefined)).toBe(false);
    });
});
