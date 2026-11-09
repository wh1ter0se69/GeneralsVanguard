#!/bin/sh
# GeneralsX launcher packaging - macOS. Runs natively on this Mac. One command.
#
#     scripts/build/launcher/package-launcher-macos.sh
#
# Tauri cannot cross-compile: its own documentation says the macOS bundle must be built "in a Mac
# computer". This is that machine.
#
# SIGNING, HONESTLY. This Mac holds exactly one codesigning identity and it is an
# "Apple Development" certificate from a free Personal Team. Gatekeeper accepts a direct download
# only when it is NOTARIZED, notarization requires a Developer ID Application certificate, and
# Tauri's own documentation states the free plan cannot notarize. Signing with the Development
# certificate would satisfy nothing and would embed a team identifier for no gain. So
# APPLE_SIGNING_IDENTITY is "-": ad-hoc, which is what the shipped GeneralsXZH binary already is
# (codesign reports flags=0x20002 adhoc,linker-signed), and which the launcher's own self-update
# depends on - an ad-hoc binary with no team identifier can be replaced freely.
# docs/HOWTO/LAUNCHER_FIRST_LAUNCH.md carries the one-time unblock the player performs instead.
set -eu

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
export GX_REPO="$REPO"
HERE="$REPO/scripts/build/launcher"
. "$HERE/toolchain.env"
. "$HERE/lib-gate.sh"
# The launcher must say the release being staged (lib-gate.sh), before anything is built.
gx_release_version_gate || exit 1

CRATE="$REPO/scripts/launcher-rs/gxlauncher"
DIST="$REPO/dist/launcher"
export CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-$GX_TARGET_DIR_MACOS}"

# THE WINDOW SERVER IS A PRECONDITION, and without this check its absence surfaces as
# "SMOKE FAIL: --gx-selftest did not exit within 90s" - indistinguishable from a broken bundle.
# Measured in a Terminal on this Mac's console 2026-08-20: `launchctl managername` prints `Aqua`.
# Over an ssh login, from a LaunchDaemon, or in a CI shell it prints something else and no window
# can be created at all.
MGR="$(launchctl managername 2>/dev/null || echo none)"
[ "$MGR" = "Aqua" ] || {
    echo "error: launchctl managername is '$MGR', not 'Aqua'." >&2
    echo "       This session has no window server, so --gx-selftest cannot create a window and" >&2
    echo "       the failure would look like a broken bundle. Run this from a Terminal on the" >&2
    echo "       Mac's own console, not over ssh." >&2
    exit 1
}

# THE TARGET DIRECTORY IS CHECKED FOR SPACE, NOT FOR IDENTITY.
#
# This gate used to REQUIRE /Volumes/Build to be mounted and refuse any CARGO_TARGET_DIR outside
# /Volumes, with GX_ALLOW_INTERNAL_TARGET=1 as an opt-in escape hatch. That external scratch SSD
# was physically disconnected on 2026-08-20 and is not coming back, so the default path refused
# every macOS build on the only machine that can produce one - a dead dependency wearing a safety
# check's message. Requiring a named volume also protected nothing on that volume: /Volumes/Build
# itself was never checked for free space at all.
#
# What the mount requirement was standing in for was never the volume, it was the ABSENCE of a
# free-space check. An unmounted mount point is an ordinary empty directory on the boot volume, so
# a build aimed at one silently fills the internal disk. So the identity check is replaced by the
# measurement it was a proxy for, and that measurement is now UNCONDITIONAL - it applies to an
# external volume exactly as it applies to $HOME.
#
# THE FLOOR IS DERIVED, NOT GUESSED. A release target for this crate measures ~1.2 GiB (recorded
# here 2026-08-20) and macOS gets unstable under ~1 GiB free, so 3 GiB is those two numbers plus
# room for the .app, the .dmg and the .tar.gz. Measured on this Mac 2026-08-21 building 8a57b66a0
# over a warm 401 MiB target: free space went 4 GiB -> 3 GiB, i.e. the incremental build cost about
# 1 GiB, and a from-cold build is the ~1.2 GiB above. Raise this only alongside a fresh
# measurement written into this comment.
GX_MIN_FREE_GB="${GX_MIN_FREE_GB:-3}"

# A path under /Volumes whose volume is NOT mounted is the exact trap described above. Refuse it by
# name, because df would otherwise cheerfully report the boot volume's free space for a directory
# that is in fact on the boot volume, and the build would fill the internal disk while passing the
# floor. grep -F and the " on <vol> (" framing so a volume name is matched whole, not as a prefix.
case "$CARGO_TARGET_DIR" in
    /Volumes/*)
        _vol="/Volumes/$(printf '%s' "${CARGO_TARGET_DIR#/Volumes/}" | cut -d/ -f1)"
        mount | grep -qF " on $_vol (" || {
            echo "error: CARGO_TARGET_DIR is $CARGO_TARGET_DIR but $_vol is not mounted." >&2
            echo "       That path is an ordinary directory on the boot volume, so building into" >&2
            echo "       it would fill the internal disk. Mount the volume, or point" >&2
            echo "       CARGO_TARGET_DIR at somewhere with space." >&2
            exit 1
        } ;;
esac

# df needs a path that exists, and creating the target dir first would create the very directory
# the check above exists to refuse. Walk up to the nearest existing ancestor instead.
_probe="$CARGO_TARGET_DIR"
while [ ! -d "$_probe" ] && [ "$_probe" != "/" ]; do _probe="$(dirname "$_probe")"; done
_avail_gb="$(df -g "$_probe" | awk 'NR==2{print $4}')"
[ "${_avail_gb:-0}" -ge "$GX_MIN_FREE_GB" ] || {
    echo "error: only ${_avail_gb} GiB free for CARGO_TARGET_DIR=$CARGO_TARGET_DIR." >&2
    echo "       Floor is ${GX_MIN_FREE_GB} GiB: the release target measures ~1.2 GiB and macOS" >&2
    echo "       gets unstable under 1 GiB free. Free space, or set GX_MIN_FREE_GB knowing why." >&2
    exit 1
}
mkdir -p "$CARGO_TARGET_DIR"
echo "==> target $CARGO_TARGET_DIR (${_avail_gb} GiB free, floor ${GX_MIN_FREE_GB} GiB)"

"$HERE/check-subsystem2.sh"
gx_require_clean "scripts/launcher-rs"
# the bundled notices are named for HEAD too: their inputs must be committed, not working-tree edits
gx_require_clean "THIRD_PARTY_NOTICES.txt"
gx_require_clean "resources/legal"
gx_require_clean "scripts/legal"
COMMIT9="$(gx_commit9)"
export GX_COMMIT="$COMMIT9"
# THE COMMIT'S OWN TIME, on the clock the release manifest's ordinal is on (gx-manifest.py writes
# `git show -s --format=%ct` of the release commit). The launcher compares the two to decide whether
# a published launcher is newer than itself, and a binary that carries none is a development build
# that never replaces itself. Never the packaging wall-clock time: a build packaged today from an
# older commit would then refuse genuinely newer releases.
# SET, THEN EXPORTED, on two lines: `export VAR="$(cmd)"` returns export's own status, so under
# set -e a failing git would be swallowed and the build would carry an empty commit time.
GX_COMMIT_EPOCH="$(git -C "$REPO" show -s --format=%ct HEAD)"
export GX_COMMIT_EPOCH

# ONE variable for the triple, resolved BEFORE the gate opens. The previous revision computed the
# bundle directory twice - once without the triple, once with - and opened the gate on the first,
# so gate_begin deleted paths the bundler never writes and a previous run's .app under the triple
# path survived untouched. Task 13 changes only this default.
# THE SIZE FLOORS ARE MEASURED, NOT GUESSED. Measured on this Mac 2026-08-20 at cf4fd0018:
#
#     .app bundle on disk   9,228 KiB     binary alone   9,422,400 B
#     .dmg                  2,860,166 B   .tar.gz        2,860,873 B
#
# A Tauri launcher links the OS webview instead of shipping a runtime, so both shipping forms
# compress to under 3 MiB. An earlier floor of 3,145,728 was an estimate and it FAILED THIS BUILD:
# a perfectly good .dmg was reported as "under the floor - a truncated bundle installs and then
# opens no window", which is a size assumption wearing a corruption failure's message. A floor
# exists to catch truncation, so it belongs well below the real value, not just beneath it: 1 MiB
# is under a third of what both artifacts actually measure, and a genuinely truncated bundle is
# kilobytes. Raise this only alongside a fresh measurement written into this comment.
GX_MIN_SHIPPED=1048576

MACOS_TRIPLE="${GX_MACOS_TRIPLE:-aarch64-apple-darwin}"
BUNDLE_DIR="$CARGO_TARGET_DIR/$MACOS_TRIPLE/release/bundle"
APP="$BUNDLE_DIR/macos/$GX_PRODUCT.app"
APP_BIN="$APP/Contents/MacOS/$GX_PRODUCT"
OUT_DMG="$DIST/GeneralsXLauncher-macos-$COMMIT9.dmg"
OUT_TGZ="$DIST/GeneralsXLauncher-macos-$COMMIT9.tar.gz"
OUT_ROWS="$DIST/gxartifacts-macos.txt"

mkdir -p "$DIST"

# DETACH BEFORE DELETING, or the next build fails as a toolchain error.
#
# Tauri's bundle_dmg.sh builds the image by attaching a read-write scratch copy,
# "$BUNDLE_DIR/macos/rw.<pid>.<name>.dmg", mounted at /Volumes/dmg.XXXXXX. If a run is
# interrupted - or simply loses the race - that scratch image is left ATTACHED. gx_gate_begin then
# deletes "$BUNDLE_DIR/macos", which removes the backing file of a still-live disk image, and the
# NEXT run's bundle_dmg.sh dies with
#
#     Error failed to bundle project: error running bundle_dmg.sh
#
# and nothing else. Measured here 2026-08-20: one orphaned attachment at /Volumes/dmg.PLBnoK made
# a build that had just compiled and signed cleanly fail at the last step, with a message that
# names neither the mount nor the cause.
#
# Scoped deliberately to images whose backing file is under this build's own bundle directory.
# Every other attached image on the machine - the operator's own volumes, the iOS simulator
# runtime under /System - is matched by nothing here and left alone.
gx_detach_bundle_images() {
    _devs="$(python3 - "$1" <<'PY'
import plistlib, subprocess, sys
root = sys.argv[1].rstrip("/") + "/"
try:
    out = subprocess.run(["hdiutil", "info", "-plist"],
                         capture_output=True, check=True).stdout
    info = plistlib.loads(out)
except Exception:
    raise SystemExit(0)
for img in info.get("images", []):
    if not str(img.get("image-path", "")).startswith(root):
        continue
    # The whole-disk entity, not a slice: detaching /dev/diskN takes its slices with it.
    for ent in img.get("system-entities", []):
        dev = ent.get("dev-entry", "")
        if dev and dev.count("s") == 1:   # /dev/disk8, not /dev/disk8s1
            print(dev)
            break
PY
)"
    for _d in $_devs; do
        echo "==> detaching an orphaned bundler scratch image at $_d"
        hdiutil detach "$_d" -force >/dev/null 2>&1 || true
    done
    unset _devs _d
}
gx_detach_bundle_images "$BUNDLE_DIR"

gx_gate_begin "$BUNDLE_DIR/macos" "$BUNDLE_DIR/dmg" "$OUT_DMG" "$OUT_TGZ" "$OUT_ROWS"

echo "==> building the macOS bundle at $COMMIT9 for $MACOS_TRIPLE"
# CI=true: Tauri's bundler then runs bundle_dmg.sh with --skip-jenkins, which leaves out its Finder AppleScript (window
# size, icon positions). That step needs a Finder the process can talk to: from a Terminal tab it takes the lead's Terminal
# hostage for its minute, and under gx-offterm (a launchd agent) it never gets an answer and the bundler dies with
# "error running bundle_dmg.sh" (AppleScript -1712; measured 12/10/2026). tauri.conf.json sets no dmg layout or background,
# so the image only loses Finder's default icon placement; the app, the dmg and its checks are unchanged.
# Freshness gate: refuse to build a launcher whose notices file is stale (Cargo.lock / licence data moved without regenerating).
python3 -I "$REPO/scripts/legal/third_party_notices.py" check || exit 1
( cd "$CRATE" && CI=true APPLE_SIGNING_IDENTITY="-" \
    cargo tauri build --target "$MACOS_TRIPLE" --bundles app,dmg )

gx_gate_assert "$APP_BIN" 1048576

# Third-party licence notices ride in the app next to LICENSE.md (tauri.conf.json "resources"; generated by
# scripts/legal/third_party_notices.py). A bundle without them would ship MIT/BSD/Apache crates and fonts un-noticed.
[ -s "$(dirname "$(dirname "$APP_BIN")")/Resources/THIRD_PARTY_NOTICES.txt" ] ||
    { echo "GATE FAIL: the launcher bundle carries no THIRD_PARTY_NOTICES.txt" >&2; exit 1; }

# GATE THE BUNDLER'S OWN .dmg, BEFORE COPYING IT. `cp` stamps the destination with the current
# time, so asserting the copy can never catch a stale source: last week's .dmg would be copied
# forward and gated green. The .dmg is the artifact humans download; it gets a real gate.
# A GLOB, NOT `find ... | head -1`. Two reasons, both measured on this Mac 2026-08-20:
#   1. `find` here is bfs, shimmed onto PATH ahead of /usr/bin/find, and it is on PATH for a
#      #!/bin/sh script too. It printed "grep: db/*.symbols: No such file or directory" three
#      times and matched nothing, so a .dmg the bundler had just written was reported as
#      "the dmg bundler wrote no .dmg" - a naming failure wearing a build failure's message.
#   2. `head -1` closes the pipe on the first match, which SIGPIPEs the producer. That is
#      load-bearing under `set -o pipefail` and invisible without it.
# The gate emptied this directory moments ago, so there is exactly one .dmg in it and a glob is
# both simpler and dependency-free. `[ -e ]` guards the no-match case, where an unmatched glob
# expands to the pattern itself.
DMG_SRC=""
for _dmg in "$BUNDLE_DIR/dmg"/*.dmg; do
    [ -e "$_dmg" ] || continue
    DMG_SRC="$_dmg"
    break
done
[ -n "$DMG_SRC" ] || { echo "GATE FAIL: the dmg bundler wrote no .dmg" >&2; exit 1; }
gx_gate_assert "$DMG_SRC" "$GX_MIN_SHIPPED"
cp "$DMG_SRC" "$OUT_DMG"

# The self-update artifact. COPYFILE_DISABLE keeps AppleDouble "._" members out of the archive;
# without it the extracted bundle carries junk beside every file and its signature check is a coin
# flip. A .tar.gz of the whole .app, replaced wholesale, is also the shape Tauri's own updater uses
# on macOS - the one part of that plugin's design worth copying. Task 14 extracts this tarball and
# smokes what comes out, because "the .app in the build tree launches" says nothing about it.
( cd "$BUNDLE_DIR/macos" && COPYFILE_DISABLE=1 tar czf "$OUT_TGZ" "$GX_PRODUCT.app" )
gx_gate_assert "$OUT_TGZ" "$GX_MIN_SHIPPED"

echo "==> smoke launching the build-tree app"
# The smoke OPENS the launcher's window. A GUI process started from a Terminal tab registers with LaunchServices as a second
# "Terminal" and blocks the lead's AppleScript for its whole run (-1708), so it goes through gx-offterm when this tree has it.
OFFTERM=""
[ -x "$REPO/scripts/session/gx-offterm.sh" ] && OFFTERM="$REPO/scripts/session/gx-offterm.sh"
${OFFTERM:+"$OFFTERM"} "$HERE/smoke-launch.sh" \
    --expect-commit "$COMMIT9" --expect-platform macos \
    --forbid-prefix "$APP" --forbid-prefix "/private/var/folders" \
    -- "$APP_BIN"

printf 'runtime macos %s %s %s\n' \
    "$(basename "$OUT_TGZ")" "$(gx_sha256 "$OUT_TGZ")" "$(gx_size "$OUT_TGZ")" > "$OUT_ROWS"

echo
echo "  dmg   : $OUT_DMG"
echo "  update: $OUT_TGZ"
echo "  rows  : $OUT_ROWS"
cat "$OUT_ROWS"
