"""A matrix of content-age passes on this host: the product's virtual display
at several rates × the cadence keys, each pass on a --dev instance launched with
that pass's keys.

    python local_matrix.py --rates 60,240,500 --cadences client,host,host-ceiling,host-guarded

A client on the host's own machine is for tuning the bench, not for the verdict
(plan framerate-hote §2): it shares the host's compositor, whose clock follows
the primary screen — the virtual display itself, while it streams. With
--client-port / --client-url the client is a Chrome on another machine.

The virtual display's settings file on DualRTX belongs to Bruno's VDD: the
product adds its bench modes to it (2560x1440 at 240, at 500). It is saved
first and put back byte for byte at the end, and the screens are listed before
and after (memory dualrtx-vdd-display-tests).
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
EXE = os.path.join(REPO, "build", "MoonlightWeb.exe")
OUT = os.path.join(REPO, "bench-out", "content-age")
WORKER_LOGS = os.path.join(os.environ["APPDATA"], "MoonlightWeb", "MoonlightWeb-dev", "logs")
VDD_XML = r"C:\VirtualDisplayDriver\vdd_settings.xml"
HOST_LINES = ("[native] cadence", "decode credit", "capture wake-ups", "capture loop",
              "frames arrive at", "MW_NATIVE_TUNING", "[native] deadline")
# "[native] cadence" also catches the detection's lines: the steps applied or
# refused, and "cadence steps: … asked, … applied" at the end.


def monitors():
    return subprocess.run(["powershell", "-NoProfile", "-File",
                           os.path.join(os.path.dirname(HERE), "monitors.ps1")],
                          capture_output=True, text=True).stdout.strip()


def kill_dev():
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    "Get-CimInstance Win32_Process -Filter \"Name='MoonlightWeb.exe'\" | "
                    "Where-Object { $_.CommandLine -like '*--dev*' } | "
                    "ForEach-Object { Stop-Process -Id $_.ProcessId -Force }"],
                   capture_output=True, text=True)
    time.sleep(2)


def launch_dev(rate, cadence, log):
    env = dict(os.environ)
    env.pop("MW_NATIVE_TUNING", None)
    env.pop("MW_VDD_REFRESH", None)
    if rate > 0:  # 0: the rate the product chooses
        env["MW_VDD_REFRESH"] = str(rate)
    # "client" is today's Auto; "detect" the same host, the client's
    # detection on (pass.py --autostep): neither is a host key.
    if cadence not in ("client", "detect"):
        env["MW_NATIVE_TUNING"] = "cadence=" + cadence
    subprocess.Popen([EXE, "--dev", "--log", log], env=env,
                     creationflags=getattr(subprocess, "DETACHED_PROCESS", 0))
    time.sleep(8)


def names(listing):
    return {line.split()[0] for line in listing.splitlines() if line.strip()}


def wait_released(baseline, timeout=150):
    """The virtual display goes 4 s after the host sees the page gone — which
    takes it up to a minute. Wait for it to be gone: no screen that was not
    there before. A screen of the baseline missing afterwards is said, not
    waited for — DualRTX's RTX screen has left the desktop on a switch."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        now = monitors()
        if names(now) <= names(baseline):
            missing = names(baseline) - names(now)
            if missing:
                print("   !! screen(s) gone from the desktop: %s" % " ".join(sorted(missing)),
                      flush=True)
            return True
        time.sleep(5)
    return False


def host_lines(tag, since):
    logs = [p for p in glob.glob(os.path.join(WORKER_LOGS, "moonlightweb-worker-*.log"))
            if os.path.getmtime(p) >= since]
    lines = []
    for p in sorted(logs, key=os.path.getmtime):
        with open(p, encoding="utf-8", errors="replace") as f:
            lines += [l.rstrip() for l in f if any(k in l for k in HOST_LINES)]
    with open(os.path.join(OUT, tag + ".host.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    for l in lines:
        if any(k in l for k in ("cadence:", "cadence step", "decode credit", "deadline:")):
            print("   ", l[l.find("[native]"):][:240], flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rates", default="60,240,500",
                    help="the virtual display's rates (MW_VDD_REFRESH); 0 = the product's own")
    ap.add_argument("--cadences", default="client,host,host-ceiling,host-guarded",
                    help="client = today's Auto; detect = Auto with detection "
                         "(design §33.10); host, host-ceiling, host-guarded, deadline = the "
                         "bench keys")
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--secs", type=float, default=30)
    ap.add_argument("--settle", type=float, default=6,
                    help="seconds between the page's calibration and the window (pass.py "
                         "--settle); 14 lets the detection settle first")
    ap.add_argument("--bitrate", type=int, default=0, help="kbps (pass.py --bitrate)")
    ap.add_argument("--vsync", choices=["on", "off"], default="off",
                    help="on = tearing off: the client paints on its refresh (pass.py --vsync)")
    ap.add_argument("--every", type=int, default=1,
                    help="read one frame in N: the copy costs a weak client its frame rate "
                         "(an N95 fell from 59 to 53 draws a second, 42 to 87 ms of capture age)")
    ap.add_argument("--prefix", default="loc")
    ap.add_argument("--client-port", type=int, default=0,
                    help="a client on another machine (pass.py --client-port)")
    ap.add_argument("--client-url", default="")
    ap.add_argument("--local-storage", action="append", default=[], metavar="KEY=VALUE")
    ap.add_argument("--game-fps", default="", help="the page at a game's rate (pass.py --game-fps)")
    ap.add_argument("--clicks", type=int, default=0, help="click → flag samples (pass.py --clicks)")
    ap.add_argument("--hold", type=int, default=0,
                    help="the stream held that long for a game, no bench page (pass.py --hold)")
    ap.add_argument("--vdd-gpu", default="",
                    help="the GPU that renders the virtual display (the XML's <friendlyname>), "
                         "e.g. \"NVIDIA GeForce RTX 5060 Ti\"; put back at the end")
    a = ap.parse_args()
    client = (["--client-port", str(a.client_port), "--client-url", a.client_url]
              if a.client_port else [])
    for kv in a.local_storage:
        client += ["--local-storage", kv]
    if a.game_fps:
        client += ["--game-fps", a.game_fps]
    if a.clicks:
        client += ["--clicks", str(a.clicks)]
    if a.hold:
        client += ["--hold", str(a.hold)]
    os.makedirs(OUT, exist_ok=True)
    scratch = os.path.join(OUT, "vdd_settings.saved.xml")
    shutil.copyfile(VDD_XML, scratch)
    if a.vdd_gpu:
        import re
        with open(VDD_XML, encoding="utf-8") as f:
            xml = f.read()
        xml2 = re.sub(r"<friendlyname>[^<]*</friendlyname>",
                      "<friendlyname>%s</friendlyname>" % a.vdd_gpu, xml, count=1)
        with open(VDD_XML, "w", encoding="utf-8", newline="") as f:
            f.write(xml2)
        print("virtual display rendered by", a.vdd_gpu, flush=True)
    # Each pass starts from this file: the product adds the mode it asks for,
    # and a list grown pass after pass has stopped the display from appearing
    # at all (29-30/09: a second size at 500 Hz, then every rate).
    with open(VDD_XML, "rb") as f:
        prepared = f.read()
    baseline = monitors()
    print("screens before:\n" + baseline, flush=True)
    try:
        for rep in range(a.repeat):
            for rate in [int(r) for r in a.rates.split(",")]:
                for cadence in a.cadences.split(","):
                    tag = "%s-v%d-%s-r%d" % (a.prefix, rate, cadence, rep)
                    print("==", tag, flush=True)
                    kill_dev()
                    with open(VDD_XML, "wb") as f:
                        f.write(prepared)
                    since = time.time()
                    launch_dev(rate, cadence, os.path.join(OUT, tag + ".server.log"))
                    r = subprocess.run([sys.executable, os.path.join(HERE, "pass.py"), "--tag", tag,
                                        "--target", "vdisplay", "--secs", str(a.secs),
                                        "--every", str(a.every), "--vsync", a.vsync,
                                        "--bitrate", str(a.bitrate), "--settle", str(a.settle)]
                                       + (["--autostep"] if cadence == "detect" else [])
                                       + client,
                                       capture_output=True, text=True)
                    tail = (r.stdout + r.stderr).strip().splitlines()
                    print("\n".join("   " + l for l in tail[-12:]), flush=True)
                    if not wait_released(baseline):
                        print("   !! the screens did not come back:\n" + monitors(), flush=True)
                    host_lines(tag, since)
    finally:
        kill_dev()
        shutil.copyfile(scratch, VDD_XML)
        print("settings file put back; screens after:\n" + monitors(), flush=True)


if __name__ == "__main__":
    main()
