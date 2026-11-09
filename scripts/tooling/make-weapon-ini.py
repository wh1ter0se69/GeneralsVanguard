#!/usr/bin/env python3
# GeneralsX @feature Claude 24/09/2026 Data/INI/Weapon.ini: the Community Patch's Weapon.ini plus
# GeneralsX's balance edits. A RECIPE, NOT A COPY.
#
#   make-weapon-ini.py --out <Data/INI/Weapon.ini> --search <dir> [--search <dir>]...
#   make-weapon-ini.py --root <game folder> [--root <game folder>]... [--search <dir>]... [--if-archive]
#   make-weapon-ini.py --selftest
#
# --root is the ship-time step for a folder a peer runs from (a deploy's runtime folder, a
# harness peer's game-data folder): it writes
# <root>/Data/INI/Weapon.ini, taking the archive from where that peer's engine mounts it, in the
# engine's order (root_search): the --search dirs, then the primary asset root ($CNC_GENERALS_ZH_PATH
# when it holds archives, else <root>), then the overlay ($GENERALSX_PATCH_PATH when set, else
# <root>/GeneralsXPatch - never the <root>/../GeneralsXPatch sibling on its own, which the engine does
# not mount either). <root> is taken to be the peer's executable folder and working directory; a wrapper
# that sets either variable must set it for this too (deploy-*-zh.sh export the sibling as
# GENERALSX_PATCH_PATH, as their run.sh will; setup-run-win64.ps1's go.bat mounts no sibling).
# Idempotent: an identical file is left alone. A folder with no archive in any of those is an error,
# or with --if-archive a WARNING and a skip (the deploy scripts: such a peer joins nobody anyway).
#
# The bytes come from 500_900_CommunityPatch_CoreINI.big, a mandatory overlay archive every packager
# already stages, so no third-party INI is committed (the rule build-overlay-generated.py states).
# The first --search directory holding the archive is used. Its Weapon.ini must hash to SOURCE_SHA256:
# a different Community Patch release is refused, never silently edited.
#
# WHY A WHOLE FILE. A Weapon block cannot be partially overridden at load time:
# WeaponStore::parseWeaponTemplateDefinition refuses a second definition of a name outside
# INI_LOAD_CREATE_OVERRIDES. A loose Data\INI\Weapon.ini replaces the archive's by name
# (INI::loadFileDirectory's "X.ini" half), so the edit ships as the full file.
#
# IT MOVES dataID. The file goes through the INI parser, so it is in iniCRC: every peer must carry
# the same bytes or joins are refused with DATA_DIFFERS. The output is deterministic, so every box
# that runs this against the pinned archive produces the same file.
#
# THE EDIT: Toxin Tractor option 1 (docs/WORKDIR/STREAM_WEAPON_FPS_DESIGN_2026-09-23.md section 5,
# owner decision 24/09/2026), re-tuned 05/10/2026 for Generals Online fire timing: DelayBetweenShots
# 40 -> 120 ms and twice the damage, on every ToxinTruckGun* weapon (GLA, Demolition, Stealth and Toxin
# generals, all upgrades). Each shot creates and destroys a missile-class projectile Object, so fewer
# shots is less of that cost. ClipReloadTime moves with it, as in the source (reload == delay, so the
# clip stays invisible).
#
# WHY 120, NOT THE 80 OF 24/09. 80 ms was chosen on the retail-grid timing (80 ms = 6 frames: +33%
# direct damage). fix/go-parity-fire (owner 05/10: fire like Generals Online) rounds fire timers on the
# 60 Hz grid (INI::parseDurationUnsignedIntGO: 40 ms = 3 frames, raised to GO's 4-frame minimum; 80 ms =
# 5; 120 ms = 8) and the tractor's gun is turret-driven, so it fires on GO's 4-frame turret cadence, and
# a RATE_OF_FIRE bonus divides then FLOORS the frame count (WeaponTemplate::getDelayBetweenShots).
# Computed from the code (go-parity-fire tools/predict.py), frames per shot and direct damage per second
# against GO's own 40 ms / 1x data:
#   rank         GO 40 ms   80 ms x2            120 ms x2
#   rookie       4          8 (1.00x)           8 (1.00x)
#   veteran+     4          4 (2.00x)           8 (1.00x)   (veteran, horde 125%, elite, heroic)
#   solo-AI 80%  8          8 (2.00x)          12 (1.33x)
# 80 ms doubled GO's direct damage from veteran up; 120 ms matches GO at every player rank with half
# GO's projectile Objects (owner 05/10: yes, "do what GO does"). Rate-of-fire bonuses ADD: once the total
# passes 160% (a veteran or better firing at a target an Avenger has marked, 150%; hero + hard AI; elite or
# hero + a propaganda tower) 120 ms floors to 4 frames, the turret cadence, and does 2x GO's direct damage.
#
# The one intended balance change: poison damage over time is the LAST hit's damage, repeated every
# PoisonDamageInterval (PoisonedBehavior::startPoisonedEffects). Doubling the hit doubles it: every
# infantry unit (PoisonDamageInterval 100, PoisonDuration 3000) takes about +100 poison dps from a
# base Toxin Tractor, and the poison left after the stream stops is doubled too.
# THE ALPHA AURORA FIX (owner-approved 26/09/2026, balance side included): GeneralsGamePatch PR 292 (commy2,
# 2021). The retail SupW_AuroraFuelAirBomb carries HeightDieUpdate (15 above the terrain under it, or above any
# structure within its 12-unit radius) and spawns the fuel-air cloud from its DEATH, while the 400 direct hit only
# fires on a COLLISION (MissileAIUpdate::detonate). So the bomb usually dies before it touches its target: no direct
# hit, and the cloud forms wherever it died - "the first OR second hit does no damage". PR 292 removes the height
# death and the death OCL, spawns the cloud from the weapon (ProjectileDetonationOCL) at the real impact, kills the
# cloud at once (LifetimeUpdate 0; it still detonates after its 1 s SlowDeath), lifts the cloud 15 above the impact
# and gives AirF_AuroraBombScatter1 a 1-frame lifetime so it works from the weapon. Measured headless 26/09/2026:
# every sortie lands both hits (War Factory 3 sorties -> 2, Tunnel 2 -> 1). Balance: the direct hit now also lands
# on units beside the impact (an Overlord took 400, before 1.2) - the upstream fix, accepted as such.
# That is four more files than Weapon.ini, each cut from the same archive and pinned by hash: RECIPE_FILES below.
# Every caller stages all of them; `--list` prints their paths so no caller keeps its own copy of the list.
import argparse
import hashlib
import os
import struct
import sys
import tempfile

ARCHIVE = "500_900_CommunityPatch_CoreINI.big"
MEMBER = "Data\\INI\\Weapon.ini"
SOURCE_SHA256 = "52b4b3453e610d0efc7159f9b3388b5e448260ddc63f6734adf85f2130a38a12"

TOXIN_GUNS = [
    "ToxinTruckGun", "ToxinTruckGunUpgraded",
    "ToxinTruckGunPlusOne", "ToxinTruckGunUpgradedPlusOne",
    "ToxinTruckGunPlusTwo", "ToxinTruckGunUpgradedPlusTwo",
    "Chem_ToxinTruckGunGamma", "Chem_ToxinTruckGunGammaPlusOne", "Chem_ToxinTruckGunGammaPlusTwo",
]
DELAY_OLD, DELAY_NEW = "40", "120"

HEADER = (
    b"; GeneralsX: the Community Patch's Weapon.ini (500_900_CommunityPatch_CoreINI.big) with the\r\n"
    b"; ToxinTruckGun* weapons firing every 120 ms instead of 40, at twice the damage, and the Alpha\r\n"
    b"; Aurora's fuel-air cloud spawned by its weapon (GeneralsGamePatch PR 292). Generated by\r\n"
    b"; scripts/tooling/make-weapon-ini.py; edit the recipe there, never this file.\r\n"
)


def recipe_header(member):
    return (b"; GeneralsX: the Community Patch's %s with the Alpha Aurora fix (GeneralsGamePatch PR 292).\r\n"
            b"; Generated by scripts/tooling/make-weapon-ini.py; edit the recipe there, never this file.\r\n"
            % member.split("\\")[-1].encode())


def replace_once(text, old, new, what):
    """text with old replaced by new; old must occur exactly once, and the result must differ."""
    if text.count(old) != 1:
        raise SystemExit("ERROR: %s: expected exactly one %r, found %d" % (what, old[:60], text.count(old)))
    return text.replace(old, new, 1)


def edit_aurora_weapon(text):
    """SupW_AuroraFuelBombWeapon spawns the fuel-air cloud itself, at the impact (PR 292)."""
    head = b"Weapon SupW_AuroraFuelBombWeapon\r\n"
    start = text.find(head)
    if start < 0 or text.find(head, start + 1) >= 0 or (start > 0 and text[start - 2:start] != b"\r\n"):
        raise SystemExit("ERROR: expected exactly one 'Weapon SupW_AuroraFuelBombWeapon' block")
    stop = text.index(b"\r\nEnd\r\n", start)
    block = text[start:stop]
    for field in (b"FireFX", b"ProjectileDetonationFX", b"ProjectileDetonationOCL"):
        if any(l.split(b"=")[0].strip() == field for l in block.split(b"\r\n")):
            raise SystemExit("ERROR: Weapon SupW_AuroraFuelBombWeapon already has %s" % field.decode())
    lines = block.split(b"\r\n")
    hits = [i for i, l in enumerate(lines) if l.strip() == b"ProjectileObject = SupW_AuroraFuelAirBomb"]
    if len(hits) != 1:
        raise SystemExit("ERROR: Weapon SupW_AuroraFuelBombWeapon has %d 'ProjectileObject = SupW_AuroraFuelAirBomb' "
                         "lines, expected 1" % len(hits))
    lines[hits[0] + 1:hits[0] + 1] = [b"FireFX = FX_AuroraBombLaunch", b"ProjectileDetonationFX = AirF_FX_AuroraBombExplode",
                                      b"ProjectileDetonationOCL = SupW_OCL_FuelAirBomb"]
    block = b"\r\n".join(lines)
    return text[:start] + block + text[stop:]


def edit_aurora_bomb(text):
    """No height death and no death OCL/FX: the bomb falls until it hits something (PR 292)."""
    text = replace_once(text, b"Behavior = InstantDeathBehavior ModuleTag_03\r\nFX = AirF_FX_AuroraBombExplode\r\n"
                        b"OCL = SupW_OCL_FuelAirBomb\r\nEnd\r\n", b"", "SupW_AuroraFuelAirBomb death OCL")
    return replace_once(text, b"Behavior = HeightDieUpdate ModuleTag_07\r\nTargetHeight = 15.0\r\n"
                        b"TargetHeightIncludesStructures = YES\r\nEnd\r\n", b"", "SupW_AuroraFuelAirBomb HeightDieUpdate")


def edit_aurora_gas(text):
    """The cloud dies at once and detonates after its SlowDeath delay, where it was spawned (PR 292)."""
    return replace_once(text, b"Behavior = HeightDieUpdate ModuleTag_03\r\nTargetHeight = 15.0\r\n"
                        b"TargetHeightIncludesStructures = YES\r\nDestroyAttachedParticlesAtHeight = 16.0\r\nEnd\r\n",
                        b"Behavior = LifetimeUpdate ModuleTag_03\r\nMinLifetime = 0\r\nMaxLifetime = 0\r\nEnd\r\n",
                        "SupW_AuroraFuelAirGas HeightDieUpdate")


def edit_aurora_ocl(text):
    """The cloud 15 above the impact, not inside the ground (PR 292)."""
    return replace_once(text, b"ObjectCreationList SupW_OCL_FuelAirBomb\r\nCreateObject\r\nObjectNames = SupW_AuroraFuelAirGas\r\n",
                        b"ObjectCreationList SupW_OCL_FuelAirBomb\r\nCreateObject\r\nOffset = X:0 Y:0 Z:15\r\n"
                        b"ObjectNames = SupW_AuroraFuelAirGas\r\n", "SupW_OCL_FuelAirBomb")


def edit_aurora_particles(text):
    """AirF_AuroraBombScatter1 lives one frame, so it works from the weapon's detonation FX (PR 292)."""
    head = b"ParticleSystem AirF_AuroraBombScatter1\r\n"
    if text.count(head) != 1:
        raise SystemExit("ERROR: expected exactly one 'ParticleSystem AirF_AuroraBombScatter1' block")
    start = text.index(head)
    stop = text.index(b"\r\nEnd\r\n", start)
    return text[:start] + replace_once(text[start:stop] + b"\r\n", b"\r\nSystemLifetime = 0\r\n",
                                       b"\r\nSystemLifetime = 1\r\n", "AirF_AuroraBombScatter1")[:-2] + text[stop:]


def edit_block(text, head, pairs, what):
    """text with each (old, new) applied once inside the one ParticleSystem block `head`; refused unless every old is there once."""
    if text.count(head) != 1:
        raise SystemExit("ERROR: expected exactly one %r block" % head.strip())
    start = text.index(head)
    stop = text.index(b"\r\nEnd\r\n", start)
    block = text[start:stop] + b"\r\n"
    for old, new in pairs:
        block = replace_once(block, old, new, what)
    return text[:start] + block[:-2] + text[stop:]


# GeneralsX @feature Claude 06/10/2026 Flares you can read (owner GO 05/10: "must look like the 2003 game, only better").
# A missile turning onto a flare is the whole point of the Countermeasures upgrade and was hard to see: five to ten small
# short-lived sparks and a thin faint smoke thread. Same effect, same textures, same colour family: bigger and more
# numerous sparks, a hotter white core, a smoke trail that lives about twice as long and is denser. Client-only: particles
# draw from the client's own random stream and no simulation code reads them (ParticleSystem.ini is loaded outside the data
# CRC, GameEngine.cpp), so nothing here can split a lobby.
FLARE_EDITS = [
    (b"ParticleSystem CountermeasureFlareParticleSys\r\n", [
        (b"\r\nLifetime = 2.00 2.00\r\n", b"\r\nLifetime = 3.00 3.00\r\n"),
        (b"\r\nSize = 3.00 5.00\r\n", b"\r\nSize = 4.50 7.00\r\n"),
        (b"\r\nColor1 = R:210 G:242 B:255 0\r\n", b"\r\nColor1 = R:235 G:252 B:255 0\r\n"),
        (b"\r\nBurstCount = 5.00 10.00\r\n", b"\r\nBurstCount = 8.00 14.00\r\n"),
    ]),
    (b"ParticleSystem CounterMeasureSmoke\r\n", [
        (b"\r\nLifetime = 120.00 150.00\r\n", b"\r\nLifetime = 210.00 270.00\r\n"),
        (b"\r\nSize = 0.50 1.00\r\n", b"\r\nSize = 0.90 1.60\r\n"),
        (b"\r\nAlpha1 = 0.10 0.25 0\r\n", b"\r\nAlpha1 = 0.22 0.40 0\r\n"),
        # The fade-out keyframe is an absolute particle age: left at 150 the longer-lived smoke would reach zero alpha and be
        # culled there (Codex flare-look-r1 P2). Stretched to the new longest life it fades over the whole life instead.
        (b"\r\nAlpha2 = 0.00 0.00 150\r\n", b"\r\nAlpha2 = 0.00 0.00 270\r\n"),
        (b"\r\nBurstDelay = 3.00 5.00\r\n", b"\r\nBurstDelay = 2.00 3.00\r\n"),
    ]),
]


def edit_flare_particles(text):
    for head, pairs in FLARE_EDITS:
        text = edit_block(text, head, pairs, head.strip().decode())
    return text


def edit_particles(text):
    """Both ParticleSystem.ini edits, one file."""
    return edit_flare_particles(edit_aurora_particles(text))


# (archive member, sha256 of that member in the pinned CoreINI, edit). Output: the member's path below Data\INI,
# EXACT CASE - the engine merges a loose file with the archive's by the exact path string (INI::loadDirectory) and
# the Linux/macOS data overlay opens by exact case, so another spelling would load the object twice.
RECIPE_FILES = [
    ("Data\\INI\\ObjectCreationList.ini", "f60c0c9e4e117ff418c3efd8251decf9e6b243804f1f5cf5553cec025f19fd4e", edit_aurora_ocl),
    ("Data\\INI\\ParticleSystem.ini", "5b0076df81b4d246291f173940737ee5ec28e82c5a1b0f13fb14fa675e8800f0", edit_particles),
    ("Data\\INI\\Object\\Shared\\Weapon\\SupW_AuroraFuelAirBomb.ini",
     "fafe0dfaa7bcd8b1bfa9f8e20b0c9521cfc38695e5d6bd2a0dcd6475d0a7e4f8", edit_aurora_bomb),
    ("Data\\INI\\Object\\Shared\\Weapon\\SupW_AuroraFuelAirGas.ini",
     "2a2ee8c1a332586e0504493c0645d5df2aab0d0e7d76fcf73544b3d7c5953f2f", edit_aurora_gas),
]
# GeneralsX @feature Claude 29/09/2026 The skirmish-AI script file rides the same call sites: every packager and
# deploy step that already runs this recipe and stages `--list` now also gets Data/Scripts/SkirmishScripts.scb, cut
# from the retail loose copy by scripts/tooling/make-skirmish-scripts.py (sha256-pinned, deterministic, moves dataID).
import importlib.util as _ilu
_spec = _ilu.spec_from_file_location("make_skirmish_scripts", os.path.join(os.path.dirname(os.path.abspath(__file__)), "make-skirmish-scripts.py"))
SKIRMISH = _ilu.module_from_spec(_spec)
_spec.loader.exec_module(SKIRMISH)
# GeneralsX @feature Claude 08/11/2026 The skirmish AI's base layouts (Data/INI/AIData.ini, doors facing the base front) ride the same call
# sites: cut from the SAME CoreINI archive by scripts/tooling/make-aidata-layouts.py (its source member sha256-pinned, its default = the
# shipped file, both keys on every list but SHIP_OFF). It moves dataID, so a package without it is refused by one that has it (DATA_DIFFERS).
# Before this, nothing staged it and the base-layout engine half shipped with no layouts to read (Codex vnext1108-r1 P2).
_aspec = _ilu.spec_from_file_location("make_aidata_layouts", os.path.join(os.path.dirname(os.path.abspath(__file__)), "make-aidata-layouts.py"))
AIDATA = _ilu.module_from_spec(_aspec)
_aspec.loader.exec_module(AIDATA)
OUTPUTS = (["Data/INI/Weapon.ini"] + ["/".join(m.split("\\")) for m, _, _ in RECIPE_FILES] + AIDATA.OUTPUTS
           + SKIRMISH.OUTPUTS)


def read_member(big_path, member):
    """The named member's bytes, straight from the archive's directory table."""
    with open(big_path, "rb") as f:
        data = f.read()
    if data[:4] != b"BIGF":
        raise SystemExit("ERROR: %s is not a BIGF archive" % big_path)
    count = struct.unpack_from(">I", data, 8)[0]
    pos = 16
    for _ in range(count):
        off, size = struct.unpack_from(">II", data, pos)
        pos += 8
        end = data.index(b"\0", pos)
        name = data[pos:end].decode("latin-1")
        pos = end + 1
        if name.lower() == member.lower():
            return data[off:off + size]
    raise SystemExit("ERROR: %s has no %s" % (big_path, member))


def set_field(block, name, field, old, new):
    """Replace 'field = old' with 'field = new' in one block; exactly one such line must exist."""
    lines = block.split(b"\r\n")
    hits = [i for i, l in enumerate(lines) if l.split(b"=")[0].strip() == field.encode()]
    if len(hits) != 1:
        raise SystemExit("ERROR: Weapon %s has %d '%s' lines, expected 1" % (name, len(hits), field))
    i = hits[0]
    value = lines[i].split(b"=", 1)[1].strip().decode()
    if old is not None and value != old:
        raise SystemExit("ERROR: Weapon %s %s = %s, the recipe expects %s" % (name, field, value, old))
    lines[i] = ("%s = %s" % (field, new(value) if callable(new) else new)).encode()
    return b"\r\n".join(lines)


def edit(text):
    """The source Weapon.ini with the recipe applied. Every edit is asserted, never assumed."""
    for name in TOXIN_GUNS:
        head = ("Weapon %s\r\n" % name).encode()
        start = text.find(head)
        if start < 0 or text.find(head, start + 1) >= 0 or (start > 0 and text[start - 2:start] != b"\r\n"):
            raise SystemExit("ERROR: expected exactly one 'Weapon %s' block" % name)
        stop = text.index(b"\r\nEnd\r\n", start)
        block = text[start:stop]
        block = set_field(block, name, "PrimaryDamage", None, lambda v: "%.1f" % (2 * float(v)))
        block = set_field(block, name, "DelayBetweenShots", DELAY_OLD, DELAY_NEW)
        block = set_field(block, name, "ClipReloadTime", DELAY_OLD, DELAY_NEW)
        text = text[:start] + block + text[stop:]
    return HEADER + edit_aurora_weapon(text)


def find_archive(search):
    """The archive in the first search dir that holds it, or None."""
    return next((b for b in (os.path.join(d, ARCHIVE) for d in search) if os.path.isfile(b)), None)


def write(out, result):
    """out holds exactly result afterwards; an identical file is left alone."""
    if os.path.isfile(out) and open(out, "rb").read() == result:
        print("  unchanged  %s  %s" % (out, hashlib.sha256(result).hexdigest()[:16]))
        return
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    # Same directory, so the rename is atomic: a half-written INI would hash into a dataID nobody has.
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(os.path.abspath(out)), suffix=".partial")
    with os.fdopen(fd, "wb") as f:
        f.write(result)
    os.replace(tmp, out)
    print("  wrote      %s  %s" % (out, hashlib.sha256(result).hexdigest()[:16]))


def pinned_member(big, member, want):
    src = read_member(big, member)
    got = hashlib.sha256(src).hexdigest()
    if got != want:
        raise SystemExit(
            "ERROR: %s in %s hashes %s, not %s.\n"
            "  A different Community Patch release. Re-derive the recipe against it, and update\n"
            "  its pinned hash, before shipping."
            % (member, big, got, want))
    return src


def build(search, out, retail_extra=(), need_scb=True, keep_source=False, no_scb=False):
    """Weapon.ini at out, and every RECIPE_FILES output beside it under the same Data/INI folder. Every source
    is checked before anything is written, so a refused archive leaves no half-updated set behind."""
    big = find_archive(search)
    if big is None:
        raise SystemExit("ERROR: %s not found in: %s" % (ARCHIVE, ", ".join(search) or "(no --search)"))
    print("  source     %s" % big)
    ini_dir = os.path.dirname(os.path.abspath(out))
    files = [(out, edit(pinned_member(big, MEMBER, SOURCE_SHA256)))]
    for member, want, fn in RECIPE_FILES:
        files.append((os.path.join(ini_dir, *member.split("\\")[2:]),
                      recipe_header(member) + fn(pinned_member(big, member, want))))
    # The AI base layouts, from this same archive (its own pinned member; refused, before anything is written, on a different one).
    for rel in AIDATA.OUTPUTS:
        files.append((os.path.join(ini_dir, *rel.split("/")[2:]),
                      AIDATA.generate(AIDATA.parse_lists(AIDATA.stock_text([os.path.dirname(big)])))))
    # The skirmish AI script file: Data/Scripts/ beside Data/INI/. Its retail source is found among the --search dirs
    # (a --root call adds the root itself) and the game-folder variables; checked BEFORE anything is written.
    scb_out = os.path.join(os.path.dirname(ini_dir), "Scripts", "SkirmishScripts.scb")
    scb_bytes = None
    if not no_scb and not SKIRMISH.already_done(scb_out):
        if need_scb or SKIRMISH.find_source(list(search) + list(retail_extra)) is not None:
            scb_bytes = SKIRMISH.build_bytes(list(search) + list(retail_extra))
        else:
            # --if-archive callers (deploy scripts on a peer that may hold no retail script file) keep working; such a
            # peer's dataID then matches nobody who ships the AI script edits, which its own join check reports.
            print("WARNING: no retail Data/Scripts/SkirmishScripts.scb for %s - %s NOT generated." % (out, scb_out))
    for path, data in files:
        write(path, data)
    if scb_bytes is not None:
        SKIRMISH.write(scb_out, scb_bytes, keep_source=keep_source)   # --root deploys keep <file>.retail (the pinned source) when they overwrite the retail original in place


def env_path(name, root):
    """StdBIGFileSystem's sanitizeConfiguredPath: trimmed, one pair of enclosing quotes dropped. A
    relative value is resolved against root, the peer's working directory. "" when unset."""
    v = os.environ.get(name, "").strip()
    if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
        v = v[1:-1]
    return os.path.join(root, v) if v else ""


def has_big(d):
    """Whether the engine's non-recursive *.big scan of d mounts anything (unreadable: nothing)."""
    try:
        return any(n.lower().endswith(".big") and os.path.isfile(os.path.join(d, n)) for n in os.listdir(d))
    except OSError:
        return False


def root_search(root, extra):
    """Where a peer whose executable folder and working directory are `root` gets the archive, in the
    order StdBIGFileSystem::init resolves it, after the --search dirs."""
    root = os.path.abspath(root)
    # loadPrimaryGameAssets mounts ONE asset root, before the overlay: the first of these variables
    # naming a folder with archives in it, else the executable's folder. On a filename tie the archive
    # mounted first wins (loadIntoDirectoryTreeAtPriority), so the asset root is searched first.
    # ponytail: Options.ini [Paths] AssetPath and the registry InstallPath, tried by the engine between
    # these and the executable's folder, are not read; name such a root in CNC_GENERALS_ZH_PATH.
    legacy = os.environ.get("CNC_ZH_INSTALLPATH", "")  # the engine takes this one raw
    primary = next((d for d in (env_path("CNC_GENERALS_ZH_PATH", root), env_path("GENERALSX_ASSET_PATH", root),
                                legacy and os.path.join(root, legacy)) if has_big(d)), root)
    # resolvePatchOverlayDirectory: GENERALSX_PATCH_PATH when set - a folder that is not there turns the
    # overlay OFF, never falls back - else <exe dir>/GeneralsXPatch. The engine never looks at
    # <root>/../GeneralsXPatch, so neither does this (Codex batch-r3): searching it unasked cut Weapon.ini
    # from an archive a go.bat peer never loads and hid that its overlay was missing. A launcher that
    # mounts the sibling exports it as GENERALSX_PATCH_PATH, and its caller does the same for this call.
    overlay = env_path("GENERALSX_PATCH_PATH", root) or os.path.join(root, "GeneralsXPatch")
    return list(extra) + [primary, overlay]


def build_roots(roots, extra, if_archive, game_dirs=(), no_scb=False):
    for root in roots:
        search = root_search(root, extra)
        if if_archive and find_archive(search) is None:
            print("WARNING: no %s in %s - %s NOT generated. Point GENERALSX_PATCH_PATH at the overlay "
                  "this peer mounts (or CNC_GENERALS_ZH_PATH at its asset root) and run this again." % (ARCHIVE, ", ".join(search), os.path.join(root, "Data", "INI", "Weapon.ini")))
            continue
        build(search, os.path.join(root, "Data", "INI", "Weapon.ini"), retail_extra=[root] + list(game_dirs), need_scb=not if_archive, keep_source=True, no_scb=no_scb)


def selftest():
    """Fails (exit 1) if the recipe misses an edit, edits the wrong block, or accepts a changed source."""
    def block(name, dmg, delay="40", reload_="40"):
        return ("Weapon %s\r\nPrimaryDamage = %s\r\nDelayBetweenShots = %s\r\nClipSize = 30\r\n"
                "ClipReloadTime = %s\r\nEnd\r\n" % (name, dmg, delay, reload_)).encode()
    aur = b"Weapon SupW_AuroraFuelBombWeapon\r\nProjectileObject = SupW_AuroraFuelAirBomb\r\nEnd\r\n"
    src = b"".join(block(n, "12.5") for n in TOXIN_GUNS) + block("ToxinTruckSprayer", "0.0", "200", "0") + aur
    out = edit(src)
    assert out.startswith(HEADER)
    for n in TOXIN_GUNS:
        assert block(n, "25.0", "120", "120") in out, n
    assert block("ToxinTruckSprayer", "0.0", "200", "0") in out, "a non-recipe weapon was touched"
    assert out.count(b"\r\nEnd\r\n") == len(TOXIN_GUNS) + 2
    for bad in (src.replace(b"Weapon ToxinTruckGunPlusTwo\r\n", b"Weapon Renamed\r\n"),
                src.replace(b"DelayBetweenShots = 40", b"DelayBetweenShots = 50", 1),
                src + block("ToxinTruckGun", "10.0")):
        try:
            edit(bad)
        except SystemExit:
            continue
        raise AssertionError("a source the recipe was not written against was accepted")
    # The Alpha Aurora edits (PR 292): each lands once, and a source that lacks its anchor or already carries the
    # change is refused rather than silently passed through.
    cr = lambda *ls: b"".join(l.encode() + b"\r\n" for l in ls)
    wsrc = src.replace(aur, cr("Weapon SupW_AuroraFuelBombWeapon", "PrimaryDamage = 400.0",
                               "ProjectileObject = SupW_AuroraFuelAirBomb", "ClipSize = 1", "End"))
    wout = edit(wsrc)
    assert cr("ProjectileObject = SupW_AuroraFuelAirBomb", "FireFX = FX_AuroraBombLaunch",
              "ProjectileDetonationFX = AirF_FX_AuroraBombExplode", "ProjectileDetonationOCL = SupW_OCL_FuelAirBomb",
              "ClipSize = 1", "End") in wout
    bomb = cr("Object SupW_AuroraFuelAirBomb", "Behavior = InstantDeathBehavior ModuleTag_03", "FX = AirF_FX_AuroraBombExplode",
              "OCL = SupW_OCL_FuelAirBomb", "End", "Behavior = HeightDieUpdate ModuleTag_07", "TargetHeight = 15.0",
              "TargetHeightIncludesStructures = YES", "End", "Locomotor = SET_NORMAL AuroraBombLocomotor", "End")
    assert edit_aurora_bomb(bomb) == cr("Object SupW_AuroraFuelAirBomb", "Locomotor = SET_NORMAL AuroraBombLocomotor", "End")
    gas = cr("Object SupW_AuroraFuelAirGas", "Behavior = HeightDieUpdate ModuleTag_03", "TargetHeight = 15.0",
             "TargetHeightIncludesStructures = YES", "DestroyAttachedParticlesAtHeight = 16.0", "End", "End")
    assert edit_aurora_gas(gas) == cr("Object SupW_AuroraFuelAirGas", "Behavior = LifetimeUpdate ModuleTag_03",
                                      "MinLifetime = 0", "MaxLifetime = 0", "End", "End")
    ocl = cr("ObjectCreationList SupW_OCL_FuelAirBomb", "CreateObject", "ObjectNames = SupW_AuroraFuelAirGas", "End", "End")
    assert b"CreateObject\r\nOffset = X:0 Y:0 Z:15\r\nObjectNames = SupW_AuroraFuelAirGas" in edit_aurora_ocl(ocl)
    ps = cr("ParticleSystem Other", "SystemLifetime = 0", "End", "ParticleSystem AirF_AuroraBombScatter1", "Gravity = 0.00",
            "SystemLifetime = 0", "Size = 0.10 0.10", "End")
    assert edit_aurora_particles(ps) == ps.replace(b"Gravity = 0.00\r\nSystemLifetime = 0", b"Gravity = 0.00\r\nSystemLifetime = 1")
    for fn, bad in ((edit, wout[len(HEADER):]), (edit, src.replace(b"ProjectileObject = SupW_AuroraFuelAirBomb", b"ProjectileObject = X")), (edit_aurora_bomb, edit_aurora_bomb(bomb)),
                    (edit_aurora_gas, edit_aurora_gas(gas)), (edit_aurora_ocl, edit_aurora_ocl(ocl)),
                    (edit_aurora_particles, ps.replace(b"AirF_AuroraBombScatter1", b"Renamed"))):
        try:
            fn(bad)
        except SystemExit:
            continue
        raise AssertionError("%s accepted a source the recipe was not written against" % fn.__name__)
    # The flare look: each of the eight values moves once, in its own block only; a source that lacks one is refused.
    fl = cr("ParticleSystem CountermeasureFlareParticleSys", "Lifetime = 2.00 2.00", "Size = 3.00 5.00",
            "Color1 = R:210 G:242 B:255 0", "BurstCount = 5.00 10.00", "End",
            "ParticleSystem CounterMeasureSmoke", "Lifetime = 120.00 150.00", "Size = 0.50 1.00", "Alpha1 = 0.10 0.25 0", "Alpha2 = 0.00 0.00 150",
            "BurstDelay = 3.00 5.00", "End", "ParticleSystem Other", "Size = 3.00 5.00", "End")
    fo = edit_flare_particles(fl)
    assert fo == cr("ParticleSystem CountermeasureFlareParticleSys", "Lifetime = 3.00 3.00", "Size = 4.50 7.00",
                    "Color1 = R:235 G:252 B:255 0", "BurstCount = 8.00 14.00", "End",
                    "ParticleSystem CounterMeasureSmoke", "Lifetime = 210.00 270.00", "Size = 0.90 1.60", "Alpha1 = 0.22 0.40 0", "Alpha2 = 0.00 0.00 270",
                    "BurstDelay = 2.00 3.00", "End", "ParticleSystem Other", "Size = 3.00 5.00", "End"), "flare edit"
    for bad in (fo, fl.replace(b"BurstCount = 5.00 10.00", b"BurstCount = 6.00 10.00"), fl.replace(b"CounterMeasureSmoke", b"Renamed")):
        try:
            edit_flare_particles(bad)
        except SystemExit:
            continue
        raise AssertionError("edit_flare_particles accepted a source the recipe was not written against")
    assert OUTPUTS[0] == "Data/INI/Weapon.ini" and "Data/INI/Object/Shared/Weapon/SupW_AuroraFuelAirBomb.ini" in OUTPUTS
    assert "Data/INI/AIData.ini" in OUTPUTS and OUTPUTS[-1] == "Data/Scripts/SkirmishScripts.scb", OUTPUTS   # the AI base layouts ship with the rest
    # --root takes the archive the peer's engine mounts. Each location gets its own copy; the pick must be
    # the one StdBIGFileSystem resolves (primary asset root, then the ONE overlay), whatever else exists.
    keys = ("GENERALSX_PATCH_PATH", "CNC_GENERALS_ZH_PATH", "GENERALSX_ASSET_PATH", "CNC_ZH_INSTALLPATH")
    with tempfile.TemporaryDirectory() as t:
        t = os.path.realpath(t)
        def mk(*parts):
            d = os.path.join(t, *parts)
            os.makedirs(d, exist_ok=True)
            open(os.path.join(d, ARCHIVE), "wb").close()
            return d
        def pick(root, **env):
            saved = {k: os.environ.pop(k) for k in keys if k in os.environ}
            os.environ.update(env)
            try:
                big = find_archive(root_search(root, []))
            finally:
                for k in keys:
                    os.environ.pop(k, None)
                os.environ.update(saved)
            return big and os.path.normpath(os.path.dirname(big))
        run, sib, inf = os.path.join(t, "a", "run"), mk("a", "GeneralsXPatch"), mk("a", "run", "GeneralsXPatch")
        assets, nobig = mk("assets"), os.path.join(t, "nobig")
        os.makedirs(nobig)
        assert pick(run) == inf, "both overlays: the engine's <exe dir>/GeneralsXPatch, not the sibling"
        assert pick(run, GENERALSX_PATCH_PATH=sib) == sib, "GENERALSX_PATCH_PATH wins (the deploy run.sh)"
        assert pick(run, GENERALSX_PATCH_PATH=" '%s' " % sib) == sib, "the engine trims and unquotes it"
        assert pick(run, GENERALSX_PATCH_PATH="../GeneralsXPatch") == sib, "relative to the peer's cwd"
        assert pick(run, GENERALSX_PATCH_PATH=os.path.join(t, "gone")) is None, "a missing overlay is OFF"
        assert pick(run, CNC_GENERALS_ZH_PATH=assets) == assets, "the asset root mounts first; a tie keeps it"
        assert pick(run, CNC_GENERALS_ZH_PATH=nobig) == inf, "an asset root with no archives is skipped"
        assert pick(run, CNC_ZH_INSTALLPATH=assets) == assets, "the legacy asset variable"
        mk("a", "run")
        assert pick(run) == run, "archives in the executable's folder are the primary root"
        assert pick(run, CNC_GENERALS_ZH_PATH=nobig) == run
        os.remove(os.path.join(run, ARCHIVE))
        os.remove(os.path.join(inf, ARCHIVE))
        # Codex batch-r3: the engine never mounts the sibling on its own, so neither does the search. A launcher that
        # mounts it says so in GENERALSX_PATCH_PATH (run.sh, and deploy-*-zh.sh for this call); go.bat does not.
        assert pick(run) is None, "no engine default and no GENERALSX_PATCH_PATH: the sibling is NOT the overlay"
    print("selftest OK")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out")
    ap.add_argument("--search", action="append", default=[])
    ap.add_argument("--root", action="append", default=[])
    ap.add_argument("--if-archive", action="store_true")
    ap.add_argument("--no-scb", action="store_true", help="skip the SkirmishScripts.scb output (verify-bundle-overlay checks it against the pinned sha256 instead)")
    ap.add_argument("--game-dir", action="append", default=[], help="a retail game folder that may hold Data/Scripts/SkirmishScripts.scb (the packagers pass the folders they already resolve the archives from)")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--list", action="store_true", help="print every generated path below the game folder, one per line")
    a = ap.parse_args()
    if a.list:
        print("\n".join(OUTPUTS))
        return
    if a.out and a.root:
        ap.error("--out and --root are exclusive")
    if a.selftest:
        selftest()
    elif a.out:
        build(a.search, a.out, retail_extra=a.game_dir, no_scb=a.no_scb)
    elif a.root:
        build_roots(a.root, a.search, a.if_archive, a.game_dir, a.no_scb)
    else:
        ap.error("--out, --root or --selftest")


if __name__ == "__main__":
    main()
