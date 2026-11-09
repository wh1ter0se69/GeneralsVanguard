#!/usr/bin/env python3
# GeneralsX @feature Claude 17/08/2026 Build the overlay archives this project AUTHORS.
#
#   build-overlay-generated.py --dest <dir> [--game-dir <dir>]... [--check] [--quiet]
#   build-overlay-generated.py --resolve [--game-dir <dir>]...
#
# WHY THIS EXISTS, and it is not "a build step for a .big".
#
# Every .wnd layout change made in this project reached exactly one machine. The archive that
# carries them, !GXUI.big, was hand-made on the developer's Mac and lived in ~/GeneralsX/GeneralsZH
# and nowhere else, so a smaller map preview, a resized friends tab and every future layout edit
# were blocked behind a distribution step nobody had written. The overlay mechanism itself was
# never the problem - overlay-optional.txt, gx_overlay_stage_optional and the Windows payload tiers
# all already work. The missing half was that a listed archive has to EXIST on the packaging box,
# and the community archives get there by being downloaded once while this one never could.
#
# THE KEY PROPERTY THAT MAKES A GENERATOR THE RIGHT ANSWER RATHER THAN A COPY STEP. !GXUI.big is
# derived from retail bytes that every install already has: make-gxui-archive.py reads
# Window\Menus\PopupHostGame.wnd out of WindowZH.big and transforms it. So the archive does not
# have to be shipped between machines at all - it can be rebuilt wherever it is needed, from
# content that is already there. Measured 17/08/2026: rebuilding it in a scratch directory on this
# Mac reproduced the installed archive byte for byte,
# sha256 b61c9bccefaeb90cb76783d62e9722879b174bc17fd179a1deb6432d24628eea, which is what makes
# "generate it on each box" equivalent to "ship it" rather than merely similar. Two peers that both
# generate it therefore agree on assetID as well, and neither sees the differing-archives chat line.
#
# It also means the retail bytes are never committed. This repository is public; a layout lifted
# out of EA's archive is not ours to publish, and a recipe that reads the player's own copy is.
#
# WHAT IT REFUSES TO DO, because each of these ships a break that only shows up on a peer:
#
#   * Generate an archive that overlay-optional.txt does not name. The staging scripts iterate the
#     LIST, so an archive built under a name nothing lists is built and then silently dropped.
#   * Generate one that overlay-archives.txt names. That list's entry bar is "removing it moves
#     dataID"; a generated archive that moved dataID would refuse every peer that had not run this
#     script, which is a far worse failure than the one this fixes.
#   * Transform a source archive whose layout is not where the generator assumes. make-gxui-archive
#     slices WindowZH.big by hardcoded offset. Handed a different release, a different language or
#     a modded archive, it would slice the middle of some other file, "succeed", and produce a .wnd
#     the engine parses into a broken host popup. The entry is looked up in the archive's own
#     directory table and checked against the expected offset and size first.
#
# IT IS IDEMPOTENT AND WRITES ONLY ON A REAL CHANGE. package-windows-share.ps1 names its artifacts
# by a content id over the payload, so re-running a packaging job must not perturb bytes it did not
# mean to change; and an unchanged archive keeps its URL and is skipped by the publisher.
import argparse
import hashlib
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
BUILD = os.path.join(REPO, "scripts", "build")
OPTIONAL_LIST = os.path.join(BUILD, "overlay-optional.txt")
REQUIRED_LIST = os.path.join(BUILD, "overlay-archives.txt")

# The recipes. The point of the table is that adding one is a change HERE and not in three platform
# scripts, which is the same reason the archive lists are files.
#
# 'expect' is (member, offset, size) as the member sits in the UNMODIFIED source archive. It is the
# guard described above, and the numbers must agree with the generator's own OFF/LEN.
GENERATED = [
    {
        "name": "!GXUI.big",
        "generator": os.path.join(BUILD, "make-gxui-archive.py"),
        "source": "WindowZH.big",
        "member": "Window\\Menus\\PopupHostGame.wnd",
        "expect": ("Window\\Menus\\PopupHostGame.wnd", 5323212, 40101),
        "what": "host-game popup with the Confirm Password field",
    },
    # GeneralsX @feature Claude 03/10/2026 The 7x16 / 10x24 occupancy pips (GXImageVariant.h). NO SOURCE ARCHIVE: the art
    # is drawn in the generator's own grids, so nothing is read from an install and nothing EA shipped is committed. The
    # generator writes 'members' (archive path -> file it wrote) into the directory it is given.
    {
        "name": "!GXUIHD.big",
        "generator": os.path.join(BUILD, "make-gxuihd-archive.py"),
        "source": None,
        "members": [("Art\\Textures\\GXUIHD_Pips.tga", "GXUIHD_Pips.tga"),
                    ("Data\\INI\\MappedImages\\TextureSize_512\\GXUIHD.ini", "GXUIHD.ini")],
        "what": "hand-drawn 7x16 / 10x24 unit pips for 1440p and taller",
    },
    # GeneralsX @feature Claude 08/10/2026 The control bar's side buttons (Options, Worker, Chat, Beacon, the General's star button) and
    # the General's experience ladder, redrawn in the player HUD's palette. Redefines 137 EXISTING MappedImage names from
    # Data\INI\MappedImages\HandCreated\zz..., which ImageCollection::load parses last. NO SOURCE ARCHIVE, like !GXUIHD.big.
    {
        "name": "!GXUISide.big",
        "generator": os.path.join(BUILD, "make-gxsidebar-archive.py"),
        "source": None,
        "members": [("Art\\Textures\\GXSidebar.tga", "GXSidebar.tga"),
                    ("Data\\INI\\MappedImages\\HandCreated\\zzGXSidebar.ini", "zzGXSidebar.ini")],
        "what": "control bar side buttons and the General's experience ladder in the GeneralsX HUD look",
    },
]


def load_make_big():
    """Import scripts/tooling/make-big.py, whose hyphen keeps it out of a plain import."""
    path = os.path.join(HERE, "make-big.py")
    spec = importlib.util.spec_from_file_location("gx_make_big", path)
    if spec is None or spec.loader is None:
        raise SystemExit("cannot load %s" % path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def read_list(path, split_fields):
    """Strip comments and blanks. split_fields picks the filename out of an optional-list line."""
    if not os.path.isfile(path):
        raise SystemExit("ERROR: archive list not found at %s - run this from a checkout." % path)
    names = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            names.append(line.split("|")[1].strip() if split_fields else line)
    return names


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def check_source(make_big, archive, expect):
    """The source archive must carry the expected member at the expected offset and size."""
    want_name, want_off, want_len = expect
    try:
        entries = make_big.read_entries(archive)
    except SystemExit as exc:
        raise SystemExit("ERROR: %s: %s" % (archive, exc))
    for name, off, size in entries:
        if name.lower() != want_name.lower():
            continue
        if off != want_off or size != want_len:
            raise SystemExit(
                "ERROR: %s carries %s at offset %d size %d, but the generator slices %d/%d.\n"
                "  This archive is not the retail one the recipe was written against (different\n"
                "  release, different language, or already modded). Refusing to transform it: the\n"
                "  slice would land in another file and produce a layout that parses into a broken\n"
                "  popup rather than failing." % (archive, name, off, size, want_off, want_len))
        return
    raise SystemExit("ERROR: %s does not contain %s." % (archive, want_name))


# GeneralsX @bugfix Claude 18/09/2026 EVERY --game-dir IS TRIED, IN ORDER, and a miss names them all.
#
# This took one directory, and each packager picked it from an env chain of its own rather than
# from the directories its archive search walks. Release 85f9a6650 stopped on "WindowZH.big not
# found" on all three packaging hosts with the retail data on each; the plainest case was macOS,
# whose bundler passed GX_PATCH_SRC - always set, never holding WindowZH.big - while the archive
# search also walked the game folder that did. Taking a list lets every caller hand over the SAME
# directories its archive search walks, so the two cannot disagree again.
#
# First hit wins, NOT first hit that passes check_source. A WindowZH.big that is the wrong release
# must stop the build with check_source's message; quietly trying the next directory would build
# the archive from whichever copy happened to pass, and say nothing about the one that did not.
def find_source(game_dirs, recipe):
    tried = []
    for game_dir in game_dirs:
        path = os.path.join(game_dir, recipe["source"])
        if os.path.isfile(path):
            return path
        tried.append(path)
    raise SystemExit(
        "ERROR: %s not found in any of the %d place(s) tried:\n%s\n"
        "  %s is generated FROM the retail archive, so a game install has to be reachable.\n"
        "  Set GX_GAME_DATA to the directory holding %s, or pass --game-dir."
        % (recipe["source"], len(tried), "\n".join("    " + t for t in tried),
           recipe["name"], recipe["source"]))


def build_one(make_big, recipe, game_dirs, dest_dir, check_only, quiet):
    name = recipe["name"]
    target = os.path.join(dest_dir, name)
    if recipe["source"] is not None:
        source = find_source(game_dirs, recipe)
        # Always said, --quiet or not: with several candidates, which install an archive was cut from
        # is the one fact a packaging log cannot be reconstructed without.
        print("  source     %s" % source)
        check_source(make_big, source, recipe["expect"])

    tmpdir = tempfile.mkdtemp(prefix="gxoverlay")
    try:
        if recipe["source"] is not None:
            wnd = os.path.join(tmpdir, os.path.basename(recipe["member"].replace("\\", "/")))
            env = dict(os.environ, GX_WINDOW_BIG=source)
            argv = [sys.executable, recipe["generator"], wnd]
            members = [(recipe["member"], wnd)]
        else:
            out = os.path.join(tmpdir, "members")
            env = dict(os.environ)
            argv = [sys.executable, recipe["generator"], out]
            members = [(vpath, os.path.join(out, f)) for vpath, f in recipe["members"]]
        # The generator prints its own layout report; keep it out of a packaging log unless it
        # fails, where it is the only thing that says which child moved.
        proc = subprocess.run(argv, env=env, capture_output=True, text=True)
        if proc.returncode != 0 or not all(os.path.isfile(f) for _v, f in members):
            sys.stderr.write(proc.stdout)
            sys.stderr.write(proc.stderr)
            raise SystemExit("ERROR: %s failed to produce %s" % (recipe["generator"], ", ".join(f for _v, f in members)))

        staged = os.path.join(tmpdir, name)
        table, total = make_big.write_big(staged, members)
        if make_big.read_entries(staged) != table:
            raise SystemExit("ERROR: readback of %s does not match what was written" % staged)

        digest = sha256_of(staged)
        if os.path.isfile(target) and sha256_of(target) == digest:
            if not quiet:
                print("  unchanged  %s  %s  (%s)" % (name, digest[:16], recipe["what"]))
            return digest, False
        if check_only:
            state = "STALE" if os.path.isfile(target) else "MISSING"
            print("  %-9s %s  %s  (%s)" % (state, name, digest[:16], recipe["what"]))
            return digest, True
        os.makedirs(dest_dir, exist_ok=True)
        # Same directory, so this is a rename: an interrupted run cannot leave a half-written
        # archive for the engine to open and hash into a dataID nobody else computes.
        partial = target + ".partial"
        shutil.copyfile(staged, partial)
        os.replace(partial, target)
        print("  wrote      %s  %s  %s bytes  (%s)" % (name, digest[:16], format(total, ","), recipe["what"]))
        return digest, True
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


def main(argv):
    ap = argparse.ArgumentParser(description="Build the overlay archives GeneralsX authors.")
    ap.add_argument("--dest",
                    help="directory the packaging scripts stage optional archives FROM")
    ap.add_argument("--game-dir", action="append",
                    help="install holding the retail archives; repeat it, and the first that holds "
                         "the source wins (default $GX_GAME_DATA, then ~/GeneralsX/GeneralsZH)")
    ap.add_argument("--resolve", action="store_true",
                    help="print the source archive each recipe would read, and exit without opening it")
    ap.add_argument("--check", action="store_true",
                    help="report what would be written and exit non-zero if anything would be")
    ap.add_argument("--quiet", action="store_true", help="say nothing about unchanged archives")
    args = ap.parse_args(argv)
    if not args.resolve and not args.dest:
        ap.error("--dest is required unless --resolve is given")
    # Empty entries dropped, so a caller can pass an unset variable through without a special case.
    game_dirs = [d for d in (args.game_dir or [os.environ.get("GX_GAME_DATA"),
                                               os.path.expanduser("~/GeneralsX/GeneralsZH")]) if d]

    optional = read_list(OPTIONAL_LIST, split_fields=True)
    required = read_list(REQUIRED_LIST, split_fields=False)

    for recipe in GENERATED:
        name = recipe["name"]
        # Both directions, because each failure is silent in its own way: unlisted means built and
        # never staged, mandatory means peers refused until they run this script.
        if name in required:
            raise SystemExit(
                "ERROR: %s is named by overlay-archives.txt, the MANDATORY list.\n"
                "  That list's bar is 'removing it moves dataID'. A generated archive that moved\n"
                "  dataID would refuse every peer that had not run this script - a worse failure\n"
                "  than the one it fixes. Move it to overlay-optional.txt." % name)
        if name not in optional:
            raise SystemExit(
                "ERROR: %s is not named by overlay-optional.txt.\n"
                "  Every packaging script iterates that list, so an archive built under a name it\n"
                "  does not carry is built and then silently dropped. Add a line for it." % name)

    if args.resolve:
        for recipe in GENERATED:
            if recipe["source"] is not None:
                print(find_source(game_dirs, recipe))
        return 0

    make_big = load_make_big()
    print("generated overlay archives -> %s" % args.dest)
    changed = 0
    for recipe in GENERATED:
        _digest, moved = build_one(make_big, recipe, game_dirs, args.dest, args.check, args.quiet)
        changed += 1 if moved else 0

    if args.check and changed:
        print("%d archive(s) would be written." % changed)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
