"""One line per run and guest count, from owner_load.py's results.

    python summary.py ../../../bench-out/shared-feed/s0-*.json

The windows with the same number of guests are folded together: the mean of
their medians and of their p99, the worst p99 of any second, and what the
machine was doing (workers, NVENC sessions, video-encode engine load).
"""
import json
import statistics
import sys


def mean(xs):
    xs = [x for x in xs if isinstance(x, (int, float))]
    return statistics.mean(xs) if xs else None


def fmt(v, w=6, d=2):
    return ("%*.*f" % (w, d, v)) if isinstance(v, (int, float)) else " " * (w - 1) + "-"


def first_number(text):
    import re
    m = re.search(r"(-?\d+(?:\.\d+)?)", text or "")
    return float(m.group(1)) if m else None


def main(paths):
    print("%-14s %-6s %2s | %-20s | %-20s | %6s | %6s | %5s | %5s | %4s | %-9s | %s" % (
        "run", "guests", "n", "owner total p50/p99", "owner encode p50/p99", "worst",
        "E2E", "fps", "sent", "wkrs", "enc. load", "guests' encode p50 (fps)"))
    for p in paths:
        with open(p) as f:
            d = json.load(f)
        enc_luid = (d.get("encoder") or {}).get("luid")
        by = {}
        for w in d["windows"]:
            by.setdefault(w["guests"], []).append(w)
        for guests in sorted(by):
            ws = by[guests]
            o = lambda s, k: mean([(w["owner"].get(s) or {}).get(k) for w in ws])  # noqa: E731
            worst = max(((w["owner"].get("total") or {}).get("p99WorstMs") or 0) for w in ws)
            e2e = mean([first_number(w["ownerOverlay"].get("Latency:")) for w in ws])
            fps = mean([first_number(w["ownerOverlay"].get("Framerate:")) for w in ws])
            # What the host really sent the owner: frames the stats windows
            # closed, over the seconds they covered.
            sent = mean([(w["owner"].get("total") or {}).get("frames", 0) /
                         max(1, w["owner"].get("seconds", 1)) for w in ws])
            wk = mean([w.get("workers") for w in ws])
            def busiest(w):
                engines = (w.get("encodeEngines") or {}).get(enc_luid)
                # Older runs kept one number per adapter; newer ones every
                # video engine type, of which the busiest is the encoder's.
                if isinstance(engines, dict):
                    return max(engines.values()) if engines else None
                return engines
            load = mean([busiest(w) for w in ws]) if enc_luid else None
            nv = mean([(w.get("nvenc") or {}).get("sessions") for w in ws])
            gs = []
            for w in ws:
                for slot, s in sorted(w["guestStages"].items()):
                    enc = (s.get("encode") or {}).get("p50Ms")
                    fr = (s.get("total") or {}).get("frames", 0) / max(1, s.get("seconds", 1))
                    gs.append("%s(%.0f)" % (fmt(enc, 4, 1).strip(), fr))
            print("%-14s %-6d %2d | %s / %s | %s / %s | %s | %s | %s | %s | %s | %s | %s" % (
                d["tag"][:14], guests, len(ws), fmt(o("total", "p50Ms")), fmt(o("total", "p99Ms")),
                fmt(o("encode", "p50Ms")), fmt(o("encode", "p99Ms")), fmt(worst), fmt(e2e, 6, 1),
                fmt(fps, 5, 1), fmt(sent, 5, 1), fmt(wk, 4, 1),
                (fmt(load, 3, 0) + "%" if load is not None else "   -") +
                (" nv" + fmt(nv, 2, 0) if nv is not None else ""),
                " ".join(gs[:6]) or "-"))


if __name__ == "__main__":
    main(sys.argv[1:])
