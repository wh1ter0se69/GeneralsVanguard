#!/bin/bash
# Fetch the dynamic MoltenVK.framework for iOS (DXVK dlopens it from the app
# bundle at runtime). Pinned to a specific release so the packaged driver is
# reproducible; bump MVK_VERSION + MVK_SHA256 together.
set -euo pipefail

MVK_VERSION="v1.4.1"
MVK_SHA256="54336b90212c390ed5935c96460aed3bf651ad7d3c0f0e956586ce18e9c0b701"
DEST="${GX_MOLTENVK_ROOT:-${HOME}/GeneralsX/MoltenVK}"

FW="MoltenVK/MoltenVK/dynamic/MoltenVK.xcframework/ios-arm64/MoltenVK.framework"
STAMP=".gx-mvk-version"

mkdir -p "${DEST}"
cd "${DEST}"

# THE GUARD NAMES THE VERSION, AND IT NAMES THE DRIVER. It used to be `[[ -d "${FW}" ]]` alone,
# which answers "does a directory exist at this path" - a question with no version in it and no
# file in it. Two ways that shipped the wrong thing, both measured 2026-09-22:
#
#   1. Bumping MVK_VERSION + MVK_SHA256 - the exact workflow the header above prescribes - was a
#      NO-OP on any Mac that had already staged an older release. The script printed "already
#      staged" and exited 0 having downloaded, verified and changed nothing; package-ios-zh.sh
#      then embedded the OLD framework and the build log showed a clean fetch. The iPad is the
#      slowest place in this project to discover anything, and this sends you there.
#   2. A scratch DEST holding that directory path and ZERO files took the early exit. tar lists
#      the framework directory (member 74) before its 4.7 MB binary (member 75), so a download
#      interrupted between the two leaves the guard permanently satisfied by an empty shell.
#
# MVK_SHA256 cannot cover either: it is only ever checked on bytes that are downloaded, and in
# this path nothing is. So the stamp is the only record of which release produced this tree.
if [[ -f "${FW}/MoltenVK" && "$(cat "${STAMP}" 2>/dev/null)" == "${MVK_VERSION}" ]]; then
    echo "MoltenVK ${MVK_VERSION} already staged at ${DEST}"
    exit 0
fi

echo "==> Downloading MoltenVK ${MVK_VERSION} (iOS)"
curl -fL -o MoltenVK-ios.tar \
    "https://github.com/KhronosGroup/MoltenVK/releases/download/${MVK_VERSION}/MoltenVK-ios.tar"
echo "${MVK_SHA256}  MoltenVK-ios.tar" | shasum -a 256 -c -

# NOTHING STAGED IS TOUCHED UNTIL THE NEW BYTES HAVE PASSED THE CHECKSUM. A 404 or a mismatched
# MVK_SHA256 - the usual shape of "MVK_VERSION was bumped and its sha was not" - must leave the
# previously staged, known-good driver exactly where it was, not delete it on the way to failing.
#
# The stamp goes FIRST and comes back LAST, and that ordering is the whole guarantee: between
# these two lines there is no stamp, so an interrupted extraction can never leave a tree that
# claims to be a complete release. Clearing rather than extracting over the old tree because tar
# overwrites only the members it carries - a file dropped between releases would otherwise survive
# into the shipped .app. Both paths are relative to the ${DEST} already cd'd into.
rm -f "${STAMP}"
rm -rf MoltenVK
tar -xf MoltenVK-ios.tar
# The driver, not the directory - the same thing the guard above asks for. If the tarball ever
# stops carrying it, this refuses here instead of letting the next run take the early exit.
if [[ ! -f "${FW}/MoltenVK" ]]; then
    echo "ERROR: ${MVK_VERSION} extracted but ${DEST}/${FW}/MoltenVK is not there." >&2
    exit 1
fi
printf '%s\n' "${MVK_VERSION}" > "${STAMP}"
echo "==> Staged: ${DEST}/${FW}"
