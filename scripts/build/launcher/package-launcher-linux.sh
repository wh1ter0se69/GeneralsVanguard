#!/bin/sh
# GeneralsX launcher packaging - Linux. Runs on this Mac, builds on GX_LINUX_BUILD_HOST.
#
#     scripts/build/launcher/package-launcher-linux.sh
#
# AppImage rather than .deb, for three reasons in order of weight:
#   1. The launcher self-updates by replacing its own bytes. A .deb is owned by dpkg; updating it
#      in place either fights the package manager or needs sudo on every update. An AppImage is one
#      file the launcher can replace exactly the way it replaces the Windows exe.
#   2. A .deb serves Debian and Ubuntu. An AppImage serves Arch and Fedora too, with no second
#      pipeline and no second manifest row.
#   3. The manifest model is already "download one file, check one hash, run it". That IS an
#      AppImage. It is not a .deb.
# The cost is honest and documented: built on Ubuntu 24.04, the AppImage requires glibc 2.39, so it
# will not start on Ubuntu 22.04 or Debian 12. See docs/HOWTO/LAUNCHER_FIRST_LAUNCH.md.
#
# THE SOURCE IS SHIPPED, NOT PULLED, for the same reason as the Windows packager: this branch is
# not pushed, and a `git fetch && git reset --hard` on the remote is blocked by any stray local
# edit there. A tarball is self-contained and cannot go stale.
#
# TWO MEASURED DEVIATIONS FROM THE 2026-08-20 PLAN, both already paid for on the Windows box that
# day and recorded in package-launcher-windows.sh's header, which says in as many words that "the
# Linux packager needs the same fix":
#
#   1. THE TARBALL IS TAKEN FROM THE REPOSITORY ROOT OVER BOTH PATHS. tauri.conf.json's
#      build.frontendDist is "../../../design/launcher", outside scripts/launcher-rs. The plan's
#      `( cd "$REPO/scripts" && tar czf "$TARBALL" launcher-rs )` carries no frontend, and a build
#      that works on this Mac - where the sibling directory is present - fails only on the remote,
#      which is the most expensive place to find out. The frontend path is read out of
#      tauri.conf.json rather than repeated here, so moving it into the crate needs no edit.
#
#   2. COPYFILE_DISABLE=1. Without it macOS bsdtar attaches every file's xattrs and GNU tar on the
#      remote materialises each as a real "._<name>" file; Tauri globs capabilities/*.json and dies
#      on "stream did not contain valid UTF-8". `tar tzf` on macOS lists none of them, so this is
#      invisible from the side that creates it. remote-build-linux.sh refuses the tarball outright
#      rather than deleting the members, keeping the fault where it was made.
#
# AND ONE ADDITION, forced by a real collision on 2026-08-20: THE SOURCE SNAPSHOT IS `git archive`
# AT THE COMMIT, NOT A TAR OF THE WORKING TREE.
#
# The plan (and the Windows packager) tar the working tree and rely on gx_require_clean, run
# moments earlier, to make "named for HEAD" true. That is a race, and it lost: another session was
# editing scripts/launcher-rs/gxio and design/launcher/screens in this same tree, the clean check
# passed at 13:38 and refused at 14:15, and had it passed a second time the AppImage would have
# carried that session's half-finished, uncommitted modules under the name of a commit that does
# not contain them. `git archive HEAD -- <paths>` makes the shipped bytes ARE the named commit,
# by construction rather than by check - so the invariant gx_require_clean was defending is now
# enforced rather than merely verified, and a concurrent editor cannot corrupt this build at all.
# Uncommitted work under those paths is reported loudly and excluded; it is not a reason to stop,
# because it is not in the artifact.
#
# git archive is also why COPYFILE_DISABLE stops mattering on this path: it writes tar entries from
# the object database and never touches macOS xattrs. remote-build-linux.sh keeps its AppleDouble
# refusal anyway, because it costs one find and it is the check that names the fault correctly if
# this ever goes back to a working-tree tar.
set -eu

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
export GX_REPO="$REPO"
HERE="$REPO/scripts/build/launcher"
. "$HERE/toolchain.env"
. "$HERE/lib-gate.sh"
# The launcher must say the release being staged (lib-gate.sh), before anything is built.
gx_release_version_gate || exit 1
[ -f "$REPO/scripts/test/local.env" ] ||
    { echo "error: no scripts/test/local.env - copy local.env.example and fill it in" >&2; exit 1; }
. "$REPO/scripts/test/local.env"
[ -n "${GX_LINUX_BUILD_HOST:-}" ] || {
    echo "error: GX_LINUX_BUILD_HOST is not set in scripts/test/local.env." >&2
    echo "       It is deliberately not defaulted to VPS: that host runs the relay." >&2
    exit 1
}

DIST="$REPO/dist/launcher"
mkdir -p "$DIST"
"$HERE/check-subsystem2.sh"

# The frontend directory, read out of tauri.conf.json. Empty when it already lives inside
# scripts/launcher-rs, in which case the second tar path and the second clean check are both
# unnecessary rather than wrong.
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

COMMIT="$(git -C "$REPO" rev-parse HEAD)"
COMMIT9="$(gx_commit9)"

# NOT gx_require_clean: see the header. The artifact is cut from $COMMIT by git archive, so
# uncommitted work under these paths cannot reach it. It is still worth printing - a reader
# comparing this AppImage against their working tree needs to know the difference exists.
DIRTY="$(git -C "$REPO" status --porcelain -- scripts/launcher-rs ${FRONTEND_REL:+"$FRONTEND_REL"})"
if [ -n "$DIRTY" ]; then
    echo "warning: uncommitted changes under the launcher source, EXCLUDED from this artifact:" >&2
    printf '%s\n' "$DIRTY" >&2
    echo "         The AppImage is cut from $COMMIT by git archive, so it contains none of them." >&2
fi

OUT_IMG="$DIST/GeneralsXLauncher-linux-x86_64-$COMMIT9.AppImage"
OUT_ROWS="$DIST/gxartifacts-linux.txt"
gx_gate_begin "$OUT_IMG" "$OUT_ROWS" "$DIST/linux-selftest.json"

TARBALL="$(mktemp -t gxsrc).tgz"
# The notices gate runs on what is committed at $COMMIT (the same snapshot `git archive` ships), not on a dirty working
# tree: GATE holds the checker, its data, the notices file and Cargo.lock exactly as committed.
GATE="$(mktemp -d)"
trap 'rm -f "$TARBALL"; rm -rf "$GATE"' EXIT
git -C "$REPO" archive "$COMMIT" -- scripts/legal resources/legal THIRD_PARTY_NOTICES.txt scripts/launcher-rs/Cargo.lock | tar -x -C "$GATE" ||
    { echo "error: could not extract the committed notices tooling from $COMMIT (are scripts/legal and THIRD_PARTY_NOTICES.txt committed?)" >&2; exit 1; }
# Freshness gate: refuse to build a launcher whose notices file is stale (Cargo.lock / licence data moved without regenerating).
python3 -I "$GATE/scripts/legal/third_party_notices.py" check || exit 1
if [ -n "$FRONTEND_REL" ]; then
    git -C "$REPO" archive --format=tar.gz -o "$TARBALL" "$COMMIT" \
        -- scripts/launcher-rs LICENSE.md THIRD_PARTY_NOTICES.txt "$FRONTEND_REL"
else
    git -C "$REPO" archive --format=tar.gz -o "$TARBALL" "$COMMIT" -- scripts/launcher-rs LICENSE.md THIRD_PARTY_NOTICES.txt
fi
# GATE ON THE SNAPSHOT. `git archive` over a pathspec that matches nothing exits non-zero, but an
# archive that is technically valid and missing the crate is the failure that only shows up on the
# remote. Assert the two things the build cannot start without.
tar tzf "$TARBALL" | grep -q '^scripts/launcher-rs/gxlauncher/tauri.conf.json$' ||
    { echo "error: the source archive carries no gxlauncher/tauri.conf.json" >&2; exit 1; }
# tauri.conf.json bundles ../../../LICENSE.md (GPL-3 + EA's terms); a build without it fails on the remote.
tar tzf "$TARBALL" | grep -q '^LICENSE.md$' ||
    { echo "error: the source archive carries no LICENSE.md" >&2; exit 1; }
# tauri.conf.json also bundles ../../../THIRD_PARTY_NOTICES.txt (third-party licence notices; scripts/legal/third_party_notices.py).
tar tzf "$TARBALL" | grep -q '^THIRD_PARTY_NOTICES.txt$' ||
    { echo "error: the source archive carries no THIRD_PARTY_NOTICES.txt" >&2; exit 1; }
if [ -n "$FRONTEND_REL" ]; then
    tar tzf "$TARBALL" | grep -q "^$FRONTEND_REL/" ||
        { echo "error: the source archive carries no $FRONTEND_REL - frontendDist would be absent" >&2; exit 1; }
fi

SSH="ssh -o ConnectTimeout=30 -o BatchMode=yes -o ServerAliveInterval=30"
SCP="scp -q -o ConnectTimeout=30 -o BatchMode=yes"
H="$GX_LINUX_BUILD_HOST"

echo "==> shipping source and build script to $([ "$H" = "${VPS:-}" ] && echo 'THE RELAY HOST' || echo 'the build host')"
$SSH "$H" 'rm -rf /root/gxbuild/src && mkdir -p /root/gxbuild/src'
$SCP "$TARBALL" "$H:/root/gxbuild/src/launcher-rs.tgz"
$SCP "$HERE/toolchain.env"        "$H:/root/gxbuild/toolchain.env"
$SCP "$HERE/lib-gate.sh"          "$H:/root/gxbuild/lib-gate.sh"
$SCP "$HERE/remote-build-linux.sh" "$H:/root/gxbuild/remote-build-linux.sh"
$SSH "$H" 'cd /root/gxbuild/src && tar xzf launcher-rs.tgz && rm -f launcher-rs.tgz && chmod +x /root/gxbuild/remote-build-linux.sh'

echo "==> building at $COMMIT9"
$SSH "$H" "/root/gxbuild/remote-build-linux.sh $COMMIT9" 2>&1 | tee "$DIST/linux-build.log"
grep -q '^GATE OK ' "$DIST/linux-build.log" ||
    { echo "error: the Linux build printed no GATE OK line - it did not produce an artifact" >&2; exit 1; }

echo "==> retrieving the AppImage and the selftest it captured"
$SCP "$H:/root/gxbuild/out/GeneralsXLauncher-linux-x86_64-$COMMIT9.AppImage" "$OUT_IMG"
# Payload gate, on the build host (it has the Ubuntu packages the AppImage was assembled from; --appimage-extract runs no app
# code): every library in the AppImage must be owned by a component of THIRD_PARTY_NOTICES.txt AND come from the package version
# the notices name. The Tauri bundler pulls the GTK/WebKit stack in transitively. The scratch directory is removed on exit.
( cd "$GATE" && COPYFILE_DISABLE=1 tar cz . ) | $SSH "$H" "T=\$(mktemp -d /tmp/gxai.XXXXXX) && trap 'rm -rf \"\$T\"' EXIT && cd \"\$T\" && mkdir gate && tar xz -C gate && cp /root/gxbuild/out/GeneralsXLauncher-linux-x86_64-$COMMIT9.AppImage a.AppImage && chmod +x a.AppImage && ./a.AppImage --appimage-extract >/dev/null 2>&1 && python3 -I gate/scripts/legal/third_party_notices.py check-path --platform linux squashfs-root" ||
    { echo "error: the AppImage payload failed the third-party notices gate (see above)" >&2; exit 1; }
$SCP "$H:/root/gxbuild/out/selftest.json" "$DIST/linux-selftest.json"
chmod +x "$OUT_IMG"
gx_gate_assert "$OUT_IMG" 20971520

cat > "$DIST/replay-linux.sh" <<'REPLAY'
#!/bin/sh
cat "$(dirname "$0")/linux-selftest.json"
REPLAY
chmod +x "$DIST/replay-linux.sh"
# TWO forbidden prefixes, because the runtime uses two different roots and only one of them is the
# FUSE mount. --appimage-extract-and-run - the flag this build must use, since 24.04 ships no
# libfuse2 - unpacks under /tmp/appimage_extracted_* and deletes it on exit; a FUSE launch mounts
# at /tmp/.mount_*; and `--appimage-extract` writes ./squashfs-root beside the file. Forbidding
# only /tmp/.mount_ would be a guard that can never fire in the configuration this plan runs.
"$HERE/smoke-launch.sh" \
    --expect-commit "$COMMIT9" --expect-platform linux \
    --forbid-prefix '/tmp/appimage_extracted_' \
    --forbid-prefix '/tmp/.mount_' \
    --forbid-prefix '/root/gxbuild/out/squashfs-root' \
    -- "$DIST/replay-linux.sh"

printf 'runtime linux %s %s %s\n' \
    "$(basename "$OUT_IMG")" "$(gx_sha256 "$OUT_IMG")" "$(gx_size "$OUT_IMG")" > "$OUT_ROWS"

echo
echo "  appimage: $OUT_IMG"
echo "  rows    : $OUT_ROWS"
cat "$OUT_ROWS"
