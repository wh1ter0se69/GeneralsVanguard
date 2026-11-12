#!/bin/bash
# Copyright 2026 GeneralsX contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Ship the openal-soft this build compiled, not the distro's.
#
# Sourced by the Linux deploy and tarball scripts. cmake/openal.cmake FetchContents openal-soft
# 1.24.2 on EVERY platform, patches it (openal-startsources-guard.cmake) and links the engine to
# it - but on Linux nothing ever copied libopenal.so.1 next to the binary. The engine therefore ran
# on whatever libopenal.so.1 the player's distro had, or failed to start without one: none of our
# OpenAL fixes, and on an old distro no ALC_SOFT_system_events / ALC_SOFT_reopen_device, so the
# follow-the-default-output-device code in OpenALAudioManager logs "off" and does nothing.
# macOS, iOS and Windows have always carried the library they were built against.
#
# REFUSES rather than stages a library that cannot do the job:
#   - without the PulseAudio, PipeWire and ALSA backends. openal-soft enables each one only when
#     its headers are found at configure time and says nothing when they are not; the result
#     plays through OSS or not at all, and only the Pulse and PipeWire backends report default
#     device changes. config_backends.h is the configure summary in machine-readable form.
#     The backends dlopen their system libraries at run time (ALSOFT_DLOPEN is on by default),
#     so enabling all three adds no hard dependency for a player who has only one of them.
#   - without the GeneralsX patch in the source it was compiled from, which is the fix this whole
#     exercise exists to ship.
#
# gx_openal_stage <build dir> <dest dir> [<licence dir>]
#   Copies libopenal.so.1 and the file it names into <dest dir>. With <licence dir>, also the
#   upstream licence texts and a NOTICE.txt saying where the source is - required when the
#   library is REDISTRIBUTED (the tarball); a deploy onto the build host's own runtime is not.
#   ON A REFUSAL <dest dir> is left holding NO libopenal.so.1*: a redeploy into a runtime an
#   earlier deploy stocked would otherwise keep that older copy - and $ORIGIN/LD_LIBRARY_PATH
#   would load it - while the caller reports the system's copy. Unlinking is safe under a
#   running game; the mapped inode lives until it exits.
gx_openal_stage() {
    _gx_openal_stage "$@" && return 0
    rm -f "$2/libopenal.so.1" "$2"/libopenal.so.1.*
    return 1
}
_gx_openal_stage() {
    local build="$1" dest="$2" lic="${3:-}"
    local libdir="${build}/_deps/openal_soft-build" srcdir="${build}/_deps/openal_soft-src"
    local cfg="${libdir}/config_backends.h" b real ver f

    if [[ ! -f "${libdir}/libopenal.so.1" ]]; then
        echo "ERROR: ${libdir}/libopenal.so.1 not found - build the engine first (cmake/openal.cmake builds it)." >&2
        return 1
    fi
    if [[ ! -f "${cfg}" ]]; then
        echo "ERROR: ${cfg} not found - cannot tell which audio backends openal-soft was built with." >&2
        return 1
    fi
    for b in PULSEAUDIO PIPEWIRE ALSA; do
        if ! grep -Eq "^#define HAVE_${b} 1\$" "${cfg}"; then
            echo "ERROR: openal-soft was configured without its ${b} backend (${cfg})." >&2
            echo "  Install libpulse-dev libpipewire-0.3-dev libasound2-dev (or your distro's" >&2
            echo "  equivalents), then reconfigure and rebuild." >&2
            return 1
        fi
    done
    if ! grep -q "GeneralsX: StartSources voice guard" "${srcdir}/al/source.cpp" 2>/dev/null; then
        echo "ERROR: ${srcdir}/al/source.cpp lacks the GeneralsX StartSources guard - this openal-soft" >&2
        echo "  was not built from cmake/openal.cmake's patched source. Delete ${build}/_deps/openal_soft-*" >&2
        echo "  and rebuild." >&2
        return 1
    fi
    for f in COPYING BSD-3Clause LICENSE-pffft; do
        if [[ -n "${lic}" && ! -s "${srcdir}/${f}" ]]; then
            echo "ERROR: ${srcdir}/${f} missing - refusing to redistribute openal-soft without its licence." >&2
            return 1
        fi
    done

    # The SONAME link and the versioned file it points at - what the loader opens, nothing more.
    real="$(readlink -f "${libdir}/libopenal.so.1")" || return 1
    cp -a "${libdir}/libopenal.so.1" "${dest}/" || return 1
    if [[ -L "${libdir}/libopenal.so.1" ]]; then
        cp -a "${real}" "${dest}/" || return 1
    fi
    if [[ ! -f "${dest}/libopenal.so.1" ]]; then
        echo "ERROR: ${dest}/libopenal.so.1 does not resolve to a file after the copy." >&2
        return 1
    fi
    echo "  + libopenal.so.1 -> $(basename "${real}") (PulseAudio, PipeWire, ALSA)"

    [[ -n "${lic}" ]] || return 0
    mkdir -p "${lic}" || return 1
    for f in COPYING BSD-3Clause LICENSE-pffft; do
        cp "${srcdir}/${f}" "${lic}/" || return 1
    done
    ver="$(basename "${real}")"
    ver="${ver#libopenal.so.}"
    cat > "${lic}/NOTICE.txt" << EOF_NOTICE || return 1
OpenAL Soft ${ver} - libopenal.so.1 in the game folder

GeneralsX ships this build of OpenAL Soft so the game does not depend on the copy your
distribution provides. It is licensed under the GNU Library General Public License, version 2
(COPYING), with portions under the BSD 3-Clause licence (BSD-3Clause) and the PFFFT licence
(LICENSE-pffft).

Source: the upstream openal-soft ${ver} release tarball, pinned by URL and SHA-256 in
cmake/openal.cmake of the GeneralsX source, with one GeneralsX change applied at build time by
cmake/openal-startsources-guard.cmake (al/source.cpp, StartSources: do not dereference a null
voice). Both files are part of the GeneralsX source release.

The game loads libopenal.so.1 from its own folder, so you may replace that file with any
compatible build of the library.
EOF_NOTICE
    echo "  + $(basename "${lic}")/ (openal-soft licence texts + NOTICE.txt)"
}

# gx_origin_runpath <binary>
#   Sets the binary's runpath to exactly $ORIGIN, so it loads the libraries shipped beside it -
#   libopenal.so.1 above all - even when started without run.sh's LD_LIBRARY_PATH. A CMake
#   build-tree binary carries the build tree's own absolute library directories instead, which
#   exist on no player's machine, so a direct launch falls through to the distro's libopenal.so.1.
#   For the tarball only: the harness runs build-tree binaries in place and relies on that runpath.
gx_origin_runpath() {
    local bin="$1" got
    if ! command -v patchelf > /dev/null 2>&1; then
        echo "ERROR: patchelf is not installed (apt install patchelf) - cannot point ${bin} at the libraries beside it." >&2
        return 1
    fi
    # shellcheck disable=SC2016 # a literal $ORIGIN is the point: the loader expands it, not the shell
    patchelf --set-rpath '$ORIGIN' "${bin}" || return 1
    got="$(patchelf --print-rpath "${bin}")" || return 1
    # shellcheck disable=SC2016
    if [[ "${got}" != '$ORIGIN' ]]; then
        echo "ERROR: ${bin} has runpath '${got}' after patchelf, expected \$ORIGIN." >&2
        return 1
    fi
    echo "  + runpath \$ORIGIN on $(basename "${bin}")"
}
