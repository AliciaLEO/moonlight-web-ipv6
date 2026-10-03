/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect } from 'vitest';
import { isViewMessage } from '../js/api/hostMessages.js';

describe('hostMessages — what reaches the stream view', () => {
    it('forwards every message StreamView handles, the pointer position included', () => {
        for (const type of [
            'stats',
            'pong',
            'rumble',
            'clipboard',
            'clipboardcaps',
            'cursor',
            'cursorpos',
            'inputgate',
            'displayformat',
            'feedcodec',
            'fpsstep',
            'inputstamp',
        ]) {
            expect(isViewMessage(type)).toBe(true);
        }
    });

    it('keeps what the transports handle themselves', () => {
        for (const type of ['takeover', 'revoked', 'exit', '', undefined]) {
            expect(isViewMessage(type)).toBe(false);
        }
    });
});
