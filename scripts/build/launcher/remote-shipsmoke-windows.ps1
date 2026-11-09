# GeneralsX launcher packaging - install the NSIS setup, launch what it installed, uninstall.
#
# "Installs and then opens no window" is the failure smoke-launch.sh's own header names as the one
# that ships silently, and it is most likely on exactly this artifact: the loose exe already ran,
# so the only new thing being tested is what the installer did to it.
#
# /S is NSIS's silent switch. The Tauri NSIS template installs PER USER under
# %LOCALAPPDATA%\Programs\<productName> and writes an HKCU uninstall key - which is where the real
# install location is read from, rather than assumed.
param(
    [Parameter(Mandatory=$true)][string]$SetupPath,
    [Parameter(Mandatory=$true)][string]$Product,
    [Parameter(Mandatory=$true)][string]$BundleId
)
$ErrorActionPreference = "Stop"

if (-not (Test-Path $SetupPath)) { throw "no setup exe at $SetupPath" }

# REFUSE TO ACT ON AN INSTALL THAT IS ALREADY THERE. The silent install writes the HKCU uninstall key and
# %LOCALAPPDATA%\Programs\<product>, and the caller then runs that product's uninstall.exe /S: on a box where the
# owner has the launcher installed under this name both would act on THEIR install. Measured 01/10/2026: the
# Windows PC has "GeneralsX Launcher" registered, so this smoke cannot run there until that is a throwaway machine.
# Both key names are checked, as the post-install lookup below accepts either.
$preKeys = @($Product, $BundleId) | ForEach-Object { Join-Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall" $_ }
$preDir = Join-Path (Join-Path $env:LOCALAPPDATA "Programs") $Product
$preFound = @($preKeys | Where-Object { Test-Path $_ }) + @($preDir | Where-Object { Test-Path $_ })
if ($preFound.Count -gt 0) {
    throw "REFUSED: ${Product} is already installed here ($($preFound -join '; ')); the silent install and uninstall would act on that install."
}

# THE CALL OPERATOR DOES NOT WAIT FOR THIS, AND THE PLAN SAID IT WOULD. Measured 2026-08-20:
# `& $SetupPath /S` returned instantly and left $LASTEXITCODE UNSET - the failure printed as
# "the NSIS setup exited " with an empty number - and nothing was installed. PowerShell only waits
# on a CONSOLE-subsystem process; an NSIS installer is GUI-subsystem, so the call operator launches
# it and returns, the ssh session ends, and the child dies with it. The project's standing rule
# "never Start-Process over ssh" is about GUI apps that need a desktop to render on; /S renders
# nothing, so -Wait -PassThru is the right tool here and is the only form that yields an exit code.
#
# The exit code is still not the gate. It is reported, and then the uninstall key and the installed
# exe are what the run is actually judged on, because an installer that exits 0 having written
# nothing is exactly the failure this whole file exists to catch.
$proc = Start-Process -FilePath $SetupPath -ArgumentList '/S' -Wait -PassThru
$rc = $proc.ExitCode
Write-Host ("SETUP EXITCODE {0}" -f $rc)
if ($rc -ne 0) { throw "the NSIS setup exited $rc" }

# THE KEY IS NAMED FOR THE PRODUCT, NOT THE IDENTIFIER, AND THE PLAN SAYS OTHERWISE. Measured
# 2026-08-20 against tauri-cli 2.11.4: the Tauri NSIS template writes
# HKCU:\...\Uninstall\<productName>, so looking $BundleId up finds nothing and the run reports
# "the setup exited 0 but wrote no uninstall key" about an install that completed perfectly.
# $BundleId is still tried second, so a future template that switches to the identifier is picked
# up rather than mis-reported - and which one answered is printed, because a silent switch between
# them is exactly the kind of thing that should not be discovered by a player.
#
# EXACT KEY NAMES ONLY. This box also carries HKCU\...\Uninstall\{8F3C1D42-...}_is1 for the OLD
# Inno-packaged "GeneralsX" launcher, and further down this script an uninstaller gets run. A
# fuzzy match on "Generals" here would uninstall somebody else's product.
$root = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall"
$key = $null
foreach ($candidate in @($Product, $BundleId)) {
    $p = Join-Path $root $candidate
    if (Test-Path $p) { $key = $p; Write-Host ("UNINSTALLKEY {0}" -f $candidate); break }
}
if (-not $key) {
    throw ("GATE FAIL: the setup exited 0 but wrote no uninstall key under $root as either " +
           "'$Product' or '$BundleId' - nothing was installed")
}

# AND THE VALUE ARRIVES QUOTED. The template writes InstallLocation as "C:\...\GeneralsX Launcher"
# WITH the double quotes inside the string value, so Test-Path on it fails on a directory that is
# certainly there. Trimming is not cosmetic; without it every path built from $loc is wrong.
$loc = (Get-ItemProperty -Path $key).InstallLocation
if ($loc) { $loc = $loc.Trim().Trim('"').TrimEnd('\') }
if (-not $loc -or -not (Test-Path $loc)) {
    throw "GATE FAIL: the uninstall key names InstallLocation '$loc', which does not exist"
}
$installed = Join-Path $loc ("{0}.exe" -f $Product)
if (-not (Test-Path $installed)) { throw "GATE FAIL: no $installed after a silent install" }

Write-Host ("INSTALLED {0}" -f $installed)
Write-Host ("INSTALLDIR {0}" -f $loc)
