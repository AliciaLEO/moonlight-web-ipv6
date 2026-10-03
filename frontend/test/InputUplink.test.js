/*
 * MoonlightWeb — tests for stream/InputUplink.js: the way up of an input, put
 * on this client's clock from the host's answer to its stamp.
 */
import { describe, it, expect, vi } from 'vitest';
import { InputUplink, summarise } from '../js/stream/InputUplink.js';

/** Host clock = client clock + 5 s, in µs; links of `upMs` and `downMs`. */
function host(clientMs) {
    return (clientMs + 5000) * 1000;
}

function feedPongs(uplink, upMs = 1, downMs = 1, n = 6) {
    // Exchanges a second apart, symmetric unless told otherwise.
    for (let i = 0; i < n; i++) {
        const sent = i * 1000;
        uplink.notePong(
            { type: 'pong', seq: i, ts: sent, host: host(sent + upMs) },
            sent + upMs + downMs,
        );
    }
}

function make(extra = {}) {
    const sent = [];
    const pings = [];
    const timers = {
        setInterval: vi.fn(() => 1),
        clearInterval: vi.fn(),
        setTimeout: vi.fn(),
    };
    const uplink = new InputUplink({
        send: (m) => sent.push(m),
        sendPing: (seq, ts) => pings.push({ seq, ts }),
        timers,
        ...extra,
    });
    return { uplink, sent, pings, timers };
}

describe('InputUplink', () => {
    it("puts the host's arrival on this clock: the way up, the injection, the round trip", () => {
        const { uplink } = make();
        feedPongs(uplink);
        const onReply = vi.fn();
        const id = uplink.stamp(10000, onReply);
        // Arrived 42 ms after it left, injected 0.3 ms later, answer back at +45.
        const split = uplink.noteReply(
            { type: 'inputstamp', id, recv: host(10042), done: host(10042.3) },
            10045,
        );
        expect(split.upMs).toBeCloseTo(42, 3);
        expect(split.hostInMs).toBeCloseTo(0.3, 3);
        expect(split.rttMs).toBe(45);
        expect(onReply).toHaveBeenCalledWith(split);
        // Answered once only.
        expect(uplink.noteReply({ type: 'inputstamp', id, recv: 1, done: 1 }, 1)).toBeNull();
    });

    it('says nothing of the way up before the clock is known', () => {
        const { uplink } = make();
        const id = uplink.stamp(0);
        const split = uplink.noteReply({ id, recv: host(10), done: host(10) }, 12);
        expect(split.upMs).toBeNull();
        expect(split.rttMs).toBe(12);
    });

    it('forgets stamps nobody answered', () => {
        const { uplink } = make();
        const old = uplink.stamp(0);
        uplink.stamp(6000);
        expect(uplink.noteReply({ id: old, recv: 1, done: 1 }, 6001)).toBeNull();
    });

    it('pings fast only while something holds it', () => {
        const { uplink, pings, timers } = make();
        uplink.hold();
        uplink.hold();
        expect(pings).toHaveLength(1);
        expect(timers.setInterval).toHaveBeenCalledTimes(1);
        uplink.release();
        expect(timers.clearInterval).not.toHaveBeenCalled();
        uplink.release();
        expect(timers.clearInterval).toHaveBeenCalledTimes(1);
        uplink.release(); // one too many: harmless
        expect(timers.clearInterval).toHaveBeenCalledTimes(1);
    });

    it('benches the way up with dated messages that do nothing on the host', async () => {
        vi.useFakeTimers();
        let now = 0;
        const results = [];
        const sent = [];
        let uplink;
        uplink = new InputUplink({
            send: (m) => {
                sent.push(m);
                if (m.type === 'uprobe') {
                    // The host answers 30 ms later, the message having taken 20 up.
                    const at = now;
                    setTimeout(
                        () =>
                            uplink.noteReply(
                                { id: m.stamp, recv: host(at + 20), done: host(at + 20) },
                                at + 30,
                            ),
                        30,
                    );
                }
            },
            sendPing: (seq, ts) => {
                setTimeout(() => uplink.notePong({ seq, ts, host: host(ts + 1) }, ts + 2), 2);
            },
            bufferedAmount: () => 0,
            results,
            now: () => now,
        });
        const step = async (ms) => {
            for (let i = 0; i < ms; i++) {
                now += 1;
                await vi.advanceTimersByTimeAsync(1);
            }
        };
        const p = uplink.run({ hz: 50, secs: 2, label: 'test' });
        await step(1000 + 2000 + 1600);
        const entry = await p;
        expect(entry.label).toBe('test');
        expect(entry.sent).toBeGreaterThanOrEqual(99);
        expect(entry.answered).toBe(entry.sent);
        expect(entry.up.median).toBeCloseTo(20, 0);
        expect(entry.rtt.median).toBe(30);
        expect(
            sent.filter((m) => m.type === 'uprobe').every((m) => typeof m.stamp === 'number'),
        ).toBe(true);
        expect(results).toEqual([entry]);
        vi.useRealTimers();
    });

    it('summarises median, p90, p99 and max', () => {
        const s = summarise([5, 1, 3, 2, 4, null, NaN]);
        expect(s).toEqual({ n: 5, median: 3, p90: 5, p99: 5, max: 5 });
        expect(summarise([]).median).toBeNull();
    });
});
