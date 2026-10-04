# The client's half of the click → photon bench (POC Ultra U0.4), without a
# camera: clicks into a streaming client's window and times, on this machine's
# own screen, how long the host's flag-window.ps1 takes to flip there. The same
# measure for any client: Steam Remote Play (HEVC or PyroWave), MoonlightWeb in
# Chrome.
#
#   powershell -NoProfile -File click-photon.ps1 -Process streaming_client -Clicks 60 -Out steam-hevc.json
#   powershell -NoProfile -File click-photon.ps1 -Process chrome -Title MoonlightWeb -Out mw.json
#
# A click is injected (SendInput) at the window's centre, then the pixel there
# is read back from the composed desktop (GDI, the DWM's last composed frame)
# until its brightness moves by more than half the range. Time: the click's
# send to the first changed read, QueryPerformanceCounter. What it includes:
# the way up, the host's flip, its capture, encode, network, decode, the
# client's present and the DWM's composition, plus up to one read interval
# (~1 ms). What it leaves out: the screen's own scan-out and response, the same
# for every client on one screen.
# The mouse must stay still meanwhile; the window must not be covered.
param(
    [string] $Process = 'streaming_client',
    [string] $Title = '',
    [int] $Clicks = 60,
    [int] $IntervalMs = 700,
    [int] $TimeoutMs = 1500,
    # The pixel read, from the click: MoonlightWeb's bench flag
    # (latency_flag_enabled) is drawn where the click lands and would hide it.
    [int] $ReadDx = 0,
    [int] $ReadDy = 0,
    [string] $Out = ''
)
Add-Type @"
using System; using System.Diagnostics; using System.Runtime.InteropServices; using System.Threading;
public static class Photon {
  [DllImport("user32.dll")] static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr h);
  [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr h, IntPtr dc);
  [DllImport("gdi32.dll")] static extern uint GetPixel(IntPtr dc, int x, int y);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] i, int size);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] struct MOUSEINPUT { public int dx, dy; public uint data, flags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public MOUSEINPUT mi; }
  public static void Init() { SetProcessDpiAwarenessContext((IntPtr)(-4)); }
  public static int Luma(int x, int y) {
    IntPtr dc = GetDC(IntPtr.Zero); uint c = GetPixel(dc, x, y); ReleaseDC(IntPtr.Zero, dc);
    int r = (int)(c & 0xff), g = (int)((c >> 8) & 0xff), b = (int)((c >> 16) & 0xff);
    return (r * 3 + g * 6 + b) / 10;
  }
  static void Click() {
    var i = new INPUT[2];
    i[0].type = 0; i[0].mi.flags = 0x0002;  // LEFTDOWN
    i[1].type = 0; i[1].mi.flags = 0x0004;  // LEFTUP
    SendInput(2, i, Marshal.SizeOf(typeof(INPUT)));
  }
  // One click; ms until the pixel at (x, y) moved past half the range, or -1.
  public static double Once(int x, int y, int rx, int ry, int timeoutMs) {
    SetCursorPos(x, y);
    Thread.Sleep(30);
    int before = Luma(rx, ry);
    long t0 = Stopwatch.GetTimestamp();
    Click();
    long end = t0 + (long)timeoutMs * Stopwatch.Frequency / 1000;
    while (Stopwatch.GetTimestamp() < end) {
      if (Math.Abs(Luma(rx, ry) - before) > 127)
        return (Stopwatch.GetTimestamp() - t0) * 1000.0 / Stopwatch.Frequency;
    }
    return -1;
  }
}
"@
[Photon]::Init()
$procs = Get-Process -Name $Process -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 }
if ($Title) { $procs = $procs | Where-Object { $_.MainWindowTitle -like "*$Title*" } }
$p = $procs | Select-Object -First 1
if (-not $p) { throw "no window of process '$Process'$(if ($Title) { " titled *$Title*" })" }
$r = New-Object Photon+RECT
[void][Photon]::GetWindowRect($p.MainWindowHandle, [ref]$r)
$x = [int](($r.L + $r.R) / 2); $y = [int](($r.T + $r.B) / 2)
"window '$($p.MainWindowTitle)' ($($p.ProcessName) $($p.Id)) at $($r.L),$($r.T)-$($r.R),$($r.B); clicks at $x,$y, reads at $($x + $ReadDx),$($y + $ReadDy)"
[void][Photon]::SetForegroundWindow($p.MainWindowHandle)
Start-Sleep -Milliseconds 500
$samples = New-Object System.Collections.Generic.List[double]
$missed = 0
for ($i = 0; $i -lt $Clicks; $i++) {
    $ms = [Photon]::Once($x, $y, $x + $ReadDx, $y + $ReadDy, $TimeoutMs)
    if ($ms -lt 0) { $missed++ } else { $samples.Add([math]::Round($ms, 2)) }
    Start-Sleep -Milliseconds ([math]::Max(50, $IntervalMs - [int][math]::Max(0, $ms)))
}
$sorted = @($samples | Sort-Object)
function Pct($p) { if ($sorted.Count) { $sorted[[math]::Min($sorted.Count - 1, [int][math]::Floor($p * $sorted.Count))] } else { $null } }
$summary = [ordered]@{
    process = $p.ProcessName; title = $p.MainWindowTitle; clicks = $Clicks; measured = $sorted.Count; missed = $missed
    medianMs = (Pct 0.5); p90Ms = (Pct 0.9); minMs = $(if ($sorted.Count) { $sorted[0] }); maxMs = $(if ($sorted.Count) { $sorted[-1] })
    samples = $samples; at = (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')
}
"click -> photon: $($summary.measured) of $Clicks measured, median $($summary.medianMs) ms, p90 $($summary.p90Ms) ms ($($summary.minMs) to $($summary.maxMs)), $missed missed"
if ($Out) { $summary | ConvertTo-Json -Depth 3 | Set-Content -Path $Out -Encoding utf8 }
