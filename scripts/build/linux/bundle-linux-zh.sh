#!/bin/bash
# GeneralsX @build BenderAI 03/03/2026 Bundle Linux GeneralsXZH binary + .so libs into a tarball archive
# Packages the same files as deploy-linux-zh.sh into GeneralsXZH-linux-x86_64.tar.gz

set -e

# GeneralsX @bugfix BenderAI 09/03/2026 Resolve repository root correctly from scripts/build/linux.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build/linux64-deploy"
DXVK_LIB_DIR="${BUILD_DIR}/_deps/dxvk-src/lib"
SDL3_LIB_DIR="${BUILD_DIR}/_deps/sdl3-build"
SDL3_IMAGE_LIB_DIR="${BUILD_DIR}/_deps/sdl3_image-build"
GAMESPY_LIB="${BUILD_DIR}/libgamespy.so"
FFMPEG_LIB_DIR="/usr/lib/x86_64-linux-gnu"
FFMPEG_DEP_LIB_DIR="/lib/x86_64-linux-gnu"
BINARY_SRC="${BUILD_DIR}/GeneralsMD/GeneralsXZH"
DXVK_CONF_SRC="${PROJECT_ROOT}/resources/dxvk/dxvk.conf"
OUTPUT_TARBALL="${PROJECT_ROOT}/GeneralsXZH-linux-x86_64.tar.gz"

# GeneralsX @bugfix Claude 18/09/2026 THE OVERLAY'S SOURCE DIRECTORIES, AS ONE LIST, UP HERE.
#
# ONE list feeds both the archive search (gx_find_overlay_archive) and the generator's --game-dir
# candidates. The generator used to get ${GX_GAME_DATA:-${GX_OVERLAY_SRC:-${GX_PATCH_SRC}}} - a
# chain of its own, with the macOS bundler's GX_PATCH_SRC in it, which nothing else in this file
# reads - so the two could name different places. They now cannot.
#
# NOT HARDCODED: /root/gamedata. The harness host keeps the retail data and the mandatory archives
# there, and release 85f9a6650 needed GX_OVERLAY_SRC=/root/gamedata to package on it. That is one
# host's layout, so it is recorded in docs/WORKDIR/RUNBOOK_2026-08-24_ATOMIC_SHIP.md, not baked in.
#
# GX_OVERLAY_SRC, NOT GX_PATCH_SRC, and that disagreement with the macOS bundler is left exactly as
# it was found. Renaming either one is a breaking change for whoever sets it on a packaging box, and
# it is not this change's to make. Whoever unifies them must update both scripts and every
# packaging box in the same step.
#
# Defined before the build checks so GX_OVERLAY_RESOLVE_ONLY=1 can answer "which WindowZH.big would
# this read" on a box with no build at all. scripts/test/overlay-gamedir-test.sh is the check that
# fails if a directory the archive search walks stops being one the generator tries.
GX_SRC_DIR="${GX_OVERLAY_SRC:-${HOME}/GeneralsX/GeneralsZH}"
GX_OVERLAY_SRC_DIRS=("${GX_SRC_DIR}/../GeneralsXPatch" "${GX_SRC_DIR}" "${GX_SRC_DIR}/GeneralsXPatch")
# GX_GAME_DATA first: it is the explicit override, and the generator's own error names it.
_gx_gen="${GX_REPO_ROOT:-${PROJECT_ROOT}}/scripts/tooling/build-overlay-generated.py"
_gx_gen_dirs=()
for _d in ${GX_GAME_DATA:+"${GX_GAME_DATA}"} "${GX_OVERLAY_SRC_DIRS[@]}"; do
    _gx_gen_dirs+=(--game-dir "${_d}")
done
if [[ -n "${GX_OVERLAY_RESOLVE_ONLY:-}" ]]; then
    exec python3 "${_gx_gen}" --resolve "${_gx_gen_dirs[@]}"
fi

# GeneralsX @bugfix Claude 20/08/2026 THE TARBALL SHIPPED ZERO ARCHIVES, and that is a join refusal.
#
# This bundled the engine and its .so files and nothing else, while its macOS counterpart carries the
# overlay inside the .app at Contents/Resources/GeneralsXPatch. So a Linux player who installed this
# got the right sourceID and the WRONG dataID - every peer refused them with DATA_DIFFERS, which
# reads as a network fault rather than a missing archive. Measured 2026-08-20: 0 of the 8 archives
# the fleet mounts were in the tarball.
#
# The same overlay list every other platform reads. There is no second definition of the set.
# shellcheck source=scripts/build/lib-overlay.sh
source "${SCRIPT_DIR}/../lib-overlay.sh"

copy_ldd_deps() {
    local root="$1"
    [[ -e "${root}" ]] || return 0

    while IFS= read -r dep; do
        case "${dep}" in
            linux-vdso.so.1 | \
            /lib64/ld-linux* | /lib/*/ld-linux* | /usr/lib/*/ld-linux* | /usr/lib64/ld-linux* | \
            /lib/*/libc.so.* | /lib64/libc.so.* | /usr/lib/*/libc.so.* | /usr/lib64/libc.so.* | \
            /lib/*/libm.so.* | /lib64/libm.so.* | /usr/lib/*/libm.so.* | /usr/lib64/libm.so.* | \
            /lib/*/libpthread.so.* | /lib64/libpthread.so.* | /usr/lib/*/libpthread.so.* | /usr/lib64/libpthread.so.* | \
            /lib/*/librt.so.* | /lib64/librt.so.* | /usr/lib/*/librt.so.* | /usr/lib64/librt.so.* | \
            /lib/*/libdl.so.* | /lib64/libdl.so.* | /usr/lib/*/libdl.so.* | /usr/lib64/libdl.so.*)
                continue
                ;;
        esac

        cp -a "${dep}" "${BUNDLE_DIR}/" 2>/dev/null || true
        if [[ -L "${dep}" ]]; then
            local resolved
            resolved="$(readlink -f "${dep}")"
            cp -a "${resolved}" "${BUNDLE_DIR}/" 2>/dev/null || true
        fi
    done < <(ldd "${root}" | awk '{for (i = 1; i <= NF; ++i) { if ($i ~ /^\//) { print $i; break } }}' | sort -u)
}

echo "Bundling GeneralsXZH (Linux x86_64)"

# Validate binary
if [[ ! -f "${BINARY_SRC}" ]]; then
    echo "ERROR: Binary not found at ${BINARY_SRC}"
    echo "Build first: ./scripts/build/linux/docker-build-linux-zh.sh linux64-deploy"
    exit 1
fi
if [[ ! -s "${BINARY_SRC}" ]]; then
    echo "ERROR: Binary at ${BINARY_SRC} is empty - build may have failed"
    exit 1
fi

# Check if DXVK libraries exist
if [[ ! -d "${DXVK_LIB_DIR}" ]]; then
    echo "ERROR: DXVK libraries not found at ${DXVK_LIB_DIR}"
    echo "Configure first: ./scripts/build/linux/docker-configure-linux.sh linux64-deploy"
    exit 1
fi

# Check if SDL3 libraries exist
if [[ ! -d "${SDL3_LIB_DIR}" ]]; then
    echo "ERROR: SDL3 libraries not found at ${SDL3_LIB_DIR}"
    echo "Build first: ./scripts/build/linux/docker-build-linux-zh.sh linux64-deploy"
    exit 1
fi

if [[ ! -d "${SDL3_IMAGE_LIB_DIR}" ]]; then
    echo "ERROR: SDL3_image libraries not found at ${SDL3_IMAGE_LIB_DIR}"
    echo "Build first: ./scripts/build/linux/docker-build-linux-zh.sh linux64-deploy"
    exit 1
fi

# Check if GameSpy library exists
if [[ ! -f "${GAMESPY_LIB}" ]]; then
    echo "ERROR: GameSpy library not found at ${GAMESPY_LIB}"
    echo "Build first: ./scripts/build/linux/docker-build-linux-zh.sh linux64-deploy"
    exit 1
fi

# Prepare temp staging directory
STAGE_DIR="$(mktemp -d)"
trap 'rm -rf "${STAGE_DIR}"' EXIT
BUNDLE_DIR="${STAGE_DIR}/GeneralsXZH-linux"
mkdir -p "${BUNDLE_DIR}"

echo "  Staging files to ${BUNDLE_DIR}..."

# Binary
echo "  + GeneralsXZH"
cp "${BINARY_SRC}" "${BUNDLE_DIR}/GeneralsXZH"
chmod +x "${BUNDLE_DIR}/GeneralsXZH"

# GeneralsX @build Claude 25/09/2026 The openal-soft this engine was built against, and a runpath
# of $ORIGIN so it is the one that loads. The tarball shipped no libopenal at all, so every player
# ran the distro's copy - see scripts/build/linux/lib-openal.sh.
# shellcheck source=scripts/build/linux/lib-openal.sh
source "${SCRIPT_DIR}/lib-openal.sh"
gx_origin_runpath "${BUNDLE_DIR}/GeneralsXZH" || exit 1
gx_openal_stage "${BUILD_DIR}" "${BUNDLE_DIR}" "${BUNDLE_DIR}/licenses/openal-soft" || exit 1

# DXVK libraries
echo "  + DXVK libraries"
cp "${DXVK_LIB_DIR}"/libdxvk_d3d8.so* "${BUNDLE_DIR}/" 2>/dev/null || echo "    (WARNING: libdxvk_d3d8.so not found)"
cp "${DXVK_LIB_DIR}"/libdxvk_d3d9.so* "${BUNDLE_DIR}/" 2>/dev/null || true

# SDL3 and SDL3_image libraries
echo "  + SDL3 libraries"
cp "${SDL3_LIB_DIR}"/libSDL3.so* "${BUNDLE_DIR}/"
cp "${SDL3_IMAGE_LIB_DIR}"/libSDL3_image.so* "${BUNDLE_DIR}/"

# GameSpy library
echo "  + GameSpy library"
cp "${GAMESPY_LIB}" "${BUNDLE_DIR}/"

# SagePatch (optional, gated by RTS_BUILD_OPTION_SAGE_PATCH at configure time).
SAGE_PATCH_LIB="${BUILD_DIR}/Patches/SagePatch/libsage_patch.so"
if [[ -f "${SAGE_PATCH_LIB}" ]]; then
    echo "  + libsage_patch (SagePatch QoL)"
    cp "${SAGE_PATCH_LIB}" "${BUNDLE_DIR}/"
fi

# GeneralsX @build GitHubCopilot 17/05/2026 Copy FFmpeg runtime libs transitively so the bundle is independent of host SONAME layout.
echo "  + FFmpeg runtime libraries"
shopt -s nullglob
ffmpeg_roots=(
    "${FFMPEG_LIB_DIR}"/libavcodec.so*
    "${FFMPEG_LIB_DIR}"/libavformat.so*
    "${FFMPEG_LIB_DIR}"/libavutil.so*
    "${FFMPEG_LIB_DIR}"/libswresample.so*
    "${FFMPEG_LIB_DIR}"/libswscale.so*
    "${FFMPEG_DEP_LIB_DIR}"/libavcodec.so*
    "${FFMPEG_DEP_LIB_DIR}"/libavformat.so*
    "${FFMPEG_DEP_LIB_DIR}"/libavutil.so*
    "${FFMPEG_DEP_LIB_DIR}"/libswresample.so*
    "${FFMPEG_DEP_LIB_DIR}"/libswscale.so*
)
shopt -u nullglob
for ffmpeg_root in "${ffmpeg_roots[@]}"; do
    cp -a "${ffmpeg_root}" "${BUNDLE_DIR}/" 2>/dev/null || true
    copy_ldd_deps "${ffmpeg_root}"
done

if ! compgen -G "${BUNDLE_DIR}/libavcodec.so*" > /dev/null; then
    echo "ERROR: Missing required bundle library libavcodec.so*"
    exit 1
fi

# DXVK config
if [[ -f "${DXVK_CONF_SRC}" ]]; then
    echo "  + dxvk.conf"
    cp "${DXVK_CONF_SRC}" "${BUNDLE_DIR}/dxvk.conf"
else
    echo "WARNING: ${DXVK_CONF_SRC} not found - terrain shaders may fail"
fi

# GeneralsX @bugfix Claude 20/08/2026 Stage the community-patch overlay.
#
# WHY IT GOES BESIDE THE EXE HERE AND NOT ON A DEPLOYED LINUX BOX. lib-overlay.sh documents that
# <exe dir>/GeneralsXPatch is WRONG for a deployed macOS/Linux install, because there the exe lives
# INSIDE the game folder, so the overlay becomes a subdirectory of the asset root and
# loadBigFilesFromDirectory mounts every archive a second time under a cwd-relative spelling.
#
# The tarball's layout is not that layout: the exe sits in GeneralsXZH-linux/ and the game data is a
# SIBLING directory the wrapper never cds into. The overlay is therefore outside the asset root, and
# run.sh exports GENERALSX_PATCH_PATH explicitly so the engine cannot resolve it any other way.
#
# GeneralsX @bugfix Claude 14/09/2026 THE TARBALL SHIPPED NO OPTIONAL CONTENT AND NO MANIFESTS.
#
# This block read overlay-archives.txt and stopped there. overlay-optional.txt was never opened, so
# the Linux tarball carried the 2 mandatory archives while the macOS .app carried those plus the 5
# optional ones - and it carried neither optional.txt nor required.txt, which are the two files the
# launcher reads to answer "is this install complete" and "what may I toggle".
#
# MEASURED HERE 14/09/2026, and the two halves have different authorities, so they are separated.
# What this session checked itself: `grep -n 'optional.txt|required.txt|gx_overlay_stage_optional'`
# over this file matched NOTHING while the same pattern matched 10 lines in bundle-macos-zh.sh; and
# a shipped .app on this box (gx-testreset-20260826) carries 340_ControlBarProZH.big,
# 340_ControlBarPro-Fix1440ZH.big, 340_ControlBarPro1440ZH.big, !HotkeysLeikezeZH.big and !GXUI.big
# alongside the mandatory pair, plus an optional.txt this file now reproduces byte for byte. That
# .app predates required.txt, which bundle-macos-zh.sh has written since 10/09/2026.
# Reported by the republish packaging run rather than re-measured here: the live tarball is
# 214,105,383 bytes and the optional set is ~59 MB UNCOMPRESSED, so no published Linux build has
# ever carried it. The tarball grows by far less than that: overlay-optional.txt records the
# COMPRESSED sizes - 340_ControlBarProZH 20.7 MB, 340_ControlBarPro-Fix1440ZH 117 KB,
# 340_ControlBarPro1440ZH 3.4 KB, !HotkeysLeikezeZH 186 KB - and !GXUI.big is 46,979 bytes on
# disk, so the real growth is about 21 MB and the artifact lands near 235 MB, not 273.
#
# THIS IS NOT A DATA ID BUG. Every entry in overlay-optional.txt is measured dataID-neutral (see the
# per-archive "0 fingerprinted" counts recorded in that file), so a Linux player joins the fleet
# today. What they lose is the content itself - and, because the toggle is a RENAME and not a
# download, the ability to switch any of it on later. That is why a missing optional archive is
# fatal here by default, exactly as in bundle-macos-zh.sh and package-windows-share.ps1.
#
# THE PROCESS-SUBSTITUTION TRAP, WHICH THIS FILE STILL HAD ON THE MANDATORY SIDE. lib-overlay.sh
# documents gx_overlay_count as existing because `while read < <(gx_overlay_archives)` runs the
# producer in a CHILD and throws its exit status away: a missing or empty list makes the loop body
# run zero times, gx_missing stays empty, and the script reports success having staged NOTHING. The
# loop below was written without that pre-flight, so an unreadable overlay-archives.txt would have
# produced a tarball with an empty GeneralsXPatch and printed "0 archive(s)" as though that were
# fine. gx_overlay_count is now called first, and the optional list is CAPTURED into a variable so
# its status is checked by the same command that reads its bytes - the stronger form, and the one
# bundle-macos-zh.sh adopted for its own OVERLAY_OPTIONAL_LINES capture, after exactly this failure
# let optional.txt advertise a set that was never staged.
GX_PATCH_STAGE="${BUNDLE_DIR}/GeneralsXPatch"
mkdir -p "${GX_PATCH_STAGE}"

# GeneralsX @feature Claude 15/09/2026 GENERATE !GXUI.big HERE, the way the Windows packager
# already does. It is now on overlay-archives.txt, so a missing copy is a refusal rather than a
# quiet omission - and only package-windows-share.ps1 ran the generator, despite a comment in
# overlay-optional.txt claiming iOS, Linux and Windows all did. They did not; that line was wrong
# when it was written and is corrected in the same commit as this.
#
# Deterministic and needs no network: it slices retail bytes out of WindowZH.big and rebuilds the
# archive, so two hosts produce identical output and assetID agrees. The source is looked for the
# same way the archives are, and a host without retail data gets the generator's own message
# naming --game-dir rather than a confusing "mandatory archive missing" later on.

# Generate into a TEMP DIR that is then searched FIRST, not straight into the stage. Writing into
# the stage looks equivalent and is not: the staging loop below resolves every archive through
# gx_find_overlay_archive, which searches the SOURCE directories, so a file placed directly in the
# stage is invisible to it and the mandatory check then refuses a bundle whose archive is sitting
# right there. Routing the generated copy through the same search keeps ONE code path, so the -f/-s
# test, the plain-cp dereference and the "only list what was actually staged" rule all apply to it
# exactly as they do to a downloaded archive.
GX_GENERATED_DIR="$(mktemp -d)"
trap 'rm -rf "${GX_GENERATED_DIR}"' EXIT
if [[ -f "${_gx_gen}" ]]; then
    if ! python3 "${_gx_gen}" --dest "${GX_GENERATED_DIR}" "${_gx_gen_dirs[@]}" --quiet; then
        echo "ERROR: could not generate the project's own overlay archives." >&2
        echo "  Set GX_OVERLAY_SRC to the directory holding the retail data AND the overlay archives -" >&2
        echo "  /root/gamedata on the harness host - or GX_GAME_DATA to one holding WindowZH.big alone." >&2
        exit 1
    fi
else
    echo "ERROR: overlay generator missing at ${_gx_gen} - run this from a checkout." >&2
    exit 1
fi

# GX_SRC_DIR and the directories searched around it are defined at the top of this file, as
# GX_OVERLAY_SRC_DIRS, so the generator step above searches the same ones.
#
# The same two directories this script already searched - the game folder and a GeneralsXPatch
# underneath it - now behind one helper so the mandatory and optional passes cannot drift on which
# places they look or on what counts as a usable file.
#
# -f AND -s, both, which is a deliberate tightening of the bare -f this loop used before. Measured
# on this box (bash 5.3.15, Darwin 25.6.0, APFS) rather than reasoned, because the two tests fail on
# OPPOSITE inputs and neither is a superset of the other:
#
#   test          directory   zero-byte file   1-byte file
#   -f            FALSE       TRUE             TRUE
#   -s            TRUE        FALSE            TRUE
#
# -f alone accepts a ZERO-BYTE archive: the tarball then looks complete, the engine mounts nothing
# from it, and the player gets a dataID that matches no peer with no message anywhere saying why.
# That is the failure this whole block exists to end, so the rule is "a regular file with bytes in
# it". The same reasoning, and the same table, is recorded above find_overlay_archive in
# bundle-macos-zh.sh.
# THE SIBLING IS SEARCHED FIRST, because it is the canonical location and this function did not
# look there. lib-overlay.sh's gx_overlay_dir() is `<runtime>/../GeneralsXPatch` - a SIBLING of the
# game folder, not a child - and it is what migrate-patch-overlay.sh and deploy-linux-zh.sh both
# compute. The macOS bundler's first search dir is that same sibling (GX_PATCH_SRC defaults to
# ~/GeneralsX/GeneralsXPatch), and so is the iOS packager's. Only this script looked exclusively
# game-folder-relative, so on a migrated packaging box - the arrangement lib-overlay.sh documents as
# correct for Linux - it opened neither directory the archives are in. That was survivable while
# only two archives were staged and loudly fatal; with the optional set it would silently ship five
# fewer. The game-folder paths stay, after it, so an unmigrated box keeps working.
gx_find_overlay_archive() {
    local name="$1" dir
    for dir in "${GX_GENERATED_DIR}" "${GX_OVERLAY_SRC_DIRS[@]}"; do
        [[ -n "${dir}" ]] || continue
        if [[ -f "${dir}/${name}" && -s "${dir}/${name}" ]]; then
            printf '%s\n' "${dir}/${name}"
            return 0
        fi
    done
    return 1
}

# The two lists have different consequences and are tracked separately. Discarding the mandatory
# archives because an optional one was absent would take a tarball that joins the fleet perfectly
# and make it join nobody.
gx_staged=0
gx_missing_required=()
gx_missing_optional=()
gx_staged_optional=()

# Required: the set every peer must mount or dataID does not match.
gx_overlay_count > /dev/null || exit 1
while IFS= read -r gx_arch; do
    [[ -n "${gx_arch}" ]] || continue
    if gx_src="$(gx_find_overlay_archive "${gx_arch}")"; then
        # PLAIN cp, NOT `cp -a`, and this is the whole point of the -s test above. A packaging
        # box that SYMLINKS its archives rather than duplicating them is the normal arrangement -
        # the Linux build host used for the 14/09/2026 republish has every .big in /root/gamedata
        # reached through a symlink - and `[[ -f link && -s link ]]` is TRUE for a symlink to a
        # real file, because both tests follow the link. `cp -a` would then preserve the LINK,
        # putting a dangling symlink inside the tarball: GeneralsXPatch looks complete,
        # required.txt validates, the engine mounts nothing, and dataID matches no peer with
        # nothing saying why. Measured on this box: `cp -a link dst` yields a symlink, `cp link
        # dst` yields the file. bundle-macos-zh.sh uses plain cp on the same two lines.
        cp "${gx_src}" "${GX_PATCH_STAGE}/${gx_arch}" || exit 1
        echo "      [required] ${gx_arch}"
        gx_staged=$((gx_staged + 1))
    else
        gx_missing_required+=("${gx_arch}")
    fi
done < <(gx_overlay_archives)

# Optional: ships either way, and the default decides only what the file is CALLED on arrival -
# <name>.big is mounted, <name>.big.off is not (the overlay scan matches "*.big", and
# SearchStringMatches requires pattern and name to terminate together). Shipping it switched off
# rather than not shipping it is the point: enabling it later is then a rename, not a download.
#
# Captured, not process-substituted: one read, one status, and the loop cannot see different bytes
# from the ones the check passed. Empty output is legitimate (a build with no optional content is a
# good build), so the guard is on STATUS and never on count - and `<<<` on an empty string yields a
# single empty line, which the -n test drops.
GX_OPTIONAL_LINES="$(gx_overlay_optional)" || exit 1
while IFS= read -r gx_line; do
    [[ -n "${gx_line}" ]] || continue
    gx_name="$(gx_overlay_optional_name "${gx_line}")"
    gx_dflt="$(gx_overlay_optional_default "${gx_line}")"
    # if/else rather than `[[ test ]] && assignment`, because errexit exempts every element of an
    # AND-list but the last: the bare form is safe at statement level and exits with status 1 the
    # moment anyone moves it into a helper or to the end of the script. The GX_OVERLAY_SIBLING
    # comment in deploy-linux-zh.sh records the same measurement.
    if [[ "${gx_dflt}" == "off" ]]; then
        gx_out="${gx_name}${GX_OVERLAY_OFF_SUFFIX}"
    else
        gx_out="${gx_name}"
    fi
    if gx_src="$(gx_find_overlay_archive "${gx_name}")"; then
        # Plain cp for the same reason as the mandatory loop above: never stage a symlink.
        cp "${gx_src}" "${GX_PATCH_STAGE}/${gx_out}" || exit 1
        echo "      [${gx_dflt}] ${gx_out}"
        gx_staged=$((gx_staged + 1))
        gx_staged_optional+=("${gx_name}|$(gx_overlay_optional_label "${gx_line}")")
    else
        gx_missing_optional+=("${gx_name}")
    fi
done <<< "${GX_OPTIONAL_LINES}"

# The two manifests, shipped as data rather than compiled into anything so they cannot drift from
# the lists. optional.txt names only what was actually staged - offering a toggle for an archive
# that is not there would be a switch that silently does nothing - and required.txt is the bare
# filename set the launcher's readiness check reads. A .txt in here is inert to the engine, whose
# overlay scan matches "*.big".
#
# INSIDE THIS GUARD ON PURPOSE, as on macOS: this is the arm where every mandatory archive really
# was staged, so neither file can advertise a set the tarball does not have.
if [[ ${#gx_missing_required[@]} -eq 0 ]]; then
    : > "${GX_PATCH_STAGE}/optional.txt"
    for gx_o in ${gx_staged_optional[@]+"${gx_staged_optional[@]}"}; do
        printf '%s\n' "${gx_o}" >> "${GX_PATCH_STAGE}/optional.txt"
    done
    : > "${GX_PATCH_STAGE}/required.txt"
    gx_overlay_archives >> "${GX_PATCH_STAGE}/required.txt" || exit 1
    # SCOPE, decided (Codex launcher-maps r2 P1): the Flatpak (flatpak/*.yml, build-linux-flatpak.yml) carries
    # no GeneralsXPatch overlay at all today (grep -c GeneralsXPatch = 0 in all three), so it ships none of the
    # overlay archives either; the maps ride with the overlay and gain nothing there that dataID does not
    # already lose. A Flatpak player still gets the maps by lobby transfer.
    gx_overlay_stage_shipped_maps "${GX_PATCH_STAGE}" || exit 1
    echo "  + GeneralsXPatch/ (${gx_staged} archive(s) + required.txt + optional.txt)"
fi

if [[ ${#gx_missing_required[@]} -gt 0 || ${#gx_missing_optional[@]} -gt 0 ]]; then
    if [[ "${GX_BUNDLE_ALLOW_NO_OVERLAY:-0}" == "1" ]]; then
        # The same escape hatch, the same variable and the same stamp as bundle-macos-zh.sh, for a
        # deliberately incomplete packaging smoke test. It did not exist here before, and adding it
        # changes nothing unless it is set: unset, both arms below are as fatal as the block this
        # replaces. It is here because the optional set is now a hard requirement and !GXUI.big is
        # BUILT rather than downloaded, so a packaging box that could produce a tarball yesterday
        # can be short of it today through no fault of its own.
        #
        # EITHER ARM IS A SMOKE TEST, and OVERLAY-MISSING.txt says "not a release" in both. Read by
        # the BUILD.txt step below, which must not ship the packager's statement that it is one.
        GX_BUNDLE_NOT_A_RELEASE=1
        if [[ ${#gx_missing_required[@]} -gt 0 ]]; then
            # A PARTIAL MANDATORY SET IS NOT A HALFWAY HOUSE, it is a third dataID that matches
            # nobody and costs the player the download anyway.
            rm -rf "${GX_PATCH_STAGE}"
            echo "WARNING: mandatory overlay archives missing - THIS TARBALL CANNOT JOIN ANY GAME."
            for gx_m in "${gx_missing_required[@]}"; do echo "  missing: ${gx_m}"; done
            {
                echo "GX_BUNDLE_INCOMPLETE=required"
                echo "This tarball was built with GX_BUNDLE_ALLOW_NO_OVERLAY=1."
                echo "It carries NO community-patch archives, computes a dataID that matches no peer,"
                echo "and every multiplayer join will be refused with DATA_DIFFERS."
                echo "It is a packaging smoke test, not a release. Missing:"
                for gx_m in "${gx_missing_required[@]}"; do echo "  ${gx_m}"; done
                for gx_m in ${gx_missing_optional[@]+"${gx_missing_optional[@]}"}; do
                    echo "  ${gx_m} (optional)"
                done
            } > "${BUNDLE_DIR}/OVERLAY-MISSING.txt"
        else
            # Mandatory set complete, so this tarball DOES join the fleet - it is merely short of
            # cosmetic content. Say exactly that rather than crying that it cannot play.
            echo "WARNING: optional overlay archives missing - the tarball still joins games,"
            echo "  but ships without this content and cannot switch it on later:"
            for gx_m in "${gx_missing_optional[@]}"; do echo "  missing: ${gx_m}"; done
            {
                echo "GX_BUNDLE_INCOMPLETE=optional"
                echo "This tarball was built with GX_BUNDLE_ALLOW_NO_OVERLAY=1."
                echo "The mandatory overlay is complete, so it JOINS games and computes the fleet dataID."
                echo "It is short of optional content, which cannot be switched on later because the"
                echo "toggle is a rename and not a download. It is a packaging smoke test, not a"
                echo "release. Missing:"
                for gx_m in "${gx_missing_optional[@]}"; do echo "  ${gx_m}"; done
            } > "${BUNDLE_DIR}/OVERLAY-MISSING.txt"
        fi
        echo "  GX_BUNDLE_ALLOW_NO_OVERLAY=1 set; continuing for a packaging-only build."
        echo "  Stamped ${BUNDLE_DIR}/OVERLAY-MISSING.txt - this tarball is not a release."
    else
        # REFUSE rather than ship a tarball that is short of what the .app carries. Fatal for BOTH
        # lists, matching bundle-macos-zh.sh and package-windows-share.ps1: a missing mandatory
        # archive means the engine starts, the game runs, and every peer says no; a missing optional
        # one cannot be switched on later, because the toggle is a rename and not a download.
        echo "ERROR: community-patch overlay incomplete - refusing to ship an incomplete tarball." >&2
        for gx_m in ${gx_missing_required[@]+"${gx_missing_required[@]}"}; do
            echo "  missing (required, moves dataID): ${gx_m}" >&2
        done
        for gx_m in ${gx_missing_optional[@]+"${gx_missing_optional[@]}"}; do
            echo "  missing (optional, dataID-neutral): ${gx_m}" >&2
        done
        for gx_m in "${GX_OVERLAY_SRC_DIRS[@]}"; do
            echo "  looked in: ${gx_m}" >&2
        done
        if [[ ${#gx_missing_required[@]} -gt 0 ]]; then
            echo "  A player on this tarball installs a game that plays single-player perfectly and is" >&2
            echo "  refused by every peer with DATA_DIFFERS, which reads as a network fault." >&2
        fi
        echo "  Set GX_OVERLAY_SRC to a directory holding them, or set" >&2
        echo "  GX_BUNDLE_ALLOW_NO_OVERLAY=1 for a deliberately incomplete packaging test." >&2
        exit 1
    fi
fi

# GeneralsX @bugfix Claude 25/09/2026 THE LOOSE DATA EVERY OTHER PLATFORM SHIPS (Codex P1 on
# feat/next-batch-defaults). Data/INI/Multiplayer.ini (the sixteen colours) and Data/INI/Weapon.ini (the
# Toxin Tractor change, cut by scripts/tooling/make-weapon-ini.py from the CoreINI staged above) both go
# through the INI parser, so both are in iniCRC -> dataID: a tarball without them is refused by every
# Mac, iPad and Windows peer with DATA_DIFFERS. This tarball never carried the colour pair either.
# Served from <tarball>/gamedata through GENERALSX_DATA_OVERLAY (run.sh below), as the macOS .app
# does, never copied into the player's game folder. The overlay matches exact case, so the CSF goes in
# under the one spelling the Linux engine asks for (SDL3Main g_csfFile, data/<language>/generals.csf,
# language "english") - also the only spelling a case-insensitive packaging host can hold for both.
GX_MP_INI_SRC="${PROJECT_ROOT}/GeneralsZH/Data/INI/Multiplayer.ini"
GX_COLOR_CSF_SRC="${PROJECT_ROOT}/GeneralsZH/Data/English/Generals.csf"
GX_DATA_STAGE="${BUNDLE_DIR}/gamedata"
if [[ ! -f "${GX_MP_INI_SRC}" || ! -f "${GX_COLOR_CSF_SRC}" ]]; then
    echo "ERROR: ${GX_MP_INI_SRC} or ${GX_COLOR_CSF_SRC} is missing - a broken checkout." >&2
    exit 1
fi
mkdir -p "${GX_DATA_STAGE}/Data/INI" "${GX_DATA_STAGE}/data/english"
cp "${GX_MP_INI_SRC}" "${GX_DATA_STAGE}/Data/INI/Multiplayer.ini"
cp "${GX_COLOR_CSF_SRC}" "${GX_DATA_STAGE}/data/english/generals.csf"
# A case-insensitive packaging host (a Mac) folds data/ into Data/, and the Linux engine's exact-case
# overlay lookup then misses the CSF: lobby colour names read MISSING (dataID is unaffected, the CSF is
# not in iniCRC). Say so; package on the Linux host for a release.
if [[ " $(ls "${GX_DATA_STAGE}" | tr '\n' ' ') " != *" data "* ]]; then
    echo "WARNING: this volume is case-insensitive, so the tarball's CSF landed under Data/, not data/."
    echo "  Linux players on it get the retail colour names. Package on a case-sensitive volume."
fi
if python3 "${PROJECT_ROOT}/scripts/tooling/make-weapon-ini.py" --search "${GX_PATCH_STAGE}" \
        --game-dir "${GX_SRC_DIR}" ${GX_GAME_DATA:+--game-dir "${GX_GAME_DATA}"} --out "${GX_DATA_STAGE}/Data/INI/Weapon.ini"; then
    echo "  + gamedata/ (Multiplayer.ini, Generals.csf, $(python3 "${PROJECT_ROOT}/scripts/tooling/make-weapon-ini.py" --list | sed 's|^Data/INI/||' | tr '\n' ' '); moves dataID)"
elif [[ "${GX_BUNDLE_ALLOW_NO_OVERLAY:-0}" == "1" ]]; then
    # Stamped whatever the overlay block wrote, keeping only its list of missing archives (an
    # "optional" stamp's prose says the tarball joins games, which is no longer true).
    GX_BUNDLE_NOT_A_RELEASE=1
    echo "WARNING: Data/INI/Weapon.ini NOT staged - THIS TARBALL CANNOT JOIN ANY GAME."
    gx_stamp="${BUNDLE_DIR}/OVERLAY-MISSING.txt"
    gx_prev=""
    if [[ -f "${gx_stamp}" ]]; then
        gx_prev="$(sed -n '/^  /p' "${gx_stamp}")"
        if [[ -n "${gx_prev}" && "$(head -1 "${gx_stamp}")" == "GX_BUNDLE_INCOMPLETE=optional" ]]; then
            gx_prev="$(printf '%s\n' "${gx_prev}" | sed 's/$/ (optional)/')"
        fi
    fi
    {
        echo "GX_BUNDLE_INCOMPLETE=required"
        echo "This tarball was built with GX_BUNDLE_ALLOW_NO_OVERLAY=1 and carries no Data/INI/Weapon.ini"
        echo "(make-weapon-ini.py could not cut it from GeneralsXPatch), so it computes a dataID that"
        echo "matches no peer and every multiplayer join will be refused with DATA_DIFFERS."
        echo "It is a packaging smoke test, not a release. Missing:"
        echo "  Data/INI/Weapon.ini"
        [[ -z "${gx_prev}" ]] || printf '%s\n' "${gx_prev}"
    } > "${gx_stamp}"
else
    echo "ERROR: could not generate Data/INI/Weapon.ini from ${GX_PATCH_STAGE}." >&2
    exit 1
fi

# Run wrapper
echo "  + run.sh"
cat > "${BUNDLE_DIR}/run.sh" << 'WRAPPER'
#!/bin/bash
# GeneralsX @build BenderAI 03/03/2026 - Linux wrapper for bundled runtime
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Set LD_LIBRARY_PATH to find DXVK, SDL3, and other libs in same directory
export LD_LIBRARY_PATH="${SCRIPT_DIR}:${LD_LIBRARY_PATH:-}"

# GeneralsX @bugfix Claude 20/08/2026 Point the engine at the bundled community-patch overlay.
# Explicit rather than relying on <exe dir>/GeneralsXPatch: the default is correct for this layout
# but wrong for a deployed install, and one spelling that is always right beats two that differ.
if [[ -d "${SCRIPT_DIR}/GeneralsXPatch" ]]; then
    export GENERALSX_PATCH_PATH="${SCRIPT_DIR}/GeneralsXPatch"
fi
# GeneralsX @bugfix Claude 25/09/2026 The loose data (colour pair, Weapon.ini) this tarball carries: the
# engine reads a relative path here before the working directory (StdLocalFileSystem.cpp).
if [[ -d "${SCRIPT_DIR}/gamedata" ]]; then
    export GENERALSX_DATA_OVERLAY="${GENERALSX_DATA_OVERLAY:-${SCRIPT_DIR}/gamedata}"
fi

# Set DXVK environment
export DXVK_WSI_DRIVER="SDL3"
export DXVK_LOG_LEVEL="${DXVK_LOG_LEVEL:-info}"

# SagePatch (optional QoL: F11 screenshot, Scroll Lock cursor lock,
# Ctrl+PgUp/Dn brightness, Ctrl+1..5 window snap). Loaded via LD_PRELOAD only
# if libsage_patch.so is bundled. DXVK_HUD defaults to "fps" when active.
if [[ -f "${SCRIPT_DIR}/libsage_patch.so" && "${SAGE_PATCH_DISABLED:-0}" != "1" ]]; then
    if [[ -n "${LD_PRELOAD:-}" ]]; then
        export LD_PRELOAD="${SCRIPT_DIR}/libsage_patch.so:${LD_PRELOAD}"
    else
        export LD_PRELOAD="${SCRIPT_DIR}/libsage_patch.so"
    fi
    export DXVK_HUD="${DXVK_HUD:-fps}"
else
    export DXVK_HUD="${DXVK_HUD:-0}"
fi

# Auto-detect base Generals install path
if [[ -z "${CNC_GENERALS_INSTALLPATH:-}" && -d "${SCRIPT_DIR}/../Generals" ]]; then
    export CNC_GENERALS_INSTALLPATH="${SCRIPT_DIR}/../Generals/"
fi

# GeneralsX @bugfix Claude 20/08/2026 NAME THE ZERO HOUR DATA ROOT, do not let it fall out of cwd.
#
# This wrapper execs the engine without cd-ing, so the asset root was whatever directory the user
# happened to be in. dataID is computed over the mounted archives and is cwd-relative, so the same
# install launched from two different directories computes two different dataIDs and the second one
# is refused by every peer with DATA_DIFFERS - which reads as a network fault, not a wrong cwd.
# The macOS .app launcher has always exported this; the tarball never did.
#
# Sibling first, because that is the layout the tarball's own instructions describe, then the
# conventional user path the Linux deploy script uses. An operator-set value always wins.
if [[ -z "${CNC_GENERALS_ZH_PATH:-}" ]]; then
    for _zh in "${SCRIPT_DIR}/../GeneralsZH" "${SCRIPT_DIR}/../GeneralsMD" "${HOME}/GeneralsX/GeneralsZH"; do
        if [[ -d "${_zh}" ]]; then
            export CNC_GENERALS_ZH_PATH="${_zh}/"
            break
        fi
    done
    unset _zh
fi
if [[ -z "${CNC_GENERALS_ZH_PATH:-}" ]]; then
    echo "GeneralsX: no Zero Hour data found next to this folder or at ~/GeneralsX/GeneralsZH." >&2
    echo "           Extract this tarball beside your GeneralsZH/ folder, or set" >&2
    echo "           CNC_GENERALS_ZH_PATH to it. Starting anyway - the engine will look in \$PWD." >&2
fi

# GeneralsX @bugfix BenderAI 06/03/2026 - Exclude LLVMpipe Vulkan ICD (LLVM 20.x crash workaround)
# libvulkan_lvp.so (LLVMpipe) crashes during static initialization with LLVM 20.x.
# Filter hardware-only ICDs via VK_DRIVER_FILES to prevent loading the crashing library.
# User can override by setting VK_DRIVER_FILES or VK_ICD_FILENAMES before running.
if [[ -z "${VK_DRIVER_FILES:-}" && -z "${VK_ICD_FILENAMES:-}" ]]; then
    _hw_icds=""
    for _dir in /usr/share/vulkan/icd.d /etc/vulkan/icd.d; do
        [[ -d "$_dir" ]] || continue
        for _f in "$_dir"/*.json; do
            [[ -f "$_f" ]] || continue
            _base="$(basename "$_f")"
            case "${_base,,}" in
                *lvp* | *lavapipe* | *softpipe* | *llvmpipe*)
                    echo "INFO: Vulkan ICD filter: skipping software ICD '$_base'" ;;
                *)
                    _hw_icds="${_hw_icds:+${_hw_icds}:}$_f" ;;
            esac
        done
    done
    if [[ -n "$_hw_icds" ]]; then
        export VK_DRIVER_FILES="$_hw_icds"
        echo "INFO: Vulkan ICD filter: VK_DRIVER_FILES=$VK_DRIVER_FILES"
    else
        echo "WARNING: Vulkan ICD filter: no hardware ICDs found, LLVMpipe exclusion skipped"
        echo "WARNING: If startup crashes, set VK_DRIVER_FILES to your hardware Vulkan ICD JSON"
    fi
fi

# GeneralsX @bugfix 09/03/2026 - Work around openal-soft 1.25.1 movaps alignment crash
# alcOpenDevice() crashes with SIGSEGV in a 'movaps %xmm1, 0x26260(%rbx)' instruction
# inside openal-soft's device initializer. movaps requires 16-byte alignment; if the
# ALCdevice struct is not aligned correctly, it faults regardless of which backend is
# selected. Disabling CPU extensions forces openal-soft to use scalar code paths that
# do not have alignment requirements. The pipewire backend is also excluded because it
# has its own crash at device-open time on PipeWire 1.4.x.
# These env vars are read by openal-soft's static constructor at library load time,
# so they must be set here in the launcher before the binary starts.
# User can override by setting ALSOFT_DISABLE_CPU_EXTS or ALSOFT_DRIVERS explicitly.
# GeneralsX @build Claude 25/09/2026 The crashes above were recorded with the DISTRO's 1.25.1,
# which this tarball no longer loads (it ships 1.24.2 beside the binary). Kept as defaults because
# nobody has measured the bundled copy without them. Pulse first still follows default-output
# changes: the Pulse backend reports them, on PipeWire desktops too (via pipewire-pulse). The
# native PipeWire backend is built in, so ALSOFT_DRIVERS=pipewire selects it.
if [[ -z "${ALSOFT_DISABLE_CPU_EXTS:-}" ]]; then
    export ALSOFT_DISABLE_CPU_EXTS="all"
    echo "INFO: OpenAL: ALSOFT_DISABLE_CPU_EXTS=all (movaps alignment crash workaround)"
fi
if [[ -z "${ALSOFT_DRIVERS:-}" ]]; then
    export ALSOFT_DRIVERS="pulse,alsa,oss,jack,null,wave"
    echo "INFO: OpenAL: ALSOFT_DRIVERS=$ALSOFT_DRIVERS (pipewire excluded)"
fi

# GeneralsX @bugfix Claude 28/09/2026 READ THE dxvk.conf THIS TARBALL SHIPS. DXVK loads $DXVK_CONFIG_FILE,
# else $PWD/dxvk.conf (util/config/config.cpp getUserConfig), and this wrapper never cds: the launcher
# starts it with the cwd at the Zero Hour data root. So the conf beside this script was never read and
# Linux terrain went without the 16x anisotropy it carries. A caller's DXVK_CONFIG_FILE, or a dxvk.conf
# the player keeps in the directory DXVK would read, still wins.
if [[ -z "${DXVK_CONFIG_FILE:-}" && ! -f dxvk.conf && -f "${SCRIPT_DIR}/dxvk.conf" ]]; then
    export DXVK_CONFIG_FILE="${SCRIPT_DIR}/dxvk.conf"
fi

# GeneralsX @feature Claude 07/11/2026 THE SHIPPED MAPS ARE INSTALLED HERE TOO (owner 07/11: "ship the maps we are making by
# default"). This tarball carries GeneralsXPatch/ShippedMaps/<Name>/ (scripts/build/overlay-maps.txt), but only the
# launcher's PLAY copied them into the engine's <userdata>/Maps, and the Linux launcher neither installs nor finds a
# tarball engine - so a player who starts this script got them only by lobby transfer. The rule is gxio shippedmaps.rs's:
# a map is (re)installed when its <Name>.map is missing or its bytes differ; only the shipped folder's own regular files
# are written, the .map last, each through a fresh temp file and a rename; nothing is deleted; a symlink or a folder without
# <Name>.map is skipped. The folder is the one the ENGINE reads (GlobalData.cpp + GXUserDataDir.h: GX_USERDATA_DIR, else
# $XDG_DATA_HOME, else ~/.local/share, then GeneralsX/GeneralsZH). A failure is one line and never stops the game.
# scripts/build/test-runsh-shipped-maps.sh runs this text in a tarball-shaped tree.
# The temp is a FRESH file from mktemp (created exclusively, never an existing name), so a symlink planted at a predictable
# name can never be written through (Codex maps-default-r1 P2) and two launches sharing one user folder cannot write into
# each other's temp. A folder sitting at the target's name is refused: mv would move the file INTO it and call that done.
gx_put_shipped_file() {  # <file> <dest folder>: a fresh temp beside the target, then a rename over it
    local dst="${2}/${1##*/}" tmp
    if [[ ! -d "${dst}" ]] && tmp="$(mktemp "${dst}.gxtmp.XXXXXX" 2>/dev/null)"; then
        cp "${1}" "${tmp}" 2>/dev/null && chmod 644 "${tmp}" 2>/dev/null && mv -f "${tmp}" "${dst}" 2>/dev/null && return 0
        rm -f "${tmp}" 2>/dev/null
    fi
    echo "GeneralsX: shipped map ${2##*/} was not installed: cannot write ${dst}" >&2
    return 1
}
gx_install_shipped_maps() {
    local src="${GENERALSX_PATCH_PATH:-${SCRIPT_DIR}/GeneralsXPatch}/ShippedMaps" ud maps dir name f ok
    [[ -d "${src}" ]] || return 0   # an older payload: nothing to install, nothing to say
    # Present but unlistable: the glob below would match nothing and the maps would be silently skipped (Codex r4 P2).
    if [[ ! -r "${src}" ]]; then
        echo "GeneralsX: shipped maps: cannot list ${src}; none installed" >&2
        return 0
    fi
    if [[ -n "${GX_USERDATA_DIR+x}" ]]; then
        # The engine refuses an empty or relative value and does not start: nothing to install for.
        [[ "${GX_USERDATA_DIR}" == /* ]] || return 0
        ud="${GX_USERDATA_DIR}"
    elif [[ -n "${XDG_DATA_HOME+x}" ]]; then
        # Set but empty, the engine resolves a cwd-relative folder; do not guess which.
        [[ -n "${XDG_DATA_HOME}" ]] || return 0
        ud="${XDG_DATA_HOME}/GeneralsX/GeneralsZH"
    elif [[ -n "${HOME:-}" ]]; then
        ud="${HOME}/.local/share/GeneralsX/GeneralsZH"
    else
        return 0
    fi
    maps="${ud%/}/Maps"
    for dir in "${src}"/*; do
        [[ -d "${dir}" && ! -L "${dir}" ]] || continue
        name="${dir##*/}"
        case "${name}" in *:*|*\\*) continue ;; esac   # not one plain segment everywhere (the launcher's safe_name)
        [[ -f "${dir}/${name}.map" && ! -L "${dir}/${name}.map" ]] || continue
        cmp -s "${dir}/${name}.map" "${maps}/${name}/${name}.map" 2>/dev/null && continue
        # A folder that cannot be LISTED hides its sidecars from the glob below; installing its .map alone would match on
        # every later start and never fetch them (Codex maps-default-r3 P2). The launcher refuses this case too.
        if [[ ! -r "${dir}" ]]; then
            echo "GeneralsX: shipped map ${name} was not installed: cannot list ${dir}" >&2
            continue
        fi
        # The player's own Maps/<Name> may be a link to a folder elsewhere (a shared or retail map folder): never write
        # through it (Codex maps-default-r5 P2). A linked Maps folder as a whole is fine; only this one level is refused.
        if [[ -L "${maps}/${name}" ]]; then
            echo "GeneralsX: shipped map ${name} was not installed: ${maps}/${name} is a link, left alone" >&2
            continue
        fi
        if ! mkdir -p "${maps}/${name}" 2>/dev/null; then
            echo "GeneralsX: shipped map ${name} was not installed: cannot create ${maps}/${name}" >&2
            continue
        fi
        # The sidecars first and the .map LAST: an interrupted install leaves the old .map, so the next start sees the
        # difference and tries again.
        ok=1
        for f in "${dir}"/*; do
            [[ "${f}" != "${dir}/${name}.map" && -f "${f}" && ! -L "${f}" ]] || continue
            gx_put_shipped_file "${f}" "${maps}/${name}" || { ok=0; break; }
        done
        [[ "${ok}" == 1 ]] && gx_put_shipped_file "${dir}/${name}.map" "${maps}/${name}"
    done
    return 0
}
gx_install_shipped_maps

exec "${SCRIPT_DIR}/GeneralsXZH" "$@"
WRAPPER
chmod +x "${BUNDLE_DIR}/run.sh"

# GeneralsX @feature Claude 18/09/2026 BUILD.txt, SO THE LAUNCHER CAN TELL THIS PAYLOAD IS THE RELEASE.
#
# The launcher identifies a Linux engine from the gitinfo compiled into it, and that names a
# COMMIT - which any build at that commit also carries, local changes and other flags included. So
# it no longer calls such a match "Up to date"; a tarball install read "Built from the release
# commit". This is the packager's statement about the binary it packed, the same one
# package-windows-share.ps1 ships for Windows, and the launcher reads it only from the folder it
# runs the engine from (cmd_release.rs, engine_build_stamp).
#
# THE COMMIT IS READ OUT OF THE PACKED BINARY, NEVER FROM git HEAD: a bundler run at a later
# checkout would otherwise stamp a commit the engine was not built from. The rule is the Windows
# packager's: a 40-character lowercase sha ADJACENT to its own 7-12 character prefix - `strings`
# drops the zero padding a compiler puts between the two globals. No commit, no bundle: a payload
# that cannot name its build must not ship as one that can.
#
# EITHER ORDER. MSVC and clang emit the sha then its prefix; GCC emits them the other way round -
# measured 18/09/2026 on the 093922015 and 85f9a6650 Linux engines, `strings` prints the date, the
# short form, then the sha. Accepting only the MSVC order refused every Linux bundle.
# scripts/test/embedded-commit-test.sh runs this function out of this file.
#
# WINDOWS-GRADE TRUST: nothing re-checks the binary against the `exe sha256` line after install.
#
# awk READS TO THE END rather than exiting at the match: an early exit hands `strings` a SIGPIPE,
# and under `set -o pipefail` that failure would refuse every bundle.
gx_embedded_commit() {
    strings -a -n 7 "$1" | awk '
        function sha(s) { return length(s) == 40 && s ~ /^[0-9a-f]+$/ }
        function short_of(s, full) { return length(s) >= 7 && length(s) <= 12 && index(full, s) == 1 }
        !found && sha(prev) && short_of($0, prev) { print prev; found = 1 }
        !found && sha($0) && short_of(prev, $0) { print $0; found = 1 }
        { prev = $0 }
        END { exit found ? 0 : 1 }'
}
gx_write_build_txt() {
    echo "  + BUILD.txt"
    if ! command -v strings > /dev/null 2>&1; then
        echo "ERROR: 'strings' (binutils) is not installed, so the commit embedded in the engine cannot be read."
        exit 1
    fi
    GX_ENGINE_COMMIT="$(gx_embedded_commit "${BUNDLE_DIR}/GeneralsXZH")" || {
        echo "ERROR: no commit sha is embedded in ${BUNDLE_DIR}/GeneralsXZH - rebuild the engine from a checkout."
        exit 1
    }
    GX_ENGINE_SHA256="$(sha256sum "${BUNDLE_DIR}/GeneralsXZH" | cut -d' ' -f1)"
    cat > "${BUNDLE_DIR}/BUILD.txt" << EOF_BUILD
GeneralsX - Linux build
built from commit : ${GX_ENGINE_COMMIT}
exe sha256        : ${GX_ENGINE_SHA256}
packaged          : $(date '+%Y-%m-%d %H:%M:%S')

Every peer in a match must run a build of this exact source. The engine hashes its simulation
source into a sourceID and refuses any join whose ID differs.
EOF_BUILD
    echo "    built from commit ${GX_ENGINE_COMMIT}"
}
if [[ "${GX_BUNDLE_NOT_A_RELEASE:-0}" == "1" ]]; then
    # OVERLAY-MISSING.txt already says "not a release"; a BUILD.txt would tell the launcher it is.
    echo "  - BUILD.txt skipped: this tarball is a packaging smoke test (OVERLAY-MISSING.txt), not a release."
else
    gx_write_build_txt
fi

# GPL-3 + EA's additional terms (LICENSE.md: "Any propagation or conveyance of this program must include this
# copyright notice and these terms"): every package carries the repo's LICENSE.md.
echo "  + LICENSE.md"
cp "${PROJECT_ROOT}/LICENSE.md" "${BUNDLE_DIR}/LICENSE.md" || exit 1
# Third-party licence notices (MIT/BSD/zlib/Apache/LGPL/GPL components we bundle or link): generated by
# scripts/legal/third_party_notices.py, CI-style check: `third_party_notices.py check` and `check-packagers`.
# Freshness gate: refuse to package a stale or incomplete notices file (Cargo.lock / licence data moved without regenerating).
python3 -I "${PROJECT_ROOT}/scripts/legal/third_party_notices.py" check || exit 1
echo "  + THIRD_PARTY_NOTICES.txt"
cp "${PROJECT_ROOT}/THIRD_PARTY_NOTICES.txt" "${BUNDLE_DIR}/THIRD_PARTY_NOTICES.txt" || exit 1

# Payload gate: every library in what was just built must be owned by a component of THIRD_PARTY_NOTICES.txt
# (a new transitive dylib/so otherwise ships with no notice). Fix: add it to resources/legal/third-party and regenerate.
python3 -I "${PROJECT_ROOT}/scripts/legal/third_party_notices.py" check-path --platform linux "${BUNDLE_DIR}" || exit 1

# Create tarball
echo ""
echo "Creating ${OUTPUT_TARBALL}..."
rm -f "${OUTPUT_TARBALL}"
(cd "${STAGE_DIR}" && tar -czf "${OUTPUT_TARBALL}" GeneralsXZH-linux/)

echo ""
echo "Bundle complete: ${OUTPUT_TARBALL}"
echo "Contents:"
tar -tzf "${OUTPUT_TARBALL}" | head -30
echo ""
echo "To use: extract alongside your game data directory (GeneralsZH/)"
echo "  (legacy fallback also supported: GeneralsMD/)"
echo "  tar -xzf GeneralsXZH-linux-x86_64.tar.gz"
echo "  ./GeneralsXZH-linux/run.sh -win"
