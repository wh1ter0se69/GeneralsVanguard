#!/bin/bash
# Make a staged Mach-O bundle resolve its dylibs from itself: strip absolute LC_RPATHs, rewrite deps to @rpath/<leaf>.
# Otherwise it works only via run.sh's DYLD_LIBRARY_PATH, direct execs die at dyld, and a username leaks.
# Done at packaging time, not in CMake (cmake/ and the presets are digested).
# Run directly on an .app it only checks: exit 0 = portable, 1 = carries a path off the machine that built it.

# --- readers ---
# otool -L marks dependency lines with a leading TAB; filter on it (a fat file's slice header starts with a path).
# The readers FAIL CLOSED: they return non-zero when otool does, so callers must capture output and check status.
# Optional <arch> reads one slice, else all slices merge. Paths print whole, one per line (they may contain spaces).

gx_macho_is() {
    [[ -f "$1" && ! -L "$1" ]] || return 1
    file -b "$1" 2>/dev/null | /usr/bin/grep -aq 'Mach-O'
}

# _gx_otool <flag> <file> [arch] - otool's stdout, or an error naming the file and a non-zero status.
_gx_otool() {
    local out
    if ! out="$(otool ${3:+-arch "$3"} "$1" "$2")"; then
        echo "ERROR: otool $1 could not read $2${3:+ ($3)}" >&2
        return 1
    fi
    printf '%s\n' "${out}"
}

gx_macho_deps() {
    local out
    out="$(_gx_otool -L "$1" "${2:-}")" || return 1
    printf '%s\n' "${out}" | awk 'sub(/^\t/, "") { sub(/ \(compatibility version [^)]*\)$/, ""); print }' | sort -u
}

gx_macho_id() {
    # Header lines end in ':'; the id lines do not. Fat files print the id once per architecture.
    local out
    out="$(_gx_otool -D "$1" "${2:-}")" || return 1
    printf '%s\n' "${out}" | awk '!/:$/ && NF && !n++'
}

# One Vulkan loader build under both of its names.
#
# The game binary links @rpath/libvulkan.1.dylib, so SDL3 takes that loader (its RTLD_DEFAULT probe
# finds it already loaded). DXVK dlopens libvulkan.dylib, and DXVK's SDL3 WSI hands its VkInstance to
# SDL's copy in SDL_Vulkan_CreateSurface. On a build Mac whose CMake found Homebrew's loader,
# collect_external_dylibs staged Homebrew's libvulkan.1.dylib read-only (0444). The SDK copy over it
# then failed with "Permission denied" behind `2>/dev/null || true`. So 0.9.61 and 0.9.62 shipped
# Homebrew 1.4.357.0 for SDL next to LunarG SDK 1.4.357.1 for DXVK. That works only while the two
# loaders agree on the layout of an instance (the same release today). Measured:

# gx_macho_uuids <file> - one "<arch> <LC_UUID>" line per slice; fails closed on a slice without one.
gx_macho_uuids() {
    local archs a out u
    archs="$(lipo -archs "$1")" || { echo "ERROR: lipo could not read $1" >&2; return 1; }
    for a in ${archs}; do
        out="$(_gx_otool -l "$1" "${a}")" || return 1
        u="$(printf '%s\n' "${out}" | awk '$1 == "cmd" && $2 == "LC_UUID" { f = 1; next } f && $1 == "uuid" { print $2; exit }')"
        [[ -n "${u}" ]] || { echo "ERROR: $1 (${a}) has no LC_UUID" >&2; return 1; }
        echo "${a} ${u}"
    done
}

# gx_macho_same_build <a> <b> - refuse unless both have the same slices with the same LC_UUIDs.
# LC_UUID survives install_name_tool and codesign, so this holds before and after the relink.
gx_macho_same_build() {
    local ua ub
    ua="$(gx_macho_uuids "$1")" || return 1
    ub="$(gx_macho_uuids "$2")" || return 1
    [[ "${ua}" == "${ub}" ]] && return 0
    echo "REFUSED: $1 and $2 are not the same build" >&2
    echo "         $(basename "$1"): ${ua//$'\n'/, }" >&2
    echo "         $(basename "$2"): ${ub//$'\n'/, }" >&2
    return 1
}

# gx_copy_vulkan_loader <sdk lib dir> <dest dir> - the SDK loader under both names, or a refusal.
# cp -f replaces a read-only file that is already there.
gx_copy_vulkan_loader() {
    local n
    for n in libvulkan.dylib libvulkan.1.dylib; do
        cp -f "$1/${n}" "$2/${n}" || { echo "ERROR: could not copy $1/${n} to $2/" >&2; return 1; }
    done
    gx_macho_same_build "$2/libvulkan.dylib" "$2/libvulkan.1.dylib"
}

gx_macho_rpaths() {
    local out
    out="$(_gx_otool -l "$1" "${2:-}")" || return 1
    printf '%s\n' "${out}" | awk '
        $1 == "cmd" { in_rpath = ($2 == "LC_RPATH"); next }
        in_rpath && $1 == "path" { sub(/^[ \t]*path /, ""); sub(/ \(offset [0-9]+\)$/, ""); print; in_rpath = 0 }
    ' | sort -u
}

# _gx_macho_canon <path> - the path dyld would actually reach: physical (symlinks and ".." resolved
# by the filesystem) for the part that exists, lexical for the rest. A lexical prefix test alone
# passes lib/libfoo.dylib -> /opt/homebrew/... and bin/../../../../elsewhere, both of which begin
# with the bundle's own path. The lexical tail matters for an rpath that exists on a player's Mac
# but not on the checker's: it must still be judged, not skipped.
# Trade-off: a dangling symlink is judged by its own path, not its target; realpath cannot follow it.
_gx_macho_canon() (
    set -f
    p="$1" rest=""
    while [[ ! -e "${p}" ]]; do
        [[ "${p}" == */* ]] || { rest="/${p}${rest}"; p="."; break; }
        rest="/${p##*/}${rest}"
        p="${p%/*}"
        [[ -n "${p}" ]] || p="/"
    done
    out="$(realpath "${p}")" || exit 1
    IFS=/
    for c in ${rest}; do
        case "${c}" in
            ''|.) ;;
            ..)   out="${out%/*}" ;;
            *)    out="${out%/}/${c}" ;;
        esac
    done
    printf '%s\n' "${out:-/}"
)

# A dep we are content to leave absolute. Same definition collect_external_dylibs() uses to decide
# what it does NOT need to stage, so the two cannot drift into disagreeing about one library.
gx_macho_dep_is_system() {
    [[ "$1" == /usr/lib/* || "$1" == /System/Library/* ]]
}

# --- the relink ---
# gx_macho_relink <lib_dir> <exe_rpath> <executable>... - exe_rpath is how the executables reach lib_dir.
# For each Mach-O: drop absolute LC_RPATHs, add one relative one, set LC_ID_DYLIB and staged LC_LOAD_DYLIBs to
# @rpath/<leaf>. Deps not staged are left alone; gx_macho_assert_portable() reports them.
gx_macho_relink() {
    local lib_dir="$1" exe_rpath="$2"
    shift 2
    local staged_names f leaf dep want rp rel

    command -v install_name_tool >/dev/null 2>&1 || {
        echo "ERROR: install_name_tool not on PATH; cannot produce a portable bundle" >&2
        return 1
    }

    # Leaf names of everything in lib_dir INCLUDING symlinks - libSDL3_image.0.dylib is a symlink
    # and is exactly the name dependents ask for, so a map built from regular files only would
    # leave the real dependency edge unrewritten.
    staged_names="$(cd "${lib_dir}" && ls -1 2>/dev/null)"

    _gx_staged() { printf '%s\n' "${staged_names}" | /usr/bin/grep -Fxq -- "$1"; }

    _gx_fix() {
        local target="$1" rpath_wanted="$2" args=()
        local current_id

        # Several vcpkg- and Homebrew-provided dylibs arrive mode 444 and cp preserves that, so
        # install_name_tool cannot open them for writing. The ZH bundler already chmods the whole
        # stage before zipping, for the same reason and one step later; doing it here as well
        # means this function does not silently depend on being called after that step.
        chmod u+w "${target}" 2>/dev/null || true

        local rpaths deps
        rpaths="$(gx_macho_rpaths "${target}")" || return 1
        current_id="$(gx_macho_id "${target}")" || return 1
        deps="$(gx_macho_deps "${target}")" || return 1

        while IFS= read -r rp; do
            [[ -z "${rp}" ]] && continue
            if [[ "${rp}" == /* ]]; then
                args+=(-delete_rpath "${rp}")
            elif [[ "${rp}" == "${rpath_wanted}" ]]; then
                rpath_wanted=""   # already present; adding it again is an error
            fi
        done <<< "${rpaths}" || return 1

        if [[ -n "${current_id}" && "${current_id}" != @* ]]; then
            args+=(-id "@rpath/$(basename "${current_id}")")
        fi

        while IFS= read -r dep; do
            [[ -z "${dep}" ]] && continue
            [[ "${dep}" == @rpath/* ]] && continue
            gx_macho_dep_is_system "${dep}" && continue
            leaf="$(basename "${dep}")"
            _gx_staged "${leaf}" || continue
            want="@rpath/${leaf}"
            [[ "${dep}" == "${want}" ]] && continue
            args+=(-change "${dep}" "${want}")
        done <<< "${deps}" || return 1

        # Deletes first, adds second: -delete_rpath only ever shrinks the load commands, so doing
        # it first is what makes room for the add on a binary whose header padding is tight.
        # stderr is captured rather than discarded: on success it is only the "will invalidate the
        # code signature" warning, which the re-sign below answers, and on failure it is the only
        # thing that says which load command would not fit. Never silence a command whose failure
        # matters - gate on the status AND keep the message.
        local touched=0 err
        if [[ ${#args[@]} -gt 0 ]]; then
            if ! err="$(install_name_tool "${args[@]}" "${target}" 2>&1)"; then
                echo "ERROR: install_name_tool failed on ${target}" >&2
                [[ -n "${err}" ]] && echo "${err}" >&2
                return 1
            fi
            touched=1
        fi
        if [[ -n "${rpath_wanted}" ]]; then
            if ! err="$(install_name_tool -add_rpath "${rpath_wanted}" "${target}" 2>&1)"; then
                echo "ERROR: could not add rpath ${rpath_wanted} to ${target}" >&2
                [[ -n "${err}" ]] && echo "${err}" >&2
                return 1
            fi
            touched=1
        fi

        # Re-sign ad hoc: install_name_tool invalidates arm64 signatures, and an unsigned relinked bundle dies with SIGKILL
        # (exit 137) on exec. Callers that sign with a real identity afterwards (the iOS packager) overwrite it.
        if [[ ${touched} -eq 1 ]]; then
            if ! err="$(codesign --force --sign - --timestamp=none "${target}" 2>&1)"; then
                echo "ERROR: could not ad-hoc re-sign ${target} after relinking" >&2
                [[ -n "${err}" ]] && echo "${err}" >&2
                return 1
            fi
        fi
        return 0
    }

    # Libraries. The rpath each one needs is the way back to lib_dir from its own directory, so a
    # framework binary nested one level deeper (MoltenVK.framework/MoltenVK) still reaches its
    # siblings.
    while IFS= read -r f; do
        gx_macho_is "${f}" || continue
        rel="$(dirname "${f}")"
        rel="${rel#"${lib_dir}"}"
        rel="${rel#/}"
        if [[ -z "${rel}" ]]; then
            _gx_fix "${f}" "@loader_path" || return 1
        else
            # one ".." per path component between lib_dir and this file
            local up="" component
            while IFS= read -r component; do
                [[ -z "${component}" ]] && continue
                up="${up}/.."
            done < <(printf '%s\n' "${rel}" | tr '/' '\n')
            _gx_fix "${f}" "@loader_path${up}" || return 1
        fi
    done < <(find "${lib_dir}" -type f 2>/dev/null)

    # Executables.
    for f in "$@"; do
        gx_macho_is "${f}" || continue
        _gx_fix "${f}" "${exe_rpath}" || return 1
    done

    unset -f _gx_staged _gx_fix
    return 0
}

# --- resolution, without loading anything ---
# gx_macho_resolve <file> <dep> <executable_dir> <arch> - print the path dyld would open for that slice, or nothing.
# Static: reads load commands and the loading image's own rpaths. A candidate must be a regular file with a DYLIB slice.
# Trade-off: exact slice match; an arm64e or x86_64h loader that dyld lets fall back to a plain slice is refused.
_gx_macho_loadable() {
    [[ -f "$1" && -r "$1" ]] && otool -arch "$2" -hv "$1" 2>/dev/null \
        | awk '/^Archive :/ { exit 1 } $5 == "DYLIB" { d = 1 } END { exit !d }'
}

gx_macho_resolve() {
    local file="$1" dep="$2" exe_dir="$3" arch="$4"
    local loader_dir rp candidate
    loader_dir="$(dirname "${file}")"

    case "${dep}" in
        @loader_path/*)     candidate="${loader_dir}/${dep#@loader_path/}"
                            _gx_macho_loadable "${candidate}" "${arch}" && { echo "${candidate}"; return 0; } ;;
        @executable_path/*) candidate="${exe_dir}/${dep#@executable_path/}"
                            _gx_macho_loadable "${candidate}" "${arch}" && { echo "${candidate}"; return 0; } ;;
        @rpath/*)
            local rpaths
            rpaths="$(gx_macho_rpaths "${file}" "${arch}")" || return 1
            while IFS= read -r rp; do
                [[ -z "${rp}" ]] && continue
                rp="${rp//@loader_path/${loader_dir}}"
                rp="${rp//@executable_path/${exe_dir}}"
                candidate="${rp}/${dep#@rpath/}"
                _gx_macho_loadable "${candidate}" "${arch}" && { echo "${candidate}"; return 0; }
            done <<< "${rpaths}" ;;
        /*)                 _gx_macho_loadable "${dep}" "${arch}" && { echo "${dep}"; return 0; } ;;
    esac
    return 1
}

# _gx_macho_flag <line> - report one defect under the current file's FAIL header (printed once).
# Reads and sets the caller's f, root, slice, shown and bad.
_gx_macho_flag() {
    if [[ ${shown} -eq 0 ]]; then echo "  FAIL ${f#"${root}"/}"; shown=1; fi
    echo "       ${slice}$1"
    bad=1
}

# --- the gate ---
# gx_macho_assert_portable <root> [label] [executable_dir] - reports EVERY offending file; run on the finished artifact.
# No LC_RPATH / LC_ID_DYLIB / LC_LOAD_DYLIB may name a path outside the bundle, and every non-system dependency must
# resolve to a loadable Mach-O inside it. Judged per architecture slice, since dyld loads one.
gx_macho_assert_portable() {
    local root="$1" label="${2:-$1}" exe_dir="${3:-$1}"
    local bad=0 seen=0 f dep rp shown own_id resolved rpaths deps canon_root c archs arch slice files

    # Containment is judged on canonical paths, so the root must be canonical too - /var/folders is
    # /private/var/folders on macOS, and a root reached through a symlink would reject everything.
    canon_root="$(_gx_macho_canon "${root}")" || { echo "ERROR: cannot resolve ${root}" >&2; return 1; }
    _gx_inside() { [[ "$1" == "${canon_root}" || "$1" == "${canon_root}"/* ]]; }

    files="$(find "${root}" -type f)" || {
        echo "ERROR: ${label}: could not list every file under ${root} - nothing was proven." >&2
        return 1
    }
    while IFS= read -r f; do
        gx_macho_is "${f}" || continue
        seen=$((seen + 1))
        shown=0 slice=""

        if ! archs="$(lipo -archs "${f}")" || [[ -z "${archs}" ]]; then
            _gx_macho_flag "UNREADABLE lipo could not list its architectures - nothing about it is proven"
            continue
        fi
        for arch in ${archs}; do
            [[ "${archs}" == *" "* ]] && slice="[${arch}] "
            if ! rpaths="$(gx_macho_rpaths "${f}" "${arch}")" || ! own_id="$(gx_macho_id "${f}" "${arch}")" \
                    || ! deps="$(gx_macho_deps "${f}" "${arch}")"; then
                _gx_macho_flag "UNREADABLE otool could not read its load commands - nothing about it is proven"
                continue
            fi

            # Absolute: names a directory on whatever machine loads it. Relative: must stay inside the
            # bundle after dyld's substitution, WHETHER OR NOT IT EXISTS HERE - an rpath into a directory
            # this machine lacks can be the first one dyld finds on a player's. Anything else (a bare
            # relative path) is resolved against the process's cwd, which the bundle does not control.
            while IFS= read -r rp; do
                [[ -z "${rp}" ]] && continue
                case "${rp}" in
                    @loader_path|@loader_path/*)         c="$(dirname "${f}")${rp#@loader_path}" ;;
                    @executable_path|@executable_path/*) c="${exe_dir}${rp#@executable_path}" ;;
                    *) _gx_macho_flag "LC_RPATH  ${rp}"; continue ;;
                esac
                c="$(_gx_macho_canon "${c}")" || c=""
                _gx_inside "${c}" || _gx_macho_flag "LC_RPATH  ${rp}  (leaves the bundle: ${c:-unresolvable})"
            done <<< "${rpaths}" || return 1

            if [[ "${own_id}" == /* ]]; then
                _gx_macho_flag "LC_ID     ${own_id}"
            fi

            # otool -L lists a dylib's own LC_ID_DYLIB as its first entry. It is already reported
            # above; repeating it as a dependency reads as two defects where there is one.
            while IFS= read -r dep; do
                [[ -z "${dep}" ]] && continue
                [[ -n "${own_id}" && "${dep}" == "${own_id}" ]] && continue
                gx_macho_dep_is_system "${dep}" && continue

                if [[ "${dep}" == /* ]]; then
                    _gx_macho_flag "LC_LOAD   ${dep}"
                    continue
                fi

                resolved="$(gx_macho_resolve "${f}" "${dep}" "${exe_dir}" "${arch}" || true)"
                if [[ -z "${resolved}" ]]; then
                    _gx_macho_flag "UNRESOLVED ${dep}  (no LC_RPATH of this image reaches a loadable ${arch} Mach-O)"
                elif ! c="$(_gx_macho_canon "${resolved}")" || ! _gx_inside "${c}"; then
                    _gx_macho_flag "ESCAPES    ${dep} -> ${c:-${resolved}}"
                fi
            done <<< "${deps}" || return 1
        done
    done < <(printf '%s\n' "${files}")   # no check needed: zero iterations is seen=0, refused below
    unset -f _gx_inside

    # Zero Mach-O files is not a portable bundle, it is an unread one: a `file` that cannot identify
    # them (or a root with nothing in it) would otherwise fall straight through to OK.
    if [[ ${seen} -eq 0 ]]; then
        echo "ERROR: ${label}: no Mach-O file found under ${root} - nothing was checked." >&2
        return 1
    fi

    if [[ ${bad} -ne 0 ]]; then
        echo "ERROR: ${label} does not resolve its own libraries." >&2
        echo "       A path outside the bundle only works on the machine that built it: the player" >&2
        echo "       gets a dyld abort the moment anything execs the binary without the wrapper," >&2
        echo "       and an absolute build path ships a username and directory layout in a public" >&2
        echo "       artifact. An UNRESOLVED line means the opposite mistake - the rpath is gone" >&2
        echo "       and nothing in the bundle answers for the library." >&2
        return 1
    fi
    echo "  OK   ${label}: every Mach-O resolves its dependencies inside the bundle."
    return 0
}

# Runnable directly so the same check can be pointed at any .app - one downloaded from the CDN,
# one already installed - without unpacking this file's callers. Nothing is executed and nothing
# is written, so it is safe to run against an install somebody is playing on.
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    if [[ -z "${1:-}" || ! -d "${1}" ]]; then
        echo "usage: $(basename "$0") <path/to/Whatever.app> [executable-dir]" >&2
        exit 2
    fi
    _gx_exe_dir="${2:-}"
    if [[ -z "${_gx_exe_dir}" ]]; then
        # macOS layout first, then the iOS one; fall back to the bundle root.
        if [[ -d "${1}/Contents/Resources/bin" ]]; then
            _gx_exe_dir="${1}/Contents/Resources/bin"
        else
            _gx_exe_dir="${1}"
        fi
    fi
    gx_macho_assert_portable "${1}" "${1}" "${_gx_exe_dir}"
    exit $?
fi
