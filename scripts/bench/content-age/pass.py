"""One content-age pass on this machine: a self-stream, the bench page's time
band on the captured screen, and the client's probe reading it back.

    python pass.py --tag d1-auto --secs 30
    python pass.py --tag d1-120 --fps 120 --vsync on --every 2
    python pass.py --tag d1-detect --autostep --settle 14    # "Auto" with detection

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


def _ms(v):
    return "-" if v is None else "%.2f" % v


def click_flag(d, n, every_ms=800):
    """@p n click → flag samples (frontend LatencyProbe.js, the host's
    LatencyFlag): the whole loop the player feels, input to picture. Started
    without waiting on it — a minute is longer than a DevTools call should
    hang — and read back as it fills. None when the stream has no flag."""
    if d.eval("typeof (window.mwLatency && window.mwLatency.run)") != "function":
        print("  clicks: no click-to-photon probe on this stream (latency_flag_enabled?)",
              flush=True)
        return None
    before = d.eval("(window.mwLatencyResults || []).length") or 0
    d.eval("window.mwLatency.run(%d, %d); 1" % (n, every_ms))
    end = time.time() + n * (every_ms + 300) / 1000 + 30
    while time.time() < end:
        if (d.eval("(window.mwLatencyResults || []).length") or 0) - before >= n:
            break
        time.sleep(2)
    samples = d.json_eval("JSON.stringify((window.mwLatencyResults || []).slice(%d))" % before)
    ok = sorted(s["latencyMs"] for s in samples
                if s.get("ok") and s.get("latencyMs") is not None)
    pick = lambda q: ok[min(len(ok) - 1, int(q * len(ok)))] if ok else None  # noqa: E731
    summary = {"n": len(samples), "ok": len(ok), "medianMs": pick(0.5), "p90Ms": pick(0.9),
               "minMs": ok[0] if ok else None, "maxMs": ok[-1] if ok else None}
    # ASCII only: under local_matrix.py this goes through a cp1252 pipe.
    print("  clicks: %d of %d measured, click -> flag median %s ms (p90 %s, %s to %s)" % (
        summary["ok"], summary["n"], _ms(summary["medianMs"]), _ms(summary["p90Ms"]),
        _ms(summary["minMs"]), _ms(summary["maxMs"])), flush=True)
    # Each click split by the host's answer to its stamp (InputUplink.js, plan
    # radios T7): the way up, the injection, and the rest (flag, capture,
    # encode, the way down, decode, draw).
    for key in ("upMs", "hostInMs", "restMs"):
        vals = sorted(s[key] for s in samples
                      if s.get("ok") and isinstance(s.get(key), (int, float)))
        summary[key] = {"n": len(vals),
                        "median": vals[len(vals) // 2] if vals else None,
                        "p90": vals[min(len(vals) - 1, int(0.9 * len(vals)))] if vals else None,
                        "max": vals[-1] if vals else None}
    if summary["upMs"]["n"]:
        print("  split: up median %s ms (p90 %s, max %s), host %s ms, rest median %s ms (p90 %s)" % (
            _ms(summary["upMs"]["median"]), _ms(summary["upMs"]["p90"]), _ms(summary["upMs"]["max"]),
            _ms(summary["hostInMs"]["median"]), _ms(summary["restMs"]["median"]),
            _ms(summary["restMs"]["p90"])), flush=True)
    return {"summary": summary, "samples": samples}


def uplink_runs(d, spec, tag):
    """The way up alone (frontend InputUplink.js, plan radios T7): for each
    HZ:SECS of @p spec, that many dated messages a second that do nothing on the
    host. None when the stream has no such bench."""
    if not spec:
        return None
    if d.eval("typeof (window.mwUplink && window.mwUplink.run)") != "function":
        print("  uplink: this client has no uplink bench (older page?)", flush=True)
        return None
    runs = []
    for part in spec.split(","):
        hz, secs = (int(x) for x in part.split(":"))
        before = d.eval("(window.mwUplinkResults || []).length") or 0
        d.eval("window.mwUplink.run({hz: %d, secs: %d, label: %s}); 1" % (hz, secs, json.dumps(tag)))
        end = time.time() + secs + 15
        while time.time() < end:
            if (d.eval("(window.mwUplinkResults || []).length") or 0) > before:
                break
            time.sleep(1)
        got = d.json_eval("JSON.stringify((window.mwUplinkResults || []).slice(%d))" % before)
        if not got:
            print("  uplink %d/s: no result" % hz, flush=True)
            continue
        r = got[0]
        up, rtt = r["up"], r["rtt"]
        print("  uplink %d/s for %d s: %d/%d answered, up median %s ms (p90 %s, p99 %s, max %s), "
              "rtt median %s, %d sends queued" % (
                  hz, secs, r["answered"], r["sent"], _ms(up["median"]), _ms(up["p90"]),
                  _ms(up["p99"]), _ms(up["max"]), _ms(rtt["median"]), r["queuedSends"]), flush=True)
        runs.append(r)
    return runs


def stepper_state(d, content_ms):
    """"Auto" with detection (frontend CadenceStepper.js, design §33.10): its
    state and its decisions, each timed from the moment the content began to
    move (@p content_ms, the client's clock) — the gate wants the final step
    within ten seconds. None when it did not run on this pass."""
    st = d.json_eval("JSON.stringify(window.mwCadenceStepper ? {summary: "
                     "window.mwCadenceStepper.summary, events: window.mwCadenceStepper.events}"
                     " : null)")
    if not st:
        return None
    for e in st.get("events") or []:
        e["sinceContentS"] = (round((e["at"] - content_ms) / 1000, 2)
                              if isinstance(content_ms, (int, float)) else None)
    after = [e for e in st["events"] if (e.get("sinceContentS") or 0) >= 0]
    decided = [e for e in after if e["what"] in ("kept", "rejected", "refused", "fallback")]
    kept = [e for e in after if e["what"] == "kept"]
    s = st["summary"]
    st["firstDecisionS"] = decided[0]["sinceContentS"] if decided else None
    st["keptAtS"] = kept[-1]["sinceContentS"] if kept and s.get("stepFps") else None
    print("  steps: %s fps on a ladder %s, %d trials (%d kept, %d given up, %d refused, "
          "%d trips); first decision %s s, last kept %s s after the content moved" % (
              s.get("stepFps") or s.get("base"), s.get("levels"), s.get("trials", 0),
              s.get("kept", 0), s.get("rejected", 0), s.get("refused", 0), s.get("trips", 0),
              _ms(st["firstDecisionS"]), _ms(st["keptAtS"])), flush=True)
    for e in decided:
        facts = ", ".join("%s=%s" % (k, round(v, 2) if isinstance(v, float) else v)
                          for k, v in e.items() if k not in ("at", "what", "sinceContentS"))
        # ASCII only: a reason may carry "→", and under local_matrix.py this
        # goes through a cp1252 pipe.
        line = "    %6.2f s  %-9s %s" % (e["sinceContentS"] or 0, e["what"], facts)
        print(line.replace("→", "->").encode("ascii", "replace").decode(), flush=True)
    return st


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
    ap.add_argument("--game-fps", default="",
                    help="the page's rate, like a game's: 50, or 49-53 drawn at random")
    ap.add_argument("--client-port", type=int, default=0,
                    help="a client Chrome on another machine, its debugging port tunnelled here")
    ap.add_argument("--client-url", default="", help="the address that client reaches this host at")
    ap.add_argument("--bitrate", type=int, default=0,
                    help="kbps; 0 = the automatic one, sized for the client's rate")
    ap.add_argument("--hold", type=int, default=0,
                    help="seconds of stream held on the virtual display with no bench page "
                         "over it, for a game driven apart; no content-age reading")
    ap.add_argument("--uplink", default="",
                    help="HZ:SECS[,HZ:SECS] dated input messages, the way up alone (plan radios T7)")
    ap.add_argument("--clicks", type=int, default=0,
                    help="click → flag samples after the content-age window (needs "
                         "latency_flag_enabled in the instance's settings.json)")
    ap.add_argument("--local-storage", action="append", default=[], metavar="KEY=VALUE",
                    help="a bench switch the page reads at launch (mw_decodequeue=pending)")
    ap.add_argument("--autostep", action="store_true",
                    help="\"Auto\" with detection on (localStorage mw_autostep=1; design "
                         "§33.10): the stream may step above the client's rate. Off otherwise "
                         "(mw_autostep=0): on is the product's default, and a pass without this "
                         "flag stays the Auto from before it, the bench's reference")
    a = ap.parse_args()
    remote = a.client_port > 0
    # Another session's Chrome may already hold the client kiosk's debugging
    # port (9333 on 01/10/2026): the kiosk would not get it, and this pass would
    # drive that other browser. MW_BENCH_DEBUG_PORT moves the kiosk's.
    if os.environ.get("MW_BENCH_DEBUG_PORT"):
        run.DEBUG_PORT = int(os.environ["MW_BENCH_DEBUG_PORT"])

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
        # The bench profile keeps its localStorage from one pass to the next:
        # a switch not asked for this time is taken away.
        d.eval("localStorage.removeItem('mw_decodequeue')")
        # The detection is on by default (UA.4): off is said, for the
        # reference modes (client, host-guarded) to stay today's Auto.
        d.eval("localStorage.setItem('mw_autostep', %s)" % json.dumps("1" if a.autostep else "0"))
        # Each pass is this device's first stream: the steps it failed at in
        # the passes before are forgotten (mw_autostep_failed;
        # --local-storage puts them back for a pass that wants them).
        d.eval("localStorage.removeItem('mw_autostep_failed')")
        for kv in a.local_storage:
            k, _, v = kv.partition("=")
            d.eval("localStorage.setItem(%s, %s)" % (json.dumps(k), json.dumps(v)))
        settings = dict(run.load_matrix()["base"])
        settings.update({"stream_fps": a.fps, "tearing_default_v2": True,
                         "tearing_enabled": a.vsync == "off"})
        if a.bitrate > 0:
            settings.update({"stream_bitrate_auto": False, "stream_bitrate": a.bitrate})
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
        if a.hold > 0:
            # A game on the virtual display instead of the bench page (RE9,
            # driven by a script of its own): the stream held, nothing drawn
            # over it, the overlay read at the end.
            time.sleep(a.hold)
            # A still screen: the way up with almost no video coming down.
            uplink = uplink_runs(d, a.uplink, a.tag)
            clicks = click_flag(d, a.clicks) if a.clicks > 0 else None
            stats = d.stats()
            with open(os.path.join(age.OUT, a.tag + ".json"), "w") as f:
                json.dump({"tag": a.tag, "overlay": stats, "args": vars(a),
                           "uplink": uplink, "clicks": clicks,
                           "env": {k: os.environ.get(k, "")
                                   for k in ("MW_NATIVE_TUNING", "MW_VDD_REFRESH")}}, f)
            print("  held %d s; %s" % (a.hold, ((stats or {}).get("rows") or {}).get(
                "Framerate:", "")), flush=True)
            return
        if d.eval("typeof (window.mwContentAge && window.mwContentAge.onDecoded)") != "function":
            raise SystemExit("the page runs an older content-age probe")
        run.content_start("scroll.html?band=time&px=%d%s" % (
                              a.px, "&fps=" + a.game_fps if a.game_fps else ""), probe=False,
                          debug_port=CONTENT_PORT)
        shown = True
        # When the content began to move, on the client's clock: what the
        # detection's decisions are timed from (it tries nothing on a still
        # desktop).
        content_ms = d.eval("performance.now()")
        time.sleep(4)
        age.calibrate(argparse.Namespace(port=CONTENT_PORT, tries=40))
        time.sleep(a.settle)
        age.run(argparse.Namespace(client="localhost:%d" % (a.client_port or run.DEBUG_PORT),
                                   needle="", secs=a.secs, every=a.every, tag=a.tag,
                                   local=not remote))
        # cadence=deadline: the client's side of the grid — whether the host
        # followed it, the lead it asked for, and how many frames came late.
        grid = d.eval("window.mwVsyncGrid && window.mwVsyncGrid.running ? "
                      "window.mwVsyncGrid.summary : null")
        # "Auto" with detection: where it stands and what it decided, read
        # before the clicks (they move nothing on the screen's content).
        stepper = stepper_state(d, content_ms)
        clicks = click_flag(d, a.clicks) if a.clicks > 0 else None
        uplink = uplink_runs(d, a.uplink, a.tag)
        d.expand_latency_detail()
        stats = d.stats()
        path = os.path.join(age.OUT, a.tag + ".json")
        with open(path) as f:
            data = json.load(f)
        data["overlay"] = stats
        data["grid"] = grid
        data["stepper"] = stepper
        data["clicks"] = clicks
        data["uplink"] = uplink
        if grid:
            print("  grid: followed %s, lead %s ms, margin %s ms, %s misses in %s frames, "
                  "slack median %s ms (p5 %s)" % (
                      grid.get("followed"), _ms(grid.get("leadMs")), _ms(grid.get("marginMs")),
                      grid.get("misses"), grid.get("frames"), _ms(grid.get("slackMedianMs")),
                      _ms(grid.get("slackP5Ms"))), flush=True)
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
