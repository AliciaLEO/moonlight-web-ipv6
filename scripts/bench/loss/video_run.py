#!/usr/bin/env python3
"""Today's video channel under losses, as the viewer sees it (plan Idées Punktfunk, A0.2).

Runs on the CLIENT (Linux, netem.py next door) against a Chrome driven over
DevTools. The host's display must show the scrolling page with its frame-number
bands (`scripts/bench/content/scroll.html?band=1`, 2560x1440 host, 1080p stream);
the host's native tuning picks the recovery being measured (its default, or
`namedrops=0`, or `dpb=1` for keyframes only). Each phase shapes the link with
netem (round trip, losses at random or in bursts) and counts, from the stream's
own canvas:

  - pictures shown (a new band value), damaged ones (a band broken, or the two
    ends of the picture from different frames: predicted from a reference the
    client never had), and gaps over 100 ms between new pictures (freezes);
  - the overlay (frame rate, bitrate, latency).

    video_run.py --url https://192.168.1.66:48443/ --out run.jsonl [--display-index 1]
                 [--phases "rtt=30,loss=0;rtt=30,loss=1;rtt=30,loss=1,burst=4"]
                 [--secs 25] [--bitrate 20000] [--label x] [--cdp 9222]

One JSON line per phase with the counters' differences over it. netem is always
taken off at the end.
"""
import argparse
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "acceptance"))
sys.path.insert(0, HERE)
import drive  # noqa: E402
import netem  # noqa: E402
from flood_run import SETTINGS, parse_phases  # noqa: E402

# The host displays are 2560x1440 and the stream 1920x1080: a band of 192x16
# host pixels is 144x12 in the stream, one bit a 6x6 block. The green channel
# stands for the luma; 128 splits black from white (bench §8n.27).
BAND_JS = r"""
(() => {
  if (window.__mwBand) return 'already';
  const S = window.__mwBand = { ticks: 0, frames: 0, damaged: 0, invalid: 0, mismatch: 0,
                                gaps100: 0, frozenMs: 0, maxGapMs: 0, err: null };
  const SCALE = 1920 / 2560, BW = 8 * SCALE;
  const W = Math.ceil(24 * BW), H = Math.ceil(16 * SCALE);
  const scratch = document.createElement('canvas');
  scratch.width = W;
  scratch.height = 2 * H;
  const g = scratch.getContext('2d', { willReadFrequently: true });
  g.imageSmoothingEnabled = false;
  let prevKey = null, lastNew = performance.now();
  const decode = (d, row) => {
    let v = 0, ok = true;
    for (let b = 0; b < 24; b++) {
      const x = Math.floor(b * BW + BW / 2);
      const y1 = row + Math.floor(BW / 2), y2 = row + Math.floor(BW + BW / 2);
      const one = d[(y1 * W + x) * 4 + 1] > 128, comp = d[(y2 * W + x) * 4 + 1] > 128;
      if (one === comp) ok = false;
      if (one) v |= 1 << b;
    }
    return [v >>> 0, ok];
  };
  const tick = (now) => {
    try {
      const c = document.querySelector('canvas');
      if (c && c.width === 1920 && c.height === 1080) {
        S.ticks++;
        g.drawImage(c, 0, 0, W, H, 0, 0, W, H);
        g.drawImage(c, 0, c.height - H, W, H, 0, H, W, H);
        const d = g.getImageData(0, 0, W, 2 * H).data;
        const [top, okT] = decode(d, 0);
        const [bot, okB] = decode(d, H);
        const key = top + ':' + bot + ':' + okT + ':' + okB;
        if (key !== prevKey) {
          prevKey = key;
          S.frames++;
          const gap = now - lastNew;
          lastNew = now;
          if (gap > 100) { S.gaps100++; S.frozenMs += gap; }
          if (gap > S.maxGapMs) S.maxGapMs = gap;
          if (!okT || !okB) { S.invalid++; S.damaged++; }
          else if (top !== bot) { S.mismatch++; S.damaged++; }
        }
      }
    } catch (e) { S.err = String(e).slice(0, 200); }
    requestAnimationFrame(tick);
  };
  requestAnimationFrame(tick);
  return 'installed';
})()
"""
BAND_READ = "JSON.stringify(window.__mwBand || null)"
COUNTERS = ("ticks", "frames", "damaged", "invalid", "mismatch", "gaps100", "frozenMs")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--pin-file", default="")
    ap.add_argument("--display-index", type=int, default=0)
    ap.add_argument("--phases", default="rtt=30,loss=0;rtt=30,loss=1;rtt=30,loss=1,burst=4")
    ap.add_argument("--secs", type=float, default=25)
    ap.add_argument("--bitrate", type=int, default=20000)
    ap.add_argument("--cdp", type=int, default=9222)
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
    d.eval("localStorage.removeItem('mw_flood')")
    settings = dict(SETTINGS, stream_bitrate=a.bitrate)
    d.apply_settings(settings)
    if not d.wait_library("um790pro-loss", pin):
        raise SystemExit("no host card after the PIN")
    card, app = d.pick_tile("display", index=a.display_index)
    print("launching %s / %s" % (card.get("name"), app["name"]), flush=True)
    d.launch(card, app)
    d.wait_picture(60)
    time.sleep(4)
    print("band sampler: " + str(d.eval(BAND_JS)), flush=True)
    netem.setup(limit=a.limit)
    try:
        with open(a.out, "w") as f:
            f.write(json.dumps({"start": time.strftime("%Y-%m-%d %H:%M:%S"), "label": a.label,
                                "settings": settings}) + "\n")
            for p in phases:
                ok, msg = netem.shape(p["rtt"], p["loss"], p["burst"], p["rate"], limit=a.limit)
                b0 = d.json_eval(BAND_READ) or {}
                t0 = time.time()
                overlays = []
                while time.time() - t0 < a.secs:
                    tick = time.time()
                    try:
                        overlays.append(d.stats().get("rows", {}))
                    except Exception as e:  # a hiccup of the page, noted
                        overlays.append({"error": str(e)[:120]})
                    time.sleep(max(0.0, 1.0 - (time.time() - tick)))
                b1 = d.json_eval(BAND_READ) or {}
                seconds = time.time() - t0
                delta = {k: (b1.get(k, 0) or 0) - (b0.get(k, 0) or 0) for k in COUNTERS}
                row = {"phase": p, "ok": ok, "msg": msg if not ok else "", "seconds": round(seconds, 1),
                       "shownFps": round(delta["frames"] / seconds, 1),
                       "damaged": delta["damaged"], "freezes": delta["gaps100"],
                       "frozenMs": round(delta["frozenMs"]), "maxGapMs": b1.get("maxGapMs"),
                       "band": delta, "overlay": overlays}
                row["qdisc"] = netem.qdisc_stats()
                f.write(json.dumps(row) + "\n")
                f.flush()
                print("rtt %4g loss %4g burst %3g -> %s fps shown, %d damaged, %d freezes "
                      "(%d ms frozen)" % (p["rtt"], p["loss"], p["burst"], row["shownFps"],
                                          row["damaged"], row["freezes"], row["frozenMs"]),
                      flush=True)
    finally:
        netem.teardown()
        d.stop()
    print("done, netem off", flush=True)


if __name__ == "__main__":
    main()
