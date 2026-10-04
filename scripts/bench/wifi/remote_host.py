"""A native host on another machine, for `series.py --host` and `pass.py --host`.

Plan « Wi-Fi : la vidéo qui attend dans SCTP », before W3 (Bruno, 04/10/2026):
retrcut= and sctpburst= measured on the Linux and macOS native hosts too. The
host is that machine's DEV edition; this module does, over SSH, what the local
bench does on DualRTX:

- its `native_tuning` (settings.json), which the server reads at each /start:
  a variant needs no restart;
- the bench page (scroll.html) on its captured screen, for the stream to carry
  a real bitrate. Its time band is not read: calibrating it needs the host's
  own clock next to the page, so a remote pass keeps the per-frame log's age
  (capture → draw on the host's clock), the click → flag and the relay's log;
- its log, cut at the pass, and the relay's frame log (relaylog=1), fetched
  next to the pass's other files.

Linux: the UM790Pro under Ubuntu (bruno@192.168.1.9 through WSL's sshpass, as
series.py's lx client). The click's flag exists in an X11 session only
(LatencyFlagX11.cpp). macOS: mw-mac (its DEV app is root:wheel; updating it is
Bruno's `sudo installer`).
"""
import os
import shlex
import subprocess
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CONTENT = os.path.join(os.path.dirname(HERE), "content")
NOWIN = getattr(subprocess, "CREATE_NO_WINDOW", 0)
WSL_SSH = ["wsl.exe", "-u", "root", "--", "sshpass", "-p", "123456", "ssh",
           "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
           "-o", "LogLevel=ERROR", "-o", "ConnectTimeout=8"]

# Rewrites settings.json's native_tuning atomically (../loss/set_tuning.py),
# run on the host with its own python3.
SET_TUNING = r"""
import json, os, sys, tempfile
path, spec = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 else "").strip()
with open(path, encoding="utf-8") as f:
    obj = json.load(f)
if spec:
    obj["native_tuning"] = spec
else:
    obj.pop("native_tuning", None)
fd, tmp = tempfile.mkstemp(dir=os.path.dirname(os.path.abspath(path)), prefix=".settings-",
                           suffix=".json")
with os.fdopen(fd, "w", encoding="utf-8") as f:
    json.dump(obj, f, indent=4, ensure_ascii=False)
os.replace(tmp, path)
print("native_tuning = %r" % obj.get("native_tuning", ""))
"""


class RemoteHost:
    """What series.py and pass.py ask of a host on another machine."""

    # The fleet's id (../acceptance/fleet.py, hosts.local.json): its address,
    # its DEV edition's PIN.
    mid = ""
    # In the passes' names, where the local series names the GPU.
    tag = ""

    def ssh(self):
        raise NotImplementedError

    def sh(self, script, timeout=60):
        p = subprocess.run(self.ssh() + ["bash -s"], input=script, capture_output=True,
                           text=True, encoding="utf-8", errors="replace", timeout=timeout,
                           creationflags=NOWIN)
        return (p.stdout or "") + (p.stderr or "")

    def settings_path(self):
        raise NotImplementedError

    def log_path(self):
        raise NotImplementedError

    def set_tuning(self, spec):
        path = self.settings_path()
        if not path:
            return "no DEV settings.json found"
        return self.sh("python3 - %s %s <<'PY'\n%s\nPY\n" % (shlex.quote(path), shlex.quote(spec),
                                                            SET_TUNING)).strip()

    def log_size(self):
        path = self.log_path()
        out = self.sh("stat -c %%s %s 2>/dev/null || stat -f %%z %s" % (shlex.quote(path),
                                                                      shlex.quote(path)))
        try:
            return int(out.strip().splitlines()[-1])
        except (ValueError, IndexError):
            return 0

    def fetch_log(self, offset, dest):
        """The host's log from @p offset on (what one pass wrote) into @p dest."""
        text = self.sh("tail -c +%d %s" % (offset + 1, shlex.quote(self.log_path())), timeout=120)
        with open(dest, "w", encoding="utf-8") as f:
            f.write(text)
        return len(text)

    def fetch_relay_csv(self, since_epoch, dest):
        """The relay's frame log of the pass (relay-frames-<pid>-<ms>.csv, next to
        the log), the newest written after @p since_epoch, into @p dest."""
        folder = os.path.dirname(self.log_path())
        name = self.sh("cd %s && ls -t relay-frames-*.csv 2>/dev/null | head -1" %
                       shlex.quote(folder)).strip()
        if not name.startswith("relay-frames-"):
            return False
        try:
            ms = int(name.rsplit("-", 1)[1].split(".")[0])
        except ValueError:
            ms = 0
        if ms / 1000 < since_epoch - 5:
            return False
        text = self.sh("cat %s" % shlex.quote(folder + "/" + name), timeout=120)
        with open(dest, "w", encoding="utf-8", newline="") as f:
            f.write(text)
        return True

    def push_content(self):
        for name in sorted(os.listdir(CONTENT)):
            if not name.endswith((".html", ".js")):
                continue
            with open(os.path.join(CONTENT, name), encoding="utf-8") as f:
                body = f.read()
            p = subprocess.run(self.ssh() + ["mkdir -p /tmp/mw-content && cat > /tmp/mw-content/" +
                                             name], input=body, capture_output=True, text=True,
                               encoding="utf-8", timeout=30, creationflags=NOWIN)
            if p.returncode:
                return "push failed: %s" % (p.stderr or "")[-200:]
        return "content pushed"

    def content_start(self, page):
        raise NotImplementedError

    def content_stop(self):
        raise NotImplementedError


class LinuxHost(RemoteHost):
    mid = "um790pro"
    tag = "lx"
    PROFILE = "/tmp/mw-content-profile"

    def ssh(self):
        return WSL_SSH + ["bruno@192.168.1.9"]

    def _find(self, pattern):
        out = self.sh("ls -t %s 2>/dev/null | head -1" % pattern)
        lines = [l for l in out.splitlines() if l.startswith("/")]
        return lines[0] if lines else ""

    def settings_path(self):
        if not getattr(self, "_settings", ""):
            self._settings = self._find("$HOME/.local/share/*/MoonlightWebDev*/settings.json "
                                        "$HOME/.local/share/MoonlightWebDev*/settings.json "
                                        "$HOME/.config/*/MoonlightWebDev*/settings.json")
        return self._settings

    def log_path(self):
        if not getattr(self, "_log", ""):
            base = os.path.dirname(self.settings_path()) if self.settings_path() else ""
            self._log = self._find("%s/logs/*.log" % shlex.quote(base)) if base else ""
        return self._log

    def content_start(self, page):
        self.push_content()
        url = "file:///tmp/mw-content/" + page
        # The desktop session's own environment (X11 or Wayland, its bus): a
        # browser launched over SSH otherwise has no screen to go to.
        script = r"""
eval "$(systemctl --user show-environment | grep -E '^(DISPLAY|XAUTHORITY|WAYLAND_DISPLAY|XDG_RUNTIME_DIR|DBUS_SESSION_BUS_ADDRESS)=' | sed 's/^/export /')"
pkill -f "user-data-dir=%(p)s" 2>/dev/null
sleep 1
setsid -f google-chrome-stable --user-data-dir=%(p)s --no-first-run --no-default-browser-check \
    --ozone-platform-hint=auto --kiosk --start-fullscreen --disable-infobars \
    --disable-search-engine-choice-screen --disable-sync --password-store=basic \
    --disable-features=CalculateNativeWinOcclusion --disable-backgrounding-occluded-windows \
    --disable-renderer-backgrounding %(u)s >/tmp/mw-content-chrome.log 2>&1
sleep 3
pgrep -f "user-data-dir=%(p)s" >/dev/null && echo "content up: %(u)s" || echo "content did not start"
""" % {"p": self.PROFILE, "u": shlex.quote(url)}
        return self.sh(script, timeout=60).strip()

    def content_stop(self):
        return self.sh('pkill -f "user-data-dir=%s" 2>/dev/null; echo content stopped' %
                       self.PROFILE).strip()


class MacHost(RemoteHost):
    mid = "mw-mac"
    tag = "macos"
    PROFILE = "/tmp/mw-content-profile"

    def ssh(self):
        return ["ssh", "-o", "ConnectTimeout=8", "mw-mac"]

    def settings_path(self):
        if not getattr(self, "_settings", ""):
            out = self.sh("ls -t \"$HOME\"/Library/Application\\ Support/*/MoonlightWebDev*/"
                          "settings.json 2>/dev/null | head -1")
            lines = [l for l in out.splitlines() if l.startswith("/")]
            self._settings = lines[0] if lines else ""
        return self._settings

    def log_path(self):
        if not getattr(self, "_log", ""):
            base = os.path.dirname(self.settings_path()) if self.settings_path() else ""
            out = self.sh("ls -t %s/logs/*.log 2>/dev/null | head -1" % shlex.quote(base)) \
                if base else ""
            lines = [l for l in out.splitlines() if l.startswith("/")]
            self._log = lines[0] if lines else ""
        return self._log

    def content_start(self, page):
        self.push_content()
        url = "file:///tmp/mw-content/" + page
        script = r"""
pkill -f "user-data-dir=%(p)s" 2>/dev/null
sleep 1
open -na "Google Chrome" --args --user-data-dir=%(p)s --no-first-run --no-default-browser-check \
    --kiosk --disable-infobars --disable-backgrounding-occluded-windows \
    --disable-renderer-backgrounding %(u)s
sleep 3
pgrep -f "user-data-dir=%(p)s" >/dev/null && echo "content up: %(u)s" || echo "content did not start"
""" % {"p": self.PROFILE, "u": shlex.quote(url)}
        return self.sh(script, timeout=60).strip()

    def content_stop(self):
        return self.sh('pkill -f "user-data-dir=%s" 2>/dev/null; echo content stopped' %
                       self.PROFILE).strip()


HOSTS = {"um790pro": LinuxHost, "lx": LinuxHost, "mw-mac": MacHost, "macos": MacHost}


def for_machine(name):
    return HOSTS[name]()


if __name__ == "__main__":
    # A dry look at a host: where its DEV keeps its settings and its log.
    import sys
    h = for_machine(sys.argv[1] if len(sys.argv) > 1 else "um790pro")
    print("settings:", h.settings_path() or "-")
    print("log:", h.log_path() or "-", "(%d bytes)" % h.log_size() if h.log_path() else "")
