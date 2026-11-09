#!/bin/sh
# GeneralsX launcher packaging - launch what a player actually downloads.
#
#     scripts/build/launcher/smoke-shipping-artifacts.sh                 # all three platforms
#     scripts/build/launcher/smoke-shipping-artifacts.sh --only macos    # just this Mac's half
#
# Everything before this smokes the BUILD TREE. What ships is a .tar.gz, a .dmg, an NSIS installer
# and an AppImage that travelled over scp. Each of those is a separate opportunity to produce a
# file that installs cleanly and opens no window, and every one of them reaches a player before it
# reaches anyone here.
#
# --only exists because the three halves complete on three different schedules and on three
# different machines. With no argument this runs all of them, which is what package-launcher.sh
# calls; --only scopes BOTH the required-artifact check and the sections that run, so the macOS
# half can be proven on a Mac that has never seen a Windows or Linux artifact. It is a filter on
# what is checked, never a way to declare an unchecked platform good: each section still prints
# its own SHIPSMOKE OK line and the caller greps for the ones it required.
set -eu

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
export GX_REPO="$REPO"
HERE="$REPO/scripts/build/launcher"
. "$HERE/toolchain.env"
. "$HERE/lib-gate.sh"

WANT="macos win32 linux"
while [ $# -gt 0 ]; do
    case "$1" in
        --only) WANT="$(printf '%s' "$2" | tr ',' ' ')"; shift 2 ;;
        *) echo "error: unknown argument $1" >&2; exit 1 ;;
    esac
done
want() {
    for _w in $WANT; do [ "$_w" = "$1" ] && return 0; done
    return 1
}
for _w in $WANT; do
    case "$_w" in
        macos|win32|linux) ;;
        *) echo "error: --only takes macos, win32 or linux, not '$_w'" >&2; exit 1 ;;
    esac
done

DIST="$REPO/dist/launcher"

# WHICH artifact set is under test. Defaults to HEAD, which is right inside package-launcher.sh
# where the build and this run are one uninterrupted sequence. It is NOT right the moment anything
# commits in between - including committing the packaging scripts themselves, and including
# another session committing to this tree - because artifacts are named for HEAD at BUILD time and
# this would then look for a set that was never built and report it as "missing, run
# package-launcher.sh first".
#
# This selects a set; it does not certify one. The commit assertion lives in smoke-launch.sh and
# compares against the commit compiled INTO each binary, so naming an older set here still proves
# those exact binaries report that exact commit. There is no value of GX_COMMIT9 that makes a
# stale artifact pass as a fresh one.
COMMIT9="${GX_COMMIT9:-$(gx_commit9)}"

SSH="ssh -o ConnectTimeout=30 -o BatchMode=yes"
SCP="scp -q -o ConnectTimeout=30 -o BatchMode=yes"

TGZ="$DIST/GeneralsXLauncher-macos-$COMMIT9.tar.gz"
DMG="$DIST/GeneralsXLauncher-macos-$COMMIT9.dmg"
SETUP="$DIST/GeneralsXLauncher-Setup-$COMMIT9.exe"
IMG="$DIST/GeneralsXLauncher-linux-x86_64-$COMMIT9.AppImage"

NEED=""
want macos && NEED="$NEED $TGZ $DMG"
want win32 && NEED="$NEED $SETUP"
want linux && NEED="$NEED $IMG"
for f in $NEED; do
    [ -f "$f" ] || { echo "error: missing $f - run package-launcher.sh first" >&2; exit 1; }
done

# The remote hosts are a precondition only for the halves that actually use them. Requiring them
# unconditionally would make a Mac-only run fail with "WIN is not set", which says nothing about
# the artifacts being tested.
if want win32 || want linux; then
    . "$REPO/scripts/test/local.env"
fi
if want win32; then
    [ -n "${WIN:-}" ] || { echo "error: WIN is not set in scripts/test/local.env" >&2; exit 1; }
fi
if want linux; then
    [ -n "${GX_LINUX_BUILD_HOST:-}" ] ||
        { echo "error: GX_LINUX_BUILD_HOST is not set in scripts/test/local.env" >&2; exit 1; }
fi

WORK="$(mktemp -d)"
MNT=""
cleanup() {
    if [ -n "$MNT" ]; then hdiutil detach "$MNT" -quiet 2>/dev/null || true; fi
    rm -rf "$WORK"
}
trap cleanup EXIT

if want macos; then
# ---- macOS, from the tarball the updater will replace the app with -------------------------------
# COPYFILE_DISABLE=1 keeps AppleDouble "._" members out; without it the extracted bundle carries
# junk beside every file and its signature check is a coin flip. Nothing has ever checked that.
echo "==> extracting $TGZ"
mkdir -p "$WORK/tgz"
tar xzf "$TGZ" -C "$WORK/tgz"
EX_APP="$WORK/tgz/$GX_PRODUCT.app"
[ -d "$EX_APP" ] || { echo "SHIPSMOKE FAIL: the tarball contains no $GX_PRODUCT.app" >&2; exit 1; }

# The ARCHIVE's member list, not a walk of the extraction. Two reasons: `find` on this Mac is bfs,
# shimmed onto PATH ahead of /usr/bin/find and therefore in scope for a #!/bin/sh script, and it
# has already once reported "no matches" for a file that was plainly there. And the archive is the
# thing that ships - checking the extraction tests this machine's tar as much as the artifact.
#
# HONESTY ABOUT THIS CHECK: it has never been observed to fire here, and it structurally cannot
# with this toolchain. Measured 2026-08-20 on bsdtar 3.5.3 / libarchive 3.7.4: xattrs are written
# as pax SCHILY.xattr / LIBARCHIVE.xattr extended headers, and AppleDouble "._" members are never
# emitted as separate entries - with COPYFILE_DISABLE set OR unset. COPYFILE_DISABLE is still not
# a no-op (2,860,921 vs 2,861,304 bytes on the same bundle, so it does suppress provenance and
# quarantine xattrs) and it stays set, but the "junk beside every file" failure this was written
# for belongs to an older tar. Kept because it costs one pipe and would catch an archive built by
# GNU tar or by an older bsdtar; NOT relied on.
#
# The check that actually discriminates is `codesign --verify` below, and that one IS proven to
# fire: adding one unsealed file inside Contents/MacOS makes it exit 1 with "a sealed resource is
# missing or invalid". That is the gate protecting the extracted bundle.
if tar tzf "$TGZ" | grep -q '/\._'; then
    echo "SHIPSMOKE FAIL: the tarball carries AppleDouble ._ members" >&2
    tar tzf "$TGZ" | grep '/\._' | sed -n '1,5p' >&2
    exit 1
fi

codesign --verify --deep --strict "$EX_APP" ||
    { echo "SHIPSMOKE FAIL: the extracted app fails its own signature check" >&2; exit 1; }
"$HERE/smoke-launch.sh" --expect-commit "$COMMIT9" --expect-platform macos \
    --forbid-prefix "$EX_APP" --forbid-prefix "/private/var/folders" \
    -- "$EX_APP/Contents/MacOS/$GX_PRODUCT" >/dev/null
echo "SHIPSMOKE OK macos-tarball $(basename "$TGZ")"

# ---- macOS, from the disk image a human downloads -----------------------------------------------
echo "==> mounting $DMG"
MNT="$WORK/mnt"
mkdir -p "$MNT"
hdiutil attach -nobrowse -readonly -mountpoint "$MNT" "$DMG" >/dev/null
[ -d "$MNT/$GX_PRODUCT.app" ] ||
    { echo "SHIPSMOKE FAIL: the dmg contains no $GX_PRODUCT.app" >&2; exit 1; }
# Copied out first, because the mount is read-only and a launcher that cannot write its state
# directory would fail for the wrong reason.
mkdir -p "$WORK/dmg"
cp -R "$MNT/$GX_PRODUCT.app" "$WORK/dmg/"
hdiutil detach "$MNT" -quiet
MNT=""
"$HERE/smoke-launch.sh" --expect-commit "$COMMIT9" --expect-platform macos \
    --forbid-prefix "$WORK/dmg/$GX_PRODUCT.app" --forbid-prefix "/private/var/folders" \
    -- "$WORK/dmg/$GX_PRODUCT.app/Contents/MacOS/$GX_PRODUCT" >/dev/null
echo "SHIPSMOKE OK macos-dmg $(basename "$DMG")"
fi

# The replay shim: the remote halves cannot run smoke-launch.sh on this Mac against a remote
# process, so the remote prints its selftest line, it is captured to a file, and this cats it back
# so the SAME parser validates it. One implementation of the contract, three platforms.
cat > "$WORK/replay-remote.sh" <<'REPLAY'
#!/bin/sh
cat "$1"
REPLAY
chmod +x "$WORK/replay-remote.sh"

if want win32; then
# ---- Windows, from the installer, as installed ---------------------------------------------------
echo "==> installing the NSIS setup on the Windows box and launching what it installed"
$SCP "$SETUP" "$WIN:C:/dev/gx/out/shipsmoke-setup.exe"
$SCP "$HERE/remote-shipsmoke-windows.ps1" "$WIN:C:/dev/gx/remote-shipsmoke-windows.ps1"
# Shipped again here rather than assumed to be left over from the Windows packaging run, so this
# section stands alone when only the shipping smoke is re-run.
$SCP "$HERE/remote-selftest-windows.ps1"  "$WIN:C:/dev/gx/remote-selftest-windows.ps1"
$SSH "$WIN" "powershell -NoProfile -ExecutionPolicy Bypass -File C:\\dev\\gx\\remote-shipsmoke-windows.ps1 -SetupPath C:\\dev\\gx\\out\\shipsmoke-setup.exe -Product \"$GX_PRODUCT\" -BundleId \"$GX_BUNDLE_ID\"" 2>&1 |
    tr -d '\r' | tee "$WORK/win-install.log"
grep -q '^INSTALLED ' "$WORK/win-install.log" ||
    { echo "SHIPSMOKE FAIL: the Windows installer reported no INSTALLED path" >&2; exit 1; }
INSTALLED="$(sed -n 's/^INSTALLED //p' "$WORK/win-install.log" | head -1)"
INSTALLDIR="$(sed -n 's/^INSTALLDIR //p' "$WORK/win-install.log" | head -1)"

$SSH "$WIN" "powershell -NoProfile -ExecutionPolicy Bypass -File C:\\dev\\gx\\remote-selftest-windows.ps1 -ExePath \"$INSTALLED\"" 2>&1 |
    tr -d '\r' > "$DIST/win-shipsmoke.json"
# The install directory is the forbidden prefix here, and unlike "C:\Program Files" it is a path
# the exe is genuinely running from: the next update replaces that directory wholesale, so a
# launcher keeping its config there loses every setting on the first self-update.
"$HERE/smoke-launch.sh" --expect-commit "$COMMIT9" --expect-platform win32 \
    --forbid-prefix "$INSTALLDIR" \
    -- "$WORK/replay-remote.sh" "$DIST/win-shipsmoke.json" >/dev/null
# THE UNINSTALL IS WAITED FOR AND THEN VERIFIED, AND THE PLAN DID NEITHER.
# `& '<dir>\uninstall.exe' /S` does not wait: PowerShell's call operator only blocks on a
# CONSOLE-subsystem process, and an NSIS uninstaller is GUI-subsystem, so it returns instantly,
# the ssh session closes, and the child dies with it. Measured 2026-08-20 on the install half of
# this same pair: `& $SetupPath /S` left $LASTEXITCODE unset and installed nothing at all.
# And `|| true` on an unverified uninstall means a failure leaves this box carrying a stray
# per-user install that nobody is ever told about - so the removal is asserted, not hoped for.
#
# No PowerShell variable may appear in the command string: the remote's ssh shell IS PowerShell,
# so it expands $-names before the inner `powershell -Command` sees them.
$SSH "$WIN" "powershell -NoProfile -Command \"Write-Output ('UNINSTALL EXITCODE ' + (Start-Process -FilePath '$INSTALLDIR\\uninstall.exe' -ArgumentList '/S' -Wait -PassThru).ExitCode)\"" 2>&1 | tr -d '\r'
$SSH "$WIN" "powershell -NoProfile -Command \"Start-Sleep -Seconds 2; Write-Output ('POST uninstallkey=' + (Test-Path 'HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\$GX_PRODUCT')); Write-Output ('POST installdir=' + (Test-Path '$INSTALLDIR'))\"" 2>&1 |
    tr -d '\r' | tee "$WORK/win-uninstall.log"
grep -q 'POST uninstallkey=False' "$WORK/win-uninstall.log" ||
    { echo "SHIPSMOKE FAIL: the uninstall left its registry key behind" >&2; exit 1; }
grep -q 'POST installdir=False' "$WORK/win-uninstall.log" ||
    { echo "SHIPSMOKE FAIL: the uninstall left $INSTALLDIR behind" >&2; exit 1; }
echo "SHIPSMOKE OK win32-installed $(basename "$SETUP")"
fi

if want linux; then
# ---- Linux, from the file that travelled back and forth ------------------------------------------
# Sending the RETRIEVED AppImage back proves the bytes on this Mac - the ones that will be uploaded
# - are what launches, and that the executable bit survives. The build-tree run in Task 9 proves
# neither. FUSE is not exercised: see this task's owner-confirmation banner.
echo "==> sending the retrieved AppImage back and running it from a clean directory"
H="$GX_LINUX_BUILD_HOST"
$SSH "$H" 'rm -rf /root/gxbuild/shipsmoke && mkdir -p /root/gxbuild/shipsmoke'
$SCP "$IMG" "$H:/root/gxbuild/shipsmoke/$(basename "$IMG")"
# TWO CORRECTIONS TO THE PLAN'S LINUX SECTION, both measured on the build host 2026-08-20:
#
#   LIBGL_ALWAYS_SOFTWARE=1. Without it Mesa's DRI3 probe fails under Xvfb and writes two
#   "libEGL warning" lines TO STDOUT - selftest.err was empty and selftest.json held three lines -
#   so smoke-launch.sh refused with "not exactly one line of output" on a window that had genuinely
#   painted. Forcing llvmpipe silences it at the source, which keeps the one-line contract a real
#   check rather than something the parser has been taught to ignore.
#
#   2>&1 IS REPLACED BY A SEPARATE STDERR FILE. Merging the remote's stderr into the file that is
#   then parsed as the selftest line guarantees the same "not exactly one line" failure the moment
#   anything - a GTK warning, an ssh notice - reaches stderr, and reports it as a broken launcher.
#   remote-build-linux.sh already keeps the two streams apart; this is the same fix on this side,
#   and the stderr is kept as evidence rather than discarded.
$SSH "$H" "cd /root/gxbuild/shipsmoke && chmod +x '$(basename "$IMG")' && \
  LIBGL_ALWAYS_SOFTWARE=1 WEBKIT_DISABLE_COMPOSITING_MODE=1 WEBKIT_DISABLE_DMABUF_RENDERER=1 \
  xvfb-run -a -s '-screen 0 1280x800x24' \
  './$(basename "$IMG")' --appimage-extract-and-run --gx-selftest" 2>"$DIST/linux-shipsmoke.err" |
    tr -d '\r' > "$DIST/linux-shipsmoke.json"
"$HERE/smoke-launch.sh" --expect-commit "$COMMIT9" --expect-platform linux \
    --forbid-prefix '/tmp/appimage_extracted_' \
    --forbid-prefix '/tmp/.mount_' \
    --forbid-prefix '/root/gxbuild/shipsmoke' \
    -- "$WORK/replay-remote.sh" "$DIST/linux-shipsmoke.json" >/dev/null
echo "SHIPSMOKE OK linux-transferred $(basename "$IMG")"
fi

echo
echo "every shipping artifact checked ($WANT) opened a window at $COMMIT9"
