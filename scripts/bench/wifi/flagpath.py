"""Where the flag's frame waited, click by click (plan « Wi-Fi : la vidéo qui
attend dans SCTP », W1).

    python flagpath.py <tag> [<tag>...]          # one pass each: its clicks, then a summary
    python flagpath.py --prefix w1 [--clicks]     # every pass of a series, a row each

A pass run with `--tuning relaylog=1` leaves three logs beside its JSON
(bench-out/content-age):

- `<tag>.relay.csv`, the relay's frame log on the host's steady clock (µs):
  each frame's capture, its hand-over to the relay, the decision (sent, dropped
  by the backlog, gated awaiting a keyframe…), the sender's first and last
  fragment into the DataChannel, `bufferedAmount` before and after;
- `<tag>.clicks.frames.csv`, the client's per-frame log of the clicks' minute
  (frontend stream/FrameLog.js): arrival of the last chunk, decode, draw, on the
  client's clock, and each frame's stamp on the host's;
- `<tag>.server.log`, where the host says when each flag went up
  (`[LatencyFlag] … shown at steady N us`).

For each click the probe measured, the frame that showed its flag is the last
one drawn before the probe saw it. Its host stamp finds it in the relay's log,
the flag's line finds the moment it went up, and the click's own split (way
up, injection) closes the loop. The legs, in ms:

  up        click sent → arrived on the host (the host's stamp, InputUplink.js)
  inject    arrived → injected
  raise     injected → flag shown (the overlay's message loop)
  toCap     flag shown → capture of the frame that showed it: the compositor,
            the capture's wait, and every frame in between that did not go
  encode    capture → handed to the relay
  send      handed → last fragment into the DataChannel (the sender's queue)
  net       last fragment in → last chunk arrived on the client: libdatachannel's
            queue, usrsctp's (unseen), the air, and any wait behind a
            retransmitted chunk (ordered delivery)
  decode    arrived → decoded
  draw      decoded → drawn
  detect    drawn → the probe saw the flag
  sum       their total, against the measured click → flag

And for the flag's frame: `skipped`, frames captured after the flag went up
that did not go out (gated, dropped, evicted) — when the flag was in one of
them, the click waited for the next that went; `buf` the bufferedAmount the
relay saw (KB), `after` what libdatachannel still held after its last fragment
(0: usrsctp took it all); `retr` usrsctp's retransmissions while it was in
flight; `srtt` SCTP's smoothed round trip.
"""
import argparse
import bisect
import csv
import glob
import json
import os
import re
import statistics

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
CA_OUT = os.path.join(REPO, "bench-out", "content-age")
FLAG = re.compile(r"\[LatencyFlag\] injected click at .* shown at steady (\d+) us")
LEGS = ["up", "inject", "raise", "toCap", "encode", "send", "net", "decode", "draw", "detect"]


def read_csv(path):
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            out = {}
            for k, v in r.items():
                try:
                    out[k] = float(v) if v not in ("", None) else None
                except ValueError:
                    out[k] = v
            rows.append(out)
    return rows


def q(vals, p):
    v = sorted(x for x in vals if x is not None)
    return v[min(len(v) - 1, int(p * len(v)))] if v else None


def med(vals):
    v = [x for x in vals if x is not None]
    return statistics.median(v) if v else None


def load(tag):
    base = os.path.join(CA_OUT, tag)
    need = [base + ".json", base + ".relay.csv", base + ".clicks.frames.csv", base + ".server.log"]
    missing = [os.path.basename(p) for p in need if not os.path.exists(p)]
    if missing:
        raise SystemExit("%s: missing %s (a pass with --tuning relaylog=1?)"
                         % (tag, ", ".join(missing)))
    with open(base + ".json") as f:
        d = json.load(f)
    relay = read_csv(base + ".relay.csv")
    frames = [r for r in read_csv(base + ".clicks.frames.csv") if r.get("drawnMs") is not None]
    with open(base + ".server.log", encoding="utf-8", errors="replace") as f:
        flags = sorted(int(m.group(1)) for m in FLAG.finditer(f.read()))
    return d, relay, frames, flags


def clicks_of(tag):
    d, relay, frames, flags = load(tag)
    clicks = d.get("clicks") or {}
    origin = clicks.get("timeOrigin")
    if origin is None:
        raise SystemExit("%s: the clicks carry no time origin (an older pass.py)" % tag)
    # The client's clock to the host's: every frame carries both stamps.
    offs = [r["hostMs"] - r["captureMs"] for r in frames
            if r.get("hostMs") is not None and r.get("captureMs") is not None]
    if not offs:
        raise SystemExit("%s: the client's frame log has no host stamps" % tag)
    off = statistics.median(offs)
    drawn = [r["drawnMs"] for r in frames]
    by_cap = {}
    for r in relay:
        if r.get("captureUs") is not None and r["captureUs"] >= 0:
            by_cap[int(r["captureUs"] // 1000)] = r
    caps = sorted((r["captureUs"], i) for i, r in enumerate(relay)
                  if r.get("captureUs") is not None and r["captureUs"] >= 0)
    cap_keys = [c for c, _ in caps]
    inn = sorted((r["inUs"], i) for i, r in enumerate(relay) if r.get("inUs"))
    inn_keys = [t for t, _ in inn]
    out = []
    for s in clicks.get("samples") or []:
        if not s.get("ok") or s.get("latencyMs") is None:
            continue
        t0 = s["ts"] / 1000.0 - origin
        hit = t0 + s["latencyMs"]
        i = bisect.bisect_right(drawn, hit + 0.5) - 1
        if i < 0:
            continue
        fr = frames[i]
        rec = by_cap.get(int(round(fr["hostMs"]))) if fr.get("hostMs") is not None else None
        t0h = (t0 + off) * 1000.0  # µs, host clock
        up = s.get("upMs")
        inject = s.get("hostInMs")
        injected = t0h + 1000.0 * ((up or 0) + (inject or 0))
        k = bisect.bisect_left(flags, int(t0h))
        shown = flags[k] if k < len(flags) and flags[k] - t0h < 250_000 else None
        c = {"ts": s["ts"], "latency": s["latencyMs"], "up": up, "inject": inject,
             "raise": (shown - injected) / 1000.0 if shown else None,
             "decode": (fr["decodedMs"] - fr["arrivedMs"]) if fr.get("decodedMs") is not None
             and fr.get("arrivedMs") is not None else None,
             "draw": (fr["drawnMs"] - fr["decodedMs"]) if fr.get("decodedMs") is not None else None,
             "detect": hit - fr["drawnMs"], "matched": rec is not None}
        if rec is not None:
            arrived_h = ((fr["arrivedMs"] + off) * 1000.0
                         if fr.get("arrivedMs") is not None else None)
            c["toCap"] = (rec["captureUs"] - shown) / 1000.0 if shown else None
            c["encode"] = (rec["inUs"] - rec["captureUs"]) / 1000.0
            c["send"] = (rec["lastUs"] - rec["inUs"]) / 1000.0 if rec.get("lastUs") else None
            c["net"] = ((arrived_h - rec["lastUs"]) / 1000.0
                        if arrived_h and rec.get("lastUs") else None)
            c["buf"] = (rec.get("buffered") or 0) / 1024.0
            c["after"] = (rec["bufferedAfter"] / 1024.0
                          if rec.get("bufferedAfter") is not None and rec["bufferedAfter"] >= 0
                          else None)
            c["srtt"] = rec.get("srttMs") if (rec.get("srttMs") or -1) >= 0 else None
            c["key"] = bool(rec.get("key"))
            # Frames captured between the flag going up and this one, that did
            # not go out: the flag may have been in the first of them.
            skipped = []
            if shown:
                a = bisect.bisect_right(cap_keys, shown)
                b = bisect.bisect_left(cap_keys, rec["captureUs"])
                skipped = [relay[caps[j][1]]["outcome"] for j in range(a, b)
                           if relay[caps[j][1]]["outcome"] != "sent"]
            c["skipped"] = len(skipped)
            c["skippedWhy"] = ",".join(sorted(set(skipped)))
            # usrsctp's retransmissions while it was in flight: the counters of
            # the frame handed over just before it arrived, less its own.
            if arrived_h:
                j = bisect.bisect_right(inn_keys, arrived_h) - 1
                later = relay[inn[j][1]] if j >= 0 else None
                c["retr"] = (later["retrans"] - rec["retrans"]) if later else None
        c["sum"] = sum(c[k] for k in LEGS if c.get(k) is not None) if all(
            c.get(k) is not None for k in LEGS) else None
        out.append(c)
    return out, relay


def fmt(v, w=6):
    if v is None:
        return " " * (w - 1) + "-"
    if isinstance(v, bool):
        return "%*s" % (w, "k" if v else "")
    if isinstance(v, (int, float)):
        return "%*.1f" % (w, v)
    return "%*s" % (w, v)


def summary(tag, cs, relay):
    row = {"tag": tag, "n": len(cs), "matched": sum(1 for c in cs if c["matched"])}
    for k in ["latency"] + LEGS + ["sum", "buf", "after", "retr", "srtt"]:
        row[k] = med([c.get(k) for c in cs])
    row["netP90"] = q([c.get("net") for c in cs], 0.9)
    row["latP90"] = q([c.get("latency") for c in cs], 0.9)
    m = [c for c in cs if c["matched"]]
    row["skipShare"] = 100.0 * sum(1 for c in m if c.get("skipped")) / len(m) if m else None
    row["retrShare"] = 100.0 * sum(1 for c in m if (c.get("retr") or 0) > 0) / len(m) if m else None
    row["afterShare"] = (100.0 * sum(1 for c in m if (c.get("after") or 0) > 0) / len(m)
                         if m else None)
    outcomes = {}
    for r in relay:
        outcomes[r["outcome"]] = outcomes.get(r["outcome"], 0) + 1
    row["outcomes"] = outcomes
    # Every frame sent, not only the flags: how long its last fragment took
    # to go in once handed over.
    row["sendAll"] = med([(r["lastUs"] - r["inUs"]) / 1000.0 for r in relay
                          if r["outcome"] == "sent" and r.get("lastUs")])
    return row


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("tags", nargs="*")
    ap.add_argument("--prefix", default="", help="every pass whose name starts with this")
    ap.add_argument("--clicks", action="store_true", help="one line per click too")
    ap.add_argument("--json", default="")
    ap.add_argument("--dir", default="",
                    help="where the passes are (default bench-out/content-age)")
    a = ap.parse_args()
    global CA_OUT
    if a.dir:
        CA_OUT = os.path.abspath(a.dir)
    tags = list(a.tags)
    if a.prefix:
        tags += sorted(os.path.basename(p)[:-len(".relay.csv")]
                       for p in glob.glob(os.path.join(CA_OUT, a.prefix + "*.relay.csv")))
    rows = []
    for tag in tags:
        cs, relay = clicks_of(tag)
        if a.clicks or len(tags) == 1:
            print("== %s: %d clicks measured, %d found in the relay's log" % (
                tag, len(cs), sum(1 for c in cs if c["matched"])))
            cols = ["latency"] + LEGS + ["sum", "buf", "after", "retr", "srtt"]
            print(" ".join("%6s" % k[:6] for k in cols) + "  key skipped")
            for c in cs:
                print(" ".join(fmt(c.get(k)) for k in cols) + "  %3s %s" % (
                    "k" if c.get("key") else "", ("%d %s" % (c["skipped"], c["skippedWhy"]))
                    if c.get("skipped") else ""))
        rows.append(summary(tag, cs, relay))
    if not rows:
        raise SystemExit("no pass")
    w = max(len(r["tag"]) for r in rows)
    cols = ["latency", "latP90"] + LEGS + ["netP90", "sum", "sendAll", "buf", "after", "srtt",
                                          "skipShare", "retrShare", "afterShare"]
    heads = {"latency": "click", "latP90": "p90", "netP90": "netp90", "sendAll": "sendAl",
             "skipShare": "skip%", "retrShare": "retr%", "afterShare": "aft%"}
    print("%-*s %3s " % (w, "pass", "n") + " ".join("%6s" % heads.get(k, k[:6]) for k in cols))
    for r in rows:
        print("%-*s %3d " % (w, r["tag"], r["n"]) + " ".join(fmt(r.get(k)) for k in cols))
        print("%-*s     frames: %s" % (w, "", ", ".join("%s %d" % kv for kv in
                                                    sorted(r["outcomes"].items()))))
    if a.json:
        with open(a.json, "w") as f:
            json.dump(rows, f, indent=1)


if __name__ == "__main__":
    main()
