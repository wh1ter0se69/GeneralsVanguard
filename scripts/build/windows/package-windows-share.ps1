# Build a self-contained zip a friend can run, from an already-staged run folder.
# GeneralsX @feature Claude 07/08/2026
#
# WHY THIS EXISTS SEPARATELY FROM setup-run-win64.ps1: that script stages OUR box - it reads from
# C:\dev\GeneralsX\build\win64 and a hardcoded Steam path, and it stages a .pdb and logs. A friend
# has none of the build tree and a different install location. This produces the redistributable
# half: our binaries plus a setup script that resolves THEIR install at their end.
#
# WHAT IS AND IS NOT IN THE ZIP
#
# IN:  our exe, DXVK's d3d8/d3d9, OpenAL32.dll, zlib1.dll, the Miles/Bink x64 stubs, dxvk.conf.
#      All of it is ours or redistributable.
# OUT: every piece of EA game data. Data\Scripts, Data\Cursors, the .big archives and the maps stay
#      on the friend's own installation and are copied locally by the setup script at their end.
#      They must own Zero Hour. We ship an engine, not the game.
# OUT: the .pdb (59 MB, useless without our symbols server) and every .err/.log in the run folder.
#
# NO RELAY ADDRESS GOES IN THE ZIP, and that is the whole point of the bootstrap.
#
# This script used to take -RelayAddress and write relay-settings.ini into the package, which the
# installer then copied into Options.ini. That made the address something a person had to be given,
# and a copy of the zip passed on second-hand had no sidecar and dead-ended on "this copy did not
# come with a server address". The launcher now fetches a SIGNED relay list over HTTPS from the one
# name it is compiled with and writes the relay into Options.ini itself. See gxlaunch.c's bootstrap
# section and scripts/tooling/publish-bootstrap.sh.
#
# The practical consequence for whoever runs this: the zip is now the same for everybody, it carries
# nothing operator-specific, and changing relays does not mean repackaging anything.
#
# Usage, on the Windows box, after setup-run-win64.ps1 has staged a good run folder:
#   powershell -ExecutionPolicy Bypass -File package-windows-share.ps1
#
# It takes no arguments in the normal case, and -Out in particular is now derived rather than given:
# the bundle's name has to agree with the BUILD.txt inside it or every launch re-downloads the whole
# payload. See "THE BUNDLE'S NAME" near the end.

# GeneralsX @feature Claude 12/08/2026 THE COMMUNITY PATCH NOW RIDES IN THE ZIP, and it is the
# reason a friend can join at all.
#
# dataID covers the mounted archives and every peer must agree or the join is refused with
# DATA_DIFFERS. Our peers carry the Community Patch, so a friend on retail data alone was refused
# by all four of us - measured: retail only is 38744C41, this fleet is FA018128. Handing them the
# .big to drop into their Zero Hour folder was the obvious fix and is the one thing this project
# will not do: an engine loads every *.big in the game directory, so a community archive in a
# Steam install is also loaded by GENERALS ONLINE, which validates those INIs and would desync
# the owner against every GO player. That already broke somebody's game once.
#
# So the archives ship in a GeneralsX-owned subfolder of OUR install directory. The engine mounts
# <exe dir>\GeneralsXPatch at the precedence each filename earns among the archives already
# mounted, which reproduces the in-folder layout exactly - measured identical, FA018128 both ways
# - while the friend's Zero Hour folder is never written to. tar.exe recreates the subdirectory on
# extract and the launcher's place_tree recurses into it, so update and first install agree.
param(
    [string]$Run       = "C:\dev\GeneralsX-run",
    # GeneralsX @bugfix Claude 13/08/2026 EMPTY BY DEFAULT, AND THE NAME IS DERIVED. See "THE
    # BUNDLE'S NAME" below - a bundle name that disagrees with the BUILD.txt inside it makes every
    # launch re-download the whole payload, forever. Pass this only to put the zip somewhere else;
    # a name that contradicts the binary is refused rather than shipped.
    [string]$Out       = "",
    # Where the overlay archives are staged on this box. Defaults inside the run folder, so the
    # dev peer and the friend payload are wired identically and a broken overlay is visible here
    # before it ships.
    [string]$Patch     = "",
    # GeneralsX @feature Claude 12/08/2026 Where the SPLIT artifacts land. See "THE SPLIT" below.
    #
    # THIS DIRECTORY IS MEANT TO PERSIST BETWEEN RUNS and that is a correctness property, not
    # housekeeping. An artifact whose content is unchanged keeps its name, and reusing the zip
    # ALREADY SITTING HERE guarantees it keeps its bytes too - so the sha256 in the manifest does
    # not move and clients skip the download. Repacking would produce a different zip for identical
    # input (Compress-Archive stores mtimes), which would mean the same URL serving different bytes:
    # exactly the edge-cache trap publish-bootstrap.sh:314-319 was written about.
    [string]$ArtifactDir = "C:\dev\GeneralsX-artifacts"
)

$ErrorActionPreference = "Stop"

# The exact payload. Anything not named here is deliberately excluded - the run folder accumulates
# old exes, crash logs and shader caches, and shipping those to a friend is how a stale binary gets
# played by mistake.
$payload = @(
    "generalszh.exe",   # the engine
    "d3d8.dll",         # DXVK - Windows 11 x64 ships no 64-bit d3d8 at all
    "d3d9.dll",         # DXVK
    "OpenAL32.dll",     # audio backend. Absent -> 0xC0000135 before WinMain, no output whatsoever
    "zlib1.dll",        # vcpkg app-local dependency
                        # mss32.dll and binkw32.dll are deliberately absent: TheSuperHackers
                        # #3272 replaced both import stubs with runtime LoadLibrary calls, so the
                        # exe no longer imports either and nothing has to ship them
    "dxvk.conf",
    "alsoft.ini"        # OpenAL Soft: resampler = bsinc24 (setup-run-win64.ps1 says why)
)

# GeneralsX Launcher - the program a friend actually double-clicks - and the updater it drives.
#
# THESE ARE REQUIRED NOW, AND THEY USED TO BE OPTIONAL. The reasoning behind "optional" was that a
# run folder predating them should still produce a shippable zip, and that a zip without them just
# meant no auto-update. That stopped being true when relay-settings.ini was deleted: the updater is
# now the ONLY thing that discovers a relay, so a package without it installs a game that can never
# reach anybody. Transport::initRelay finds no RelayAddress, treats the install as LAN-only, and the
# person is left with a working single-player game and no way to tell why multiplayer is empty.
#
# A zip that cannot reach a match is not a shippable zip, so this is fatal rather than a NOTE.
$payload += @(
    "GeneralsXLauncher.exe",
    "GeneralsXUpdate.exe"
)

if ($Patch -eq "") { $Patch = Join-Path $Run "GeneralsXPatch" }

# GeneralsX @feature Claude 17/08/2026 Build the overlay archives GeneralsX AUTHORS, before the
# staging loops below go looking for them in $Patch.
#
# WHY A BUILD STEP APPEARS IN A PACKAGER. Every archive on either list used to be third-party
# content somebody had downloaded once, so "it is in $Patch" was a safe assumption. !GXUI.big is
# this project's own .wnd layout overrides and there was no way to get it here: it existed on the
# developer's Mac and nowhere else, which is why no .wnd change in this repository had ever
# reached a player. It is derived from Window\Menus\PopupHostGame.wnd inside the retail
# WindowZH.big, so the box that packages the game can simply rebuild it from data it already has.
#
# FATAL. The optional staging below throws on a missing archive anyway - by design, because an
# absent optional archive can never be switched on later - so failing here only moves the same
# refusal to where the cause is still legible.
#
# python and not python3 first: this leg runs on the Windows box, where the launcher's own test
# harness already invokes the bare name (launcher\test-launcher.ps1). py -3 is the fallback for a
# machine carrying only the Python launcher.
$genScript = Join-Path $PSScriptRoot "..\..\tooling\build-overlay-generated.py"
if (-not (Test-Path $genScript)) {
    throw "overlay generator not found at $genScript - this script must run from a checkout."
}
#
# EACH CANDIDATE IS RUN, not merely resolved. Windows ships an App Execution Alias called
# python.exe that exists, satisfies Get-Command, and does nothing but open the Store - so
# "found an interpreter" is not the same claim as "an interpreter runs", and taking the first
# resolvable name would abort packaging with a python error rather than a useful one.
#
# [pscustomobject] rows and not nested arrays, which is not style: PowerShell's array literal
# absorbs an empty nested array, so @("python", @()) has a Count of ONE and the prefix-argument
# element silently disappears. Named fields cannot do that.
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
if (-not $pyExe) {
    throw ("no working python interpreter on PATH. It builds !GXUI.big, which is listed in " +
           "overlay-optional.txt and is generated rather than downloaded, so packaging cannot " +
           "produce a complete payload without it.")
}
# --game-dir is the RUN folder WHEN THE RUN FOLDER HAS THE RETAIL ARCHIVES: that is the install this
# payload is being cut from, so the archive it reads is the one this build's players will have.
#
# GeneralsX @bugfix Claude 17/08/2026 But a run folder is not required to be a full install, and on
# the machine that actually cuts releases it is not one - the engine there prints
# "[ASSET_ROOT] WARNING: no base Generals assets found next to C:\dev\GeneralsX-run" on every launch,
# because the retail data lives in a separate directory and is reached through CNC_GENERALS_ZH_PATH.
# Passing $Run unconditionally therefore made packaging fail outright on the release box, and it
# overrode GX_GAME_DATA - the very escape hatch the generator's own error message tells you to use.
#
# Prefer $Run so the default keeps its intended meaning, and fall back only when the source archive
# is genuinely absent there. Failing with the generator's own message is still the outcome when
# neither location has it, which is correct: a payload without !GXUI.big is incomplete.
#
# GeneralsX @bugfix Claude 18/09/2026 AND THEN EVERY OTHER PLACE IT COULD BE, all handed to the
# generator, which takes the first that holds WindowZH.big and names every one it tried on a miss.
# This probed $Run and GX_GAME_DATA only, so release 85f9a6650 still stopped on "WindowZH.big not
# found" on the release box until GX_GAME_DATA was set by hand. Added: CNC_GENERALS_ZH_PATH, the
# variable the engine itself reads the retail data through (and run.cmd sets), and $Patch, the one
# directory the archive staging below searches - so the generator tries at least what staging does.
# Still not hardcoded: C:\dev\gxdata. It is this box's layout, recorded in
# docs\WORKDIR\RUNBOOK_2026-08-24_ATOMIC_SHIP.md, not a default for anyone else's.
#
# Resolved up here, before anything is staged, so GX_OVERLAY_RESOLVE_ONLY=1 can answer "which
# WindowZH.big would this read" without a run folder. scripts\test\overlay-gamedir-test.ps1 is the
# check that fails if one of these stops being tried.
$genDirArgs = @()
foreach ($d in @($Run, $env:GX_GAME_DATA, $env:CNC_GENERALS_ZH_PATH, $Patch)) {
    # An unset variable is $null, and a $null splatted into a native command's arguments vanishes,
    # leaving a bare --game-dir to swallow the next flag. Skip it here instead.
    if ($d) { $genDirArgs += @("--game-dir", $d) }
}
if ($env:GX_OVERLAY_RESOLVE_ONLY) {
    $resolveArgs = $pyArgs + @($genScript, "--resolve") + $genDirArgs
    & $pyExe @resolveArgs
    exit $LASTEXITCODE
}

$stage = Join-Path $env:TEMP ("gxshare-" + [Guid]::NewGuid().ToString("N").Substring(0,8))
New-Item -ItemType Directory -Force -Path $stage | Out-Null

$missing = @()
foreach ($f in $payload) {
    $src = Join-Path $Run $f
    if (Test-Path $src) { Copy-Item $src $stage -Force }
    else { $missing += $f }
}
if ($missing.Count -gt 0) {
    Remove-Item -Recurse -Force $stage
    throw "run folder is incomplete, refusing to ship a broken zip. Missing: $($missing -join ', ')"
}

# GeneralsX @bugfix Claude 24/09/2026 Presence is not identity: refuse a C# loader under the Tauri
# launcher's name. See launcher-kind.ps1.
. (Join-Path $PSScriptRoot "launcher-kind.ps1")
try { Assert-TauriLauncher (Join-Path $stage "GeneralsXLauncher.exe") }
catch { Remove-Item -Recurse -Force $stage; throw }
# The updater too: see Assert-GxUpdater. Release sha = the repo this script lives in.
$gxRepo = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$gxHead = (& git -C $gxRepo rev-parse HEAD).Trim()
try { Assert-GxUpdater (Join-Path $stage "GeneralsXUpdate.exe") $gxRepo $gxHead }
catch { Remove-Item -Recurse -Force $stage; throw }
Write-Host "  launcher        : GeneralsXLauncher.exe (Tauri) + GeneralsXUpdate.exe"
# GeneralsX @feature Claude 03/10/2026 Presence is not identity for the DXVK pair either: both DLLs must be ONE pinned
# pair (dxvk\dxvk-win64.pins) - ours (gx) or the official 2.6 release - never a stray, a meas/oracle build, or a d3d8
# from one build beside a d3d9 from another. setup-run-win64.ps1 -DxvkPair picks which.
. (Join-Path $PSScriptRoot "dxvk\dxvk-pair.ps1")
try { $dxvkPair = Get-DxvkPairName $stage }
catch { Remove-Item -Recurse -Force $stage; throw }
Write-Host "  DXVK pair       : $dxvkPair (d3d9.dll $((Get-FileHash -Algorithm SHA256 (Join-Path $stage 'd3d9.dll')).Hash.Substring(0, 16)))"

# GeneralsX @bugfix Claude 12/08/2026 THE SIXTEEN-COLOUR PAIR, WHICH ONLY macOS AND iOS SHIPPED.
#
# deploy-macos-zh.sh:188-199 and the iOS bundler both copy Data\INI\Multiplayer.ini and
# Data\English\Generals.csf out of the checkout. This packager never did. Multiplayer.ini goes
# through the INI parser, so it is CRC'd into iniCRC -> dataID, and dataID is a REFUSAL field
# (SimIdCompare reads engineID, sourceID, dataID, ordinalID). The result measured tonight:
#
#     macOS  data=8A40A36B     iPad  data=8A40A36B     Windows  data=FA018128
#
# i.e. the Windows peer could not join the Mac or the iPad at all, and because it also lacked the
# colour definitions its lobby rendered every player's colour as "???". Both symptoms, one cause.
# Shipping the pair here CONVERGES the fleet onto one dataID rather than splitting it further -
# the two platforms that already carry it do not move.
#
# Refuse rather than ship half, exactly as the macOS script does and for the same reason: the CSF
# is NOT part of iniCRC (TheGameText is initialised with a null Xfer, GameEngine.cpp:589), so a
# peer that got the INI but not the CSF still joins everyone and merely renders
# MISSING: 'Color:Crimson' in the lobby - which reads as a font or localisation fault instead of
# as a package that stopped halfway.
$repoRoot     = Resolve-Path (Join-Path $PSScriptRoot "..\..\..")
$mpIniSrc     = Join-Path $repoRoot "GeneralsZH\Data\INI\Multiplayer.ini"
$colorCsfSrc  = Join-Path $repoRoot "GeneralsZH\Data\English\Generals.csf"

if (Test-Path $mpIniSrc) {
    if (-not (Test-Path $colorCsfSrc)) {
        Remove-Item -Recurse -Force $stage
        throw ("$mpIniSrc exists but $colorCsfSrc does not. Shipping the INI without the CSF " +
               "gives every peer a matching dataID and a lobby full of MISSING: 'Color:...' " +
               "strings. Refusing to ship half of it.")
    }
    # The paths are not parallel and that is not a typo: .wnd files live under Window\ inside
    # WindowZH.big, while INI and CSF live under Data\.
    foreach ($pair in @(@($mpIniSrc, "Data\INI"), @($colorCsfSrc, "Data\English"))) {
        $dstDir = Join-Path $stage $pair[1]
        New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
        Copy-Item -LiteralPath $pair[0] -Destination $dstDir -Force
    }
    Write-Host "  colour data     : Data\INI\Multiplayer.ini + Data\English\Generals.csf (moves dataID)"
} else {
    Write-Host "  colour data     : none in the checkout - shipping retail colours"
}

# The community-patch overlay, staged into a subdirectory of the zip.
#
# FATAL WHEN INCOMPLETE, for the same reason the launcher is: a payload without these installs a
# game that plays perfectly on its own and is refused by every peer with DATA_DIFFERS - which
# reads as a network fault and sends the person to their router. A zip that cannot reach a match
# is not a shippable zip.
#
# !GXUI.big is built into $Patch first. The interpreter, the generator and its --game-dir
# candidates were resolved at the top, before $stage existed - see "Build the overlay archives".
$genArgs = $pyArgs + @($genScript, "--dest", $Patch) + $genDirArgs
& $pyExe @genArgs
if ($LASTEXITCODE -ne 0) {
    Remove-Item -Recurse -Force $stage
    throw "building the generated overlay archives into ${Patch} failed (exit $LASTEXITCODE)."
}

# The archive NAMES come from scripts\build\overlay-archives.txt so that this script, the two
# POSIX deploy scripts and the iOS packager cannot disagree about the set. Read relative to
# $PSScriptRoot, i.e. out of the repo checkout this file is running from - the same relative read
# that once shipped two deleted installer scripts because a stale copy sat next to the packager.
# The list travels with this script in git, so the two are the same vintage by construction; if
# you are running package-windows-share.ps1 from outside a checkout, that is the bug.
$listPath = Join-Path $PSScriptRoot "..\overlay-archives.txt"
if (-not (Test-Path $listPath)) {
    Remove-Item -Recurse -Force $stage
    throw "overlay archive list not found at $listPath - this script must run from a checkout."
}
$overlay = @(Get-Content -LiteralPath $listPath |
    ForEach-Object { ($_ -split '#')[0].Trim() } |
    Where-Object { $_ -ne "" })
if ($overlay.Count -eq 0) {
    Remove-Item -Recurse -Force $stage
    throw "overlay archive list at $listPath names nothing."
}

$patchStage = Join-Path $stage "GeneralsXPatch"
New-Item -ItemType Directory -Force -Path $patchStage | Out-Null
$missingPatch = @()
foreach ($a in $overlay) {
    # -LiteralPath throughout: these filenames are the community's and already begin with '!'.
    # PowerShell's -Path treats [ ] as wildcards, so a future archive named with brackets would
    # silently resolve to nothing and ship a zip that looks complete.
    $src = Join-Path $Patch $a
    # Test-Path alone is not enough and the POSIX side already knew it: gx_overlay_verify uses
    # -s, which requires a non-empty file. Test-Path says True for a DIRECTORY of that name and
    # for a zero-byte file, and either would have been copied, counted, and shipped - producing a
    # zip that looks complete and an engine that mounts nothing. Cross-model review catch.
    $item = Get-Item -LiteralPath $src -ErrorAction SilentlyContinue
    if ($item -and -not $item.PSIsContainer -and $item.Length -gt 0) {
        Copy-Item -LiteralPath $src -Destination $patchStage -Force
    }
    else { $missingPatch += $a }
}
if ($missingPatch.Count -gt 0) {
    Remove-Item -Recurse -Force $stage
    throw ("community-patch overlay incomplete in ${Patch}: " + ($missingPatch -join ', ') +
           ". A friend on this payload would be refused by every peer with DATA_DIFFERS.")
}
# GeneralsX @feature Claude 24/09/2026 Data\INI\Weapon.ini, the Toxin Tractor balance change, cut from
# the CoreINI archive just staged by scripts\tooling\make-weapon-ini.py (never committed: it is the
# Community Patch's file). It is in iniCRC, so it moves dataID and rides the REQUIRED tier with the
# colour pair. FATAL: every other platform ships it, so a payload without it joins nobody.
# $genDirArgs is the same "--game-dir <retail folder>" list the overlay generator gets: the recipe cuts
# Data\Scripts\SkirmishScripts.scb (AI script edits, 29/09/2026) from the retail loose copy in one of them.
$weaponArgs = $pyArgs + @((Join-Path $PSScriptRoot "..\..\tooling\make-weapon-ini.py"),
                          "--search", $patchStage) + $genDirArgs + @("--out", (Join-Path $stage "Data\INI\Weapon.ini"))
& $pyExe @weaponArgs
if ($LASTEXITCODE -ne 0) {
    Remove-Item -Recurse -Force $stage
    throw "generating Data\INI\Weapon.ini from $patchStage failed (exit $LASTEXITCODE)."
}
# Weapon.ini and the Alpha Aurora fix files beside it (26/09/2026): the recipe names them all.
$listArgs = $pyArgs + @((Join-Path $PSScriptRoot "..\..\tooling\make-weapon-ini.py"), "--list")
$recipeFiles = @(& $pyExe @listArgs | ForEach-Object { $_.Trim() -replace '/', '\' } | Where-Object { $_ })
if ($LASTEXITCODE -ne 0 -or $recipeFiles.Count -lt 1) {
    Remove-Item -Recurse -Force $stage
    throw "make-weapon-ini.py --list failed (exit $LASTEXITCODE)."
}
Write-Host "  balance data    : $($recipeFiles -join ', ') (generated, moves dataID)"
# Optional overlay content: ships in the payload either way, and the DEFAULT decides only what
# the file is called on arrival - <name>.big is mounted, <name>.big.off is not.
#
# SHIPPING IT SWITCHED OFF RATHER THAN NOT SHIPPING IT is the whole point. The launcher's toggle
# is then a rename, which is instant and works offline; the alternative - fetching the archive
# when somebody ticks the box - turns a checkbox into a download that can fail, and a failed
# download would leave the box ticked and the content absent.
#
# NOTHING HERE MAY AFFECT dataID. Every entry has been measured to contain zero files the SimID
# fingerprint reads (see overlay-optional.txt), so a player who turns one off still joins one who
# leaves it on. An entry that moved dataID would make this checkbox a way to silently lose your
# friends, which is why the list is separate from the mandatory one rather than a flag on it.
$optListPath = Join-Path $PSScriptRoot "..\overlay-optional.txt"
if (-not (Test-Path $optListPath)) {
    Remove-Item -Recurse -Force $stage
    throw "optional archive list not found at $optListPath - this script must run from a checkout."
}
$optional = @(Get-Content -LiteralPath $optListPath |
    ForEach-Object { ($_ -split '#')[0].Trim() } |
    Where-Object { $_ -ne "" } |
    ForEach-Object {
        $f = $_ -split '\|', 3
        if ($f.Count -ne 3) { throw "malformed line in ${optListPath}: $_" }
        if ($f[0] -ne 'on' -and $f[0] -ne 'off') { throw "default must be on/off, got '$($f[0])' in ${optListPath}" }
        [pscustomobject]@{ Default = $f[0]; Name = $f[1]; Label = $f[2] }
    })

$missingOpt = @()
$manifest = @()
# The names as they land in the payload - "<name>.big" or "<name>.big.off". Collected because the
# artifact split below has to name exactly these files, and re-deriving the .off suffix in a second
# place is how the two would eventually disagree about which archive ships switched off.
$optionalStaged = @()
foreach ($o in $optional) {
    $src = Join-Path $Patch $o.Name
    $item = Get-Item -LiteralPath $src -ErrorAction SilentlyContinue
    if (-not $item -or $item.PSIsContainer -or $item.Length -le 0) { $missingOpt += $o.Name; continue }
    $dstName = if ($o.Default -eq 'off') { $o.Name + '.off' } else { $o.Name }
    Copy-Item -LiteralPath $src -Destination (Join-Path $patchStage $dstName) -Force
    # GeneralsX @feature Claude 17/08/2026 THE DEFAULT IS CARRIED THROUGH AS A THIRD FIELD, so the
    # launcher can tell a player what shipped as standard and whether they have changed it. It had
    # no way to know before: the payload said which archives exist and what to call them, and the
    # only other thing on disk is the CURRENT state, which is not the default and stops being it the
    # moment somebody ticks a box.
    #
    # APPENDED, NEVER PREPENDED, even though overlay-optional.txt puts the default first. A launcher
    # updates on its own tier, so an OLD launcher will meet this new payload, and its Split('|', 2)
    # must still find the archive filename in field 0 - it then merely renders "ControlBar Pro|on"
    # as the label, which is ugly and harmless. Leading with the default would make field 0 the word
    # "on": the old launcher would look for a file called "on", find neither spelling, and silently
    # drop every toggle. verify-bundle-overlay.sh reads the same first field with "${_l%%|*}" and is
    # unaffected for the same reason.
    $manifest += ($o.Name + '|' + $o.Label + '|' + $o.Default)
    $optionalStaged += $dstName
}
if ($missingOpt.Count -gt 0) {
    Remove-Item -Recurse -Force $stage
    throw ("optional overlay archives missing from ${Patch}: " + ($missingOpt -join ', ') +
           ". They ship switched off rather than not at all, so an absent one cannot be enabled later.")
}

# The launcher needs to know which archives it is ALLOWED to toggle, and it must never be able to
# switch off a mandatory one. Rather than compile that list into the launcher - where it would
# drift from this repo the first time the set changed - it ships beside the archives and is read
# at runtime. A .txt in GeneralsXPatch is inert to the engine: the overlay scan matches "*.big".
$manifest | Set-Content -Encoding UTF8 (Join-Path $patchStage "optional.txt")

# GeneralsX @feature Claude 30/09/2026 THE SHIPPED MAPS (scripts/build/overlay-maps.txt): loose files in
# GeneralsXPatch\ShippedMaps\<Name>\, carried by the OPTIONAL tier (never required - maps are outside
# dataID). The engine's overlay scan matches "*.big" only, so they are inert here; the launcher copies them
# into <userdata>\Maps (gxio shippedmaps.rs). Refuses a .map that does not hash to the listed value.
$mapsList = Join-Path $PSScriptRoot "..\overlay-maps.txt"
$mapsSrc  = Join-Path $PSScriptRoot "..\..\test\maps"
$shippedMaps = @()
foreach ($line in (Get-Content -LiteralPath $mapsList)) {
    $l = ($line -split '#')[0].Trim()
    if ($l -eq "") { continue }
    $f = $l -split '\|', 2
    if ($f.Count -ne 2) { Remove-Item -Recurse -Force $stage; throw "malformed line in ${mapsList}: $l" }
    $mapFile = Join-Path (Join-Path $mapsSrc $f[0]) ($f[0] + ".map")
    if (-not (Test-Path -LiteralPath $mapFile)) { Remove-Item -Recurse -Force $stage; throw "shipped map $($f[0]): $mapFile is missing" }
    $got = (Get-FileHash -LiteralPath $mapFile -Algorithm SHA256).Hash.ToLower()
    if ($got -ne $f[1].Trim().ToLower()) { Remove-Item -Recurse -Force $stage; throw "shipped map $($f[0]): .map sha256 $got is not the $($f[1]) overlay-maps.txt lists" }
    $mapDst = Join-Path $patchStage ("ShippedMaps\" + $f[0])
    New-Item -ItemType Directory -Force -Path $mapDst | Out-Null
    foreach ($m in (Get-ChildItem -LiteralPath (Join-Path $mapsSrc $f[0]) -File)) {
        Copy-Item -LiteralPath $m.FullName -Destination $mapDst -Force
        $shippedMaps += ("GeneralsXPatch\ShippedMaps\" + $f[0] + "\" + $m.Name)
    }
}
if ($shippedMaps.Count -eq 0) { Remove-Item -Recurse -Force $stage; throw "overlay-maps.txt lists no map" }

# GeneralsX @feature Claude 17/08/2026 The MANDATORY set, beside the optional one and for the
# mirror-image reason. optional.txt tells the launcher what it may switch off; required.txt tells
# it what must be present for a join to be legal at all, so the network test can name the archive
# that is missing instead of reporting a generic "something is wrong with your data".
#
# READ AT RUNTIME, NOT COMPILED IN, exactly as the optional list is. A launcher carrying its own
# copy of overlay-archives.txt would drift from the payload the first time the set changed, and the
# drift is silent in both directions: it would report a complete install that is missing an archive,
# or blame the player for one that no longer ships.
#
# BARE FILENAMES. The optional list needs a label per line because a person ticks those boxes; these
# are not offered as a choice, so there is nothing to label. The reader tolerates a trailing
# "|label" so the file can grow that shape later without breaking installs in the field.
$overlay | Set-Content -Encoding UTF8 (Join-Path $patchStage "required.txt")

$patchBytes = (Get-ChildItem -LiteralPath $patchStage -Filter *.big* | Measure-Object -Property Length -Sum).Sum
Write-Host ("  patch overlay   : {0} required + {1} optional, {2:N1} MB raw, from {3}" -f `
    $overlay.Count, $optional.Count, ($patchBytes / 1MB), $Patch)
foreach ($o in $optional) { Write-Host ("                    [{0}] {1}" -f $o.Default, $o.Label) }

# Record exactly which build this is. A friend reporting a bug against an unknown binary costs more
# than the 200 bytes this takes, and sourceID must match across peers or every join is refused.
$b = [IO.File]::ReadAllBytes((Join-Path $Run "generalszh.exe"))
$s = [Text.Encoding]::ASCII.GetString($b)
$sha = "unknown"
# GeneralsX @bugfix Claude 13/08/2026 THIS USED TO BE `$s -match '([0-9a-f]{40})'` AND IT PICKED THE
# WRONG STRING ON 13/08/2026, out of a binary that contained the correct one.
#
# Two independent faults, and either alone is enough to name a release after a data pattern:
#
#   1. PowerShell's -match is CASE-INSENSITIVE by default. A run of 0x41 bytes ('A') is 40 valid hex
#      digits under that rule, so a 59-byte 'A' run at 0x2244b6 - ordinary padding, hundreds of KB
#      before the real sha - matched first and the packager reported the commit as
#      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA". Every artifact name derives from this variable,
#      so that would have shipped as GeneralsX-win64-AAAAAAAAA.zip and poisoned the cache key for
#      that name permanently - the exact failure the "unknown" guard below exists to prevent, walked
#      around by a string that IS 40 hex characters.
#   2. First-match-wins over the whole file. The same binary also carries a 40-digit '6' run AFTER
#      the sha, which is lowercase-clean and would win the moment anything reorders the sections.
#
# So the rule is now three conditions, and a candidate must satisfy all of them:
#   (?-i)          lowercase only - git emits lowercase, padding runs are typically not
#   (?<!..)(?!..)  a STANDALONE token, non-hex on both sides, which rejects any run LONGER than 40
#                  (the 'A' run is 59, the '6' run is 40 inside a longer field)
#   corroboration  the engine embeds the short sha a few bytes after the full one, so the candidate
#                  must be followed within 64 bytes by its own 7..12 character prefix. A padding run
#                  cannot satisfy this against itself once the standalone rule has bounded it.
#
# Verified BOTH directions against the real 13/08/2026 binary: the old rule returns the 'A' run, the
# new rule returns 51f5cecdc4ebd327c7c2e3328a68dfbb1ac106d3, and a binary with no embedded sha still
# falls through to the refusal below rather than inventing one.
foreach ($m in [regex]::Matches($s, '(?-i)(?<![0-9a-f])[0-9a-f]{40}(?![0-9a-f])')) {
    $cand = $m.Value
    $tail = $s.Substring($m.Index + 40, [Math]::Min(64, $s.Length - $m.Index - 40))
    if ([regex]::IsMatch($tail, '(?-i)(?<![0-9a-f])' + $cand.Substring(0, 7) + '[0-9a-f]{0,5}(?![0-9a-f])')) {
        $sha = $cand
        break
    }
}
# GeneralsX @bugfix Claude 13/08/2026 REFUSED RATHER THAN SHIPPED AS "unknown", and this is not
# pedantry about a nice-to-have field. Every published name below is derived from $sha, so a build
# that cannot state its commit produces "GeneralsX-win64-unknown.zip" - a key that TWO DIFFERENT
# BUILDS would share. The second upload then sits behind the first one's edge entry - permanently,
# because no later build can ever pick a different name either.
#
if ($sha -eq "unknown") {
    Remove-Item -Recurse -Force $stage
    throw ("no 40-character commit sha is embedded in $Run\generalszh.exe. Every artifact name and " +
           "BUILD.txt are derived from it, so packaging would publish 'unknown' as a permanent " +
           "cache key shared by every future build. Rebuild the engine from a checkout.")
}
# NINE CHARACTERS, and this is THE source of truth for every name this script emits. BUILD.txt
# (below) and the engine artifact and the bundle all take it from this one variable, because
# state_load (gxlaunch.c:1609-1627) compares the two against each other on every launch and
# discards the whole install record when they disagree. Deriving either one from anything else -
# publish-time `git rev-parse HEAD`, for instance, which is what the PUBLISHER used to do - makes
# that guard fire forever on a build whose exe was correctly not rebuilt.
$engineTag = $sha.Substring(0, 9)
# SHA-256, not MD5. This line is an integrity claim somebody may check before running an unsigned
# binary, and MD5 has practical collision attacks - a matching MD5 proves nothing about a file an
# attacker was free to construct. The manifest already uses SHA-256 for build payloads; these must
# agree or the two halves of the trust story contradict each other.
$sha256 = (Get-FileHash (Join-Path $Run "generalszh.exe") -Algorithm SHA256).Hash

@"
GeneralsX - Windows share build
built from commit : $sha
exe sha256        : $sha256
packaged          : $(Get-Date -Format "yyyy-MM-dd HH:mm:ss")
user data         : Documents\GeneralsX\GeneralsZH

Verify this download before running it, on Windows:
    certutil -hashfile generalszh.exe SHA256
The value must match the sha256 above. If it does not, do not run it.

Every peer in a match must run THIS EXACT BINARY. The engine hashes its own simulation source
into a sourceID and refuses any join whose ID differs, so a friend on an older zip cannot join -
the failure reads as a network error and is not one.
"@ | Set-Content -Encoding ASCII (Join-Path $stage "BUILD.txt")

# GPL-3 + EA's additional terms (LICENSE.md: "Any propagation or conveyance of this program must include this
# copyright notice and these terms"): every package carries the repo's LICENSE.md. Engine tier, so it ships every release.
Copy-Item -LiteralPath (Join-Path $gxRepo "LICENSE.md") -Destination (Join-Path $stage "LICENSE.md") -ErrorAction Stop
# Third-party licence notices (generated by scripts/legal/third_party_notices.py): next to LICENSE.md, same tier.
& $pyExe @pyArgs -I (Join-Path $gxRepo "scripts\legal\third_party_notices.py") check
if ($LASTEXITCODE -ne 0) { throw "THIRD_PARTY_NOTICES.txt is stale or incomplete: run scripts/legal/third_party_notices.py generate" }
Copy-Item -LiteralPath (Join-Path $gxRepo "THIRD_PARTY_NOTICES.txt") -Destination (Join-Path $stage "THIRD_PARTY_NOTICES.txt") -ErrorAction Stop
# Payload gate: every .dll/.exe staged so far must be owned by a component of the notices file.
& $pyExe @pyArgs -I (Join-Path $gxRepo "scripts\legal\third_party_notices.py") check-path --platform windows $stage
if ($LASTEXITCODE -ne 0) { throw "the Windows payload carries a library with no entry in THIRD_PARTY_NOTICES.txt" }

# NO INSTALLER SCRIPTS IN THE ZIP, and this used to ship two.
#
# GeneralsX-Setup.cmd and install-windows-share.ps1 are DELETED from the repo - the wizard is now
# GeneralsX-Setup.exe, which is downloaded on its own and fetches THIS zip as the signed manifest's
# win32 artifact. So the zip is the payload, not a self-installing bundle.
#
# They were still being copied from $PSScriptRoot, which on the build box is a directory that has
# its own stale copies of both - so the packager cheerfully shipped two deleted files describing a
# flow that no longer exists, and said nothing. A file that is gone from the repo can still be
# sitting next to the script that copies it.

@"
GeneralsX
=========

This folder is the GeneralsX payload. You do not normally unzip it by hand:
GeneralsX-Setup.exe downloads and unpacks it for you, and the launcher it installs
keeps it up to date from then on.

If you are here because you unzipped it anyway, run GeneralsX.cmd - but only after
GeneralsX-Setup.exe has run once, because that is what works out where your game is.

You need Command & Conquer: Generals Zero Hour already installed - Steam, Origin/EA or disc all
work. Setup finds it on its own and asks only if it cannot.

Your existing game is NOT modified. Nothing is written into your Steam folder, so Generals Online
keeps working exactly as it did.

The GeneralsXPatch folder in here is the TheSuperHackers Community Patch, and GeneralsX loads it
from where it sits - your own game files are left alone. Everyone in a match has to be running the
same data or the game refuses the join, which is why it ships with the engine rather than being
something you install. Delete that folder and you will not be able to play with anybody.

If Windows shows a blue "Windows protected your PC" box: More info -> Run anyway. That appears
because this build is not code-signed, not because anything is wrong with it.
"@ | Set-Content -Encoding ASCII (Join-Path $stage "READ ME FIRST.txt")

# THE BUNDLE IS PACKED FURTHER DOWN, not here, and the move is the fix for a live bug rather than
# tidying. Its name is now derived from the content id of this staging tree and from $engineTag, so
# it needs Get-GxContentId - which is defined below with the other artifact machinery, because the
# bundle and the four artifacts must be named by ONE implementation. See "THE BUNDLE'S NAME".

# ==================================================================================================
# THE SPLIT
#
# GeneralsX @feature Claude 12/08/2026. Design: docs/WORKDIR/DESIGN_2026-08-12_payload-granularity.md
#
# WHY. The manifest named ONE artifact, so every update was all-or-nothing: the owner downloaded
# 112 MB to receive a 3.4 MB engine change. Measured on the published afa9a64b2 payload - 117.4 MB
# zipped, of which GeneralsXLauncher.exe is 60.7 MB (52%) and the engine is 3.4 MB (3%).
#
# So the payload is cut by HOW OFTEN EACH PART CHANGES, and the launcher skips any artifact it
# already has. state_hash_of (gxlaunch.c:1701-1710) looks an artifact up by (platform, name) and
# then compares hashes, so an unchanged part that keeps its name is skipped for free. That
# mechanism is not new - it has been in gxlaunch.c since its first commit; only the PUBLISHER
# never used it.
#
#     engine      3.6 MB   every release        generalszh.exe + the updater + BUILD.txt
#     runtime    63.5 MB   rarely               the launcher and the DLLs beside it
#     required   29.2 MB   almost never         the archives dataID depends on
#     optional   21.1 MB   never                ControlBar Pro and friends
#
# Routine engine update: 117.4 MB -> 3.6 MB.
#
# THE BUNDLE IS STILL BUILT AND IT IS STILL THE THING THE INSTALLER FETCHES. Splitting the
# installer's payload the way the manifest is split would be catastrophic and unrecoverable:
# GeneralsX.iss:458-514 takes the FIRST win32 row of gxsetup.txt as the whole payload, so four rows
# would hand every already-downloaded installer an engine with no launcher and no updater - and no
# way to self-repair, because the updater is the missing piece. See publish-bootstrap.sh's
# gxsetup.txt section, which enforces the one-row rule.
#
# THE BUNDLE IS NO LONGER CALLED "GeneralsX-win64-<commit>.zip", and that rename is what unblocks
# all of this. It was the engine tier's name exactly - two different byte-streams competing for one
# R2 key - which is why the split below has existed since 12/08 and has never been published. The
# bundle is now "GeneralsX-bundle-<content8>-<commit9>.zip"; nothing parses it but gxsetup.txt, and
# the reasoning for every part of that name is at "THE BUNDLE'S NAME" further down.
#
# NAMING, and each rule here is load-bearing:
#
#   engine   GeneralsX-win64-<commit>.zip  - unchanged shape, and the COMMIT MUST BE THE LAST
#            DASH-SEPARATED TOKEN. Two things read it: LauncherForm.cs:460 (the retired C# loader,
#            still run by legacy installs) matches
#            -win64-([0-9a-f]{7,40})(?:-|\.) for the "Available" line and can only print
#            "up to date" when that capture is non-empty; and state_load (gxlaunch.c:1609-1620)
#            takes the text after the LAST '-' and compares its first 7 chars to BUILD.txt's
#            commit, discarding the whole install record when nothing agrees. Appending a content
#            hash - "GeneralsX-win64-<commit>-<content>.zip", which a stale comment in
#            LauncherForm.cs:318 describes - would make that guard fire on EVERY launch and force a
#            full re-download forever. Do not do it.
#
#   the rest gxrt_<content8>.zip etc, named by their OWN content and with NO DASH. Content, so an
#            unchanged part keeps its URL and is skipped (the entire saving) while changed bytes
#            always get a fresh URL and never need a cache purge. No dash, so strrchr(name,'-') in
#            that same guard returns NULL for them and the ENGINE artifact is the only thing
#            driving it - which is what it was written to read.
# ==================================================================================================

# The tiers, as paths relative to $stage. Every staged file must appear in exactly one.
$tierEngine = @(
    "generalszh.exe",
    "GeneralsXUpdate.exe",     # 120 KB, and it is the program that PARSES the manifest: it must
                               # never be older than the format it is handed. MANIFEST_VERSION
                               # mismatch is refused outright (gxlaunch.c:671-677), which in the
                               # field reads as "updates just stopped".
    "BUILD.txt",               # state_load cross-checks this against the engine artifact's own
                               # name. They MUST move together or that guard fires every launch.
    "LICENSE.md",              # GPL-3 + EA's terms; ~35 KB, the tier every install receives.
    "GeneralsXPatch\optional.txt",  # ~200 bytes describing the optional set; riding with the tier
                                    # that ships every release keeps it current for free.
    "GeneralsXPatch\required.txt"   # the mandatory set, same size and the same argument. It must
                                    # not ride the REQUIRED tier it describes: that tier only moves
                                    # when an archive does, so a list that changed without one
                                    # changing would not reach anybody.
)
$tierRuntime = @(
    "GeneralsXLauncher.exe",
    "d3d8.dll", "d3d9.dll", "OpenAL32.dll", "zlib1.dll",
    # The notices of the libraries above (and of the launcher). They change when a dependency does, independent of the
    # engine commit, so they ride the CONTENT-ADDRESSED runtime tier (gxrt_<id>.zip): the engine tier is named by the
    # engine commit and a same-name, different-content zip would break a CDN edge against the signed hash.
    "THIRD_PARTY_NOTICES.txt",
    "dxvk.conf", "alsoft.ini", "READ ME FIRST.txt"
)
# The sixteen-colour pair rides the REQUIRED tier, and platform "any" is right for it: it is data,
# byte-identical on every platform, and it moves dataID - so a peer that misses it cannot join one
# that has it. Same tier as the overlay archives, for exactly the same reason. Guarded by Test-Path
# because the staging block above ships the pair only when the checkout carries it.
# GeneralsX @feature E · look-packs-0965 17/10/2026 THE MODEL PACK RIDES AN ARTIFACT OF ITS OWN. Every shipped Windows updater
# abandons ANY single download over MAX_DOWNLOAD_BYTES (512 MiB, gxlaunch.c:184 and :1168) - the cap is per download, a local
# total - and the one required zip holding !!GXModelsHD.big (~399 MB deflated) beside the HD textures (~183 MB) would cross it, so
# every existing install would abandon the whole update before applying anything. A new updater in the same release cannot help
# clients that already have the old one. The shipped updaters download every platform-"any" row (gxlaunch.c:2208, up to 16 rows)
# into one stage, each under its own cap, so a separate gxmdl_ row reaches them with no client change. The archive stays on
# required.txt (the mandatory set above): only its transport moves. Archives named here leave the required zip for their own.
$ownArtifact = @('!!GXModelsHD.big')
$tierModels   = @($overlay | Where-Object { $ownArtifact -contains $_ } | ForEach-Object { "GeneralsXPatch\$_" })
$tierRequired = @($overlay | Where-Object { $ownArtifact -notcontains $_ } | ForEach-Object { "GeneralsXPatch\$_" }) +
                @(@("Data\INI\Multiplayer.ini", "Data\English\Generals.csf") + $recipeFiles |
                  Where-Object { Test-Path (Join-Path $stage $_) })
$tierOptional = @($optionalStaged | ForEach-Object { "GeneralsXPatch\$_" }) + $shippedMaps

# EVERY STAGED FILE MUST BE IN EXACTLY ONE TIER, and this is a gate rather than a nicety. A file
# that is in no tier ships in the bundle and in NO artifact, so first installs get it and updates
# never do - the divergence appears months later as "it works on a fresh install". A file in two
# tiers lands in do_update's shared staging tree twice, where the documented resolution is "the
# second one wins" (gxlaunch.c:2061-2064) - defined, but not something to leave to chance.
$staged = @(Get-ChildItem -LiteralPath $stage -Recurse -File |
    ForEach-Object { $_.FullName.Substring($stage.Length + 1) })
$assigned = $tierEngine + $tierRuntime + $tierRequired + $tierModels + $tierOptional
$unassigned = @($staged   | Where-Object { $assigned -notcontains $_ })
$phantom    = @($assigned | Where-Object { $staged   -notcontains $_ })
$dupes      = @($assigned | Group-Object | Where-Object { $_.Count -gt 1 } | ForEach-Object { $_.Name })
if ($unassigned.Count -or $phantom.Count -or $dupes.Count) {
    Remove-Item -Recurse -Force $stage
    $msg = "the artifact tiers do not partition the payload, refusing to publish a split that would "
    $msg += "deliver files on first install and never on update."
    if ($unassigned.Count) { $msg += "`n  in the payload but in no tier : " + ($unassigned -join ', ') }
    if ($phantom.Count)    { $msg += "`n  named by a tier but not staged: " + ($phantom -join ', ') }
    if ($dupes.Count)      { $msg += "`n  named by two tiers            : " + ($dupes -join ', ') }
    throw $msg
}

# The content id: SHA-256 over "<relpath>\n<sha256>\n" for every file, ordinally sorted. It depends
# on CONTENT AND LAYOUT ONLY - never on mtimes, packing order or the machine - so identical input
# yields an identical name on any box, which is what makes "unchanged, so skip it" true.
#
# [Array]::Sort with StringComparer::Ordinal rather than Sort-Object: the default comparison is
# culture-sensitive, and a culture that orders '_' differently would silently change the id.
function Get-GxContentId {
    param([string]$Root, [string[]]$Rel)
    $sorted = [string[]]$Rel.Clone()
    [Array]::Sort($sorted, [StringComparer]::Ordinal)
    $sb = New-Object System.Text.StringBuilder
    foreach ($r in $sorted) {
        $h = (Get-FileHash -LiteralPath (Join-Path $Root $r) -Algorithm SHA256).Hash.ToLower()
        [void]$sb.Append($r.Replace('\', '/')).Append("`n").Append($h).Append("`n")
    }
    # ::Create()/ComputeHash rather than ::HashData, which is .NET 5+. This script is launched as
    # `powershell -File`, i.e. Windows PowerShell 5.1 on .NET Framework, where HashData does not
    # exist - and the failure would be a MethodNotFound at packaging time, on the build box, at the
    # end of a long stage.
    $alg = [Security.Cryptography.SHA256]::Create()
    try { $digest = $alg.ComputeHash([Text.Encoding]::UTF8.GetBytes($sb.ToString())) }
    finally { $alg.Dispose() }
    return (($digest[0..3] | ForEach-Object { $_.ToString('x2') }) -join '')
}

function New-GxArtifact {
    param([string]$Role, [string]$Platform, [string]$Name, [string[]]$Rel, [string]$ContentId = "")

    # An empty tier would produce a zip Compress-Archive refuses to make from an empty directory,
    # and an empty artifact is meaningless anyway. Cannot happen with today's lists - the loops
    # above throw when anything is missing - so this is about the day somebody empties one.
    if ($Rel.Count -eq 0) {
        throw "artifact tier '$Role' names no files. An empty artifact cannot be published."
    }

    $dest = Join-Path $ArtifactDir $Name
    # THE REUSE BRANCH USED TO TRUST THE FILENAME ALONE, and its comment justified that with "same
    # content id means same content". True for the three CONTENT-ADDRESSED tiers - gxrt_, gxreq_,
    # gxopt_ carry their own id in the name - and FALSE for the engine tier, whose name is
    # GeneralsX-win64-<engineTag>.zip where $engineTag is read out of generalszh.exe's embedded
    # commit and therefore says nothing about the tier's OTHER members: GeneralsXUpdate.exe,
    # BUILD.txt, LICENSE.md, GeneralsXPatch\optional.txt and GeneralsXPatch\required.txt.
    #
    # Any of those changes while the engine exe is correctly NOT rebuilt - a launcher-only fix
    # in gxlaunch.c is the documented, deliberate workflow (publish-bootstrap.sh:400-403, and it
    # has happened three times in this repo's history), and a new overlay-optional.txt entry is
    # another. The packager then found the old zip, printed "(unchanged, reused)" and wrote the OLD
    # sha256 and size into gxartifacts.txt. Nothing downstream could catch it: the commit-tag gate
    # sees one consistent tag, and artifact verification re-hashes the reused zip and agrees with
    # itself. Every EXISTING install skips that artifact on platform+name+hash and never receives
    # the fix; only a fresh bundle carries it - the "it works on a fresh install" divergence the
    # tier-partition gate at the top of this section exists to prevent, arriving by another door.
    #
    # So the caller passes the tier's real content id and the reuse branch is taken only when a
    # sidecar beside the zip agrees with it. The sidecar is never published - it is not in any
    # tier and lives in $ArtifactDir, not $stage.
    #
    # An artifact packed BEFORE this change has no sidecar, so it is repacked once. Measured on
    # Windows PowerShell 5.1, 2026-09-22: that repack did NOT move its sha256 - Compress-Archive
    # stores the SOURCE files' mtimes rather than the pack time, so unchanged staged files give
    # byte-identical bytes. Expect one repack and, normally, no re-download. Do not lean on it:
    # restage the tier and the timestamps move with it.
    $stamp = "$dest.contentid"
    $reuse = Test-Path -LiteralPath $dest
    if ($reuse -and $ContentId -ne "") {
        # -join '' rather than Get-Content -Raw: an empty or absent sidecar must read as "" and
        # compare unequal, never blow up on .Trim() against $null. -cne because the ids are hex
        # and PowerShell's default comparison is case-INSENSITIVE.
        $have = (@(Get-Content -LiteralPath $stamp -ErrorAction SilentlyContinue) -join '').Trim()
        if ($have -cne $ContentId) {
            Write-Host ("  {0,-9} : {1}  (content changed, repacking)" -f $Role, $Name)
            Remove-Item -LiteralPath $dest -Force
            $reuse = $false
        }
    }
    if ($reuse) {
        # Reuse, and this branch is the whole point - see the note on -ArtifactDir. Same content id
        # means same content, so the zip already here is a correct answer AND keeps its sha256,
        # which is what lets a client skip the download.
        Write-Host ("  {0,-9} : {1}  (unchanged, reused)" -f $Role, $Name)
    }
    else {
        $sub = Join-Path $env:TEMP ("gxart-" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
        foreach ($r in $Rel) {
            $dst = Join-Path $sub $r
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
            Copy-Item -LiteralPath (Join-Path $stage $r) -Destination $dst -Force
        }
        # PACKED UNDER A TEMPORARY NAME AND RENAMED, never written straight to $dest, and that is
        # not tidiness. The reuse branch above trusts any file bearing the right name - so a run
        # interrupted mid-Compress-Archive would leave a TRUNCATED zip under the final name, the
        # next run would reuse it, and publish-bootstrap.sh could not catch it: we would sign the
        # hash OF THE CORRUPT FILE, so the document would be perfectly self-consistent and every
        # client would install a broken payload. A rename on one filesystem has no such window.
        #
        # ".partial.zip", NOT ".zip.partial", and that ordering is not style. Windows PowerShell
        # 5.1's Compress-Archive insists the destination carry a .zip extension - it appends one
        # when it is missing rather than writing what it was asked to - so a ".zip.partial" target
        # produces "....zip.partial.zip" and the Move-Item below then fails on a path that was
        # never written. Caught by cross-model review; it could not be caught by running this,
        # because it is only reachable on Windows. Keep the extension last.
        $partial = Join-Path $ArtifactDir ($Name -replace '\.zip$', '.partial.zip')
        if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Force }
        Compress-Archive -Path (Join-Path $sub "*") -DestinationPath $partial -CompressionLevel Optimal
        Move-Item -LiteralPath $partial -Destination $dest
        Remove-Item -Recurse -Force $sub
        # Written only after the rename, for the same reason the rename exists: a sidecar beside a
        # truncated zip would make the next run trust it.
        if ($ContentId -ne "") { Set-Content -LiteralPath $stamp -Value $ContentId -Encoding ASCII }
        Write-Host ("  {0,-9} : {1}" -f $Role, $Name)
    }

    $item = Get-Item -LiteralPath $dest
    return [pscustomobject]@{
        Role     = $Role
        Platform = $Platform
        Name     = $Name
        Sha256   = (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash.ToLower()
        Size     = $item.Length
    }
}

New-Item -ItemType Directory -Force -Path $ArtifactDir | Out-Null
Write-Host ""
Write-Host "  the split artifacts:"

# The engine keeps the commit-shaped name; everything else is named by its own content. $engineTag
# is the first nine characters of the sha read out of the binary, computed beside BUILD.txt so the
# two cannot come from different places.
$artifacts = @(
    # -ContentId, and ONLY this tier needs it: the other three carry their id in the name, so a
    # changed tier lands on a different filename and the reuse branch answers correctly by
    # construction. This one's name carries the COMMIT, which four of its five members do not
    # depend on, so the id has to be passed in and checked.
    (New-GxArtifact -Role "engine"   -Platform "win32" -Rel $tierEngine `
        -ContentId (Get-GxContentId -Root $stage -Rel $tierEngine) `
        -Name ("GeneralsX-win64-{0}.zip" -f $engineTag)),
    (New-GxArtifact -Role "runtime"  -Platform "win32" -Rel $tierRuntime `
        -Name ("gxrt_{0}.zip"  -f (Get-GxContentId -Root $stage -Rel $tierRuntime))),
    (New-GxArtifact -Role "required" -Platform "any"   -Rel $tierRequired `
        -Name ("gxreq_{0}.zip" -f (Get-GxContentId -Root $stage -Rel $tierRequired))),
    # PLATFORM "win32", NOT "opt", AND THAT IS PHASE 1 ON PURPOSE. Publishing this under a platform
    # token no launcher recognises would make it opt-in - do_update skips anything that is neither
    # the client's platform nor "any" (gxlaunch.c:2016-2018) - but nothing can yet FETCH it on
    # demand, so a new install would lose ControlBar Pro with no way to get it back. The opt-in flow
    # and the launcher UI that drives it are Phase 2, and the token moves only once they ship.
    (New-GxArtifact -Role "optional" -Platform "win32" -Rel $tierOptional `
        -Name ("gxopt_{0}.zip" -f (Get-GxContentId -Root $stage -Rel $tierOptional)))
)
# The model pack's own row (see $ownArtifact), appended rather than written into the literal above: an empty @(if ...) inside an
# array literal is an EMPTY ELEMENT, not nothing, and every loop below would meet it. Role "required" because that is what it is -
# publish-bootstrap.sh's role whitelist (engine|runtime|required|optional) accepts it and every client treats the name as opaque.
if ($tierModels.Count) {
    $artifacts += New-GxArtifact -Role "required" -Platform "any" -Rel $tierModels `
        -Name ("gxmdl_{0}.zip" -f (Get-GxContentId -Root $stage -Rel $tierModels))
}
# NO ARTIFACT MAY CROSS THE SHIPPED UPDATERS' CAP. gxlaunch.c abandons a download over 512 MiB (MAX_DOWNLOAD_BYTES, :184 / :1168)
# and every install already in the field has that number compiled in, so a bigger zip is not a slow update - it is no update, for
# every Windows player at once. Checked here, before anything is published, because nothing downstream measures it.
$updaterCap = 512 * 1024 * 1024
$overCap = @($artifacts | Where-Object { $_.Size -gt $updaterCap })
if ($overCap.Count) {
    throw ("artifact(s) over the shipped updaters' 512 MiB download cap (gxlaunch.c MAX_DOWNLOAD_BYTES): " +
        (($overCap | ForEach-Object { "{0} {1:N0} B" -f $_.Name, $_.Size }) -join ', ') +
        " - split the tier (see `$ownArtifact) instead of publishing a release every existing install would abandon")
}

# ==================================================================================================
# THE INSTALL RECORD RIDES INSIDE THE BUNDLE
#
# GeneralsX @bugfix Claude 19/08/2026. This is the fix for "setup downloads 113 MB, then the
# launcher immediately downloads the same 118 MB again", measured on a clean install this session:
#
#   gxlaunch.log  state: installed ordinal 0, 1 known artifact(s)
#                 update: 4 artifact(s) to fetch, 118754724 bytes
#
# The installer's own record was correct when the payload was ONE zip whose name was in the
# manifest. THE SPLIT made the payload four tiers and made the bundle a CONTAINER, and a container's
# name appears in no manifest by construction. state_hash_of (gxlaunch.c:1701-1710) matches on
# platform AND name with strcmp, so the bundle row matches nothing, every tier reads as missing, and
# the launcher refetches the lot. Recording an unmatchable name is not better than recording
# nothing - it is exactly the "DELETING IT cost a download instead" failure GeneralsX.iss:668-673
# already describes, arriving by a different route.
#
# WHY THE RECORD IS WRITTEN HERE AND NOT BY THE INSTALLER. Setup cannot name the four tiers: it
# downloads one opaque zip and never sees the manifest. It could FETCH the manifest, but then a
# publish landing between its gxsetup.txt read and its manifest read would have it record artifacts
# it does not have - a STRANDED install, which GeneralsX.iss:664-669 measured and which is strictly
# worse than a redundant download. The packager, by contrast, knows all four hashes because it just
# computed them, so a record written here describes exactly the bytes in this bundle and cannot
# disagree with them.
#
# ordinal 0 is deliberate and unchanged: state_hash_of ignores the ordinal entirely, and the
# launcher's only ordinal comparison is manifest-vs-cached-manifest. Claiming a real ordinal here
# would be a guess about a manifest that has not been signed yet.
#
# The hash encoding is the launcher's, not the packager's: base64url of the FIRST 16 BYTES of the
# sha256, unpadded (gxlaunch.c:595, hash_matches at :972). Verified against all four live artifacts
# before this was written. GeneralsX.iss carries the same conversion in Pascal for its own row; if
# either moves, both move.
#
# This file is added AFTER the tier partition gate above, on purpose. It ships in the bundle and in
# no tier, which is precisely right: a fresh install needs it, and an update must never lay a stale
# copy over the record the launcher has been maintaining since.
function Get-GxManifestHash {
    param([Parameter(Mandatory=$true)][string]$Sha256Hex)
    $bytes = [byte[]]::new(16)
    for ($i = 0; $i -lt 16; $i++) {
        $bytes[$i] = [Convert]::ToByte($Sha256Hex.Substring($i * 2, 2), 16)
    }
    ([Convert]::ToBase64String($bytes)).TrimEnd('=').Replace('+', '-').Replace('/', '_')
}

$recordLines = @(
    '# GeneralsX installed state. Written by the installer; the launcher maintains it after.'
    'ordinal 0'
)
foreach ($a in $artifacts) {
    $recordLines += ('item {0} {1} {2}' -f $a.Platform, $a.Name, (Get-GxManifestHash -Sha256Hex $a.Sha256))
}
# CRLF and no trailing blank line: state_load reads this with the same parser it uses for its own
# output, and the installer writes CRLF for the row it still emits.
Set-Content -Encoding ASCII -LiteralPath (Join-Path $stage "gxinstalled.txt") -Value $recordLines

# The bundle's content id must cover it, or two bundles differing only in their record would collide
# on one name and the CDN would serve whichever was uploaded first.
$staged += "gxinstalled.txt"

Write-Host ""
Write-Host "  install record (ships inside the bundle, so first launch fetches nothing):"
foreach ($l in $recordLines | Where-Object { $_ -like 'item *' }) { Write-Host ("    " + $l) }

# ==================================================================================================
# THE BUNDLE'S NAME
#
# GeneralsX @bugfix Claude 13/08/2026. This is the fix for "every launch re-downloads 117 MB,
# forever", observed on the owner's install and reproducible from the published release.
#
# WHAT WENT WRONG. The bundle was named at PUBLISH time, from `git rev-parse HEAD` on the box doing
# the publishing, while BUILD.txt inside it carried the commit read out of generalszh.exe. Those two
# are only equal when the last commit touched engine source. `d549762d3` changed nothing but this
# very script, so the exe was correctly NOT rebuilt and still carried `30848e4ce` - and the release
# went out as GeneralsX-win64-d549762d3.zip wrapping a BUILD.txt that said 30848e4ce.
#
# state_load (gxlaunch.c:1596-1631) takes the text after the LAST '-' in each recorded artifact name
# and compares seven characters against BUILD.txt's commit. The installer records the bundle's own
# basename as that artifact (GeneralsX.iss:688-694). So: 'd549762' != '30848e4' -> positive
# contradiction -> the install record is memset -> every artifact looks missing -> the whole payload
# is fetched again. Re-applying it restores the same BUILD.txt, so the contradiction never clears.
# Verbatim from the owner's log:
#
#   state: gxinstalled.txt says 'GeneralsX-win64-d549762d3.zip' but BUILD.txt was built from
#   30848e4ce - the files were replaced behind the launcher. Discarding the record so this launch
#   re-checks the manifest.
#
# THE FIX IS STRUCTURAL, not a corrected string. Both halves now come from $engineTag, which is read
# out of the binary a few dozen lines above BUILD.txt is written from the same variable. There is no
# longer a second source that could disagree, so the guard cannot be made to fire by a commit that
# did not rebuild the engine.
#
# THE NAME ALSO STOPS COLLIDING WITH THE ENGINE TIER. "GeneralsX-win64-<commit>.zip" is exactly what
# the engine artifact is called, and the two are DIFFERENT byte-streams - 3.6 MB against 117.4 MB.
# Publishing both would put two objects on one R2 key, and whichever landed second would be served
# to clients expecting the other, failing its signed hash. That collision is the reason the split
# has been implemented since 12/08 and never published.
#
#   <content8>  the first 4 bytes of a SHA-256 over this staging tree's paths and file hashes.
#               Different bytes always get a different URL, so a re-pack never lands on an existing
#               edge entry - the trap publish-bootstrap.sh:441-445 records - and identical bytes
#               keep their URL and their sha256, so nothing has to be re-uploaded at all.
#   <commit9>   LAST, and that position is load-bearing: strrchr(name, '-') must find this token for
#               state_load to check it against BUILD.txt. Putting the content id last instead would
#               make the guard compare a content hash against a commit and fire on every launch,
#               which is the bug this comment exists about, reintroduced from the other side.
$bundleContent = Get-GxContentId -Root $stage -Rel $staged
$bundleName    = "GeneralsX-bundle-{0}-{1}.zip" -f $bundleContent, $engineTag

if ($Out -eq "") {
    $Out = Join-Path $ArtifactDir $bundleName
}
else {
    # An explicit -Out is still honoured - somebody wants the zip on a different drive - but not a
    # name that would strand the install it produces. The check is state_load's own rule, applied
    # here where it is still cheap: everything after the last '-', first seven characters, against
    # the commit BUILD.txt will carry. A name with no '-' at all is fine (strrchr returns NULL and
    # the guard skips it); a name with one that disagrees is the exact shape that shipped.
    $outLeaf = Split-Path -Leaf $Out
    $lastDash = $outLeaf.LastIndexOf('-')
    if ($lastDash -ge 0) {
        $tail = $outLeaf.Substring($lastDash + 1)
        # -cne, not -ne. PowerShell's default comparison is case-INSENSITIVE and the launcher's is
        # strncmp, which is not - so a name carrying uppercase hex would pass here and still be
        # rejected on the machine it shipped to, which is the worst of both.
        if ($tail.Length -ge 7 -and $tail.Substring(0, 7) -cne $sha.Substring(0, 7)) {
            Remove-Item -Recurse -Force $stage
            throw ("-Out names '$outLeaf', whose last dash-separated token starts '" +
                   $tail.Substring(0, 7) + "', but this payload's BUILD.txt says '" +
                   $sha.Substring(0, 9) + "'. The installer records the bundle's basename as an " +
                   "artifact name and the launcher compares the two on every start " +
                   "(gxlaunch.c:1609-1627), so this zip would make every launch discard its " +
                   "install record and re-download the whole payload, forever. Leave -Out unset " +
                   "and it is named '$bundleName'.")
        }
    }
}

# Packed with the same partial-then-rename and reuse discipline as the four artifacts, and for the
# same two reasons: an interrupted run must not leave a truncated zip under the final name for the
# next run to trust, and identical content must keep its bytes so its sha256 does not move.
if (Test-Path -LiteralPath $Out) {
    Write-Host ""
    Write-Host ("  bundle    : {0}  (unchanged, reused)" -f (Split-Path -Leaf $Out))
}
else {
    # Derived from $Out by substitution rather than by Split-Path/Join-Path: a bare "-Out foo.zip"
    # has no parent, and Join-Path with an empty parent throws on Windows PowerShell 5.1.
    #
    # ".partial.zip", NOT ".zip.partial" - 5.1's Compress-Archive appends .zip when the destination
    # lacks it, so the latter would write "...zip.partial.zip" and the Move-Item below would fail on
    # a path nothing wrote. Same reasoning as New-GxArtifact; keep the extension last.
    $bundlePartial = $Out -replace '\.zip$', '.partial.zip'
    if (Test-Path -LiteralPath $bundlePartial) { Remove-Item -LiteralPath $bundlePartial -Force }
    Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $bundlePartial `
        -CompressionLevel Optimal
    Move-Item -LiteralPath $bundlePartial -Destination $Out
}

Remove-Item -Recurse -Force $stage

# What publish-bootstrap.sh reads, so the operator does not retype four hashes. The bundle is
# listed too, under the role that tells the publisher it is the INSTALLER's payload and not a
# manifest entry - the distinction the one-row rule rests on.
$bundleItem = Get-Item -LiteralPath $Out
# THE BUNDLE'S OWN SHA-256, not $sha256 - which is the ENGINE EXE's, computed above for BUILD.txt.
# This row becomes the --bundle argument, which becomes the hash in gxsetup.txt, which Inno checks
# natively against the file it downloads. Publishing the exe's digest for the zip would make every
# installer fail that check and fall back to its stale compiled-in pin - silently, because falling
# back is the designed behaviour for a resolver that does not parse. Cross-model review catch.
$bundleSha = (Get-FileHash -LiteralPath $Out -Algorithm SHA256).Hash.ToLower()
$artifactList = Join-Path $ArtifactDir "gxartifacts.txt"
@(
    "# GeneralsX build artifacts. Written by package-windows-share.ps1; read by publish-bootstrap.sh."
    "# Format: <role> <platform> <filename> <sha256hex> <size>"
    "#"
    "# role 'bundle' is the COMPLETE payload and is the only thing gxsetup.txt may name - old"
    "# installers take the first win32 row of that file as the entire install. Every other role is a"
    "# manifest --build entry."
    "bundle win32 $($bundleItem.Name) $bundleSha $($bundleItem.Length)"
) + ($artifacts | ForEach-Object {
    "$($_.Role) $($_.Platform) $($_.Name) $($_.Sha256) $($_.Size)"
}) | Set-Content -Encoding ASCII $artifactList

Write-Host ""
Write-Host ("  bundle   : {0}" -f $Out)
Write-Host ("  size     : {0:N1} MB   <- what GeneralsX-Setup.exe downloads, one row in gxsetup.txt" -f ($bundleItem.Length / 1MB))
foreach ($a in $artifacts) {
    Write-Host ("  {0,-9}: {1,-34} {2,8:N1} MB" -f $a.Role, $a.Name, ($a.Size / 1MB))
}
Write-Host ("  update   : {0:N1} MB for an engine-only release" -f `
    (($artifacts | Where-Object { $_.Role -eq 'engine' }).Size / 1MB))
Write-Host ""
Write-Host ("  commit   : {0}" -f $sha)
Write-Host ("  exe sha256 : {0}" -f $sha256)
Write-Host ("  artifact list : {0}" -f $artifactList)
Write-Host ""
Write-Host "  Upload the bundle AND all four artifacts to the CDN, then sign a manifest naming the"
Write-Host "  four. Hand publish-bootstrap.sh the list rather than retyping any of it:"
Write-Host ""
Write-Host ("      scripts/tooling/publish-bootstrap.sh --artifacts {0} \" -f $artifactList)
Write-Host "          --relay HOST:PORT:REGION --bucket BUCKET --dry-run"
Write-Host ""
Write-Host "  People get GeneralsX-Setup.exe, not this. They need their own Zero Hour. No EA data"
Write-Host "  is in it."
