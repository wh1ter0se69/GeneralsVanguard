#!/bin/bash
# GeneralsX: Launch script for macOS (MoltenVK + SDL3)

set -e

# GeneralsX: Resolve repository root correctly from scripts/build/macos.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build/macos-vulkan"
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
LOG_FILE="${PROJECT_ROOT}/logs/run_zh_macos.log"

if [[ ! -f "${GAME_BINARY}" ]]; then
    echo "ERROR: Game binary not found at ${GAME_BINARY}"
    echo "Run deploy first: ./scripts/build/macos/deploy-macos-zh.sh"
    exit 1
fi

if [[ ! -f "${GAME_DIR}/libSDL3.0.dylib" ]]; then
    echo "ERROR: SDL3 library not found in ${GAME_DIR}"
    echo "Run deploy first: ./scripts/build/macos/deploy-macos-zh.sh"
    exit 1
fi

# GeneralsX: Community-patch overlay - see scripts/build/lib-overlay.sh.
#
# This script launches the binary directly instead of going through the deployed run.sh, so it
# has to carry the same environment. A launch path that forgets this does not fail visibly: it
# plays single-player perfectly and is refused by every peer with DATA_DIFFERS.
# This script has the repo, so it runs the check that can tell "not migrated" (fine, identical
# dataID) from "not there at all" (refused by every peer) rather than warning on both.
# shellcheck source=scripts/build/lib-overlay.sh
source "${PROJECT_ROOT}/scripts/build/lib-overlay.sh"
GX_PATCH_DIR="$(cd "${GAME_DIR}/.." 2>/dev/null && pwd)/GeneralsXPatch"
if [[ -d "${GX_PATCH_DIR}" ]]; then
    export GENERALSX_PATCH_PATH="${GENERALSX_PATCH_PATH:-${GX_PATCH_DIR}}"
fi
if ! GX_WHERE="$(gx_overlay_where "${GAME_DIR}")"; then
    echo "WARNING: this peer is missing shared game archives and cannot join anybody:"
    printf '%s\n' "${GX_WHERE}" | awk '$1=="ABSENT" { print "  missing: " $2 }'
    echo "  Fix once with: ${PROJECT_ROOT}/scripts/build/migrate-patch-overlay.sh"
fi

# Dylibs in game dir + Vulkan SDK for MoltenVK
export DYLD_LIBRARY_PATH="${GAME_DIR}:${DYLD_LIBRARY_PATH:-}"

# MoltenVK ICD so Vulkan loader finds MoltenVK.
# deploy-macos-zh.sh copies MoltenVK_icd.json into the game runtime dir;
# fall back to the Vulkan SDK path if it is not there yet.
MVK_ICD="${GAME_DIR}/MoltenVK_icd.json"
if [[ ! -f "${MVK_ICD}" ]]; then
    # Try the Vulkan SDK installation path (LunarG installer layout)
    for sdk_candidate in "${HOME}/VulkanSDK"/*/macOS; do
        if [[ -f "${sdk_candidate}/share/vulkan/icd.d/MoltenVK_icd.json" ]]; then
            MVK_ICD="${sdk_candidate}/share/vulkan/icd.d/MoltenVK_icd.json"
            break
        fi
    done
fi
if [[ -f "${MVK_ICD}" ]]; then
    export VK_ICD_FILENAMES="${MVK_ICD}"
fi

# Disable validation layers in production runs (remove for debug)
export VK_INSTANCE_LAYERS=""

# GeneralsX @bugfix fbraz3 20/03/2026 DXVK requires DXVK_WSI_DRIVER on non-Win32; must match game windowing (SDL3)
export DXVK_WSI_DRIVER="SDL3"

# GeneralsX: Explicitly point DXVK to runtime config file.
# Relying only on CWD can hide config/deploy issues and mislead terrain shader diagnostics.
if [[ -f "${GAME_DIR}/dxvk.conf" ]]; then
    export DXVK_CONFIG_FILE="${GAME_DIR}/dxvk.conf"
else
    echo "WARNING: ${GAME_DIR}/dxvk.conf not found; DXVK will use defaults."
fi

# GeneralsX: Ensure bundled Fontconfig is used when launching via this wrapper.
if [[ -f "${GAME_DIR}/fontconfig/fonts.conf" ]]; then
    export FONTCONFIG_FILE="${GAME_DIR}/fontconfig/fonts.conf"
    export FONTCONFIG_PATH="${GAME_DIR}/fontconfig"
else
    echo "WARNING: ${GAME_DIR}/fontconfig/fonts.conf not found; text rendering may fail."
fi

# GeneralsX: Optional shader-cache reset for terrain debugging.
# Use GX_CLEAR_DXVK_SHADER_CACHE=1 to force fresh shader compilation.
if [[ "${GX_CLEAR_DXVK_SHADER_CACHE:-0}" == "1" ]]; then
    DXVK_CACHE_DIR="${XDG_CACHE_HOME:-${HOME}/.cache}/dxvk"
    if [[ -d "${DXVK_CACHE_DIR}" ]]; then
        echo "Clearing DXVK shader cache in ${DXVK_CACHE_DIR}"
        rm -f "${DXVK_CACHE_DIR}"/*.dxvk.bin "${DXVK_CACHE_DIR}"/*.dxvk.lut 2>/dev/null || true
    fi
fi

mkdir -p "${PROJECT_ROOT}/logs"

echo "Launching GeneralsXZH (macOS)"
echo "   Game:  ${GAME_BINARY}"
echo "   SDL3:  ${GAME_DIR}/libSDL3.0.dylib"
echo "   Vulkan ICD: ${VK_ICD_FILENAMES:-system default}"
echo "   Overlay:    ${GENERALSX_PATCH_PATH:-none - joins will be refused}"
echo "   DXVK config: ${DXVK_CONFIG_FILE:-cwd/dxvk.conf (implicit)}"
echo "   Fontconfig file: ${FONTCONFIG_FILE:-unset}"
echo ""

cd "${GAME_DIR}"
exec "${GAME_BINARY}" "$@" 2>&1 | tee "${LOG_FILE}"
