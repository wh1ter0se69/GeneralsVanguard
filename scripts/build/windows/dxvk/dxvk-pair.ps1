# GeneralsX @feature Claude 03/10/2026 WHICH DXVK PAIR A FOLDER HOLDS - dot-sourced by setup-run-win64.ps1,
# package-windows-share.ps1 and build-dxvk-win64.ps1. Windows PowerShell 5.1 compatible.
#
# Two pairs exist, both pinned in dxvk-win64.pins:
#   gx        upstream v2.6 + patches\ (the fixed-function constant fix: zoomed-out fps), built by build-dxvk-win64.ps1
#             into C:\dev\dxvk-win64\out\<gx_tag>. What players get from 0.9.54 on (0.9.53 was never released).
#   official  the DXVK 2.6 release's x64 pair (C:\dev\dxvk\dxvk-2.6\x64). The way back: setup-run-win64.ps1 -DxvkPair official.
#
# d3d8.dll and d3d9.dll ALWAYS come from one pair. DXVK's d3d8 drives d3d9 through a private interface, so a d3d8 from
# one build beside a d3d9 from another is an untested combination - a mixed folder is refused, never "mostly right".

$DxvkPinsFile = Join-Path $PSScriptRoot 'dxvk-win64.pins'	# captured at dot-source time: $PSScriptRoot is this file's folder here

function Read-DxvkPins([string]$Path = $DxvkPinsFile) {
	$p = @{}
	foreach ($line in Get-Content -LiteralPath $Path -ErrorAction Stop) {
		$l = ($line -split '#', 2)[0].Trim()
		if (-not $l) { continue }
		$kv = $l -split '\s*=\s*', 2
		if ($kv.Count -ne 2 -or -not $kv[1]) { throw "dxvk-win64.pins: malformed line '$line'" }
		$p[$kv[0]] = $kv[1]
	}
	$p
}

function Get-DxvkPairDefaultDir([ValidateSet('gx', 'official')][string]$Pair, [hashtable]$Pins = (Read-DxvkPins)) {
	if ($Pair -eq 'official') { 'C:\dev\dxvk\dxvk-2.6\x64' } else { "C:\dev\dxvk-win64\out\$($Pins['gx_tag'])" }
}

# 'gx' or 'official' when BOTH DLLs in $Dir are that pinned pair; throws otherwise (missing file, unknown or mixed pair).
function Get-DxvkPairName([string]$Dir, [hashtable]$Pins = (Read-DxvkPins)) {
	$h = @{}
	foreach ($f in 'd3d8', 'd3d9') {
		$p = Join-Path $Dir "$f.dll"
		if (-not (Test-Path -LiteralPath $p)) { throw "no $f.dll in $Dir" }
		$h[$f] = (Get-FileHash -Algorithm SHA256 -LiteralPath $p).Hash.ToLower()
	}
	foreach ($n in 'gx', 'official') {
		# an 'unset' pin (a gx pair not built and recorded yet) can never match a real hash
		if ($h['d3d8'] -eq $Pins["${n}_d3d8_sha256"] -and $h['d3d9'] -eq $Pins["${n}_d3d9_sha256"]) { return $n }
	}
	$which = foreach ($f in 'd3d8', 'd3d9') {
		$m = @('gx', 'official' | Where-Object { $h[$f] -eq $Pins["${_}_${f}_sha256"] })
		"$f.dll $($h[$f].Substring(0, 16)) = $(if ($m) { $m -join '/' } else { 'no pinned build' })"
	}
	throw "the DXVK DLLs in $Dir are not one pinned pair ($($which -join '; ')). Build the gx pair with scripts\build\windows\dxvk\build-dxvk-win64.ps1, or stage the official one (-DxvkPair official)."
}
