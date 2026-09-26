# ============================================================================
# The Phase 1 campaign of the D3D12 plan (C1.4): mw-d3d12-lab's probes on each
# GPU of the bench, at rest and under mw-gpu-load, with what each one needs:
#   queues   the conversion on D3D11, D3D12 DIRECT and COMPUTE, by priority
#   encode   D3D12 Video Encode (the product's CBR), alone and after the
#            conversion on a DIRECT queue
#   vendors  NVENC / AMF fed D3D12 pictures (RTX / AMD)
#   interop  the DDA handshake, on the GPU's own display showing the band page
#
# Run it twice: from a normal PowerShell (a limited token: the HIGH class) and
# from an elevated one (REALTIME, GLOBAL_REALTIME queues). -Loads external
# measures under whatever already runs (a game started by hand: RE9).
#
#   .\d3d12-lab-campaign.ps1 [-Gpus rtx,arc,amd] [-Loads rest,load]
#                            [-Seconds 12] [-Out <dir>] [-Build <dir>]
#
# mw-gpu-load never runs more than 60 s and stops itself when its GPU runs
# hot, so every probe call below fits one load run of its own: the load
# calibrates to ~45 frames/s on the GPU under test (a game's saturation, the
# same on the three), then the probe runs, then the load stops and the GPU
# gets a few seconds to cool. Pure ASCII on purpose (PowerShell 5.1 reads a
# BOM-less script as ANSI).
# ============================================================================
param(
    [string[]] $Gpus = @('rtx', 'arc', 'amd'),
    [string[]] $Loads = @('rest', 'load'),
    [int] $Seconds = 12,
    [string] $Out = '',
    [string] $Build = ''
)
$ErrorActionPreference = 'Continue'
# powershell -File hands "rtx,arc" over as one string, not as an array.
$Gpus = @($Gpus | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Loads = @($Loads | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
if (-not $Build) { $Build = Join-Path $root 'build-d3d12' }
if (-not $Out) { $Out = Join-Path $root 'bench-out\d3d12v2\g1' }
$lab = Join-Path $Build 'native-host\tools\d3d12-lab\mw-d3d12-lab.exe'
$loadExe = Join-Path $Build 'native-host\tools\gpu-load\mw-gpu-load.exe'
$kiosk = Join-Path $PSScriptRoot 'kiosk.ps1'
$band = 'file:///' + ((Join-Path $PSScriptRoot 'content\scroll.html') -replace '\\', '/') + '?band=1'
$env:PATH = "C:\Qt\6.10.3\msvc2022_64\bin;$env:PATH"
foreach ($f in @($lab, $loadExe)) { if (-not (Test-Path $f)) { throw "missing $f (build with -DMW_BUILD_TOOLS=ON)" } }

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
$token = if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { 'elevated' } else { 'limited' }
$names = @{ rtx = 'RTX'; arc = 'Arc'; amd = 'AMD' }
Write-Host "d3d12-lab campaign: token $token, GPUs $($Gpus -join ','), loads $($Loads -join ','), $Seconds s per variant"

function Start-Load($gpu, $json) {
    $p = Start-Process -FilePath $loadExe -PassThru -ArgumentList @(
        '--gpu', $names[$gpu], '--autostart', '--duration', '60', '--no-music', '--json', $json)
    # Calibration (8 s) and the first steady second.
    Start-Sleep -Seconds 10
    return $p
}

function Stop-Load($p) {
    if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Seconds 6
}

# One probe call, under a load run of its own when asked. Output and JSON go
# to $dir\<name>.txt / .json.
function Probe($gpu, $loadName, $dir, $name, [string[]] $probeArgs) {
    $json = Join-Path $dir "$name.json"
    $txt = Join-Path $dir "$name.txt"
    $loadJson = Join-Path $dir "$name-load.jsonl"
    $p = $null
    if ($loadName -eq 'load') { $p = Start-Load $gpu $loadJson }
    $all = @($probeArgs)
    if ($loadName -eq 'load' -and $probeArgs[0] -eq 'queues') { $all += @('--load-json', $loadJson) }
    if ($probeArgs[0] -ne 'vendors') { $all += @('--json', $json) }
    # UTF-8, not the UTF-16 that *> writes in PowerShell 5.1.
    & $lab @all 2>&1 | Out-File -FilePath $txt -Encoding utf8
    Stop-Load $p
    $summary = [string](Get-Content $txt |
        Where-Object { $_ -match 'refused|wall ms, P|NVENC-D3D12|AMF-DX12|ddasync=gpu|^(d3d11|ps|cs) .*\|' } |
        Select-Object -First 1)
    Write-Host ("  {0,-16} {1}" -f $name, ($summary -replace '\s+', ' '))
}

foreach ($gpu in $Gpus) {
    $n = $names[$gpu]
    foreach ($loadName in $Loads) {
        $dir = Join-Path $Out "$gpu-$loadName-$token"
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        Write-Host "=== $gpu, $loadName, token $token -> $dir"

        # The conversion's queues, one variant per call: under load a late
        # submission waits for the game's frame, so a variant of 12 s can
        # take 30, and a second one would run past the load's 60 s.
        $s = "$Seconds"
        $queueVariants = @(
            @('q-d3d11', 'd3d11', 'high', 'own'),
            @('q-ps-high-own', 'ps', 'high', 'own'),
            @('q-ps-high-default', 'ps', 'high', 'default'),
            @('q-ps-realtime', 'ps', 'realtime', 'own'),
            @('q-cs-realtime', 'cs', 'realtime', 'own'),
            @('q-ps-normal', 'ps', 'normal', 'own'),
            @('q-cs-normal', 'cs', 'normal', 'own'),
            @('q-cs-high-own', 'cs', 'high', 'own'),
            @('q-cs-high-default', 'cs', 'high', 'default'))
        foreach ($q in $queueVariants) {
            Probe $gpu $loadName $dir $q[0] @('queues', '--adapter', $n, '--seconds', $s, '--warmup', '30',
                '--variants', $q[1], '--priorities', $q[2], '--creators', $q[3])
        }

        # The encoder: D3D12 Video Encode alone, then after the conversion.
        # $Seconds of frames: late ones stretch the run the same way.
        Probe $gpu $loadName $dir 'encode' @('encode', '--adapter', $n, '--rc', 'cbr', '--qvs', '0',
            '--seconds', $s)
        Probe $gpu $loadName $dir 'encode-convert' @('encode', '--adapter', $n, '--rc', 'cbr', '--qvs', '0',
            '--seconds', $s, '--convert', 'ps', '--source', '2560x1440')
        if ($gpu -ne 'arc') {
            Probe $gpu $loadName $dir 'vendors' @('vendors', '--adapter', $n, '--frames', "$([Math]::Min(180 * $Seconds / 3, 2400))")
        }

        # The handshake, on this GPU's display showing the band page.
        $rect = & $lab interop --display $n --seconds 1 --modes gpu --delays 0 --no-json 2>&1 |
            Select-String 'goes on (\d+),(\d+),(\d+),(\d+)'
        if ($rect) {
            $m = $rect.Matches[0].Groups
            & powershell -NoProfile -ExecutionPolicy Bypass -File $kiosk -Url $band -X $m[1].Value -Y $m[2].Value `
                -W $m[3].Value -H $m[4].Value | Out-Null
            Start-Sleep -Seconds 4
            Probe $gpu $loadName $dir 'interop' @('interop', '--display', $n, '--seconds', "$Seconds",
                '--modes', 'none,gpu,cpu', '--delays', '0')
        } else {
            Write-Host "  interop skipped: no display on $n"
        }
    }
}

# The band page off the screen again.
Get-CimInstance Win32_Process -Filter "Name='chrome.exe'" | Where-Object { $_.CommandLine -like '*.chrome-bench*' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Write-Host "=== done ($Out)"
