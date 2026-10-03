# Stop a series cleanly, whatever it was doing, and leave DualRTX as it was: the
# series' processes, the --dev instance the passes launch, the tunnels to the
# clients' DevTools, the content kiosk; the virtual display's settings file put
# back; the virtual display turned off by a short --dev start (vdd-reset.ps1).
# Careful: it kills any --dev: check first that no other session runs one.
#   powershell -File stop.ps1
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$ca = Join-Path $repo 'bench-out\content-age'
Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match 'wifi[\/]series\.py|series\.py (mac|n95|um|lx|loc)|local_matrix\.py|pass\.py --tag' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue; "killed $($_.ProcessId)" }
Get-CimInstance Win32_Process -Filter "Name='MoonlightWeb.exe'" | Where-Object { $_.CommandLine -like '*--dev*' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue; "dev killed $($_.ProcessId)" }
Get-CimInstance Win32_Process -Filter "Name='ssh.exe'" | Where-Object { $_.CommandLine -match '-L\S* ?(942[2-5]):127\.0\.0\.1' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue; "tunnel killed $($_.ProcessId)" }
Get-CimInstance Win32_Process -Filter "Name='chrome.exe'" | Where-Object { ($_.CommandLine -like '*--remote-debugging-port=9334*' -or $_.CommandLine -like '*--remote-debugging-port=9353*') -and $_.CommandLine -notlike '*--type=*' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue; "kiosk killed $($_.ProcessId)" }
& wsl.exe -u root -- pkill -f '127.0.0.1:9425:127.0.0.1:9222' 2>$null
$saved = Join-Path $ca 'vdd_settings.saved.xml'
if (Test-Path $saved) {
    Copy-Item $saved C:\VirtualDisplayDriver\vdd_settings.xml
    "xml " + (Get-FileHash C:\VirtualDisplayDriver\vdd_settings.xml -Algorithm SHA256).Hash.Substring(0, 16)
}
& (Join-Path $PSScriptRoot 'vdd-reset.ps1')
exit 0
