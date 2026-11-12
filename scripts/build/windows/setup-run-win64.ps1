# Copyright 2026 GeneralsX contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Stage the Windows x64 build into its OWN run folder and launch it.
# GeneralsX:
#
# WHY A SEPARATE FOLDER: Generals Online runs EAC (Easy Anti-Cheat) against its signed
# exe inside the Steam directory. Dropping our unsigned DXVK d3d8.dll/d3d9.dll beside it
# would at best break Generals Online and at worst flag the account. So the Steam install
# is treated as READ-ONLY game data and nothing is ever written into it.
#
#   C:\dev\GeneralsX-run\      our exe + our DXVK DLLs + our stubs + our logs
#   Steam\...\Zero Hour\       READ-ONLY, untouched, EAC-clean
#
# Verify that claim with scripts/build/windows/steam-manifest.ps1 before and after a run:
# the MANIFEST-SHA256 must be identical.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File setup-run-win64.ps1            # stage only
#   powershell -ExecutionPolicy Bypass -File setup-run-win64.ps1 -Launch    # stage + launch

param(
    [switch]$Launch,
    [int]$Seconds = 60,
    [string]$GameArgs = "-win -xres 1600 -yres 900",
    # The Tauri launcher as package-launcher-windows.sh leaves it (remote-build-windows.ps1:83).
    [string]$LauncherExe = "C:\dev\gx\out\GeneralsXLauncher.exe",
    # Overridable only so the staging can be dry-run against a scratch tree; the defaults are the box.
    [string]$run   = "C:\dev\GeneralsX-run",
    [string]$build = "C:\dev\GeneralsX\build\win64",
    # WHICH DXVK PAIR (dxvk\dxvk-pair.ps1, pinned in dxvk\dxvk-win64.pins):
    # gx = ours, upstream 2.6 + the fixed-function constant fix, built by dxvk\build-dxvk-win64.ps1 - the default, what
    # ships from 0.9.54; official = the DXVK 2.6 release pair every earlier version shipped. That is the way back:
    # restage with -DxvkPair official and package, and the runtime tier returns to the official bytes.
    [ValidateSet('gx', 'official')][string]$DxvkPair = 'gx',
    # Dry runs against a scratch tree only: take d3d8/d3d9 from this folder WITHOUT the pin check (a warning says so;
    # package-windows-share.ps1 still refuses anything but a pinned pair). Empty = the folder -DxvkPair names.
    [string]$dxvk  = "",
    [string]$steam = "C:\Program Files (x86)\Steam\steamapps\common\Command & Conquer Generals - Zero Hour\"
)

# RelWithDebInfo, the config that ships - never Release (scripts/build/verify-4platform.sh records why).
# A stale GeneralsMD\Release exe would be staged over the real one, and a Release build has no generalszh.pdb.
# The win64 build preset builds RelWithDebInfo too.
$zh    = "$build\GeneralsMD\RelWithDebInfo"

# NOTE THE TRAILING BACKSLASH - it is load-bearing, not cosmetic.
# Win32LocalFileSystem::getFileListInDirectory (Win32LocalFileSystem.cpp:131-137) builds its
# search string as originalDirectory + currentDirectory + searchName with NO separator inserted.
# Without the trailing backslash the mask becomes "...Zero Hour*.big", FindFirstFile matches
# nothing, and the engine reports "did not provide BIG files" and then dies loading INI - a
# completely silent, extremely misleading failure mode.
# ($steam is a parameter above; its default carries the backslash.)

# Stage the Tauri launcher (scripts/launcher-rs), built on this box by package-launcher-windows.sh.
# Checked before anything is copied, so a refusal leaves the run folder as it was: a C# or unrecognised exe is fatal,
# no exe is a warning (engine-only staging). A -Setup-<commit9> name that differs from HEAD only warns.
. (Join-Path $PSScriptRoot "launcher-kind.ps1")
# The engine's own inputs, also before anything is copied: a build missing one must not half-replace
# the run folder. OpenAL32.dll is one of them - see where it is staged below.
$openal = "$build\_deps\openal_soft-build\RelWithDebInfo\OpenAL32.dll"
foreach ($f in @("$zh\generalszh.exe", "$zh\generalszh.pdb", "$zh\zlib1.dll", $openal)) {
    if (-not (Test-Path $f)) { throw "no $f - build z_generals with --config RelWithDebInfo from the win64 preset first; nothing staged" }
}
# The DXVK pair, before anything is copied too: an unbuilt or tampered pair must not half-replace the run folder.
. (Join-Path $PSScriptRoot "dxvk\dxvk-pair.ps1")
if ($dxvk -eq "") {
    $dxvk = Get-DxvkPairDefaultDir $DxvkPair
    if (-not (Test-Path -LiteralPath $dxvk)) {
        throw "no DXVK pair '$DxvkPair' at $dxvk - $(if ($DxvkPair -eq 'gx') { 'build it with scripts\build\windows\dxvk\build-dxvk-win64.ps1' } else { 'unpack the official dxvk-2.6.tar.gz there' }); nothing staged"
    }
    $got = Get-DxvkPairName $dxvk    # throws on a missing, unknown or mixed pair
    if ($got -ne $DxvkPair) { throw "$dxvk holds the $got pair, not $DxvkPair; nothing staged" }
    Write-Host "  DXVK pair       : $DxvkPair ($dxvk)"
} else {
    Write-Warning "DXVK taken from $dxvk WITHOUT the pin check (-dxvk is for dry runs) - package-windows-share.ps1 refuses an unpinned pair"
}
$stageLauncher = Test-Path -LiteralPath $LauncherExe
if ($stageLauncher) {
    Assert-TauriLauncher $LauncherExe
} else {
    Write-Warning "no Tauri launcher at $LauncherExe - leaving whatever GeneralsXLauncher.exe is in $run (package-windows-share.ps1 refuses a non-Tauri one)"
}

New-Item -ItemType Directory -Force -Path $run,"$run\logs","$run\shadercache" | Out-Null

Copy-Item "$zh\generalszh.exe" $run -Force
Copy-Item "$zh\generalszh.pdb" $run -Force   # symbols beside the exe for crash triage
Copy-Item "$zh\zlib1.dll"      $run -Force   # vcpkg app-local dependency

if ($stageLauncher) {
    Copy-Item -LiteralPath $LauncherExe $run -Force
    $li = Get-Item (Join-Path $run "GeneralsXLauncher.exe")
    $setupExe = Get-ChildItem -LiteralPath (Split-Path $LauncherExe) -Filter "GeneralsXLauncher-Setup-*.exe" -ErrorAction SilentlyContinue |
                Select-Object -First 1
    $built = if ($setupExe) { $setupExe.BaseName -replace '^GeneralsXLauncher-Setup-', '' } else { "unknown" }
    Write-Host ("  launcher staged : Tauri, built at {0}  {1}  {2} bytes  {3}" -f $built, $li.LastWriteTime, $li.Length, `
        (Get-FileHash $li.FullName -Algorithm SHA256).Hash.Substring(0,16))
    $head = if (Get-Command git -ErrorAction SilentlyContinue) { & git -C $PSScriptRoot rev-parse --short=9 HEAD 2>$null } else { $null }
    if ($head -and $built -ne $head) {
        Write-Warning "launcher was built at $built, this checkout is at $head - rebuild with package-launcher-windows.sh unless that is intended"
    }
}

# Windows 11 x64 ships NO d3d8.dll (only a 32-bit one in SysWOW64), so DXVK's is required. Both from $dxvk, always
# together (dxvk-pair.ps1: d3d8 drives d3d9 through a private interface).
Copy-Item "$dxvk\d3d8.dll" $run -Force
Copy-Item "$dxvk\d3d9.dll" $run -Force

# TheSuperHackers @refactor xezon 09/09/2026 Neither stub is staged any more, and neither stub
# exists. mss32.dll and binkw32.dll used to be x64 DLLs built from source by miles-sdk-stub and
# bink-sdk-stub, deliberately named after the retail 32-bit libraries purely so they satisfied the
# import table - omitting them killed the exe at 0xC0000135 STATUS_DLL_NOT_FOUND before WinMain
# ran, with zero output on stdout and stderr. Dependencies/Miles/milesloader and
# Dependencies/Bink/binkloader LoadLibrary() them instead, after the command line is parsed, so
# there is no import-table entry left to satisfy: a missing DLL now costs audio or video rather
# than the whole process. $build\_deps\miles-build and $build\_deps\bink-build are both gone with
# cmake/miles.cmake and cmake/bink.cmake, so copying from either would fail the script outright.

# Stage OpenAL32.dll: openal-soft builds as a SHARED DLL that vcpkg's applocal pass cannot cover.
# Without it the exe dies at 0xC0000135 STATUS_DLL_NOT_FOUND before WinMain, silently. REQUIRED, and checked
# with the other inputs before anything is copied. Taken from RelWithDebInfo, like the exe.
Copy-Item $openal $run -Force
Write-Host "  staged OpenAL32.dll"

# Stage Data\Scripts. Without it, a match with an AI player desyncs at frame 0 against a non-Windows peer.
# SidesList::prepareForMP_or_Skirmish opens Data\Scripts\SkirmishScripts.scb relative to the working directory; a silent
# open failure skips the shared-RNG draws of the delayed-eval scripts, offsetting the stream permanently.
# Workaround for loose data ignoring CNC_GENERALS_ZH_PATH. SimID cannot catch it: .scb is outside m_iniCRC.
$scriptsSrc = Join-Path $steam "Data\Scripts"
$scriptsDst = "$run\Data\Scripts"
if (Test-Path $scriptsSrc) {
    New-Item -ItemType Directory -Force -Path $scriptsDst | Out-Null
    Copy-Item "$scriptsSrc\*" $scriptsDst -Force
    Write-Host "[setup] staged Data\Scripts ($((Get-ChildItem $scriptsDst).Count) files)"
} else {
    Write-Warning "[setup] $scriptsSrc NOT FOUND - matches with AI players will desync against other platforms"
}

# Stage MapsZH.big: the map cache is scanned relative to the WORKING DIRECTORY, so a run folder without .big files
# indexes only user maps; the joiner then reports "You do not have the map" and the host sees a peer that never accepts.
# Same workaround as Data\Scripts and Data\Cursors; honouring CNC_GENERALS_ZH_PATH in the engine would remove all three.
$mapsSrc = Join-Path $steam "MapsZH.big"
if (Test-Path $mapsSrc) {
    Copy-Item $mapsSrc $run -Force
    Write-Host "[setup] staged MapsZH.big ($([math]::Round((Get-Item "$run\MapsZH.big").Length/1MB,1)) MB)"
} else {
    Write-Warning "[setup] $mapsSrc NOT FOUND - built-in maps will be missing and cross-platform joins will fail on map availability"
}

# Data\INI\Weapon.ini (Toxin Tractor balance change) in the RUN folder: go.bat launches from $run and a relative loose
# file is opened first, so a peer without it computes a dataID every packaged peer refuses (DATA_DIFFERS).
# Cut by make-weapon-ini.py --root from the CoreINI this peer mounts (asset root, then overlay); CNC_GENERALS_ZH_PATH
# is set for the call as go.bat does. No archive is a warning; a refused archive is fatal. Idempotent.
$pyExe = $null
$pyArgs = @()
foreach ($cand in @(
        [pscustomobject]@{ Exe = "python";  Pre = @() },
        [pscustomobject]@{ Exe = "python3"; Pre = @() },
        [pscustomobject]@{ Exe = "py";      Pre = @("-3") })) {
    if (-not (Get-Command $cand.Exe -ErrorAction SilentlyContinue)) { continue }
    $probeArgs = $cand.Pre + @("--version")
    & $cand.Exe @probeArgs > $null 2>&1
    if ($LASTEXITCODE -eq 0) { $pyExe = $cand.Exe; $pyArgs = $cand.Pre; break }
}
if ($pyExe) {
    $weaponArgs = $pyArgs + @((Join-Path $PSScriptRoot "..\..\tooling\make-weapon-ini.py"), "--root", $run, "--if-archive")
    $savedAssetRoot = $env:CNC_GENERALS_ZH_PATH
    $env:CNC_GENERALS_ZH_PATH = $steam
    try { & $pyExe @weaponArgs; $weaponExit = $LASTEXITCODE } finally { $env:CNC_GENERALS_ZH_PATH = $savedAssetRoot }
    if ($weaponExit -ne 0) { throw "generating $run\Data\INI\Weapon.ini failed (exit $weaponExit)" }
} else {
    Write-Warning "[setup] no working python on PATH - Data\INI\Weapon.ini NOT generated; every peer carrying it will refuse this one (DATA_DIFFERS)"
}

# Present with MAILBOX, not FIFO (fullscreen FIFO ran at exactly 60 it/s on a 60 Hz panel).
# This heredoc is the source of every Windows dxvk.conf: the updater overwrites {app}\dxvk.conf, so hand edits are lost.
#   presentInterval = 0 sets sync interval 0; tearFree = True then asks for MAILBOX only (FIFO if absent): no tearing.
# 16x anisotropy is overridden per launch by W3DDisplay::init; the line stays so the runtime tier is unchanged.
@"
# Logs and shader cache are pinned inside the run folder so nothing lands in Steam.
dxvk.logLevel = info

# MAILBOX present: no vsync wait, no tearing (falls back to FIFO where the driver lacks it).
d3d9.presentInterval = 0
dxvk.tearFree = True

# 16x anisotropic filtering: the RTS camera sees the ground at a grazing angle.
d3d9.samplerAnisotropy = 16
"@ | Set-Content "$run\dxvk.conf" -Encoding ASCII

# alsoft.ini beside generalszh.exe, which OpenAL Soft reads from the executable's folder. A Bluetooth headset whose mic is in use drops to its hands-free profile and
# WASAPI grants a 16 kHz mono mix; OpenAL Soft's default resampler then aliases the bright UI clicks into a high-pitched sound. bsinc24 avoids that, in menus and in game.
# The engine's [GXAUDIO] line names the resampler in use, which is how a log shows this file was read.
@"
[general]
resampler = bsinc24
"@ | Set-Content "$run\alsoft.ini" -Encoding ASCII

# Quoting the whole assignment keeps the '&' in the Steam path literal under cmd.
@"
@echo off
set "CNC_GENERALS_ZH_PATH=$steam"
set "DXVK_LOG_PATH=$run\logs"
set "DXVK_STATE_CACHE_PATH=$run\shadercache"
set "DXVK_CONFIG_FILE=$run\dxvk.conf"
cd /d $run
$run\generalszh.exe %* 2> logs\err.txt
echo GAME EXIT: %errorlevel% >> logs\err.txt
"@ | Set-Content "$run\go.bat" -Encoding ASCII

Write-Output "=== staged into $run ==="
Get-ChildItem $run -File | Select-Object Name,Length | Format-Table -AutoSize

if (-not $Launch) { return }

# A GUI + Vulkan app must run in the interactive session: over SSH it lands in session 0, where DXVK finds no adapters.
# A transient scheduled task with /it reaches that session; it is deleted below.
# Only the game THIS launch started is stopped (Generals Online runs generalszh.exe too): the task's command line
# carries a token, and Ours THROWS when the process table or a path is unreadable, so "none" never means "cannot tell".
$tok = "gxrun-$PID-$((Get-Date).ToString('yyyyMMddHHmmss'))"
$tokRe = '(?<![A-Za-z0-9._-])' + [regex]::Escape($tok) + '(?![A-Za-z0-9._-])'
function Ours {
    $all = @(Get-CimInstance Win32_Process -ErrorAction Stop)
    $mine = @($all | Where-Object { $_.Name -eq 'generalszh.exe' } | Where-Object {
        $q = $_; $hit = $false
        for ($i = 0; $i -lt 5 -and $q -and -not $hit; $i++) {
            $hit = [bool]($q.CommandLine -and $q.CommandLine -match $tokRe)
            $pp = $q.ParentProcessId; $born = $q.CreationDate   # an ancestor is older: a younger one reused the pid
            $q = $all | Where-Object { $_.ProcessId -eq $pp -and -not ($born -and $_.CreationDate -and $_.CreationDate -gt $born) } |
                Select-Object -First 1
        }
        $hit })
    if (@($mine | Where-Object { -not $_.ExecutablePath }).Count -gt 0) { throw "a game of this launch has no readable path" }
    @($mine | Where-Object { $_.ExecutablePath.StartsWith($run.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) } |
        ForEach-Object { Get-Process -Id $_.ProcessId -ErrorAction SilentlyContinue })
}
Write-Output ("PRE count: " + @(Get-Process generalszh -ErrorAction SilentlyContinue).Count + " generalszh on this box, any folder")
schtasks /create /tn GXRun /tr "cmd /c set GXRUN=$tok&& $run\go.bat $GameArgs" /sc once /st 23:59 /f /it /ru $env:USERNAME | Out-Null
schtasks /run /tn GXRun | Out-Null

Start-Sleep -Seconds $Seconds
# The game is looked up ONCE, while the task's cmd - the ancestor carrying $tok - is still up, and from then on
# followed by pid and start time: ending the task below takes that ancestor away.
$game = @(); $known = $true
try { $game = @(Ours) } catch { $known = $false; Write-Output "UNKNOWN: this launch's game cannot be told ($_) - nothing is stopped" }
if ($known) {
    if ($game.Count -gt 0) {
        Write-Output ("RUNNING cpu=" + [math]::Round($game[0].TotalProcessorTime.TotalSeconds,2) +
                      "s ws=" + [math]::Round($game[0].WorkingSet64/1MB,1) + "MB")
    } else {
        Write-Output "EXITED on its own"
    }
}
$game | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
schtasks /end /tn GXRun 2>$null | Out-Null
schtasks /delete /tn GXRun /f | Out-Null
# A kill that killed nothing looks identical to one that worked - always confirm the count: the same processes,
# by pid AND start time (a pid reused since is not ours).
if ($known) {
    $left = @($game | Where-Object { $x = $_; $n = Get-Process -Id $x.Id -ErrorAction SilentlyContinue; $n -and $n.StartTime -eq $x.StartTime }).Count
    Write-Output ("POST count: " + $left)
} else {
    Write-Output "POST count: UNKNOWN"
}

if (Test-Path "$run\logs\err.txt") {
    $a = Get-Content "$run\logs\err.txt"
    Write-Output ("log lines: " + $a.Count)
    Write-Output "--- subsystems initialised ---"
    ($a | Select-String -Pattern "initSubsystem\(.*\) END").Count
    Write-Output "--- errors ---"
    # \bERROR\b deliberately: a bare "ERROR" pattern also matches WW3D_ERROR_OK, which is a
    # SUCCESS line, and reports a phantom error on a completely clean boot.
    ($a | Select-String -Pattern "\bERROR\b|FATAL|CRASH|err:").Count
}
