#!/usr/bin/env python3
"""Animate one monitor at its own frame clock, and say how fast it ticks.

    python3 anim.py <connector> [fullscreen|maximized] [seconds]

A bench helper for plan « Idées Punktfunk » C1: a GTK 4 window put full screen
on the monitor whose connector is given (a Mutter virtual monitor is
"Meta-0", "Meta-1"...), redrawn at every tick of its frame clock, so the
compositor has something new to show at every refresh of that monitor. One
JSON line per second: the ticks counted, GTK's own estimate, and the refresh
rate the monitor announces to clients. "maximized" opens an ordinary window
on the primary monitor instead (GNOME 46 sends no picture of a full-screen
window to a shared-memory consumer; see the README).
"""
import json
import sys
import time

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
gi.require_version("Graphene", "1.0")
from gi.repository import Gdk, Gio, GLib, Graphene, Gtk  # noqa: E402

CONNECTOR = sys.argv[1] if len(sys.argv) > 1 else ""
MODE = sys.argv[2] if len(sys.argv) > 2 else "fullscreen"
SECONDS = int(sys.argv[3]) if len(sys.argv) > 3 else 0

state = {"ticks": 0, "frame": 0, "t0": None, "elapsed": 0, "monitor": None}


def say(obj):
    print(json.dumps(obj), flush=True)


class Flicker(Gtk.Widget):
    """A full colour that changes and a moving bar, drawn by GSK on the GPU
    (a cairo DrawingArea at 1080p capped the window near 70 frames/s)."""

    def do_snapshot(self, snapshot):
        n = state["frame"]
        width, height = self.get_width(), self.get_height()
        shade = (n % 64) / 64.0
        bg = Gdk.RGBA()
        bg.red, bg.green, bg.blue, bg.alpha = shade, 0.2, 1.0 - shade, 1.0
        rect = Graphene.Rect().init(0, 0, width, height)
        snapshot.append_color(bg, rect)
        bar = max(8, width // 16)
        fg = Gdk.RGBA()
        fg.red = fg.green = fg.blue = fg.alpha = 1.0
        snapshot.append_color(fg, Graphene.Rect().init((n * 7) % max(1, width - bar), 0,
                                                        bar, height))


def tick(widget, clock):
    state["ticks"] += 1
    state["frame"] += 1
    widget.queue_draw()
    now = time.monotonic()
    if state["t0"] is None:
        state["t0"] = now
        return True
    if now - state["t0"] >= 1.0:
        state["elapsed"] += 1
        mon = state["monitor"]
        say({
            "anim_t": state["elapsed"],
            "ticks": state["ticks"],
            "gtk_fps": round(clock.get_fps(), 1),
            "monitor_mhz": mon.get_refresh_rate() if mon else None,
            "monitor": mon.get_connector() if mon else None,
        })
        state["ticks"] = 0
        state["t0"] = now
        if SECONDS and state["elapsed"] >= SECONDS:
            widget.get_root().get_application().quit()
    return True


def activate(app):
    win = Gtk.ApplicationWindow(application=app, title="mw-anim")
    area = Flicker()
    area.set_hexpand(True)
    area.set_vexpand(True)
    win.set_child(area)
    monitors = Gdk.Display.get_default().get_monitors()
    names = []
    for i in range(monitors.get_n_items()):
        m = monitors.get_item(i)
        geo = m.get_geometry()
        names.append({"connector": m.get_connector(), "mhz": m.get_refresh_rate(),
                      "geometry": [geo.x, geo.y, geo.width, geo.height],
                      "scale": m.get_scale_factor()})
        if m.get_connector() == CONNECTOR:
            state["monitor"] = m
    say({"monitors": names})
    if MODE == "fullscreen" and state["monitor"] is not None:
        win.fullscreen_on_monitor(state["monitor"])
    else:
        win.maximize()
    win.present()
    area.add_tick_callback(tick)


# NON_UNIQUE: one animator per virtual monitor can run at once.
app = Gtk.Application(application_id="top.moonlightweb.bench.anim",
                      flags=Gio.ApplicationFlags.NON_UNIQUE)
app.connect("activate", activate)
GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, 15, lambda: (app.quit(), False)[1])
app.run([sys.argv[0]])
