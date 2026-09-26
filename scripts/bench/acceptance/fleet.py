"""Reach the bench fleet, and install on it.

Nothing in scripts/bench/ knows how to log into a machine: discover.ps1 tests
port 22 and stops there. The transport has always been a pair of hands. This
module is that pair of hands, and it is the whole reason the acceptance run can
be unattended.

Three ways in, because the fleet has three:

  ssh           an entry of ~/.ssh/config, or a key file — the good case
  wsl-sshpass   a password-only box, reached through WSL's sshpass because the
                Windows OpenSSH client has no way to feed one
  local         this machine, through PowerShell

Two rules learned the hard way and enforced here:

  * A command is never interpolated into `ssh "..."`. PowerShell 5.1 eats the
    double quotes before ssh sees them, and the remote shell on a Windows box
    is cmd.exe, which eats what is left. Every script is written to a file,
    copied, and run from there.
  * A Windows SSH session lands in session 0. It has no desktop, so anything
    that must touch the screen goes through a scheduled task, and an installer
    that launches the app never gives the shell back — it is started detached.
"""
import json
import os
import shlex
import subprocess
import tempfile
import time
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
SSH_OPTS = ["-o", "StrictHostKeyChecking=no", "-o", "ConnectTimeout=12",
            "-o", "ServerAliveInterval=20"]

# What each machine IS — never where it lives or how to log into it.
#
# The split is the one scripts/bench/hosts.json already makes, and for the same
# reason: a bench fleet is a description of one room, true there and false
# everywhere else, and a repository is the wrong place for somebody else's
# hardware. Addresses, accounts and passwords come from hosts.local.json, which
# git ignores. Putting them here, even briefly, is how a password reaches a
# public remote.
RECIPES = {
    "local": {
        "label": "the machine running the campaign — reference host and measuring client",
        "os": "windows", "perf": True,
    },
    "mw-mac": {"label": "MacBook Pro M1 Pro, VideoToolbox", "os": "macos", "perf": True},
    "mw-intel": {"label": "Intel N95 / UHD Graphics, the only QuickSync bench",
                 "os": "windows", "arch": "x64", "perf": True},
    "mw-arm": {"label": "Snapdragon 7c, Windows on ARM. Compatibility only",
               "os": "windows", "arch": "arm64", "perf": False},
    "um790pro": {"label": "mini PC, Radeon 780M / VA-API under Linux", "os": "linux",
                 "perf": True},
    "mw-debian": {"label": "Debian in a VM, no GPU. Compatibility only", "os": "linux",
                  "perf": False},
}

# An OS guess for a machine the recipes do not name, from what discover.ps1
# style hints are available.
_OS_HINTS = {"windows": "windows", "windows-arm64": "windows", "macos": "macos",
             "linux": "linux", "darwin": "macos"}


def _load_fleet():
    """Merge the recipes above with the addresses in hosts.local.json.

    Missing file, or a machine with no address: that machine is simply absent
    from the run, and the report says so, rather than the harness failing to
    import.
    """
    here = os.path.dirname(HERE)
    out = {}
    local = {}
    path = os.path.join(here, "hosts.local.json")
    if os.path.exists(path):
        try:
            with open(path, encoding="utf-8") as f:
                data = json.load(f)
            for entry in data.get("machines", []):
                if entry.get("id"):
                    local[entry["id"]] = entry
        except (ValueError, OSError):
            local = {}

    for mid, recipe in RECIPES.items():
        entry = local.get(mid, {})
        m = dict(recipe)
        expect = entry.get("expect") or {}
        m["os"] = _OS_HINTS.get(expect.get("os", ""), m["os"])
        m["address"] = entry.get("address", "127.0.0.1" if mid == "local" else "")
        kind = entry.get("kind") or ("local" if mid == "local" else "ssh")
        if entry.get("sshAlias"):
            m["kind"] = "ssh"
            m["ssh"] = entry["sshAlias"]
        elif kind == "local":
            m["kind"] = "local"
        elif entry.get("password"):
            # No sshpass on a Windows desk; WSL has one. This says nothing about
            # the remote's OS — the dual-boot mini PC answers here under Windows
            # too, and run_script picks the shell from m["os"], not from this.
            m["kind"] = "wsl-sshpass"
            m["user"] = entry.get("user", "")
            m["password"] = entry["password"]
        else:
            m["kind"] = "ssh"
            m["ssh"] = "%s@%s" % (entry.get("user", ""), m["address"])
        if entry.get("keyFile"):
            m["key"] = os.path.expanduser(entry["keyFile"])
        if entry.get("password"):
            m["sudo_password"] = entry["password"]
        if entry.get("sudoPassword"):
            m["sudo_password"] = entry["sudoPassword"]
        if m["kind"] == "local" or m["address"]:
            out[mid] = m
    return out


MACHINES = _load_fleet()

DEV_HTTP = 48080
DEV_HTTPS = 48443


class RemoteError(Exception):
    pass


def _run(argv, timeout=300, check=False):
    p = subprocess.run(argv, capture_output=True, text=True, timeout=timeout,
                       encoding="utf-8", errors="replace")
    if check and p.returncode != 0:
        raise RemoteError("%s -> %d\n%s\n%s" % (argv[0], p.returncode, p.stdout, p.stderr))
    return p


def _wsl_path(win_path):
    """C:\\x -> /mnt/c/x, so WSL can read a file this process just wrote."""
    p = os.path.abspath(win_path).replace("\\", "/")
    if len(p) > 1 and p[1] == ":":
        return "/mnt/" + p[0].lower() + p[2:]
    return p


def ssh_argv(m):
    argv = ["ssh"] + SSH_OPTS
    if m.get("key"):
        argv += ["-i", m["key"]]
    argv.append(m["ssh"])
    return argv


# A Windows box's default ssh shell is cmd.exe on some of these machines and
# PowerShell on others, and the difference is not cosmetic: `< NUL` and `type`
# are cmd builtins, and PowerShell answers "The '<' operator is reserved for
# future use" and then reads a file that was never written — which looks
# exactly like a script that printed nothing. Probed once per machine, because
# it cannot change under us mid-run. `%COMPUTERNAME%` is the tell: cmd expands
# it, PowerShell hands it back verbatim.
_SHELL_CACHE = {}


def _windows_shell(mid, runner):
    if mid in _SHELL_CACHE:
        return _SHELL_CACHE[mid]
    probe = runner("echo %COMPUTERNAME%", 60)
    out = (probe.stdout or "").strip()
    shell = "powershell" if "%COMPUTERNAME%" in out else "cmd"
    _SHELL_CACHE[mid] = shell
    return shell


def _win_run_cmd(script_path, log_path, shell):
    """The one-liner that runs a .ps1 to a log file, severing inherited handles.

    An installer hands its handles to a process that outlives the ssh session,
    and ssh waits for every one of them: without the redirection the channel
    hangs for the full timeout with the work long since finished.
    """
    inner = ('powershell -NoProfile -ExecutionPolicy Bypass -File "%s" '
             '> "%s" 2>&1 < NUL' % (script_path, log_path))
    if shell == "cmd":
        return inner
    # --% stops PowerShell parsing the rest, so cmd receives it verbatim.
    return "cmd --% /c " + inner


def _win_cat(path, shell):
    return ('type "%s"' % path) if shell == "cmd" else ('Get-Content -Raw "%s"' % path)


def _win_del(paths, shell):
    if shell == "cmd":
        return "del /q " + " ".join('"%s"' % p for p in paths)
    return "Remove-Item -Force -ErrorAction SilentlyContinue " + ", ".join('"%s"' % p for p in paths)


def run_script(mid, script, timeout=600, shell=None):
    """Run a script on a machine. Always by file — never interpolated into ssh.

    `shell` defaults to what the machine's OS uses: powershell on Windows,
    /bin/sh elsewhere. Returns (rc, stdout, stderr).
    """
    m = MACHINES[mid]
    shell = shell or ("powershell" if m["os"] == "windows" else "sh")
    suffix = ".ps1" if shell == "powershell" else ".sh"
    name = "mwacc-%s%s" % (uuid.uuid4().hex[:8], suffix)
    local = os.path.join(tempfile.gettempdir(), name)
    # LF and UTF-8 without a BOM: a BOM makes PowerShell report a phantom
    # "Missing closing '}'", and CRLF makes /bin/sh choke on every line.
    with open(local, "w", encoding="utf-8", newline="\n") as f:
        f.write(script)

    try:
        if m["kind"] == "local":
            p = _run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                      "-File", local], timeout=timeout)
            return p.returncode, p.stdout, p.stderr

        if m["kind"] == "wsl-sshpass":
            # The transport and the remote OS are independent. sshpass is what
            # this desk has for a machine that only takes a password, and one
            # of those machines is a Windows box: sending it `sh script` would
            # fail in a way that reads like the script itself being wrong.
            windows = m["os"] == "windows"
            remote = ("C:/Windows/Temp/" + name) if windows else ("/tmp/" + name)
            sent = _run(["wsl", "-u", "root", "--", "bash", "-c",
                         "sshpass -p %s scp %s %s %s@%s:%s" % (
                             shlex.quote(m["password"]), " ".join(SSH_OPTS),
                             shlex.quote(_wsl_path(local)), m["user"], m["address"], remote)],
                        timeout=timeout)
            if sent.returncode != 0:
                raise RemoteError("scp to %s failed: %s" % (mid, sent.stderr.strip()))

            def _sshpass(cmd, tmo):
                return _run(["wsl", "-u", "root", "--", "bash", "-c",
                             "sshpass -p %s ssh %s %s@%s %s" % (
                                 shlex.quote(m["password"]), " ".join(SSH_OPTS),
                                 m["user"], m["address"], shlex.quote(cmd))],
                            timeout=tmo)

            if windows:
                # Same inherited-handle trap as the plain-ssh Windows path: run
                # to a file, then fetch the file in a second session.
                # Backslashes throughout — cmd.exe reads a leading forward
                # slash as the start of an option.
                win = remote.replace("/", "\\")
                log = win + ".out"
                sh = _windows_shell(mid, _sshpass)
                p = _sshpass(_win_run_cmd(win, log, sh), timeout)
                back = _sshpass(_win_cat(log, sh), 120)
                _sshpass(_win_del([win, log], sh), 60)
                return p.returncode, back.stdout, (p.stderr or "") + (back.stderr or "")

            p = _sshpass("sh %s; rc=$?; rm -f %s; exit $rc" % (remote, remote), timeout)
            return p.returncode, p.stdout, p.stderr

        # plain ssh
        remote = ("C:/Windows/Temp/" + name) if m["os"] == "windows" else ("/tmp/" + name)
        scp = ["scp"] + SSH_OPTS
        if m.get("key"):
            scp += ["-i", m["key"]]
        scp += [local, "%s:%s" % (m["ssh"], remote)]
        sent = _run(scp, timeout=timeout)
        if sent.returncode != 0:
            raise RemoteError("scp to %s failed: %s" % (mid, sent.stderr.strip()))

        if m["os"] == "windows":
            # ssh waits for every inherited handle to close, not just for the
            # command to exit. An installer that launches the app hands those
            # handles to a process that outlives the session, and the channel
            # then hangs for the full timeout with the work long since done.
            # Redirecting to a file, with stdin from NUL, severs the
            # inheritance. The output is then fetched by a SECOND session:
            # wrapping both in one `cmd /c "..."` looks tidier and silently
            # produces nothing, because the inner quotes break cmd's parsing.
            # cmd.exe reads a forward slash as the start of an option, so the
            # redirection target and `type` both need backslashes — with slashes
            # the file is written nowhere and `type` reports it missing, which
            # reads exactly like a script that printed nothing.
            win = remote.replace("/", "\\")
            log = win + ".out"
            sh = _windows_shell(mid, lambda c, t: _run(ssh_argv(m) + [c], timeout=t))
            p = _run(ssh_argv(m) + [_win_run_cmd(win, log, sh)], timeout=timeout)
            back = _run(ssh_argv(m) + [_win_cat(log, sh)], timeout=120)
            _run(ssh_argv(m) + [_win_del([win, log], sh)], timeout=60)
            return p.returncode, back.stdout, (p.stderr or "") + (back.stderr or "")

        cmd = "sh %s; rc=$?; rm -f %s; exit $rc" % (remote, remote)
        p = _run(ssh_argv(m) + [cmd], timeout=timeout)
        return p.returncode, p.stdout, p.stderr
    finally:
        try:
            os.unlink(local)
        except OSError:
            pass


def push_file(mid, local_path, remote_path, timeout=900):
    """Copy one file to a machine. The installers are 40-120 MB, hence the timeout."""
    m = MACHINES[mid]
    if m["kind"] == "local":
        return remote_path
    if m["kind"] == "wsl-sshpass":
        p = _run(["wsl", "-u", "root", "--", "bash", "-c",
                  "sshpass -p %s scp %s %s %s@%s:%s" % (
                      shlex.quote(m["password"]), " ".join(SSH_OPTS),
                      shlex.quote(_wsl_path(local_path)), m["user"], m["address"],
                      remote_path)], timeout=timeout)
        if p.returncode != 0:
            raise RemoteError("scp %s -> %s: %s" % (local_path, mid, p.stderr.strip()))
        return remote_path
    scp = ["scp"] + SSH_OPTS
    if m.get("key"):
        scp += ["-i", m["key"]]
    scp += [local_path, "%s:%s" % (m["ssh"], remote_path)]
    p = _run(scp, timeout=timeout)
    if p.returncode != 0:
        raise RemoteError("scp %s -> %s: %s" % (local_path, mid, p.stderr.strip()))
    return remote_path


def reachable(mid):
    rc, out, _ = run_script(mid, "echo MW_OK", timeout=60)
    return rc == 0 and "MW_OK" in out


# ── what the host says about itself, from its own loopback ──────────────────
# /api/native/status and /api/internet/status redact everything interesting for
# any caller that is not local, and /api/admin/pin/generate refuses outright.
# That is why these go through the machine's own shell rather than over the LAN.
#
# The port is DISCOVERED, never assumed. 48080/48443 are only the defaults: a
# settings.json decides, a silent uninstall keeps that settings.json, and a
# fresh install therefore inherits whatever was there. The dev instance on this
# very desk answers on 18080/18443. Asking the process which ports it listens
# on, and then asking each of them, is the only reading that cannot be wrong.

_PROBE_PY = r'''
import hashlib, json, os, socket, subprocess, sys, urllib.request

APP = sys.argv[1] if len(sys.argv) > 1 else "MoonlightWebDev"


def listening_ports(pattern):
    """Ports held by a process whose command line matches, by whatever tool is here."""
    pids = set()
    for argv in (["pgrep", "-f", pattern], ["pgrep", pattern]):
        try:
            p = subprocess.run(argv, capture_output=True, text=True, timeout=20)
            pids.update(int(x) for x in p.stdout.split() if x.isdigit())
        except Exception:
            pass
    ports = set()
    for pid in pids:
        for argv in (["lsof", "-nP", "-a", "-p", str(pid), "-iTCP", "-sTCP:LISTEN"],
                     ["ss", "-lntp"]):
            try:
                p = subprocess.run(argv, capture_output=True, text=True, timeout=20)
            except Exception:
                continue
            for line in p.stdout.splitlines():
                if argv[0] == "ss" and ("pid=%d," % pid) not in line:
                    continue
                for tok in line.replace("\t", " ").split():
                    if ":" in tok:
                        tail = tok.rsplit(":", 1)[-1].strip("()[]")
                        if tail.isdigit() and 1 <= int(tail) <= 65535:
                            ports.add(int(tail))
            if ports:
                break
    return sorted(ports)


def open_port(port):
    """A 300 ms socket test. A filtered port can hold an HTTP client for minutes."""
    s = socket.socket()
    s.settimeout(0.3)
    try:
        s.connect(("127.0.0.1", port))
        return True
    except Exception:
        return False
    finally:
        s.close()


def answers(port, scheme="http"):
    if not open_port(port):
        return None
    url = "%s://127.0.0.1:%d/api/native/status" % (scheme, port)
    try:
        ctx = None
        if scheme == "https":
            import ssl
            ctx = ssl.create_default_context()
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
        with urllib.request.urlopen(url, timeout=6, context=ctx) as r:
            body = r.read().decode("utf-8", "replace")
        json.loads(body)
        return body
    except Exception:
        return None


def http(method, port, path, admin=""):
    """One API call. /api/admin/* needs the per-run key, so it is echoed here.

    The key is a CSRF barrier, not a credential: the server mints a fresh one
    every start and hands it to any caller the request guard already trusts.
    Without the header the PIN route answers 403 even on loopback.
    """
    url = "http://127.0.0.1:%d%s" % (port, path)
    req = urllib.request.Request(url, method=method,
                                 data=b"" if method == "POST" else None)
    if admin:
        req.add_header("X-MW-Admin-Key", admin)
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.read().decode("utf-8", "replace")
    except Exception as e:
        return json.dumps({"error": str(e)[:200]})


out = {"appName": APP}
candidates = listening_ports(APP)
# The defaults come last: a discovered port is a fact, a default is a guess.
for guess in (48080, 48443, 18080, 18443, 80, 443):
    if guess not in candidates:
        candidates.append(guess)

http_port = https_port = 0
for port in candidates:
    if not http_port and answers(port, "http"):
        http_port = port
    if not https_port and answers(port, "https"):
        https_port = port
    if http_port and https_port:
        break
out["listening"] = candidates[:12]
out["httpPort"] = http_port
out["httpsPort"] = https_port

if http_port:
    admin = ""
    try:
        admin = json.loads(http("GET", http_port, "/api/admin/token")).get("token", "")
    except Exception:
        pass
    out["adminToken"] = bool(admin)
    out["native"] = http("GET", http_port, "/api/native/status")
    out["internet"] = http("GET", http_port, "/api/internet/status")
    out["vdisplay"] = http("GET", http_port, "/api/native/virtual-display")
    out["pin"] = http("POST", http_port, "/api/admin/pin/generate", admin)
else:
    out["native"] = json.dumps({"error": "nothing answered on any listening port"})

print("MWPROBE " + json.dumps(out))
'''


def _probe_py_for(mid):
    """The same probe everywhere; only the OS-specific extras differ."""
    return _PROBE_PY


def probe(mid):
    """Version in service, autostart state, native status, rendezvous URL, fresh PIN."""
    m = MACHINES[mid]
    pyfile = "mwprobe-%s.py" % uuid.uuid4().hex[:8]

    if m["os"] == "windows":
        remote_py = "C:/Windows/Temp/" + pyfile
        # Windows has no pgrep and may have no python: the port discovery is
        # redone in PowerShell, and the HTTP calls with Invoke-WebRequest.
        script = r"""
$ErrorActionPreference = 'SilentlyContinue'
$app = 'MoonlightWebDev'
$out = @{ appName = $app }

# The HTTPS listener presents the machine's own certificate, which nothing here
# has a reason to trust. Probing whether a port answers is not a security
# decision, so the check is turned off for this script only.
[System.Net.ServicePointManager]::ServerCertificateValidationCallback = { $true }
[System.Net.ServicePointManager]::SecurityProtocol = [System.Net.SecurityProtocolType]::Tls12

$procs = Get-CimInstance Win32_Process -Filter "Name='$app.exe'"
$ids = @($procs | ForEach-Object { $_.ProcessId })
$ports = @()
if ($ids.Count -gt 0) {
    $ports = @(Get-NetTCPConnection -State Listen |
               Where-Object { $ids -contains $_.OwningProcess } |
               ForEach-Object { $_.LocalPort } | Sort-Object -Unique)
}
foreach ($g in @(48080, 48443, 18080, 18443, 80, 443)) { if ($ports -notcontains $g) { $ports += $g } }
# A build under test (MoonlightWeb.exe --dev) beside the installed edition:
# MW_BENCH_LOCAL_PORTS="18080,18443" puts its ports ahead of the edition's.
if ($env:MW_BENCH_LOCAL_PORTS) {
    $ports = @($env:MW_BENCH_LOCAL_PORTS -split ',' | ForEach-Object { [int]$_ }) + $ports
}
$out.listening = $ports | Select-Object -First 12

# Invoke-WebRequest's -TimeoutSec does not bound the TCP connect phase: against
# a port that is filtered rather than closed it can sit for minutes, and six
# candidate ports times two schemes turned a probe into a hang. A 300 ms socket
# test first means only a port that is actually open is ever asked twice.
function Test-Open($port) {
    $c = New-Object System.Net.Sockets.TcpClient
    try {
        if (-not $c.BeginConnect('127.0.0.1', $port, $null, $null).AsyncWaitHandle.WaitOne(300)) {
            return $false
        }
        return $c.Connected
    } catch { return $false } finally { $c.Close() }
}
function Try-Port($port, $scheme) {
    if (-not (Test-Open $port)) { return $null }
    try {
        $r = Invoke-WebRequest -UseBasicParsing -TimeoutSec 6 `
             -Uri "${scheme}://127.0.0.1:$port/api/native/status"
        if ($r.Content -match '^\s*\{') { return $r.Content }
    } catch { }
    return $null
}
$httpPort = 0; $httpsPort = 0
foreach ($p in $ports) {
    if ($httpPort  -eq 0 -and (Try-Port $p 'http'))  { $httpPort  = $p }
    if ($httpsPort -eq 0 -and (Try-Port $p 'https')) { $httpsPort = $p }
    if ($httpPort -ne 0 -and $httpsPort -ne 0) { break }
}
$out.httpPort = $httpPort; $out.httpsPort = $httpsPort

function Get-Api($method, $path, $admin) {
    if ($httpPort -eq 0) { return '{"error":"no port answered"}' }
    # /api/admin/* needs the per-run key the server mints at start: it is a CSRF
    # barrier, and without the header the PIN route answers 403 even on loopback.
    $headers = @{}
    if ($admin) { $headers['X-MW-Admin-Key'] = $admin }
    try {
        $r = Invoke-WebRequest -UseBasicParsing -Method $method -TimeoutSec 20 `
             -Headers $headers -Uri "http://127.0.0.1:$httpPort$path"
        return $r.Content
    } catch { return '{"error":"' + ($_.Exception.Message -replace '"', '') + '"}' }
}
$adminKey = ''
try { $adminKey = (Get-Api GET '/api/admin/token' $null | ConvertFrom-Json).token } catch { }
$out.adminToken = [bool]$adminKey
$out.native   = Get-Api GET  '/api/native/status' $null
$out.internet = Get-Api GET  '/api/internet/status' $null
$out.vdisplay = Get-Api GET  '/api/native/virtual-display' $null
$out.pin      = Get-Api POST '/api/admin/pin/generate' $adminKey

$task = Get-ScheduledTask -TaskName $app -ErrorAction SilentlyContinue
$out.autostart = if ($task) { $task.State.ToString() } else { 'absent' }
$out.running = if ($procs) { 'yes' } else { 'no' }

$log = Join-Path $env:APPDATA "MoonlightWeb\$app\logs\moonlightweb.log"
if (Test-Path $log) {
    # The last "Version:" line is the one the process running now printed at
    # start; the version shown in the UI comes from a CMake cache.
    $line = Select-String -Path $log -Pattern 'Version:' | Select-Object -Last 1
    $out.versionLine = if ($line) { $line.Line.Trim() } else { '' }
    $out.logPath = $log
} else { $out.versionLine = ''; $out.logPath = '' }

$exe = Join-Path $env:ProgramFiles "$app\$app.exe"
if (Test-Path $exe) {
    $out.exeSha = (Get-FileHash $exe -Algorithm SHA256).Hash.Substring(0, 16)
    $out.exePath = $exe
} else { $out.exeSha = ''; $out.exePath = '' }

'MWPROBE ' + ($out | ConvertTo-Json -Compress -Depth 4)
"""
        rc, out, err = run_script(mid, script, timeout=300)

    else:
        extra = ""
        if m["os"] == "macos":
            extra = r"""
p = subprocess.run(["launchctl", "print",
                    "gui/%d/com.moonlightweb.agent.dev" % os.getuid()],
                   capture_output=True, text=True)
out["agentLoaded"] = p.returncode == 0
# The app never calls `launchctl bootstrap` on itself — that would start a
# second copy beside the one already running. It writes the plist and launchd
# reads it at the NEXT login. So the thing that has to exist after an install
# is the file; "not loaded right now" is the normal state, not a failure.
plist = os.path.expanduser("~/Library/LaunchAgents/com.moonlightweb.agent.dev.plist")
out["agentPlist"] = os.path.exists(plist)
out["autostart"] = ("loaded" if out["agentLoaded"]
                    else ("login item" if out["agentPlist"] else "none"))
exe = "/Applications/MoonlightWebDev.app/Contents/MacOS/MoonlightWebDev"
logs = [os.path.expanduser("~/Library/Application Support/MoonlightWeb/MoonlightWebDev/logs/moonlightweb.log"),
        os.path.expanduser("~/Library/Logs/MoonlightWebDev/moonlightweb.log")]
"""
        else:
            extra = r"""
p = subprocess.run(["systemctl", "is-active", "moonlightweb-dev.service"],
                   capture_output=True, text=True)
out["service"] = (p.stdout or p.stderr).strip() or "absent"
d = subprocess.run(["systemctl", "get-default"], capture_output=True, text=True)
out["defaultTarget"] = d.stdout.strip()
# On a desktop distribution the packaging deliberately leaves the system unit
# disabled and starts the app inside the user's session instead
# (make-packages.sh: "postinst enables it only when the machine has no
# desktop"). What has to survive a reboot there is the XDG autostart entry, so
# THAT is the file to judge, not the unit.
desktop = os.path.expanduser("~/.config/autostart/moonlightweb-dev.desktop")
out["autostartDesktop"] = os.path.exists(desktop)
if out["defaultTarget"].startswith("graphical"):
    out["autostart"] = "session autostart" if out["autostartDesktop"] else "none"
else:
    out["autostart"] = out["service"]
exe = ""
for cand in ("/opt/moonlightweb-dev/bin/MoonlightWebDev",
             "/opt/moonlightweb-dev/MoonlightWebDev",
             "/opt/moonlightweb-dev/moonlightweb-dev"):
    if os.path.exists(cand):
        exe = cand
        break
logs = [os.path.expanduser("~/.local/share/MoonlightWeb/MoonlightWebDev/logs/moonlightweb.log"),
        "/var/log/moonlightweb-dev/moonlightweb.log"]
"""
        tail = r"""
q = subprocess.run(["pgrep", "-f", APP], capture_output=True, text=True)
out["running"] = "yes" if q.stdout.strip() else "no"
out["exePath"] = exe if exe and os.path.exists(exe) else ""
if out["exePath"]:
    h = hashlib.sha256()
    with open(exe, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    out["exeSha"] = h.hexdigest()[:16].upper()
else:
    out["exeSha"] = ""
line, found = "", ""
for log in logs:
    if os.path.exists(log):
        found = log
        # The log is appended to, never rotated per run: the LAST "Version:"
        # line is the one the process running right now printed at start. The
        # version string shown in the UI comes from a CMake cache and proves
        # nothing about which binary is actually up.
        with open(log, errors="replace") as f:
            for raw in f:
                if "Version:" in raw:
                    line = raw.strip()
        break
out["versionLine"] = line
out["logPath"] = found
print("MWPROBE " + json.dumps(out))
"""
        body = _PROBE_PY.replace('print("MWPROBE " + json.dumps(out))', extra + tail)
        # caffeinate first: this Mac falls asleep in the middle of an ssh session.
        prefix = "caffeinate -d -i -t 300 >/dev/null 2>&1 </dev/null &\n" if m["os"] == "macos" else ""
        script = prefix + "python3 - MoonlightWebDev <<'MWPY'\n" + body + "\nMWPY\n"
        rc, out, err = run_script(mid, script, timeout=300)

    parsed = {"_rc": rc, "_stderr": (err or "").strip()[:400]}
    for chunk in (out or "").splitlines():
        chunk = chunk.strip()
        if chunk.startswith("MWPROBE "):
            try:
                parsed.update(json.loads(chunk[len("MWPROBE "):]))
            except ValueError:
                parsed["_raw"] = chunk[:400]
    for key in ("native", "internet", "vdisplay", "pin"):
        val = parsed.get(key)
        if isinstance(val, str):
            try:
                parsed[key] = json.loads(val)
            except ValueError:
                parsed[key] = {"error": val[:200]}
    return parsed


_SETTINGS_PS = r"""
$ErrorActionPreference = 'SilentlyContinue'
$port = __PORT__
$body = @'
__BODY__
'@
function Call($method, $path, $admin, $data) {
    $headers = @{}
    if ($admin) { $headers['X-MW-Admin-Key'] = $admin }
    $uri = "http://127.0.0.1:$port$path"
    try {
        if ($data) {
            $r = Invoke-WebRequest -UseBasicParsing -Method $method -TimeoutSec 20 -Headers $headers `
                 -ContentType 'application/json' -Body ([System.Text.Encoding]::UTF8.GetBytes($data)) -Uri $uri
        } else {
            $r = Invoke-WebRequest -UseBasicParsing -Method $method -TimeoutSec 20 -Headers $headers -Uri $uri
        }
        return $r.Content
    } catch { return '{"error":"' + ($_.Exception.Message -replace '"', '') + '"}' }
}
$admin = ''
try { $admin = (Call GET '/api/admin/token' $null $null | ConvertFrom-Json).token } catch { }
$before = Call GET '/api/settings/streaming' $admin $null
$after = Call POST '/api/settings/streaming' $admin $body
'MWSETTINGS ' + (@{ before = $before; after = $after; admin = [bool]$admin } | ConvertTo-Json -Compress -Depth 4)
"""

_SETTINGS_PY = r'''
import json, sys, urllib.request
port, body = int(sys.argv[1]), sys.argv[2]


def call(method, path, admin="", data=None):
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), method=method,
                                 data=data.encode("utf-8") if data is not None else None)
    if admin:
        req.add_header("X-MW-Admin-Key", admin)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.read().decode("utf-8", "replace")
    except Exception as e:
        return json.dumps({"error": str(e)[:200]})


admin = ""
try:
    admin = json.loads(call("GET", "/api/admin/token")).get("token", "")
except Exception:
    pass
before = call("GET", "/api/settings/streaming", admin)
after = call("POST", "/api/settings/streaming", admin, body)
print("MWSETTINGS " + json.dumps({"before": before, "after": after, "admin": bool(admin)}))
'''


def host_settings(mid, http_port, settings):
    """Write streaming settings on the HOST, through its own loopback API.

    Some settings live on the host and not in the browser: the native engine's
    video pipeline (native_video_pipeline) is one, the latency flag another.
    drive.apply_settings() only reaches the client's localStorage, so a pass
    that changes one of these declares it as `hostSettings`, and run.py puts
    the previous values back after the pass with a second call to this.

    Returns the values the given keys had before — only those the host
    reported, so a key it did not know is not "restored" to something it never
    was. Raises RuntimeError when the host refused the change: a pass run with
    a setting silently not applied would measure the wrong thing.
    """
    if not settings:
        return {}
    if not http_port:
        raise RuntimeError("no HTTP port known for %s: cannot reach its settings" % mid)
    body = json.dumps(settings)
    m = MACHINES[mid]
    if m["os"] == "windows":
        script = _SETTINGS_PS.replace("__PORT__", str(int(http_port))).replace(
            "__BODY__", body)
    else:
        script = "python3 - %d %s <<'MWPY'\n%s\nMWPY\n" % (
            int(http_port), shlex.quote(body), _SETTINGS_PY)
    rc, out, err = run_script(mid, script, timeout=120)
    result = None
    for line in (out or "").splitlines():
        line = line.strip()
        if line.startswith("MWSETTINGS "):
            try:
                result = json.loads(line[len("MWSETTINGS "):])
            except ValueError:
                pass
    if not result:
        raise RuntimeError("no answer from %s's settings API (rc %s): %s" % (
            mid, rc, (err or out or "").strip()[:200]))
    try:
        after = json.loads(result.get("after") or "{}")
    except ValueError:
        after = {"error": (result.get("after") or "")[:200]}
    if isinstance(after, dict) and after.get("error"):
        raise RuntimeError("%s refused the host settings %s: %s" % (mid, body, after["error"]))
    try:
        before = json.loads(result.get("before") or "{}")
    except ValueError:
        before = {}
    return {k: before[k] for k in settings if isinstance(before, dict) and k in before}


def https_port_from_here(address, candidates, timeout=4):
    """Which port serves the web UI over TLS, asked from the measuring station.

    Asked from HERE and not from the host, on purpose: this is the port the
    bench client will dial, so the thing worth testing is whether this machine
    can reach it — a host-side answer would still leave a closed firewall
    undiscovered. The certificate is the machine's own and is not checked; the
    question is which port answers, not whether to trust it.
    """
    import http.client
    import ssl
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    for port in candidates:
        try:
            conn = http.client.HTTPSConnection(address, port, timeout=timeout, context=ctx)
            conn.request("GET", "/api/health")
            resp = conn.getresponse()
            resp.read(64)
            conn.close()
            # Any HTTP answer proves a web server is there; 404 counts.
            if resp.status:
                return port
        except Exception:
            continue
    return 0


def lan_url(mid, probe_result):
    """The address this machine answers on, with the port it actually listens on.

    48443 is only the default. A settings.json decides, a silent uninstall
    keeps that settings.json, and a fresh install inherits it — the dev
    instance on this desk answers on 18443.
    """
    probe_result = probe_result or {}
    address = MACHINES[mid]["address"]
    candidates = [p for p in (probe_result.get("listening") or []) if p != probe_result.get("httpPort")]
    for guess in (DEV_HTTPS, 18443, 443):
        if guess not in candidates:
            candidates.append(guess)
    port = probe_result.get("httpsPort") or https_port_from_here(address, candidates) or DEV_HTTPS
    return "https://%s:%d/" % (address, port)


def rendezvous_url(probe_result):
    """The address an owner reconnects on, out of /api/internet/status."""
    rdv = (probe_result.get("internet") or {}).get("rendezvous") or {}
    for key in ("url", "link", "owner_url"):
        if rdv.get(key):
            return rdv[key]
    ident = rdv.get("id") or rdv.get("instance_id") or rdv.get("instanceId")
    host = rdv.get("host") or rdv.get("server") or "stream.dev.moonlightweb.top"
    if ident:
        host = host.replace("https://", "").replace("http://", "").rstrip("/")
        return "https://%s/%s" % (host, ident)
    return ""


def wait_up(mid, timeout=180):
    """Poll the machine's own loopback until the server answers, or give up."""
    end = time.time() + timeout
    while time.time() < end:
        got = probe(mid)
        if isinstance(got.get("native"), dict) and "error" not in got["native"]:
            return got
        time.sleep(10)
    return probe(mid)
