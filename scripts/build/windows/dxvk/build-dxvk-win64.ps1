# Copyright 2026 GeneralsX contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the Windows DXVK pair (d3d8.dll + d3d9.dll) players get - reproducibly.
#
# WHAT: upstream DXVK v2.6 + patches\*.patch, in name order:
#   0001-0002  build fixes only: the <cstddef> include and upstream 62b99d7b0f. v2.6 does not compile with GCC 16 and
#              MinGW-w64 14 headers without them; neither changes generated code paths the game uses.
#   0003-0006  the fixed-function constant fix (GeneralsX fork branch gx/ff-constants, ported to v2.6 line for line):
#              sub-allocate FF VS constants instead of renaming a buffer per draw, cache InverseView and the view-space
#              lights, keyed on the FP control state (0006: its reader chosen by util_bit.h's architecture, so ARM64EC
#              compiles; x86/x64 code unchanged). Zoomed out it is +11-18% fps on a 9950X3D/RTX 5090 and +11-22% on an
#              M4 Mini; the constants are bit-identical to stock (oracle,
#              255M uploads, 0 mismatches).
# WHY BUILD IT HERE: macOS/iOS compile DXVK from references/fbraz3-dxvk inside cmake; Windows shipped the official
# release DLLs, so the fix never reached Windows. This script is the ONLY way the Windows pair is made.
#
# REPRODUCIBLE, AND GATED ON IT. Every input is pinned in dxvk-win64.pins - the upstream commit, the patches (their
# applied HEAD sha), the WinLibs/meson/glslang downloads by sha256 - and so is the OUTPUT: both DLLs must hash to the
# pinned values or the build FAILS and nothing is published to out\. Rebuilding on another day, in another folder, on
# another box with the same inputs gives the same bytes; a changed input is caught as a changed hash, not shipped.
# What makes it deterministic, each one measured to matter or cheap insurance:
#   * the patches are applied with a fixed committer and --committer-date-is-author-date, so the commit shas - and
#     with them DXVK_VERSION, `git describe` = v2.6-6-g<sha> - are the same every time (plain `git am` stamps "now");
#   * SOURCE_DATE_EPOCH = that HEAD's commit time: binutils' PE linker stamps it as the DLL's TimeDateStamp instead of the
#     wall clock (measured on v2.6-gx1, whose last patch was then 0005: its d3d9.dll carries 2026-10-02 21:10:27Z = 0005's
#     author date; with 0006 the stamp is 0006's author date);
#   * core.abbrev=12, so `git describe` does not grow a character as upstream's object count grows;
#   * core.autocrlf=false, so the tree is upstream's bytes whatever this box's git defaults to;
#   * the toolchain is a pinned WinLibs zip unpacked under $Root, never whatever gcc is on PATH (winget upgrades it);
#   * a fresh work folder per run (a different path each time, so a path leaking into the binary fails the gate).
# The version string also marks the DLLs as altered builds (zlib licence: "plainly marked"): DXVK logs it at start.
#
# Usage, on the Windows box (no admin; nothing outside $Root is written; PATH is changed for this process only):
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build\windows\dxvk\build-dxvk-win64.ps1 [-Root C:\dev\dxvk-win64]
# Last output line: "DXVKWIN64 OK tag=.. version=.. d3d8=.. d3d9=.. out=<dir>", "DXVKWIN64 UNPINNED d3d8=.. d3d9=.." (pins
# not recorded yet: nothing published) or "DXVKWIN64 FAIL <why>". Log: $Root\logs\<run>.log. Then setup-run-win64.ps1
# stages out\<gx_tag> (its default -DxvkPair gx); -DxvkPair official stages the official 2.6 pair instead.
param(
	[string]$Root = 'C:\dev\dxvk-win64',
	[string]$Python = ''	# a python.exe for meson's venv; default: the first working `python` on PATH
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'	# progress records come back over ssh as CLIXML and bury the result line
. (Join-Path $PSScriptRoot 'dxvk-pair.ps1')
$run = (Get-Date).ToString('yyyyMMdd-HHmmss')
$work = "$Root\work-$run"
New-Item -ItemType Directory -Force "$Root\logs", "$Root\toolchain", $work | Out-Null
$log = "$Root\logs\$run.log"
function Step($m) { "$(Get-Date -Format o) $m" | Add-Content -LiteralPath $log }
# Native tools through cmd /c: under 'Stop', Windows PowerShell turns any native stderr line (a gcc warning) into a
# terminating error. Every path here is space-free (checked below), so cmd needs no inner quotes.
function Native([string]$line, [string]$what) { Step "run: $what"; cmd /c "$line >> $log 2>&1"; if ($LASTEXITCODE) { throw "$what rc=$LASTEXITCODE (log $log)" } }
function Sha([string]$p) { (Get-FileHash -Algorithm SHA256 -LiteralPath $p).Hash.ToLower() }
function Fetch([string]$url, [string]$sha, [string]$dest) {
	if ((Test-Path -LiteralPath $dest) -and (Sha $dest) -eq $sha) { return }
	Step "download $url"
	Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile "$dest.part"
	$h = Sha "$dest.part"
	if ($h -ne $sha) { throw "$url sha256 $h, pinned $sha" }
	Move-Item -Force -LiteralPath "$dest.part" -Destination $dest
}
try {
	if ($Root -match '\s') { throw "-Root must not contain spaces ($Root)" }
	(Get-Process -Id $PID).PriorityClass = 'BelowNormal'	# inherited by every compiler process: the box may be in use
	$pins = Read-DxvkPins
	foreach ($k in 'gx_tag', 'dxvk_url', 'dxvk_tag', 'dxvk_commit', 'patched_head', 'gx_d3d8_sha256', 'gx_d3d9_sha256', 'winlibs_url',
		'winlibs_sha256', 'gcc_version', 'meson_version', 'meson_sha256', 'glslang_url', 'glslang_sha256') {
		if (-not $pins[$k]) { throw "dxvk-win64.pins has no $k" }
	}
	$patches = @(Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'patches') -Filter *.patch | Sort-Object Name)
	if (-not $patches) { throw 'no patches\*.patch beside this script' }
	if ($PSScriptRoot -match '\s') { throw "this checkout's path has spaces ($PSScriptRoot): cmd /c would split it" }
	Step "run $run root=$Root patches=$(($patches | ForEach-Object Name) -join ',')"

	# 1) toolchain, all under $Root\toolchain, each verified by its pinned sha256
	$tc = "$Root\toolchain"
	$wzip = "$tc\$(Split-Path -Leaf $pins['winlibs_url'])"
	$wdir = "$tc\winlibs-$($pins['winlibs_sha256'].Substring(0, 12))"
	if (-not (Test-Path -LiteralPath "$wdir\.complete")) {
		Fetch $pins['winlibs_url'] $pins['winlibs_sha256'] $wzip
		if (Test-Path -LiteralPath $wdir) { throw "$wdir exists without .complete (an interrupted unpack): remove it and rerun" }
		New-Item -ItemType Directory -Force $wdir | Out-Null
		Native "tar -xf $wzip -C $wdir" 'unpack WinLibs'	# Windows' bsdtar reads zip, and is far faster than Expand-Archive
		Set-Content -LiteralPath "$wdir\.complete" -Value $pins['winlibs_sha256']
	}
	$gdir = "$tc\glslang-$($pins['glslang_sha256'].Substring(0, 12))"
	if (-not (Test-Path -LiteralPath "$gdir\.complete")) {
		$gzip = "$tc\$(Split-Path -Leaf $pins['glslang_url'])"
		Fetch $pins['glslang_url'] $pins['glslang_sha256'] $gzip
		if (Test-Path -LiteralPath $gdir) { throw "$gdir exists without .complete (an interrupted unpack): remove it and rerun" }
		New-Item -ItemType Directory -Force $gdir | Out-Null
		Native "tar -xf $gzip -C $gdir" 'unpack glslang'
		Set-Content -LiteralPath "$gdir\.complete" -Value $pins['glslang_sha256']
	}
	$venv = "$tc\meson-$($pins['meson_version'])"
	if (-not (Test-Path -LiteralPath "$venv\Scripts\meson.exe")) {
		if (-not $Python) { $Python = (Get-Command python -ErrorAction SilentlyContinue).Source }
		if (-not $Python) { throw 'no python on PATH: pass -Python <python.exe>' }
		if ($Python -match '\s') { throw "python path has spaces ($Python): cmd /c would split it - pass -Python <a space-free python.exe>" }
		Native "$Python --version" 'python runs (not the WindowsApps store alias)'
		Native "$Python -m venv $venv" 'meson venv'
		Set-Content -Encoding ascii -LiteralPath "$venv\req.txt" -Value "meson==$($pins['meson_version']) --hash=sha256:$($pins['meson_sha256'])"
		Native "$venv\Scripts\python.exe -m pip install -q --disable-pip-version-check --require-hashes --no-deps --only-binary :all: -r $venv\req.txt" 'pip meson (hash-pinned)'
	}
	$mingw = @(Get-ChildItem -LiteralPath $wdir -Directory -Filter mingw64)[0].FullName
	$glsl = @(Get-ChildItem -Path $gdir -Recurse -File -Include glslang.exe, glslangValidator.exe)[0]	# -Include needs -Path
	if (-not $mingw -or -not $glsl) { throw "toolchain incomplete (mingw64=$mingw glslang=$glsl)" }
	$env:PATH = "$venv\Scripts;$($glsl.DirectoryName);$mingw\bin;$env:PATH"	# this process and its children only
	$gpp = (Get-Command x86_64-w64-mingw32-g++).Source
	if (-not $gpp.StartsWith("$mingw\")) { throw "x86_64-w64-mingw32-g++ resolves to $gpp, not the pinned WinLibs in $mingw" }
	$gccv = (& x86_64-w64-mingw32-g++ -dumpfullversion).Trim()
	if ($gccv -ne $pins['gcc_version']) { throw "WinLibs g++ is $gccv, pinned $($pins['gcc_version'])" }
	$mesonv = (& meson --version).Trim()
	if ($mesonv -ne $pins['meson_version']) { throw "meson is $mesonv, pinned $($pins['meson_version'])" }
	Step "toolchain gcc=$gccv meson=$mesonv ninja=$((& ninja --version).Trim()) glslang=$($glsl.FullName)"

	# 2) source: a clean upstream clone at the pinned commit + the patches, applied deterministically
	$src = "$work\src"
	Native "git -c core.autocrlf=false -c core.abbrev=12 clone -q -c core.autocrlf=false -c core.abbrev=12 --branch $($pins['dxvk_tag']) --recurse-submodules $($pins['dxvk_url']) $src" 'clone upstream'
	$head = (git -C $src rev-parse HEAD).Trim()
	if ($head -ne $pins['dxvk_commit']) { throw "$($pins['dxvk_tag']) is $head upstream, pinned $($pins['dxvk_commit'])" }
	# env, not -c user.*: GIT_COMMITTER_* in this box's environment would beat -c and move every sha
	$env:GIT_COMMITTER_NAME = 'GeneralsX'; $env:GIT_COMMITTER_EMAIL = 'dxvk-build@generalsx.invalid'
	Remove-Item Env:GIT_COMMITTER_DATE -ErrorAction SilentlyContinue
	# countermand the user-level settings that change what git am
	# commits - commit.gpgSign (a signature, or no key = failure), am.messageid (a Message-Id line), mailinfo.scissors.
	Native "git -C $src am -q --no-gpg-sign --no-message-id --no-scissors --committer-date-is-author-date $(($patches | ForEach-Object FullName) -join ' ')" 'git am patches'
	$head = (git -C $src rev-parse HEAD).Trim()
	if ($head -ne $pins['patched_head']) { throw "patched HEAD is $head, pinned $($pins['patched_head']) - a patch or its metadata changed: rebuild twice and re-pin" }
	$dirty = @(git -C $src status --porcelain --ignore-submodules=none)
	if ($dirty) { throw "source tree not clean after git am: $($dirty -join '; ')" }
	$version = (git -C $src describe --dirty=+).Trim()	# what meson's vcs_tag bakes into DXVK_VERSION
	$env:SOURCE_DATE_EPOCH = (git -C $src log -1 --format=%ct).Trim()

	# 3) build with DXVK's own release options (package-release.sh: release, strip, b_ndebug=if-release). WinLibs ships
	#    no x86_64-w64-mingw32-prefixed ar/strip/windres, which DXVK's build-win64.txt names: same file, plain names.
	$cross = "$work\cross-winlibs.txt"
	Set-Content -Encoding ascii -LiteralPath $cross -Value @('[binaries]', "c = 'x86_64-w64-mingw32-gcc'", "cpp = 'x86_64-w64-mingw32-g++'",
		"ar = 'ar'", "strip = 'strip'", "windres = 'windres'", '', '[properties]', 'needs_exe_wrapper = true', '',
		'[host_machine]', "system = 'windows'", "cpu_family = 'x86_64'", "cpu = 'x86_64'", "endian = 'little'")
	$bd = "$work\build"
	Native "meson setup --cross-file $cross --buildtype release --strip -Db_ndebug=if-release $bd $src" 'meson setup'
	Native "ninja -C $bd" 'ninja'

	# 4) gate: the version string we expect, no test oracle, and the pinned bytes
	$outw = "$work\out"
	New-Item -ItemType Directory -Force $outw | Out-Null
	Copy-Item -LiteralPath "$bd\src\d3d8\d3d8.dll", "$bd\src\d3d9\d3d9.dll" -Destination $outw
	$d9 = [Text.Encoding]::GetEncoding(28591).GetString([IO.File]::ReadAllBytes("$outw\d3d9.dll"))
	if (-not $d9.Contains($version)) { throw "d3d9.dll does not carry the version $version" }
	if ($d9.Contains('GX_FFVS_VERIFY')) { throw 'd3d9.dll carries the GX_FFVS_VERIFY test oracle: a meas patch is in patches\' }
	$h8 = Sha "$outw\d3d8.dll"; $h9 = Sha "$outw\d3d9.dll"
	Step "built version=$version d3d8=$h8 d3d9=$h9 in $outw"
	if ($pins['gx_d3d8_sha256'] -eq 'unset' -or $pins['gx_d3d9_sha256'] -eq 'unset') {
		"DXVKWIN64 UNPINNED version=$version d3d8=$h8 d3d9=$h9 work=$work (nothing published: build twice, then record both in dxvk-win64.pins)"
		exit 3
	}
	if ($h8 -ne $pins['gx_d3d8_sha256'] -or $h9 -ne $pins['gx_d3d9_sha256']) {
		throw "output differs from the pins: d3d8=$h8 d3d9=$h9 (pinned $($pins['gx_d3d8_sha256']) / $($pins['gx_d3d9_sha256'])). Not reproducible, or an unpinned input changed. Kept: $work"
	}
	# 5) publish: $Root\out\<gx_tag> holds exactly the pinned pair (a stale or partial copy there is replaced, never mixed).
	#    Under the default -Root that is the folder setup-run-win64.ps1 -DxvkPair gx stages from (Get-DxvkPairDefaultDir);
	#    any other -Root is a scratch build and publishes only beneath itself.
	$out = "$Root\out\$($pins['gx_tag'])"
	New-Item -ItemType Directory -Force $out | Out-Null
	Copy-Item -Force -LiteralPath "$outw\d3d8.dll", "$outw\d3d9.dll" -Destination $out
	if ((Get-DxvkPairName $out $pins) -ne 'gx') { throw "$out does not hold the gx pair after the copy" }
	Remove-Item -Recurse -Force -LiteralPath $work	# this run's own scratch; kept on every failure above
	"DXVKWIN64 OK tag=$($pins['gx_tag']) version=$version d3d8=$h8 d3d9=$h9 out=$out"
} catch {
	Step "FAIL $_"
	"DXVKWIN64 FAIL $_"
	exit 1
}
