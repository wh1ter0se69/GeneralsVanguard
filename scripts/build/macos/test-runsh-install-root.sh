#!/usr/bin/env bash
# GeneralsX @test wh1ter0se 11/11/2026 run.sh's game-folder lookup (bundle-macos-zh.sh), run for real
# against a scratch HOME: ~/GeneralsVanguard first, ~/GeneralsX as before.
#   bash scripts/build/macos/test-runsh-install-root.sh [bundle-macos-zh.sh]   exit 0 = every case passes
set -u
SRC="${1:-$(cd "$(dirname "$0")" && pwd)/bundle-macos-zh.sh}"
T="$(mktemp -d "${TMPDIR:-/tmp}/gx-runsh.XXXXXX")"; trap 'rm -rf "$T"' EXIT
# The lookup block of the generated run.sh: from its header comment to the end of the ZH path if-chain.
awk '/<< .WRAPPER.$/ {w=1; next} w && /^WRAPPER$/ {w=0} w' "$SRC" |
	awk '/^# Default asset paths matching the standard macOS deploy layout/ {p=1} p {print} p && /^    else$/ {e=1} p && e && /^fi$/ {exit}' > "$T/lookup.sh"
grep -q 'CNC_GENERALS_ZH_PATH' "$T/lookup.sh" || { echo "ERROR: no lookup block found in $SRC" >&2; exit 2; }
fail=0; n=0
big() { mkdir -p "$1" && echo big > "$1/INIZH.big"; }
other() { mkdir -p "$1" && echo big > "$1/Other.big"; }
check() {  # <name> <expected, relative to HOME> [preset CNC_GENERALS_ZH_PATH] [variable, default CNC_GENERALS_ZH_PATH]
	local got v="${4:-CNC_GENERALS_ZH_PATH}"; n=$((n + 1))
	got="$(env -i HOME="$T/home" PATH="$PATH" ${3:+CNC_GENERALS_ZH_PATH="$3"} bash -c '. "$1"; printf %s "${!2}"' _ "$T/lookup.sh" "$v")"
	if [ "$got" = "$T/home/$2" ] || [ "$got" = "$2" ]; then echo "ok   $1"; else echo "FAIL $1: got $got, want $2"; fail=1; fi
}
reset() { rm -rf "$T/home"; mkdir -p "$T/home"; }
reset; big "$T/home/GeneralsX/GeneralsZH";                check "old folder alone (every install today)" GeneralsX/GeneralsZH
reset; big "$T/home/GeneralsVanguard/GeneralsZH";         check "new folder alone" GeneralsVanguard/GeneralsZH
reset; big "$T/home/GeneralsVanguard/GeneralsZH"; ln -s GeneralsVanguard "$T/home/GeneralsX"
                                                          check "moved, old name a link to the new" GeneralsVanguard/GeneralsZH
reset; mkdir -p "$T/home/GeneralsVanguard/GeneralsZH"; big "$T/home/GeneralsX/GeneralsZH"
                                                          check "an empty new folder does not win over the old game" GeneralsX/GeneralsZH
reset; big "$T/home/GeneralsX/GeneralsMD";                check "old legacy GeneralsMD alone" GeneralsX/GeneralsMD
reset;                                                    check "nothing anywhere: the old default, unchanged" GeneralsX/GeneralsMD
reset; big "$T/home/GeneralsVanguard/GeneralsZH";         check "a caller's CNC_GENERALS_ZH_PATH wins" /elsewhere /elsewhere
reset; other "$T/home/GeneralsVanguard/GeneralsZH"; big "$T/home/GeneralsX/GeneralsZH"
                                                          check "a stray .big without INIZH.big in the new folder does not win" GeneralsX/GeneralsZH
reset; big "$T/home/GeneralsVanguard/GeneralsZH"; : > "$T/home/GeneralsVanguard/GeneralsZH/INIZH.big"; big "$T/home/GeneralsX/GeneralsZH"
                                                          check "an empty INIZH.big in the new folder does not win" GeneralsX/GeneralsZH
# The base game's folder is chosen on its own, not from the Zero Hour folder's root.
reset; big "$T/home/GeneralsVanguard/GeneralsZH"; other "$T/home/GeneralsX/Generals"
                                                          check "base game left under the old name is still used" GeneralsX/Generals "" CNC_GENERALS_PATH
reset; big "$T/home/GeneralsX/GeneralsZH"; other "$T/home/GeneralsVanguard/Generals"
                                                          check "base game under the new name is used" GeneralsVanguard/Generals "" CNC_GENERALS_PATH
reset;                                                    check "no base game anywhere: the old default" GeneralsX/Generals "" CNC_GENERALS_PATH
echo "$n cases"; exit $fail
