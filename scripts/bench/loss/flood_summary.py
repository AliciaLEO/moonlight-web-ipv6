#!/usr/bin/env python3
"""A table of flood_run.py passes: one row per phase, one column group per pass.

    flood_summary.py run1.jsonl [run2.jsonl ...] [--markdown]

Each cell: delivered Mbit/s (median of the phase's settled seconds), then the
loss the receiver counted and the extra delay p95 over the session's shortest.
"""
import json
import sys


def load(path):
    head, rows = {}, []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            o = json.loads(line)
            if "phase" in o:
                rows.append(o)
            else:
                head = o
    return head, rows


def key(p):
    burst = p.get("burst", 1)
    return "rtt %g ms, loss %g %%%s" % (p["rtt"], p["loss"],
                                        (", bursts of %g" % burst) if burst and burst > 1 else "")


def cell(r):
    if r is None or r.get("kbps") is None:
        return "-"
    return "%.1f Mb/s (loss %s %%, +%s ms p95)" % (r["kbps"] / 1000.0, r.get("lossPct"),
                                                   r.get("delayP95"))


def main():
    paths = [a for a in sys.argv[1:] if not a.startswith("--")]
    md = "--markdown" in sys.argv
    passes = []
    for p in paths:
        head, rows = load(p)
        passes.append((head.get("label") or p, {key(r["phase"]): r for r in rows}))
    order = []
    for _, rows in passes:
        for k in rows:
            if k not in order:
                order.append(k)
    if md:
        print("| phase | " + " | ".join(name for name, _ in passes) + " |")
        print("|---|" + "---|" * len(passes))
        for k in order:
            print("| %s | %s |" % (k, " | ".join(cell(rows.get(k)) for _, rows in passes)))
    else:
        for name, rows in passes:
            print("== %s" % name)
            for k in order:
                r = rows.get(k)
                busy = r.get("mainThreadBusyPct") if r else None
                print("  %-34s %s, %s msg/s, main thread %s %%"
                      % (k, cell(r), r.get("msgsPerSec") if r else "-", busy))


if __name__ == "__main__":
    main()
