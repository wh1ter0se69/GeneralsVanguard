#!/bin/bash
# Copyright 2026 GeneralsX contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# GeneralsX: Bundle macOS GeneralsXZH binary + dylibs into a zip archive
# Packages a self-contained .app for local distribution testing.

set -e

# GeneralsX: Resolve repository root correctly from scripts/build/macos.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
# GX_BUILD_DIR: a gx-ab-test flag arm's own build dir (gx-ab-build.sh).
# Its zip goes INTO that dir: both arms of one worktree share PROJECT_ROOT, so one fixed zip there would
# leave only the last arm bundled.
if [[ -n "${GX_BUILD_DIR:-}" && "${GX_BUILD_DIR}" != /* ]]; then
    echo "ERROR: GX_BUILD_DIR must be an absolute path (gx-ab-build.sh prints it): ${GX_BUILD_DIR}"; exit 1
fi
BUILD_DIR="${GX_BUILD_DIR:-${PROJECT_ROOT}/build/macos-vulkan}"
SDL3_LIB_DIR="${BUILD_DIR}/_deps/sdl3-build"
SDL3_IMAGE_LIB_DIR="${BUILD_DIR}/_deps/sdl3_image-build"
FONTCONFIG_ETC_DIR="${BUILD_DIR}/vcpkg_installed/arm64-osx/etc/fonts"
GAMESPY_LIB="${BUILD_DIR}/libgamespy.dylib"
# GeneralsX: Resolve DXVK dylib paths from both install copy and Meson output to avoid stale/incomplete bundles.
DXVK_D3D8_LIB_INSTALL="${BUILD_DIR}/libdxvk_d3d8.0.dylib"
DXVK_D3D9_LIB_INSTALL="${BUILD_DIR}/libdxvk_d3d9.0.dylib"
DXVK_D3D8_LIB_MESON="${BUILD_DIR}/_deps/dxvk-build-macos/src/d3d8/libdxvk_d3d8.0.dylib"
DXVK_D3D9_LIB_MESON="${BUILD_DIR}/_deps/dxvk-build-macos/src/d3d9/libdxvk_d3d9.0.dylib"
BINARY_SRC="${BUILD_DIR}/GeneralsMD/GeneralsXZH"
DXVK_CONF_SRC="${PROJECT_ROOT}/resources/dxvk/dxvk.conf"
OUTPUT_ZIP="${GX_BUILD_DIR:-${PROJECT_ROOT}}/GeneralsXZH-macos-arm64.zip"

# The overlay's source directories, as one list: it feeds both the archive search (find_overlay_archive) and the
# generator's --game-dir candidates. GX_PATCH_SRC is always set to a directory that never holds WindowZH.big, so the
# generator must also try GX_GAME_SRC. Defined before the build checks so GX_OVERLAY_RESOLVE_ONLY=1 works with no build.
# scripts/test/overlay-gamedir-test.sh checks that the two stay the same list.
GX_PATCH_SRC="${GX_PATCH_SRC:-${HOME}/GeneralsX/GeneralsXPatch}"
if [[ -n "${CNC_GENERALS_ZH_PATH:-}" ]]; then
    GX_GAME_SRC="${CNC_GENERALS_ZH_PATH}"
elif [[ -d "${HOME}/GeneralsX/GeneralsZH" ]]; then
    GX_GAME_SRC="${HOME}/GeneralsX/GeneralsZH"
else
    GX_GAME_SRC="${HOME}/GeneralsX/GeneralsMD"
fi
GX_OVERLAY_SRC_DIRS=("${GX_PATCH_SRC}" "${GX_GAME_SRC}")
# GX_GAME_DATA first: it is the explicit override, and the generator's own error names it.
_gx_gen="${GX_REPO_ROOT:-${PROJECT_ROOT}}/scripts/tooling/build-overlay-generated.py"
_gx_gen_dirs=()
for _d in ${GX_GAME_DATA:+"${GX_GAME_DATA}"} "${GX_OVERLAY_SRC_DIRS[@]}"; do
    _gx_gen_dirs+=(--game-dir "${_d}")
done
if [[ -n "${GX_OVERLAY_RESOLVE_ONLY:-}" ]]; then
    exec python3 "${_gx_gen}" --resolve "${_gx_gen_dirs[@]}"
fi

DXVK_D3D8_LIB="${DXVK_D3D8_LIB_INSTALL}"
DXVK_D3D9_LIB="${DXVK_D3D9_LIB_INSTALL}"
# The NEWER of the two, not the install copy whenever it exists: cmake refreshes that
# copy only when its own target is built, and a build of the game alone (gx-ab-build.sh) leaves it at whatever DXVK was
# current then, so the bundle would ship an old DXVK (without the key-file lock).
if [[ -f "${DXVK_D3D8_LIB_MESON}" && ( ! -f "${DXVK_D3D8_LIB}" || "${DXVK_D3D8_LIB_MESON}" -nt "${DXVK_D3D8_LIB}" ) ]]; then
    DXVK_D3D8_LIB="${DXVK_D3D8_LIB_MESON}"
fi
if [[ -f "${DXVK_D3D9_LIB_MESON}" && ( ! -f "${DXVK_D3D9_LIB}" || "${DXVK_D3D9_LIB_MESON}" -nt "${DXVK_D3D9_LIB}" ) ]]; then
    DXVK_D3D9_LIB="${DXVK_D3D9_LIB_MESON}"
fi

# Resolve @rpath/@loader_path deps while collecting external dylibs for portable bundles.
extract_rpaths() {
    local target="$1"
    otool -l "${target}" 2>/dev/null | awk '
        $1 == "cmd" && $2 == "LC_RPATH" { in_rpath = 1; next }
        in_rpath && $1 == "path" { print $2; in_rpath = 0 }
    '
}

resolve_dep_path() {
    local target="$1"
    local dep="$2"
    local executable_dir="$3"
    local loader_dir dep_name rpath candidate

    loader_dir="$(dirname "${target}")"

    if [[ "${dep}" == /* ]]; then
        [[ -f "${dep}" ]] && { echo "${dep}"; return 0; }
        return 1
    fi

    if [[ "${dep}" == @loader_path/* ]]; then
        candidate="${loader_dir}/${dep#@loader_path/}"
        [[ -f "${candidate}" ]] && { echo "${candidate}"; return 0; }
        return 1
    fi

    if [[ "${dep}" == @executable_path/* ]]; then
        candidate="${executable_dir}/${dep#@executable_path/}"
        [[ -f "${candidate}" ]] && { echo "${candidate}"; return 0; }
        return 1
    fi

    if [[ "${dep}" == @rpath/* ]]; then
        dep_name="${dep#@rpath/}"

        # Resolve @rpath deps already staged in the same lib directory before scanning LC_RPATH.
        candidate="${loader_dir}/${dep_name}"
        [[ -f "${candidate}" ]] && { echo "${candidate}"; return 0; }

        while IFS= read -r rpath; do
            [[ -z "${rpath}" ]] && continue
            rpath="${rpath//@loader_path/${loader_dir}}"
            rpath="${rpath//@executable_path/${executable_dir}}"
            candidate="${rpath}/${dep_name}"
            [[ -f "${candidate}" ]] && { echo "${candidate}"; return 0; }
        done < <(extract_rpaths "${target}")

        for candidate in \
            "/opt/homebrew/lib/${dep_name}" \
            "/usr/local/lib/${dep_name}"; do
            [[ -f "${candidate}" ]] && { echo "${candidate}"; return 0; }
        done

        if command -v brew >/dev/null 2>&1; then
            local brew_prefix
            brew_prefix="$(brew --prefix 2>/dev/null || true)"
            if [[ -n "${brew_prefix}" ]]; then
                for candidate in \
                    "${brew_prefix}/lib/${dep_name}" \
                    "${brew_prefix}/opt/webp/lib/${dep_name}"; do
                    [[ -f "${candidate}" ]] && { echo "${candidate}"; return 0; }
                done
            fi
        fi
    fi

    return 1
}

collect_external_dylibs() {
    local lib_dir="$1"
    shift
    local roots=("$@")
    local executable_dir

    executable_dir="$(dirname "${roots[0]}")"

    if ! command -v otool >/dev/null 2>&1; then
        echo "WARNING: otool not available; skipping external dylib collection"
        return 0
    fi

    local pending_file processed_file
    pending_file="${STAGE_DIR}/.bundle_pending.txt"
    processed_file="${STAGE_DIR}/.bundle_processed.txt"
    : > "${pending_file}"
    : > "${processed_file}"

    local root
    for root in "${roots[@]}"; do
        if [[ -f "${root}" ]]; then
            echo "${root}" >> "${pending_file}"
        fi
    done

    while [[ -s "${pending_file}" ]]; do
        local target
        target="$(head -n 1 "${pending_file}")"
        tail -n +2 "${pending_file}" > "${pending_file}.next" && mv "${pending_file}.next" "${pending_file}"

        if grep -Fqx "${target}" "${processed_file}"; then
            continue
        fi
        echo "${target}" >> "${processed_file}"

        local dep resolved_dep
        while IFS= read -r dep; do
            [[ -z "${dep}" ]] && continue
            # Skip known system libraries based on the raw otool entry before resolution.
            if [[ "${dep}" == /System/Library/* ]] || [[ "${dep}" == /usr/lib/* ]]; then
                continue
            fi
            resolved_dep="$(resolve_dep_path "${target}" "${dep}" "${executable_dir}" || true)"
            if [[ -z "${resolved_dep}" ]]; then
                # Warn when a non-system dep cannot be resolved instead of silently dropping it.
                echo "WARNING: Unable to resolve dependency '${dep}' for target '${target}'" >&2
                continue
            fi
            [[ ! -f "${resolved_dep}" ]] && continue

            local dep_name dep_dst
            dep_name="$(basename "${resolved_dep}")"
            dep_dst="${lib_dir}/${dep_name}"

            if [[ ! -f "${dep_dst}" ]]; then
                echo "  + external ${dep_name} (from ${resolved_dep})"
                # Dereference symlinks so we copy the actual dylib, not a dangling symlink.
                cp -L "${resolved_dep}" "${dep_dst}"
            fi

            if ! grep -Fqx "${dep_dst}" "${processed_file}" && ! grep -Fqx "${dep_dst}" "${pending_file}"; then
                echo "${dep_dst}" >> "${pending_file}"
            fi
        done < <(otool -L "${target}" 2>/dev/null | awk 'NR>1 {print $1}')
    done
}

# Locate installed Vulkan SDK:
#  1) Prefer explicit env vars (VULKAN_SDK, then VULKAN_SDK_ROOT)
#  2) Fall back to ~/VulkanSDK/*/macOS by convention
# Honor VULKAN_SDK/VULKAN_SDK_ROOT before scanning ~/VulkanSDK.
if [[ -n "${VULKAN_SDK:-}" ]] && [[ -f "${VULKAN_SDK}/lib/libvulkan.dylib" ]]; then
    VULKAN_SDK_ROOT="${VULKAN_SDK}"
elif [[ -n "${VULKAN_SDK_ROOT:-}" ]] && [[ -f "${VULKAN_SDK_ROOT}/lib/libvulkan.dylib" ]]; then
    : # pre-exported VULKAN_SDK_ROOT is valid; keep as-is
else
    VULKAN_SDK_ROOT=""
    for sdk_candidate in "${HOME}/VulkanSDK"/*/macOS; do
        if [[ -f "${sdk_candidate}/lib/libvulkan.dylib" ]]; then
            VULKAN_SDK_ROOT="${sdk_candidate}"
        fi
    done
fi

# Produce a macOS .app bundle with internal runtime env defaults.
APP_NAME="GeneralsXZH"
APP_DIR_NAME="${APP_NAME}.app"
INCLUDE_EXTERNAL_DYLIBS="${GX_BUNDLE_INCLUDE_EXTERNAL_DYLIBS:-1}"

echo "Bundling ${APP_NAME} (macOS ARM64)"

# Validate binary
if [[ ! -f "${BINARY_SRC}" ]]; then
    echo "ERROR: Binary not found at ${BINARY_SRC}"
    echo "Build first: ./scripts/build/macos/build-macos-zh.sh"
    exit 1
fi
if [[ ! -s "${BINARY_SRC}" ]]; then
    echo "ERROR: Binary at ${BINARY_SRC} is empty - build may have failed"
    exit 1
fi

# Prepare temp staging directory
STAGE_DIR="$(mktemp -d)"
trap 'rm -rf "${STAGE_DIR}"' EXIT
APP_DIR="${STAGE_DIR}/${APP_DIR_NAME}"
CONTENTS_DIR="${APP_DIR}/Contents"
MACOS_DIR="${CONTENTS_DIR}/MacOS"
RESOURCES_DIR="${CONTENTS_DIR}/Resources"
BIN_DIR="${RESOURCES_DIR}/bin"
LIB_DIR="${RESOURCES_DIR}/lib"
mkdir -p "${MACOS_DIR}" "${BIN_DIR}" "${LIB_DIR}"

# Info.plist
cat > "${CONTENTS_DIR}/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>
    <string>Generals Vanguard ZH</string>
    <key>CFBundleDisplayName</key>
    <string>Generals Vanguard ZH</string>
    <key>CFBundleIdentifier</key>
    <string>com.generalsx.generalsxzh</string>
    <key>CFBundleVersion</key>
    <string>0.1.0</string>
    <key>CFBundleShortVersionString</key>
    <string>0.1.0</string>
    <key>CFBundleExecutable</key>
    <string>run.sh</string>
    <key>CFBundleIconFile</key>
    <string>generalsx-zh_icon.png</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>LSMinimumSystemVersion</key>
    <string>15.0</string>
</dict>
</plist>
PLIST

echo "  Staging files to ${APP_DIR}..."

# Binary
echo "  + GeneralsXZH"
cp "${BINARY_SRC}" "${BIN_DIR}/GeneralsXZH"
chmod +x "${BIN_DIR}/GeneralsXZH"

# Icon
echo "  + Icon (generalsx-zh_icon.png)"
cp "${PROJECT_ROOT}/assets/generalsx-zh_icon.png" "${RESOURCES_DIR}/"

# GPL-3 + EA's additional terms (LICENSE.md: "Any propagation or conveyance of this program must include this
# copyright notice and these terms"): every package carries the repo's LICENSE.md.
echo "  + LICENSE.md"
cp "${PROJECT_ROOT}/LICENSE.md" "${RESOURCES_DIR}/LICENSE.md" || exit 1
# Third-party licence notices (MIT/BSD/zlib/Apache/LGPL/GPL components we bundle or link): generated by
# scripts/legal/third_party_notices.py, CI-style check: `third_party_notices.py check` and `check-packagers`.
# Freshness gate: refuse to package a stale or incomplete notices file (Cargo.lock / licence data moved without regenerating).
python3 -I "${PROJECT_ROOT}/scripts/legal/third_party_notices.py" check || exit 1
echo "  + THIRD_PARTY_NOTICES.txt"
cp "${PROJECT_ROOT}/THIRD_PARTY_NOTICES.txt" "${RESOURCES_DIR}/THIRD_PARTY_NOTICES.txt" || exit 1

# SDL3
echo "  + libSDL3"
cp "${SDL3_LIB_DIR}"/libSDL3.0.dylib "${LIB_DIR}/"
ln -sf libSDL3.0.dylib "${LIB_DIR}/libSDL3.dylib"

echo "  + libSDL3_image"
cp "${SDL3_IMAGE_LIB_DIR}"/libSDL3_image.0.4.0.dylib "${LIB_DIR}/"
ln -sf libSDL3_image.0.4.0.dylib "${LIB_DIR}/libSDL3_image.0.dylib"
ln -sf libSDL3_image.0.4.0.dylib "${LIB_DIR}/libSDL3_image.dylib"

# GameSpy
echo "  + libgamespy"
cp "${GAMESPY_LIB}" "${LIB_DIR}/"

# DXVK
if [[ ! -f "${DXVK_D3D9_LIB}" || ! -f "${DXVK_D3D8_LIB}" ]]; then
    echo "ERROR: Required DXVK dylibs were not found in expected locations:"
    echo "  d3d9 install: ${DXVK_D3D9_LIB_INSTALL}"
    echo "  d3d8 install: ${DXVK_D3D8_LIB_INSTALL}"
    echo "  d3d9 meson:   ${DXVK_D3D9_LIB_MESON}"
    echo "  d3d8 meson:   ${DXVK_D3D8_LIB_MESON}"
    echo "Build DXVK first: cmake --build build/macos-vulkan --target dxvk_d3d8_install"
    exit 1
fi
echo "  + libdxvk_d3d9"
cp "${DXVK_D3D9_LIB}" "${LIB_DIR}/libdxvk_d3d9.0.dylib"
ln -sf libdxvk_d3d9.0.dylib "${LIB_DIR}/libdxvk_d3d9.dylib"

# THE STAGED FILE, NOT THE ONE WE MEANT TO COPY.
#
# Checked here rather than at ${DXVK_D3D9_LIB} because the two can differ: DXVK_D3D9_LIB prefers
# the INSTALL copy (${BUILD_DIR}/libdxvk_d3d9.0.dylib), which cmake produces through a rule that
# depends on the ExternalProject TARGET and not on the meson output FILE - an ordering dependency,
# not a file one - so a stale install copy survives a perfectly good DXVK rebuild. Verifying the
# staged byte is the only check that cannot be satisfied by a different file than the one shipped.
"${SCRIPT_DIR}/verify-dxvk-retina-patch.sh" "${LIB_DIR}/libdxvk_d3d9.0.dylib" "staged libdxvk_d3d9.0.dylib" || exit 1
echo "  + libdxvk_d3d8"
cp "${DXVK_D3D8_LIB}" "${LIB_DIR}/libdxvk_d3d8.0.dylib"
ln -sf libdxvk_d3d8.0.dylib "${LIB_DIR}/libdxvk_d3d8.dylib"

# SagePatch (optional, gated by RTS_BUILD_OPTION_SAGE_PATCH at configure time).
SAGE_PATCH_LIB="${BUILD_DIR}/Patches/SagePatch/libsage_patch.dylib"
if [[ -f "${SAGE_PATCH_LIB}" ]]; then
    echo "  + libsage_patch (SagePatch QoL)"
    cp "${SAGE_PATCH_LIB}" "${LIB_DIR}/libsage_patch.dylib"
fi

if [[ "${INCLUDE_EXTERNAL_DYLIBS}" == "1" ]]; then
    echo "  + scanning for external dylibs (Homebrew/system extras)"
    collect_external_dylibs "${LIB_DIR}" \
        "${BIN_DIR}/GeneralsXZH" \
        "${LIB_DIR}/libSDL3.0.dylib" \
        "${LIB_DIR}/libSDL3_image.0.4.0.dylib" \
        "${LIB_DIR}/libgamespy.dylib" \
        "${LIB_DIR}/libdxvk_d3d8.0.dylib" \
        "${LIB_DIR}/libdxvk_d3d9.0.dylib"
else
    echo "  + skipping external dylib scan (GX_BUNDLE_INCLUDE_EXTERNAL_DYLIBS=${INCLUDE_EXTERNAL_DYLIBS})"
fi

# Vulkan + MoltenVK
if [[ -n "${VULKAN_SDK_ROOT}" ]]; then
    echo "  + libvulkan + libMoltenVK (from ${VULKAN_SDK_ROOT})"
    # The SDK loader under both names, one build or a refusal (see gx_copy_vulkan_loader).
    source "${SCRIPT_DIR}/lib-macho.sh"
    gx_copy_vulkan_loader "${VULKAN_SDK_ROOT}/lib" "${LIB_DIR}" || exit 1
    cp "${VULKAN_SDK_ROOT}/lib/libMoltenVK.dylib" "${LIB_DIR}/"
    cat > "${RESOURCES_DIR}/MoltenVK_icd.json" <<'EOF'
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "./lib/libMoltenVK.dylib",
        "api_version": "1.4.0",
        "is_portability_driver": true
    }
}
EOF
else
    # Fail bundle when Vulkan SDK is missing unless explicitly allowed.
    if [[ "${GX_ALLOW_MISSING_VULKAN_IN_BUNDLE:-0}" == "1" ]]; then
        echo "WARNING: Vulkan SDK not found at ~/VulkanSDK/*/macOS - Vulkan libs will be missing from bundle"
        echo "  GX_ALLOW_MISSING_VULKAN_IN_BUNDLE=1 set; producing bundle without Vulkan for local experiments"
        echo "  Note: This .app/.zip may be non-functional on machines without a system Vulkan/MoltenVK install."
    else
        echo "ERROR: Vulkan SDK not found at ~/VulkanSDK/*/macOS - Vulkan libs cannot be copied into bundle" >&2
        echo "  Install from: https://vulkan.lunarg.com/ and ensure VULKAN_SDK_ROOT is set," >&2
        echo "  or set GX_ALLOW_MISSING_VULKAN_IN_BUNDLE=1 to intentionally produce a non-Vulkan bundle." >&2
        exit 1
    fi
fi

# DXVK config
if [[ -f "${DXVK_CONF_SRC}" ]]; then
    echo "  + dxvk.conf"
    cp "${DXVK_CONF_SRC}" "${RESOURCES_DIR}/dxvk.conf"
else
    echo "WARNING: ${DXVK_CONF_SRC} not found - terrain shaders may fail on macOS"
fi

# A2: the shipped pipeline record a fresh install warms from (SDL3Main gxSeedDxvkCache
# copies it into the player's DXVK folder when absent; run.sh below names the folder).
# Fails rather than skips: a bundle without it is one whose first match compiles mid-match again, and nothing else says so.
DXVK_SEED_SRC="${PROJECT_ROOT}/resources/dxvk/seed"
if [[ ! -s "${DXVK_SEED_SRC}/GeneralsXZH.dxvk-cache" || ! -s "${DXVK_SEED_SRC}/GeneralsXZH.dxvk-ffkeys" ]]; then
    echo "ERROR: ${DXVK_SEED_SRC} lacks GeneralsXZH.dxvk-cache / .dxvk-ffkeys (the A2 pipeline seed)." >&2
    exit 1
fi
echo "  + dxvk-seed"
mkdir -p "${RESOURCES_DIR}/dxvk-seed"
cp "${DXVK_SEED_SRC}/GeneralsXZH.dxvk-cache" "${DXVK_SEED_SRC}/GeneralsXZH.dxvk-ffkeys" "${RESOURCES_DIR}/dxvk-seed/"

# Bundle Fontconfig config so FreeType/Fontconfig font matching works in app launcher runtime.
# Guard Fontconfig conf.d copy so missing directory does not break bundling under set -e.
if [[ -f "${FONTCONFIG_ETC_DIR}/fonts.conf" ]]; then
    echo "  + Fontconfig config"
    mkdir -p "${RESOURCES_DIR}/fontconfig"
    cp "${FONTCONFIG_ETC_DIR}/fonts.conf" "${RESOURCES_DIR}/fontconfig/fonts.conf"
    # vcpkg's relocatable fonts.conf names its FIRST cache dir as
    # `./../../var/cache/fontconfig`, and Fontconfig resolves that against the WORKING directory - the
    # Zero Hour data folder the launch script cd's into. MEASURED: a headless launch from ~/GeneralsX/GeneralsZH
    # wrote ~/var/cache/fontconfig/*.cache-9 into the player's home, which no uninstall can tell from anyone
    # else's; from a Steam library it would write into Steam's folder. The cache goes to ~/Library/Caches under the
    # bundle's own identifier, which uninstall removes: a fixed file cannot follow the product-folder rule
    # (installname::product_dir_under), and naming GeneralsVanguard there would CREATE it and move every program's
    # profile to an empty folder. Refuse to bundle if any relative cache dir is left.
    # Any cache dir that is neither absolute, nor ~-rooted, nor XDG-relative is the vcpkg one, whatever its
    # attributes (prefix="default" and the like).
    perl -0pi -e 's#<cachedir(?![^>]*prefix="xdg")[^>]*>\s*(?![~/])[^<]*</cachedir>#<cachedir>~/Library/Caches/com.generalsx.generalsxzh/fontconfig</cachedir>#g' \
        "${RESOURCES_DIR}/fontconfig/fonts.conf"
    if perl -0ne 'exit((/<cachedir(?![^>]*prefix="xdg")[^>]*>\s*(?![~\/])/) ? 0 : 1)' "${RESOURCES_DIR}/fontconfig/fonts.conf"; then
        echo "ERROR: ${RESOURCES_DIR}/fontconfig/fonts.conf still has a relative <cachedir> - it would write beside the game data" >&2
        exit 1
    fi
    rm -rf "${RESOURCES_DIR}/fontconfig/conf.d"
    if [[ -d "${FONTCONFIG_ETC_DIR}/conf.d" ]]; then
        cp -R "${FONTCONFIG_ETC_DIR}/conf.d" "${RESOURCES_DIR}/fontconfig/conf.d"
    else
        echo "WARNING: ${FONTCONFIG_ETC_DIR}/conf.d not found - using default Fontconfig configuration; font matching may differ at runtime"
    fi
else
    echo "WARNING: ${FONTCONFIG_ETC_DIR}/fonts.conf not found - in-game font lookup may fail on macOS"
fi

# Ship the community-patch data; without it every join is refused with DATA_DIFFERS. The .big overlay rides in
# Contents/Resources/GeneralsXPatch (run.sh exports GENERALSX_PATCH_PATH); Multiplayer.ini and Generals.csf ride in
# Contents/Resources/gamedata (GENERALSX_DATA_OVERLAY) and are opened by name before the cwd, so the player's folder is
# never written. Optional archives may stay in the game folder. Incomplete is fatal unless GX_BUNDLE_ALLOW_NO_OVERLAY=1.
# shellcheck source=scripts/build/lib-overlay.sh
source "${PROJECT_ROOT}/scripts/build/lib-overlay.sh"

PATCH_STAGE="${RESOURCES_DIR}/GeneralsXPatch"
GAMEDATA_STAGE="${RESOURCES_DIR}/gamedata"

# GX_PATCH_SRC and GX_GAME_SRC - the two places the archives are looked for - are defined at the
# top of this file, as GX_OVERLAY_SRC_DIRS, so the generator step below searches the same ones.

# A usable archive is -f AND -s: -f alone accepts a zero-byte file (a zip that looks complete over an engine that mounts
# nothing), -s alone accepts a directory named like an archive. lib-overlay.sh's gx_overlay_verify/gx_overlay_where test
# -s alone; their input is a directory their own deploy script populated.
find_overlay_archive() {
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

echo "  + community-patch overlay"
mkdir -p "${PATCH_STAGE}"

# GENERATE !GXUI.big HERE, the way the Windows packager
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
        echo "  Set GX_GAME_DATA to a retail install (the directory holding WindowZH.big)." >&2
        exit 1
    fi
else
    echo "ERROR: overlay generator missing at ${_gx_gen} - run this from a checkout." >&2
    exit 1
fi
# THE TWO LISTS HAVE DIFFERENT CONSEQUENCES AND ARE TRACKED SEPARATELY. Conflating them is a
# mistake the partial-source test catches: every entry in
# overlay-optional.txt is MEASURED to contain zero files the SimID fingerprint reads, so a
# missing optional archive cannot move dataID and cannot cost anybody a join. Discarding the
# mandatory archives because an optional one was absent would take a bundle that joins the fleet
# perfectly and make it join nobody - the exact failure this script exists to remove.
OVERLAY_MISSING_REQUIRED=()
OVERLAY_MISSING_OPTIONAL=()
OVERLAY_STAGED_OPTIONAL=()
OVERLAY_STAGED=0

# Required: the set every peer must mount or dataID does not match. Names come from
# overlay-archives.txt via the shared library - never hardcoded here, because that file exists
# precisely so the four platforms cannot drift.
gx_overlay_count > /dev/null || exit 1
while IFS= read -r _a; do
    if _src="$(find_overlay_archive "${_a}")"; then
        cp "${_src}" "${PATCH_STAGE}/${_a}" || exit 1
        echo "      [required] ${_a}"
        OVERLAY_STAGED=$((OVERLAY_STAGED + 1))
    else
        OVERLAY_MISSING_REQUIRED+=("${_a}")
    fi
done < <(gx_overlay_archives)

# Optional archives ship either way; the default decides only the name on arrival (<name>.big is mounted, .big.off is
# not), so enabling one later is a rename, not a download. Nothing on this list may move dataID. The list is captured
# so the producer's exit status is checked: `while read < <(...)` discards it. Empty output is a valid build.
OVERLAY_OPTIONAL_LINES="$(gx_overlay_optional)" || exit 1
while IFS= read -r _line; do
    [[ -n "${_line}" ]] || continue
    _name="$(gx_overlay_optional_name "${_line}")"
    _dflt="$(gx_overlay_optional_default "${_line}")"
    _out="${_name}"
    [[ "${_dflt}" == "off" ]] && _out="${_name}${GX_OVERLAY_OFF_SUFFIX}"
    if _src="$(find_overlay_archive "${_name}")"; then
        cp "${_src}" "${PATCH_STAGE}/${_out}" || exit 1
        echo "      [${_dflt}] ${_out}"
        OVERLAY_STAGED=$((OVERLAY_STAGED + 1))
        OVERLAY_STAGED_OPTIONAL+=("${_name}|$(gx_overlay_optional_label "${_line}")")
    else
        OVERLAY_MISSING_OPTIONAL+=("${_name}")
    fi
done <<< "${OVERLAY_OPTIONAL_LINES}"

# Which archives may be toggled, shipped as data rather than compiled into anything, so it cannot
# drift from overlay-optional.txt. Only the ones actually staged are listed: offering a toggle for
# an archive that is not there would be a switch that silently does nothing. A .txt in here is
# inert to the engine - the overlay scan matches "*.big". Same format the Windows payload uses.
if [[ ${#OVERLAY_MISSING_REQUIRED[@]} -eq 0 ]]; then
    : > "${PATCH_STAGE}/optional.txt"
    for _o in ${OVERLAY_STAGED_OPTIONAL[@]+"${OVERLAY_STAGED_OPTIONAL[@]}"}; do
        printf '%s\n' "${_o}" >> "${PATCH_STAGE}/optional.txt"
    done

    # And the mandatory set, as required.txt, in the format package-windows-share.ps1 writes: bare filenames. Without
    # it the launcher's readiness check can never say Ready on a Mac. Read at runtime, not compiled in, so it cannot
    # drift from the payload. Written only in the arm where every mandatory archive was staged.
    : > "${PATCH_STAGE}/required.txt"
    gx_overlay_archives >> "${PATCH_STAGE}/required.txt" || exit 1

    # The shipped maps (overlay-maps.txt) ride in the payload as loose files; the launcher copies them
    # into <userdata>/Maps. Inside this guard with the lists, and fatal: a bundle that advertises
    # maps it cannot carry is worse than none.
    gx_overlay_stage_shipped_maps "${PATCH_STAGE}" || exit 1
    echo "      ${OVERLAY_STAGED} archive(s), $(du -sh "${PATCH_STAGE}" | cut -f1)"
fi

if [[ ${#OVERLAY_MISSING_REQUIRED[@]} -gt 0 || ${#OVERLAY_MISSING_OPTIONAL[@]} -gt 0 ]]; then
    if [[ "${GX_BUNDLE_ALLOW_NO_OVERLAY:-0}" == "1" ]]; then
        # Same escape hatch, and the same shape, as GX_ALLOW_MISSING_VULKAN_IN_BUNDLE above: an
        # intentionally incomplete bundle for a compile/packaging smoke test.
        #
        # THE STAMP IS WRITTEN ON BOTH ARMS, and that is the part that makes the release gate
        # possible rather than merely advisory. It used to be written only when a MANDATORY
        # archive was absent, so an escape-hatch build that was short only optional content
        # produced an artifact indistinguishable from a complete one - and shipping it would have
        # been a silent content regression rather than a loud one. Its first line is machine
        # readable on purpose: verify-bundle-overlay.sh refuses any .app that carries this file,
        # whatever the reason it was written.
        if [[ ${#OVERLAY_MISSING_REQUIRED[@]} -gt 0 ]]; then
            # A PARTIAL MANDATORY SET IS NOT A HALFWAY HOUSE, it is a third dataID that matches
            # nobody and costs the player the download anyway. Throw the staging away so an
            # unjoinable bundle is unambiguously unjoinable rather than mysteriously incompatible.
            rm -rf "${PATCH_STAGE}"
            echo "WARNING: mandatory overlay archives missing - THIS BUNDLE CANNOT JOIN ANY GAME."
            for _a in "${OVERLAY_MISSING_REQUIRED[@]}"; do echo "  missing: ${_a}"; done
            {
                echo "GX_BUNDLE_INCOMPLETE=required"
                echo "This bundle was built with GX_BUNDLE_ALLOW_NO_OVERLAY=1."
                echo "It carries NO community-patch archives, computes a dataID that matches no peer,"
                echo "and every multiplayer join will be refused with DATA_DIFFERS."
                echo "It is a packaging smoke test, not a release. Missing:"
                for _a in "${OVERLAY_MISSING_REQUIRED[@]}"; do echo "  ${_a}"; done
                for _a in ${OVERLAY_MISSING_OPTIONAL[@]+"${OVERLAY_MISSING_OPTIONAL[@]}"}; do
                    echo "  ${_a} (optional)"
                done
            } > "${RESOURCES_DIR}/OVERLAY-MISSING.txt"
        else
            # Mandatory set complete, so this bundle DOES join the fleet - it is merely short of
            # cosmetic content. Say exactly that rather than crying that it cannot play.
            echo "WARNING: optional overlay archives missing - the bundle still joins games,"
            echo "  but ships without this content and cannot switch it on later:"
            for _a in "${OVERLAY_MISSING_OPTIONAL[@]}"; do echo "  missing: ${_a}"; done
            {
                echo "GX_BUNDLE_INCOMPLETE=optional"
                echo "This bundle was built with GX_BUNDLE_ALLOW_NO_OVERLAY=1."
                echo "The mandatory overlay is complete, so it JOINS games and computes the fleet dataID."
                echo "It is short of optional content, which cannot be switched on later because the"
                echo "toggle is a rename and not a download. It is a packaging smoke test, not a"
                echo "release. Missing:"
                for _a in "${OVERLAY_MISSING_OPTIONAL[@]}"; do echo "  ${_a}"; done
            } > "${RESOURCES_DIR}/OVERLAY-MISSING.txt"
        fi
        echo "  GX_BUNDLE_ALLOW_NO_OVERLAY=1 set; continuing for a packaging-only build."
        echo "  Stamped ${RESOURCES_DIR}/OVERLAY-MISSING.txt - the release gate refuses this bundle."
    else
        # Fatal for BOTH lists by default, matching package-windows-share.ps1 - a shipped artifact
        # should be complete, and an optional archive that is absent cannot be switched on later
        # because the toggle is a rename, not a download. The two are still reported apart so the
        # message says what is actually at stake.
        echo "ERROR: community-patch overlay incomplete - refusing to ship an incomplete zip." >&2
        for _a in ${OVERLAY_MISSING_REQUIRED[@]+"${OVERLAY_MISSING_REQUIRED[@]}"}; do
            echo "  missing (required, moves dataID): ${_a}" >&2
        done
        for _a in ${OVERLAY_MISSING_OPTIONAL[@]+"${OVERLAY_MISSING_OPTIONAL[@]}"}; do
            echo "  missing (optional, dataID-neutral): ${_a}" >&2
        done
        echo "  Looked in: ${GX_PATCH_SRC}" >&2
        echo "         and: ${GX_GAME_SRC}" >&2
        if [[ ${#OVERLAY_MISSING_REQUIRED[@]} -gt 0 ]]; then
            echo "  A player on this bundle installs a game that plays single-player perfectly and is" >&2
            echo "  refused by every peer with DATA_DIFFERS, which reads as a network fault." >&2
        fi
        echo "  Point GX_PATCH_SRC at a directory holding them, or set" >&2
        echo "  GX_BUNDLE_ALLOW_NO_OVERLAY=1 for a deliberately incomplete packaging test." >&2
        exit 1
    fi
fi

# The loose pair. Both are tracked in this repo, so a checkout always has them and their absence
# is a broken checkout rather than a missing environment - hence no escape hatch here.
#
# Refuse to ship half of it, exactly as deploy-macos-zh.sh and the Windows packager do and for
# the same reason: the CSF is NOT part of iniCRC (TheGameText is initialised with a null Xfer -
# GeneralsMD/Code/GameEngine/Source/Common/GameEngine.cpp:589, the last argument of
# initSubsystem), so a player who got the INI but not the CSF still joins everybody and
# merely renders MISSING: 'Color:Turquoise' in the lobby - which reads as a font or localisation
# fault instead of as a package that stopped halfway.
MP_INI_SRC="${PROJECT_ROOT}/GeneralsZH/Data/INI/Multiplayer.ini"
COLOR_CSF_SRC="${PROJECT_ROOT}/GeneralsZH/Data/English/Generals.csf"
if [[ -f "${MP_INI_SRC}" ]]; then
    if [[ ! -f "${COLOR_CSF_SRC}" ]]; then
        echo "ERROR: ${MP_INI_SRC} exists but ${COLOR_CSF_SRC} does not." >&2
        echo "       The colour blocks and the labels that name them ship together." >&2
        exit 1
    fi
    echo "  + loose player-colour data (served from Resources/gamedata via GENERALSX_DATA_OVERLAY)"
    mkdir -p "${GAMEDATA_STAGE}/Data/INI" "${GAMEDATA_STAGE}/Data/English"
    cp "${MP_INI_SRC}"    "${GAMEDATA_STAGE}/Data/INI/Multiplayer.ini"
    cp "${COLOR_CSF_SRC}" "${GAMEDATA_STAGE}/Data/English/Generals.csf"
    GX_GAMEDATA_FILES=("Data/INI/Multiplayer.ini" "Data/English/Generals.csf")

    # Data/INI/Weapon.ini, the Toxin Tractor balance change, cut
    # from the CoreINI archive staged above (scripts/tooling/make-weapon-ini.py; never committed, it is
    # the Community Patch's file). It moves dataID, so a release bundle without it is refused. Only the
    # GX_BUNDLE_ALLOW_NO_OVERLAY smoke bundle goes without: it is already stamped as no release.
    if python3 "${PROJECT_ROOT}/scripts/tooling/make-weapon-ini.py" --search "${PATCH_STAGE}" \
            --game-dir "${GX_GAME_SRC}" --out "${GAMEDATA_STAGE}/Data/INI/Weapon.ini"; then
        # Weapon.ini and the Alpha Aurora fix files beside it: the recipe names them all.
        while IFS= read -r _gx_rf; do
            GX_GAMEDATA_FILES+=("${_gx_rf}")
        done < <(python3 "${PROJECT_ROOT}/scripts/tooling/make-weapon-ini.py" --list)
    elif [[ "${GX_BUNDLE_ALLOW_NO_OVERLAY:-0}" == "1" ]]; then
        # Stamped whatever the overlay block wrote: a complete overlay with a CoreINI the recipe refuses writes no stamp there, and
        # verify-bundle-overlay.sh would then pass a bundle whose dataID matches no peer. It is required-level: the bundle cannot join.
        echo "WARNING: Data/INI/Weapon.ini NOT staged - THIS BUNDLE CANNOT JOIN ANY GAME."
        # An overlay stamp already written keeps its list of missing archives (its prose would
        # contradict this one: an "optional" stamp says the bundle joins games).
        _gx_stamp="${RESOURCES_DIR}/OVERLAY-MISSING.txt"
        _gx_prev=""
        if [[ -f "${_gx_stamp}" ]]; then
            _gx_prev="$(sed -n '/^  /p' "${_gx_stamp}")"
            if [[ -n "${_gx_prev}" && "$(head -1 "${_gx_stamp}")" == "GX_BUNDLE_INCOMPLETE=optional" ]]; then
                _gx_prev="$(printf '%s\n' "${_gx_prev}" | sed 's/$/ (optional)/')"
            fi
        fi
        {
            echo "GX_BUNDLE_INCOMPLETE=required"
            echo "This bundle was built with GX_BUNDLE_ALLOW_NO_OVERLAY=1 and carries no Data/INI/Weapon.ini"
            echo "(make-weapon-ini.py could not cut it from Contents/Resources/GeneralsXPatch), so it computes"
            echo "a dataID that matches no peer and every multiplayer join will be refused with DATA_DIFFERS."
            echo "It is a packaging smoke test, not a release. Missing:"
            echo "  Data/INI/Weapon.ini"
            [[ -z "${_gx_prev}" ]] || printf '%s\n' "${_gx_prev}"
        } > "${_gx_stamp}"
    else
        echo "ERROR: could not generate Data/INI/Weapon.ini from ${PATCH_STAGE}." >&2
        exit 1
    fi

    # The launcher gets a manifest instead of enumerating at runtime: a `find` earlier on the player's PATH can shadow
    # /usr/bin/find and silently install nothing. run.sh reads it only to remove byte-identical copies an older build
    # left in a game folder.
    printf '%s\n' "${GX_GAMEDATA_FILES[@]}" > "${RESOURCES_DIR}/gamedata.manifest"

    # Prove the manifest and the staging agree, here, rather than letting a player find out. Every
    # listed path must exist and be non-empty; anything staged but unlisted would never install.
    while IFS= read -r _rel; do
        [[ -n "${_rel}" ]] || continue
        if [[ ! -s "${GAMEDATA_STAGE}/${_rel}" ]]; then
            echo "ERROR: gamedata.manifest lists '${_rel}' but it was not staged." >&2
            exit 1
        fi
        echo "      ${_rel}"
        # run.sh removes a copy from a game folder only when it matches this list, so a pair that
        # is not on it would be left behind in every folder an older build wrote into. Weapon.ini is
        # not on it on purpose (no build ever wrote it into a game folder): its bytes are the
        # recipe's, checked again by verify-bundle-overlay.sh.
        # Nor are the other recipe files (the Alpha Aurora fix), for the same reason.
        python3 "${PROJECT_ROOT}/scripts/tooling/make-weapon-ini.py" --list | grep -qxF "${_rel}" && continue
        _sum="$(shasum -a 256 "${GAMEDATA_STAGE}/${_rel}")"
        if ! grep -qxF "${_sum%% *}  ${_rel}" "${SCRIPT_DIR}/gamedata-shipped.sha256"; then
            echo "ERROR: ${_rel} (sha256 ${_sum%% *}) is not in scripts/build/macos/gamedata-shipped.sha256." >&2
            echo "       Append that line; never remove one." >&2
            exit 1
        fi
    done < "${RESOURCES_DIR}/gamedata.manifest"
    cp "${SCRIPT_DIR}/gamedata-shipped.sha256" "${RESOURCES_DIR}/gamedata-shipped.sha256"
fi

# App launcher wrapper
echo "  + App launcher"
cat > "${MACOS_DIR}/run.sh" << 'WRAPPER'
#!/bin/bash
# GeneralsX: App launcher with bundled runtime + default asset paths
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CONTENTS_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
RESOURCES_DIR="${CONTENTS_DIR}/Resources"
BIN_DIR="${RESOURCES_DIR}/bin"
LIB_DIR="${RESOURCES_DIR}/lib"

export DYLD_LIBRARY_PATH="${LIB_DIR}:${BIN_DIR}:${DYLD_LIBRARY_PATH:-}"

# GeneralsX: Point the engine at the overlay this bundle carries.
#
# Unlike the deploy layout, the .app's executable (Contents/Resources/bin) is disjoint from the
# asset root, so the engine's own <exe dir>/GeneralsXPatch default would not double-mount and
# would have worked too. Setting it explicitly matches deploy-macos-zh.sh, keeps one mechanism
# across both macOS paths, and survives the binary being moved.
#
# ${VAR:-default}, so a player or a test harness that already set it keeps their own overlay.
GX_PATCH_DIR="${RESOURCES_DIR}/GeneralsXPatch"
if [[ -d "${GX_PATCH_DIR}" ]]; then
    export GENERALSX_PATCH_PATH="${GENERALSX_PATCH_PATH:-${GX_PATCH_DIR}}"
fi

# SagePatch (optional QoL: F11 screenshot, Scroll Lock cursor lock, Ctrl+PgUp/Dn
# brightness, Ctrl+1..5 window snap). Only loaded when the bundled dylib is
# present and SAGE_PATCH_DISABLED is not set. Also seeds the engine INI loader
# via Resources/Data/INI/GameData/SagePatch.ini.
if [[ -f "${LIB_DIR}/libsage_patch.dylib" && "${SAGE_PATCH_DISABLED:-0}" != "1" ]]; then
    if [[ -n "${DYLD_INSERT_LIBRARIES:-}" ]]; then
        export DYLD_INSERT_LIBRARIES="${LIB_DIR}/libsage_patch.dylib:${DYLD_INSERT_LIBRARIES}"
    else
        export DYLD_INSERT_LIBRARIES="${LIB_DIR}/libsage_patch.dylib"
    fi
fi

# GeneralsX @bugfix fbraz3 20/03/2026 DXVK requires this env var on non-Win32; SDL3 matches game windowing layer
export DXVK_WSI_DRIVER="SDL3"

# DXVK HUD: kept opt-in. MoltenVK on macOS 26 cannot compile DXVK's HUD
# pipeline shader (gl_DrawID / SPIR-V DrawIndex has no MSL equivalent yet).
# Defaulting it on causes the swap chain blit pipeline to fail. Users wanting
# an FPS overlay set DXVK_HUD=fps themselves.
export DXVK_HUD="${DXVK_HUD:-0}"

if [[ -f "${RESOURCES_DIR}/MoltenVK_icd.json" ]]; then
    export VK_ICD_FILENAMES="${RESOURCES_DIR}/MoltenVK_icd.json"
    # GeneralsX @bugfix fbraz3 20/03/2026 Vulkan Loader 1.3.236+ uses VK_DRIVER_FILES; keep VK_ICD_FILENAMES for older loaders
    export VK_DRIVER_FILES="${RESOURCES_DIR}/MoltenVK_icd.json"
fi

# GeneralsX: Select default Zero Hour asset path by .big presence, with GeneralsMD fallback.
# Default asset paths matching the standard macOS deploy layout (allow user override)
# GeneralsX @feature wh1ter0se 11/11/2026 The install folder is ~/GeneralsVanguard from the rename on and
# ~/GeneralsX before it: the first whose Zero Hour folder holds a non-empty INIZH.big, else ~/GeneralsX as
# before. The base game's folder is chosen on its own, so it may stay under either name.
GX_INSTALL_ROOT="${HOME}/GeneralsX"
for gx_root in "${HOME}/GeneralsVanguard" "${HOME}/GeneralsX"; do
    if [[ -s "${gx_root}/GeneralsZH/INIZH.big" || -s "${gx_root}/GeneralsMD/INIZH.big" ]]; then
        GX_INSTALL_ROOT="${gx_root}"
        break
    fi
done
GX_BASE_DEFAULT="${HOME}/GeneralsX/Generals"
if [[ -n "$(compgen -G "${HOME}/GeneralsVanguard/Generals/*.big" 2>/dev/null)" ]]; then
    GX_BASE_DEFAULT="${HOME}/GeneralsVanguard/Generals"
fi
export CNC_GENERALS_PATH="${CNC_GENERALS_PATH:-${GX_BASE_DEFAULT}}"
if [[ -z "${CNC_GENERALS_ZH_PATH:-}" ]]; then
    if [[ -d "${GX_INSTALL_ROOT}/GeneralsZH" && -n "$(compgen -G "${GX_INSTALL_ROOT}/GeneralsZH/*.big" 2>/dev/null)" ]]; then
        export CNC_GENERALS_ZH_PATH="${GX_INSTALL_ROOT}/GeneralsZH"
    elif [[ -d "${GX_INSTALL_ROOT}/GeneralsMD" && -n "$(compgen -G "${GX_INSTALL_ROOT}/GeneralsMD/*.big" 2>/dev/null)" ]]; then
        export CNC_GENERALS_ZH_PATH="${GX_INSTALL_ROOT}/GeneralsMD"
    elif [[ -d "${GX_INSTALL_ROOT}/GeneralsZH" ]]; then
        export CNC_GENERALS_ZH_PATH="${GX_INSTALL_ROOT}/GeneralsZH"
    else
        export CNC_GENERALS_ZH_PATH="${GX_INSTALL_ROOT}/GeneralsMD"
    fi
fi

# Backward compatibility for existing runtime readers
if [[ -z "${CNC_GENERALS_INSTALLPATH:-}" ]]; then
    export CNC_GENERALS_INSTALLPATH="${CNC_GENERALS_PATH}"
fi

if [[ -f "${RESOURCES_DIR}/dxvk.conf" ]]; then
    export DXVK_CONFIG_FILE="${RESOURCES_DIR}/dxvk.conf"
fi

# GeneralsX: A2: the shipped pipeline record (SDL3Main gxSeedDxvkCache). A caller's value - even an
# empty one, which turns the seed off - wins.
export GX_DXVK_SEED_DIR="${GX_DXVK_SEED_DIR-${RESOURCES_DIR}/dxvk-seed}"

# GeneralsX: Set bundled Fontconfig config path to avoid "Cannot load default config file: (null)" on macOS.
if [[ -f "${RESOURCES_DIR}/fontconfig/fonts.conf" ]]; then
    export FONTCONFIG_FILE="${RESOURCES_DIR}/fontconfig/fonts.conf"
    export FONTCONFIG_PATH="${RESOURCES_DIR}/fontconfig"
fi

# GeneralsX: THE LOOSE PAIR IS SERVED FROM INSIDE THE .app, AND THE GAME
# FOLDER IS NEVER WRITTEN TO.
#
# This used to cp Data/INI/Multiplayer.ini and Data/English/Generals.csf INTO the game folder, over
# whatever was there, with no backup. On 2026-09-23 a player pointed the app at a folder he shares
# with other Zero Hour clients. They have no CSF fallback, so they loaded our 6-label table and
# rendered every menu label as MISSING: 'GUI:...'. The overwritten Multiplayer.ini also changed
# their dataID. GeneralsX has no business writing into a folder it does not own.
#
# The engine now reads the pair from here. StdLocalFileSystem resolves a relative READ against
# GENERALSX_DATA_OVERLAY before the working directory, so the pair beats the archives exactly as a
# loose file in the game folder did. m_iniCRC hashes INI line content, not the path it came from,
# so dataID is unchanged. ${VAR:-default}, so a harness that set its own overlay keeps it.
GX_GAMEDATA_DIR="${RESOURCES_DIR}/gamedata"
if [[ -d "${GX_GAMEDATA_DIR}" ]]; then
    export GENERALSX_DATA_OVERLAY="${GENERALSX_DATA_OVERLAY:-${GX_GAMEDATA_DIR}}"
fi

# Run from the detected Zero Hour asset root when available.
if [[ -d "${CNC_GENERALS_ZH_PATH}" ]]; then
    cd "${CNC_GENERALS_ZH_PATH}"

    # SagePatch INI override: the engine now auto-creates SagePatch.ini with
    # defaults in the user data directory on first run.

    # Undo what older builds of this wrapper did, and nothing else. A file is removed ONLY when
    # its sha256 and its path both match a line in gamedata-shipped.sha256, the list of every
    # version of the pair GeneralsX ever shipped. So it is byte-for-byte our file, and the
    # overlay above serves the same bytes. A file that differs in any way stays: it may be the
    # player's own. If an older build overwrote a player's own file, the original is gone and
    # this cannot bring it back.
    #
    # Skipped when the folder holds a GeneralsX binary: the macOS/Linux deploy layout
    # (GeneralsXZH) or a Windows GeneralsX install (generalszh.exe, GeneralsXLauncher.exe,
    # GeneralsXUpdate.exe - e.g. in a CrossOver bottle). Those are folders we own whose own runs
    # have no overlay and need the pair; removing it there would split that install's dataID.
    # And skipped when any directory on the way is a symlink (pwd -P differs), so a link can never
    # carry the removal into some other folder.
    # And skipped when the overlay is on a case-sensitive volume: the engine asks for
    # data/<lang>/generals.csf (SDL3Main g_csfFile) and the overlay matches exact case only
    # (StdLocalFileSystem), so there the loose copy is the one CSF the engine can still find.
    # DATA resolves to the overlay's Data only where case folds.
    # /usr/bin/shasum and builtins only: a launcher runs on someone else's PATH, and a shimmed
    # `find` once made the old install loop silently do nothing. No shasum means no removal, the
    # safe direction.
    GX_GAMEDATA_MANIFEST="${RESOURCES_DIR}/gamedata.manifest"
    GX_SHIPPED_SHA="${RESOURCES_DIR}/gamedata-shipped.sha256"
    if [[ -f "${GX_GAMEDATA_MANIFEST}" && -f "${GX_SHIPPED_SHA}" && -x /usr/bin/shasum && -e "${GX_GAMEDATA_DIR}/DATA" \
          && ! -e ./GeneralsXZH && ! -e ./generalszh.exe && ! -e ./GeneralsXLauncher.exe && ! -e ./GeneralsXUpdate.exe ]]; then
        gx_shipped=$'\n'"$(<"${GX_SHIPPED_SHA}")"$'\n'
        gx_here="$(pwd -P)"
        while IFS= read -r gx_rel || [[ -n "${gx_rel}" ]]; do
            [[ -n "${gx_rel}" && "${gx_rel}" == */* && -f "${gx_rel}" && ! -L "${gx_rel}" ]] || continue
            [[ "$(cd "${gx_rel%/*}" 2>/dev/null && pwd -P)" == "${gx_here}/${gx_rel%/*}" ]] || continue
            gx_sum="$(/usr/bin/shasum -a 256 "${gx_rel}" 2>/dev/null)"
            gx_sum="${gx_sum%% *}"
            [[ ${#gx_sum} -eq 64 ]] || continue
            if [[ "${gx_shipped}" == *$'\n'"${gx_sum}  ${gx_rel}"$'\n'* ]]; then
                if /bin/rm -f -- "${gx_rel}"; then
                    echo "[GXDATA] removed ${gx_rel} from ${PWD}: an older GeneralsX installed it there" >&2
                fi
            fi
        done < "${GX_GAMEDATA_MANIFEST}"
    fi
fi

# A PLAYER WHO DOUBLE-CLICKED THE APP NEVER SEES stderr (26/08/2026).
#
# The engine now stops immediately when it cannot find Zero Hour's data and prints what to do
# (StdBIGFileSystem.cpp, exit 78 = EX_CONFIG). From a terminal that is enough. From Finder it is
# not: the process writes to a stderr nobody is reading and the window simply never appears, which
# is what "it just crashes" looks like from the outside. So the wrapper turns that one exit code
# into a dialog.
#
# `exec` is dropped ONLY on the path that needs a parent to inspect the code. Everything else is
# unchanged: a normal run still ends when the engine ends, and any other exit code is passed
# straight through so nothing else is swallowed or reinterpreted.
"${BIN_DIR}/GeneralsXZH" "$@"
GX_RC=$?
if [ "${GX_RC}" -eq 78 ]; then
    # -e with a literal, not a variable, so nothing the engine printed can be read as AppleScript.
    osascript -e 'display dialog "Generals Vanguard cannot find your Command & Conquer: Generals Zero Hour game files.

Generals Vanguard is the engine. It does not include the game'"'"'s data, so it needs a copy of Zero Hour that you already own.

Put your Zero Hour files in a folder called GeneralsZH inside a folder called GeneralsX in your home folder. It should contain about two dozen .big files, including INIZH.big.

Open the setup instructions?" with title "Zero Hour game files not found" buttons {"Quit", "Open instructions"} default button "Open instructions" with icon caution' >/dev/null 2>&1 \
        && open "https://wh1ter0se.dev/#download" >/dev/null 2>&1
fi
exit "${GX_RC}"
WRAPPER
chmod +x "${MACOS_DIR}/run.sh"

# Keep compatibility with direct calls using the game name.
ln -sf run.sh "${MACOS_DIR}/GeneralsXZH"

# Make the bundle resolve its own libraries, and refuse to ship one that does not: without the relink the .app carries
# LC_RPATHs into the build machine's home and /opt/homebrew dylib paths that only run.sh's DYLD_LIBRARY_PATH masks.
# No escape hatch: nothing legitimately produces a bundle that runs only where it was built. See lib-macho.sh.
source "${SCRIPT_DIR}/lib-macho.sh"
echo ""
echo "  Relinking Mach-O load commands to be bundle-relative..."
gx_macho_relink "${LIB_DIR}" "@loader_path/../lib" "${BIN_DIR}/GeneralsXZH" || exit 1
gx_macho_assert_portable "${APP_DIR}" "${APP_DIR_NAME}" "${BIN_DIR}" || exit 1

# THE ZIP CONTAINS EXACTLY ONE THING: GeneralsXZH.app.
#
# A top-level run.sh used to be written here as a "helper runner for launching the generated .app
# from terminal" - a convenience for whoever was building it, shipped to every player. Measured on
# a real first install: the folder opens showing GeneralsXZH and run.sh, side by side, with nothing
# saying which is which. run.sh LOOKS like the instructions, so it gets opened, and macOS opens a
# .sh with no execute bit in a text editor. That is where a first-time player stops.
#
# Anyone who wants the terminal entry point already has it INSIDE the bundle at
# GeneralsXZH.app/Contents/MacOS/run.sh, which is what the deleted file exec'd anyway. Nothing is
# lost and the folder now has one obvious thing in it.

# A gx-ab-build.sh arm carries its build's provenance into the app: a --flag A/B stages two apps of ONE sha, and only this stamp
# (arm, sha, build dir, the cache's GX_ entries, the bundled binary's md5) lets gx-ab.sh's gate tell them apart.
# Only a dir gx-ab-build.sh stamped: every other bundle is unchanged.
GX_AB_STAMP="${BUILD_DIR}/gx-ab-stamp.txt"
if [[ -e "${GX_AB_STAMP}" ]]; then
    gx_ab_sha="$(sed -n 's/^sha=//p' "${GX_AB_STAMP}")"
    if [[ ! "${gx_ab_sha}" =~ ^[0-9a-f]{40}$ ]] || ! LC_ALL=C /usr/bin/grep -a -q -F "${gx_ab_sha}" "${BIN_DIR}/GeneralsXZH"; then
        echo "ERROR: ${GX_AB_STAMP} names sha '${gx_ab_sha}', which the binary does not carry - the last gx-ab-build.sh of this dir did not PASS" >&2
        exit 1
    fi
    printf '%s\nmd5=%s\n' "$(cat "${GX_AB_STAMP}")" "$(md5 -q "${BIN_DIR}/GeneralsXZH")" > "${RESOURCES_DIR}/gx-ab-arm.txt"
fi

# Create zip
echo ""
echo "Creating ${OUTPUT_ZIP}..."
rm -f "${OUTPUT_ZIP}"
# Make everything owner-writable before archiving. Several vcpkg-provided
# dylibs (libSvtAv1Enc, libavcodec, libx264, libpng16 and friends) arrive mode 444, and zip
# preserves that. On the machine that unpacks the archive, "xattr -dr com.apple.quarantine"
# then fails with EACCES on exactly those files, because removing an extended attribute needs
# write permission. The user is left with a bundle whose FFmpeg and codec libraries are still
# quarantined -- and macOS refuses to load a quarantined dylib, so the app dies at launch
# rather than showing anything useful. Read-only was never load-bearing here; these are copies
# in a staging directory.
chmod -R u+w "${STAGE_DIR}/${APP_DIR_NAME}"

# Payload gate: every library in what was just built must be owned by a component of THIRD_PARTY_NOTICES.txt
# (a new transitive dylib/so otherwise ships with no notice). Fix: add it to resources/legal/third-party and regenerate.
python3 -I "${PROJECT_ROOT}/scripts/legal/third_party_notices.py" check-path --platform macos "${STAGE_DIR}/${APP_DIR_NAME}" || exit 1

(cd "${STAGE_DIR}" && zip -r "${OUTPUT_ZIP}" "${APP_DIR_NAME}" run.sh)

# AND ONCE MORE OUT OF THE FINISHED ZIP.
#
# The staged check above ran against the staging tree; this one runs against the artifact a player
# downloads, so nothing between staging and `zip` - a permissions pass, a re-copy, a stray earlier
# file inside STAGE_DIR - can put an unpatched dylib in the shipped archive unnoticed.
GX_ZIP_VERIFY_DIR="$(mktemp -d)"
# BOTH directories, because `trap ... EXIT` REPLACES the handler installed at STAGE_DIR's mktemp
# rather than adding to it. A second trap naming only this directory silently disables the stage
# cleanup, and every successful packaging run then leaks a ~90 MB tree under $TMPDIR.
trap 'rm -rf "${STAGE_DIR}" "${GX_ZIP_VERIFY_DIR}"' EXIT
# BOTH member names: `zip -r` dereferences the unversioned symlink into a second regular file, so
# the archive carries two independent copies and verifying one says nothing about the other.
for gx_member in libdxvk_d3d9.0.dylib libdxvk_d3d9.dylib; do
    if ! unzip -o -j "${OUTPUT_ZIP}" "*/lib/${gx_member}" -d "${GX_ZIP_VERIFY_DIR}" >/dev/null 2>&1; then
        echo "ERROR: could not extract ${gx_member} back out of ${OUTPUT_ZIP} to verify it." >&2
        exit 1
    fi
    "${SCRIPT_DIR}/verify-dxvk-retina-patch.sh" \
        "${GX_ZIP_VERIFY_DIR}/${gx_member}" "${gx_member} inside $(basename "${OUTPUT_ZIP}")" || exit 1
done

echo ""
echo "Bundle complete: ${OUTPUT_ZIP}"
echo "Contents:"
unzip -l "${OUTPUT_ZIP}" | sed '1,3d;$d'
echo ""
echo "To use locally:"
echo "  1) unzip ${OUTPUT_ZIP}"
echo "  2) run: ./run.sh -win"
echo "  3) or open: open ${APP_DIR_NAME}"
echo ""
echo "Runtime env defaults inside app launcher:"
echo '  CNC_GENERALS_PATH=$HOME/GeneralsVanguard/Generals or $HOME/GeneralsX/Generals'
echo '  CNC_GENERALS_ZH_PATH=<that folder>/GeneralsZH (fallback: <that folder>/GeneralsMD)'
echo '  GENERALSX_PATCH_PATH=<app>/Contents/Resources/GeneralsXPatch'
echo ""
if [[ -f "${RESOURCES_DIR}/OVERLAY-MISSING.txt" ]]; then
    # Read the verdict back off the stamp rather than reprinting one of the two cases from memory.
    # The optional-only arm JOINS games, and telling its builder it cannot is how a warning stops
    # being believed.
    if head -1 "${RESOURCES_DIR}/OVERLAY-MISSING.txt" | grep -qx 'GX_BUNDLE_INCOMPLETE=required'; then
        echo "Community-patch data: NOT SHIPPED - this bundle cannot join any game."
    else
        echo "Community-patch data: MANDATORY SET ONLY - joins games, ships without optional content."
    fi
    echo "  Stamped Contents/Resources/OVERLAY-MISSING.txt. NOT RELEASABLE:"
    echo "  scripts/build/macos/verify-bundle-overlay.sh refuses it, and release.yml runs that."
else
    echo "Community-patch data:"
    echo "  overlay archives -> Contents/Resources/GeneralsXPatch (mounted via GENERALSX_PATCH_PATH)"
    echo "  loose Data/      -> Contents/Resources/gamedata, installed into the game folder at launch"
fi
