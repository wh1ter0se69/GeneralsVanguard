#!/bin/bash
# GeneralsX @build felipebraz 16/02/2026 Launch script for Linux with DXVK

set -e

LOG_FILE="logs/run_zh.log"

# Runtime directory
# GeneralsX: Prefer GeneralsZH for user-facing Zero Hour runtime path with GeneralsMD fallback.
PREFERRED_GAME_DIR="${HOME}/GeneralsX/GeneralsZH"
LEGACY_GAME_DIR="${HOME}/GeneralsX/GeneralsMD"
GAME_DIR="${PREFERRED_GAME_DIR}"

if [[ -d "${PREFERRED_GAME_DIR}" && -n "$(compgen -G "${PREFERRED_GAME_DIR}/*.big" 2>/dev/null)" ]]; then
    GAME_DIR="${PREFERRED_GAME_DIR}"
elif [[ -d "${LEGACY_GAME_DIR}" && -n "$(compgen -G "${LEGACY_GAME_DIR}/*.big" 2>/dev/null)" ]]; then
    GAME_DIR="${LEGACY_GAME_DIR}"
    echo "INFO: Zero Hour assets detected in legacy path; using ${LEGACY_GAME_DIR}"
elif [[ -d "${PREFERRED_GAME_DIR}" ]]; then
    GAME_DIR="${PREFERRED_GAME_DIR}"
elif [[ -d "${LEGACY_GAME_DIR}" ]]; then
    GAME_DIR="${LEGACY_GAME_DIR}"
    echo "INFO: Using legacy runtime path ${LEGACY_GAME_DIR}"
fi

GAME_BINARY="${GAME_DIR}/GeneralsXZH"

# Check if binary exists
if [[ ! -f "${GAME_BINARY}" ]]; then
    echo "❌ ERROR: Game binary not found at ${GAME_BINARY}"
    echo "Run deploy first: ./scripts/build/linux/deploy-linux-zh.sh"
    exit 1
fi

# Check if DXVK library is deployed
if [[ ! -f "${GAME_DIR}/libdxvk_d3d8.so" ]]; then
    echo "❌ ERROR: DXVK library not found in ${GAME_DIR}"
    echo "Run deploy first: ./scripts/build/linux/deploy-linux-zh.sh"
    exit 1
fi

# GeneralsX: Community-patch overlay - see scripts/build/lib-overlay.sh.
#
# Pointed at by environment rather than left to the engine's <exe dir>/GeneralsXPatch default:
# on Linux the executable lives IN the game folder, so that default path sits inside the asset
# root and the primary archive scan - which recurses - mounts every archive a second time.
# This script has the repo, so it runs the check that can tell "not migrated" (fine, identical
# dataID) from "not there at all" (refused by every peer) rather than warning on both.
GX_LIB="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/lib-overlay.sh"
# shellcheck source=scripts/build/lib-overlay.sh
source "${GX_LIB}"
GX_PATCH_DIR="$(cd "${GAME_DIR}/.." 2>/dev/null && pwd)/GeneralsXPatch"
if [[ -d "${GX_PATCH_DIR}" ]]; then
    export GENERALSX_PATCH_PATH="${GENERALSX_PATCH_PATH:-${GX_PATCH_DIR}}"
fi
if ! GX_WHERE="$(gx_overlay_where "${GAME_DIR}")"; then
    echo "WARNING: this peer is missing shared game archives and cannot join anybody:"
    printf '%s\n' "${GX_WHERE}" | awk '$1=="ABSENT" { print "  missing: " $2 }'
    echo "  Fix once with: scripts/build/migrate-patch-overlay.sh ${GAME_DIR}"
fi

# Set LD_LIBRARY_PATH so dlopen() can find DXVK dependencies
export LD_LIBRARY_PATH="${GAME_DIR}:${LD_LIBRARY_PATH:-}"

# Set DXVK environment
export DXVK_WSI_DRIVER="SDL3"
export DXVK_LOG_LEVEL="${DXVK_LOG_LEVEL:-info}"  # Override with 'debug' if needed
export DXVK_HUD="${DXVK_HUD:-devinfo,fps}"       # Show GPU info + FPS overlay; override with DXVK_HUD=0 to disable

# GeneralsX: Exclude LLVMpipe Vulkan ICD (LLVM 20.x crash workaround)
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
if [[ -z "${ALSOFT_DISABLE_CPU_EXTS:-}" ]]; then
    export ALSOFT_DISABLE_CPU_EXTS="all"
    echo "INFO: OpenAL: ALSOFT_DISABLE_CPU_EXTS=all (movaps alignment crash workaround)"
fi
if [[ -z "${ALSOFT_DRIVERS:-}" ]]; then
    export ALSOFT_DRIVERS="pulse,alsa,oss,jack,null,wave"
    echo "INFO: OpenAL: ALSOFT_DRIVERS=$ALSOFT_DRIVERS (pipewire excluded)"
fi

# GeneralsX @feature felipebraz 18/02/2026 - Base Generals path and language are
# auto-detected by the game from filesystem (ZH_Generals/ or ../Generals/).
# Override by setting CNC_GENERALS_INSTALLPATH and/or CNC_ZH_LANGUAGE explicitly.

echo "🚀 Launching GeneralsXZH (Linux)"
echo "   Game: ${GAME_BINARY}"
echo "   DXVK: libdxvk_*.so (local + LD_LIBRARY_PATH)"
echo ""

# Change to game directory (needs game data files)
cd "${GAME_DIR}"

# Ensure logs directory exists in game dir
mkdir -p logs

# Launch with arguments (pass all script args to game)
exec "${GAME_BINARY}" "$@" 2>&1 |tee "$LOG_FILE"

echo 
echo "------------------------"
echo "✅ Run complete:"
echo "Exit code: $?"
echo "Log file: ${LOG_FILE}"