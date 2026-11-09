# Stage the Windows x64 build into its OWN run folder and launch it.
# GeneralsX @build Claude 27/07/2026
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
    # GeneralsX @feature Claude 03/10/2026 WHICH DXVK PAIR (dxvk\dxvk-pair.ps1, pinned in dxvk\dxvk-win64.pins):
    # gx = ours, upstream 2.6 + the fixed-function constant fix, built by dxvk\build-dxvk-win64.ps1 - the default, what
    # ships from 0.9.54; official = the DXVK 2.6 release pair every earlier version shipped. That is the way back:
    # restage with -DxvkPair official and package, and the runtime tier returns to the official bytes.
    [ValidateSet('gx', 'official')][string]$DxvkPair = 'gx',
    # Dry runs against a scratch tree only: take d3d8/d3d9 from this folder WITHOUT the pin check (a warning says so;
    # package-windows-share.ps1 still refuses anything but a pinned pair). Empty = the folder -DxvkPair names.
    [string]$dxvk  = "",
    [string]$steam = "C:\Program Files (x86)\Steam\steamapps\common\Command & Conquer Generals - Zero Hour\"
)

# GeneralsX @bugfix Claude 25/09/2026 RelWithDebInfo, the config that ships - never Release.
#
# This read GeneralsMD\Release from 27/07/2026, when that was the build config. The shipping config
# became RelWithDebInfo on 24/08/2026 (scripts/build/verify-4platform.sh records why), and nothing
# here followed: on the build box GeneralsMD\Release still holds an 8.0 MB exe from 24/08, so every
# run of this script since would have staged a month-old Release binary over the real one, and a
# Release build has no generalszh.pdb at all. The win64 build preset builds RelWithDebInfo too.
$zh    = "$build\GeneralsMD\RelWithDebInfo"

# NOTE THE TRAILING BACKSLASH - it is load-bearing, not cosmetic.
# Win32LocalFileSystem::getFileListInDirectory (Win32LocalFileSystem.cpp:131-137) builds its
# search string as originalDirectory + currentDirectory + searchName with NO separator inserted.
# Without the trailing backslash the mask becomes "...Zero Hour*.big", FindFirstFile matches
# nothing, and the engine reports "did not provide BIG files" and then dies loading INI - a
# completely silent, extremely misleading failure mode.
# ($steam is a parameter above; its default carries the backslash.)

# GeneralsX @bugfix Claude 24/09/2026 STAGE THE TAURI LAUNCHER, NOT THE C# LOADER.
#
# This used to `dotnet publish` scripts/build/windows/loader and stage that as GeneralsXLauncher.exe
# (the 17/08 fix: never ship a stale launcher). The published win32 tier has carried the Tauri
# launcher (scripts/launcher-rs) since the runtime-tier switch, so every run of this script put the
# retired C# loader back into the folder package-windows-share.ps1 zips. The C# source stays in the
# tree, unstaged; nothing in packaging builds it any more.
#
# Build the Tauri one with scripts/build/launcher/package-launcher-windows.sh (it must build ON this
# box - the bundler is MSVC-only). Checked BEFORE anything is copied, so a refusal leaves the run
# folder exactly as it was:
#   * a C# (or unrecognised) exe at $LauncherExe is fatal - that is the bug this fixes;
#   * NO exe there is a warning, as a missing loader project was: an engine-only staging must not
#     need a launcher build (remote-build-windows.ps1 wipes C:\dev\gx\out first). The packager
#     still refuses anything but the Tauri launcher, so this cannot ship the wrong one.
# Staleness: remote-build-windows.ps1 leaves GeneralsXLauncher-Setup-<commit9>.exe beside the exe,
# so the commit it was built at is printed and compared with this checkout's HEAD. A difference is
# a warning, not a refusal - RUNBOOK_2026-08-24_ATOMIC_SHIP.md ships launchers built at another
# commit on purpose - but it is now on screen instead of silent.
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

# GeneralsX @feature Claude 06/08/2026 Stage OpenAL32.dll when the build has the OpenAL backend.
#
# cmake/openal.cmake FetchContents openal-soft 1.24.2 and never sets LIBTYPE, so openal-soft's
# SHARED default applies and the artifact is a DLL rather than a static lib - confirmed in our own
# tree, where the same FetchContent produces _deps/openal_soft-build/libopenal.1.24.2.dylib on
# macOS. vcpkg's applocal pass cannot cover it because vcpkg.json excludes openal-soft on Windows,
# so nothing else stages it.
#
# The failure if this is missing is the same one the comment above describes for mss32.dll:
# 0xC0000135 STATUS_DLL_NOT_FOUND before WinMain, no output on stdout or stderr. That reads as a
# broken build rather than a missing file, which is why it is worth staging unconditionally-if-
# present rather than leaving it to whoever wires the backend up.
#
# GeneralsX @bugfix Claude 25/09/2026 REQUIRED now, and from the exe's own config. The win64 preset
# sets SAGE_USE_OPENAL (25/09/2026), so every build it makes imports OpenAL32.dll, and "stage it if it
# happens to be there" skipped it without a word whenever it was not at the path looked in - leaving
# an exe that dies at 0xC0000135. It is checked
# with the other inputs, BEFORE anything is copied (see $openal above), so a refusal leaves the run
# folder as it was. RelWithDebInfo, like the exe: building z_generals in that config is what builds
# this DLL, whereas _deps\openal_soft-build\Release is only refreshed by a separate Release build of
# the OpenAL target, and an older one there predates whatever openal-soft patch the exe was built with.
Copy-Item $openal $run -Force
Write-Host "  staged OpenAL32.dll"

# GeneralsX @bugfix Claude 28/07/2026 Stage Data\Scripts. WITHOUT THIS, ANY MATCH CONTAINING AN AI
# PLAYER DESYNCS AT FRAME 0 AGAINST A NON-WINDOWS PEER.
#
# SidesList::prepareForMP_or_Skirmish (SidesList.cpp:520) loads "Data\Scripts\SkirmishScripts.scb"
# by a path RELATIVE TO THE WORKING DIRECTORY when the map supplies no skirmish scripts of its own.
# The run folder is the working directory and had no Data\Scripts, so on Windows that open failed -
# silently, because the only diagnostics for it sit behind #ifdef ALLOW_DEBUG_UTILS, which is off in
# release. The Mac's working directory IS its game folder, so the Mac loaded the file fine.
#
# Consequence, measured by replaying one .rep headless on both platforms:
#   Windows  ScriptEngine::newMap  delayDraws=0   seed unchanged  2805577022 -> 2805577022
#   Mac      ScriptEngine::newMap  delayDraws=91  seed changed    2805577022 -> 1560412358
# Every delayed-eval script draws once from the SHARED simulation RNG to jitter its first evaluation
# frame (ScriptEngine.cpp:6892). 91 draws on one peer and 0 on the other offsets the stream
# permanently, and the first visible symptom is every starting unit placed at the same radius from
# its Command Center but a DIFFERENT ANGLE.
#
# It is AI-only because skirmish scripts are attached only for skirmish/AI players, so an AI-free
# match has zero scripts on both peers and stays in sync - which is why 74,705-frame human-only
# matches were clean while every AI match died at frame 0.
#
# Before staging: 13,919 of 13,919 frames differed. After: 0 differed. Same binaries, same replay.
#
# NOTE this is a WORKAROUND at the same layer as the Data\Cursors one. The real bug is that loose
# data files are opened by a working-directory-relative path that ignores CNC_GENERALS_ZH_PATH -
# the same defect as Win32Mouse.cpp:376. Fixing the engine's path resolution would cover both.
#
# NOTE ALSO that SimID cannot catch this: .scb files are not INI, so they are outside m_iniCRC.
# Two peers can present identical engine/source/data/ordinal IDs and still simulate differently.
$scriptsSrc = Join-Path $steam "Data\Scripts"
$scriptsDst = "$run\Data\Scripts"
if (Test-Path $scriptsSrc) {
    New-Item -ItemType Directory -Force -Path $scriptsDst | Out-Null
    Copy-Item "$scriptsSrc\*" $scriptsDst -Force
    Write-Host "[setup] staged Data\Scripts ($((Get-ChildItem $scriptsDst).Count) files)"
} else {
    Write-Warning "[setup] $scriptsSrc NOT FOUND - matches with AI players will desync against other platforms"
}

# GeneralsX @bugfix Claude 28/07/2026 Stage MapsZH.big. THIRD instance of the same defect as
# Data\Scripts above and Data\Cursors: the map cache is scanned relative to the WORKING
# DIRECTORY, so with the run folder holding no .big files it indexed only the user maps under
# Documents and found zero built-in "maps\..." entries. The Mac game folder is self-contained
# (25 .big files including this one) and had them all, so the two machines disagreed about which
# maps exist.
#
# The symptom is remote and misleading: the JOINER reports "You do not have the map" for a map it
# can see in single player, while the HOST just logs a peer that joined and never accepted. It
# cost a full cross-platform soak run to localise.
#
# Same WORKAROUND layer as the other two - fixing the engine's path resolution to honour
# CNC_GENERALS_ZH_PATH would remove all three.
$mapsSrc = Join-Path $steam "MapsZH.big"
if (Test-Path $mapsSrc) {
    Copy-Item $mapsSrc $run -Force
    Write-Host "[setup] staged MapsZH.big ($([math]::Round((Get-Item "$run\MapsZH.big").Length/1MB,1)) MB)"
} else {
    Write-Warning "[setup] $mapsSrc NOT FOUND - built-in maps will be missing and cross-platform joins will fail on map availability"
}

# GeneralsX @feature Claude 25/09/2026 Data\INI\Weapon.ini, the Toxin Tractor balance change, in the RUN
# folder (Codex P2 on feat/next-batch-defaults). go.bat launches from $run, and Win32LocalFileSystem opens
# a relative loose file in the working directory first, so a peer launched here without it computes a
# dataID every packaged peer refuses (DATA_DIFFERS). Cut from the CoreINI this peer mounts by
# scripts\tooling\make-weapon-ini.py --root, which follows the engine's mount order: the asset root, then
# the overlay (GENERALSX_PATCH_PATH, else the run folder's GeneralsXPatch). go.bat below sets
# CNC_GENERALS_ZH_PATH to the Steam folder, so it is set the same way for the call, not passed as a
# --search that would outrank the engine's order; idempotent. No archive anywhere is a warning, as in the
# deploy scripts - such a peer joins nobody anyway; a refused archive (another Community Patch release) is fatal.
# Python found the way package-windows-share.ps1 finds it: each candidate RUN, since the Store's
# python.exe alias resolves and does nothing, and [pscustomobject] rows so an empty prefix survives.
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

# GeneralsX @tweak Claude 24/09/2026 PRESENT WITH MAILBOX, NOT FIFO (owner decision 24/09/2026).
#
# THIS HEREDOC IS THE SOURCE OF TRUTH FOR EVERY WINDOWS dxvk.conf: package-windows-share.ps1 zips
# this run folder's copy into the runtime tier, the installer unpacks that zip, and
# GeneralsXUpdate.exe's place_tree replaces {app}\dxvk.conf whenever the tier's content moves. So an
# existing install is rewritten on its next update; a hand edit there is overwritten, as on every
# earlier runtime-tier change. resources/dxvk/dxvk.conf (macOS/Linux) and ios/config/dxvk.conf are
# separate files and are not touched.
#
# Measured before this: fullscreen FIFO ran at exactly 60 it/s on a 60 Hz panel, zero slack; the
# same scene under MAILBOX ran ~6370 it/s. The two keys, read from references/fbraz3-dxvk at the
# pinned 1e1e0638 (both keys date from before upstream "Release 2.6", ad253b8a, which both pairs
# this script stages are built from - -DxvkPair gx and official alike):
#   d3d9.presentInterval = 0  d3d9_options.cpp:46, applied in D3D9SwapChainEx::Present over the
#                             engine's own PresentationInterval: sync interval 0 on every present.
#   dxvk.tearFree = True      dxvk_options.cpp:14. With sync interval 0, Presenter::pickPresentMode
#                             then asks for MAILBOX only, never IMMEDIATE, so this does not tear.
#                             A driver without MAILBOX falls back to FIFO, the mode we had.
#
# GeneralsX @tweak Claude 28/09/2026 16x ANISOTROPIC FILTERING, the value macOS and iOS already ship
# (resources/dxvk/dxvk.conf, ios/config/dxvk.conf). Every terrain shader hard-sets LINEAR min/mag
# (W3DShaderManager.cpp TerrainShaderPixelShader::set, TerrainShader2Stage::set), never ANISOTROPIC,
# so the in-game anisotropy option never reaches the ground and Windows terrain was trilinear-only at
# the RTS camera's grazing angle. DXVK applies this key to every mipped sampler whose min filter is
# above POINT (d3d9_device.cpp, samplerAnisotropy), once, at device creation - so it is not a knob a
# later in-match quality governor can turn.
#
# GeneralsX @bugfix Claude 05/10/2026 Zero Hour no longer uses the samplerAnisotropy line below:
# W3DDisplay::init replaces it per launch through DXVK_CONFIG with the launcher's Texture filtering
# choice (the level for Anisotropic, -1 otherwise; the unpicked default is Anisotropic 16x). The line
# is left in the heredoc on purpose - changing the heredoc moves the runtime tier and rewrites every
# install's dxvk.conf for nothing.
@"
# Logs and shader cache are pinned inside the run folder so nothing lands in Steam.
dxvk.logLevel = info

# MAILBOX present: no vsync wait, no tearing (falls back to FIFO where the driver lacks it).
d3d9.presentInterval = 0
dxvk.tearFree = True

# 16x anisotropic filtering: the RTS camera sees the ground at a grazing angle.
d3d9.samplerAnisotropy = 16
"@ | Set-Content "$run\dxvk.conf" -Encoding ASCII

# GeneralsX @bugfix Claude 24/09/2026 alsoft.ini beside generalszh.exe, which OpenAL Soft reads from the
# executable's folder. A Bluetooth headset whose mic is in use drops to its hands-free profile and WASAPI
# grants a 16 kHz mono mix; OpenAL Soft's default resampler then aliases the bright UI clicks into an
# "eerie, high-pitched" sound. bsinc24 is the fix a player confirmed, in menus and in game (24/09/2026).
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

# A GUI + Vulkan app MUST run in the interactive session. Launched over SSH you land in
# session 0, where DXVK enumerates ZERO adapters and W3DDisplay::init() dies at 0xC0000005.
# A transient scheduled task with /it reaches the logged-on user's session; it is deleted below.
#
# GeneralsX @bugfix Claude 27/09/2026 Only the game THIS launch started. Generals Online runs generalszh.exe out
# of Steam on this box, and `Get-Process generalszh | Stop-Process` below ended it along with ours; the RUNNING
# line could report its CPU as ours; and a folder alone cannot tell this launch's game from another session's.
# So the task's command line carries a token, and Ours is a generalszh whose own command line or one of its four
# nearest (older) ancestors' holds it - the task's cmd, which waits on go.bat, which waits on the game - and
# which runs out of $run. Ours THROWS when that cannot be told - the process table cannot be read, or one of
# them has no readable path - so "none" never stands in for "cannot tell".
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
