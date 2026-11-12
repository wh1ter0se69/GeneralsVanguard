#!/bin/bash
# Copyright 2026 GeneralsX contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Package the iOS build of Zero Hour into a signed .app and install it on a device.
#
# Flow:
#   1. xcodegen + xcodebuild produce a provisioning-shell app with automatic signing
#      (bundle id me.ammaar.generalszh, team 7S264298H8, Info.plist with file sharing).
#   2. The stub executable is replaced with the real z_generals engine binary.
#   3. Runtime dylibs (DXVK d3d8/d3d9, SDL3, SDL3_image) are embedded in Frameworks/
#      and their install names rewritten to @rpath.
#   4. Everything is re-signed inside-out with the Apple Development identity and the
#      provisioning profile from step 1, preserving entitlements.
#   5. Optional: install to the selected device via devicectl.
#
# Usage: ./scripts/build/ios/package-ios-zh.sh [--dev] [--install] [--device ID]
#   --dev        skip bundling the 2.7 GB of game assets (code-only iteration)
#   --install    install the packaged app to the selected device
#   --device ID  the install target, as a CoreDevice UUID (8-4-4-4-12) or a hardware UDID
#                (8-16); both are printed by `xcrun devicectl list devices`. Overrides
#                GX_IOS_DEVICE, which in turn overrides GX_IOS_DEVICE in scripts/test/local.env.
#                With none of the three set, the script selects the single paired iPad and
#                refuses to choose when that is not unambiguous - see lib-ios-device.sh.
set -euo pipefail

DEV_MODE=0
DO_INSTALL=0
DEVICE_ARG=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dev)      DEV_MODE=1 ;;
        --install)  DO_INSTALL=1 ;;
        --device)
            # Guard the missing-value case explicitly. `--device --install` would otherwise
            # consume the flag as the device id and then fail far away with "no paired device
            # matches --install", which reads like a device problem and is not one.
            if [[ $# -lt 2 || "${2}" == --* ]]; then
                echo "ERROR: --device needs a CoreDevice UUID or hardware UDID."; exit 1
            fi
            DEVICE_ARG="$2"; shift ;;
        --device=*)
            DEVICE_ARG="${1#--device=}"
            if [[ -z "${DEVICE_ARG}" ]]; then
                echo "ERROR: --device= needs a CoreDevice UUID or hardware UDID."; exit 1
            fi ;;
        *) echo "ERROR: unknown argument '$1' (usage: $0 [--dev] [--install] [--device ID])"; exit 1 ;;
    esac
    shift
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build/ios-vulkan"
IOS_DIR="${PROJECT_ROOT}/ios"
DERIVED="${IOS_DIR}/build"
# Overridable, because this is the single largest thing the repo produces: the packaged .app carries ~2.9 GB of BUNDLED GAME DATA and is rebuilt from scratch on every run (see the rm -rf below), which can fill the boot volume.
#
# A symlink here does NOT work: line ~237 does `rm -rf "${OUT_DIR}"` and then mkdir, which replaces the link with a real directory on the boot volume again. An env override survives that.
#
# Point it at external storage:  GX_IOS_PACKAGE_DIR=/Volumes/1TB/GeneralsX/ios-package
OUT_DIR="${GX_IOS_PACKAGE_DIR:-${PROJECT_ROOT}/build/ios-package}"
APP_NAME="GeneralsXZH"
IDENTITY="${GX_SIGN_IDENTITY:-Apple Development}"

# Signing/bundle identity. The defaults are upstream's and wrong for everyone else: a wrong team fails loudly, but
# a wrong bundle id builds and installs as a different app with its own container, hiding assets seeded into Documents.
# Set GX_TEAM_ID and GX_BUNDLE_ID in ios/signing.local.env (gitignored); the environment wins over it.
SIGNING_ENV="${IOS_DIR}/signing.local.env"
if [[ -f "${SIGNING_ENV}" ]]; then
    # shellcheck disable=SC1090
    set -a; source "${SIGNING_ENV}"; set +a
    echo "==> Signing config from $(basename "${SIGNING_ENV}")"
fi
TEAM_ID="${GX_TEAM_ID:-7S264298H8}"
BUNDLE_ID="${GX_BUNDLE_ID:-me.ammaar.generalszh}"

if [[ "${BUNDLE_ID}" == "me.ammaar.generalszh" ]]; then
    echo "WARNING: using upstream's bundle id (${BUNDLE_ID})."
    echo "  If you have previously installed under a different id, this build gets a"
    echo "  SEPARATE container and will not see assets seeded by that install."
    echo "  Set GX_BUNDLE_ID, or create ${SIGNING_ENV}."
fi

GAME_BIN="${BUILD_DIR}/GeneralsMD/GeneralsXZH.app/GeneralsXZH"
DXVK_BUILD="${BUILD_DIR}/_deps/dxvk-build-macos"

if [[ ! -f "${GAME_BIN}" ]]; then
    echo "ERROR: engine binary not found at ${GAME_BIN} — build the ios-vulkan preset first."
    exit 1
fi

# Refuse to ship an engine with no Vulkan entry point: SDL3's UIKit loader resolves vkGetInstanceProcAddr via
# dlsym(RTLD_DEFAULT), and a build that dropped libMoltenVK.a links but black-screens on the device.
# Capture nm's output first, no pipe: `nm | grep -q` under pipefail fails on SIGPIPE (141).
# Regression test: scripts/test/pipefail-symcheck-test.sh.
GAME_SYMS="$(nm -gU "${GAME_BIN}" 2>/dev/null || true)"
if ! grep -q " _vkGetInstanceProcAddr$" <<< "${GAME_SYMS}"; then
    echo "ERROR: ${GAME_BIN} exports no vkGetInstanceProcAddr."
    echo "  MoltenVK was not linked into the executable, so SDL3 cannot find Vulkan and the app"
    echo "  will black-screen. This is what a plain 'cmake -S . -B build/ios-vulkan' produces:"
    echo "  it drops the preset's Vulkan_* cache entries and the -force_load that depends on them."
    echo "  Fix: cmake --preset ios-vulkan && cmake --build build/ios-vulkan --target z_generals"
    exit 1
fi

# Install target: devicectl wants the CoreDevice UUID, xcodebuild the hardware UDID; gx_ios_select_device resolves both.
# Precedence: --device, then GX_IOS_DEVICE from the environment, then from scripts/test/local.env. The environment value
# is captured before the file is sourced so it wins however local.env writes it.
# shellcheck source=scripts/build/ios/lib-ios-device.sh
. "${SCRIPT_DIR}/lib-ios-device.sh"

GX_IOS_DEVICE_FROM_ENV="${GX_IOS_DEVICE:-}"
LOCAL_ENV="${PROJECT_ROOT}/scripts/test/local.env"
if [[ -f "${LOCAL_ENV}" ]]; then
    # shellcheck disable=SC1090
    . "${LOCAL_ENV}"
fi
DEVICE_REQ="${DEVICE_ARG}"
[[ -n "${DEVICE_REQ}" ]] || DEVICE_REQ="${GX_IOS_DEVICE_FROM_ENV}"
[[ -n "${DEVICE_REQ}" ]] || DEVICE_REQ="${GX_IOS_DEVICE:-}"

# `|| DEVICE_RC=$?` rather than a bare call: this runs under `set -e`, and a non-zero return
# here is a result to act on, not a crash. Captured into a variable rather than tested with a
# bare `$?` in an elif, which reads the status of whatever ran last and is a trap waiting for
# the next edit to add a line above it.
#
# Return 1 is always fatal: an explicitly named target could not be honoured, and quietly
# downgrading that to a generic build would defeat the point of naming it. Return 2 means
# nothing was selected, which is only fatal when installing.
DEVICE_UUID=""
DEVICE_UDID=""
DEVICE_RC=0
gx_ios_select_device "${DEVICE_REQ}" || DEVICE_RC=$?
if [[ "${DEVICE_RC}" -eq 1 ]]; then
    exit 1
fi
if [[ "${DEVICE_RC}" -eq 0 ]]; then
    DEVICE_UUID="${GX_DEVICE_UUID}"
    DEVICE_UDID="${GX_DEVICE_UDID}"
fi

# Registering the device with the developer account requires xcodebuild to be
# pointed at that specific device. With `-destination generic/platform=iOS` the
# provisioning profile is generated (or reused) without the device in its
# ProvisionedDevices list, and the install is then refused by the device.
if [[ -n "${DEVICE_UDID}" ]]; then
    DESTINATION="platform=iOS,id=${DEVICE_UDID}"
    echo "==> Target device: ${GX_DEVICE_NAME} (${GX_DEVICE_TYPE})"
    echo "    hardware UDID ${DEVICE_UDID} / CoreDevice ${DEVICE_UUID}"
else
    DESTINATION="generic/platform=iOS"
    if [[ "${DO_INSTALL}" == "1" ]]; then
        echo "ERROR: --install requested but no install target was selected (see above)."
        exit 1
    fi
    echo "==> No target device; building against a generic destination."
    echo "    NOTE: the resulting profile will not authorise any new device."
fi

echo "==> Generating Xcode project (xcodegen)"
(cd "${IOS_DIR}" && xcodegen generate --quiet)

echo "==> Building provisioning shell app"
# GeneralsX:
# This was 'xcodebuild ... | tail -3', and a pipeline's exit status is its LAST command's. tail
# exits 0 whatever xcodebuild did, so a failed - or SIGTERMed - build reported success and the
# script sailed on. Measured: an interrupted build printed '** BUILD INTERRUPTED **' and
# 'Terminated: 15', and the script still exited 0 with nothing installed on the device.
# ${PIPESTATUS[0]} is the real status; this file is #!/bin/bash so the bash-ism is safe here.
#
# The 'shell app not produced' guard below is NOT a substitute: a stale app from a PREVIOUS run
# satisfies it, so an interrupted build can be laundered into a green by its own leftovers.
set -o pipefail
xcodebuild -project "${IOS_DIR}/${APP_NAME}.xcodeproj" \
    -scheme "${APP_NAME}" -configuration Release \
    -destination "${DESTINATION}" \
    -derivedDataPath "${DERIVED}" \
    DEVELOPMENT_TEAM="${TEAM_ID}" \
    PRODUCT_BUNDLE_IDENTIFIER="${BUNDLE_ID}" \
    -allowProvisioningUpdates build | tail -3
xcb_rc=${PIPESTATUS[0]}
set +o pipefail
if [[ ${xcb_rc} -ne 0 ]]; then
    echo "ERROR: xcodebuild failed (exit ${xcb_rc}). Nothing was installed."
    echo "  If this says 'BUILD INTERRUPTED' / 'Terminated: 15', something SIGTERMed it."
    echo "  Note: 'pkill -f GeneralsXZH' matches this build too - xcodebuild's own command line"
    echo "  carries GeneralsXZH.xcodeproj and -scheme GeneralsXZH. Never run that pattern while"
    echo "  an iOS package is in flight. 'pkill -x GeneralsXZH' is no better: it ends every game on"
    echo "  this Mac, other sessions' included. Stop your own game by its pid."
    exit "${xcb_rc}"
fi

SHELL_APP="${DERIVED}/Build/Products/Release-iphoneos/${APP_NAME}.app"
if [[ ! -d "${SHELL_APP}" ]]; then
    echo "ERROR: shell app not produced at ${SHELL_APP}"
    exit 1
fi

echo "==> Assembling final app"
rm -rf "${OUT_DIR}"
mkdir -p "${OUT_DIR}"
cp -R "${SHELL_APP}" "${OUT_DIR}/"
APP="${OUT_DIR}/${APP_NAME}.app"

# Replace stub executable with the engine
cp "${GAME_BIN}" "${APP}/${APP_NAME}"
# GPL-3 + EA's additional terms (LICENSE.md: "Any propagation or conveyance of this program must include this
# copyright notice and these terms"): every package carries the repo's LICENSE.md.
cp "${PROJECT_ROOT}/LICENSE.md" "${APP}/LICENSE.md"
# Third-party licence notices (MIT/BSD/zlib/Apache/LGPL/GPL components we bundle or link): generated by
# scripts/legal/third_party_notices.py, CI-style check: `third_party_notices.py check` and `check-packagers`.
# Freshness gate: refuse to package a stale or incomplete notices file (Cargo.lock / licence data moved without regenerating).
python3 -I "${PROJECT_ROOT}/scripts/legal/third_party_notices.py" check || exit 1
cp "${PROJECT_ROOT}/THIRD_PARTY_NOTICES.txt" "${APP}/THIRD_PARTY_NOTICES.txt" || exit 1

# Embed runtime dylibs
mkdir -p "${APP}/Frameworks"
for lib in \
    "${DXVK_BUILD}/src/d3d8/libdxvk_d3d8.0.dylib" \
    "${DXVK_BUILD}/src/d3d9/libdxvk_d3d9.0.dylib" \
    "${BUILD_DIR}/_deps/sdl3-build/libSDL3.0.dylib" \
    "${BUILD_DIR}/_deps/sdl3_image-build/libSDL3_image.0.dylib" \
    "${BUILD_DIR}/_deps/openal_soft-build/libopenal.1.24.2.dylib" \
    "${BUILD_DIR}/libgamespy.dylib"; do
    if [[ -f "${lib}" ]]; then
        cp "${lib}" "${APP}/Frameworks/"
        echo "    embedded $(basename "${lib}")"
    else
        case "$(basename "${lib}")" in
            libgamespy.dylib)
                echo "    (skip, optional: $(basename "${lib}"))" ;;
            *)
                echo "ERROR: required dylib not built: ${lib}"
                echo "  Build first: cmake --preset ios-vulkan && cmake --build build/ios-vulkan --target z_generals"
                exit 1 ;;
        esac
    fi
done

# openal-soft's install name is libopenal.1.dylib; the embedded file must match it
if [[ -f "${APP}/Frameworks/libopenal.1.24.2.dylib" ]]; then
    mv "${APP}/Frameworks/libopenal.1.24.2.dylib" "${APP}/Frameworks/libopenal.1.dylib"
fi

# MoltenVK: DXVK dlopens @executable_path/Frameworks/MoltenVK.framework/MoltenVK.
# An app without it launches and dies at Vulkan init, so missing framework is fatal.
# (This used to live in /tmp, which the OS periodically cleans — hence the hard error.)
MVK_FRAMEWORK="${GX_MOLTENVK:-${HOME}/GeneralsX/MoltenVK/MoltenVK/MoltenVK/dynamic/MoltenVK.xcframework/ios-arm64/MoltenVK.framework}"
# -f on the DRIVER, not -d on the directory. DXVK dlopens the binary, so an empty or half-extracted
# MoltenVK.framework directory satisfied `-d`, printed "embedded MoltenVK.framework", and shipped an
# .app that dies at Vulkan init - the exact outcome the hard error below exists to prevent.
if [[ -f "${MVK_FRAMEWORK}/MoltenVK" ]]; then
    cp -R "${MVK_FRAMEWORK}" "${APP}/Frameworks/"
    echo "    embedded MoltenVK.framework"
else
    echo "ERROR: MoltenVK.framework not found at ${MVK_FRAMEWORK}"
    echo "  Run scripts/build/ios/fetch-moltenvk.sh (pinned version + checksum)."
    
    exit 1
fi

# Game assets inside the bundle (iOS-sanctioned home for read-only resources):
# the app is fully self-contained, nothing lives in Documents. Skip with --dev
# for fast code-only iterations (the engine falls back to Documents assets).
GAME_DATA_SRC="${GX_GAME_DATA:-${HOME}/GeneralsX/GeneralsZH}"
FONTS_SRC="${GX_FONTS:-${HOME}/GeneralsX/ios-staging/fonts}"
CONFIG_SRC="${GX_CONFIG:-${IOS_DIR}/config}"
# Community-patch overlay on iOS: the engine's default <exe dir>/GeneralsXPatch, a sibling of the asset root.
# The archives never go inside GameData (that would mount them twice); they are staged from PATCH_SRC.
# shellcheck source=scripts/build/lib-overlay.sh
source "${PROJECT_ROOT}/scripts/build/lib-overlay.sh"
PATCH_SRC="${GX_PATCH_SRC:-${HOME}/GeneralsX/GeneralsXPatch}"
# Captured into a variable rather than read from a process substitution, so a failure to parse the
# list stops the packager instead of shipping an overlay short of its optional archives.
GX_OPTIONAL_NAMES="$(gx_overlay_optional_names)" || {
    echo "ERROR: cannot read the optional overlay list; refusing to build a bundle whose"
    echo "  GeneralsXPatch could not be checked against it."
    exit 1
}
GX_OPTIONAL_COUNT=0
while IFS= read -r _a; do
    [[ -n "${_a}" ]] || continue
    GX_OPTIONAL_COUNT=$((GX_OPTIONAL_COUNT + 1))
done <<< "${GX_OPTIONAL_NAMES}"

# GameData is an allow-list, not a mirror of the Mac's game folder: a bulk copy shipped stray files and symlinks.
# iOS has no player install, so it bundles retail archives and loose folders (regular files only).
# scripts/test/ios-gamedata-listing-test.sh fails if a stray reaches GameData. English Steam layout only.
if [[ "${DEV_MODE}" != "1" ]]; then
    echo "==> Bundling game assets into the app"
    mkdir -p "${APP}/GameData"
    GX_IOS_RETAIL_ARCHIVES=(
        AudioEnglishZH.big AudioZH.big EnglishZH.big gensecZH.big INIZH.big MapsZH.big Music.big MusicZH.big
        PatchData.big PatchINI.big PatchWindow.big PatchZH.big ShadersZH.big SpeechEnglishZH.big SpeechZH.big
        TerrainZH.big TexturesZH.big W3DEnglishZH.big W3DZH.big WindowZH.big
        ZH_Generals/Audio.big ZH_Generals/AudioEnglish.big ZH_Generals/English.big ZH_Generals/gensec.big
        ZH_Generals/INI.big ZH_Generals/maps.big ZH_Generals/Music.big ZH_Generals/Patch.big
        ZH_Generals/shaders.big ZH_Generals/Speech.big ZH_Generals/SpeechEnglish.big ZH_Generals/Terrain.big
        ZH_Generals/Textures.big ZH_Generals/W3D.big ZH_Generals/Window.big
    )
    # Each retail folder is flat and holds one kind of file: the folder, then the names it may hold.
    GX_IOS_RETAIL_DIRS=(
        "Data/Cursors *.ani" "Data/English/Movies *.bik" "Data/Movies *.bik" "Data/Scripts *.scb Scripts.ini"
        "Data/WaterPlane *.tga" "ZH_Generals/Data/Cursors *.ani" "ZH_Generals/Data/Movies *.bik"
        "ZH_Generals/Data/Scripts *.scb" "ZH_Generals/Data/WaterPlane *.tga"
    )
    GX_IOS_COLOUR_PAIR=(Data/INI/Multiplayer.ini Data/English/Generals.csf)
    # A named path is staged only if no part of it below GAME_DATA_SRC is a symlink: cp would follow
    # one and sign whatever it points at into the app.
    gx_ios_refuse_link() {
        local _p="" _c
        while IFS= read -r _c; do
            _p="${_p:+${_p}/}${_c}"
            if [[ -L "${GAME_DATA_SRC}/${_p}" ]]; then
                echo "ERROR: ${GAME_DATA_SRC}/${_p} is a symlink; the app takes retail data from real files only."
                exit 1
            fi
        done < <(tr '/' '\n' <<< "$1")
    }
    for _f in "${GX_IOS_RETAIL_ARCHIVES[@]}"; do
        gx_ios_refuse_link "${_f}"
        if [[ ! -f "${GAME_DATA_SRC}/${_f}" ]]; then
            echo "ERROR: retail ${_f} not found in ${GAME_DATA_SRC} - the app would lack game data."
            echo "  Point GX_GAME_DATA at a complete Zero Hour install (Steam layout)."
            exit 1
        fi
        mkdir -p "${APP}/GameData/$(dirname "${_f}")"
        cp "${GAME_DATA_SRC}/${_f}" "${APP}/GameData/${_f}"
    done
    for _e in "${GX_IOS_RETAIL_DIRS[@]}"; do
        read -r -a _w <<< "${_e}"
        _d="${_w[0]}"
        _names=()
        for _n in "${_w[@]:1}"; do _names+=(-o -iname "${_n}"); done
        gx_ios_refuse_link "${_d}"
        # /usr/bin/find by path: this machine's PATH has a different `find` ahead of it (see
        # bundle-macos-zh.sh). Only this folder's own regular files of its retail kinds: no descent, no
        # symlink, no hidden file, so Data/Scripts/.git/config or notes.txt cannot ride along.
        # -iname because retail itself ships EA_LOGO.BIK beside *.bik.
        _list="$(cd "${GAME_DATA_SRC}" && /usr/bin/find "${_d}" -mindepth 1 -maxdepth 1 -type f ! -name '.*' \
            \( "${_names[@]:1}" \) -print)" || _list=""
        if [[ -z "${_list}" ]]; then
            echo "ERROR: retail folder ${_d} is missing or holds no ${_w[*]:1} in ${GAME_DATA_SRC}."
            exit 1
        fi
        rsync -a --files-from=<(printf '%s\n' "${_list}") "${GAME_DATA_SRC}/" "${APP}/GameData/"
    done
    for _f in "${GX_IOS_COLOUR_PAIR[@]}"; do
        if [[ ! -f "${PROJECT_ROOT}/GeneralsZH/${_f}" ]]; then
            echo "ERROR: ${PROJECT_ROOT}/GeneralsZH/${_f} missing (it ships with the repo)"
            exit 1
        fi
        mkdir -p "${APP}/GameData/$(dirname "${_f}")"
        cp "${PROJECT_ROOT}/GeneralsZH/${_f}" "${APP}/GameData/${_f}"
    done
    if [[ -d "${FONTS_SRC}" ]]; then
        mkdir -p "${APP}/GameData/fonts"
        cp "${FONTS_SRC}"/*.ttf "${APP}/GameData/fonts/"
    else
        echo "ERROR: fonts not staged at ${FONTS_SRC} — the app would render no text."
        echo "  Run scripts/build/ios/stage-fonts.sh once (downloads Liberation fonts and"
        echo "  renames them to the arial/couriernew/timesnewroman names the game expects)."
        exit 1
    fi
    for cfg in dxvk.conf Options.ini; do
        if [[ ! -f "${CONFIG_SRC}/${cfg}" ]]; then
            echo "ERROR: ${CONFIG_SRC}/${cfg} missing (should ship with the repo in ios/config/)"
            exit 1
        fi
    done
    cp "${CONFIG_SRC}/dxvk.conf" "${APP}/GameData/dxvk.conf"
    cp "${CONFIG_SRC}/Options.ini" "${APP}/GameData/DefaultOptions.ini"
    # A2: the shipped pipeline record (SDL3Main gxSeedDxvkCache copies it into
    # Library/Caches when absent).
    # Fails rather than skips, as bundle-macos-zh.sh does.
    if [[ ! -s "${PROJECT_ROOT}/resources/dxvk/seed/GeneralsXZH.dxvk-cache" || ! -s "${PROJECT_ROOT}/resources/dxvk/seed/GeneralsXZH.dxvk-ffkeys" ]]; then
        echo "ERROR: resources/dxvk/seed lacks GeneralsXZH.dxvk-cache / .dxvk-ffkeys (the A2 pipeline seed)."
        exit 1
    fi
    mkdir -p "${APP}/GameData/dxvk-seed"
    cp "${PROJECT_ROOT}/resources/dxvk/seed/GeneralsXZH.dxvk-cache" "${PROJECT_ROOT}/resources/dxvk/seed/GeneralsXZH.dxvk-ffkeys" "${APP}/GameData/dxvk-seed/"
    echo "    bundled $(du -sh "${APP}/GameData" | cut -f1) of game data"

fi

# The overlay is staged in both modes: a --dev bundle warns when it is missing, a full bundle fails (without it
# every peer refuses the iPad with DATA_DIFFERS). The generate step runs first, against PATCH_SRC, so !GXUI.big
# exists before the optional staging looks for it. GAME_DATA_SRC is where the retail archive is read from.
if [[ -d "${PATCH_SRC}" ]] && ! gx_overlay_generate "${PATCH_SRC}" "${GAME_DATA_SRC}"; then
    echo "ERROR: could not build the generated overlay archives into ${PATCH_SRC}."
    exit 1
fi
if gx_overlay_verify "${PATCH_SRC}"; then
    mkdir -p "${APP}/GeneralsXPatch"
    while IFS= read -r _a; do
        cp "${PATCH_SRC}/${_a}" "${APP}/GeneralsXPatch/${_a}" || exit 1
    done < <(gx_overlay_archives)
    # Optional content ships too, under the name its default dictates. Fatal on a missing source
    # for the same reason the Windows payload is: an absent archive cannot be switched on later,
    # and on iOS there is no launcher to switch anything at all - what lands in the bundle is what
    # the player has forever.
    if ! gx_overlay_stage_optional "${PATCH_SRC}" "${APP}/GeneralsXPatch" > /dev/null; then
        exit 1
    fi
    # The shipped maps (scripts/build/overlay-maps.txt), as on macOS and Linux: loose
    # files in GeneralsXPatch/ShippedMaps/<Name>/, inert to the overlay scan ("*.big"). There is no launcher here to copy
    # them into <userdata>/Maps, so the engine does it at start (SDL3GameEngine.cpp, GameClient/GXShippedMaps.h). Fatal
    # like the archives: a bundle that cannot carry the maps the list names is worse than one that says so.
    gx_overlay_stage_shipped_maps "${APP}/GeneralsXPatch" || exit 1
    # Data/INI/Weapon.ini (Toxin Tractor balance change) is generated from the staged CoreINI archive by make-weapon-ini.py.
    # It moves dataID, so it is never taken from the Mac's game folder (a stale one would ship an iPad no peer accepts).
    # A --dev bundle reads Data/ from Documents, so the file is also copied into Documents/Data/INI at --install.
    WEAPON_INI_OUT="${APP}/GameData/Data/INI/Weapon.ini"
    [[ "${DEV_MODE}" != "1" ]] || WEAPON_INI_OUT="${OUT_DIR}/Documents/Data/INI/Weapon.ini"
    python3 "${PROJECT_ROOT}/scripts/tooling/make-weapon-ini.py" --search "${APP}/GeneralsXPatch" \
        --game-dir "${GAME_DATA_SRC}" --out "${WEAPON_INI_OUT}" || exit 1
    echo "==> Overlay: $(du -sh "${APP}/GeneralsXPatch" | cut -f1) at the bundle root ($(gx_overlay_count) required + ${GX_OPTIONAL_COUNT} optional)"
    while IFS= read -r _o; do
        [[ -n "${_o}" ]] || continue
        printf '    [%s] %s\n' "$(gx_overlay_optional_default "${_o}")" "$(gx_overlay_optional_label "${_o}")"
    done < <(gx_overlay_optional)
elif [[ "${DEV_MODE}" == "1" ]]; then
    echo "WARNING: no overlay at ${PATCH_SRC} - this --dev bundle will be refused by every peer."
    echo "  Set GX_PATCH_SRC, or run scripts/build/migrate-patch-overlay.sh once."
else
    echo "  Set GX_PATCH_SRC, or run scripts/build/migrate-patch-overlay.sh once."
    exit 1
fi

# Loose icon PNGs alongside the compiled asset catalog: SpringBoard on some
# iOS versions won't read Assets.car icons from developer-signed sideloads
# until a reboot, but it always honors CFBundleIconFiles PNGs in the bundle root.
ICON_SRC="${IOS_DIR}/Stub/Assets.xcassets/AppIcon.appiconset/icon.png"
if [[ -f "${ICON_SRC}" ]]; then
    sips -z 120 120 "${ICON_SRC}" --out "${APP}/AppIcon60x60@2x.png"  >/dev/null
    sips -z 152 152 "${ICON_SRC}" --out "${APP}/AppIcon76x76@2x.png"  >/dev/null
    sips -z 167 167 "${ICON_SRC}" --out "${APP}/AppIcon83.5x83.5@2x.png" >/dev/null
    echo "    icon PNG fallbacks added"
fi

# Point the executable at the embedded frameworks and delete the rpaths that point at this Mac: the linker wrote
# absolute paths into the build machine's home directory, which would put a username in a signed artifact.
# This MUST stay above the re-signing block: install_name_tool invalidates signatures.
source "${PROJECT_ROOT}/scripts/build/macos/lib-macho.sh"
echo "==> Relinking Mach-O load commands to be bundle-relative"
gx_macho_relink "${APP}/Frameworks" "@executable_path/Frameworks" "${APP}/${APP_NAME}" || exit 1
gx_macho_assert_portable "${APP}" "${APP_NAME}.app" "${APP}" || exit 1

echo "==> Re-signing"
ENTITLEMENTS="${OUT_DIR}/entitlements.plist"
codesign -d --entitlements - --xml "${SHELL_APP}" > "${ENTITLEMENTS}" 2>/dev/null

for f in "${APP}/Frameworks/"*.dylib; do
    [[ -f "$f" ]] && codesign --force --sign "${IDENTITY}" --timestamp=none "$f"
done
if [[ -d "${APP}/Frameworks/MoltenVK.framework" ]]; then
    codesign --force --sign "${IDENTITY}" --timestamp=none "${APP}/Frameworks/MoltenVK.framework"
fi
codesign --force --sign "${IDENTITY}" --timestamp=none \
    --entitlements "${ENTITLEMENTS}" "${APP}"

codesign --verify --deep "${APP}" && echo "    signature OK"

# Payload gate: every library in the signed app must be owned by a component of THIRD_PARTY_NOTICES.txt
# (a new transitive dylib otherwise ships with no notice). Fix: add it to resources/legal/third-party and regenerate.
python3 -I "${PROJECT_ROOT}/scripts/legal/third_party_notices.py" check-path --platform ios "${APP}" || exit 1

echo "==> App ready: ${APP}"
if [[ "${DEV_MODE}" == "1" && "${DO_INSTALL}" != "1" && -f "${OUT_DIR}/Documents/Data/INI/Weapon.ini" ]]; then
    echo "    --dev: copy these from ${OUT_DIR}/Documents/ into the app's Documents/, same paths"
    echo "    (--install does it), or this build is refused by every peer with DATA_DIFFERS:"
    python3 "${PROJECT_ROOT}/scripts/tooling/make-weapon-ini.py" --list | sed 's/^/      /'
fi

if [[ "${DO_INSTALL}" == "1" ]]; then
    echo "==> Installing to ${GX_DEVICE_NAME} (${GX_DEVICE_TYPE}, CoreDevice ${DEVICE_UUID})"
    # Install by CoreDevice UUID — the hardware UDID above is what xcodebuild
    # provisioned against, so the profile in the bundle lists this same device.
    xcrun devicectl device install app --device "${DEVICE_UUID}" "${APP}"
    if [[ "${DEV_MODE}" == "1" ]]; then
        if [[ -f "${OUT_DIR}/Documents/Data/INI/Weapon.ini" ]]; then
            # Weapon.ini and the Alpha Aurora fix files beside it: the recipe names them all.
            # A Documents seeded before them has no Data/INI/Object/Shared/Weapon/; `copy to` creates missing
            # parent folders.
            while IFS= read -r _gx_rf; do
                echo "==> Copying ${_gx_rf} into the app's Documents (a --dev build reads Data/ there)"
                if ! xcrun devicectl device copy to --device "${DEVICE_UUID}" --domain-type appDataContainer \
                        --domain-identifier "${BUNDLE_ID}" --source "${OUT_DIR}/Documents/${_gx_rf}" \
                        --destination "Documents/${_gx_rf}"; then
                    echo "ERROR: could not copy ${_gx_rf} into the app's Documents. Without it this"
                    echo "  --dev build is refused by every peer with DATA_DIFFERS. Copy"
                    echo "  ${OUT_DIR}/Documents/${_gx_rf} into Documents/$(dirname "${_gx_rf}")/ via Finder."
                    exit 1
                fi
            done < <(python3 "${PROJECT_ROOT}/scripts/tooling/make-weapon-ini.py" --list)
        fi
        echo "==> Installed (--dev). Copy game assets, fonts/ and dxvk.conf into the"
        echo "    app's Documents via Finder file sharing — a --dev bundle ships none"
        echo "    of them, and without fonts/ the game renders no text."
    else
        echo "==> Installed. Assets are bundled; nothing further to copy."
    fi
fi
