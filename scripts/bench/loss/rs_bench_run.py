#!/usr/bin/env python3
"""Run rs-bench.html in a Chrome driven over DevTools and print its results.

    rs_bench_run.py --port 9222 --url file:///C:/Users/max/mw-loss/rs-bench.html [--runs 200]

The page must be reachable from that browser (a file it has, or any URL).
Prints the JSON of globalThis.__rsBench once the page is done.
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import cdp  # noqa: E402  — scripts/bench/cdp.py


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9222)
    ap.add_argument("--url", required=True)
    ap.add_argument("--runs", type=int, default=200)
    ap.add_argument("--timeout", type=float, default=300)
    a = ap.parse_args()
    c = cdp.Cdp(a.port)
    sep = "&" if "?" in a.url else "?"
    c.call("Page.navigate", url="%s%sauto=1&runs=%d" % (a.url, sep, a.runs))
    end = time.time() + a.timeout
    while time.time() < end:
        time.sleep(2)
        try:
            out = c.eval("JSON.stringify(globalThis.__rsBench || null)")
        except (Exception, SystemExit):
            continue
        if out and out != "null" and out.strip('"') != "null":
            data = json.loads(out) if isinstance(out, str) else out
            print(json.dumps(data, indent=1))
            return
    raise SystemExit("rs-bench did not finish in %ds" % a.timeout)


if __name__ == "__main__":
    main()
