/*
 * MoonlightWeb — messages the host sends to the stream view.
 * Copyright (C) 2026 Bruno Martin. GPLv3 — see repository LICENSE.
 */

/**
 * The JSON messages a host sends on the input channel that belong to the
 * stream view — handed to its `onStats` — as opposed to the ones a transport
 * handles itself (take-over, revocation, exit notice…).
 *
 * One list for every transport. There used to be four copies, one per message
 * handler in WebRtcDataChannel and WebRtcMedia, and each new message had to be
 * added to all of them: `cursorpos`, the host's word on where the pointer is,
 * was added to none, so a touch screen drawing the pointer itself never heard
 * it and started from the middle of the picture, uncorrected (found
 * 15/09/2026). A transport that never carries a message (WSS has no pointer
 * messages) loses nothing by listing it.
 */
const VIEW_MESSAGES = new Set([
    'stats',
    'pong',
    'rumble',
    'clipboard',
    'clipboardcaps',
    // The pointer, when the client draws it: its shape, and its position.
    'cursor',
    'cursorpos',
    // Native host only: presses refused, and the display changing mode.
    'inputgate',
    'displayformat',
    // Native host, a guest on the shared feed: the feed changes codec.
    'feedcodec',
]);

/** Whether a host message of this type goes to the stream view. */
export function isViewMessage(type) {
    return VIEW_MESSAGES.has(type);
}
