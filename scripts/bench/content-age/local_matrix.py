"""A matrix of content-age passes on this machine: the product's virtual display
at several rates × the cadence keys, each pass on a --dev instance launched with
that pass's keys.

    python local_matrix.py --rates 60,240,500 --cadences client,host,host-ceiling,host-guarded

A client on the host's own machine is for tuning the bench, not for the verdict
(plan framerate-hote §2): it shares the host's compositor, whose clock follows
the primary screen — the virtual display itself, while it streams.

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
              "frames arrive at", "MW_NATIVE_TUNING")


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
    env["MW_VDD_REFRESH"] = str(rate)
    if cadence != "client":
        env["MW_NATIVE_TUNING"] = "cadence=" + cadence
    subprocess.Popen([EXE, "--dev", "--log", log], env=env,
                     creationflags=getattr(subprocess, "DETACHED_PROCESS", 0))
    time.sleep(8)


def wait_released(baseline, timeout=150):
    """The virtual display goes 4 s after the host sees the page gone — which
    takes it up to a minute. Wait for the screens to be as they were."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        if monitors() == baseline:
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
        if "cadence:" in l or "decode credit" in l:
            print("   ", l[l.find("[native]"):][:240], flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rates", default="60,240,500")
    ap.add_argument("--cadences", default="client,host,host-ceiling,host-guarded")
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--secs", type=float, default=30)
    ap.add_argument("--prefix", default="loc")
    a = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    scratch = os.path.join(OUT, "vdd_settings.saved.xml")
    shutil.copyfile(VDD_XML, scratch)
    baseline = monitors()
    print("screens before:\n" + baseline, flush=True)
    try:
        for rep in range(a.repeat):
            for rate in [int(r) for r in a.rates.split(",")]:
                for cadence in a.cadences.split(","):
                    tag = "%s-v%d-%s-r%d" % (a.prefix, rate, cadence, rep)
                    print("==", tag, flush=True)
                    kill_dev()
                    since = time.time()
                    launch_dev(rate, cadence, os.path.join(OUT, tag + ".server.log"))
                    r = subprocess.run([sys.executable, os.path.join(HERE, "pass.py"), "--tag", tag,
                                        "--target", "vdisplay", "--secs", str(a.secs)],
                                       capture_output=True, text=True)
                    tail = (r.stdout + r.stderr).strip().splitlines()
                    print("\n".join("   " + l for l in tail[-8:]), flush=True)
                    if not wait_released(baseline):
                        print("   !! the screens did not come back:\n" + monitors(), flush=True)
                    host_lines(tag, since)
    finally:
        kill_dev()
        shutil.copyfile(scratch, VDD_XML)
        print("settings file put back; screens after:\n" + monitors(), flush=True)


if __name__ == "__main__":
    main()
