#!/bin/bash
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
# GeneralsX @build Claude 11/08/2026 Overridable, because this is the single largest thing the
# repo produces: the packaged .app carries ~2.9 GB of BUNDLED GAME DATA, it is rebuilt from
# scratch on every run (see the rm -rf below), and it filled the boot volume completely on
# 2026-08-11 - to the point where no command could run at all, because even a shell could not
# create its output file.
#
# A symlink here does NOT work: line ~237 does `rm -rf "${OUT_DIR}"` and then mkdir, which
# replaces the link with a real directory on the boot volume again. An env override survives that.
#
# Point it at external storage:  GX_IOS_PACKAGE_DIR=/Volumes/1TB/GeneralsX/ios-package
OUT_DIR="${GX_IOS_PACKAGE_DIR:-${PROJECT_ROOT}/build/ios-package}"
APP_NAME="GeneralsXZH"
IDENTITY="${GX_SIGN_IDENTITY:-Apple Development}"

# Signing/bundle identity.
#
# The defaults below are UPSTREAM's, inherited from the fork, and are wrong for
# everyone else. Getting them wrong fails in two ways, and the second is nasty:
#   * wrong team    -> xcodebuild dies with `No Account for Team "..."`. Loud, fine.
#   * wrong bundle  -> it BUILDS AND INSTALLS, but iOS treats a different bundle id
#                      as a different app with its own container. The assets seeded
#                      into Documents by a previous full install are then invisible,
#                      and a --dev build comes up with no game data — indistinguishable
#                      from "seeding never ran". Hours vanish here.
#
# So: put your values in ios/signing.local.env (gitignored, not shared) once:
#     GX_TEAM_ID=ABCDE12345        <- your 10-char Team ID, developer.apple.com > Membership
#     GX_BUNDLE_ID=com.you.generalszh
# Environment variables still win over that file, and that file wins over the
# upstream defaults.
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

# GeneralsX @bugfix Claude 06/08/2026 Refuse to ship an engine with no Vulkan entry point.
#
# This script only COPIES; it never builds. A build that dropped libMoltenVK.a still links, still
# exits 0, and still produces a plausible binary — it just black-screens on the device with
# "Failed to load Vulkan Portability library", because SDL3's UIKit loader resolves Vulkan through
# dlsym(RTLD_DEFAULT, "vkGetInstanceProcAddr") and its only dylib fallback is "libvulkan.dylib",
# which this bundle does not contain (it ships MoltenVK.framework instead). That shipped once and
# cost a session, and the device is the slowest place in this project to find anything out.
#
# One nm call, checked here rather than in CMake, because the packaged binary is the artifact that
# actually reaches the iPad.
# GeneralsX @bugfix Claude 09/08/2026 THIS GUARD USED TO FIRE BECAUSE THE SYMBOL WAS PRESENT.
#
# It was `nm ... | grep -q ...` under the `set -euo pipefail` at the top of this file. `grep -q`
# exits the moment it matches and closes the pipe; `nm` is then killed by SIGPIPE part-way through
# writing 35,222 symbols, exits 141, and `pipefail` makes the whole pipeline non-zero - which the
# leading `!` turns into "the symbol is missing". A correctly linked binary therefore failed the
# check, and the bigger the symbol table the more reliably it failed.
#
# The failure is perfectly disguised: the error text says MoltenVK was not linked and prescribes a
# rebuild, so the reading is "the build is broken", and rebuilding produces the same result forever.
# Measured: exit 141 with pipefail, exit 0 without, on a binary where `nm | grep -c` finds it.
#
# Capture first, match second. No pipe, nothing to signal.
#
# GeneralsX @bugfix Claude 18/09/2026 ...and the 09/08 fix still piped. It captured nm's output and
# then matched with `printf '%s\n' "${GAME_SYMS}" | grep -q`, which only MOVED the writer from nm to
# printf: grep -q still exits at the match, printf is SIGPIPEd writing the rest (141), and pipefail
# still fails the pipeline. The real symbol sits 145 lines from the end of 35,423, so it raced -
# passed once, then failed 5 of 5 and blocked the iPad install. A here-string has no writer process.
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

# Install target.
#
# Two different identifiers are in play and they are NOT interchangeable:
#   * CoreDevice UUID (8-4-4-4-12), which is what `devicectl list devices` prints
#     and what `devicectl device install` expects.
#   * Hardware UDID (8-16), which is what `xcodebuild -destination id=` expects
#     and what appears in a provisioning profile's ProvisionedDevices.
# gx_ios_select_device resolves both from one listing; see lib-ios-device.sh for why this is
# no longer "the first device devicectl prints" (short version: that was alphabetical order,
# so renaming a device could redirect the install to a different one, silently).
#
# Precedence, highest first: --device, then GX_IOS_DEVICE from the environment, then
# GX_IOS_DEVICE from scripts/test/local.env. The environment value is captured BEFORE the file
# is sourced so it wins regardless of whether local.env writes `GX_IOS_DEVICE=x` or the
# `GX_IOS_DEVICE="${GX_IOS_DEVICE:-x}"` form used by the rest of that file.
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
# GeneralsX @bugfix Claude 02/08/2026
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
# GeneralsX @feature Claude 12/08/2026 The community-patch overlay, and why iOS is not the
# awkward platform it looks like.
#
# There is no environment to set on a sideloaded app, so iOS must use the engine's own default:
# <exe dir>/GeneralsXPatch. On iOS the executable IS the bundle root and the assets are
# <bundle>/GameData, so that default lands on a sibling of the asset root - disjoint, mounted
# exactly once, at the precedence its filename earns. That is the same shape Windows has and the
# one the dataID equality was measured on. No code change is needed here at all.
#
# The archives never ride inside GameData: that would mount them twice. GameData is an allow-list of
# retail files (see below), so none of the overlay archives can reach it, and both lists - mandatory
# and optional, the optional ones under the name their default dictates - are staged explicitly
# into GeneralsXPatch from PATCH_SRC. (17/08/2026: before that, the optional set reached the iPad
# only because a GameData rsync swept it out of an un-migrated Mac folder, and an entry defaulting
# to off would have shipped switched on.)
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

# GeneralsX @bugfix Claude 26/09/2026 GAMEDATA IS AN ALLOW-LIST, NOT A MIRROR OF THE MAC'S GAME FOLDER.
#
# This was `rsync -a` of the whole folder minus a hand-kept exclude list, so anything else the folder
# held shipped to the iPad inside the signed app. The 672e2a199 package carried a hotkey mod on
# neither overlay list (!HotkeysLeikezeZH.big), the owner's keybinds (Data/INI/CommandMap/
# GXUserKeys.ini), an extra Data/INI/INIZH.big, Generals.exe, WorldBuilder.exe, Steam's
# installScript.vdf, a dev-deploy Window/Menus/ExtrasMenu.wnd and absolute symlinks back into the
# build machine's home. No other platform ships any of that: their payloads carry the archives the
# overlay lists name, the files make-weapon-ini.py --list names and the colour pair, and read retail
# data from the player's own install.
#
# iOS has no player install to read, so it bundles the retail data, and only that: the archives and
# loose retail folders of a Steam Zero Hour install, named here, regular files only (a symlink is never
# followed or copied). The overlay archives go to GeneralsXPatch and the recipe files are cut into
# GameData further down, as before. The colour pair comes from the repository, as bundle-macos-zh.sh
# takes it, not from whatever copy a deploy left in the Mac's folder.
# scripts/test/ios-gamedata-listing-test.sh is the check that fails if a stray reaches GameData.
# ponytail: the English Steam layout only; a localised install (SpeechGermanZH.big, ...) stops here
# with a named missing file - add its names when one is packaged.
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
    # one and sign whatever it points at into the app (Codex ios-stray-r1).
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
        # symlink, no hidden file, so Data/Scripts/.git/config or notes.txt cannot ride along (Codex
        # ios-stray-r1, r2). -iname because retail itself ships EA_LOGO.BIK beside *.bik.
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
    # GeneralsX @perf Claude 01/10/2026 A2: the shipped pipeline record (SDL3Main gxSeedDxvkCache copies it into
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

# The overlay goes in either way, and the DEV_MODE difference is only how loudly it complains.
#
# A --dev bundle ships no GameData and reads its assets from Documents, so it is not a peer
# anybody ships - but it IS a peer somebody tries to join a match with, and 49 MB on top of a
# code-only iteration is cheap enough that "the dev build cannot join" is not worth having. So
# it gets the overlay when one is available and a warning when it is not. A full bundle treats
# the same absence as fatal, like the Windows payload and for the same reason: an iPad without
# these plays single-player perfectly and is refused by every peer with DATA_DIFFERS. The device
# is the slowest place in this project to find anything out, and that failure looks like wifi.
#
# The generate step runs FIRST and against PATCH_SRC, not against the bundle. !GXUI.big has to
# exist in the source directory before gx_overlay_stage_optional looks for it there, and building
# it into PATCH_SRC rather than straight into the app means the Mac's own overlay picks up the
# same archive - one copy, one sha, and `probe-dataid.sh --overlay '!GXUI.big'` measures the file
# the iPad actually got. GAME_DATA_SRC is the install the retail archive is read out of.
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
    # GeneralsX @feature Claude 07/11/2026 The shipped maps (scripts/build/overlay-maps.txt), as on macOS and Linux: loose
    # files in GeneralsXPatch/ShippedMaps/<Name>/, inert to the overlay scan ("*.big"). There is no launcher here to copy
    # them into <userdata>/Maps, so the engine does it at start (SDL3GameEngine.cpp, GameClient/GXShippedMaps.h). Fatal
    # like the archives: a bundle that cannot carry the maps the list names is worse than one that says so.
    gx_overlay_stage_shipped_maps "${APP}/GeneralsXPatch" || exit 1
    # GX_OPTIONAL_COUNT was counted where the list was already read, rather than re-derived here.
    # One parse, one place the number can be wrong. (`$(... | grep -c .)` in this position was
    # measured NOT to abort the packager on a zero count despite `set -euo pipefail` - a failing
    # substitution inside an argument is not the command's own status - so this is about not
    # reading the list twice, not about dodging errexit.)
    # GeneralsX @feature Claude 24/09/2026 Data/INI/Weapon.ini (the Toxin Tractor balance change),
    # generated from the CoreINI archive just staged (scripts/tooling/make-weapon-ini.py). It moves
    # dataID, so it is cut here from the staged archive, never taken from the Mac's game folder
    # (GameData's allow-list above does not copy it): a folder that predates it would ship an iPad
    # no other peer accepts.
    #
    # 25/09/2026 (Codex P2): a --dev bundle too. It reads Data/ from the app's Documents
    # (SDL3Main.cpp), which the bundle cannot carry and which a pre-Weapon.ini full install seeded
    # without it, so the file is cut here and copied into Documents at --install below.
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

# POINT THE EXECUTABLE AT THE EMBEDDED FRAMEWORKS - AND DELETE THE ONES THAT POINT AT THIS MAC.
#
# This used to be a bare `install_name_tool -add_rpath "@executable_path/Frameworks" ... || true`,
# which was wrong twice. It ADDED the right rpath but never removed the four the linker wrote in,
# each an absolute path into the build machine's home directory:
#
#     /Users/<builder>/Developer/GeneralsX-work/build/ios-vulkan/_deps/sdl3_image-build
#     /Users/<builder>/Developer/GeneralsX-work/build/ios-vulkan/_deps/sdl3-build
#     /Users/<builder>/Developer/GeneralsX-work/build/ios-vulkan
#     /Users/<builder>/Developer/GeneralsX-work/build/ios-vulkan/_deps/openal_soft-build
#
# Measured on build/ios-package/GeneralsXZH.app 23/09/2026: all four present, plus one on
# Frameworks/libSDL3_image.0.dylib. iOS keeps WORKING because @executable_path/Frameworks is in
# the list too and the dead entries simply miss - so this is the quiet instance. It ships a
# username to a device with no shell wrapper to blame, in an artifact that is signed and handed
# out, and nothing would ever have failed to say so.
#
# And `|| true` meant the add itself could not fail. gx_macho_relink returns non-zero and this
# script stops, which is the point: a packaging step that cannot fail is not a packaging step.
#
# Ordering: this MUST stay above the re-signing block. install_name_tool invalidates a signature;
# the ad-hoc signature gx_macho_relink leaves behind is then overwritten by the real identity
# below, inside-out, exactly as before.
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
            # Weapon.ini and the Alpha Aurora fix files beside it (26/09/2026): the recipe names them all.
            # A Documents seeded before them has no Data/INI/Object/Shared/Weapon/; `copy to` creates missing
            # parent folders (measured 26/09/2026 on an iPad, temporary domain: rc=0, all five levels created).
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
