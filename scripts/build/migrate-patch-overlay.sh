#!/bin/bash
# Copyright 2026 GeneralsX contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# One-time migration: move the overlay archives out of a
# game folder and into the GeneralsX-owned sibling directory the run wrapper points at.
#
#   scripts/build/migrate-patch-overlay.sh [<runtime-dir>] [--dry-run]
#
# Default runtime dir: $HOME/GeneralsX/GeneralsZH. The overlay lands in $HOME/GeneralsX/GeneralsXPatch.
#
# THIS IS dataID-NEUTRAL, AND THAT IS MEASURED, NOT ASSUMED. The same two archives mounted from
# the game folder and mounted from an overlay outside it produce the identical dataID
# (FA018128 both ways - arms B1 and C1).
# So peers may migrate one at a time; there is no atomic-rebuild requirement here and a
# half-migrated fleet still interoperates. That is the whole reason this is safe to run on a
# live install at all.
#
# IT STILL REFUSES TO RUN WHILE THE GAME IS UP. The engine mounts archives once at startup and
# hashes the loose data into a dataID every peer must agree on; moving files under a running
# match is how a session ends up debugging a "network fault" that is a file move. Close the
# game first - the script names the pids rather than making you guess.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/build/lib-overlay.sh
source "${SCRIPT_DIR}/lib-overlay.sh"

RUNTIME="${HOME}/GeneralsX/GeneralsZH"
DRY=0
for arg in "$@"; do
    case "${arg}" in
        --dry-run) DRY=1 ;;
        -*) echo "ERROR: unknown option ${arg}" >&2; exit 1 ;;
        *)  RUNTIME="${arg}" ;;
    esac
done

if [[ ! -d "${RUNTIME}" ]]; then
    echo "ERROR: ${RUNTIME} is not a directory" >&2
    exit 1
fi
RUNTIME="$(cd "${RUNTIME}" && pwd)"
OVERLAY="$(gx_overlay_dir "${RUNTIME}")"

# pgrep -x, never -f: a path-shaped -f pattern matches nothing and exits silently (identical to
# success), and a bare -f name over-matches (it matched a live rsync of the game
# directory). macOS pgrep has no -c, hence the wc.
RUNNING="$(pgrep -x GeneralsXZH 2>/dev/null | tr '\n' ' ')"
if [[ -n "${RUNNING// /}" ]]; then
    echo "REFUSING: GeneralsXZH is running (pids: ${RUNNING})." >&2
    echo "  Archives are mounted once at startup and dataID is computed from them; moving them" >&2
    echo "  under a live match refuses every join with DATA_DIFFERS. Close the game and re-run." >&2
    exit 1
fi

echo "runtime : ${RUNTIME}"
echo "overlay : ${OVERLAY}"
echo ""

moved=0
already=0
absent=0
while IFS= read -r name; do
    src="${RUNTIME}/${name}"
    dst="${OVERLAY}/${name}"

    if [[ -s "${dst}" ]]; then
        if [[ -e "${src}" ]]; then
            echo "  ALREADY in the overlay, and still in the game folder: ${name}"
            echo "     -> remove ${src} by hand once you are satisfied; leaving both is harmless"
            echo "        (identical bytes, one wins) but it doubles the mount."
        else
            echo "  already migrated: ${name}"
        fi
        already=$((already + 1))
        continue
    fi

    if [[ ! -s "${src}" ]]; then
        echo "  NOT FOUND in ${RUNTIME}: ${name}"
        absent=$((absent + 1))
        continue
    fi

    if [[ "${DRY}" == "1" ]]; then
        echo "  would move: ${name}"
    else
        mkdir -p "${OVERLAY}" || exit 1
        # mv within one filesystem is a rename: atomic, and it cannot leave a half-copied
        # archive behind for the engine to open. Across filesystems mv falls back to a copy,
        # which is why the result is verified below rather than trusted.
        mv "${src}" "${dst}" || { echo "ERROR: could not move ${name}" >&2; exit 1; }
        echo "  moved: ${name}"
    fi
    moved=$((moved + 1))
done < <(gx_overlay_archives)

echo ""
if [[ "${DRY}" == "1" ]]; then
    echo "dry run - nothing was moved (${moved} would move, ${already} already there, ${absent} absent)"
    exit 0
fi

if ! gx_overlay_verify "${OVERLAY}"; then
    exit 1
fi

echo "overlay verified: $(gx_overlay_archives | wc -l | tr -d ' ') archive(s) present in ${OVERLAY}"
echo ""
echo "Next: redeploy so run.sh exports GENERALSX_PATCH_PATH, then confirm the engine agrees -"
echo "  the launch stderr must carry one '[PATCH_OVERLAY] Mounted' line per archive and"
echo "  '[SIMID] local ... data=' must read the same value it did before this migration."
