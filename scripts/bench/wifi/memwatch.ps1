# Stop a series when DualRTX runs short of commit (a pass was killed for it on
# 03/10/2026 at 06:35; 97 GB of 102 committed that morning). Ends by itself
# when no series.py runs any more.
#   powershell -File memwatch.ps1 [-MinFreeGB 1.5]
param([double]$MinFreeGB = 1.5)
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$log = Join-Path $repo 'bench-out\wifi\memwatch.log'
while ($true) {
    $free = (Get-CimInstance Win32_OperatingSystem).FreeVirtualMemory / 1MB
    Add-Content $log ((Get-Date -Format 'HH:mm:ss ') + ('{0:N2} GB commit free' -f $free))
    if ($free -lt $MinFreeGB) {
        Add-Content $log 'LOW: stopping the series'
        & (Join-Path $PSScriptRoot 'stop.ps1') | Out-Null
        break
    }
    if (-not (Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match 'series\.py (mac|n95|um|lx|loc)' })) { break }
    Start-Sleep -Seconds 10
}
