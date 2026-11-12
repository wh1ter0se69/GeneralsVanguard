#!/bin/sh
# GeneralsX @test wh1ter0se 11/11/2026 GeneralsX.iss after the rename: the player sees Generals Vanguard, and
# nothing an install, an updater or an uninstall entry is keyed on moved. Static, so any host can run it.
#   sh scripts/build/windows/installer/test-installer-names.sh [GeneralsX.iss]   exit 0 = every check passes
set -u
ISS="${1:-$(cd "$(dirname "$0")" && pwd)/GeneralsX.iss}"
fail=0; n=0
ok()  { n=$((n + 1)); echo "ok   $1"; }
bad() { n=$((n + 1)); echo "FAIL $1"; fail=1; }
has() { grep -Fqx -- "$1" "$ISS"; }          # an exact whole line
chk() { if has "$2"; then ok "$1"; else bad "$1: no line '$2'"; fi; }
chk "AppId unchanged (a new one is a second install with an orphaned uninstall entry)" 'AppId={{8F3C1D42-6A21-4B7E-9F0C-2E5A7D9B4C11}'
chk "the name players see" 'AppName=Generals Vanguard'
chk "the Add/Remove Programs name" 'UninstallDisplayName=Generals Vanguard'
chk "a new install's folder, no space" 'DefaultDirName={localappdata}\Programs\GeneralsVanguard'
chk "the Start Menu folder" 'DefaultGroupName=Generals Vanguard'
chk "the Start Menu folder takes the new name over an old install" 'UsePreviousGroup=no'
if grep -Eiq '^[[:space:]]*UsePreviousAppDir[[:space:]]*=[[:space:]]*(no|false|0)' "$ISS"; then
	bad "UsePreviousAppDir is off: a reinstall would leave an existing install behind in Programs\\GeneralsX"
else ok "an existing install keeps its folder (UsePreviousAppDir left at yes)"; fi
chk "Start Menu shortcut, same exe" 'Name: "{group}\Generals Vanguard"; Filename: "{app}\GeneralsXLauncher.exe"; WorkingDir: "{app}"'
chk "desktop shortcut, same exe" 'Name: "{userdesktop}\Generals Vanguard"; Filename: "{app}\GeneralsXLauncher.exe"; WorkingDir: "{app}"; Tasks: desktopicon'
chk "the old Start Menu shortcut is removed" 'Type: files; Name: "{autoprograms}\GeneralsX\GeneralsX.lnk"'
chk "the old Start Menu folder is removed when empty" 'Type: dirifempty; Name: "{autoprograms}\GeneralsX"'
chk "the old desktop shortcut goes only when a new one replaces it" 'Type: files; Name: "{userdesktop}\GeneralsX.lnk"; Tasks: desktopicon'
chk "the uninstall icon is still the launcher exe" 'UninstallDisplayIcon={app}\GeneralsXLauncher.exe'
# Every "GeneralsX" left outside comments must be a file or folder name something is keyed on, an old
# shortcut being removed, or the installed-state file's own header.
left="$(sed -e '/^[[:space:]]*;/d' -e '/^[[:space:]]*\/\//d' -e 's#\([^:]\)//.*$#\1#' "$ISS" |
	grep -n 'GeneralsX' |
	grep -v 'GeneralsX\(Launcher\|Update\|Patch\|ZH\)\|GeneralsX\.\(cmd\|ico\|iss\)\|GeneralsX-\(Setup\|win64\)\|Documents\\GeneralsX\|GeneralsX\\GeneralsZH' |
	grep -v '{autoprograms}\\GeneralsX\|GeneralsX\.lnk\|# GeneralsX installed state' || true)"
if [ -z "$left" ]; then ok "no player-visible GeneralsX left"; else bad "player-visible GeneralsX left:"; printf '%s\n' "$left"; fi
echo "$n checks"; exit $fail
