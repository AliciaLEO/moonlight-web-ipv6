/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

/**
 * PipelineDiag — observable behaviour only: what a snapshot reports for a given
 * sequence of pipeline events, and what the formatted line contains.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import {
    PipelineDiag,
    MainThreadProbe,
    formatDiag,
    formatMainThread,
} from '../js/stream/PipelineDiag.js';

/** Drive performance.now() by hand so the sliding window is deterministic. */
let clock = 0;

beforeEach(() => {
    clock = 1000;
    vi.spyOn(performance, 'now').mockImplementation(() => clock);
});

afterEach(() => {
    vi.restoreAllMocks();
});

describe('PipelineDiag', () => {
    it('reports zeros before any observation', () => {
        const snap = new PipelineDiag().snapshot();
        expect(snap.arrivalAvgMs).toBe(0);
        expect(snap.decodeQueueMax).toBe(0);
        expect(snap.renderPath).toBe('');
        expect(snap.dropStale).toBe(0);
    });

    it('measures the interval between arrivals, not the first one', () => {
        const d = new PipelineDiag();
        d.noteArrival(); // no interval yet
        clock += 16;
        d.noteArrival();
        clock += 20;
        d.noteArrival();

        const snap = d.snapshot();
        expect(snap.arrivalAvgMs).toBe(18);
        expect(snap.arrivalMaxMs).toBe(20);
    });

    it('records encoded frame sizes, which have no millisecond bound', () => {
        const d = new PipelineDiag();
        d.noteArrival(1000);
        d.noteArrival(180000); // full-size frame: well past the ms sanity bound
        d.noteArrival(); // size unknown — not a zero-byte sample

        const snap = d.snapshot();
        expect(snap.frameBytesAvg).toBe(90500);
        expect(snap.frameBytesMax).toBe(180000);
    });

    it('reports avg and max queue depths', () => {
        const d = new PipelineDiag();
        d.noteDecodeQueue(1);
        d.noteDecodeQueue(7);
        d.noteFrameQueue(0);
        d.noteFrameQueue(2);

        const snap = d.snapshot();
        expect(snap.decodeQueueAvg).toBe(4);
        expect(snap.decodeQueueMax).toBe(7);
        expect(snap.frameQueueAvg).toBe(1);
        expect(snap.frameQueueMax).toBe(2);
    });

    it('splits the draw into submit and wait, and keeps the last path', () => {
        const d = new PipelineDiag();
        d.noteDraw({ submitMs: 1, waitMs: 20, path: 'sgsr' });
        d.noteDraw({ submitMs: 3, waitMs: 10, path: 'off' });
        d.noteDraw(null); // renderer not ready — ignored, no throw

        const snap = d.snapshot();
        expect(snap.renderSubmitMs).toBe(2);
        expect(snap.renderWaitMs).toBe(15);
        expect(snap.renderPath).toBe('off');
    });

    it('divides the draw latency by the concurrency to get the per-frame cost', () => {
        // Same GPU cost per frame, drawn two at a time: each draw waits about
        // twice as long, but the pipeline still pays ~10ms per frame.
        const serial = new PipelineDiag();
        serial.noteDraw({ submitMs: 0.2, waitMs: 10, path: 'fsr1' }, 1);
        const solo = serial.snapshot();
        expect(solo.drawConcurrency).toBe(1);
        expect(solo.renderServiceMs).toBeCloseTo(10.2, 5);

        const pipelined = new PipelineDiag();
        pipelined.noteDraw({ submitMs: 0.2, waitMs: 20.2, path: 'fsr1' }, 2);
        const duo = pipelined.snapshot();
        expect(duo.drawConcurrency).toBe(2);
        expect(duo.renderServiceMs).toBeCloseTo(10.2, 5);
    });

    it('assumes serial drawing when the concurrency is not given', () => {
        const d = new PipelineDiag();
        d.noteDraw({ submitMs: 0, waitMs: 8, path: 'off' });
        const snap = d.snapshot();
        expect(snap.drawConcurrency).toBe(1);
        expect(snap.renderServiceMs).toBe(8);
    });

    it('counts drops per cause and ignores unknown ones', () => {
        const d = new PipelineDiag();
        d.noteDrop('stale');
        d.noteDrop('stale');
        d.noteDrop('queueFull');
        d.noteDrop('nonsense');

        const snap = d.snapshot();
        expect(snap.dropStale).toBe(2);
        expect(snap.dropQueueFull).toBe(1);
        expect(snap.dropBackpressure).toBe(0);
    });

    it('forgets samples older than the window (drop counters stay cumulative)', () => {
        const d = new PipelineDiag(2000);
        d.noteDecodeQueue(8);
        d.noteDrop('stale');
        clock += 2500;
        d.noteDecodeQueue(1);

        const snap = d.snapshot();
        expect(snap.decodeQueueAvg).toBe(1);
        expect(snap.decodeQueueMax).toBe(1);
        expect(snap.dropStale).toBe(1);
    });

    it('rejects out-of-range samples instead of poisoning the window', () => {
        const d = new PipelineDiag();
        d.noteArrival();
        clock += 60000; // stream stalled a minute — not a cadence measurement
        d.noteArrival();
        d.noteDecodeQueue(-1);

        const snap = d.snapshot();
        expect(snap.arrivalAvgMs).toBe(0);
        expect(snap.decodeQueueAvg).toBe(0);
    });
});

describe('formatDiag', () => {
    it('returns an empty string without a snapshot', () => {
        expect(formatDiag(null, {})).toBe('');
    });

    it('shows the decoded-vs-presented gap and the draw split', () => {
        const d = new PipelineDiag();
        d.noteDraw({ submitMs: 1.5, waitMs: 22.25, path: 'drawImage' });
        d.noteDrop('stale');

        const line = formatDiag(d.snapshot(), {
            decodedFps: 60,
            presentedFps: 41.6,
            dropsPerSec: 18.4,
        });

        expect(line).toContain('fps in/out 60/42');
        expect(line).toContain('frame 0.0/0.0KB');
        expect(line).toContain('draw 1.5+22.3ms ×1.0→23.8ms');
        expect(line).toContain('[drawImage]');
        expect(line).toContain('stale 1');
    });

    it('tolerates missing rates', () => {
        const line = formatDiag(new PipelineDiag().snapshot(), {});
        expect(line).toContain('fps in/out 0/0');
    });
});

describe('MainThreadProbe', () => {
    afterEach(() => {
        vi.useRealTimers();
    });

    it('turns the input messages of its window into a rate and a cost', () => {
        const probe = new MainThreadProbe(2000);
        for (let i = 0; i < 2000; i++) probe.noteInput(0.01);
        const snap = probe.snapshot();
        expect(snap.inputsPerSec).toBe(1000);
        expect(snap.inputMsPerSec).toBeCloseTo(10, 5);
    });

    it('forgets the input that left the window', () => {
        const probe = new MainThreadProbe(2000);
        probe.noteInput(0.01);
        clock += 2500;
        expect(probe.snapshot().inputsPerSec).toBe(0);
    });

    it('reads a timer that fires late as event-loop lag', () => {
        vi.useFakeTimers();
        vi.spyOn(performance, 'now').mockImplementation(() => clock);
        const probe = new MainThreadProbe(2000);
        probe.start();
        // On time, then held up 30ms by whatever else the thread was doing.
        clock += 10;
        vi.advanceTimersByTime(10);
        clock += 40;
        vi.advanceTimersByTime(10);
        const snap = probe.snapshot();
        probe.stop();
        expect(snap.loopLagMaxMs).toBe(30);
        expect(snap.loopLagAvgMs).toBe(15);
    });

    it('stops its timer', () => {
        vi.useFakeTimers();
        const probe = new MainThreadProbe(2000);
        probe.start();
        probe.stop();
        expect(vi.getTimerCount()).toBe(0);
    });

    it('formats the tail of the perf line', () => {
        const line = formatMainThread({
            inputsPerSec: 987.4,
            inputMsPerSec: 14.26,
            loopLagAvgMs: 0.4,
            loopLagMaxMs: 21.7,
            longTasks: 2,
            longTaskMaxMs: 63.2,
        });
        expect(line).toBe('input 987/s 14.3ms/s · loop lag 0.4/21.7ms · longtask 2/63ms');
        expect(formatMainThread(null)).toBe('');
    });

    it('turns the reads of a forwarded pad into a rate, a gap and the states sent', () => {
        const probe = new MainThreadProbe(2000);
        // 500 reads 4 ms apart, one of them held up 15 ms; 120 states out.
        for (let i = 0; i < 500; i++) probe.notePadRead(i === 7 ? 15 : 4);
        for (let i = 0; i < 120; i++) probe.notePadSend();
        const snap = probe.snapshot();
        expect(snap.padReadsPerSec).toBe(250);
        expect(snap.padSendsPerSec).toBe(60);
        expect(snap.padGapP95Ms).toBe(4);
        expect(snap.padGapMaxMs).toBe(15);
    });

    it('times the mouse: report age, the gap inside a movement, and the wait', () => {
        const probe = new MainThreadProbe(2000);
        // One movement coalesced to 60 Hz, each motion 0.5 ms after its report.
        for (let i = 0; i < 30; i++) {
            probe.noteMouseSend(0.5);
            clock += 16;
        }
        // The hand rests a second, then moves once more: that is no 1 s gap.
        clock += 1000;
        probe.noteMouseSend(2.5);
        const snap = probe.snapshot();
        expect(snap.mouseSendsPerSec).toBe(15.5);
        expect(snap.mouseAgeAvgMs).toBeCloseTo(17.5 / 31, 5);
        expect(snap.mouseAgeMaxMs).toBe(2.5);
        expect(snap.mouseGapAvgMs).toBe(16);
        expect(snap.mouseGapP95Ms).toBe(16);
        // A report waits its age plus half a gap on average.
        expect(formatMainThread(snap)).toContain(
            ' · mouse 16/s age 0.6/2.5ms gap 16.0/16.0ms wait 8.6ms',
        );
    });

    it('says nothing about pads until one is read, and forgets them after', () => {
        const probe = new MainThreadProbe(2000);
        const quiet = formatMainThread(probe.snapshot());
        expect(quiet).not.toContain('pad');
        expect(quiet).not.toContain('mouse');
        probe.notePadRead(16.7);
        probe.notePadSend();
        expect(formatMainThread(probe.snapshot())).toBe(
            quiet + ' · pad read 1/s sent 1/s gap 16.7/16.7ms',
        );
        clock += 2500;
        expect(formatMainThread(probe.snapshot())).toBe(quiet);
    });
});
