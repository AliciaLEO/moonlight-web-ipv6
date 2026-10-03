"""What a series.py prefix gave: one row per client × content (the mean of its
rounds), or one per pass with --passes.

    python report.py w0 [--passes] [--json out.json]

Columns (ms unless said):
  click / p90   click → flag, median and p90 (frontend LatencyProbe.js), taken
                after the content-age window: the probe is stopped by then
  up / rest     that click split by the host's stamp: the way up, and the rest
                (flag, capture, encode, the way down, decode, draw)
  shown         the content's age on screen (band of scroll.html). The frames
                the probe reads wait ~12 ms more before their draw (U0.2
                cross-check, 03/10/2026): this is high by about that much
  e2e           host capture → client draw, every frame drawn (stream/FrameLog.js):
                median, mean and p90 (Wi-Fi's cost is in the tail); then the
                median of its first leg, capture → last chunk arrived (`down`),
                which holds the encode, the send queue and the air
  retr%         SCTP chunks the host retransmitted, share of those sent; T3
                the retransmission timeouts
  drop/m        video frames the relay threw away before SCTP (sctpDelta +
                gatedDelta) per minute of stream; buf the largest
                bufferedAmount it logged (KB)
  rep/m         refreshes that showed no new picture, a minute
  Mbps, fps     the overlay's at the end
  msg           a small message's round trip on the input channel (uprobe:
                up + down), median and p90
  udp           a bare UDP ping to the client beside the pass, median / p99
  inRec, cwndH  with relaylog=1: losses inside a recovery already under way
                (usrsctp spared the window a second cut), sends the window held
  >1f% …        with relaylog=1: share of the time bufferedAmount held more
                than 1, 2 and 4 frames' worth
  pace%, pmax   with pace=: the frames the pacing held at least once, and the
                longest it held one (ms)
"""
import argparse
import csv
import glob
import json
import os
import re
import statistics

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
CA_OUT = os.path.join(REPO, "bench-out", "content-age")
WIFI_OUT = os.path.join(REPO, "bench-out", "wifi")
SCTP = re.compile(r"SCTP this session: (\d+) data chunks sent, (\d+) retransmitted "
                  r"\(\s*(\d+) fast\), (\d+) T3")
WINDOW = re.compile(r"SCTP window this session: (\d+) losses inside a recovery, (\d+) chunks "
                    r"fast-retransmitted twice, (\d+) sends held by the window")
PACING = re.compile(r"bench pacing this session: (\d+) frames, (\d+) waited at least once, "
                    r"([\d.]+) ms waited in all, the longest ([\d.]+) ms")
OCCUPANCY = re.compile(r"bufferedAmount above 1 / 2 / 4 frames ([\d.]+) / ([\d.]+) / ([\d.]+) %")
DROPS = re.compile(r"Drop counters .*?sctpDelta: (\d+) sctpDeltaNamed: (\d+) "
                   r"sctpKeyframe: (\d+) gatedDelta: (\d+) .*?bufferedAmount: (\d+)")


def tag_re(prefix):
    # <prefix>-<client>-<gpu>-<content>[-<round>]-v<rate>-<cadence>-r<n>, as
    # series.py and local_matrix.py name them (T7's passes had no round).
    return re.compile(r"^(?P<prefix>%s)-(?P<client>[^-]+)-(?P<gpu>[^-]+)-(?P<content>[^-]+)"
                      r"(?:-(?P<round>\d+))?-v\d+-(?P<cad>.+)-r\d+$" % re.escape(prefix))


def median(xs):
    xs = [x for x in xs if x is not None]
    return statistics.median(xs) if xs else None


def frames(path):
    if not os.path.exists(path):
        return {}
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            try:
                rows.append({k: float(v) for k, v in r.items() if v not in ("", None)})
            except ValueError:
                continue
    e2e = sorted(r["e2eMs"] for r in rows if "e2eMs" in r)
    down = [r["arrivedMs"] - r["captureMs"] for r in rows if "arrivedMs" in r and "captureMs" in r]
    return {"e2e": median(e2e), "e2eMean": statistics.mean(e2e) if e2e else None,
            "e2eP90": e2e[int(0.9 * (len(e2e) - 1))] if e2e else None,
            "down": median(down), "frames": len(rows)}


def host_log(path):
    out = {}
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    m = None
    for m in SCTP.finditer(text):
        pass
    if m:
        sent, retr, fast, t3 = (int(x) for x in m.groups())
        out.update(sent=sent, retr=retr, fast=fast, t3=t3,
                   retrPct=100.0 * retr / sent if sent else None)
    w = None
    for w in WINDOW.finditer(text):
        pass
    if w:
        out.update(inRecovery=int(w.group(1)), twice=int(w.group(2)), cwndHeld=int(w.group(3)))
    o = None
    for o in OCCUPANCY.finditer(text):
        pass
    if o:
        out.update(q1=float(o.group(1)), q2=float(o.group(2)), q4=float(o.group(3)))
    p = None
    for p in PACING.finditer(text):
        pass
    if p:
        frames, paced = int(p.group(1)), int(p.group(2))
        out.update(pacedPct=100.0 * paced / frames if frames else None,
                   paceMaxMs=float(p.group(4)))
    drops = list(DROPS.finditer(text))
    if drops:
        last = drops[-1]
        out["dropped"] = int(last.group(1)) + int(last.group(4))
        out["bufMaxKB"] = max(int(d.group(5)) for d in drops) / 1024
    return out


def overlay_num(stats, key, unit):
    rows = (stats or {}).get("rows") or {}
    v = rows.get(key) or ""
    m = re.search(r"([\d.]+)\s*%s" % unit, v)
    return float(m.group(1)) if m else None


def one(path, prefix):
    tag = os.path.basename(path)[:-5]
    m = tag_re(prefix).match(tag)
    if not m:
        return None
    with open(path) as f:
        d = json.load(f)
    clicks = ((d.get("clicks") or {}).get("summary")) or {}
    up = (d.get("uplink") or [{}])[0] or {}
    h = host_log(path[:-5] + ".server.log")
    fr = frames(path[:-5] + ".frames.csv")
    secs = (d.get("args") or {}).get("secs")
    matrix = tag.rsplit("-v", 1)[0]
    udp = {}
    up_path = os.path.join(WIFI_OUT, matrix + ".udp.json")
    if os.path.exists(up_path):
        with open(up_path) as f:
            udp = json.load(f).get("summary") or {}
    # The stream's length, for the drops a minute: the host's own line when
    # it is there ("… presents in N s"), else the pass's window.
    stream_s = None
    hp = path[:-5] + ".host.txt"
    if os.path.exists(hp):
        with open(hp, encoding="utf-8", errors="replace") as f:
            mm = re.search(r"presents in ([\d.]+) s", f.read())
            stream_s = float(mm.group(1)) if mm else None
    stream_s = stream_s or secs
    return {
        "tag": tag, **m.groupdict(),
        "click": clicks.get("medianMs"), "clickP90": clicks.get("p90Ms"),
        "up": (clicks.get("upMs") or {}).get("median"),
        "rest": (clicks.get("restMs") or {}).get("median"),
        "shown": (d.get("shown") or {}).get("medianMs"),
        "invalid": sum((d.get("invalid") or {}).values()),
        "e2e": fr.get("e2e"), "e2eMean": fr.get("e2eMean"), "e2eP90": fr.get("e2eP90"),
        "down": fr.get("down"),
        "retrPct": h.get("retrPct"), "t3": h.get("t3"),
        "dropPerMin": (60.0 * h["dropped"] / stream_s) if h.get("dropped") is not None and stream_s
        else None,
        "bufMaxKB": h.get("bufMaxKB"),
        "rep": d.get("repeatsPerMinute"), "draws": d.get("drawsPerSecond"),
        "mbps": overlay_num(d.get("overlay"), "Bitrate:", "Mbps"),
        "fps": overlay_num(d.get("overlay"), "Framerate:", "fps"),
        "msg": (up.get("rtt") or {}).get("median"), "msgP90": (up.get("rtt") or {}).get("p90"),
        "udp": udp.get("median"), "udpP99": udp.get("p99"),
        # With relaylog=1 (plan W1): the window's view, and how long the
        # buffer held more than 1, 2 and 4 frames.
        "inRecovery": h.get("inRecovery"), "cwndHeld": h.get("cwndHeld"),
        "q1": h.get("q1"), "q2": h.get("q2"), "q4": h.get("q4"),
        # With pace= (plan W2 A): the frames that waited, the longest wait.
        "pacedPct": h.get("pacedPct"), "paceMaxMs": h.get("paceMaxMs"),
    }


COLS = [("click", 6), ("clickP90", 6), ("up", 5), ("rest", 6), ("shown", 6), ("e2e", 6),
        ("e2eMean", 6), ("e2eP90", 6), ("down", 6), ("retrPct", 6), ("t3", 3),
        ("dropPerMin", 6), ("bufMaxKB", 5), ("rep", 6), ("mbps", 5), ("fps", 4), ("msg", 6),
        ("msgP90", 6), ("udp", 5), ("udpP99", 5), ("inRecovery", 5), ("cwndHeld", 6),
        ("q1", 5), ("q2", 5), ("q4", 5), ("pacedPct", 5), ("paceMaxMs", 5)]
HEAD = {"clickP90": "p90", "e2eMean": "mean", "e2eP90": "p90", "retrPct": "retr%",
        "dropPerMin": "drop/m", "bufMaxKB": "buf", "msgP90": "p90", "udpP99": "p99", "rep": "rep/m",
        "inRecovery": "inRec", "cwndHeld": "cwndH", "q1": ">1f%", "q2": ">2f%", "q4": ">4f%",
        "pacedPct": "pace%", "paceMaxMs": "pmax"}


def fmt(v, w):
    if v is None:
        return " " * (w - 1) + "-"
    if isinstance(v, float):
        return "%*.*f" % (w, 2 if abs(v) < 10 and w >= 5 else 1, v) if w > 3 else "%*d" % (w, v)
    return "%*s" % (w, v)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("prefix")
    ap.add_argument("--passes", action="store_true", help="one row per pass")
    ap.add_argument("--only", default="", help="the passes whose name starts with this")
    ap.add_argument("--json", default="", help="write the rows there too")
    a = ap.parse_args()
    rows = [r for r in (one(p, a.prefix) for p in sorted(glob.glob(
        os.path.join(CA_OUT, (a.only or a.prefix) + "-*.json")))) if r]
    if not a.passes:
        groups = {}
        for r in rows:
            groups.setdefault((r["client"], r["gpu"], r["content"]), []).append(r)
        merged = []
        for (client, gpu, content), rs in groups.items():
            row = {"tag": "%s %s %s" % (client, gpu, content), "client": client, "gpu": gpu,
                   "content": content, "n": len(rs)}
            for k, _ in COLS:
                vals = [r[k] for r in rs if r[k] is not None]
                row[k] = statistics.mean(vals) if vals else None
            merged.append(row)
        rows = merged
    name_w = max([len(r["tag"]) for r in rows] + [10])
    print("%-*s %s" % (name_w, "pass" if a.passes else "client gpu content",
                       " ".join("%*s" % (w, HEAD.get(k, k)) for k, w in COLS))
          + ("" if a.passes else "  n"))
    for r in rows:
        print("%-*s %s" % (name_w, r["tag"], " ".join(fmt(r[k], w) for k, w in COLS))
              + ("" if a.passes else "  %d" % r["n"]))
        if a.passes and r.get("invalid"):
            print("%-*s   !! %d band reads invalid" % (name_w, "", r["invalid"]))
    if a.json:
        with open(a.json, "w") as f:
            json.dump(rows, f, indent=1)


if __name__ == "__main__":
    main()
