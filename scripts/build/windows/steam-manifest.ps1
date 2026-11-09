# READ-ONLY integrity manifest of the Steam Zero Hour install.
# GeneralsX @build Claude 27/07/2026
#
# Generals Online + Easy Anti-Cheat live in that folder. Our build must treat it as read-only
# game data. Run this BEFORE and AFTER a session and compare MANIFEST-SHA256 - a single changed
# byte is a bug, not an acceptable side effect.
#
#   powershell -ExecutionPolicy Bypass -File steam-manifest.ps1 -Out C:\dev\steam_BEFORE.txt
#   ... run the game ...
#   powershell -ExecutionPolicy Bypass -File steam-manifest.ps1 -Out C:\dev\steam_AFTER.txt
#
# ~3.1 GB over 404 files hashes in well under a minute, so there is no excuse for skipping it.
param([string]$Out = "C:\dev\steam_manifest.txt")

$root = "C:\Program Files (x86)\Steam\steamapps\common\Command & Conquer Generals - Zero Hour"
if (-not (Test-Path -LiteralPath $root)) { Write-Output "ERROR: root not found: $root"; exit 1 }

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$files = Get-ChildItem -LiteralPath $root -Recurse -File -Force -ErrorAction SilentlyContinue
Write-Output ("files: " + $files.Count)
Write-Output ("bytes: " + ($files | Measure-Object -Property Length -Sum).Sum)

$lines = New-Object System.Collections.Generic.List[string]
foreach ($f in $files) {
    $rel = $f.FullName.Substring($root.Length + 1)
    $h = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash
    $lines.Add("$h  $($f.Length)  $rel")
}
[System.IO.File]::WriteAllLines($Out, ($lines | Sort-Object))
$sw.Stop()

Write-Output ("manifest written: " + $Out)
Write-Output ("elapsed: " + [math]::Round($sw.Elapsed.TotalSeconds,1) + " s")
Write-Output ("MANIFEST-SHA256: " + (Get-FileHash -LiteralPath $Out -Algorithm SHA256).Hash)
