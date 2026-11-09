#!/bin/sh
# GeneralsX launcher packaging - the build body that runs ON the Linux build host.
#
# APPIMAGE_EXTRACT_AND_RUN=1 is not optional. linuxdeploy, the tool Tauri's AppImage bundler
# fetches and runs, is ITSELF an AppImage, and Ubuntu 24.04 does not ship libfuse2. Without this
# the bundler fails with a FUSE mount error that reads like a Tauri bug. Installing libfuse2 would
# also work and would add a twelfth system-wide package - on a host that may be the relay - to
# satisfy one build step; this does not.
#
# THE COST OF THAT CHOICE IS STATED RATHER THAN HIDDEN: because libfuse2 is absent, the FUSE launch
# path a player on a FUSE-2 distribution uses is NOT exercised here, and cannot be without adding
# that package to this host. The selftest runs the extract-and-run path instead, and
# docs/HOWTO/LAUNCHER_FIRST_LAUNCH.md tells the player the flag to use if their distribution lacks
# FUSE 2. Task 14 repeats this note where it is easiest to miss.
#
# THREE MEASURED DEVIATIONS FROM THE 2026-08-20 PLAN, the first two already proven on the Windows
# box that day and written into remote-build-windows.ps1's own header:
#
#   1. THE TARBALL MUST CARRY build.frontendDist, WHICH IS NOT UNDER scripts/launcher-rs.
#      tauri.conf.json points frontendDist at "../../../design/launcher", which resolves OUTSIDE
#      the crate. The plan tarred `launcher-rs` alone from $REPO/scripts and extracted it to
#      /root/gxbuild/src/launcher-rs, which carries no frontend at all. package-launcher-linux.sh
#      therefore tars from the repository ROOT over both paths, preserving repo-relative paths, and
#      the crate lands one level deeper - hence src/scripts/launcher-rs rather than src/launcher-rs.
#
#   2. THE SOURCE TAR NEEDS COPYFILE_DISABLE=1, AND macOS tar HIDES THE EVIDENCE THAT IT DOES.
#      Without it macOS bsdtar attaches every file's extended attributes and GNU tar here
#      materialises each as a real "._<name>" file; Tauri globs capabilities/*.json and the
#      AppleDouble blob is not UTF-8. `tar tzf` ON THE MAC lists none of them, so the fault is
#      invisible from the side that creates it. Refusing here keeps it on that side.
#
#   3. THE JOB COUNT IS BOUNDED AND THE BUILD IS NICED. The plan lets cargo saturate all four
#      cores; on the relay host those are four cores not answering UDP. CARGO_BUILD_JOBS defaults
#      to 3 here and the compile runs at nice 10. Neither changes the emitted artifact - -j is
#      parallelism, not codegen-units.
set -eu

COMMIT9="$1"
. "$HOME/.cargo/env"
export CARGO_TARGET_DIR=/root/gxbuild/rust-target
export GX_COMMIT="$COMMIT9"
export APPIMAGE_EXTRACT_AND_RUN=1
export NO_STRIP=1
export CARGO_BUILD_JOBS="${CARGO_BUILD_JOBS:-3}"

SRC=/root/gxbuild/src
CRATE=$SRC/scripts/launcher-rs/gxlauncher
[ -f "$CRATE/tauri.conf.json" ] ||
    { echo "error: no tauri.conf.json at $CRATE - the source tarball did not extract" >&2; exit 1; }

APPLEDOUBLE="$(find "$SRC" -type f -name '._*' -print | head -20)"
if [ -n "$APPLEDOUBLE" ]; then
    echo "GATE FAIL: the source tarball carried AppleDouble members, e.g." >&2
    printf '%s\n' "$APPLEDOUBLE" >&2
    echo "           Re-tar with COPYFILE_DISABLE=1; note that \`tar tzf\` on macOS lists none." >&2
    exit 1
fi

. /root/gxbuild/toolchain.env
. /root/gxbuild/lib-gate.sh
BUNDLE=$CARGO_TARGET_DIR/release/bundle/appimage
OUT=/root/gxbuild/out
DEST="$OUT/GeneralsXLauncher-linux-x86_64-$COMMIT9.AppImage"
gx_gate_begin "$BUNDLE" "$OUT"

cd "$CRATE"
nice -n 10 cargo tauri build --bundles appimage

APPIMAGE="$(find "$BUNDLE" -maxdepth 1 -name '*.AppImage' -print | head -1)"
[ -n "$APPIMAGE" ] || { echo "GATE FAIL: the appimage bundler wrote no .AppImage" >&2; exit 1; }
gx_gate_assert "$APPIMAGE" 20971520

mkdir -p "$OUT"
cp "$APPIMAGE" "$DEST"
chmod +x "$DEST"
gx_gate_assert "$DEST" 20971520

# THE BUILD HOST IS HEADLESS. Xvfb gives WebKitGTK a real X display; without one the app exits
# before creating a window and the failure is indistinguishable from a broken build.
# WEBKIT_DISABLE_COMPOSITING_MODE and WEBKIT_DISABLE_DMABUF_RENDERER because WebKitGTK's
# accelerated paths require a GPU this box does not have, and their absence shows up as a BLANK
# WINDOW rather than an error - which a naive "did it start" check passes and which
# smoke-launch.sh's webview=="painted" assertion does not.
#
# LIBGL_ALWAYS_SOFTWARE=1 IS NOT COSMETIC AND IT IS NOT A GRAPHICS SETTING. Without it Mesa probes
# for DRI3 under Xvfb, fails, and writes two "libEGL warning: DRI3 error" lines TO STDOUT - not to
# stderr, measured 2026-08-20, selftest.err was empty while selftest.json held three lines. The
# selftest contract is exactly one line of JSON on stdout, so smoke-launch.sh correctly refused
# with "not exactly one line of output (3 lines)" on a build whose window had genuinely painted.
# The fix belongs here, at the source of the noise, rather than in the parser: teaching
# smoke-launch.sh to skip lines it does not recognise would retire a real check - that the launcher
# prints one line and nothing else - to work around an environment quirk of this build host. With
# llvmpipe forced, stdout is one line and the contract is enforced unweakened. If Mesa ever starts
# writing to stdout again the smoke fails loudly rather than passing quietly, which is the right
# way round.
echo "==> selftest under Xvfb"
cd "$OUT"
LIBGL_ALWAYS_SOFTWARE=1 WEBKIT_DISABLE_COMPOSITING_MODE=1 WEBKIT_DISABLE_DMABUF_RENDERER=1 \
xvfb-run -a -s "-screen 0 1280x800x24" \
    "$DEST" --appimage-extract-and-run --gx-selftest > "$OUT/selftest.json" 2> "$OUT/selftest.err" || {
        echo "SELFTEST FAIL: --gx-selftest exited non-zero" >&2
        head -20 "$OUT/selftest.err" >&2
        exit 1
    }
cat "$OUT/selftest.json"
