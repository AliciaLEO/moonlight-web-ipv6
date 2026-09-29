"""The content-age bench: how old what the client shows is, on the host's clock.

    python age.py calibrate [--port 9334]
        put the host's steady clock into scroll.html?band=time, which is open in
        a kiosk Chrome on the captured screen with that debugging port
        (kiosk.ps1 -DebugPort 9334). Run it on the HOST.
    python age.py run [--client localhost:9333] [--secs 30] [--every 1] --tag <name>
        mwContentAge.start() in the client's page, wait, stop(), and save the
        summary to bench-out/content-age/<name>.json
    python age.py summary <file.json>...
        one line per run

Plan framerate-hote H1, design §33.3; the client side is
frontend/js/stream/ContentAgeProbe.js.

── The clocks ────────────────────────────────────────────────────────────────

The age is the client's draw, put on the host's steady clock, minus the time
the page coded into its band. Three clocks meet:

- the backend's std::chrono::steady_clock, which the pong carries: on Windows,
  QueryPerformanceCounter since boot;
- this script's time.perf_counter_ns(), the same counter read by CPython, on the
  same machine;
- the page's performance.now(), the same counter again from another origin.

calibrate() reads the page's clock between two reads of its own, keeps the
exchange with the shortest round trip, and hands the page the offset between
the two. What is left is half that round trip, a fraction of a millisecond.

A client on another machine reaches its own debugging port only on localhost:
open an SSH tunnel to it first (ssh -L 9333:127.0.0.1:9333 <machine>).

Needs `pip install websocket-client`.
"""
import argparse
import json
import os
import sys
import time
import urllib.request

import websocket  # websocket-client

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(HERE))), "bench-out",
                   "content-age")


class Page:
    """One page's DevTools socket, picked by a piece of its URL."""

    def __init__(self, where, needle):
        host, _, port = where.rpartition(":")
        host = host or "localhost"
        with urllib.request.urlopen(f"http://{host}:{port}/json", timeout=10) as r:
            tabs = json.load(r)
        pages = [t for t in tabs if t["type"] == "page" and needle in t["url"]]
        if not pages:
            raise SystemExit(f"no page with {needle!r} in its URL on {host}:{port}: "
                             + ", ".join(t["url"] for t in tabs if t["type"] == "page"))
        self.ws = websocket.create_connection(pages[0]["webSocketDebuggerUrl"],
                                              suppress_origin=True, timeout=10)
        self.n = 0

    def eval(self, js, timeout=30):
        self.n += 1
        self.ws.send(json.dumps({"id": self.n, "method": "Runtime.evaluate",
                                 "params": {"expression": js, "returnByValue": True,
                                            "awaitPromise": True}}))
        deadline = time.time() + timeout
        while True:
            self.ws.settimeout(max(0.1, deadline - time.time()))
            msg = json.loads(self.ws.recv())
            if msg.get("id") != self.n:
                continue
            if "error" in msg:
                raise RuntimeError(msg["error"])
            res = msg.get("result", {})
            if "exceptionDetails" in res:
                raise RuntimeError(res["exceptionDetails"].get("text", "exception"))
            return res.get("result", {}).get("value")


def calibrate(args):
    page = Page(f"localhost:{args.port}", "scroll.html")
    if not page.eval("typeof window.mwBand === 'object'"):
        raise SystemExit("the page has no mwBand — open scroll.html?band=time")
    best = None
    for _ in range(args.tries):
        t0 = time.perf_counter_ns()
        now = page.eval("performance.now()")
        t1 = time.perf_counter_ns()
        rtt = (t1 - t0) / 1e6
        offset = (t0 + t1) / 2 / 1e6 - now
        if best is None or rtt < best[0]:
            best = (rtt, offset)
    print(page.eval("mwBand.calibrate(%r)" % best[1]))
    print("round trip %.3f ms: the offset is good to %.3f ms" % (best[0], best[0] / 2))


def run(args):
    page = Page(args.client, args.needle)
    print(page.eval("mwContentAge ? mwContentAge.start({every: %d}) : 'no mwContentAge'"
                    % args.every))
    time.sleep(args.secs)
    check = clock_check(page) if args.client.split(":")[0] in ("localhost", "127.0.0.1") else None
    summary = page.eval("JSON.stringify(mwContentAge.stop())")
    if not summary or summary == "null":
        raise SystemExit("the probe returned nothing — was it running?")
    data = json.loads(summary)
    data["tag"] = args.tag
    data["client"] = args.client
    data["clockErrorMs"] = check
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, args.tag + ".json")
    with open(path, "w") as f:
        json.dump(data, f)
    print(line(data))
    print("saved", path)


def clock_check(page):
    """A client on this machine reads the same counter as the host: its exact
    offset is measurable here, and the probe's estimate — made only from the
    ping/pong, as it must be across two machines — is checked against it.
    Positive: the estimate runs ahead of the host's clock."""
    best = None
    for _ in range(20):
        t0 = time.perf_counter_ns()
        est = page.eval("(() => { const n = performance.now(); "
                        "return [n, mwContentAge.hostUs(n)]; })()")
        t1 = time.perf_counter_ns()
        if best is None or t1 - t0 < best[0]:
            best = (t1 - t0, (t0 + t1) / 2 / 1000, est)
    _rtt, host_us, (_now, est_us) = best
    err = (est_us - host_us) / 1000
    print("clock check: the estimate is %+.3f ms off the host's clock (exchange %.3f ms)"
          % (err, best[0] / 1e6))
    return round(err, 3)


def line(d):
    c = d.get("clock") or {}
    cap = d.get("capture") or {}
    pre = d.get("beforeCapture") or {}
    return ("%-28s ages %5d  content median %6s p90 %6s p99 %6s  capture %6s  before %6s ms  "
            "invalid %s  rtt %.2f ms  drift %.0f ppm%s" % (
                d.get("tag", "?"), d.get("ages", 0), d.get("medianMs"), d.get("p90Ms"),
                d.get("p99Ms"), cap.get("medianMs"), pre.get("medianMs"),
                ",".join("%s=%s" % kv for kv in (d.get("invalid") or {}).items() if kv[1]) or "0",
                c.get("rttMinMs") or 0, c.get("driftPpm") or 0,
                "" if d.get("clockErrorMs") is None else "  clock %+.2f ms" % d["clockErrorMs"]))


def summary(args):
    for p in args.files:
        with open(p) as f:
            print(line(json.load(f)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("calibrate")
    c.add_argument("--port", type=int, default=9334)
    c.add_argument("--tries", type=int, default=40)
    r = sub.add_parser("run")
    r.add_argument("--client", default="localhost:9333")
    r.add_argument("--needle", default="", help="a piece of the client page's URL")
    r.add_argument("--secs", type=float, default=30)
    r.add_argument("--every", type=int, default=1)
    r.add_argument("--tag", required=True)
    s = sub.add_parser("summary")
    s.add_argument("files", nargs="+")
    args = ap.parse_args()
    {"calibrate": calibrate, "run": run, "summary": summary}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
