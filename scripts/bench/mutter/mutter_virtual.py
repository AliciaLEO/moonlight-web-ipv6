#!/usr/bin/env python3
"""Probe Mutter's own virtual monitors: the D-Bus API under the portal.

    python3 mutter_virtual.py [--size WxH] [--hz N] [--cursor hidden|embedded|metadata]
                              [--no-rd] [--count N] [--layout extend|primary|exclusive]
                              [--anim fullscreen|maximized|none] [--wiggle]
                              [--lock-at S] [--seconds S] [--pw ./pw_vcount]

Plan « Idées Punktfunk » C1. The sequence gnome-remote-desktop and Punktfunk
use, with no portal and no dialog:

  RemoteDesktop.CreateSession -> ScreenCast.CreateSession(remote-desktop-session-id)
  -> Session.RecordVirtual({cursor-mode, modes?}) -> Start -> PipeWireStreamAdded(node)

then counts what the node delivers (pw_vcount), what the new monitor
announces (DisplayConfig), and, with --anim, how fast a window on it ticks
(anim.py). --hz > 0 passes `modes` (one mode: the size at that rate; Mutter
47+). Every line printed is JSON with a "src" key. Run it inside the user's
graphical session (XDG_RUNTIME_DIR, DBUS_SESSION_BUS_ADDRESS, WAYLAND_DISPLAY),
from a binary with no file capabilities.

Monitor changes are made one at a time and waited out (the configuration
serial stops moving) before the next: Punktfunk saw gnome-shell crash on
concurrent rebuilds. Teardown never re-applies a layout: a temporary
configuration reverts by itself when the virtual monitor goes.
"""
import argparse
import json
import os
import subprocess
import sys
import threading
import time

from gi.repository import Gio, GLib

RD_BUS, RD_PATH, RD_IFACE = ("org.gnome.Mutter.RemoteDesktop", "/org/gnome/Mutter/RemoteDesktop",
                             "org.gnome.Mutter.RemoteDesktop")
SC_BUS, SC_PATH, SC_IFACE = ("org.gnome.Mutter.ScreenCast", "/org/gnome/Mutter/ScreenCast",
                             "org.gnome.Mutter.ScreenCast")
DC_BUS, DC_PATH, DC_IFACE = ("org.gnome.Mutter.DisplayConfig", "/org/gnome/Mutter/DisplayConfig",
                             "org.gnome.Mutter.DisplayConfig")
CURSOR = {"hidden": 0, "embedded": 1, "metadata": 2}
HERE = os.path.dirname(os.path.abspath(__file__))
T0 = time.monotonic()
print_lock = threading.Lock()


def say(src, **kw):
    kw = {"src": src, "at": round(time.monotonic() - T0, 3), **kw}
    with print_lock:
        print(json.dumps(kw), flush=True)


bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)


def call(name, path, iface, method, args=None, reply=None, timeout=10000):
    v = bus.call_sync(name, path, iface, method, args,
                      GLib.VariantType(reply) if reply else None,
                      Gio.DBusCallFlags.NONE, timeout, None)
    return v.unpack() if v is not None else None


def prop(name, path, iface, key):
    try:
        return call(name, path, "org.freedesktop.DBus.Properties", "Get",
                    GLib.Variant("(ss)", (iface, key)), "(v)")[0]
    except GLib.Error as e:
        return f"error: {e.message}"


def state():
    serial, monitors, logical, props = call(DC_BUS, DC_PATH, DC_IFACE, "GetCurrentState",
                                            None, "(ua((ssss)a(siiddada{sv})a{sv})"
                                            "a(iiduba(ssss)a{sv})a{sv})")
    return {"serial": serial, "monitors": monitors, "logical": logical, "props": props}


def monitor_summary(st, connector):
    for (spec, modes, mprops) in st["monitors"]:
        if spec[0] != connector:
            continue
        cur = [m for m in modes if m[6].get("is-current")]
        pref = [m for m in modes if m[6].get("is-preferred")]
        pick = (cur or pref or modes)[0] if modes else None
        logical = None
        for (x, y, scale, transform, primary, specs, _lp) in st["logical"]:
            if any(s[0] == connector for s in specs):
                logical = {"x": x, "y": y, "scale": scale, "primary": primary}
        return {"connector": connector, "vendor": spec[1], "product": spec[2], "serial": spec[3],
                "modes": [[m[0], m[1], m[2], round(m[3], 3)] for m in modes][:8],
                "current": [pick[0], pick[1], pick[2], round(pick[3], 3)] if pick else None,
                "is_current_flag": bool(cur), "builtin": mprops.get("is-builtin"),
                "display_name": mprops.get("display-name"), "logical": logical}
    return None


def connectors(st):
    return {spec[0] for (spec, _m, _p) in st["monitors"]}


def settle(gone=None, deadline_s=4.0):
    """Wait until `gone` has left and the configuration serial holds still."""
    start = time.monotonic()
    while gone and time.monotonic() - start < deadline_s:
        if gone not in connectors(state()):
            break
        time.sleep(0.1)
    last = None
    while time.monotonic() - start < deadline_s:
        s = state()["serial"]
        if s == last:
            break
        last = s
        time.sleep(0.15)
    return round(time.monotonic() - start, 3)


def pump(seconds):
    ctx = GLib.MainContext.default()
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        while ctx.pending():
            ctx.iteration(False)
        time.sleep(0.002)


class Virtual:
    def __init__(self, idx, args):
        self.idx, self.args = idx, args
        self.rd_path = self.sc_path = self.stream_path = None
        self.node = None
        self.connector = None
        self.procs = []

    def create(self, before):
        a = self.args
        t = time.monotonic()
        if a.rd:
            self.rd_path = call(RD_BUS, RD_PATH, RD_IFACE, "CreateSession", None, "(o)")[0]
            sid = prop(RD_BUS, self.rd_path, RD_IFACE + ".Session", "SessionId")
            opts = {"remote-desktop-session-id": GLib.Variant("s", sid)}
        else:
            opts = {}
        self.sc_path = call(SC_BUS, SC_PATH, SC_IFACE, "CreateSession",
                            GLib.Variant("(a{sv})", (opts,)), "(o)")[0]
        rec = {"cursor-mode": GLib.Variant("u", CURSOR[a.cursor])}
        if a.hz > 0:
            mode = {"size": GLib.Variant("(uu)", (a.w, a.h)),
                    "refresh-rate": GLib.Variant("d", float(a.hz)),
                    "is-preferred": GLib.Variant("b", True)}
            rec["modes"] = GLib.Variant("aa{sv}", [mode])
        self.stream_path = call(SC_BUS, self.sc_path, SC_IFACE + ".Session", "RecordVirtual",
                                GLib.Variant("(a{sv})", (rec,)), "(o)")[0]
        got = {}

        def on_added(_c, _s, _p, _i, _n, params):
            got["node"] = params.unpack()[0]

        sub = bus.signal_subscribe(None, SC_IFACE + ".Stream", "PipeWireStreamAdded",
                                   self.stream_path, None, Gio.DBusSignalFlags.NONE, on_added)
        if a.rd:
            call(RD_BUS, self.rd_path, RD_IFACE + ".Session", "Start")
        else:
            call(SC_BUS, self.sc_path, SC_IFACE + ".Session", "Start")
        end = time.monotonic() + 10
        ctx = GLib.MainContext.default()
        while "node" not in got and time.monotonic() < end:
            ctx.iteration(True)
        bus.signal_unsubscribe(sub)
        if "node" not in got:
            raise RuntimeError("PipeWireStreamAdded did not come within 10 s")
        self.node = got["node"]
        node_ms = round((time.monotonic() - t) * 1000)
        params = prop(SC_BUS, self.stream_path, SC_IFACE + ".Stream", "Parameters")
        say(f"v{self.idx}", event="node", node=self.node, node_ms=node_ms,
            stream=self.stream_path, parameters=params)
        return node_ms

    def start_counter(self):
        a = self.args
        # Runs until stop() ends it (SIGTERM: the counter prints its totals).
        p = subprocess.Popen([a.pw, str(self.node), "0", str(a.w), str(a.h),
                              str(a.hz if a.hz > 0 else a.maxfps), a.pw_opts],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.procs.append(p)
        threading.Thread(target=relay, args=(p, f"pw{self.idx}"), daemon=True).start()

    def find_connector(self, before, wait_s=4.0):
        end = time.monotonic() + wait_s
        while time.monotonic() < end:
            st = state()
            new = connectors(st) - before
            if new:
                self.connector = sorted(new)[-1]
                say(f"v{self.idx}", event="monitor", **monitor_summary(st, self.connector))
                return st
            pump(0.1)
        say(f"v{self.idx}", event="monitor", error="no new connector")
        return state()

    def start_anim(self):
        a = self.args
        p = subprocess.Popen([sys.executable, os.path.join(HERE, "anim.py"), self.connector or "",
                              a.anim, "0"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True)
        self.procs.append(p)
        threading.Thread(target=relay, args=(p, f"anim{self.idx}"), daemon=True).start()

    def stop(self):
        for p in self.procs:
            if p.poll() is None:
                p.terminate()
        for p in self.procs:
            try:
                p.wait(3)
            except subprocess.TimeoutExpired:
                p.kill()
        t = time.monotonic()
        try:
            if self.rd_path:
                call(RD_BUS, self.rd_path, RD_IFACE + ".Session", "Stop")
            else:
                call(SC_BUS, self.sc_path, SC_IFACE + ".Session", "Stop")
        except GLib.Error as e:
            say(f"v{self.idx}", event="stop", error=e.message)
        waited = settle(gone=self.connector)
        still = self.connector in connectors(state()) if self.connector else None
        say(f"v{self.idx}", event="stopped", stop_ms=round((time.monotonic() - t) * 1000),
            settle_s=waited, connector_still_there=still)


seen = {}


def relay(proc, src):
    for line in proc.stdout:
        line = line.strip()
        if not line:
            continue
        try:
            obj = json.loads(line)
        except ValueError:
            say(src, text=line[:300])
            continue
        seen.setdefault(src, []).append(obj)
        say(src, **obj)


def median(values):
    values = sorted(values)
    return values[len(values) // 2] if values else None


def summary():
    """Medians after the first two seconds: frames/s delivered, ticks/s of the window."""
    out = {}
    for src, lines in sorted(seen.items()):
        per_s = [o for o in lines if "t" in o and o["t"] > 2] or \
                [o for o in lines if "anim_t" in o and o["anim_t"] > 2]
        if src.startswith("pw"):
            fmt = next((o for o in lines if "format" in o), {})
            out[src] = {"fps": median([o["fps"] for o in per_s]),
                        "corrupted": median([o["corrupted"] for o in per_s]),
                        "empty": median([o["empty"] for o in per_s]),
                        "cursor": median([o["cursor"] for o in per_s]),
                        "cursor_bitmap": median([o["cursor_bitmap"] for o in per_s]),
                        "max_gap_ms": median([o["max_gap_ms"] for o in per_s]),
                        "size": [fmt.get("w"), fmt.get("h")], "max_rate": fmt.get("max_rate"),
                        "memory": fmt.get("memory")}
        elif src.startswith("anim"):
            out[src] = {"ticks": median([o["ticks"] for o in per_s]),
                        "monitor_mhz": next((o["monitor_mhz"] for o in per_s), None)}
    say("summary", **out)


def apply_layout(kind, virtuals):
    """Make the virtual monitors primary (physicals kept to their right) or the only ones."""
    st = state()
    layout_mode = st["props"].get("layout-mode", 1)
    vconns = [v.connector for v in virtuals if v.connector]
    if not vconns:
        return
    mode_of = {}
    for (spec, modes, _p) in st["monitors"]:
        cur = [m for m in modes if m[6].get("is-current")] or \
              [m for m in modes if m[6].get("is-preferred")] or modes
        if cur:
            mode_of[spec[0]] = cur[0]
    logical = []
    x = 0
    for c in vconns:
        m = mode_of[c]
        logical.append((x, 0, 1.0, 0, c == vconns[0], [(c, m[0], {})]))
        x += m[1]
    if kind == "primary":
        physical = sorted(st["logical"], key=lambda lm: (lm[0], lm[1]))
        for (_x, _y, scale, transform, _prim, specs, _lp) in physical:
            mons = [(s[0], mode_of[s[0]][0], {}) for s in specs if s[0] not in vconns]
            if not mons:
                continue
            w = mode_of[specs[0][0]][1]
            if transform in (1, 3, 5, 7):
                w = mode_of[specs[0][0]][2]
            logical.append((x, 0, scale, transform, False, mons))
            x += int(w / scale) if layout_mode == 1 else w
    args = GLib.Variant("(uua(iiduba(ssa{sv}))a{sv})", (st["serial"], 1, logical, {}))
    t = time.monotonic()
    try:
        call(DC_BUS, DC_PATH, DC_IFACE, "ApplyMonitorsConfig", args)
        waited = settle()
        st2 = state()
        say("layout", kind=kind, ok=True, apply_ms=round((time.monotonic() - t) * 1000),
            settle_s=waited, monitors=[monitor_summary(st2, s[0]) for (s, _m, _p)
                                       in st2["monitors"]])
    except GLib.Error as e:
        say("layout", kind=kind, ok=False, error=e.message)


def wiggle_loop(v, stop_evt):
    """Move the pointer in a circle over the first virtual monitor (remote desktop input)."""
    import math
    n = 0
    while not stop_evt.is_set():
        x = v.args.w / 2 + v.args.w / 4 * math.cos(n / 30)
        y = v.args.h / 2 + v.args.h / 4 * math.sin(n / 30)
        try:
            call(RD_BUS, v.rd_path, RD_IFACE + ".Session", "NotifyPointerMotionAbsolute",
                 GLib.Variant("(sdd)", (v.stream_path, x, y)))
        except GLib.Error as e:
            say("wiggle", error=e.message)
            return
        n += 1
        time.sleep(0.008)


def graphical_session():
    out = subprocess.run(["loginctl", "list-sessions", "--no-legend"], capture_output=True,
                         text=True).stdout
    for line in out.splitlines():
        sid = line.split()[0]
        t = subprocess.run(["loginctl", "show-session", sid, "-p", "Type", "--value"],
                           capture_output=True, text=True).stdout.strip()
        if t in ("wayland", "x11"):
            return sid
    return None


def shell_pid():
    r = subprocess.run(["pgrep", "-xo", "gnome-shell"], capture_output=True, text=True)
    return r.stdout.strip() or None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", default="1920x1080")
    ap.add_argument("--hz", type=int, default=0, help="> 0: pass `modes` at this rate")
    ap.add_argument("--maxfps", type=int, default=240, help="consumer's max rate without --hz")
    ap.add_argument("--cursor", choices=CURSOR, default="metadata")
    ap.add_argument("--no-rd", dest="rd", action="store_false")
    ap.add_argument("--count", type=int, default=1)
    ap.add_argument("--layout", choices=("extend", "primary", "exclusive"), default="extend",
                    help="exclusive switches the physical monitors off: see the README first")
    ap.add_argument("--allow-exclusive", action="store_true",
                    help="gnome-shell 46 crashed switching a second GPU's monitor back on")
    ap.add_argument("--anim", choices=("fullscreen", "maximized", "none"), default="fullscreen")
    ap.add_argument("--wiggle", action="store_true")
    ap.add_argument("--lock-at", type=float, default=0)
    ap.add_argument("--seconds", type=int, default=8)
    ap.add_argument("--pw", default=os.path.join(HERE, "pw_vcount"))
    ap.add_argument("--pw-opts", default="dmabuf",
                    help="pw_vcount options: dmabuf, size=pin|range, rate=max|range|product")
    a = ap.parse_args()
    a.w, a.h = (int(v) for v in a.size.lower().split("x"))
    if a.layout == "exclusive" and not a.allow_exclusive:
        ap.error("--layout exclusive needs --allow-exclusive (README, Traps)")

    pid0 = shell_pid()
    say("env", shell=subprocess.run(["gnome-shell", "--version"], capture_output=True,
                                    text=True).stdout.strip(),
        screencast_version=prop(SC_BUS, SC_PATH, SC_IFACE, "Version"),
        remotedesktop_version=prop(RD_BUS, RD_PATH, RD_IFACE, "Version"),
        session_type=os.environ.get("XDG_SESSION_TYPE"), shell_pid=pid0,
        args={k: v for k, v in vars(a).items() if k != "pw"})
    st = state()
    say("before", serial=st["serial"], layout_mode=st["props"].get("layout-mode"),
        monitors=[monitor_summary(st, s[0]) for (s, _m, _p) in st["monitors"]])

    virtuals = []
    try:
        for i in range(a.count):
            v = Virtual(i, a)
            before = connectors(state())
            v.create(before)
            virtuals.append(v)
            # Mutter makes the monitor when a consumer negotiates the stream's format.
            v.start_counter()
            v.find_connector(before)
            say(f"v{i}", event="settled", settle_s=settle())
        if a.layout != "extend":
            apply_layout(a.layout, virtuals)
        # The pointer first: GNOME opens a window on the monitor under it.
        stop_evt = threading.Event()
        if a.wiggle and a.rd and virtuals:
            threading.Thread(target=wiggle_loop, args=(virtuals[0], stop_evt), daemon=True).start()
            pump(0.5)
        for v in virtuals:
            if a.anim != "none":
                v.start_anim()
        sid = graphical_session() if a.lock_at else None
        if a.lock_at:
            pump(a.lock_at)
            subprocess.run(["loginctl", "lock-session", sid])
            say("lock", session=sid, locked=True)
            pump(max(0.0, a.seconds - a.lock_at))
        else:
            pump(a.seconds)
        stop_evt.set()
        if a.lock_at:
            subprocess.run(["loginctl", "unlock-session", sid])
            say("lock", session=sid, locked=False)
        pump(1.5)
    except (GLib.Error, RuntimeError) as e:
        say("error", error=getattr(e, "message", str(e)))
    finally:
        for v in reversed(virtuals):
            v.stop()
        summary()
        st = state()
        pid1 = shell_pid()
        say("after", serial=st["serial"], shell_pid=pid1, shell_survived=(pid1 == pid0),
            monitors=[monitor_summary(st, s[0]) for (s, _m, _p) in st["monitors"]])


if __name__ == "__main__":
    main()
