#!/bin/bash
# Copyright 2026 GeneralsX contributors
# SPDX-License-Identifier: GPL-3.0-or-later
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

# The guard checks the version stamp and the driver file, not just the directory: otherwise a bumped
# MVK_VERSION/MVK_SHA256 is a no-op on a Mac with an older release, and an empty framework shell takes the early exit.
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
