# GeneralsX launcher packaging - provision the Windows build box.
#
# This box already has what is awkward: MSVC C++ tools (it builds the engine) and the WebView2
# Evergreen Runtime. The whole delta is Rust. Tauri requires the MSVC toolchain: the
# x86_64-pc-windows-gnu target is not supported by the bundler, which is why the Windows artifact
# cannot be produced from the Mac even though that Mac has a windows-gnu target installed.
#
# The tauri-cli version is a PARAMETER, read from toolchain.env by the caller, so there is exactly
# one place in the repository where it is written down.
#
# Invoked as a FILE, never as a piped block:
#   powershell -NoProfile -ExecutionPolicy Bypass -File C:\dev\gx\provision-windows.ps1 -TauriCliVersion 2.11.4
param(
    [Parameter(Mandatory=$true)][string]$TauriCliVersion
)
$ErrorActionPreference = "Stop"

New-Item -ItemType Directory -Force -Path "C:\dev\gx" | Out-Null
New-Item -ItemType Directory -Force -Path "C:\dev\gx\rust-target" | Out-Null

$cargoBin = Join-Path $env:USERPROFILE ".cargo\bin"
if (-not (Test-Path (Join-Path $cargoBin "cargo.exe"))) {
    $init = "C:\dev\gx\rustup-init.exe"
    Invoke-WebRequest -Uri "https://static.rust-lang.org/rustup/dist/x86_64-pc-windows-msvc/rustup-init.exe" -OutFile $init
    # -y non-interactive; the default host triple on this box is already x86_64-pc-windows-msvc,
    # which is the one Tauri supports. Naming it anyway so a future host change is loud.
    & $init -y --default-host x86_64-pc-windows-msvc --default-toolchain stable --profile minimal
    if ($LASTEXITCODE -ne 0) { throw "rustup-init exited $LASTEXITCODE" }
}

$env:Path = "$cargoBin;$env:Path"
[Environment]::SetEnvironmentVariable("Path", "$cargoBin;" +
    [Environment]::GetEnvironmentVariable("Path", "User"), "User")

if (-not (Test-Path (Join-Path $cargoBin "cargo-tauri.exe"))) {
    $env:CARGO_TARGET_DIR = "C:\dev\gx\rust-target"
    & cargo install --locked ("tauri-cli@{0}" -f $TauriCliVersion)
    if ($LASTEXITCODE -ne 0) { throw "cargo install tauri-cli exited $LASTEXITCODE" }
}

# GATE ON THE ARTIFACT. `cargo install` exits 0 when the crate is already present, so the only
# evidence that matters is the exe existing and answering.
$tauriExe = Join-Path $cargoBin "cargo-tauri.exe"
if (-not (Test-Path $tauriExe)) { throw "no cargo-tauri.exe at $tauriExe after install" }

& rustc --version
& cargo tauri --version
& rustup show active-toolchain
