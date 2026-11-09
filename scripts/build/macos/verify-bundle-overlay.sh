#!/bin/bash
# GeneralsX @build Claude 17/08/2026 Refuse to publish a macOS .app that cannot join a game.
#
# WHY THIS EXISTS AS A SEPARATE SCRIPT RATHER THAN A FLAG. bundle-macos-zh.sh already refuses to
# build an incomplete bundle by default, and GX_BUNDLE_ALLOW_NO_OVERLAY=1 is the documented escape
# for a packaging smoke test. CI needs that escape - a GitHub runner has no copy of the community
# archives, they are not in the repository, and 80 MB of third-party .big does not belong in git.
# So the smoke-test build sets the flag and stays green.
#
# The moment the escape exists, "the release must not use it" is a convention, and a convention is
# not a guarantee. This script converts it into a property of the ARTIFACT: it opens the .app that
# is about to be published and checks what is actually inside it. A mis-plumbed workflow input, a
# re-run that reused an older artifact, a hand-uploaded asset, or a future edit that flips the
# default all fail here, because none of them can put archives into a bundle that has none.
#
# WHAT COUNTS AS PUBLISHABLE - all of it, not the first failure. Every check reports, then the
# script exits once, so one CI run tells you everything that is wrong.
#
#   1. no OVERLAY-MISSING.txt stamp (bundle-macos-zh.sh writes it on EITHER escape-hatch arm)
#   2. Contents/Resources/GeneralsXPatch exists
#   3. every archive in overlay-archives.txt is there, a regular file, non-empty
#   4. every archive in overlay-optional.txt is there under the name its default dictates
#   5. nothing ELSE matching *.big / *.big.off is there - a stale archive splits the fleet exactly
#      as a missing one does (see the UNEXPECTED note in lib-overlay.sh)
#   6. optional.txt names exactly the staged optional set
#  6b. required.txt names exactly the mandatory set - without it `discover()` on macOS can only
#      ever answer `Verdict::Unknown`, whatever is actually installed (G4)
#   7. gamedata.manifest exists and every path it names is staged and non-empty
#   8. the launcher actually consumes both - exports GENERALSX_PATCH_PATH and reads the manifest
#   9. every Mach-O in the bundle resolves its libraries from inside the bundle - no LC_RPATH or
#      LC_LOAD_DYLIB pointing at the build machine, and no dependency that nothing answers for
#
# Check 8 is the one that is easy to leave out and the one that caught a real bug in this feature's
# first cut: data staged into the .app that the launcher never reads is indistinguishable, from the
# outside, from data that was never staged. The player's engine computes the wrong dataID either
# way. Verifying the payload without verifying that something consumes it proves nothing.
#
# Usage:  verify-bundle-overlay.sh <path/to/GeneralsXZH.app>
# Exit:   0 publishable, 1 not publishable or the .app could not be read.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"

# shellcheck source=scripts/build/lib-overlay.sh
source "${PROJECT_ROOT}/scripts/build/lib-overlay.sh"

APP_DIR="${1:-}"
if [[ -z "${APP_DIR}" ]]; then
    echo "usage: $(basename "$0") <path/to/GeneralsXZH.app>" >&2
    exit 1
fi
if [[ ! -d "${APP_DIR}" ]]; then
    echo "ERROR: not a directory: ${APP_DIR}" >&2
    exit 1
fi

RES_DIR="${APP_DIR}/Contents/Resources"
PATCH_DIR="${RES_DIR}/GeneralsXPatch"
GAMEDATA_DIR="${RES_DIR}/gamedata"
MANIFEST="${RES_DIR}/gamedata.manifest"
LAUNCHER="${APP_DIR}/Contents/MacOS/run.sh"

FAILURES=0
fail() { echo "  FAIL: $*" >&2; FAILURES=$((FAILURES + 1)); }
ok()   { echo "  ok:   $*"; }

echo "Verifying macOS bundle is publishable: ${APP_DIR}"

# The list files are read through the library's own guarded entry points, so a missing or empty
# overlay-archives.txt fails the RELEASE rather than silently verifying an empty expectation.
# `gx_overlay_count` is the guard the library documents for exactly this; the optional list has no
# count guard because empty is legitimate there, so it is captured once and its STATUS checked.
if ! gx_overlay_count > /dev/null; then
    echo "ERROR: cannot read ${GX_OVERLAY_LIST} - refusing to verify against an unknown set." >&2
    exit 1
fi
if ! OPTIONAL_LINES="$(gx_overlay_optional)"; then
    echo "ERROR: cannot read ${GX_OVERLAY_OPTIONAL_LIST} - refusing to verify against an unknown set." >&2
    exit 1
fi

# 1. The escape-hatch stamp. Checked first and reported loudest, because it names the cause of
#    every failure that follows it and reading those instead sends you debugging a symptom.
if [[ -e "${RES_DIR}/OVERLAY-MISSING.txt" ]]; then
    fail "bundle is stamped OVERLAY-MISSING.txt - it was built with GX_BUNDLE_ALLOW_NO_OVERLAY=1."
    while IFS= read -r _l; do echo "        | ${_l}" >&2; done < "${RES_DIR}/OVERLAY-MISSING.txt"
else
    ok "no GX_BUNDLE_ALLOW_NO_OVERLAY stamp"
fi

# 2/3/4. The overlay itself.
EXPECTED_IN_PATCH=()
if [[ ! -d "${PATCH_DIR}" ]]; then
    fail "no Contents/Resources/GeneralsXPatch - the bundle carries no community-patch overlay."
    fail "  every peer will refuse this install with DATA_DIFFERS, which reads as a network fault."
else
    while IFS= read -r _name; do
        EXPECTED_IN_PATCH+=("${_name}")
        if [[ -f "${PATCH_DIR}/${_name}" && -s "${PATCH_DIR}/${_name}" ]]; then
            ok "required ${_name}"
        else
            fail "required archive missing or empty: ${_name} (moves dataID - no peer will join)"
        fi
    done < <(gx_overlay_archives)

    EXPECTED_OPTIONAL=()
    while IFS= read -r _line; do
        [[ -n "${_line}" ]] || continue
        _name="$(gx_overlay_optional_name "${_line}")"
        _dflt="$(gx_overlay_optional_default "${_line}")"
        _out="${_name}"
        [[ "${_dflt}" == "off" ]] && _out="${_name}${GX_OVERLAY_OFF_SUFFIX}"
        EXPECTED_IN_PATCH+=("${_out}")
        EXPECTED_OPTIONAL+=("${_name}")
        if [[ -f "${PATCH_DIR}/${_out}" && -s "${PATCH_DIR}/${_out}" ]]; then
            ok "optional ${_out} [${_dflt}]"
        else
            # dataID-neutral, so this does not cost a join - but it cannot be switched on later
            # either, because the toggle is a rename and not a download. A release short of it
            # ships content that no player can ever reach.
            fail "optional archive missing or empty: ${_out} (dataID-neutral, but unreachable later)"
        fi
    done <<< "${OPTIONAL_LINES}"

    # 5. Anything present that is not expected. The engine mounts every *.big it finds here, so a
    #    stale archive moves dataID exactly as a missing one does - and it splits the fleet
    #    silently, because both halves look healthy to themselves.
    while IFS= read -r _found; do
        [[ -n "${_found}" ]] || continue
        _known=0
        for _e in ${EXPECTED_IN_PATCH[@]+"${EXPECTED_IN_PATCH[@]}"}; do
            [[ "${_found}" == "${_e}" ]] && { _known=1; break; }
        done
        [[ ${_known} -eq 1 ]] || fail "unexpected archive in GeneralsXPatch: ${_found} (moves dataID)"
    done < <(cd "${PATCH_DIR}" && ls -1 2>/dev/null | grep -E '\.big(\.off)?$')

    # 6. optional.txt is what a launcher toggle UI reads, and the invariant is SET EQUALITY with
    #    overlay-optional.txt - checked in BOTH directions, once per name.
    #
    #    The one-directional version of this check (every entry has a file) was wrong in exactly
    #    the way this whole change is about, and Codex caught it: it passes an optional.txt that
    #    lists ONE archive while four are staged. The player then gets a toggle UI that can reach a
    #    quarter of the content in their own download, and nothing anywhere reports a problem. That
    #    truncation is not hypothetical - it is the precise shape the swallowed producer status
    #    produced in the bundler before this same commit fixed it, so a gate that cannot see it is
    #    a gate against the bug's symptom rather than the bug.
    #
    #    A required archive appearing here is also a failure: mandatory archives are not toggleable,
    #    and offering a switch that removes one hands the player a button that silently makes them
    #    unjoinable.
    if [[ ! -f "${PATCH_DIR}/optional.txt" ]]; then
        fail "GeneralsXPatch/optional.txt missing - nothing can enumerate the toggles."
    else
        LISTED_OPTIONAL=()
        _opt_before=${FAILURES}
        while IFS= read -r _l; do
            [[ -n "${_l}" ]] || continue
            _n="${_l%%|*}"
            if [[ ! -f "${PATCH_DIR}/${_n}" && ! -f "${PATCH_DIR}/${_n}${GX_OVERLAY_OFF_SUFFIX}" ]]; then
                fail "optional.txt offers '${_n}' but no such archive is in the bundle."
            fi
            # Duplicate detection has to precede the membership test, or a name listed twice
            # satisfies "every expected name appears" while the UI shows the same toggle twice.
            for _seen in ${LISTED_OPTIONAL[@]+"${LISTED_OPTIONAL[@]}"}; do
                [[ "${_seen}" == "${_n}" ]] && fail "optional.txt lists '${_n}' more than once."
            done
            LISTED_OPTIONAL+=("${_n}")

            _known=0
            for _e in ${EXPECTED_OPTIONAL[@]+"${EXPECTED_OPTIONAL[@]}"}; do
                [[ "${_n}" == "${_e}" ]] && { _known=1; break; }
            done
            if [[ ${_known} -eq 0 ]]; then
                # One problem, one message. A mandatory name gets the specific diagnosis and not
                # also the generic one, so the failure count stays a count of problems.
                _is_required=0
                while IFS= read -r _req; do
                    [[ "${_n}" == "${_req}" ]] && { _is_required=1; break; }
                done < <(gx_overlay_archives)
                if [[ ${_is_required} -eq 1 ]]; then
                    fail "optional.txt offers '${_n}', which is MANDATORY - toggling it off breaks every join."
                else
                    fail "optional.txt names '${_n}', which is on neither overlay list."
                fi
            fi
        done < "${PATCH_DIR}/optional.txt"

        for _e in ${EXPECTED_OPTIONAL[@]+"${EXPECTED_OPTIONAL[@]}"}; do
            _listed=0
            for _n in ${LISTED_OPTIONAL[@]+"${LISTED_OPTIONAL[@]}"}; do
                [[ "${_n}" == "${_e}" ]] && { _listed=1; break; }
            done
            [[ ${_listed} -eq 1 ]] || \
                fail "optional.txt omits '${_e}' - it ships in the bundle but no toggle can reach it."
        done

        # Scoped to this block's own failures: an earlier unrelated failure must not turn this
        # check's verdict into silence.
        [[ ${FAILURES} -eq ${_opt_before} ]] && ok "optional.txt names exactly the optional set, once each"
    fi

    # 6b. required.txt, and SET EQUALITY again for the same reason check 6 needs both directions.
    #
    # G4: no macOS payload carried this file until 10/09/2026. `game::read_required()` came back
    # empty on every Mac, so `discover()` could only ever answer `Verdict::Unknown` and a player
    # with a complete install was told "This install has no required.txt ...". A gate is what stops
    # that from being reintroduced by an edit to the bundler; a comment is not.
    #
    # An entry that is NOT mandatory is a failure of its own: required.txt is what the launcher
    # refuses to switch off, so a name that wandered in from the optional list would take a real
    # toggle away from the player and give them no way to get it back.
    if [[ ! -f "${PATCH_DIR}/required.txt" ]]; then
        fail "GeneralsXPatch/required.txt missing - the launcher can only answer 'Unknown' about \
this install, whatever is actually in it."
    else
        LISTED_REQUIRED=()
        _req_before=${FAILURES}
        while IFS= read -r _l; do
            _l="${_l%%#*}"
            _l="${_l#"${_l%%[![:space:]]*}"}"
            _l="${_l%"${_l##*[![:space:]]}"}"
            [[ -n "${_l}" ]] || continue
            _n="${_l%%|*}"
            [[ -f "${PATCH_DIR}/${_n}" && -s "${PATCH_DIR}/${_n}" ]] || \
                fail "required.txt names '${_n}' but no such archive is in the bundle."
            for _seen in ${LISTED_REQUIRED[@]+"${LISTED_REQUIRED[@]}"}; do
                [[ "${_seen}" == "${_n}" ]] && fail "required.txt lists '${_n}' more than once."
            done
            LISTED_REQUIRED+=("${_n}")
            _known=0
            while IFS= read -r _req; do
                [[ "${_n}" == "${_req}" ]] && { _known=1; break; }
            done < <(gx_overlay_archives)
            [[ ${_known} -eq 1 ]] || \
                fail "required.txt names '${_n}', which is not on the mandatory list."
        done < "${PATCH_DIR}/required.txt"

        while IFS= read -r _e; do
            [[ -n "${_e}" ]] || continue
            _listed=0
            for _n in ${LISTED_REQUIRED[@]+"${LISTED_REQUIRED[@]}"}; do
                [[ "${_n}" == "${_e}" ]] && { _listed=1; break; }
            done
            [[ ${_listed} -eq 1 ]] || \
                fail "required.txt omits '${_e}' - the launcher would call this install complete \
without it."
        done < <(gx_overlay_archives)

        [[ ${FAILURES} -eq ${_req_before} ]] && ok "required.txt names exactly the mandatory set, once each"
    fi
fi

# 7. The loose pair. It is staged under Resources/gamedata plus a manifest, and the engine reads it
#    from there through GENERALSX_DATA_OVERLAY (it is never copied into the game folder).
if [[ ! -f "${MANIFEST}" ]]; then
    fail "no Contents/Resources/gamedata.manifest - the loose player-colour data will never install."
else
    _n_manifest=0
    while IFS= read -r _rel; do
        [[ -n "${_rel}" ]] || continue
        _n_manifest=$((_n_manifest + 1))
        if [[ -f "${GAMEDATA_DIR}/${_rel}" && -s "${GAMEDATA_DIR}/${_rel}" ]]; then
            ok "gamedata ${_rel}"
        else
            fail "gamedata.manifest lists '${_rel}' but it is not staged (or is empty)."
        fi
    done < "${MANIFEST}"
    [[ ${_n_manifest} -gt 0 ]] || fail "gamedata.manifest is empty - it names nothing to install."
    # GeneralsX @feature Claude 25/09/2026 The Toxin Tractor balance file moves dataID, so a bundle
    # without it matches no peer, stamp or no stamp.
    # 26/09/2026: the same for every file the recipe writes - Weapon.ini and the Alpha Aurora fix files beside it
    # (make-weapon-ini.py --list). Listed, and their BYTES (Codex P2): recomputed from this bundle's own CoreINI by
    # the same recipe the bundler ran, so a hand-edited or stale copy cannot pass on its name alone.
    _recipe="$(python3 "${PROJECT_ROOT}/scripts/tooling/make-weapon-ini.py" --list 2>/dev/null)"
    [[ -n "${_recipe}" ]] || fail "cannot list the recipe's files (python3 missing?)."
    while IFS= read -r _rf; do
        [[ -n "${_rf}" ]] || continue
        grep -qxF "${_rf}" "${MANIFEST}" \
            || fail "gamedata.manifest does not list ${_rf} - its dataID matches no peer."
    done <<< "${_recipe}"
    if [[ -f "${GAMEDATA_DIR}/Data/INI/Weapon.ini" ]]; then
        _wdir="$(mktemp -d)" || _wdir=""
        if [[ -z "${_wdir}" || ! -d "${_wdir}" ]]; then
            fail "cannot create a scratch directory to recompute the recipe's files."
        elif ! python3 "${PROJECT_ROOT}/scripts/tooling/make-weapon-ini.py" --search "${PATCH_DIR}" --no-scb \
                --out "${_wdir}/Data/INI/Weapon.ini" > /dev/null 2>&1; then
            fail "cannot recompute the recipe's files from GeneralsXPatch (python3 missing, or a CoreINI the recipe refuses)."
        else
            while IFS= read -r _rf; do
                [[ -n "${_rf}" ]] || continue
                if [[ "${_rf}" == "Data/Scripts/SkirmishScripts.scb" ]]; then
                    # cut from the RETAIL loose file, which a release runner does not have: the bytes are pinned instead.
                    _want="$(python3 "${PROJECT_ROOT}/scripts/tooling/make-skirmish-scripts.py" --output-sha256)"
                    _got="$(shasum -a 256 "${GAMEDATA_DIR}/${_rf}" 2>/dev/null | cut -d' ' -f1)"
                    if [[ -n "${_want}" && "${_got}" == "${_want}" ]]; then
                        ok "gamedata ${_rf} is the recipe's pinned output (sha256 ${_want:0:12})"
                    else
                        fail "gamedata ${_rf} is not the recipe's pinned output (got ${_got:-none}, want ${_want}) - dataID matches no peer."
                    fi
                    continue
                fi
                if cmp -s "${_wdir}/${_rf}" "${GAMEDATA_DIR}/${_rf}"; then
                    ok "gamedata ${_rf} is what the recipe cuts from this bundle's CoreINI"
                else
                    fail "gamedata ${_rf} differs from what the recipe cuts from this bundle's CoreINI - dataID matches no peer."
                fi
            done <<< "${_recipe}"
        fi
        [[ -n "${_wdir}" && -d "${_wdir}" ]] && rm -rf -- "${_wdir}"
    fi
fi

# 8. Something has to CONSUME what was staged. Payload without a consumer is the exact shape of the
#    bug this feature shipped in its first cut: files present in the .app, launcher never reading
#    them, engine computing the wrong dataID, and every check that only looked at the payload
#    passing. Grepping the launcher is crude but it is the property that matters and it is cheap.
if [[ ! -f "${LAUNCHER}" ]]; then
    fail "no Contents/MacOS/run.sh - nothing consumes the staged data."
else
    if grep -q 'GENERALSX_PATCH_PATH' "${LAUNCHER}"; then
        ok "launcher exports GENERALSX_PATCH_PATH"
    else
        fail "launcher never sets GENERALSX_PATCH_PATH - the staged overlay is never mounted."
    fi
    if grep -q 'export GENERALSX_DATA_OVERLAY=' "${LAUNCHER}"; then
        ok "launcher exports GENERALSX_DATA_OVERLAY"
    else
        fail "launcher never sets GENERALSX_DATA_OVERLAY - the loose pair is never read, dataID is wrong."
    fi
    # GeneralsX @bugfix Claude 24/09/2026 The launcher must never write into the game folder.
    if grep -qE '(^|[^#])[[:space:]]cp[[:space:]].*gx_' "${LAUNCHER}"; then
        fail "launcher copies files into the game folder - it must never write there."
    else
        ok "launcher does not copy anything into the game folder"
    fi
    if [[ -s "${RES_DIR}/gamedata-shipped.sha256" ]]; then
        ok "gamedata-shipped.sha256 staged (cleanup of older installs)"
    else
        fail "no Contents/Resources/gamedata-shipped.sha256 - copies older builds left behind are never removed."
    fi
fi

# 9. The binary must not need the launcher to find its own libraries.
#
#    Check 8 above proves run.sh is the thing that mounts the overlay, which is correct. What was
#    never checked is that run.sh is the ONLY thing it has to be. Until 23/09/2026 the executable
#    carried four LC_RPATH entries into the build machine's home directory and ~25 absolute
#    /opt/homebrew LC_LOAD_DYLIB entries, and run.sh's DYLD_LIBRARY_PATH hid every one of them -
#    dyld searches DYLD_LIBRARY_PATH before the recorded path. Anything that exec'd
#    Contents/Resources/bin/GeneralsXZH directly died at dyld, and every published .app shipped a
#    username.
#
#    This is deliberately checked HERE as well as in the bundler, because the two answer different
#    questions. The bundler proves the bundle it just built is portable. This proves the ARTIFACT
#    IN FRONT OF IT is - a hand-assembled .app, one re-downloaded from an older run, or a tar
#    somebody rebuilt by other means. Same reasoning as the OVERLAY-MISSING stamp check.
#
#    Nothing is executed: gx_macho_assert_portable reads load commands and performs the @rpath
#    substitution dyld would perform. Running the game to find out would open a window on the
#    machine doing the verifying.
#
#    ONLY A MAC RUNS THIS CHECK. release.yml runs this script on ubuntu-latest, which has no otool.
#    The readers used to swallow that and this line printed "ok" over a bundle nothing had read.
#    They now fail closed, so a non-Mac host says SKIP instead - out loud, and not as a pass. The
#    property is still enforced where the bundle is built: bundle-macos-zh.sh and
#    bundle-macos-generals.sh refuse a non-portable bundle unconditionally, and build-macos.yml runs
#    this script on macOS against the unpacked artifact. A MAC without otool is not excused: there
#    the readers fail and so does this check, because a Mac is where it is supposed to run.
if [[ ! -f "${PROJECT_ROOT}/scripts/build/macos/lib-macho.sh" ]]; then
    fail "scripts/build/macos/lib-macho.sh is missing - cannot check bundle portability."
elif [[ "$(uname -s)" != "Darwin" ]]; then
    echo "  SKIP: check 9 (Mach-O portability) - $(uname -s) cannot read Mach-O load commands, so it is NOT proven by this run."
    echo "        It is enforced by the macOS bundlers and by build-macos.yml's run of this script."
else
    # shellcheck source=scripts/build/macos/lib-macho.sh
    source "${PROJECT_ROOT}/scripts/build/macos/lib-macho.sh"
    if gx_macho_assert_portable "${APP_DIR}" "$(basename "${APP_DIR}")" "${RES_DIR}/bin"; then
        ok "every Mach-O resolves its libraries inside the bundle"
    else
        fail "bundle portability not proven - a binary is unreadable, or resolves outside the bundle (details above)."
    fi
fi

echo ""
if [[ ${FAILURES} -gt 0 ]]; then
    echo "NOT PUBLISHABLE: ${FAILURES} check(s) failed." >&2
    echo "" >&2
    echo "This bundle would install a game that plays single-player perfectly and is refused by" >&2
    echo "every peer with DATA_DIFFERS - a failure that reads as a network fault, costs the player" >&2
    echo "the whole download, and can only be recovered by walking them through a manual file copy." >&2
    echo "" >&2
    echo "That diagnosis covers checks 1-8. A check 9 failure is a different fault with a" >&2
    echo "different cure: the bundle's own load commands are wrong, not its data. Rebuild it with" >&2
    echo "scripts/build/macos/bundle-macos-zh.sh, which relinks and refuses to emit a bundle that" >&2
    echo "cannot resolve its libraries; re-check any .app on its own with" >&2
    echo "  scripts/build/macos/lib-macho.sh <path/to/GeneralsXZH.app>" >&2
    echo "" >&2
    echo "To produce a publishable bundle the build host needs the community-patch archives:" >&2
    echo "  * point GX_PATCH_SRC at a directory holding them, or" >&2
    echo "  * configure an archive source for CI - see scripts/build/fetch-overlay-archives.sh" >&2
    echo "    and the 'Fetch community-patch archives' step in .github/workflows/build-macos.yml." >&2
    exit 1
fi

echo "PUBLISHABLE: bundle carries the full community-patch overlay and the loose data."
exit 0
