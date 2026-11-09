# GeneralsX launcher packaging - the artifact gate.
#
# WHY THIS FILE EXISTS. A build that does nothing exits 0. Every packaging failure this project has
# actually had looked like success: a stale build log read as a fresh one, an artifact copied back
# from a previous run, a publish that rebuilt nothing. So no packager here believes an exit code.
# It deletes the expected output, runs the build, and then asserts the output exists, is not
# absurdly small, and has an mtime at or after the moment the build started. Restoring last week's
# file into place fails that third check, which is the whole point.
#
# Sourced, never executed. POSIX sh: it runs under macOS sh and under the build host's dash.

# The pinned names live in toolchain.env. A sourced file cannot find its own path in POSIX sh, so
# every caller sources toolchain.env FIRST and this refuses to run if it did not. Refusing is the
# point: an unset GX_PRODUCT would expand to an empty string and the packager would gate on
# "$BUNDLE/macos/.app", a path that never exists, reported as a build failure.
: "${GX_PRODUCT:?source toolchain.env before lib-gate.sh}"
: "${GX_APP_VERSION:?source toolchain.env before lib-gate.sh}"
: "${TAURI_CLI_VERSION:?source toolchain.env before lib-gate.sh}"

# GNU stat first, BSD second. THE ORDER IS LOAD-BEARING AND THE OTHER ORDER SILENTLY DISABLED THE
# GATE ON LINUX. This was written "BSD first, GNU second" as
#     gx_size() { stat -f '%z' "$1" 2>/dev/null || stat -c '%s' "$1"; }
# on the reasoning that the wrong flag simply errors and falls through. It does not. On GNU
# coreutils `-f` is --file-system, not a format flag, so `stat -f '%z' FILE` PRINTS A FILESYSTEM
# DUMP for FILE and exits non-zero only because of the bogus '%z' operand - which means the ||
# fallback ALSO ran, and gx_size returned five lines of filesystem statistics with the real byte
# count appended. Measured on the Ubuntu 24.04 build host 2026-08-20:
#     /root/gxbuild/remote-build-linux.sh: 55: [: Illegal number:   File: "...AppImage"
# Every arithmetic comparison downstream failed that way, and because `[ "$_size" -lt "$_min" ]`
# and the mtime comparison both sit inside `if` conditions, `set -e` did not stop anything: the
# size floor and the mtime freshness check SILENTLY DID NOT FIRE. Only the existence check
# survived, on the one platform whose whole build runs through this file remotely.
#
# The GNU form is safe to try first because it is unambiguous in the other direction: BSD stat has
# no -c option at all, so it exits non-zero having printed only a usage line, and the fallback is
# genuinely reached. tests/test-gate.sh pins both, and is now run on the Linux host as well as on
# this Mac - a unit test that only ever runs on the platform where the bug is invisible is how this
# shipped in the first place.
gx_size()  { stat -c '%s' "$1" 2>/dev/null || stat -f '%z' "$1"; }
gx_mtime() { stat -c '%Y' "$1" 2>/dev/null || stat -f '%m' "$1"; }

gx_sha256() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d' ' -f1
    else
        sha256sum "$1" | cut -d' ' -f1
    fi
}

# Delete every expected output and stamp the clock. Everything after this is measured against
# GX_GATE_EPOCH, so a build that reuses an old file is caught even when the bytes are perfect.
gx_gate_begin() {
    GX_GATE_EPOCH="$(date +%s)"
    for _p in "$@"; do rm -rf -- "$_p"; done
    unset _p
    # One-second resolution: a fast build can finish inside the same second the gate opened, which
    # would make a legitimately fresh artifact look one second too old. Back the stamp off by one
    # so the comparison is >= and still rejects anything from a prior run.
    GX_GATE_EPOCH=$((GX_GATE_EPOCH - 1))
    export GX_GATE_EPOCH
}

# gx_gate_assert <path> <min_bytes>
gx_gate_assert() {
    _path="$1"; _min="$2"
    if [ ! -e "$_path" ]; then
        echo "GATE FAIL: the build produced no $_path" >&2
        echo "           The build command's exit code is not evidence. Nothing was written." >&2
        return 1
    fi
    _size="$(gx_size "$_path")"
    if [ "$_size" -lt "$_min" ]; then
        echo "GATE FAIL: $_path is $_size bytes, under the $_min floor" >&2
        echo "           A truncated bundle installs and then opens no window." >&2
        return 1
    fi
    _mt="$(gx_mtime "$_path")"
    if [ "$_mt" -lt "${GX_GATE_EPOCH:-0}" ]; then
        echo "GATE FAIL: $_path has mtime $_mt, before this build started at ${GX_GATE_EPOCH:-0}" >&2
        echo "           This is a previous run's artifact. The build did not relink." >&2
        return 1
    fi
    echo "GATE OK $_path $_size $(gx_sha256 "$_path")"
    unset _path _min _size _mt
    return 0
}

gx_commit9() { git -C "${GX_REPO:-.}" rev-parse HEAD | cut -c1-9; }

# sourceID hashes the git ls-tree output PLUS a dirty-file overlay, so a dirty tree at the right
# commit produces a different digest from the same content committed. An artifact named for HEAD
# that was built from uncommitted bytes is a lie the name cannot expose. Commit, then build.
gx_require_clean() {
    _dirty="$(git -C "${GX_REPO:-.}" status --porcelain -- "$1")"
    if [ -n "$_dirty" ]; then
        echo "error: uncommitted changes under $1:" >&2
        printf '%s\n' "$_dirty" >&2
        echo "       Artifacts are named for HEAD. Commit before you build." >&2
        return 1
    fi
    unset _dirty
    return 0
}

# THE LAUNCHER'S VERSION IS THE RELEASE'S, AND A PACKAGER REFUSES OTHERWISE (28/09/2026).
# Every release from 0.9.42 to 0.9.47 shipped a launcher whose About row said 0.9.41: the packagers
# stamp the commit, and GX_APP_VERSION (with its two mirrors, Cargo.toml and tauri.conf.json, held
# equal by cmd_release.rs's the_launcher_version_is_real_and_all_three_files_agree) was never bumped,
# because nothing compared it with the release. The release's number is the one publish-bootstrap
# titles the changelog entry with ("GeneralsX 0.9.<n>"); the operator names it here and the tree
# must already say it. A function, called by the three Mac-side packagers only: remote-build-linux.sh
# sources this file on the build host, where GX_RELEASE_VERSION is not set and need not be.
gx_release_version_gate() {
    if [ -z "${GX_RELEASE_VERSION:-}" ]; then
        # Never suggest $GX_APP_VERSION here: copying the tree's own number past the gate is the
        # exact mistake it exists to catch.
        echo "error: GX_RELEASE_VERSION is not set. Name the release being staged: the number its" >&2
        echo "       changelog title will carry, the live gxchangelog.txt's newest 0.9.<n> plus one." >&2
        return 1
    fi
    # COMMITTED, and all three: package-launcher-linux.sh ships `git archive HEAD`, so a bump left in the
    # working tree passes a working-tree check and ships the old number (Codex r1 of e2d7b2360, P2).
    _repo="${GX_REPO:-$(git rev-parse --show-toplevel)}"
    _cargo="scripts/launcher-rs/gxlauncher/Cargo.toml"; _tauri="scripts/launcher-rs/gxlauncher/tauri.conf.json"
    if ! git -C "$_repo" diff --quiet HEAD -- scripts/build/launcher/toolchain.env "$_cargo" "$_tauri"; then
        echo "error: the launcher's version files have uncommitted changes. Packagers ship HEAD; commit first." >&2
        return 1
    fi
    for _got in "toolchain.env=$GX_APP_VERSION" \
        "Cargo.toml=$(sed -n 's/^version = "\(.*\)"$/\1/p' "$_repo/$_cargo" | head -n 1)" \
        "tauri.conf.json=$(sed -n 's/^ *"version": "\(.*\)",$/\1/p' "$_repo/$_tauri" | head -n 1)"; do
        if [ "${_got#*=}" != "$GX_RELEASE_VERSION" ]; then
            echo "error: the launcher's ${_got%%=*} says ${_got#*=} but the release being staged is $GX_RELEASE_VERSION." >&2
            echo "       Set $GX_RELEASE_VERSION in scripts/build/launcher/toolchain.env (GX_APP_VERSION)," >&2
            echo "       $_cargo and $_tauri, commit, and package again." >&2
            return 1
        fi
    done
    unset _repo _cargo _tauri _got
}
