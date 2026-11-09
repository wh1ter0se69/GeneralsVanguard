# GeneralsX launcher packaging - the build body that runs ON the Windows box.
#
# Invoked as a FILE by package-launcher-windows.sh. Never Start-Process: over SSH it returns a PID
# and runs nothing, and the stale log that follows reads exactly like success.
#
# NSIS RATHER THAN WiX/MSI. The MSI bundle installs per-machine and elevates, so a launcher that
# replaces its own bytes would need elevation on every update. NSIS does the per-user install the
# existing Inno installer already uses, and is the smaller bundle-time download on a box with a
# documented TLS-fetch history.
param(
    [Parameter(Mandatory=$true)][string]$Commit9,
    [Parameter(Mandatory=$true)][string]$Product
)
$ErrorActionPreference = "Stop"

$env:Path = (Join-Path $env:USERPROFILE ".cargo\bin") + ";" + $env:Path
$env:CARGO_TARGET_DIR = "C:\dev\gx\rust-target"
$env:GX_COMMIT = $Commit9

# MEASURED DEVIATION FROM THE PLAN, 2026-08-20. The plan extracts the tarball to
# C:\dev\gx\src\launcher-rs. tauri.conf.json's build.frontendDist is "../../../design/launcher",
# which resolves OUTSIDE scripts/launcher-rs, so a tarball of launcher-rs alone does not carry the
# frontend and the build fails on the remote only. package-launcher-windows.sh therefore tars from
# the repository root, preserving repo-relative paths, and the crate lands one level deeper.
$src   = "C:\dev\gx\src\scripts\launcher-rs"
$crate = Join-Path $src "gxlauncher"
if (-not (Test-Path (Join-Path $crate "tauri.conf.json"))) {
    throw "no tauri.conf.json at $crate - the source tarball did not extract"
}

# A tar taken on macOS without COPYFILE_DISABLE=1 carries every file's extended attributes, and
# bsdtar here materialises each as a real "._<name>" file. Tauri globs capabilities/*.json, so
# "._default.json" is read as a capability and the build dies on "stream did not contain valid
# UTF-8" - a message that names neither tar nor macOS. Measured 2026-08-20: 77 such files.
# Refusing here rather than deleting them keeps the fault on the side that created it.
$appleDouble = @(Get-ChildItem -Recurse -Force -File "C:\dev\gx\src" |
                 Where-Object { $_.Name -like "._*" })
if ($appleDouble.Count -gt 0) {
    throw ("GATE FAIL: the source tarball carried {0} AppleDouble members, e.g. {1}. " +
           "Re-tar with COPYFILE_DISABLE=1; note that `tar tzf` on macOS lists none of them." `
           -f $appleDouble.Count, $appleDouble[0].FullName)
}

# GATE ON THE ARTIFACT. Delete the expected outputs, stamp the clock, and afterwards require each
# file to exist with an mtime at or after the stamp. An exit code proves nothing: a build that
# skips everything exits 0, and a previous run's exe sitting in place is indistinguishable from a
# fresh one by name and size alone.
$bundleRoot = Join-Path $env:CARGO_TARGET_DIR "release\bundle\nsis"
$rawExe     = Join-Path $env:CARGO_TARGET_DIR ("release\{0}.exe" -f $Product)
if (Test-Path $bundleRoot) { Remove-Item -Recurse -Force $bundleRoot }
if (Test-Path $rawExe)     { Remove-Item -Force $rawExe }
$stamp = (Get-Date).AddSeconds(-1)

Push-Location $crate
try {
    # The NSIS bundler requires a Windows .ico; the crate ships only icons/icon.png. `cargo tauri
    # icon` generates the whole set from that png and is part of the pinned CLI, so this needs no
    # extra toolchain. It is idempotent and cheap, and skipping it fails the bundle, not the build,
    # which is a confusing place to discover a missing file.
    if (-not (Test-Path (Join-Path $crate "icons\icon.ico"))) {
        & cargo tauri icon (Join-Path $crate "icons\icon.png")
        if ($LASTEXITCODE -ne 0) { throw "cargo tauri icon exited $LASTEXITCODE" }
    }
    & cargo tauri build --bundles nsis
    if ($LASTEXITCODE -ne 0) { throw "cargo tauri build exited $LASTEXITCODE" }
} finally { Pop-Location }

$exe = Get-ChildItem -LiteralPath $bundleRoot -Filter "*-setup.exe" -File -ErrorAction SilentlyContinue |
       Select-Object -First 1
if (-not $exe) { throw "GATE FAIL: the nsis bundler wrote no *-setup.exe under $bundleRoot" }
if ($exe.Length -lt 1048576) { throw "GATE FAIL: $($exe.FullName) is $($exe.Length) bytes, under the 1 MB floor" }
if ($exe.LastWriteTime -lt $stamp) { throw "GATE FAIL: $($exe.FullName) predates this build - it did not relink" }

if (-not (Test-Path $rawExe)) { throw "GATE FAIL: no unpacked exe at $rawExe" }
$raw = Get-Item $rawExe
if ($raw.LastWriteTime -lt $stamp) { throw "GATE FAIL: $rawExe predates this build - it did not relink" }

$out = "C:\dev\gx\out"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force -Path $out | Out-Null
$dest = Join-Path $out ("GeneralsXLauncher-Setup-{0}.exe" -f $Commit9)
Copy-Item -LiteralPath $exe.FullName -Destination $dest -Force
Copy-Item -LiteralPath $rawExe -Destination (Join-Path $out "GeneralsXLauncher.exe") -Force

$sha = (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash.ToLower()
Write-Host ("GATE OK {0} {1} {2}" -f $dest, (Get-Item $dest).Length, $sha)
