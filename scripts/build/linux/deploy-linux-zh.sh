#!/bin/bash
# GeneralsX @build felipebraz 16/02/2026 Deploy Linux build to runtime directory
# GeneralsX @bugfix BenderAI 19/02/2026 Add non-empty check to guard against stale CMake placeholder files

set -e

# Directories
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
# GeneralsX @bugfix BenderAI 01/04/2026 Align deploy runtime selection with launcher logic by preferring the directory that contains .big assets.
PREFERRED_RUNTIME_DIR="${HOME}/GeneralsX/GeneralsZH"
LEGACY_RUNTIME_DIR="${HOME}/GeneralsX/GeneralsMD"
RUNTIME_DIR="${PREFERRED_RUNTIME_DIR}"

if [[ -d "${PREFERRED_RUNTIME_DIR}" && -n "$(compgen -G "${PREFERRED_RUNTIME_DIR}/*.big" 2>/dev/null)" ]]; then
    RUNTIME_DIR="${PREFERRED_RUNTIME_DIR}"
    echo "INFO: Detected Zero Hour assets in ${PREFERRED_RUNTIME_DIR}; deploying there"
elif [[ -d "${LEGACY_RUNTIME_DIR}" && -n "$(compgen -G "${LEGACY_RUNTIME_DIR}/*.big" 2>/dev/null)" ]]; then
    RUNTIME_DIR="${LEGACY_RUNTIME_DIR}"
    echo "INFO: Detected Zero Hour assets in legacy runtime ${LEGACY_RUNTIME_DIR}; deploying there"
elif [[ -d "${PREFERRED_RUNTIME_DIR}" ]]; then
    RUNTIME_DIR="${PREFERRED_RUNTIME_DIR}"
elif [[ -d "${LEGACY_RUNTIME_DIR}" ]]; then
    RUNTIME_DIR="${LEGACY_RUNTIME_DIR}"
    echo "INFO: No .big assets found; using existing legacy runtime ${LEGACY_RUNTIME_DIR}"
fi

# GeneralsX @bugfix Claude 12/08/2026 GX_RUNTIME_DIR overrides the auto-detection above.
#
# THE PEER THE HARNESSES RUN IS NOT THE ONE THIS SCRIPT FINDS. Every cross-platform script
# defaults to VPS_GAME=/root/gamedata (probe-headless-crc-ab.sh:33, xplat-4platform-play.sh:129
# and four more), while the detection above prefers ${HOME}/GeneralsX/GeneralsZH. On a test
# host where that directory exists and holds exactly one .big, the first branch takes it and the
# deploy reports success into a directory nothing ever launches - and the run silently measures
# the peer that was already there. That has cost this project a full round of "the fix did not
# land".
if [[ -n "${GX_RUNTIME_DIR:-}" ]]; then
    RUNTIME_DIR="${GX_RUNTIME_DIR}"
    echo "INFO: GX_RUNTIME_DIR override; deploying to ${RUNTIME_DIR}"
fi

# Say the archive count out loud. A runtime holding one .big is a decoy, not an install, and
# the number is the cheapest way to see that before the copy rather than after the test.
#
# THE WHOLE PATTERN IS QUOTED, and that is not style. Written as compgen -G "${DIR}"/*.big the
# shell expands the glob ITSELF before compgen runs, so compgen receives 26 arguments and honours
# only the first - and prints 1. Measured: this line reported "1 .big archive(s)" for the very
# 26-archive install it exists to distinguish from a one-archive decoy, i.e. it failed in exactly
# the direction that makes a healthy runtime look like the broken one.
echo "INFO: ${RUNTIME_DIR} currently holds $(compgen -G "${RUNTIME_DIR}/*.big" 2>/dev/null | wc -l | tr -d ' ') .big archive(s)"
# Note: CMakeLists.txt uses OUTPUT_NAME GeneralsXZH on Linux (see GeneralsMD/Code/Main/CMakeLists.txt)
BINARY_SRC="${BUILD_DIR}/GeneralsMD/GeneralsXZH"

echo "Deploying GeneralsXZH (Linux) to ${RUNTIME_DIR}"

# Check if binary exists and is non-zero (CMake creates a 0-byte placeholder before the link step)
if [[ ! -f "${BINARY_SRC}" ]]; then
    echo "ERROR: Binary not found at ${BINARY_SRC}"
    echo "Build first: ./scripts/build/linux/docker-build-linux-zh.sh linux64-deploy"
    exit 1
fi
if [[ ! -s "${BINARY_SRC}" ]]; then
    echo "ERROR: Binary at ${BINARY_SRC} is empty (0 bytes) - build may have failed"
    echo "Check build logs: ./scripts/build/linux/docker-build-linux-zh.sh linux64-deploy"
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

# Create runtime directory if needed
mkdir -p "${RUNTIME_DIR}"

# Copy executable (deploy as GeneralsXZH for run script compatibility)
echo "  Copying GeneralsXZH..."
cp -v "${BINARY_SRC}" "${RUNTIME_DIR}/GeneralsXZH"
chmod +x "${RUNTIME_DIR}/GeneralsXZH"

# Copy DXVK libraries
echo "  Copying DXVK libraries..."
cp -v "${DXVK_LIB_DIR}"/libdxvk_d3d8.so* "${RUNTIME_DIR}/"
cp -v "${DXVK_LIB_DIR}"/libdxvk_d3d9.so* "${RUNTIME_DIR}/" 2>/dev/null || true

# Copy SDL3 and SDL3_image libraries (for cursor loading and window management)
echo "  Copying SDL3 libraries..."
cp -v "${SDL3_LIB_DIR}"/libSDL3.so* "${RUNTIME_DIR}/"
cp -v "${SDL3_IMAGE_LIB_DIR}"/libSDL3_image.so* "${RUNTIME_DIR}/"

# Copy GameSpy library (for online multiplayer)
echo "  Copying GameSpy library..."
cp -v "${GAMESPY_LIB}" "${RUNTIME_DIR}/"

# GeneralsX @build Claude 25/09/2026 The openal-soft this engine was built against. Never copied
# before, so the runtime loaded the distro's libopenal.so.1 - see scripts/build/linux/lib-openal.sh.
# run.sh's LD_LIBRARY_PATH finds it here first, and so does the $ORIGIN runpath set below when
# patchelf is installed.
#
# A WARNING HERE, NOT A REFUSAL, unlike the tarball: this is a runtime on the build host, often a
# headless harness peer, and a library that fails the check (no Pulse/PipeWire/ALSA backend) would
# be WORSE than the distro's copy it replaces. So nothing is copied, a copy an earlier deploy left
# here is removed (gx_openal_stage does that on any refusal), and the deploy carries on.
echo "  Copying OpenAL library..."
# shellcheck source=scripts/build/linux/lib-openal.sh
source "${SCRIPT_DIR}/lib-openal.sh"
if ! gx_openal_stage "${BUILD_DIR}" "${RUNTIME_DIR}"; then
    echo "WARNING: libopenal.so.1 not deployed (see above) - this runtime loads the system's copy, if any."
fi

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

        cp -a "${dep}" "${RUNTIME_DIR}/" 2>/dev/null || true
        if [[ -L "${dep}" ]]; then
            local resolved
            resolved="$(readlink -f "${dep}")"
            cp -a "${resolved}" "${RUNTIME_DIR}/" 2>/dev/null || true
        fi
    done < <(ldd "${root}" | awk '{for (i = 1; i <= NF; ++i) { if ($i ~ /^\//) { print $i; break } }}' | sort -u)
}

# GeneralsX @build GitHubCopilot 17/05/2026 Deploy FFmpeg runtime libs transitively so runtime does not depend on host SONAME layout.
echo "  Copying FFmpeg runtime libraries..."
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
    cp -a "${ffmpeg_root}" "${RUNTIME_DIR}/" 2>/dev/null || true
    copy_ldd_deps "${ffmpeg_root}"
done

if ! compgen -G "${RUNTIME_DIR}/libavcodec.so*" > /dev/null; then
    echo "ERROR: Missing required runtime library: libavcodec.so*"
    echo "Install FFmpeg runtime/dev packages (e.g. libavcodec-dev) and rebuild/deploy"
    exit 1
fi

# Set RPATH so executable finds libraries in same directory
echo "  Setting RPATH to \$ORIGIN..."
patchelf --set-rpath '$ORIGIN' "${RUNTIME_DIR}/GeneralsXZH" 2>/dev/null || {
    echo "WARNING: patchelf not found. Install with: sudo apt install patchelf"
    echo "    Libraries will need LD_LIBRARY_PATH or manual RPATH setting"
}

# SagePatch (optional, gated by RTS_BUILD_OPTION_SAGE_PATCH at configure time).
# When the .so exists, deploy it. The engine auto-creates SagePatch.ini with
# defaults in the user data directory on first run.
SAGE_PATCH_LIB="${BUILD_DIR}/Patches/SagePatch/libsage_patch.so"
if [[ -f "${SAGE_PATCH_LIB}" ]]; then
    echo "  Deploying SagePatch (libsage_patch.so)..."
    cp -v "${SAGE_PATCH_LIB}" "${RUNTIME_DIR}/"
fi

# GeneralsX @build BenderAI 08/06/2026 Deploy ExtrasMenu.wnd so the engine can
# load it via TheFileSystem (local files override BIG archives).
EXTRAS_WND_SRC="${PROJECT_ROOT}/GeneralsZH/Data/Window/Menus/ExtrasMenu.wnd"
if [[ -f "${EXTRAS_WND_SRC}" ]]; then
    mkdir -p "${RUNTIME_DIR}/Window/Menus"
    cp -v "${EXTRAS_WND_SRC}" "${RUNTIME_DIR}/Window/Menus/ExtrasMenu.wnd"
fi

# GeneralsX @bugfix Claude 12/08/2026 Deploy the loose Multiplayer.ini (twelve player colours)
# and the string table that names them. Mirrors deploy-macos-zh.sh and the iOS bundler; Windows
# got the same thing in package-windows-share.ps1.
#
# THIS BEING ABSENT IS WHAT SPLIT THE FLEET. Multiplayer.ini goes through the INI parser, so it
# is CRC'd into iniCRC -> dataID, and dataID is a refusal field: a Linux peer without it reports
# a different dataID and every join is refused with DATA_DIFFERS. sourceID stays identical on
# both sides, so it reads as "stale build" and redeploying engines never fixes it.
#
# Refuse the deploy rather than ship half of it. The .csf is NOT in dataID (SimulationId.cpp's
# loose allow-list covers Data/Scripts/*.scb and CommandButton/CommandSet only), so a runtime
# that got the INI but not the CSF still joins everyone and merely renders MISSING:'Color:Turquoise'
# in the lobby - which reads as a font or localisation fault, not as a half-finished deploy.
#
# The paths are not parallel to ExtrasMenu.wnd above and that is not a typo: .wnd files live at
# Window/... while INI and CSF live under Data/..., one level deeper.
MP_INI_SRC="${PROJECT_ROOT}/GeneralsZH/Data/INI/Multiplayer.ini"
COLOR_CSF_SRC="${PROJECT_ROOT}/GeneralsZH/Data/English/Generals.csf"
if [[ -f "${MP_INI_SRC}" ]]; then
    if [[ ! -f "${COLOR_CSF_SRC}" ]]; then
        echo "ERROR: ${MP_INI_SRC} exists but ${COLOR_CSF_SRC} does not."
        echo "       The colour blocks and the labels that name them ship together."
        exit 1
    fi
    echo "  Deploying loose player-colour data..."
    mkdir -p "${RUNTIME_DIR}/Data/INI" "${RUNTIME_DIR}/Data/English"
    # GeneralsX @bugfix Claude 24/08/2026 SKIP A COPY ONTO ITSELF, or the script exits 1 having
    # done every real thing correctly. On a build host where RUNTIME_DIR lives INSIDE
    # PROJECT_ROOT - ~/GeneralsX and ~/GeneralsX/GeneralsZH, say -
    # source and destination are the same inode, cp refuses with "are the same file", and
    # `set -e` kills the script on its very last step. Every caller that gated on this exit code
    # then read a fully successful deploy as a failure. `-ef` compares device+inode, so it is
    # true for a hard link or a symlinked path too, which a string compare would miss.
    for _pair in "${MP_INI_SRC}|${RUNTIME_DIR}/Data/INI/Multiplayer.ini" \
                 "${COLOR_CSF_SRC}|${RUNTIME_DIR}/Data/English/Generals.csf"; do
        _src="${_pair%%|*}"
        _dst="${_pair##*|}"
        if [[ -e "${_dst}" ]] && [[ "${_src}" -ef "${_dst}" ]]; then
            echo "    already in place (same file): ${_dst}"
        else
            cp -v "${_src}" "${_dst}"
        fi
    done
fi

# GeneralsX @feature Claude 24/09/2026 Data/INI/Weapon.ini (the Toxin Tractor balance change), cut
# from this peer's own CoreINI archive by scripts/tooling/make-weapon-ini.py - never committed, it is
# the Community Patch's file. It is in iniCRC, so it moves dataID exactly like Multiplayer.ini. A peer
# with no CoreINI archive anywhere joins nobody anyway (reported at the end), so that alone warns; an
# archive the recipe refuses (another Community Patch release) is fatal. Mirrors deploy-macos-zh.sh.
# 25/09/2026 (Codex P2): make-weapon-ini.py --root searches where this peer's engine mounts the
# archive, $GENERALSX_PATCH_PATH and $CNC_GENERALS_ZH_PATH included, so a custom overlay or asset root
# is not skipped (run.sh cds here, so the loose file in this folder is the one the engine reads).
# It follows the ENGINE's order, and run.sh below changes that order: it exports the sibling
# GeneralsXPatch as GENERALSX_PATCH_PATH when that folder exists. So export it the same way here, in a
# subshell, or a runtime folder with both overlays gets its Weapon.ini from the one the peer never mounts.
echo "  Generating Data/INI/Weapon.ini..."
(
    GX_SIBLING_PATCH="$(cd "${RUNTIME_DIR}/.." 2>/dev/null && pwd)/GeneralsXPatch"
    if [[ -d "${GX_SIBLING_PATCH}" ]]; then
        export GENERALSX_PATCH_PATH="${GENERALSX_PATCH_PATH:-${GX_SIBLING_PATCH}}"
    fi
    python3 "${PROJECT_ROOT}/scripts/tooling/make-weapon-ini.py" --root "${RUNTIME_DIR}" --if-archive
) || exit 1

# Copy run wrapper script
echo "  Copying run.sh wrapper..."
cat > "${RUNTIME_DIR}/run.sh" << 'EOF'
#!/bin/bash
# GeneralsX @build felipebraz 16/02/2026 - Wrapper script for runtime directory
# Sets LD_LIBRARY_PATH to find DXVK libraries

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# GeneralsX @feature Claude 12/08/2026 Community-patch overlay.
#
# The archives every peer must load live OUTSIDE this folder, in a GeneralsX-owned sibling, and
# the engine is pointed at them by environment. The engine's own <exe dir>/GeneralsXPatch
# default cannot be used here: on Linux the executable lives IN the game folder, so that path is
# inside the asset root, and the primary archive scan recurses - it mounts every one of them a
# second time under a spelling loadPatchOverlay's already-mounted check does not recognise.
# See scripts/build/lib-overlay.sh.
#
# Silent when the directory is not there, deliberately. A peer that has not migrated still has
# these archives in this folder and is working perfectly - identical dataID, measured - so a
# warning here would fire on a healthy install every launch, which is how a warning stops being
# read. deploy-linux-zh.sh runs the check that can tell the two apart. The /root/gamedata test
# peer is one of the un-migrated ones, on purpose.
GX_PATCH_DIR="$(cd "${SCRIPT_DIR}/.." 2>/dev/null && pwd)/GeneralsXPatch"
if [[ -d "${GX_PATCH_DIR}" ]]; then
    export GENERALSX_PATCH_PATH="${GENERALSX_PATCH_PATH:-${GX_PATCH_DIR}}"
fi

# Set LD_LIBRARY_PATH to current directory (where DXVK libs are)
export LD_LIBRARY_PATH="${SCRIPT_DIR}:${LD_LIBRARY_PATH:-}"

# Set DXVK environment
export DXVK_WSI_DRIVER="SDL3"
export DXVK_LOG_LEVEL="${DXVK_LOG_LEVEL:-info}"
# DXVK HUD: SagePatch builds default to "fps" so casual users see frame rate
# without extra config. Set DXVK_HUD=0 explicitly to disable, or anything else
# (e.g. fps,memory,version) to customize. See DXVK docs for full list.
if [[ -f "${SCRIPT_DIR}/libsage_patch.so" && "${SAGE_PATCH_DISABLED:-0}" != "1" ]]; then
    export DXVK_HUD="${DXVK_HUD:-fps}"
else
    export DXVK_HUD="${DXVK_HUD:-0}"
fi

# SagePatch (optional QoL features). Loaded via LD_PRELOAD so it can interpose
# SDL3 functions for hot-keys (F11 screenshot, Scroll Lock cursor lock,
# Ctrl+PageUp/PageDown brightness, Ctrl+1..5 window snap).
if [[ -f "${SCRIPT_DIR}/libsage_patch.so" && "${SAGE_PATCH_DISABLED:-0}" != "1" ]]; then
    if [[ -n "${LD_PRELOAD:-}" ]]; then
        export LD_PRELOAD="${SCRIPT_DIR}/libsage_patch.so:${LD_PRELOAD}"
    else
        export LD_PRELOAD="${SCRIPT_DIR}/libsage_patch.so"
    fi
fi

# GeneralsX @feature felipebraz 25/02/2026 Auto-detect base Generals install path
# Set CNC_GENERALS_INSTALLPATH if not already set and ../Generals/ exists
if [[ -z "${CNC_GENERALS_INSTALLPATH:-}" && -d "${SCRIPT_DIR}/../Generals" ]]; then
    export CNC_GENERALS_INSTALLPATH="${SCRIPT_DIR}/../Generals/"
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
# GeneralsX @build Claude 25/09/2026 The crashes above were recorded with the DISTRO's 1.25.1;
# this runtime now carries the 1.24.2 it was built against. Kept as defaults because nobody has
# measured that copy without them. See the same note in bundle-linux-zh.sh.
if [[ -z "${ALSOFT_DISABLE_CPU_EXTS:-}" ]]; then
    export ALSOFT_DISABLE_CPU_EXTS="all"
    echo "INFO: OpenAL: ALSOFT_DISABLE_CPU_EXTS=all (movaps alignment crash workaround)"
fi
if [[ -z "${ALSOFT_DRIVERS:-}" ]]; then
    export ALSOFT_DRIVERS="pulse,alsa,oss,jack,null,wave"
    echo "INFO: OpenAL: ALSOFT_DRIVERS=$ALSOFT_DRIVERS (pipewire excluded)"
fi

# The engine resolves Local FS lookups (Data/INI/Default/... overrides, etc.)
# relative to the binary's cwd. Without this cd, anything launched via absolute
# path misses every loose INI / asset and only sees BIG-archived data.
cd "${SCRIPT_DIR}"

# Run game with all arguments
exec "./GeneralsXZH" "$@"
EOF
chmod +x "${RUNTIME_DIR}/run.sh"

# GeneralsX @feature Claude 12/08/2026 Report where this peer's shared archives are. A report and
# not a refusal - see the same block in deploy-macos-zh.sh for why the shipped-artifact builders
# are fatal and this is not. Note the Linux test peer at /root/gamedata is deliberately left
# un-migrated: it has no third-party Zero Hour install to protect, which is the only thing the
# overlay buys, and INFOLDER produces the identical dataID.
# shellcheck source=scripts/build/lib-overlay.sh
source "${SCRIPT_DIR}/../lib-overlay.sh"

# GeneralsX @feature Claude 17/08/2026 Build this project's own overlay archives onto this peer.
#
# THE ONLY OPTIONAL ARCHIVE LINUX CAN ACTUALLY RECEIVE IS ONE IT CAN BUILD, and that is the whole
# shape of the problem this closes. The community archives are third-party downloads: nothing in
# this repository can put them on a Linux box, which is why the block below only ever REPORTED on
# them. !GXUI.big is different - it is derived from WindowZH.big, which this runtime already has,
# so it can be produced here rather than shipped here. That is what finally gives a .wnd change a
# route to a Linux player.
#
# WHERE IT LANDS, and the choice is not arbitrary. Prefer the sibling overlay directory when this
# peer has migrated, because run.sh exports GENERALSX_PATCH_PATH only when that directory exists;
# otherwise put it in the runtime folder beside the archives this peer already mounts in place.
# Exactly one copy either way. Two copies would mount twice - same bytes and the right dataID, so
# nothing breaks, but it is the arrangement gx_overlay_where calls BOTH and it is pure waste.
#
# NOT FATAL, matching the rest of this block. deploy-linux-zh.sh reports and continues by design
# (the /root/gamedata test peer is deliberately un-migrated); a peer without this archive still
# joins everyone, because assetID is a warning field and not one SimIdCompare refuses over.
#
# WRITTEN FOR `set -e`, which is on from line 5, and the if/else below is deliberate rather than
# verbose. Measured both forms rather than trusting the folklore, because the folklore is only
# half right: a bare `[[ test ]] && assignment` at statement level does NOT exit on a false test -
# errexit exempts every element of an AND-list but the last - yet the SAME line as the final
# command of a function or script does exit, with status 1. So the bare form is safe exactly where
# it sits today and unsafe the moment anyone moves it into a helper. if/else is safe in both
# places, and it also sets GX_OVERLAY_OTHER once per branch instead of computing it and then
# conditionally overwriting it.
GX_OVERLAY_SIBLING="$(gx_overlay_dir "${RUNTIME_DIR}")" || GX_OVERLAY_SIBLING=""
if [[ -n "${GX_OVERLAY_SIBLING}" && -d "${GX_OVERLAY_SIBLING}" ]]; then
    GX_OVERLAY_TARGET="${GX_OVERLAY_SIBLING}"
    GX_OVERLAY_OTHER="${RUNTIME_DIR}"
else
    GX_OVERLAY_TARGET="${RUNTIME_DIR}"
    GX_OVERLAY_OTHER="${GX_OVERLAY_SIBLING}"
fi
if gx_overlay_generate "${GX_OVERLAY_TARGET}" "${RUNTIME_DIR}"; then
    # A copy in the place we did NOT choose is stale and is now mounted alongside the fresh one.
    # Say so by name - the engine will mount both and the older filename wins nothing, but a
    # reader hunting a layout that "did not update" needs to be told the old bytes are still there.
    GX_OPTIONAL_NAMES="$(gx_overlay_optional_names)" || GX_OPTIONAL_NAMES=""
    [[ -n "${GX_OVERLAY_OTHER}" ]] || GX_OPTIONAL_NAMES=""
    while IFS= read -r _opt; do
        [[ -n "${_opt}" ]] || continue
        if [[ -s "${GX_OVERLAY_OTHER}/${_opt}" && -s "${GX_OVERLAY_TARGET}/${_opt}" ]]; then
            echo "WARNING: ${_opt} is in BOTH ${GX_OVERLAY_TARGET} and ${GX_OVERLAY_OTHER}."
            echo "  Both are mounted. Delete the one in ${GX_OVERLAY_OTHER} - it is the stale copy."
        fi
    done <<< "${GX_OPTIONAL_NAMES}"
else
    echo "WARNING: could not build the generated overlay archives into ${GX_OVERLAY_TARGET}."
    echo "  Deploy continues and this peer still joins everyone - assetID is a warning field, not"
    echo "  a refusal one - but it will not carry this build's UI layout changes."
fi

OVERLAY_WHERE="$(gx_overlay_where "${RUNTIME_DIR}")" && OVERLAY_OK=1 || OVERLAY_OK=0
if [[ "${OVERLAY_OK}" != "1" ]]; then
    echo ""
    echo "WARNING: this peer is missing shared game archives and cannot join anybody."
    printf '%s\n' "${OVERLAY_WHERE}" | awk '$1=="ABSENT" { print "  missing: " $2 }'
    echo "  Deploy continues - the binary is fine - but the engine computes a different dataID"
    echo "  and every join is refused with DATA_DIFFERS, which reads as a network fault."
    echo "  Fix it once with:"
    echo "    ${PROJECT_ROOT}/scripts/build/migrate-patch-overlay.sh ${RUNTIME_DIR}"
fi

echo ""
echo "Deploy complete"
printf '   Overlay:    %s\n' "$(printf '%s\n' "${OVERLAY_WHERE}" | awk '{c[$1]++} END {for (k in c) printf "%s=%d ", k, c[k]}')"
echo "   Executable: ${RUNTIME_DIR}/GeneralsXZH"
echo "   SDL3 libs:  ${RUNTIME_DIR}/libSDL3*.so* + ${RUNTIME_DIR}/libSDL3_image*.so*"
echo "   GameSpy:    ${RUNTIME_DIR}/libgamespy.so"
echo "   OpenAL:     ${RUNTIME_DIR}/libopenal.so.1 (openal-soft, from this build)"
echo "   FFmpeg:     ${RUNTIME_DIR}/libavcodec*.so* + libavformat*.so* + libavutil*.so*"
# GeneralsX @tweak BenderAI 28/04/2026 Mirror macOS deploy summary labels/order for cross-platform script UX consistency.
echo "   DXVK d3d9:  ${RUNTIME_DIR}/libdxvk_d3d9.so*"
echo "   DXVK d3d8:  ${RUNTIME_DIR}/libdxvk_d3d8.so*"
echo "   Vulkan:     system loader (not deployed by this script)"
echo "   MoltenVK:   n/a on Linux"
echo "   VK ICD:     system JSONs (/usr/share/vulkan/icd.d or /etc/vulkan/icd.d)"
echo "   DXVK conf:  not deployed (DXVK defaults/environment)"
echo "   Wrapper:    ${RUNTIME_DIR}/run.sh"
echo ""
# GeneralsX @tweak BenderAI 28/04/2026 Keep Linux deploy run instructions aligned across Generals and Zero Hour scripts.
echo "Run with:"
echo "  ${PROJECT_ROOT}/scripts/build/linux/run-linux-zh.sh -win"
echo "  or: cd ~/GeneralsX/GeneralsZH && ./run.sh -win"
