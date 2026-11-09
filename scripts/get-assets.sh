#!/bin/bash
# Download C&C game data from your own Steam account.
#
# Usage:
#   ./get-assets.sh <steam_username>                  Zero Hour only
#   ./get-assets.sh <steam_username> --with-generals  Zero Hour AND the original Generals
#
# Steam Guard: you'll be prompted for the code on first login.
#
# GeneralsX @feature Claude 16/08/2026 --with-generals, and it is the fix for "why is there no
# music". Zero Hour's music list asks for 57 tracks and a Zero Hour install carries 8 of them; the
# other ~50 live in the ORIGINAL Generals, and Zero Hour ships a stub in their place (its
# Music.big is 768 KB and contains one non-audio file, generalsa.sec - measured, not assumed).
#
# So a script that fetches only app 2732960 fetches exactly the one product without the soundtrack,
# and then the player reasonably reports the game as broken. Nothing they reinstall can fix it,
# because the tracks were never in what we told them to download.
set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <steam_username> [--with-generals]"
    exit 1
fi

STEAM_USER="$1"; shift
WITH_GENERALS=0
for arg in "$@"; do
    case "$arg" in
        --with-generals) WITH_GENERALS=1 ;;
        *) echo "unknown option: $arg" >&2; exit 1 ;;
    esac
done

# App ids confirmed against Steam's own store API, not guessed - an earlier version of this note
# had the Generals id wrong and it resolved to an unrelated product.
ZH_APP=2732960        # Command & Conquer(tm) Generals Zero Hour
GEN_APP=2229870       # Command & Conquer(tm) Generals

ZH_DEST="$HOME/GeneralsX/GeneralsZH"
GEN_DEST="$HOME/GeneralsX/Generals"

fetch() {   # fetch <appid> <dest> <label>
    local app="$1" dest="$2" label="$3"
    local tmp="$HOME/GeneralsX/.steamcmd_$app"
    mkdir -p "$tmp" "$dest"

    echo "==> $label (app $app)"
    # The Windows depot: the assets are platform-independent, only the executables are not, and
    # those are excluded below.
    steamcmd \
        +@sSteamCmdForcePlatformType windows \
        +force_install_dir "$tmp" \
        +login "$STEAM_USER" \
        +app_update "$app" validate \
        +quit

    echo "    moving data files into $dest ..."
    # Data only; keep any deployed GeneralsX runtime (run.sh, dylibs) in place.
    rsync -a --exclude="*.exe" --exclude="*.dll" "$tmp/" "$dest/"
}

fetch "$ZH_APP" "$ZH_DEST" "Zero Hour"

if [[ "$WITH_GENERALS" -eq 1 ]]; then
    fetch "$GEN_APP" "$GEN_DEST" "Generals (soundtrack)"
fi

echo
echo "Done. Zero Hour assets:"
ls "$ZH_DEST"/*.big 2>/dev/null | head

if [[ "$WITH_GENERALS" -eq 1 ]]; then
    echo
    echo "Generals assets (this is where the missing ~50 music tracks come from):"
    ls "$GEN_DEST"/*.big 2>/dev/null | head
else
    echo
    echo "NOTE: Zero Hour ships 8 of the 57 music tracks its own music list asks for. The rest are"
    echo "      in the original Generals. Without them the game has sound effects and no soundtrack,"
    echo "      which is expected rather than a broken install."
    echo "      If you own Generals too, re-run with:  $0 $STEAM_USER --with-generals"
fi

echo
echo "Custom maps do NOT go in the folder above. They go in your user data directory:"
echo "  macOS   ~/Library/Application Support/GeneralsX/GeneralsZH/Maps/<MapName>/<MapName>.map"
echo "  Linux   ~/.local/share/GeneralsX/GeneralsZH/Maps/<MapName>/<MapName>.map"
echo
echo "Launch with: cd ~/GeneralsX/GeneralsZH && ./run.sh -win"
