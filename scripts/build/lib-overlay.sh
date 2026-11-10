#!/bin/bash
# GeneralsX: Shared helpers for the community-patch overlay.
#
# Sourced by the macOS/Linux deploy scripts and the iOS packager so that all three read the
# SAME list (scripts/build/overlay-archives.txt) and resolve the overlay directory the same
# way. The Windows packager has its own copy of this logic in PowerShell and reads the same
# file; there is no third definition of the set anywhere.
#
# WHERE THE OVERLAY DIRECTORY GOES, and this is the part that is easy to get subtly wrong.
#
# The engine takes GENERALSX_PATCH_PATH if set, otherwise <exe dir>/GeneralsXPatch. That
# default is right on Windows and iOS and WRONG on macOS and Linux, for a reason that only
# shows up in a trace:
#
#   * Windows - the exe lives in the install folder and the assets live in the user's Zero
#     Hour folder. Disjoint, so the default resolves to a directory the asset scan never
#     visits. One mount, at filename precedence. This is the shape the feature was written for.
#   * iOS     - the exe is the bundle root and the assets are <bundle>/GameData. Also disjoint.
#   * macOS / Linux - THE EXE LIVES IN THE GAME FOLDER. So <exe dir>/GeneralsXPatch is a
#     SUBDIRECTORY OF THE ASSET ROOT, and loadBigFilesFromDirectory recurses - the primary scan
#     mounts those archives itself, under a cwd-relative spelling, and loadPatchOverlay's
#     already-mounted check (keyed on the full path string) does not recognise them. Measured:
#     each archive appears TWICE in the engine's [ARCHIVES] trace, once as
#     'GeneralsXPatch/x.big' and once as the absolute path. dataID still came out right because
#     both instances are the same bytes - which is exactly why this would have shipped.
#
# So on macOS and Linux the overlay is placed OUTSIDE the game folder, as a sibling
# (<runtime>/../GeneralsXPatch), and GENERALSX_PATCH_PATH is exported by the run wrapper. That
# reproduces the Windows topology, and it is the arrangement the dataID equality was actually
# measured on (arm C1).

# Absolute path to overlay-archives.txt, resolved from this file rather than from the caller's
# location - the three scripts that source this live at different depths.
_GX_OVERLAY_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GX_OVERLAY_LIST="${_GX_OVERLAY_LIB_DIR}/overlay-archives.txt"
GX_OVERLAY_OPTIONAL_LIST="${_GX_OVERLAY_LIB_DIR}/overlay-optional.txt"

# The suffix an archive carries while it is switched off. See overlay-optional.txt for why this
# is a rename and not a subdirectory (the overlay scan recurses).
GX_OVERLAY_OFF_SUFFIX=".off"

# Print one archive filename per line.
#
# `while IFS= read -r` and not `for f in $(...)`: these filenames are chosen by the community
# and already contain '!'; a future one may well contain a space, and zsh does not word-split
# unquoted parameters at all, so the for-loop form passes the whole list as one argument.
gx_overlay_archives() {
    if [[ ! -f "${GX_OVERLAY_LIST}" ]]; then
        echo "ERROR: overlay archive list not found at ${GX_OVERLAY_LIST}" >&2
        return 1
    fi
    local line
    local n=0
    while IFS= read -r line || [[ -n "${line}" ]]; do
        line="${line%%#*}"
        line="${line#"${line%%[![:space:]]*}"}"
        line="${line%"${line##*[![:space:]]}"}"
        [[ -n "${line}" ]] || continue
        printf '%s\n' "${line}"
        n=$((n + 1))
    done < "${GX_OVERLAY_LIST}"
    if [[ ${n} -eq 0 ]]; then
        echo "ERROR: overlay archive list ${GX_OVERLAY_LIST} names nothing." >&2
        return 1
    fi
}

# gx_overlay_count - the number of archives, or non-zero if the list cannot be read.
#
# THIS EXISTS BECAUSE `while read < <(producer)` SWALLOWS THE PRODUCER'S EXIT STATUS. A missing
# or empty list made gx_overlay_archives return 1 inside a process-substitution child, the loop
# body simply never ran, and the caller saw an empty "missing" set - i.e. SUCCESS. The iOS
# packager would then have created an empty GeneralsXPatch and shipped a bundle that cannot join,
# with its fatal-completeness guarantee reporting no problem. Caught by cross-model review.
# Every caller reads the list through this first.
gx_overlay_count() {
    local out
    out="$(gx_overlay_archives)" || return 1
    printf '%s\n' "${out}" | grep -c .
}

# gx_overlay_optional - print "<on|off>|<filename>|<label>" per optional archive.
#
# Empty output is legitimate here and is NOT an error, unlike the required list: a build with no
# optional content is a perfectly good build. A MISSING FILE still is an error, because silently
# shipping nothing optional when four entries were expected is the failure this whole pair of
# lists exists to make impossible.
gx_overlay_optional() {
    if [[ ! -f "${GX_OVERLAY_OPTIONAL_LIST}" ]]; then
        echo "ERROR: optional archive list not found at ${GX_OVERLAY_OPTIONAL_LIST}" >&2
        return 1
    fi
    local line
    while IFS= read -r line || [[ -n "${line}" ]]; do
        line="${line%%#*}"
        line="${line#"${line%%[![:space:]]*}"}"
        line="${line%"${line##*[![:space:]]}"}"
        [[ -n "${line}" ]] || continue
        # Three fields exactly; a malformed line is fatal rather than skipped, because a dropped
        # entry here means content that silently never ships.
        if [[ "${line}" != *"|"*"|"* ]]; then
            echo "ERROR: malformed line in ${GX_OVERLAY_OPTIONAL_LIST}: ${line}" >&2
            return 1
        fi
        local dflt="${line%%|*}"
        if [[ "${dflt}" != "on" && "${dflt}" != "off" ]]; then
            echo "ERROR: default must be 'on' or 'off', got '${dflt}' in ${GX_OVERLAY_OPTIONAL_LIST}" >&2
            return 1
        fi
        printf '%s\n' "${line}"
    done < "${GX_OVERLAY_OPTIONAL_LIST}"
}

# gx_overlay_optional_name <line> / gx_overlay_optional_default <line> / _label
gx_overlay_optional_default() { printf '%s\n' "${1%%|*}"; }
gx_overlay_optional_name()    { local r="${1#*|}"; printf '%s\n' "${r%%|*}"; }
gx_overlay_optional_label()   { printf '%s\n' "${1#*|*|}"; }

# gx_overlay_stage_optional <source-dir> <dest-dir> - copy each optional archive into dest under
# the name its default dictates: <name>.big when on, <name>.big.off when off. Prints one line per
# archive. Missing sources are fatal: an optional archive that is absent cannot be switched ON
# later by the launcher, so "optional" would quietly become "unavailable".
#
# GeneralsX: THE PRE-FLIGHT BELOW IS THE SAME BUG gx_overlay_count EXISTS
# FOR, and this function had no equivalent. `while read < <(gx_overlay_optional)` runs the parser
# in a CHILD, so a missing or malformed list makes it return 1 where nothing can see it: the loop
# body never runs, `missing` stays empty, and this returns 0 having staged NOTHING. The mandatory
# side was given gx_overlay_count precisely because that reads as success.
#
# It was harmless while the optional list only carried content that also arrived by other means.
# It is not harmless now that the list is the ONLY delivery path for !GXUI.big - a silently empty
# stage is exactly the failure this whole workstream exists to remove, wearing a green tick.
# Calling the parser here, in the current shell, is what makes its exit status reachable. An empty
# but well-formed list still succeeds; that remains a legitimate build.
gx_overlay_stage_optional() {
    local src="$1" dst="$2"
    local line name dflt out missing=()

    gx_overlay_optional > /dev/null || return 1
    mkdir -p "${dst}" || return 1
    while IFS= read -r line; do
        [[ -n "${line}" ]] || continue
        name="$(gx_overlay_optional_name "${line}")"
        dflt="$(gx_overlay_optional_default "${line}")"
        if [[ ! -s "${src}/${name}" ]]; then missing+=("${name}"); continue; fi
        out="${name}"
        [[ "${dflt}" == "off" ]] && out="${name}${GX_OVERLAY_OFF_SUFFIX}"
        cp "${src}/${name}" "${dst}/${out}" || return 1
        printf '%s %s\n' "${dflt}" "${out}"
    done < <(gx_overlay_optional)

    if [[ ${#missing[@]} -gt 0 ]]; then
        echo "ERROR: optional archives missing from ${src}:" >&2
        for name in "${missing[@]}"; do echo "  ${name}" >&2; done
        return 1
    fi
    return 0
}

# gx_overlay_optional_names - just the filenames, one per line. The staging loop above wants the
# whole record; the rsync exclusion lists and the "is this one of ours" checks want only the name,
# and re-deriving the field split at each call site is how the two forms eventually disagree.
# Same process-substitution pre-flight as the stager, and for a sharper reason: the iOS packager
# builds its rsync exclusions from this. A silently empty result there does not merely skip the
# exclusions - it copies every optional archive into GameData while the overlay copy is staged
# too, and the iPad mounts both.
gx_overlay_optional_names() {
    local line
    gx_overlay_optional > /dev/null || return 1
    while IFS= read -r line; do
        [[ -n "${line}" ]] || continue
        gx_overlay_optional_name "${line}"
    done < <(gx_overlay_optional)
}

# gx_overlay_generate <dest-dir> [game-dir] - build the overlay archives this project AUTHORS into
# dest, so that the staging functions above have something to copy.
#
# WHY A GENERATE STEP EXISTS AT ALL, when the other six archives are just files somebody has. The
# community archives are downloaded once and copied around; !GXUI.big carries THIS project's .wnd
# layout overrides and is derived from retail bytes every install already has. Requiring it to be
# present would only relocate the gap it was written to close - it existed on one Mac for months
# precisely because "get the file onto the packaging box" was a manual step nobody had automated.
# Generating it from the local install removes the transfer entirely and, because the recipe is
# deterministic, every box produces the identical bytes (measured: sha256 b61c9bcce... on a rebuild
# against the installed copy), so peers agree on assetID as well.
#
# FATAL, NOT BEST-EFFORT. A skipped generate ships a payload that is short one listed archive, and
# the staging functions would then fail anyway - later, with a worse message. Callers that can
# legitimately proceed without an overlay (a --dev iOS bundle) must decide that BEFORE calling.
gx_overlay_generate() {
    local dst="$1" game="${2:-}"
    local script="${_GX_OVERLAY_LIB_DIR}/../tooling/build-overlay-generated.py"
    local py=""
    local c

    if [[ ! -f "${script}" ]]; then
        echo "ERROR: overlay generator not found at ${script}" >&2
        return 1
    fi
    # python3 first, then python: the Windows box answers to the bare name and macOS/Linux to the
    # versioned one. Never `python` alone on a Mac, where it may still be a 2.x stub.
    for c in python3 python; do
        if command -v "${c}" > /dev/null 2>&1; then py="${c}"; break; fi
    done
    if [[ -z "${py}" ]]; then
        echo "ERROR: no python interpreter on PATH; cannot build the generated overlay archives." >&2
        return 1
    fi

    if [[ -n "${game}" ]]; then
        "${py}" "${script}" --dest "${dst}" --game-dir "${game}" || return 1
    else
        "${py}" "${script}" --dest "${dst}" || return 1
    fi
    return 0
}

# gx_overlay_dir <runtime-dir> - the sibling directory the run wrapper will point at.
gx_overlay_dir() {
    local runtime="$1"
    printf '%s/GeneralsXPatch\n' "$(cd "${runtime}/.." && pwd)"
}

# gx_overlay_verify <dir> - every listed archive must be present and non-empty IN THAT DIRECTORY.
#
# The strict form, for the two places that BUILD a shipped artifact: the iOS bundle and the
# Windows payload. There the overlay directory is the only place the archives can come from, and
# a missing one is fatal - exactly like the launcher's mandatory payload entries. An install
# short one of these does not fail visibly: it starts, plays single-player perfectly, and is
# refused by every peer with DATA_DIFFERS, which reads as a network fault.
gx_overlay_verify() {
    local dir="$1"
    local missing=()
    local name

    gx_overlay_count > /dev/null || return 1

    if [[ ! -d "${dir}" ]]; then
        echo "ERROR: overlay directory ${dir} does not exist." >&2
        echo "  It must hold the archives listed in ${GX_OVERLAY_LIST}." >&2
        echo "  Run scripts/build/migrate-patch-overlay.sh once to populate it." >&2
        return 1
    fi

    while IFS= read -r name; do
        [[ -s "${dir}/${name}" ]] || missing+=("${name}")
    done < <(gx_overlay_archives)

    if [[ ${#missing[@]} -gt 0 ]]; then
        echo "ERROR: overlay directory ${dir} is incomplete." >&2
        for name in "${missing[@]}"; do echo "  missing: ${name}" >&2; done
        return 1
    fi

    return 0
}

# gx_overlay_where <runtime-dir> - print one "<state> <archive>" line per listed archive,
# where state is OVERLAY, INFOLDER, BOTH or ABSENT. Exit status is non-zero if any is ABSENT.
#
# THE DEPLOY SCRIPTS MUST ASK THIS QUESTION AND NOT THE STRICT ONE, because the strict one is
# false on a peer that is working perfectly. Mounting these archives from the game folder and
# mounting them from an overlay outside it produce the IDENTICAL dataID - measured both ways -
# so a peer that has not migrated is not broken and telling it that every deploy is how a
# warning gets muted and stops being read. The condition that actually matters is "this peer
# will not load these archives at all", and only ABSENT means that.
#
# BOTH is called out because it is the one arrangement that is subtly wasteful rather than
# wrong: on macOS and Linux the game folder is also the executable directory, so an archive
# present in both places is mounted twice - same bytes, right dataID, two copies of that
# archive's directory table in memory for nothing.
# overlay-archives.txt's dated exception: mandatory to SHIP (gx_overlay_verify and the packagers still
# require it) but textures only - an install without it joins everyone (dataID unchanged), so readiness never fails on it.
# !!GXModelsHD.big (the rounded/relief W3D model pack) joins it - art only, the same dated
# exception, measured not to move dataID; the name of the list predates it ("texture only" = "art only, no dataID").
GX_OVERLAY_TEXTURE_ONLY=('!!GXPropsHD.big' '!!GXModelsHD.big')
gx_overlay_texture_only() {
    local n
    for n in "${GX_OVERLAY_TEXTURE_ONLY[@]}"; do [[ "$1" == "${n}" ]] && return 0; done
    return 1
}

gx_overlay_where() {
    local runtime="$1"
    local overlay
    overlay="$(gx_overlay_dir "${runtime}")"
    local rc=0
    local name in_o in_r

    gx_overlay_count > /dev/null || return 1

    while IFS= read -r name; do
        in_o=0; in_r=0
        [[ -s "${overlay}/${name}" ]] && in_o=1
        [[ -s "${runtime}/${name}" ]] && in_r=1
        if   [[ ${in_o} -eq 1 && ${in_r} -eq 1 ]]; then printf 'BOTH %s\n'     "${name}"
        elif [[ ${in_o} -eq 1 ]];                 then printf 'OVERLAY %s\n'  "${name}"
        elif [[ ${in_r} -eq 1 ]];                 then printf 'INFOLDER %s\n' "${name}"
        elif gx_overlay_texture_only "${name}"; then printf 'HD-ABSENT %s\n' "${name}"	# joins still work
        else printf 'ABSENT %s\n' "${name}"; rc=1
        fi
    done < <(gx_overlay_archives)

    # Anything in the overlay directory that is NOT on the list, because the engine mounts every
    # .big it finds there and an extra one moves dataID exactly as a missing one does.
    #
    # THIS IS THE ONLY THING STANDING BETWEEN A LIST EDIT AND A SILENT SPLIT FLEET. Nothing
    # deletes for us: this script iterates the CURRENT entries, and on Windows the launcher's
    # place_tree() only ever copies staged files (gxlaunch.c:1831). So dropping an entry from
    # overlay-archives.txt leaves every existing install still mounting the old archive - old
    # peers keep the old dataID while fresh installs move to the new one, and the two sets stop
    # being able to see each other. Removing an entry is therefore NOT a one-line change; see
    # the note in overlay-archives.txt.
    if [[ -d "${overlay}" ]]; then
        local found expected opt
        expected="$(gx_overlay_archives)"
        # An ENABLED optional archive is a legitimate resident and must not read as stale. A
        # disabled one never reaches this loop at all - it is named .big.off, so the *.big glob
        # does not see it, which is the same property that keeps the engine from mounting it.
        while IFS= read -r opt; do
            [[ -n "${opt}" ]] && expected+=$'\n'"$(gx_overlay_optional_name "${opt}")"
        done < <(gx_overlay_optional 2>/dev/null)
        while IFS= read -r found; do
            [[ -n "${found}" ]] || continue
            if ! printf '%s\n' "${expected}" | grep -Fxq "${found}"; then
                printf 'UNEXPECTED %s\n' "${found}"
                rc=1
            fi
        done < <(cd "${overlay}" && ls -1 ./*.big 2>/dev/null | sed 's|^\./||')
    fi

    return ${rc}
}

# GeneralsX: gx_overlay_stage_shipped_maps <patch-dir>: the maps in overlay-maps.txt,
# as GeneralsXPatch/ShippedMaps/<Name>/ (see that file). Loose files, inert to the engine's "*.big" overlay
# scan; the launcher installs them into <userdata>/Maps. Fails (rc 1, nothing half-staged is trusted) when a
# source is missing or its .map does not hash to the listed value. GX_MAPS_SRC overrides the source root (tests).
gx_overlay_stage_shipped_maps() {
    local dest="${1:?patch dir}" here root list name want got f
    here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    root="${GX_MAPS_SRC:-${here}/../test/maps}"
    list="${GX_MAPS_LIST:-${here}/overlay-maps.txt}"
    rm -rf "${dest}/ShippedMaps"
    while IFS='|' read -r name want; do
        case "${name}" in ''|'#'*) continue ;; esac
        [ -f "${root}/${name}/${name}.map" ] || { echo "ERROR: shipped map ${name}: ${root}/${name}/${name}.map is missing" >&2; return 1; }
        if command -v sha256sum >/dev/null 2>&1; then got="$(sha256sum "${root}/${name}/${name}.map" | cut -d' ' -f1)" || return 1
        else got="$(shasum -a 256 "${root}/${name}/${name}.map" | cut -d' ' -f1)" || return 1; fi
        [ -n "${got}" ] || { echo "ERROR: shipped map ${name}: could not hash the .map" >&2; return 1; }
        [ "${got}" = "${want}" ] || { echo "ERROR: shipped map ${name}: .map sha256 ${got} is not the ${want} overlay-maps.txt lists" >&2; return 1; }
        mkdir -p "${dest}/ShippedMaps/${name}" || return 1
        for f in "${root}/${name}"/*; do
            [ -f "${f}" ] && { cp "${f}" "${dest}/ShippedMaps/${name}/" || return 1; }
        done
    done < "${list}"
    [ -d "${dest}/ShippedMaps" ] || { echo "ERROR: overlay-maps.txt lists no map" >&2; return 1; }
}
