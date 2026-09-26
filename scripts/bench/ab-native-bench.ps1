# ============================================================================
# Two arms of the native bench, alternated, on one display.
#
# An arm is a build of the engine, or a build plus a few extra spec keys:
#
#   ref=E:\...\ref-bin\MoonlightWeb.exe
#   d3d12=E:\...\build-d3d12\MoonlightWeb.exe|pipeline=d3d12
#
# Each round runs every arm once on the same content, in an order that flips
# every round (A B, B A, A B, B A): whatever drifts on the machine - heat, a
# background task, the content itself - lands on both arms alike. One pass is
# one run-matrix.ps1 call, so the statistics are the matrix's own.
#
#   .\ab-native-bench.ps1 -Display 2 -KioskRect '5120,0,2560,1440' `
#       -Arms 'ref=<exe>;new=<exe>' -Spec 'codec=hevc,fps=60,width=1920,height=1080'
#       [-Rounds 4] [-Base 'seconds=12,bitrate=20000'] [-Content scroll|still|none]
#       [-ResultsDir <dir>]
#
# Arms are ONE string separated by ';' - `powershell -File` flattens an array
# argument into positional ones, and a spec is full of commas already.
#
# The summary judges the first arm against each other one on the criteria of
# gate G0 (plan pipeline-video-d3d12-v2 section 5): host total within 0.2 ms on
# average and p99 no more than 10 % worse; frames per second within 1 %;
# bytes per frame and average QP within 3 %. It says which criterion failed -
# a verdict is for the reader to take or leave, never a reason to stop.
# ============================================================================
param(
    [Parameter(Mandatory = $true)] [int] $Display,
    [Parameter(Mandatory = $true)] [string] $Arms,
    [Parameter(Mandatory = $true)] [string] $Spec,
    [int] $Rounds = 4,
    [string] $Base = 'seconds=12,bitrate=20000',
    [ValidateSet('scroll', 'still', 'none')] [string] $Content = 'scroll',
    [string] $KioskRect = '',
    [string] $ResultsDir = "$PSScriptRoot\results-ab",
    [int] $SettleMs = 3500
)

$ErrorActionPreference = 'Stop'
if ($Content -ne 'none' -and -not $KioskRect) {
    throw "-KioskRect '<x,y,w,h>' of the captured display is needed to put the content on it"
}

$armList = @()
foreach ($raw in ($Arms -split ';' | Where-Object { $_.Trim() })) {
    $eq = $raw.IndexOf('=')
    if ($eq -le 0) { throw "an arm is label=exe[|extra keys]: $raw" }
    $label = $raw.Substring(0, $eq).Trim()
    $rest = $raw.Substring($eq + 1)
    $bar = $rest.IndexOf('|')
    $exe = if ($bar -ge 0) { $rest.Substring(0, $bar).Trim() } else { $rest.Trim() }
    $extra = if ($bar -ge 0) { $rest.Substring($bar + 1).Trim() } else { '' }
    if (-not (Test-Path $exe)) { throw "arm $label : no exe at $exe" }
    $armList += [pscustomobject]@{ label = $label; exe = $exe; extra = $extra }
}
if ($armList.Count -lt 1) { throw "no arm given" }

New-Item -ItemType Directory -Force -Path $ResultsDir | Out-Null
$contentUrl = if ($Content -ne 'none') {
    'file:///' + ((Join-Path $PSScriptRoot "content\$Content.html") -replace '\\', '/')
} else { $null }

$rows = @()
for ($round = 1; $round -le $Rounds; $round++) {
    # A B, then B A: the arm that goes first alternates.
    $order = if ($round % 2 -eq 1) { $armList } else { @($armList)[($armList.Count - 1)..0] }
    foreach ($arm in $order) {
        $passSpec = if ($arm.extra) { "$Spec,$($arm.extra)" } else { $Spec }
        $dir = Join-Path $ResultsDir ("r{0}-{1}" -f $round, $arm.label)
        Write-Host ("[round {0}/{1}] {2}: {3}" -f $round, $Rounds, $arm.label, $passSpec)
        $matrixArgs = @{
            Display = $Display; Specs = @($passSpec); Base = $Base
            Exe = $arm.exe; ResultsDir = $dir
        }
        if ($contentUrl) {
            $matrixArgs.Relaunch = $true
            $matrixArgs.ContentUrl = $contentUrl
            $matrixArgs.KioskRect = $KioskRect
            $matrixArgs.SettleMs = $SettleMs
        }
        & "$PSScriptRoot\run-matrix.ps1" @matrixArgs | Out-Null
        $csv = Join-Path $dir 'native-bench.csv'
        if (-not (Test-Path $csv)) {
            Write-Warning "  no matrix row for $($arm.label), round $round"
            continue
        }
        $row = Import-Csv $csv | Select-Object -First 1
        $row | Add-Member -NotePropertyName arm -NotePropertyValue $arm.label
        $row | Add-Member -NotePropertyName round -NotePropertyValue $round
        if ($row.error) { Write-Warning "  $($arm.label): $($row.error)" }
        else {
            Write-Host ("  total {0} / p99 {1} ms   {2} fps   {3} KB   QP {4}" -f `
                $row.totalMean, $row.totalP99, $row.captureFps, $row.deltaKB, $row.avgQp)
        }
        $rows += $row
    }
}

$all = Join-Path $ResultsDir 'ab-rows.csv'
$rows | Export-Csv -Path $all -NoTypeInformation -Encoding UTF8

# PowerShell writes numbers in the machine's culture (a decimal comma here):
# read them back the same way.
function Num($v) {
    if ($null -eq $v -or "$v" -eq '') { return $null }
    return [double]::Parse("$v", [Globalization.CultureInfo]::CurrentCulture)
}
function Mean($values) {
    $v = @($values | Where-Object { $null -ne $_ })
    if ($v.Count -eq 0) { return $null }
    return ($v | Measure-Object -Average).Average
}

$summary = @()
foreach ($arm in $armList) {
    $mine = @($rows | Where-Object { $_.arm -eq $arm.label -and -not $_.error })
    $summary += [pscustomobject]@{
        arm        = $arm.label
        passes     = $mine.Count
        totalMean  = Mean ($mine | ForEach-Object { Num $_.totalMean })
        totalP99   = Mean ($mine | ForEach-Object { Num $_.totalP99 })
        encodeMean = Mean ($mine | ForEach-Object { Num $_.encodeMean })
        encodeP99  = Mean ($mine | ForEach-Object { Num $_.encodeP99 })
        fps        = Mean ($mine | ForEach-Object { Num $_.captureFps })
        deltaKB    = Mean ($mine | ForEach-Object { Num $_.deltaKB })
        avgQp      = Mean ($mine | ForEach-Object { Num $_.avgQp })
        gpuClass   = (@($mine | ForEach-Object { $_.gpuClass } | Sort-Object -Unique) -join '/')
    }
}

Write-Host ''
$summary | Format-Table arm, passes, totalMean, totalP99, encodeMean, encodeP99, fps, deltaKB, avgQp, gpuClass -AutoSize

# The gate G0 criteria, first arm against each other one.
function Pct($a, $b) { if ($null -eq $a -or $null -eq $b -or $a -eq 0) { return $null } ; return 100.0 * ($b - $a) / $a }
$ref = $summary[0]
$verdicts = @()
foreach ($other in @($summary | Select-Object -Skip 1)) {
    $checks = [ordered]@{
        'host total mean within 0.2 ms' = ($null -ne $ref.totalMean -and $null -ne $other.totalMean -and
                                          [math]::Abs($other.totalMean - $ref.totalMean) -le 0.2)
        'host total p99 at most +10 %'  = ((Pct $ref.totalP99 $other.totalP99) -le 10)
        'frames per second within 1 %'  = ([math]::Abs((Pct $ref.fps $other.fps)) -le 1)
        'bytes per frame within 3 %'    = ([math]::Abs((Pct $ref.deltaKB $other.deltaKB)) -le 3)
        'average QP within 3 %'         = ($null -eq $ref.avgQp -or [math]::Abs((Pct $ref.avgQp $other.avgQp)) -le 3)
    }
    $failed = @($checks.Keys | Where-Object { -not $checks[$_] })
    $line = "{0} against {1}: {2}" -f $other.arm, $ref.arm,
        $(if ($failed.Count -eq 0) { 'all G0 criteria met' } else { 'NOT met: ' + ($failed -join '; ') })
    Write-Host $line
    $verdicts += $line
}
$summaryPath = Join-Path $ResultsDir 'ab-summary.csv'
$summary | Export-Csv -Path $summaryPath -NoTypeInformation -Encoding UTF8
Set-Content -Path (Join-Path $ResultsDir 'ab-verdict.txt') -Value $verdicts -Encoding UTF8
Write-Host "rows: $all"
Write-Host "summary: $summaryPath"
