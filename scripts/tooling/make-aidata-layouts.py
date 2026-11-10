#!/usr/bin/env python3
# GeneralsX: Data/INI/AIData.ini: the skirmish AI's base layouts, drawn so production doors face the base front. A RECIPE:
# which entries, in which order, with which flags are read from the Community Patch's own Data\INI\Default\AIData.ini in the player's or packager's
# archive; only the coordinates and angles below are ours.
#
#   make-aidata-layouts.py --out <AIData.ini> --search <dir> [--search <dir>]... [--only <side>[,<side>]...] [--keys both|doors|layout|none] [--coords new|stock]
#   make-aidata-layouts.py --lint --search <dir>
#   make-aidata-layouts.py --selftest [--search <dir>]
#   make-aidata-layouts.py --list
#
# wh1ter0se 03/11/2026 ("look at the rotation facing back of the base"), go 04/11. Plan and registration: gx-handoff ai/base-layout/PLAN_base-layout.md.
# The stock lists draw doors all round the command centre, and their footprints overlap (the minimum gap is 0 in all 12 lists), so the AI has no room to
# turn a door. Here, in a frame with the command centre at the origin and +X at the base front:
#   * doors in a front row, each with a clear exit lane and room for R's three tries;
#   * a supply yard behind the command centre kept EMPTY: wh1ter0se's maps put the close dock there (Twin Ridges 216 wu at +/-125..170 deg off the front
#     axis, Crater Ring 190 wu at +/-165..180), and so is China's pro supply spot;
#   * airfields on the flanks, static defences outward, power and tech on the flanks and behind the yard.
# Two list keys switch the engine's half on (AISkirmishPlayer::adjustBuildList, AIPlayer.cpp's door rule): GXTurnLayout places the list rigidly, spots AND
# angles, by the octant of the base front; GXFrontDoors is the door rule. A list without them takes the retail path, bit for bit.
#
# IT MOVES dataID (data-ini), and the engine half moves SimID, so the two ship together: a release engine only DEBUG_CRASHes on an unknown INI field
# (INI.cpp), so an old engine would apply these spots with the retail transform and no door rule.
#
# --only / --keys / --coords exist for the test arms (D-<general>, D-A, D-B, D1-ALL, D2-ALL, RED-key); what ships is the default: every list, both keys, new coordinates.
import argparse, hashlib, math, os, struct, sys, tempfile

ARCHIVE = "500_900_CommunityPatch_CoreINI.big"
SOURCE_MEMBER = "Data\\INI\\Default\\AIData.ini"
SOURCE_SHA256 = "e342ecadd88a0ef6b0ea1d7f85f37e3e81950c8862019b553865c4a38f5267d8"
OUTPUTS = ["Data/INI/AIData.ini"]

# Per side, in the stock list's order: (x, y, angle in degrees), command centre at the origin, +X = the base front.
LAYOUTS = {
    'America': [
        (0, 0, 0), (30, 370, 0), (330, -130, 0), (-400, -140, 0), (-400, 140, 0), (240, 300, 0),
        (380, 0, 0), (-165, 335, 0), (95, 210, 0), (-370, 0, 0), (-140, -270, 0), (-310, -130, 0),
        (-310, 130, 0), (-270, -250, 0), (-10, -360, 0), (215, 0, 0), (95, -210, 0), (295, 135, 45),
    ],
    'AmericaAirForceGeneral': [
        (0, 0, 0), (95, -210, 0), (-380, 180, 0), (-20, 165, 0), (150, -370, 0), (30, 410, 0),
        (370, 130, 0), (110, 410, 0), (-340, 320, 0), (-380, -190, 0), (385, 0, 0), (40, -370, 0),
        (320, -150, 0), (250, -290, 0), (215, 0, 0), (170, 235, 0), (-165, 335, 0), (-165, -335, 0),
        (-405, 0, 0),
    ],
    'AmericaSuperWeaponGeneral': [
        (0, 0, 0), (-165, 335, 0), (95, -210, 0), (-370, 0, 0), (-350, -140, 0), (-350, 140, 0),
        (70, 300, 0), (-20, 165, 0), (240, -300, 0), (240, 300, 0), (-490, 0, 0), (-470, -140, 0),
        (-470, 140, 0), (-190, -420, 0), (-140, -270, 0), (80, -390, 0), (-10, -350, 0), (-340, 270, 0),
        (-100, -410, 0), (215, 0, 0), (295, 135, 45), (295, -135, -45), (180, 385, 45), (180, -385, -45),
        (380, 0, 0), (-300, -280, 0),
    ],
    'AmericaLaserGeneral': [
        (0, 0, 0), (-370, 0, 0), (95, 210, 0), (-165, 335, 0), (-20, -165, 0), (330, -130, 0),
        (30, 370, 0), (-400, -140, 0), (-400, 140, 0), (240, 300, 0), (-140, -270, 0), (-310, -130, 0),
        (-310, 130, 0), (-270, -250, 0), (-10, -360, 0), (215, 0, 0), (140, -200, 0),
    ],
    'China': [
        (0, 0, 0), (-300, 160, 0), (215, 0, 0), (95, 210, 0), (40, 360, 0), (-20, -370, 0),
        (-250, 300, 0), (-310, -180, 0), (295, 135, 45), (295, -135, -45), (-105, 350, 0), (-130, -290, 0),
        (-390, 160, 0), (-240, -340, 0), (-340, 0, 0), (180, 385, 45), (180, -385, -45), (95, -210, 0),
    ],
    'ChinaInfantryGeneral': [
        (0, 0, 0), (95, -210, 0), (95, 210, 0), (-55, -375, 0), (-55, 375, 0), (-250, 350, 0),
        (215, 0, 0), (-200, -310, 0), (-320, -180, 0), (-350, 0, 0), (295, 135, 45), (295, -135, -45),
        (180, 385, 45), (180, -385, -45), (-300, 160, 0), (-390, 150, 0), (380, 40, 0), (270, -300, 0),
        (270, 300, 0), (390, -100, 0),
    ],
    'ChinaTankGeneral': [
        (0, 0, 0), (-55, 375, 0), (-250, 350, 0), (-350, 0, 0), (-320, -180, 0), (380, 40, 0),
        (-220, -340, 0), (-300, 160, 0), (270, -300, 0), (270, 300, 0), (390, -100, 0), (-400, 170, 0),
        (295, 135, 45), (295, -135, -45), (180, 385, 45), (180, -385, -45), (215, 0, 0), (95, -210, 0),
        (95, 210, 0), (-55, -375, 0),
    ],
    'ChinaNukeGeneral': [
        (0, 0, 0), (-105, 350, 0), (95, 210, 0), (-130, -290, 0), (-340, 0, 0), (-320, -180, 0),
        (40, 360, 0), (-20, -370, 0), (-240, -340, 0), (380, 40, 0), (-250, 340, 0), (-410, 160, 0),
        (270, -300, 0), (-310, 180, 0), (295, 135, 45), (295, -135, -45), (180, 385, 45), (180, -385, -45),
        (215, 0, 0), (95, -210, 0),
    ],
    'GLA': [
        (0, 0, 0), (215, 0, 0), (95, -210, 0), (-55, -375, 0), (95, 210, 0), (-310, -160, 0),
        (-310, 160, 0), (-230, -300, 0), (10, 380, 0), (-130, 290, 0), (-340, 0, 0), (295, 135, 45),
        (-270, 300, 0), (295, -135, -45),
    ],
}
for _g in ("GLADemolitionGeneral", "GLAStealthGeneral", "GLAToxinGeneral"):
    LAYOUTS[_g] = LAYOUTS["GLA"]   # the same list with its own prefix

# GeneralsX 06/11/2026 (gx-handoff ai/base-layout/ADDENDUM_10.md (b), lead C71): the lists the shipped file leaves to the Community Patch. Their $10k
# screen cells cost (Air Force $10k FAIL / FAIL; Demolition $10k FAIL / UNKNOWN, a deficit already on the ADDENDUM_7 head), so they ship with
# stock placement until a mechanism probe explains it. A side absent from the file keeps the Community Patch's list, and a list without the keys
# runs the retail path bit for bit, so these two are inert by construction. --only still writes them (test arms).
SHIP_OFF = ("AmericaAirForceGeneral", "GLADemolitionGeneral")

# Footprints from the Community Patch's object INIs: half x, half y, rally point y (None: no production exit). The same for every general's prefix.
KINDS = {
    "AmericaAirfield": (112, 74, None), "AmericaBarracks": (55, 45, 0), "AmericaCommandCenter": (60, 70, 35), "AmericaParticleCannonUplink": (64, 38, None),
    "AmericaPowerPlant": (22, 30, None), "AmericaStrategyCenter": (62, 44, None), "AmericaSupplyDropZone": (27, 27, None), "AmericaWarFactory": (53, 60, -30),
    "AmericaPatriotBattery": (12, 12, None), "GLAArmsDealer": (40, 62, -15), "GLABarracks": (42, 52, 0), "GLABlackMarket": (35, 35, None),
    "GLACommandCenter": (65, 65, 0), "GLAPalace": (43, 58, None), "GLAScudStorm": (71, 67, None), "GLATunnelNetwork": (25, 25, None), "ChinaAirfield": (83, 76, None),
    "ChinaBarracks": (36, 44, -25), "ChinaCommandCenter": (60, 63, 40), "ChinaInternetCenter": (50, 50, None), "ChinaNuclearMissileLauncher": (45, 55, None),
    "ChinaPowerPlant": (27, 34, None), "ChinaPropagandaCenter": (33, 57, None), "ChinaSpeakerTower": (6, 6, None), "ChinaWarFactory": (57, 61, -27),
}
PREFIXES = ("AirF_", "SupW_", "Lazr_", "Infa_", "Tank_", "Nuke_", "Demo_", "Slth_", "Chem_")
MIN_GAP, MAX_CENTRE, MAX_RADIUS_X = 16.0, 100.0, 1.35   # the plan's section 2.2
RADIUS_X_EXCEPT = {"AmericaSuperWeaponGeneral": 1.40}   # 26 entries under the engine's padded rule: ADDENDUM_1, section 2
# BuildAssistant::isLocationClearOfObjects for a skirmish AI placing a building beside one that stands: its own box grows 3 cells each way (a cylinder: a box of r + 30), the
# standing box's minor radius takes the same 30 (a retail quirk: hisBounds + myExtraWidth), a standing cylinder stays a box of r, and the standing factory's exit rectangle (25 wu
# deep, its full width) counts as well. Fails it and the AI starts its position search on flat, empty ground.
ENGINE_PAD, ENGINE_MARGIN = 30.0, 2.0
EXIT_WIDTH = 25.0
EXITS = ("AmericaAirfield", "AmericaBarracks", "AmericaCommandCenter", "AmericaWarFactory", "GLAArmsDealer", "GLABarracks", "GLACommandCenter", "ChinaBarracks", "ChinaCommandCenter", "ChinaWarFactory")
ROUND = ("AmericaPatriotBattery", "GLATunnelNetwork")
YARD_RADIUS, YARD_BEARING = 300.0, 110.0
# The defence ring (AISkirmishPlayer::buildAIBaseDefenseStructure) stands at its radius + SkirmishBaseDefenseExtraDistance from the base centre. Front requests stay near the front, but flank and backdoor
# requests aim at the map's own paths and land on nearly every bearing (ring probe: every door try was a flank try), so ring_radius judges the whole circle. A ring pushed out onto worse ground is refused on
# terrain, a ring pulled in onto the list's own buildings is refused on objects, and each refusal spends arc. RING_DOOR is the widest war factory, RING_SHARE the share of the circle it may be refused on (ADDENDUM_3).
RING_EXTRA, RING_DOOR, RING_SHARE = 150.0, "ChinaWarFactory", 0.10
# A ring try: the outward octant first (gxOctantAngle), then the template's view angle, which stays fixed to the world while a keyed list turns by any octant. RotateSkirmishBases is NO in the stock data, so stock is always retail_frame.
RING_VIEW, RING_TURNS, RING_TAN = -135.0, (-135.0, -90.0, -45.0, 0.0, 45.0, 90.0, 135.0, 180.0), 0.41421356
LANE_AHEAD, LANE_HALF = 90.0, 30.0


def kind(t):
    return t.split("_", 1)[1] if t.startswith(PREFIXES) else t


def is_door(t):
    return kind(t).endswith(("Barracks", "WarFactory", "ArmsDealer"))


def is_cc(t):
    return kind(t).endswith("CommandCenter")


def is_static_defence(t):
    return kind(t).endswith(("PatriotBattery", "TunnelNetwork", "SpeakerTower"))


def read_member(big_path, member):
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


def stock_text(search):
    big = next((b for b in (os.path.join(d, ARCHIVE) for d in search) if os.path.isfile(b)), None)
    if big is None:
        raise SystemExit("ERROR: no %s in %s" % (ARCHIVE, ", ".join(search) or "(no --search)"))
    src = read_member(big, SOURCE_MEMBER)
    got = hashlib.sha256(src).hexdigest()
    if got != SOURCE_SHA256:
        raise SystemExit("ERROR: %s in %s hashes %s, not %s.\n  A different Community Patch release: re-derive the layouts against it before shipping." % (SOURCE_MEMBER, big, got, SOURCE_SHA256))
    return src.decode("latin-1")


def parse_lists(text):
    """{side: [entry]} in file order; an entry = dict(tmpl, x, y, ang, reb, init, auto), the strings as written."""
    lists, cur, ent = {}, None, None
    for raw in text.splitlines():
        line = raw.split(";")[0].strip()
        if not line:
            continue
        toks = line.split()
        k = toks[0].lower()
        if k == "skirmishbuildlist":
            cur = toks[1]
            lists[cur] = []
        elif cur is None:
            continue
        elif k == "structure":
            ent = dict(tmpl=toks[1])
            lists[cur].append(ent)
        elif k == "end":
            if ent is not None:
                ent = None
            else:
                cur = None
        elif ent is not None:
            if k == "location":
                ent["x"] = float(toks[2].split(":")[1])
                ent["y"] = float(toks[3].split(":")[1])
            elif k == "angle":
                ent["ang"] = float(toks[2])
            elif k == "rebuilds":
                ent["reb"] = toks[2]
            elif k == "initiallybuilt":
                ent["init"] = toks[2]
            elif k == "automaticallybuild":
                ent["auto"] = toks[2]
            elif k == "gxfallbacklocation":
                ent["fb"] = (float(toks[2].split(":")[1]), float(toks[3].split(":")[1]))
            elif k == "gxfallbackangle":
                ent["fbang"] = float(toks[2])
    return lists


def new_entries(side, stock):
    """The stock entries (names, order, flags) at this side's new coordinates, relative to the command centre."""
    rows = LAYOUTS[side]
    if len(rows) != len(stock):
        raise SystemExit("ERROR: %s has %d stock entries and %d layout rows" % (side, len(stock), len(rows)))
    return [dict(e, x=float(r[0]), y=float(r[1]), ang=float(r[2])) for e, r in zip(stock, rows)]


def fallback_spots(keyed, stock):
    """ADDENDUM_8 (wh1ter0se's option 2, lead C70): per keyed entry, the spot the stock list gives the same template, the k-th occurrence to the k-th: (x, y, angle)
    as the stock list writes them, or None when the stock list has no k-th one of that template (never a guess)."""
    seen, out = {}, []
    for e in keyed:
        k = seen.get(e["tmpl"], 0)
        seen[e["tmpl"]] = k + 1
        same = [o for o in stock if o["tmpl"] == e["tmpl"]]
        out.append((same[k]["x"], same[k]["y"], same[k]["ang"]) if k < len(same) else None)
    return out


def generate(lists, only=None, keys="both", coords="new", off=SHIP_OFF):
    out = ["; GeneralsX: the skirmish AI's base layouts. Generated by scripts/tooling/make-aidata-layouts.py; edit the recipe there, never this file.",
           "AIData"]
    for side in lists:
        if only is not None and side not in only:
            continue
        if only is None and side in off:
            continue   # left to the Community Patch (SHIP_OFF)
        stock = lists[side]
        cc = next(e for e in stock if is_cc(e["tmpl"]))
        if coords == "new":
            ents = [dict(e, x=e["x"] + cc["x"], y=e["y"] + cc["y"]) for e in new_entries(side, stock)]
        else:
            ents = stock
        out.append("  SkirmishBuildList %s" % side)
        if keys in ("both", "layout"):
            out.append("    GXTurnLayout = Yes")
        if keys in ("both", "doors"):
            out.append("    GXFrontDoors = Yes")
        if keys in ("both", "layout") and coords == "new":
            out.append("    GXRingRadius = %d" % math.ceil(ring_radius(new_entries(side, stock), retail_frame(stock))))
        # GXFallbackLocation / GXFallbackAngle (ADDENDUM_8): where the stock list builds the same template, in the list's own frame (the command centre keeps
        # its stock location), so adjustBuildList places it as retail does. Read only on a GXTurnLayout list, so written only there.
        fbs = fallback_spots(ents, stock) if keys in ("both", "layout") else [None] * len(ents)
        for e, fb in zip(ents, fbs):
            out += ["    Structure %s" % e["tmpl"],
                    "      Location = X:%.2f Y:%.2f" % (e["x"], e["y"]),
                    "      Rebuilds = %s" % e["reb"],
                    "      Angle = %.2f" % e["ang"],
                    "      InitiallyBuilt = %s" % e["init"],
                    "      AutomaticallyBuild = %s" % e["auto"]]
            if fb is not None:
                out += ["      GXFallbackLocation = X:%.2f Y:%.2f" % (fb[0], fb[1]),
                        "      GXFallbackAngle = %.2f" % fb[2]]
            out.append("    END")
        out.append("  END")
    out.append("End")
    return ("\r\n".join(out) + "\r\n").encode("latin-1")


def corners(e, a, b, ang=None):
    r = math.radians(e["ang"] if ang is None else ang)
    c, s = math.cos(r), math.sin(r)
    return [(e["x"] + dx * c - dy * s, e["y"] + dx * s + dy * c) for dx, dy in ((a, b), (-a, b), (-a, -b), (a, -b))]


def overlap(p, q):
    for poly in (p, q):
        for i in range(4):
            x1, y1 = poly[i]
            x2, y2 = poly[(i + 1) % 4]
            nx, ny = y2 - y1, x1 - x2
            pa = [x * nx + y * ny for x, y in p]
            qa = [x * nx + y * ny for x, y in q]
            if max(pa) < min(qa) or max(qa) < min(pa):
                return False
    return True


def engine_clear(stands, new):
    """isLocationClearOfObjects: may `new` be placed while `stands` stands?"""
    k = kind(stands["tmpl"])
    a, b = KINDS[k][:2]
    his = (a, b) if k in ROUND else (a, b + ENGINE_PAD)
    c, d = KINDS[kind(new["tmpl"])][:2]
    me = corners(new, c + ENGINE_PAD + ENGINE_MARGIN, d + ENGINE_PAD + ENGINE_MARGIN)
    if overlap(me, corners(stands, his[0], his[1])):
        return False
    if k in EXITS:
        r = math.radians(stands["ang"])
        off = a + EXIT_WIDTH / 2
        rect = dict(x=stands["x"] + math.cos(r) * off, y=stands["y"] + math.sin(r) * off, ang=stands["ang"])
        if overlap(me, corners(rect, EXIT_WIDTH / 2, b)):
            return False
    return True


def base_stats(entries):
    """AIPlayer::computeCenterAndRadiusOfBase, list-relative."""
    n = len(entries)
    cx = sum(e["x"] for e in entries) / n
    cy = sum(e["y"] for e in entries) / n
    m = 0.0
    for e in entries:
        a, b = KINDS[kind(e["tmpl"])][:2]
        br = math.hypot(a, b) * 0.4
        m = max(m, (abs(e["x"] - cx) + br) ** 2 + (abs(e["y"] - cy) + br) ** 2)
    return cx, cy, math.sqrt(m)


def ring_octant(dx, dy):
    """gxOctantAngle, in degrees."""
    ax, ay = abs(dx), abs(dy)
    if ay <= ax * RING_TAN:
        return 0.0 if dx >= 0 else 180.0
    if ax <= ay * RING_TAN:
        return 90.0 if dy >= 0 else -90.0
    if dx >= 0:
        return 45.0 if dy >= 0 else -45.0
    return 135.0 if dy >= 0 else -135.0


def ring_refused(entries, dist, turns=RING_TURNS):
    """Share of the circle, one box per whole degree, where a Hard AI's war factory may not stand beside the whole list standing (engine_clear): the outward octant and the view angle both refused, at the worst of the list's turns."""
    cx, cy = base_stats(entries)[:2]
    c, d = KINDS[RING_DOOR][:2]
    door_reach = math.hypot(c + ENGINE_PAD + ENGINE_MARGIN, d + ENGINE_PAD + ENGINE_MARGIN)
    far = []
    for e in entries:
        a, b = KINDS[kind(e["tmpl"])][:2]
        far.append((e, door_reach + math.hypot(a + EXIT_WIDTH, b + ENGINE_PAD)))   # past this, engine_clear cannot refuse: it only saves time
    n = dict.fromkeys(turns, 0)
    for deg in range(360):
        t = math.radians(deg)
        x, y = cx + dist * math.cos(t), cy + dist * math.sin(t)
        near = [e for e, reach in far if math.hypot(e["x"] - x, e["y"] - y) <= reach]
        door = dict(tmpl=RING_DOOR, x=x, y=y, ang=ring_octant(x - cx, y - cy))
        if all(engine_clear(e, door) for e in near):
            continue
        for turn in turns:
            door = dict(tmpl=RING_DOOR, x=x, y=y, ang=RING_VIEW - turn)
            n[turn] += not all(engine_clear(e, door) for e in near)
    return max(n.values()) / 360.0


def ring_on_footprint(entries, dist):
    """Does the circle run inside a planned building's footprint: between the footprint's nearest and farthest point from the base centre?"""
    cx, cy = base_stats(entries)[:2]
    for e in entries:
        a, b = KINDS[kind(e["tmpl"])][:2]
        r = math.radians(e["ang"])
        dx, dy = cx - e["x"], cy - e["y"]
        lx, ly = abs(dx * math.cos(r) + dy * math.sin(r)), abs(dy * math.cos(r) - dx * math.sin(r))
        if math.hypot(max(lx - a, 0.0), max(ly - b, 0.0)) <= dist <= math.hypot(lx + a, ly + b):
            return True
    return False


def ring_radius(entries, stock):
    """GXRingRadius of a redrawn list: the smallest ring at or past stock's where a war factory is refused on at most RING_SHARE of the circle (or on what the stock list's own ring refuses, if more) and the circle runs inside no planned building; never past today's base radius."""
    stock_ring = base_stats(stock)[2] + RING_EXTRA
    end = math.ceil(base_stats(entries)[2] + RING_EXTRA)
    allow = max(RING_SHARE, ring_refused(stock, stock_ring, (0.0,)))
    dist = math.ceil(stock_ring)
    while dist < end and (ring_refused(entries, dist) > allow or ring_on_footprint(entries, dist)):
        dist += 1
    return min(dist, end) - RING_EXTRA


def lint(side, entries, stock):
    """The plan's section 2.2 rules; returns the failures. `stock` = the stock entries in the retail frame (spots turned 135 deg, angles as listed)."""
    bad = []
    if [e["tmpl"] for e in entries] != [e["tmpl"] for e in stock]:
        bad.append("ORDER differs from the stock list")
    if not is_cc(entries[0]["tmpl"]):
        bad.append("the first entry is no command centre (adjustBuildList moves nothing then)")
    for e, o in zip(entries, stock):
        if (e.get("reb"), e.get("init"), e.get("auto")) != (o.get("reb"), o.get("init"), o.get("auto")):
            bad.append("FLAG of %s differs from stock" % e["tmpl"])
    for i, e in enumerate(entries):
        a, b = KINDS[kind(e["tmpl"])][:2]
        p = corners(e, a + MIN_GAP / 2, b + MIN_GAP / 2)
        for s in entries[i + 1:]:
            c, d = KINDS[kind(s["tmpl"])][:2]
            if overlap(p, corners(s, c + MIN_GAP / 2, d + MIN_GAP / 2)):
                bad.append("GAP under %d between %s (%d,%d) and %s (%d,%d)" % (MIN_GAP, e["tmpl"], e["x"], e["y"], s["tmpl"], s["x"], s["y"]))
            if not (engine_clear(e, s) and engine_clear(s, e)):
                bad.append("ENGINE refuses %s (%d,%d) beside %s (%d,%d)" % (e["tmpl"], e["x"], e["y"], s["tmpl"], s["x"], s["y"]))
        if is_cc(e["tmpl"]):
            continue
        for x, y in corners(e, a, b):
            if math.hypot(x, y) < YARD_RADIUS and abs(math.degrees(math.atan2(y, x))) > YARD_BEARING:
                bad.append("YARD holds %s (%d,%d)" % (e["tmpl"], e["x"], e["y"]))
                break
        if is_static_defence(e["tmpl"]):
            want = round(math.degrees(math.atan2(e["y"], e["x"])) / 45.0) * 45
            if abs((e["ang"] - want + 180) % 360 - 180) > 0.5:
                bad.append("STATIC defence %s (%d,%d) at %d, outward is %d" % (e["tmpl"], e["x"], e["y"], e["ang"], want))
        if is_door(e["tmpl"]):
            rally_y = KINDS[kind(e["tmpl"])][2]
            line_y = e["y"] + rally_y
            for s in entries:
                if s is e:
                    continue
                c, d = KINDS[kind(s["tmpl"])][:2]
                if s["x"] + c > e["x"] + a and s["x"] - c < e["x"] + a + LANE_AHEAD and s["y"] + d > line_y - LANE_HALF and s["y"] - d < line_y + LANE_HALF:
                    bad.append("LANE of %s (%d,%d) blocked by %s (%d,%d)" % (e["tmpl"], e["x"], e["y"], s["tmpl"], s["x"], s["y"]))
                # R may pick either turn, and a turned door's padded box is what the next building meets: both build orders, a turned neighbour door too
                for turn in (45, -45):
                    t = dict(e, ang=e["ang"] + turn)
                    others = [s] + ([dict(s, ang=s["ang"] + 45), dict(s, ang=s["ang"] - 45)] if is_door(s["tmpl"]) else [])
                    if not all(engine_clear(t, o) and engine_clear(o, t) for o in others):
                        bad.append("TURN %+d of %s (%d,%d) meets %s (%d,%d)" % (turn, e["tmpl"], e["x"], e["y"], s["tmpl"], s["x"], s["y"]))
    cx, cy, r = base_stats(entries)
    _, _, stock_r = base_stats(stock)
    if math.hypot(cx, cy) > MAX_CENTRE:
        bad.append("CENTRE (%d,%d) over %d wu" % (cx, cy, MAX_CENTRE))
    limit = RADIUS_X_EXCEPT.get(side, MAX_RADIUS_X)
    if r > limit * stock_r:
        bad.append("RADIUS %d over %.2f x stock %d" % (r, limit, stock_r))
    return bad


def retail_frame(stock):
    """The stock list as the retail transform places it: spots turned 135 deg about the command centre, angles untouched."""
    cc = next(e for e in stock if is_cc(e["tmpl"]))
    c, s = math.cos(math.radians(135)), math.sin(math.radians(135))
    return [dict(e, x=(e["x"] - cc["x"]) * c - (e["y"] - cc["y"]) * s, y=(e["y"] - cc["y"]) * c + (e["x"] - cc["x"]) * s) for e in stock]


def lint_all(lists):
    fails = 0
    for side, stock in lists.items():
        bad = lint(side, new_entries(side, stock), retail_frame(stock))
        cx, cy, r = base_stats(new_entries(side, stock))
        _, _, sr = base_stats(retail_frame(stock))
        print("  %-27s n=%2d centre (%4d,%4d) radius %3d (stock %3d, x%.2f) %s" % (side, len(stock), cx, cy, r, sr, r / sr, "clean" if not bad else "; ".join(bad)))
        fails += len(bad)
    return fails


def write(out, result):
    if os.path.isfile(out) and open(out, "rb").read() == result:
        print("  unchanged  %s  %s" % (out, hashlib.sha256(result).hexdigest()[:16]))
        return
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(os.path.abspath(out)), suffix=".partial")
    with os.fdopen(fd, "wb") as f:
        f.write(result)
    os.replace(tmp, out)
    print("  wrote      %s  %s" % (out, hashlib.sha256(result).hexdigest()[:16]))


def selftest(search):
    """Each rule is seen to FAIL on a mutant of a clean list; generate() round-trips; a stock-keyed arm keeps stock numbers."""
    def entry(t, x, y, ang=0.0):
        return dict(tmpl=t, x=float(x), y=float(y), ang=float(ang), reb="0", init="No", auto="No")
    clean = [entry("AmericaCommandCenter", 0, 0), entry("AmericaWarFactory", 215, 0), entry("AmericaBarracks", 95, -210), entry("AmericaWarFactory", 95, 210),
             entry("AmericaPowerPlant", -150, -320), entry("AmericaPatriotBattery", 330, 170, 45), entry("AmericaPowerPlant", -340, -70), entry("AmericaPowerPlant", -340, 70)]
    stock = [dict(e, x=e["x"], y=e["y"]) for e in clean]
    assert lint("T", clean, stock) == [], lint("T", clean, stock)

    def mutant(word, change):
        m = [dict(e) for e in clean]
        change(m)
        got = lint("T", m, stock)
        assert any(word in g for g in got), (word, got)

    mutant("GAP", lambda m: m[4].update(x=-40, y=60))                    # a footprint on the command centre
    mutant("ENGINE", lambda m: m[4].update(x=0, y=-130))                 # clear of the 16 wu gap, refused by the engine's padded boxes: (0, +/-130)
    mutant("ENGINE", lambda m: m[4].update(x=160, y=-30))                # the same with a door's exit rectangle in front of the command centre's row
    mutant("LANE", lambda m: m[4].update(x=330, y=-30))                  # a power plant in front of a door
    assert not any("ENGINE" in g for g in lint("T", [dict(e, x=-25, y=-215) if i == 4 else dict(e) for i, e in enumerate(clean)], stock)), "the TURN mutant must be clear at its list angle"
    mutant("TURN", lambda m: m[4].update(x=-25, y=-215))                 # beside a door: clear at 0, met at +/-45 (shape)
    mutant("ORDER", lambda m: m.__setitem__(4, entry("AmericaStrategyCenter", -40, -250)))
    mutant("ORDER", lambda m: m.reverse())
    mutant("no command centre", lambda m: m.__setitem__(0, entry("AmericaBarracks", 0, 0)))
    mutant("FLAG", lambda m: m[1].update(auto="Yes"))
    mutant("YARD", lambda m: m[4].update(x=-200, y=0))
    mutant("STATIC", lambda m: m[5].update(ang=0))
    far = [dict(e) for e in clean]
    far[4].update(x=-1500, y=0)
    assert any("CENTRE" in g or "RADIUS" in g for g in lint("T", far, stock)), lint("T", far, stock)

    text = """AIData
  SkirmishBuildList FactionX ; a comment
    Structure AmericaCommandCenter
      Location = X:500.00 Y:400.00
      Rebuilds = -1
      Angle = -135.00
      InitiallyBuilt = No
      AutomaticallyBuild = Yes
    END ;Structure
    Structure AmericaBarracks
      Location = X:600.50 Y:400.00
      Rebuilds = 0
      Angle = 45.00
      InitiallyBuilt = No
      AutomaticallyBuild = No
    END
  END
End
"""
    lists = parse_lists(text)
    assert [e["tmpl"] for e in lists["FactionX"]] == ["AmericaCommandCenter", "AmericaBarracks"] and lists["FactionX"][1]["x"] == 600.5
    LAYOUTS["FactionX"] = [(0, 0, 0), (190, 0, 0)]
    both = parse_lists(generate(lists).decode("latin-1"))["FactionX"]
    assert (both[0]["x"], both[0]["y"]) == (500.0, 400.0), "the command centre keeps its list location: adjustBuildList subtracts it"
    assert (both[1]["x"], both[1]["y"], both[1]["ang"]) == (690.0, 400.0, 0.0), both[1]
    assert (both[0]["reb"], both[0]["auto"], both[1]["auto"]) == ("-1", "Yes", "No"), both
    out = generate(lists, keys="both").decode("latin-1")
    assert "GXTurnLayout = Yes" in out and "GXFrontDoors = Yes" in out
    assert "GXTurnLayout" not in generate(lists, keys="doors").decode("latin-1") and "GXFrontDoors" in generate(lists, keys="doors").decode("latin-1")
    assert "GXFrontDoors" not in generate(lists, keys="layout").decode("latin-1") and "GXTurnLayout" in generate(lists, keys="layout").decode("latin-1")
    assert "GXRingRadius" in out and "GXRingRadius" in generate(lists, keys="layout").decode("latin-1"), "a list that moves its spots carries the ring"
    assert "GXRingRadius" not in generate(lists, keys="doors").decode("latin-1"), "stock spots, stock ring"
    assert "GXRingRadius" not in generate(lists, keys="both", coords="stock").decode("latin-1"), "stock spots, stock ring"
    # ring_radius: the stock ring on a clear circle; out past a building the circle would run through, on ANY bearing; never past the base radius; the share of the circle decides
    far_stock = [entry("AmericaCommandCenter", 0, 0), entry("AmericaPowerPlant", -600, 0)]
    ring = lambda ents, st=far_stock: ring_radius(ents, st) + RING_EXTRA
    stock_ring = math.ceil(base_stats(far_stock)[2] + RING_EXTRA)
    assert ring(far_stock) == stock_ring, ("a clear circle keeps the stock ring", ring(far_stock), stock_ring)
    # a base smaller than stock: its own ring, the ring never moves out
    tiny = [entry("AmericaCommandCenter", 0, 0), entry("AmericaPowerPlant", -40, 0)]
    assert ring(tiny) == math.ceil(base_stats(tiny)[2] + RING_EXTRA) < stock_ring, "never past the base radius"
    # one building ON the circle, front, flank and rear alike, a share too small to refuse (four more, near the centre, keep the centre at zero): the ring moves just past that building
    for deg in (0, 90, 135, 180, 270):
        x, y = math.cos(math.radians(deg)) * stock_ring, math.sin(math.radians(deg)) * stock_ring
        far = [entry("AmericaCommandCenter", 0, 0), entry("AmericaPatriotBattery", x, y)] + [entry("AmericaPatriotBattery", -x / 4, -y / 4) for _ in range(4)]
        got = ring(far)
        assert ring_on_footprint(far, stock_ring) and ring_refused(far, got) <= RING_SHARE, ("the test's building stands on the stock ring, under the share", deg, got)
        assert got > stock_ring and not ring_on_footprint(far, got) and ring_on_footprint(far, got - 1), ("the smallest ring off the footprint, on every bearing", deg, got)
    # a circle of war factories: no footprint on the ring, but the share refuses: the smallest ring under the limit
    wide = [entry("AmericaCommandCenter", 0, 0)] + [entry("ChinaWarFactory", math.cos(math.radians(k * 90)) * 345, math.sin(math.radians(k * 90)) * 345, k * 90) for k in range(4)]
    got = ring(wide)
    assert not ring_on_footprint(wide, stock_ring) and got < math.ceil(base_stats(wide)[2] + RING_EXTRA), ("the share is what decides here", got)
    assert got > stock_ring and ring_refused(wide, got) <= RING_SHARE < ring_refused(wide, got - 1), ("the smallest ring under the share", got, ring_refused(wide, got), ring_refused(wide, got - 1))
    # what the stock list's own ring refuses is allowed: the same list as stock keeps the stock ring however full its circle
    full = [entry("AmericaCommandCenter", 0, 0)] + [entry("AmericaAirfield", math.cos(math.radians(k * 45)) * 345, math.sin(math.radians(k * 45)) * 345) for k in range(8)]
    full_ring = math.ceil(base_stats(full)[2] + RING_EXTRA)
    assert ring_refused(full, base_stats(full)[2] + RING_EXTRA, (0.0,)) > RING_SHARE and ring(full, full) == full_ring, ("the stock list's own share is the limit", ring_refused(full, full_ring), ring(full, full), full_ring)
    # the same list against a stock whose ring is clear: the share is never met, and the ring stops at the base radius
    assert ring(full) == full_ring, ("never past the base radius, the share unmet", ring(full), full_ring)
    # a footprint follows its building's angle: this airfield's long side points along the bearing from the base centre, so the circle reaches it 97 past its centre; across the bearing it would not
    tilt = [entry("AmericaCommandCenter", 0, 0), entry("AmericaAirfield", 400, 400, 45)]
    assert ring_on_footprint(tilt, math.hypot(200, 200) + 97) and not ring_on_footprint([tilt[0], dict(tilt[1], ang=-45.0)], math.hypot(200, 200) + 97), "the footprint turns with the building"
    # the circle between two whole degrees still counts, and a footprint the circle only encloses does not
    tower = [entry("ChinaSpeakerTower", 505.9566, 6.6233), entry("ChinaSpeakerTower", -505.9566, -6.6233)]
    assert ring_on_footprint(tower, 500.0) and not ring_on_footprint(tower, 490.0) and not ring_on_footprint(tower, 520.0), "the whole circle, not its whole degrees"
    # gxOctantAngle's eight answers, and its boundary
    for (dx, dy), want in (((1, 0), 0.0), ((-1, 0), 180.0), ((0, 1), 90.0), ((0, -1), -90.0), ((1, 1), 45.0), ((1, -1), -45.0), ((-1, 1), 135.0), ((-1, -1), -135.0), ((1, 0.41), 0.0), ((1, 0.42), 45.0), ((0.41, 1), 90.0), ((0.42, 1), 45.0)):
        assert ring_octant(dx, dy) == want, (dx, dy, want)
    # a door is refused only when BOTH tries are, at the worst turn: a long building off a whole degree's octant box but on the view box. One standing building, its box against the door's at each angle
    one = [entry("AmericaAirfield", 0, 0)]
    both = sum(1 for deg in range(360) if not engine_clear(one[0], dict(tmpl=RING_DOOR, x=210 * math.cos(math.radians(deg)), y=210 * math.sin(math.radians(deg)), ang=ring_octant(math.cos(math.radians(deg)), math.sin(math.radians(deg))))) and not engine_clear(one[0], dict(tmpl=RING_DOOR, x=210 * math.cos(math.radians(deg)), y=210 * math.sin(math.radians(deg)), ang=RING_VIEW)))
    assert abs(ring_refused(one, 210, (0.0,)) - both / 360.0) < 1e-9 and 0 < both < 360, ("octant, then the view angle", both, ring_refused(one, 210, (0.0,)))
    assert ring_refused(one, 210) >= ring_refused(one, 210, (0.0,)), "the worst turn"
    none = generate(lists, keys="none").decode("latin-1")
    assert "GXTurnLayout" not in none and "GXFrontDoors" not in none
    st = parse_lists(generate(lists, coords="stock").decode("latin-1"))["FactionX"]
    assert (st[1]["x"], st[1]["ang"]) == (600.5, 45.0), "the RED-key and D1-ALL arms keep the stock numbers"
    assert "FactionX" not in generate(lists, only=["Other"]).decode("latin-1")
    # SHIP_OFF: a side named there is absent from the default (shipped) output and present when asked for by --only
    assert "SkirmishBuildList FactionX" not in generate(lists, off=("FactionX",)).decode("latin-1"), "an OFF side is left to the Community Patch"
    assert "SkirmishBuildList FactionX" in generate(lists, only=["FactionX"], off=("FactionX",)).decode("latin-1"), "--only still writes an OFF side"
    assert "SkirmishBuildList FactionX" in generate(lists, off=()).decode("latin-1")
    # ADDENDUM_8 fallbacks: the k-th to the k-th, none past the stock list's count (condition 2), never the i-th or the first
    fs = [entry("AmericaCommandCenter", 0, 0), entry("AmericaPowerPlant", -10, -20, 5), entry("AmericaBarracks", 30, 40, 15), entry("AmericaPowerPlant", -50, -60, 25),
          entry("AmericaWarFactory", 70, 80, 35), entry("AmericaWarFactory", 90, 100, 45)]
    fk = [entry("AmericaCommandCenter", 1, 1), entry("AmericaPowerPlant", 2, 2), entry("AmericaPowerPlant", 3, 3), entry("AmericaPowerPlant", 4, 4),
          entry("AmericaWarFactory", 5, 5), entry("AmericaBarracks", 6, 6), entry("AmericaStrategyCenter", 7, 7)]
    want = [(0.0, 0.0, 0.0), (-10.0, -20.0, 5.0), (-50.0, -60.0, 25.0), None, (70.0, 80.0, 35.0), (30.0, 40.0, 15.0), None]
    assert fallback_spots(fk, fs) == want, ("more power plants than stock: the 3rd has none; fewer war factories: the 1st maps to the 1st; a template stock lacks: none", fallback_spots(fk, fs))

    def fb_mutant(name, fn):
        got = fn(fk, fs)
        assert got != want, ("the selftest must see this mapping fail", name, got)
    fb_mutant("i-th", lambda k, s: [(s[i]["x"], s[i]["y"], s[i]["ang"]) if i < len(s) else None for i in range(len(k))])
    fb_mutant("first", lambda k, s: [next(((o["x"], o["y"], o["ang"]) for o in s if o["tmpl"] == e["tmpl"]), None) for e in k])
    fb_mutant("last instead of none", lambda k, s: [x if x is not None else next(((o["x"], o["y"], o["ang"]) for o in reversed(s) if o["tmpl"] == e["tmpl"]), None) for x, e in zip(fallback_spots(k, s), k)])
    rt = parse_lists(generate(lists).decode("latin-1"))["FactionX"]
    assert [(e["fb"], e["fbang"]) for e in rt] == [((500.0, 400.0), -135.0), ((600.5, 400.0), 45.0)], ("each keyed entry carries its stock spot, raw", rt)
    for k in ("doors", "none"):
        assert "GXFallback" not in generate(lists, keys=k).decode("latin-1"), ("no GXTurnLayout, no fallback", k)
    assert "GXFallbackLocation = X:600.50 Y:400.00" in generate(lists, keys="layout").decode("latin-1")
    del LAYOUTS["FactionX"]
    print("selftest: mutants and round trip OK")
    if search:
        lists = parse_lists(stock_text(search))
        assert set(lists) == set(LAYOUTS), set(lists) ^ set(LAYOUTS)
        extra = [ln for ln in stock_text(search).splitlines() if ln.strip().startswith("SkirmishBaseDefenseExtraDistance")]
        assert len(extra) == 1 and float(extra[0].split("=")[1].split(";")[0]) == RING_EXTRA, ("RING_EXTRA is not the stock SkirmishBaseDefenseExtraDistance", extra)
        rotate = [ln for ln in stock_text(search).splitlines() if ln.strip().lower().startswith("rotateskirmishbases")]
        assert len(rotate) == 1 and rotate[0].split("=")[1].split(";")[0].strip().upper() == "NO", ("stock is no longer one frame: ring_radius judges it in retail_frame alone", rotate)
        rings = dict(America=405, China=408, GLA=388, AmericaAirForceGeneral=475, AmericaSuperWeaponGeneral=455, AmericaLaserGeneral=417, ChinaInfantryGeneral=445, ChinaTankGeneral=473, ChinaNukeGeneral=425, GLADemolitionGeneral=388, GLAStealthGeneral=388, GLAToxinGeneral=388)
        got = {side: math.ceil(ring_radius(new_entries(side, lists[side]), retail_frame(lists[side]))) for side in lists}
        assert got == rings, ("the rings ADDENDUM_3 registers", {k: (got[k], rings[k]) for k in got if got[k] != rings.get(k)})
        fails = lint_all(lists)
        assert fails == 0, "%d lint failures on the shipped layouts" % fails
        first = generate(lists)
        shipped = parse_lists(first.decode("latin-1"))
        assert set(shipped) == set(lists) - set(SHIP_OFF) and set(SHIP_OFF) <= set(lists), ("the shipped file holds every list but SHIP_OFF", sorted(shipped))
        assert sorted(set(lists) - set(shipped)) == ["AmericaAirForceGeneral", "GLADemolitionGeneral"], ("ADDENDUM_10 (b): exactly these two lists are left out", sorted(set(lists) - set(shipped)))
        for side, stock in lists.items():
            if side in SHIP_OFF:
                continue
            assert [(e.get("fb"), e.get("fbang")) for e in shipped[side]] == [((o["x"], o["y"]), o["ang"]) for o in stock], ("every shipped entry falls back to its own stock spot", side)
            # each shipped list is byte for byte the list its own --only file holds (the screen's B8<g> arms)
            one = generate(lists, only=[side]).decode("latin-1")
            block = one[one.index("  SkirmishBuildList %s\r\n" % side):one.rindex("  END\r\n") + len("  END\r\n")]
            assert block in first.decode("latin-1"), ("the shipped list equals its --only list", side)
        assert generate(lists) == first, "not deterministic"
        print("selftest: the shipped layouts lint clean, output sha256 %s" % hashlib.sha256(first).hexdigest())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out")
    ap.add_argument("--search", action="append", default=[])
    ap.add_argument("--only")
    ap.add_argument("--keys", choices=["both", "doors", "layout", "none"], default="both")
    ap.add_argument("--coords", choices=["new", "stock"], default="new")
    ap.add_argument("--lint", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    if a.list:
        print("\n".join(OUTPUTS))
        return 0
    if a.selftest:
        selftest(a.search)
        return 0
    lists = parse_lists(stock_text(a.search))
    if a.lint:
        fails = lint_all(lists)
        print("lint: %d failures" % fails)
        return 1 if fails else 0
    if not a.out:
        ap.error("--out, --lint, --selftest or --list")
    only = a.only.split(",") if a.only else None
    if only:
        unknown = [s for s in only if s not in lists]
        if unknown:
            raise SystemExit("ERROR: no stock list named %s" % ", ".join(unknown))
    write(a.out, generate(lists, only=only, keys=a.keys, coords=a.coords))
    return 0


if __name__ == "__main__":
    sys.exit(main())
