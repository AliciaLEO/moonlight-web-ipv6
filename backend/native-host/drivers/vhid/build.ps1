# MoonlightWeb — builds "MoonlightWeb Virtual HID", the HID passthrough's Windows driver.
# Copyright (C) 2026 Bruno Martin. GPLv3.
#
# WinUHid's UMDF2 driver over Windows' Virtual HID Framework (third_party/winuhid, MIT), under our
# own names, with the engine's descriptor validation inside (DescriptorCheck.cpp).
#
# Nothing is installed on the machine: the WDK and the SDK come from NuGet (packages.config, restored
# into packages/), and the WDK tools are called one by one — tracewpp, cl, link, stampinf, Inf2Cat,
# signtool — instead of through the WDK's MSBuild toolset, which needs Visual Studio's WDK component.
# Needs MSVC (Build Tools) for cl and link; run it from any PowerShell, it loads vcvars itself.
#
#   .\build.ps1                 build and test-sign into build\package\
#   .\build.ps1 -Configuration Debug
#
# Signing: a self-signed certificate made once with openssl into build\testcert\ (never committed).
# Installing it needs a machine in test mode (bcdedit /set testsigning on), with the certificate in
# its LocalMachine Root and TrustedPublisher stores; install.ps1 does that part.

param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [string]$Version = '0.1.0.0'
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$kit = '10.0.28000.2526'
$kitDir = '10.0.28000.0'
$umdf = '2.15' # the inbox VHF UMDF source driver's version: Windows 10 2004 and up
$pkgs = Join-Path $here 'packages'
$sdk = Join-Path $pkgs "Microsoft.Windows.SDK.CPP.$kit\c"
$sdkX64 = Join-Path $pkgs "Microsoft.Windows.SDK.CPP.x64.$kit\c"
$wdk = Join-Path $pkgs "Microsoft.Windows.WDK.x64.$kit\c"
$winuhid = Resolve-Path (Join-Path $here '..\..\third_party\winuhid\WinUHid Driver')
$engineSrc = Resolve-Path (Join-Path $here '..\..\src')
$out = Join-Path $here 'build'
$obj = Join-Path $out "obj\$Configuration"
$package = Join-Path $out 'package'

function Step($what) { Write-Host "== $what" }
function Run($exe, [string[]]$arguments) {
    # Windows PowerShell turns a tool's stderr into errors; only its exit code counts here.
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $exe @arguments 2>&1 | ForEach-Object { "$_" } | Where-Object { $_ -notmatch '^[.+*]+$' } | Out-Host
    $code = $LASTEXITCODE
    $ErrorActionPreference = $saved
    if ($code -ne 0) { throw "$([IO.Path]::GetFileName($exe)) failed ($code)" }
}

# ── Toolchain ────────────────────────────────────────────────────────────────
if (-not (Test-Path $wdk)) {
    Step 'NuGet restore (WDK and SDK, about 1.5 GB once)'
    $nuget = Join-Path $here '.tools\nuget.exe'
    if (-not (Test-Path $nuget)) {
        New-Item -ItemType Directory -Force (Split-Path $nuget) | Out-Null
        Invoke-WebRequest https://dist.nuget.org/win-x86-commandline/latest/nuget.exe -OutFile $nuget
    }
    Run $nuget @('restore', (Join-Path $here 'packages.config'), '-PackagesDirectory', $pkgs, '-NonInteractive')
}

# cl and link from the newest MSVC, with vcvars' environment taken into this process.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'no MSVC found (Visual Studio Build Tools with the C++ workload)' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
# (vcvars looks for a vswhere.exe on PATH and complains on stderr when there is none: harmless.)
cmd /c "`"$vcvars`" >nul 2>nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item "env:$($matches[1])" $matches[2] }
}
# Headers and libraries of the NuGet kit, not of whatever SDK the machine has.
$env:INCLUDE = @(
    (Join-Path $wdk "Include\wdf\umdf\$umdf"),
    (Join-Path $wdk "Include\$kitDir\um"),
    (Join-Path $wdk "Include\$kitDir\shared"),
    (Join-Path $sdk "Include\$kitDir\um"),
    (Join-Path $sdk "Include\$kitDir\shared"),
    (Join-Path $sdk "Include\$kitDir\ucrt"),
    ($env:INCLUDE -split ';' | Where-Object { $_ -match 'MSVC' })
) -join ';'
$env:LIB = @(
    (Join-Path $wdk "Lib\wdf\umdf\x64\$umdf"),
    (Join-Path $wdk "Lib\$kitDir\um\x64"),
    (Join-Path $sdkX64 'um\x64'),
    (Join-Path $sdkX64 'ucrt\x64'),
    ($env:LIB -split ';' | Where-Object { $_ -match 'MSVC' })
) -join ';'

New-Item -ItemType Directory -Force $obj, $package | Out-Null

# ── WPP: WinUHid.c includes the WinUHid.tmh tracewpp writes ─────────────────
Step 'tracewpp'
$sdkBin = Join-Path $sdk "bin\$kitDir"
Run (Join-Path $sdkBin 'x64\tracewpp.exe') @(
    "-cfgdir:$(Join-Path $sdkBin 'WppConfig\Rev1')",
    "-scan:$(Join-Path $winuhid 'Trace.h')",
    "-odir:$obj", '-um', '-dll',
    (Join-Path $winuhid 'WinUHid.c'))

# ── Compile and link ─────────────────────────────────────────────────────────
Step "cl ($Configuration)"
$common = @('/nologo', '/c', '/W4', '/WX-', '/Zi', '/guard:cf', '/GS', '/Gy', '/Zc:wchar_t',
    "/Fd$obj\\", "/Fo$obj\\", "/I$obj", "/I$winuhid", "/I$engineSrc",
    '/DUNICODE', '/D_UNICODE', '/D_WINDLL', '/D_WIN64', '/D_AMD64_', '/DAMD64',
    '/DUMDF_VERSION_MAJOR=2', "/DUMDF_VERSION_MINOR=$($umdf.Split('.')[1])", '/DMOONLIGHTWEB_VHID',
    # From UMDF 2.15, WPP_INIT_TRACING takes the driver object and the registry path, as the WDK's
    # toolset sets up. C4005: the WPP header includes ntstatus.h after windows.h, which redefines
    # the handful of STATUS_ codes winnt.h has, to the same values.
    '/DWPP_MACRO_USE_KM_VERSION_FOR_UM', '/wd4005', '/wd4324')
if ($Configuration -eq 'Release') { $common += @('/O2', '/MT', '/DNDEBUG') } else { $common += @('/Od', '/MTd', '/DDBG=1') }
Run cl.exe ($common + @('/TC', (Join-Path $winuhid 'WinUHid.c')))
Run cl.exe ($common + @('/TP', '/std:c++17', '/EHsc', (Join-Path $here 'DescriptorCheck.cpp'),
        (Join-Path $engineSrc 'input\HidDescriptor.cpp')))

Step 'link'
$dll = Join-Path $package 'MoonlightWebVHid.dll'
Run link.exe @('/nologo', '/DLL', '/DEBUG', '/guard:cf', '/CETCOMPAT', '/DYNAMICBASE', '/NXCOMPAT',
    '/OPT:REF', '/OPT:ICF', "/OUT:$dll", "/PDB:$out\MoonlightWebVHid.pdb", "/IMPLIB:$obj\MoonlightWebVHid.lib",
    "$obj\WinUHid.obj", "$obj\DescriptorCheck.obj", "$obj\HidDescriptor.obj",
    'WdfDriverStubUm.lib', 'VhfUm.lib', 'ntdll.lib', 'kernel32.lib', 'advapi32.lib')

# ── INF and catalog ─────────────────────────────────────────────────────────
Step 'stampinf'
$inf = Join-Path $package 'MoonlightWebVHid.inf'
Copy-Item (Join-Path $here 'MoonlightWebVHid.inf') $inf -Force
Run (Join-Path $wdk "bin\$kitDir\x64\stampinf.exe") @('-f', $inf, '-d', '*', '-a', 'amd64',
    '-u', "$umdf.0", '-v', $Version)

Step 'Inf2Cat'
Remove-Item (Join-Path $package 'MoonlightWebVHid.cat') -ErrorAction SilentlyContinue
Run (Join-Path $wdk "bin\$kitDir\x86\Inf2Cat.exe") @("/driver:$package", '/os:10_X64', '/uselocaltime')

# ── Test signature ──────────────────────────────────────────────────────────
Step 'test signature'
$certDir = Join-Path $out 'testcert'
$pfx = Join-Path $certDir 'MoonlightWebVHid-test.pfx'
$cer = Join-Path $certDir 'MoonlightWebVHid-test.cer'
$passFile = Join-Path $certDir 'pfx-password.txt'
if (-not (Test-Path $pfx)) {
    New-Item -ItemType Directory -Force $certDir | Out-Null
    $openssl = (Get-Command openssl -ErrorAction SilentlyContinue).Source
    if (-not $openssl) { $openssl = 'C:\Program Files\Git\usr\bin\openssl.exe' }
    $pass = [Guid]::NewGuid().ToString('N')
    Set-Content -Path $passFile -Value $pass -Encoding ascii
    $key = Join-Path $certDir 'key.pem'
    $pem = Join-Path $certDir 'cert.pem'
    # A config of our own: openssl's default marks a self-signed certificate CA:TRUE, and
    # Windows refuses a CA certificate as a driver's signer (TRUST_E_BASIC_CONSTRAINTS).
    $cnf = Join-Path $certDir 'openssl.cnf'
    Set-Content -Path $cnf -Encoding ascii -Value @(
        '[req]', 'distinguished_name = dn', 'x509_extensions = ext', 'prompt = no',
        '[dn]', 'CN = MoonlightWeb Virtual HID (test)',
        '[ext]', 'basicConstraints = critical, CA:FALSE', 'keyUsage = critical, digitalSignature',
        'extendedKeyUsage = codeSigning', 'subjectKeyIdentifier = hash')
    Run $openssl @('req', '-x509', '-config', $cnf, '-newkey', 'rsa:3072', '-sha256', '-days', '3650',
        '-nodes', '-keyout', $key, '-out', $pem)
    Run $openssl @('pkcs12', '-export', '-inkey', $key, '-in', $pem, '-out', $pfx, '-passout', "pass:$pass")
    Run $openssl @('x509', '-in', $pem, '-outform', 'der', '-out', $cer)
    Remove-Item $key
}
$pass = (Get-Content $passFile -Raw).Trim()
$signtool = Join-Path $sdkBin 'x64\signtool.exe'
foreach ($f in @($dll, (Join-Path $package 'MoonlightWebVHid.cat'))) {
    Run $signtool @('sign', '/q', '/fd', 'sha256', '/f', $pfx, '/p', $pass, $f)
}
Copy-Item $cer $package -Force

Step "done: $package"
Get-ChildItem $package | Format-Table Name, Length -AutoSize
