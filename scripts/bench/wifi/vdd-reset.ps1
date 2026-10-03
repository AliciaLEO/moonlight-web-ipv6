# The virtual display left primary by a pass killed mid-way: a short --dev start
# resets it (the host turns it off at start-up), then that instance is stopped.
# The screens are listed at the end (monitors.ps1).
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$out = Join-Path $repo 'bench-out\wifi'
New-Item -ItemType Directory -Force $out | Out-Null
$mon = Join-Path $repo 'scripts\bench\monitors.ps1'
$virtual = { param($m) ($m -join ' ') -match 'DISPLAY([1-9][0-9]{2,})' }
$m = powershell -NoProfile -File $mon
if (& $virtual $m) {
    Start-Process -FilePath (Join-Path $repo 'build\MoonlightWeb.exe') -ArgumentList '--dev', '--log', (Join-Path $out 'dev-reset.log') | Out-Null
    $end = (Get-Date).AddSeconds(90)
    do { Start-Sleep -Seconds 3; $m = powershell -NoProfile -File $mon } while ((& $virtual $m) -and (Get-Date) -lt $end)
    Get-CimInstance Win32_Process -Filter "Name='MoonlightWeb.exe'" | Where-Object { $_.CommandLine -like '*--dev*dev-reset.log*' } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}
$m
