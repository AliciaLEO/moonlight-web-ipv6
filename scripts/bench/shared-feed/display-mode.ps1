# Set, or read, the mode of a VIRTUAL display by its GDI name (hard_cases.py,
# case B). Never a physical screen: a mode change can take one off the desktop
# for good (memory dualrtx-vdd-display-tests).
#   powershell -File display-mode.ps1 -Device \\.\DISPLAY333 [-W 2224 -H 1440 -Hz 120]
# Prints "<rc or read> <W>x<H>x<Hz>" - rc 0 is DISP_CHANGE_SUCCESSFUL.
param([string] $Device, [int] $W = 0, [int] $H = 0, [int] $Hz = 0)
. (Join-Path (Split-Path $PSScriptRoot -Parent) 'hdr-switch.ps1')
if ($W -gt 0) { $rc = [PassHdrMode]::Set($Device, $W, $H, $Hz); Start-Sleep -Milliseconds 500 } else { $rc = 'read' }
"$rc $(([PassHdrMode]::Get($Device)) -join 'x')"
