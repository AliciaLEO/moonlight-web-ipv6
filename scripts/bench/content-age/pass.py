"""One content-age pass on this machine: a self-stream, the bench page's time
band on the captured screen, and the client's probe reading it back.

    python pass.py --tag d1-auto --secs 30
    python pass.py --tag d1-120 --fps 120 --vsync on --every 2

Driven like ../cadence/cadence.py — the acceptance run's environment
(MW_BENCH_LOCAL_PORTS for a --dev instance; MW_BENCH_CLIENT_POS /
MW_BENCH_CLIENT_LUID to put the client on another screen, driven by another GPU
than the encoder's). The host's own keys (MW_NATIVE_TUNING=cadence=…,
MW_VDD_REFRESH) are the instance's environment, set when it was launched.

A client on the same machine shares the host's clock and its compositor: this
checks the instrument and the plumbing. The measurements that count come from
a client on another machine: --client-port names the debugging port of a
Chrome already running there (through an SSH tunnel), --client-url the address
it reaches this host at (plan framerate-hote §4).
"""
import argparse
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.dirname(HERE)
sys.path.insert(0, BENCH)
sys.path.insert(0, os.path.join(BENCH, "acceptance"))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(BENCH, "cadence"))
import run, drive, fleet  # noqa: E402
import age  # noqa: E402
from cadence import monitors  # noqa: E402

CONTENT_PORT = 9334


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fps", type=int, default=0, help="stream_fps; 0 = Auto")
    ap.add_argument("--vsync", choices=["on", "off"], default="off",
                    help="on = tearing off: the client paints on its refresh")
    ap.add_argument("--secs", type=float, default=30)
    ap.add_argument("--every", type=int, default=1)
    ap.add_argument("--settle", type=float, default=6)
    ap.add_argument("--tag", required=True)
    ap.add_argument("--target", default="display", help="display | vdisplay")
    ap.add_argument("--display-index", default="0")
    ap.add_argument("--px", type=int, default=600, help="the page's scroll speed")
    ap.add_argument("--client-port", type=int, default=0,
                    help="a client Chrome on another machine, its debugging port tunnelled here")
    ap.add_argument("--client-url", default="", help="the address that client reaches this host at")
    a = ap.parse_args()
    remote = a.client_port > 0

    access = dict(run.access_map().get("local") or {})
    probe = fleet.probe("local")
    pin = (probe.get("pin") or {}).get("pin")
    if pin:
        access["pin"] = pin
    access["lan"] = fleet.lan_url("local", probe) or access.get("lan")
    if remote:
        access["lan"] = a.client_url or access["lan"]
        d = drive.Driver(a.client_port)
    else:
        run.kiosk_start(access["lan"])
        d = drive.Driver(run.DEBUG_PORT)
    shown = False
    try:
        d.navigate(access["lan"])
        # The --dev instance serves frontend/ from the sources, but the service
        # worker keeps the last version it cached: an edited probe would not
        # run (memory frontend-sw-cache-stale-tests).
        d.eval("(async () => { await caches.delete('mw-shell'); for (const r of await "
               "navigator.serviceWorker.getRegistrations()) await r.unregister(); return 1; })()")
        d.navigate(access["lan"])
        d.wait_library(access.get("name", "bench"), access.get("pin", ""), tries=25)
        settings = dict(run.load_matrix()["base"])
        settings.update({"stream_fps": a.fps, "tearing_default_v2": True,
                         "tearing_enabled": a.vsync == "off"})
        d.apply_settings(settings)
        d.wait_library(access.get("name", "bench"), access.get("pin", ""), tries=25)
        os.environ["MW_BENCH_DISPLAY"] = a.display_index
        card, app = d.pick_tile(a.target)
        print("tile", card.get("name"), "/", app.get("name"), flush=True)
        before = {m[0] for m in monitors()}
        d.launch(card, app)
        d.wait_picture(timeout=60)
        # The Virtual Display only exists once the stream is up: it is the
        # screen that was not there before the launch.
        if a.target == "vdisplay":
            time.sleep(2)
            vdd = [m for m in monitors() if m[0] not in before]
            if not vdd:
                raise SystemExit("no virtual display appeared: %s" % monitors())
            x, y = vdd[0][1].split(",")
            w, h = vdd[0][2].split("x")
            os.environ["MW_BENCH_CONTENT_RECT"] = "%s,%s,%s,%s" % (x, y, w, h)
            print("virtual display", " ".join(vdd[0]), flush=True)
        if d.eval("typeof (window.mwContentAge && window.mwContentAge.onDecoded)") != "function":
            raise SystemExit("the page runs an older content-age probe")
        run.content_start("scroll.html?band=time&px=%d" % a.px, probe=False,
                          debug_port=CONTENT_PORT)
        shown = True
        time.sleep(4)
        age.calibrate(argparse.Namespace(port=CONTENT_PORT, tries=40))
        time.sleep(a.settle)
        age.run(argparse.Namespace(client="localhost:%d" % (a.client_port or run.DEBUG_PORT),
                                   needle="", secs=a.secs, every=a.every, tag=a.tag,
                                   local=not remote))
        d.expand_latency_detail()
        stats = d.stats()
        path = os.path.join(age.OUT, a.tag + ".json")
        with open(path) as f:
            data = json.load(f)
        data["overlay"] = stats
        data["args"] = vars(a)
        data["env"] = {k: os.environ.get(k, "") for k in ("MW_NATIVE_TUNING", "MW_VDD_REFRESH")}
        with open(path, "w") as f:
            json.dump(data, f)
        rows = (stats or {}).get("rows") or {}
        for k in rows:
            if any(n in k for n in ("Latency", "Framerate", "Resolution", "Codec", "Decode",
                                    "Render", "Host")):
                print("  %-28s %s" % (k, rows[k]))
    finally:
        try:
            d.stop()
        except Exception:
            pass
        if shown:
            run.content_stop()
        if not remote:
            run.kiosk_stop()


if __name__ == "__main__":
    main()
