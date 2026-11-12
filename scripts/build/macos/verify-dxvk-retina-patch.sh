#!/bin/bash
# Copyright 2026 GeneralsX contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Refuse to ship a macOS bundle whose DXVK lacks the Retina present-rect fix.
#
# THE OLD GATE PROVED THE SOURCE, NEVER THE ARTIFACT.
#
# cmake/dx8.cmake applies Patches/dxvk-ios.patch to references/fbraz3-dxvk and checks itself with
# `git apply --reverse --check`. That asks "is the SOURCE TREE patched". The question that decides
# what a player sees is "does the DYLIB IN THE ZIP carry the fix", and the two come apart:
#
#   * a stale _deps/dxvk-build-macos gives meson nothing to rebuild, the reverse-check passes, and
#     configure prints "already applied" over an unpatched dylib;
#   * the install copy at <build>/libdxvk_d3d9.0.dylib is produced by a rule with only an ORDERING
#     dependency on the ExternalProject target, so it can survive as an older file - and
#     bundle-macos-zh.sh PREFERS that copy over the meson output (DXVK_D3D9_LIB_INSTALL before
#     DXVK_D3D9_LIB_MESON). A verifier on the meson output alone would pass while the packaged copy
#     is unpatched;
#   * packaging can be run without building anything at all.
#
# So this lives in PACKAGING and checks the staged file. It is also deliberately NOT under cmake/:
# SIMDIGEST_PATHS_BUILD covers `cmake`, `triplets`, `CMakePresets.json`, `vcpkg.json` and
# `vcpkg-lock.json`, so a gate placed there would move the SimID digest and turn every future fix
# to it into a four-platform ship. scripts/ is in no digest path list, so this one is macOS-only.
#
# WHAT WENT WRONG WITHOUT IT: the payload published 2026-08-26 - engine 55739fcf9, whose source
# HAS the fix (de1782f0c, 2026-08-19) - shipped an unpatched libdxvk_d3d9 to every macOS player.
# On a Retina Mac the whole backbuffer is blitted into the top-left quarter of the surface and the
# mouse rect misses by the same factor. Nineteen days, because every check looked at source.
#
# Usage: verify-dxvk-retina-patch.sh <libdxvk_d3d9 dylib> [label]
set -u

DYLIB="${1:-}"
LABEL="${2:-${DYLIB}}"
[ -n "${DYLIB}" ] || { echo "verify-dxvk-retina-patch: usage: $0 <dylib> [label]" >&2; exit 2; }

# Absolute tool paths. A packaging script runs on somebody else's PATH, and this repo has already
# been bitten once by a ~/.local/bin shim shadowing /usr/bin/find: the loop ran zero times, printed
# nothing, and the engine shipped with the wrong dataID.
STRINGS=/usr/bin/strings
GREP=/usr/bin/grep
for t in "${STRINGS}" "${GREP}"; do
    [ -x "${t}" ] || { echo "verify-dxvk-retina-patch: ${t} is missing; cannot verify ${LABEL}" >&2; exit 2; }
done

[ -f "${DYLIB}" ] || { echo "ERROR: verify-dxvk-retina-patch: no file at ${DYLIB} (${LABEL})" >&2; exit 1; }

# 1. IT MUST ACTUALLY BE A MACH-O. Without this the check is a substring search over arbitrary
#    bytes, and any text file that happens to name the symbol - this script included - "passes".
MAGIC="$(od -An -tx1 -N4 "${DYLIB}" 2>/dev/null | tr -d ' \n')"
case "${MAGIC}" in
    cffaedfe|cefaedfe|feedfacf|feedface|cafebabe|bebafeca|cafebabf|bfbafeca) ;;
    # cafebabf/bfbafeca are FAT_MAGIC_64 and its byte-swapped form. This project ships thin arm64
    # (cffaedfe) today, but a universal build is a plausible future and a gate that rejected it
    # would read as "the fix is missing" rather than "I cannot parse this". NOTE the limit: for a
    # FAT binary the symbol search below is an AGGREGATE over every slice, so it proves at least
    # one slice carries the fix, not that all of them do. If a universal build ever ships, this
    # must grow a per-architecture check (lipo -thin per arch, then search each).
    *)
        echo "ERROR: verify-dxvk-retina-patch: ${LABEL} is not a Mach-O binary (magic ${MAGIC:-empty})." >&2
        echo "       Refusing to vouch for a file this check cannot read as a library." >&2
        exit 1 ;;
esac

# 2. POSITIVE CONTROL, and an INDEPENDENT one. `SDL_GetWindowSize` would be useless here because it
#    is a substring of `SDL_GetWindowSizeInPixels` - the control would be satisfied by the very
#    thing it is meant to be independent of. `SDL_Vulkan_CreateSurface` is present in both the
#    patched and the unpatched builds (measured: 3 occurrences in each), so if it is missing the
#    reader is broken and any verdict below would be meaningless.
if [ "$("${STRINGS}" -a "${DYLIB}" | "${GREP}" -c 'SDL_Vulkan_CreateSurface')" -eq 0 ]; then
    echo "ERROR: verify-dxvk-retina-patch: ${LABEL} contains no SDL_Vulkan_CreateSurface." >&2
    echo "       That is the CONTROL, not the fix: this check could not read the binary's strings," >&2
    echo "       so it cannot say anything about the Retina patch. Refusing to pass." >&2
    exit 1
fi

# 3. The fix itself. wsi_window_sdl3.cpp's getWindowSize must call SDL_GetWindowSizeInPixels;
#    unpatched it calls SDL_GetWindowSize, which returns POINTS for a surface measured in PIXELS.
if [ "$("${STRINGS}" -a "${DYLIB}" | "${GREP}" -c 'SDL_GetWindowSizeInPixels')" -eq 0 ]; then
    cat >&2 <<MSG
ERROR: verify-dxvk-retina-patch: the DXVK about to be packaged lacks the Retina present-rect fix.
       file:    ${LABEL}
       symbol:  SDL_GetWindowSizeInPixels ABSENT
       control: SDL_Vulkan_CreateSurface present, so the binary WAS read

       A perfectly patched source tree still produces this when a stale
       _deps/dxvk-build-macos or a stale <build>/libdxvk_d3d9.0.dylib install copy is reused.
       Shipping it puts every Retina Mac's picture in the top-left quarter of its window.

       Fix, IN THIS ORDER. The submodule comes first: without references/fbraz3-dxvk/.git,
       cmake/dx8.cmake silently builds the UNPATCHED pinned remote clone and no amount of
       rebuilding helps. The RECONFIGURE is not optional either - the ExternalProject stamp
       directory holds cmake-GENERATED metadata (*-custominfo.txt, *-update-info.txt,
       *-patch-info.txt) that ninja has no rule to recreate, so deleting the stamps and going
       straight to a build fails with "missing and no known rule to make it".

              git submodule update --init --recursive references/fbraz3-dxvk
              rm -rf <build>/_deps/dxvk-build-macos \
                     <build>/dxvk_macos_build-prefix/src/dxvk_macos_build-stamp \
                     <build>/libdxvk_d3d9*.dylib
              cmake --preset macos-vulkan
              cmake --build <build> --target dxvk_macos_build dxvk_d3d8_install

       The reconfigure is also what switches an existing remote-source build over to the patched
       local fork; dxvk_d3d8_install restores the install copies the bundler prefers.
MSG
    exit 1
fi

echo "  DXVK Retina present-rect fix: present in ${LABEL}"
