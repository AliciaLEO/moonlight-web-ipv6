# ============================================================================
# The encoder half of a campaign: --native-bench, one pass per spec.
#
# --native-bench captures, converts and encodes a display into a sink — no
# network, no browser — and writes one CSV row per frame. It is the only
# instrument that isolates the encoder from everything else, it runs on every
# platform, and a pass costs ten seconds. Every campaign starts here, and the
# browser passes then say what the rest of the chain adds.
#
#   .\run-matrix.ps1 -Display 1 -Specs @('codec=hevc','codec=h264','codec=av1')
#                    [-Base 'seconds=10,bitrate=20000']
#                    [-Exe ..\..\build\MoonlightWeb.exe]
#                    [-Relaunch -ContentUrl <url> -KioskRect '<x,y,w,h>']
#                    [-ResultsDir <path>] [-HdrDevice \\.\DISPLAYn]
#
# -Relaunch restarts the content kiosk before every pass and waits -SettleMs, so
# each pass encodes the SAME seconds of the same footage. Without it, comparing
# two settings compares two different explosions.
#
# The statistics are computed from the per-frame CSV, never scraped from the
# summary text: the columns are a contract (NativeBench.cpp), the prose is not.
# What the engine says about itself — capture API, hardware GPU scheduling, the
# GPU priority class it got, and the video pipeline once there is more than one
# — is read from its own log lines, per pass, and lands in the same row.
# ============================================================================
param(
    [Parameter(Mandatory = $true)] [int] $Display,
    # A spec is itself comma-separated ("codec=hevc,fps=60"), and `powershell
    # -File` flattens an array argument into positional ones — so a caller in
    # another process passes -SpecFile, one spec per line, and only an in-process
    # caller uses -Specs.
    [string[]] $Specs,
    [string] $SpecFile,
    [string] $Base = 'seconds=10,bitrate=20000',
    [string] $Exe = "$PSScriptRoot\..\..\build\MoonlightWeb.exe",
    [string] $ResultsDir = "$PSScriptRoot\results",
    [switch] $Relaunch,
    [string] $ContentUrl,
    [string] $KioskRect = '',
    [int] $SettleMs = 3500,
    # GDI name of the captured (physical) screen: the hdr=1 spec is then played
    # with Windows HDR on there, and the screen is put back as it was after it.
    [string] $HdrDevice = ''
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\hdr-switch.ps1"
if ($SpecFile) {
    if (-not (Test-Path $SpecFile)) { throw "no spec file at $SpecFile" }
    $Specs = @(Get-Content $SpecFile -Encoding UTF8 | Where-Object { $_.Trim() })
}
if (-not $Specs -or $Specs.Count -eq 0) { throw "give -Specs or -SpecFile" }
if (-not (Test-Path $Exe)) { throw "no MoonlightWeb.exe at $Exe — build it first" }
$csvDir = Join-Path $ResultsDir 'native-bench'
New-Item -ItemType Directory -Force -Path $csvDir | Out-Null

# Bench rule: hardware GPU scheduling stays on (docs/bench-campaign.md). A queue
# priority measured under the OS scheduler says little about the GPU's own, and
# a campaign that silently ran with HAGS switched off would compare the two. The
# switch is HwSchMode: 1 is off, 2 is on, and absent is the driver's default —
# which the engine's own log line then states, per GPU, in every row.
$hwsch = (Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\GraphicsDrivers' `
          -Name HwSchMode -ErrorAction SilentlyContinue).HwSchMode
if ($hwsch -eq 1) {
    throw "hardware GPU scheduling (HAGS) is switched off on this machine: the bench runs with HAGS on only"
}

function Get-Percentile {
    param([double[]] $Values, [double] $P)
    if ($Values.Count -eq 0) { return $null }
    $sorted = $Values | Sort-Object
    $i = [Math]::Min($sorted.Count - 1, [Math]::Max(0, [Math]::Ceiling($P * $sorted.Count) - 1))
    return [double]$sorted[$i]
}

# Every row has the SAME shape, always. Export-Csv takes its header from the
# FIRST object it is given and silently drops any property the later ones add —
# so a failed pass whose object carried only `spec` and `error` came out as a
# row of empty cells, and the failure vanished from the campaign. A bench that
# loses its failures is worse than no bench.
function New-Row {
    param([string] $Spec)
    [ordered]@{
        spec = $Spec; error = ''; negotiated = ''; csv = ''
        frames = $null; keyframes = $null; captureFps = $null
        encodeMean = $null; encodeP95 = $null; encodeP99 = $null
        totalMean = $null; totalP99 = $null
        deltaKB = $null; deltaKBp95 = $null; avgQp = $null
        # Added for the D3D12 pipeline campaigns; appended, so a reader that
        # takes columns by name keeps working.
        convertMean = $null; convertP50 = $null; convertP99 = $null
        encodeP50 = $null; totalP50 = $null
        pipeline = ''; capture = ''; hags = ''; gpuClass = ''
        # What the captured display produced, whatever the stream carried:
        # with a game as the content, the game's own frame rate, up to the
        # refresh.
        presents = $null; folded = $null; displayFps = $null
    }
}

# What the engine logged about the conditions of the pass. Its log lines are
# prose, so only the few that carry a fact are matched, and an absent line
# leaves the cell empty rather than guessed.
function Read-EngineFacts {
    param([string[]] $Lines)
    $hags = @($Lines | ForEach-Object {
        if ($_ -match 'hardware GPU scheduling \(HAGS\) (.+?), for the (\w+) device') {
            "$($Matches[2]) $($Matches[1])"
        }
    })
    # The class the process ended up with: the LAST line, because a REALTIME
    # refused is followed by the HIGH that was granted instead.
    $class = @($Lines | Where-Object { $_ -match 'GPU scheduling class (REALTIME|HIGH|ABOVE_NORMAL)' }) |
             Select-Object -Last 1
    $gpuClass = ''
    if ($class -and $class -match 'GPU scheduling class (\w+)') {
        $gpuClass = $Matches[1]
        if ($class -match 'refused|needs') { $gpuClass = '' }
        if ($class -match 'ABOVE_NORMAL instead') { $gpuClass = 'ABOVE_NORMAL' }
    }
    # The display's rate over the capture loop's span: the presents the loop
    # woke for, plus those an acquire folded in while the loop was busy
    # converting or encoding. The stream's cadence takes no part in it.
    $presents = $null; $span = $null; $folded = $null
    foreach ($line in $Lines) {
        if ($line -match 'cadence: .+ (\d+) presents in ([\d.]+) s') {
            $presents = [int]$Matches[1]
            $span = [double]::Parse($Matches[2], [Globalization.CultureInfo]::InvariantCulture)
        }
        if ($line -match 'capture loop: (\d+) presents folded') { $folded = [int]$Matches[1] }
    }
    $displayFps = $null
    if ($null -ne $presents -and $span -gt 0) {
        $displayFps = [math]::Round(($presents + [int]$folded) / $span, 1)
    }
    return @{ hags = ($hags -join '; '); gpuClass = $gpuClass
              presents = $presents; folded = $folded; displayFps = $displayFps }
}

function Measure-Pass {
    param([string] $CsvPath)
    $rows = Import-Csv -Path $CsvPath
    # Re-sent frames carry no capture and would drag every average down: the
    # engine stamps present, captured and submitted with the same instant for
    # them, and the bench flags that as captured=0.
    $captured = @($rows | Where-Object { $_.captured -eq '1' })
    if ($captured.Count -eq 0) { return $null }
    $deltas = @($captured | Where-Object { $_.keyframe -eq '0' })

    $encode = [double[]]@($captured | ForEach-Object { [double]$_.encode_us / 1000.0 })
    $convert = [double[]]@($captured | ForEach-Object { [double]$_.convert_us / 1000.0 })
    $total = [double[]]@($captured | ForEach-Object { [double]$_.host_total_us / 1000.0 })
    $bytes = [double[]]@($deltas | ForEach-Object { [double]$_.bytes })
    $qp = [double[]]@($captured | Where-Object { [double]$_.avg_qp -gt 0 } |
            ForEach-Object { [double]$_.avg_qp })

    # Presents over the span BETWEEN presents: n-1 intervals, not n, and
    # measured on the present stamps alone. Mixing t0 with the last t3 would
    # fold one frame's encode time into the period and quietly lower the rate.
    $spanUs = [double]$captured[-1].t0_present_us - [double]$captured[0].t0_present_us
    $fps = if ($spanUs -gt 0) { ($captured.Count - 1) / ($spanUs / 1e6) } else { $null }

    return [ordered]@{
        frames      = $captured.Count
        keyframes   = @($captured | Where-Object { $_.keyframe -eq '1' }).Count
        captureFps  = if ($fps) { [math]::Round($fps, 1) } else { $null }
        encodeMean  = [math]::Round(($encode | Measure-Object -Average).Average, 2)
        encodeP95   = [math]::Round((Get-Percentile $encode 0.95), 2)
        encodeP99   = [math]::Round((Get-Percentile $encode 0.99), 2)
        totalMean   = [math]::Round(($total | Measure-Object -Average).Average, 2)
        totalP99    = [math]::Round((Get-Percentile $total 0.99), 2)
        deltaKB     = if ($bytes.Count) { [math]::Round(($bytes | Measure-Object -Average).Average / 1024, 1) } else { $null }
        deltaKBp95  = if ($bytes.Count) { [math]::Round((Get-Percentile $bytes 0.95) / 1024, 1) } else { $null }
        avgQp       = if ($qp.Count) { [math]::Round(($qp | Measure-Object -Average).Average, 1) } else { $null }
        # On the D3D11 path convert_us is the CPU submission only — the GPU
        # work of the conversion lands in encode_us (plan §1.1). Compare paths
        # on totalMean / totalP99, never on convert alone.
        convertMean = [math]::Round(($convert | Measure-Object -Average).Average, 2)
        convertP50  = [math]::Round((Get-Percentile $convert 0.50), 2)
        convertP99  = [math]::Round((Get-Percentile $convert 0.99), 2)
        encodeP50   = [math]::Round((Get-Percentile $encode 0.50), 2)
        totalP50    = [math]::Round((Get-Percentile $total 0.50), 2)
    }
}

$results = @()
$index = 0
foreach ($spec in $Specs) {
    $index++
    $label = ($spec -replace '[^A-Za-z0-9=,]', '') -replace '[=,]', '-'
    $csv = Join-Path $csvDir "pass-$index-$label.csv"
    $full = "display=$Display,$Base,$spec,out=$csv"

    if ($Relaunch -and $ContentUrl) {
        if (-not $KioskRect) { throw "-Relaunch needs -KioskRect '<x,y,w,h>'; run-campaign derives it from the captured display" }
        $r = $KioskRect -split ','
        & powershell -NoProfile -File "$PSScriptRoot\kiosk.ps1" -Url $ContentUrl `
            -X ([int]$r[0]) -Y ([int]$r[1]) -W ([int]$r[2]) -H ([int]$r[3]) | Out-Null
        Start-Sleep -Milliseconds $SettleMs
    }

    Write-Host "[$index/$($Specs.Count)] $spec"
    # The one pass that needs HDR gets it on the captured screen, and gives
    # it back whatever happens (hdr-switch.ps1).
    $hdrPass = $HdrDevice -and ($spec -match '(^|,)hdr=1(,|$)')
    $hdrWas = if ($hdrPass) { Enter-PassHdr $HdrDevice } else { $null }
    try {
        # Start-Process, not `& $Exe`: Windows PowerShell 5.1 turns EVERY line a
        # native command writes to stderr into an ErrorRecord — with or without a
        # redirection — and under $ErrorActionPreference='Stop' the matrix then dies
        # on the engine's first informational line ("AMF confirmed ..."). Redirecting
        # through Start-Process keeps both streams as plain files, which is also
        # where they are wanted when a pass has to be explained afterwards.
        $errPath = [IO.Path]::ChangeExtension($csv, '.err')
        $outPath = [IO.Path]::ChangeExtension($csv, '.out')
        Start-Process -FilePath $Exe -ArgumentList @('--native-bench', $full) `
            -NoNewWindow -Wait -RedirectStandardOutput $outPath -RedirectStandardError $errPath
        # -Encoding UTF8 for the same reason as the stderr read below: the engine
        # writes UTF-8, and without it Get-Content decodes as ANSI and the middot
        # of the negotiated line lands in the report as a double-encoded 'A-tilde'.
        $stdout = if (Test-Path $outPath) { Get-Content $outPath -Raw -Encoding UTF8 } else { '' }

        $row = New-Row $spec
        $row.csv = $csv

        if (-not (Test-Path $csv)) {
            # The line that EXPLAINS, not the last four lines: the engine logs a
            # dozen informational lines before it gives up, and quoting the tail
            # puts "colour conversion: ..." in the report where the driver's refusal
            # belongs. -Encoding UTF8 because the engine writes UTF-8 and Get-Content
            # would otherwise read it as ANSI and mangle every dash.
            $errLines = if (Test-Path $errPath) { @(Get-Content $errPath -Encoding UTF8) } else { @() }
            $said = @($errLines | Where-Object {
                $_ -match 'could not|failed|unsupported|not supported|refused|error'
            })
            $tail = if ($said.Count) { ($said | Select-Object -Last 2) -join ' ' }
                    elseif ($errLines.Count) { ($errLines | Select-Object -Last 2) -join ' ' }
                    else { ($stdout -split "`n" | Select-Object -Last 2) -join ' ' }
            # An engine that never started writes NOTHING to either stream, and
            # every branch above then yields an empty array rather than a string:
            # calling .Trim() on it threw and took the whole matrix down with it,
            # hiding the one fact that mattered — the exe did not run. A missing
            # Qt runtime beside a bare payload .exe looks exactly like this.
            $tail = (@($tail) -join ' ').Trim()
            if (-not $tail) {
                $tail = "the engine wrote nothing to stdout or stderr — it did not start (missing runtime beside $Exe?)"
            }
            Write-Warning "  no CSV produced — the pass failed: $tail"
            $row.error = $tail
            $results += [pscustomobject]$row
            continue
        }

        $stats = Measure-Pass $csv
        if (-not $stats) {
            Write-Warning "  the pass produced no captured frame"
            $row.error = 'no captured frame'
            $results += [pscustomobject]$row
            continue
        }
        # The engine's own description of what it really did — encoder, codec,
        # chroma and HDR as NEGOTIATED, which is not always what was asked for.
        $negotiated = ($stdout -split "`n" | Where-Object { $_ -match '^native-bench: ' } |
                       Select-Object -First 1) -replace '^native-bench: ', ''
        $row.negotiated = (@($negotiated) -join ' ').Trim()
        # Fields of the negotiated line are separated by a middot, and a value
        # can hold spaces ("DXGI Desktop Duplication").
        if ($row.negotiated -match '· capture (.+?)( · |$)') { $row.capture = $Matches[1].Trim() }
        if ($row.negotiated -match '· pipeline (.+?)( · |$)') { $row.pipeline = $Matches[1].Trim() }
        $facts = Read-EngineFacts $(if (Test-Path $errPath) { @(Get-Content $errPath -Encoding UTF8) } else { @() })
        $row.hags = $facts.hags
        $row.gpuClass = $facts.gpuClass
        $row.presents = $facts.presents
        $row.folded = $facts.folded
        $row.displayFps = $facts.displayFps
        foreach ($k in $stats.Keys) { $row[$k] = $stats[$k] }
        $results += [pscustomobject]$row
        Write-Host ("  convert {0} / {1}   encode {2} / {3} / {4}   total {5} / {6} ms   {7} KB   QP {8}   {9} fps" -f `
            $stats.convertMean, $stats.convertP99, $stats.encodeMean, $stats.encodeP50, $stats.encodeP99,
            $stats.totalMean, $stats.totalP99, $stats.deltaKB, $stats.avgQp, $stats.captureFps)
        Write-Host ("  capture {0}   GPU class {1}   HAGS {2}   display {3} fps" -f `
            $row.capture, $row.gpuClass, $row.hags, $row.displayFps)
    } finally {
        if ($hdrPass) { Exit-PassHdr $HdrDevice $hdrWas }
    }
}

$out = Join-Path $ResultsDir 'native-bench.csv'
$results | Export-Csv -Path $out -NoTypeInformation -Encoding UTF8
Write-Host ''
Write-Host "matrix written to $out"
$results | Format-Table spec, convertMean, encodeMean, encodeP99, totalMean, totalP50, totalP99, deltaKB, avgQp, captureFps, gpuClass -AutoSize
