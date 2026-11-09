#!/bin/sh
# GeneralsX launcher packaging - Windows. Runs on this Mac, builds on the Windows box. One command.
#
#     scripts/build/launcher/package-launcher-windows.sh
#
# THIS BOX IS A HARD DEPENDENCY AND THAT IS A REGRESSION WORTH NAMING. The C# loader it replaces
# sets EnableWindowsTargeting so it could be built from macOS or Linux, producing an identical exe.
# Tauri cannot: the bundler is MSVC-only, the x86_64-pc-windows-gnu target installed on this Mac is
# unsupported, and mingw would not help. So the Windows artifact now requires the Windows box to be
# reachable and provisioned.
#
# THE SOURCE IS SHIPPED, NOT PULLED. A `git fetch && git reset --hard` on the remote is the
# documented sync, but this branch is not pushed and a stray local edit there silently blocks the
# reset. Tarring the source and extracting it is self-contained and cannot go stale.
#
# THREE MEASURED DEVIATIONS FROM THE 2026-08-20 PLAN, all proven on the box that day:
#
#   1. THE SSH DEFAULT SHELL ON THAT BOX IS POWERSHELL, NOT CMD.
#      HKLM:\SOFTWARE\OpenSSH\DefaultShell is powershell.exe, so every remote command is parsed by
#      PowerShell 5.1 first. The plan's `cmd /c mkdir ... & exit 0` and `cmd /c cd /d ... && tar`
#      one-liners die at parse time with "The ampersand (&) character is not allowed" and
#      "The token '&&' is not a valid statement separator" - before running anything. Remote
#      commands here are therefore PowerShell, which is also what a reader of this file will get if
#      they paste one by hand.
#
#   2. THE TARBALL MUST CARRY build.frontendDist, WHICH IS NOT UNDER scripts/launcher-rs.
#      tauri.conf.json points frontendDist outside the crate. A tarball of launcher-rs alone builds
#      on this Mac (where the sibling directory is present) and fails only on the remote, which is
#      the most expensive place to find out. The tar is taken from the repository ROOT over both
#      paths, so the relative link resolves identically on every host.
#
#   3. THE SOURCE TAR NEEDS COPYFILE_DISABLE=1, AND macOS tar HIDES THE EVIDENCE THAT IT DOES.
#      Without it, macOS bsdtar attaches every file's extended attributes, and bsdtar on Windows
#      materialises each one as a real "._<name>" file. Measured 2026-08-20: 77 of them, and the
#      build died on `failed to read file 'capabilities\._default.json': stream did not contain
#      valid UTF-8` - Tauri globs capabilities/*.json and the AppleDouble blob is not UTF-8. The
#      trap is that `tar tzf` ON THE MAC lists ZERO "._" members either way, because macOS bsdtar
#      merges them back on read; only the byte count of the archive differs. So this is invisible
#      from the side that creates it and fatal on the side that consumes it. The plan applies
#      COPYFILE_DISABLE=1 to the macOS SHIPPING tarball in Task 14 and to neither remote SOURCE
#      tarball; the Linux packager needs the same fix.
set -eu

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
export GX_REPO="$REPO"
HERE="$REPO/scripts/build/launcher"
. "$HERE/toolchain.env"
. "$HERE/lib-gate.sh"
# The launcher must say the release being staged (lib-gate.sh), before anything is built.
gx_release_version_gate || exit 1
[ -f "$REPO/scripts/test/local.env" ] ||
    { echo "error: no scripts/test/local.env - copy local.env.example and fill in WIN" >&2; exit 1; }
. "$REPO/scripts/test/local.env"
[ -n "${WIN:-}" ] || { echo "error: WIN is not set in scripts/test/local.env" >&2; exit 1; }

DIST="$REPO/dist/launcher"
mkdir -p "$DIST"
"$HERE/check-subsystem2.sh"
gx_require_clean "scripts/launcher-rs"
# the bundled notices are named for HEAD too: their inputs must be committed, not working-tree edits
gx_require_clean "THIRD_PARTY_NOTICES.txt"
gx_require_clean "resources/legal"
gx_require_clean "scripts/legal"
COMMIT9="$(gx_commit9)"

OUT_EXE="$DIST/GeneralsXLauncher-Setup-$COMMIT9.exe"
OUT_RAW="$DIST/GeneralsXLauncher-win32-$COMMIT9.exe"
gx_gate_begin "$OUT_EXE" "$OUT_RAW" "$DIST/win-selftest.json"

# The frontend directory is read out of tauri.conf.json rather than repeated here, so a subsystem-2
# decision to move the frontend into the crate needs no edit in this file.
FRONTEND_REL="$(python3 - "$REPO" <<'PY'
import json, os, sys
repo = sys.argv[1]
conf = os.path.join(repo, "scripts/launcher-rs/gxlauncher/tauri.conf.json")
dist = json.load(open(conf, encoding="utf-8"))["build"]["frontendDist"]
absdir = os.path.normpath(os.path.join(os.path.dirname(conf), dist))
rel = os.path.relpath(absdir, repo)
print("" if rel.startswith("scripts/launcher-rs") else rel)
PY
)"

TARBALL="$(mktemp -t gxsrc).tgz"
trap 'rm -f "$TARBALL"' EXIT
# Freshness gate: refuse to build a launcher whose notices file is stale (Cargo.lock / licence data moved without regenerating).
python3 -I "$REPO/scripts/legal/third_party_notices.py" check || exit 1
if [ -n "$FRONTEND_REL" ]; then
    ( cd "$REPO" && COPYFILE_DISABLE=1 tar czf "$TARBALL" scripts/launcher-rs LICENSE.md THIRD_PARTY_NOTICES.txt "$FRONTEND_REL" )
else
    ( cd "$REPO" && COPYFILE_DISABLE=1 tar czf "$TARBALL" scripts/launcher-rs LICENSE.md THIRD_PARTY_NOTICES.txt )
fi

SSH="ssh -o ConnectTimeout=30 -o BatchMode=yes -o ServerAliveInterval=30"
SCP="scp -q -o ConnectTimeout=30 -o BatchMode=yes"

echo "==> shipping source and build scripts to the Windows box"
$SSH "$WIN" 'if (Test-Path C:\dev\gx\src) { Remove-Item -Recurse -Force C:\dev\gx\src }; New-Item -ItemType Directory -Force -Path C:\dev\gx\src | Out-Null'
$SCP "$TARBALL" "$WIN:C:/dev/gx/src/launcher-rs.tgz"
$SCP "$HERE/remote-build-windows.ps1"    "$WIN:C:/dev/gx/remote-build-windows.ps1"
$SCP "$HERE/remote-selftest-windows.ps1" "$WIN:C:/dev/gx/remote-selftest-windows.ps1"
# tar.exe is bsdtar and has shipped in Windows since 10 1803. Confirmed present on this box.
$SSH "$WIN" 'Set-Location C:\dev\gx\src; & tar.exe -xzf launcher-rs.tgz; if ($LASTEXITCODE -ne 0) { throw "tar exited $LASTEXITCODE" }'

echo "==> building on the Windows box at $COMMIT9"
$SSH "$WIN" "powershell -NoProfile -ExecutionPolicy Bypass -File C:\\dev\\gx\\remote-build-windows.ps1 -Commit9 $COMMIT9 -Product \"$GX_PRODUCT\"" 2>&1 |
    tr -d '\r' | tee "$DIST/win-build.log"
grep -q '^GATE OK ' "$DIST/win-build.log" ||
    { echo "error: the Windows build printed no GATE OK line - it did not produce an artifact" >&2; exit 1; }

echo "==> smoke launching the unpacked exe on the Windows box, in session 1"
$SSH "$WIN" "powershell -NoProfile -ExecutionPolicy Bypass -File C:\\dev\\gx\\remote-selftest-windows.ps1 -ExePath C:\\dev\\gx\\out\\GeneralsXLauncher.exe" 2>&1 |
    tr -d '\r' > "$DIST/win-selftest.json"

# smoke-launch.sh runs a command; here the command already ran on another machine, so replay its
# captured line through the same validator. One implementation of the contract, not two.
cat > "$DIST/replay-win.sh" <<'REPLAY'
#!/bin/sh
cat "$(dirname "$0")/win-selftest.json"
REPLAY
chmod +x "$DIST/replay-win.sh"
# The forbidden prefix is the directory the exe is actually sitting in. "C:\Program Files" could
# never fire: NSIS installs per-user, and this run is a loose exe that was never installed at all.
# State written beside a loose exe under C:\dev\gx\out is exactly the mistake this catches.
"$HERE/smoke-launch.sh" \
    --expect-commit "$COMMIT9" --expect-platform win32 \
    --forbid-prefix 'C:\dev\gx\out' -- "$DIST/replay-win.sh"

echo "==> retrieving the artifacts"
$SCP "$WIN:C:/dev/gx/out/GeneralsXLauncher-Setup-$COMMIT9.exe" "$OUT_EXE"
$SCP "$WIN:C:/dev/gx/out/GeneralsXLauncher.exe" "$OUT_RAW"
gx_gate_assert "$OUT_EXE" 1048576
gx_gate_assert "$OUT_RAW" 1048576

# ADVISORY ONLY. The Windows launcher replaces GeneralsXLauncher.exe inside the existing win32
# runtime tier of package-windows-share.ps1, so it adds NO new manifest row and the win32 rows keep
# their count and their leading position in the signed document.
#
# GeneralsX @bugfix Claude 24/09/2026 THIS IS NOW THE ONLY WINDOWS LAUNCHER. setup-run-win64.ps1
# stages C:\dev\gx\out\GeneralsXLauncher.exe (left there by remote-build-windows.ps1) instead of
# building the retired C# loader, and both it and package-windows-share.ps1 refuse a C# exe.
{
    printf '# advisory: on the Windows box run setup-run-win64.ps1 (it stages %s as\n' \
        'C:\dev\gx\out\GeneralsXLauncher.exe'
    printf '# GeneralsXLauncher.exe), then package-windows-share.ps1. This launcher rides the\n'
    printf '# existing win32 runtime tier and adds no manifest row.\n'
} > "$DIST/gxartifacts-win32.launcher.txt"

echo
echo "  installer : $OUT_EXE"
echo "  unpacked  : $OUT_RAW"
