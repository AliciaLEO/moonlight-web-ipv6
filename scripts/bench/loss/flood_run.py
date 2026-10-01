#!/usr/bin/env python3
"""What SCTP carries to a real browser, phase by phase (plan Idées Punktfunk, A0.3-A0.4).

Runs on the CLIENT machine (Linux, netem.py next door), against a Chrome driven
over DevTools. The host must flood: its native tuning holds `flood=max` (or
`flood=<kbps>`), and `sctpcc=` / `floodchannel=` say what is measured. This
script sets the client's half (localStorage `mw_flood`), opens the stream on a
native display tile, then walks the phases, shaping the link with netem and
reading the flood counter (`globalThis.__mwFlood`, FloodCounter.js) and
Chrome's main-thread busy time (CDP Performance.getMetrics, TaskDuration).

    flood_run.py --url https://192.168.1.66:48443/ --out run.jsonl
                 [--pin-file pin.txt] [--mode fec|video] [--display-index 1]
                 [--phases "rtt=2,loss=0;rtt=30,loss=1;rtt=30,loss=1,burst=4"]
                 [--secs 20] [--settle 6] [--bitrate 20000] [--no-shape] [--cdp 9222]

One JSON line per phase: delivered kbps and messages/s (median of the
seconds after --settle), the counter's loss %, the extra delay over the
session's shortest (p50/p95), the share of the main thread Chrome spent busy,
and the overlay. netem is always taken off at the end.
"""
import argparse
import json
import os
import statistics
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "acceptance"))
sys.path.insert(0, HERE)
import drive  # noqa: E402
import netem  # noqa: E402

SETTINGS = {
    "video_codec": "hevc",
    "stream_resolution": "fixed",
    "stream_height": 1080,
    "stream_fps": 60,
    "stream_bitrate_auto": False,
    "hdr_enabled": False,
    "chroma_444_enabled": False,
    "video_enhancement": "off",
    "gaming_mode": False,
    "mute_host_audio": True,
    "show_performance_stats": True,
    "seamless_switching": True,
}


def parse_phases(text):
    phases = []
    for item in text.split(";"):
        item = item.strip()
        if not item:
            continue
        p = {"rtt": 0.0, "loss": 0.0, "burst": 1.0, "rate": 0}
        for kv in item.split(","):
            k, v = kv.split("=")
            p[k.strip()] = float(v) if k.strip() != "rate" else int(v)
        phases.append(p)
    return phases


def task_seconds(d):
    out = d.call("Performance.getMetrics")
    metrics = out.get("result", out).get("metrics", []) if isinstance(out, dict) else []
    for m in metrics:
        if m.get("name") == "TaskDuration":
            return float(m.get("value", 0))
    return None


def median(values):
    return statistics.median(values) if values else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--pin-file", default="")
    ap.add_argument("--mode", default="fec", choices=["fec", "video"])
    ap.add_argument("--display-index", type=int, default=0)
    ap.add_argument("--phases", default="rtt=2,loss=0;rtt=30,loss=0;rtt=30,loss=1")
    ap.add_argument("--secs", type=float, default=20)
    ap.add_argument("--settle", type=float, default=6)
    ap.add_argument("--bitrate", type=int, default=20000)
    ap.add_argument("--no-shape", action="store_true")
    ap.add_argument("--cdp", type=int, default=9222)
    # netem's limit counts the packets in its delay line too: at 14 000
    # packets/s and 40 ms one way, 200 would drop on its own. A pure delay line
    # here, never a bottleneck's buffer.
    ap.add_argument("--limit", type=int, default=20000)
    ap.add_argument("--label", default="")
    a = ap.parse_args()

    pin = ""
    if a.pin_file and os.path.exists(a.pin_file):
        with open(a.pin_file) as f:
            pin = f.read().strip()
        os.remove(a.pin_file)
    phases = parse_phases(a.phases)

    d = drive.Driver(a.cdp)
    d.navigate(a.url)
    d.eval("localStorage.setItem('mw_flood', %s)" % json.dumps(a.mode))
    settings = dict(SETTINGS, stream_bitrate=a.bitrate)
    d.apply_settings(settings)
    if not d.wait_library("um790pro-loss", pin):
        raise SystemExit("no host card after the PIN")
    card, app = d.pick_tile("display", index=a.display_index)
    print("launching %s / %s (%s)" % (card.get("name"), app["name"], a.mode), flush=True)
    d.launch(card, app)
    d.wait_picture(60)
    d.call("Performance.enable")
    time.sleep(4)
    if "null" in d.eval("JSON.stringify(globalThis.__mwFlood ? 1 : null)"):
        time.sleep(4)
    shaped = not a.no_shape
    if shaped:
        netem.setup(limit=a.limit)
    try:
        with open(a.out, "w") as f:
            f.write(json.dumps({"start": time.strftime("%Y-%m-%d %H:%M:%S"), "label": a.label,
                                "mode": a.mode, "settings": settings, "shaped": shaped}) + "\n")
            for p in phases:
                if shaped:
                    ok, msg = netem.shape(p["rtt"], p["loss"], p["burst"], p["rate"],
                                          limit=a.limit)
                else:
                    ok, msg = True, "not shaped"
                before = d.json_eval("JSON.stringify((globalThis.__mwFlood||{}).history||[])")
                n0 = len(before)
                t_task0, t0 = task_seconds(d), time.time()
                time.sleep(a.secs)
                t_task1, t1 = task_seconds(d), time.time()
                hist = d.json_eval("JSON.stringify((globalThis.__mwFlood||{}).history||[])")
                # history keeps the last 600 seconds; the phase's are the tail.
                got = hist[n0:] if len(hist) > n0 else hist[-int(a.secs):]
                keep = got[int(a.settle):] or got
                busy = None
                if t_task0 is not None and t_task1 is not None:
                    busy = round(100 * (t_task1 - t_task0) / (t1 - t0), 1)
                row = {
                    "phase": p, "ok": ok, "msg": msg if not ok else "",
                    "kbps": median([s["kbps"] for s in keep]),
                    "msgsPerSec": median([s["msgsPerSec"] for s in keep]),
                    "lossPct": median([s["lossPct"] for s in keep]),
                    "delayP50": median([s["extraDelayMs"]["p50"] for s in keep]),
                    "delayP95": median([s["extraDelayMs"]["p95"] for s in keep]),
                    "handlerMsPerSec": median([s["handlerMs"] for s in keep]),
                    "mainThreadBusyPct": busy,
                    "seconds": got,
                    "overlay": d.stats().get("rows", {}),
                }
                if shaped:
                    row["qdisc"] = netem.qdisc_stats()
                f.write(json.dumps(row) + "\n")
                f.flush()
                print("rtt %4g loss %4g burst %3g -> %s kbps, %s msg/s, loss %s %%, "
                      "+delay p50 %s / p95 %s ms, main thread %s %%"
                      % (p["rtt"], p["loss"], p["burst"], row["kbps"], row["msgsPerSec"],
                         row["lossPct"], row["delayP50"], row["delayP95"], busy), flush=True)
    finally:
        if shaped:
            netem.teardown()
        d.stop()
    print("done, netem off", flush=True)


if __name__ == "__main__":
    main()
