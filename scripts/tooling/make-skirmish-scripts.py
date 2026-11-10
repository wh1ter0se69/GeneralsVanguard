#!/usr/bin/env python3
# GeneralsX: Data/Scripts/SkirmishScripts.scb: the retail skirmish-AI script file plus
# GeneralsX's AI edits. A RECIPE, NOT A COPY (same rule as make-weapon-ini.py and build-overlay-generated.py: the
# retail bytes are EA's and this repository is public, so they are read from the player's/packager's own copy and
# the edit ships as code).
#
#   make-skirmish-scripts.py --out <Data/Scripts/SkirmishScripts.scb> [--search <retail game dir>]...
#   make-skirmish-scripts.py --root <game folder> [--root <game folder>]... [--search <dir>]... [--if-source]
#   make-skirmish-scripts.py --list | --selftest
#   make-skirmish-scripts.py --ruleset <rules.json> --out <scb> [--tier-gate on|off] [--rule-ids <rule_ids.json>] [--manifest <file>]
#   make-skirmish-scripts.py --promote-default <manifest> --ruleset <rules.json> --scb <scb>
#       the rule bank's DATA half (scripts/tooling/rulebank/compiler.py): a ruleset compiled on top of this recipe's output, plus a
#       manifest (default <scb>.manifest.json); an empty ruleset gives exactly BASE_SHA256 (the recipe's own output)
#
# The source is <dir>/Data/Scripts/SkirmishScripts.scb of the first --search dir (then $CNC_GENERALS_ZH_PATH,
# $GX_GAME_DATA, $GX_GAME_SRC, ~/GeneralsX/GeneralsZH) that holds one; it must hash to SOURCE_SHA256 or the recipe
# REFUSES (never edits a different release silently). The output is deterministic (OUTPUT_SHA256 pins it).
# IT MOVES dataID: SimulationId.cpp computeLooseDataID hashes the .scb bundles, so every peer must carry the
# same bytes or the join is refused with DATA_DIFFERS.
#
# THE EDITS (wh1ter0se 2026-09-29):
#  1. Easy-plus (B-ai-overhaul, "Easy plays a real game"): sets the Easy flag on the Hard/Normal alert, upgrade,
#     generals-power and invasion-alert scripts. Only flag bytes change.
#  2. Spend patch (B-ai-economy): Hard AI keeps its scripted cash gifts (wh1ter0se: KEEP) but turns money into army:
#       a. for each production type a Hard script already builds, four money-gated clones (credits AND escalation),
#          placed beside the supply warehouse (AI_PLAYER_BUILD_SUPPLY_CENTER: the flank/front arcs of
#          SKIRMISH_BUILD_STRUCTURE_* are shared with base defences and run out, measured 30/09), plus two extra dozer clones;
#       b. "Tech - Stop Trying" escalation threshold 100 -> 100000 (capture never stops);
#       c. teamMaxInstances of the Hard 4TH/5TH wave teams raised to WAVES (5TH: WAVES5).
#  3. Launch fixes (C-ai-pressure-3, LAUNCH_FIXES, a FIXUP_STEPS entry): four retail copy-paste bugs left 19 Hard wave teams
#     unable to leave base; one condition changed and 15 missing scripts copied in (launch_fixes below). Only Hard teams
#     reach them (ai/press/hard-only.py); the step touches no other script and no team template (--selftest checks it).
#  5. Capture from home (D-ai-map-economy, CAPTURE_HOME, a FIXUP_STEPS entry): the 13 tech-capture order scripts lose their
#     TEAM_MOVE_TOWARDS_NEAREST_OBJECT_TYPE(Tech Buildings, CombatZone) walk, so each AI captures the oil nearest its own base
#     first instead of every AI walking to the map's CombatZone (capture_home below). Normal + Hard (the scripts are diff 011).
#  6. No defence at the dock (C-ai-supply-fix, EXPAND_NO_DEFENCE, a FIXUP_STEPS entry): the 12 Hard expansion scripts stop calling
#     AI_PLAYER_BUILD_SUPPLY_CENTER with a Patriot/Gattling/Stinger template (the repeated cluster beside one dock).
#  7. Superweapons fire when ready (B-scud-fire, SW_FIRE_READY, a FIXUP_STEPS entry): the 22 Hard SCUD Storm / Particle Cannon /
#     Neutron Missile fire scripts stop waiting for the AI's own attack wave (FLAG _LAUNCH_ATTACK), as Easy/Normal already do.
#  8. Laser General plants beside the warehouse (C-laser-power, LASER_POWER, after every step but USA_SD): the Laser General's two Hard "7+ Power
#     Plants" scripts also call AI_PLAYER_BUILD_SUPPLY_CENTER(Lazr_AmericaPowerPlant), because the arc they call runs out and only
#     Laser's tanks draw power, so a capped supply disables its whole tank army (laser_power_step below).
# 10. USA Search and Destroy (D-usa-search-destroy, USA_SD, the last step): the four USA generals' "USA Use Battle Plans" is Easy + Normal now, and a Hard
#     copy asks for Search and Destroy instead of Hold the Line, checked every frame (usa_sd_step below). wh1ter0se 03/11 + 06/11, pro rows 35, 907, 1342.
#  4. Nearby expansion (C-ai-economy-4, EXPAND_NEARBY, a FIXUP_STEPS entry): the 12 Hard "Base Expand Start - H" scripts
#     ask for a source holding >= EXPAND_MIN instead of 25000, so a Hard AI expands to the piles near its base instead of
#     only ever picking a neighbour's or the centre's dock (unsafe). Normal/Easy byte-identical ([CRC] 224/224, both maps).
#  8. China Tank + China Nuke without the spend patch (box week J2, NO_SPEND): spend_patch leaves those two generals' lists and
#     owned teams as the steps before it left them (1v1 vs stock without the production ladder: Tank +3.7, Nuke +8.4 points).
#  9. China Nuke's W-NUK-12 genome (box week J4, NUKE_GENOME, after spend_patch): its Hard base-building credit gates x1.435,
#     defence-building escalation x0.898, 4TH/5TH wave sizes x1.161 (+9.6 points 1v1 on 12 held-out maps). 8 + 9 = the package
#     the box certified (J3 mirror + 6v6 safe, J1c ~1.4% less CPU per unit); each tested arm is a switch setting (PACKAGE_ARMS).
import argparse, hashlib, os, re, struct, sys, tempfile, types

HERE = os.path.dirname(os.path.abspath(__file__))
RETAIL_MEMBER = "Data/Scripts/SkirmishScripts.scb"
SOURCE_SHA256 = "8f93862b751f289b052206b87170cc840044cb66660fbf6ae30d5782c1d73776"
BASE_SHA256 = "cc302b0941cc009d950d82e60e863c2833d50ca50feede99fd0e4f6f062b7be4"     # the recipe's own output - the rule bank compiles over it; USA_SD (the last step; off = USA_SD_OFF_SHA256); before it: T2C on integration 6afae411ea (06/11/2026): TUNNEL_AA (T2, on R) then LASER_POWER (fix S, the last step); off = LASER_POWER_OFF_SHA256 (db81fa2f, T2C's package: TUNNEL_AA on all four GLA generals, Laser off); both off = TUNNEL_AA_OFF_SHA256 (14d03e02, the 0.9.68-0.9.69 package = R: FACE_FRONT on; TUNNEL_AA off with Laser on = 990834a2, integration 6afae411ea's package); FACE_FRONT off too = FACE_FRONT_OFF_SHA256 (14414fcd, the 0.9.67 package); with the China package NO_SPEND + NUKE_GENOME (without: 752d52a6, 0.9.64-0.9.66; without SW_FIRE_READY too: cf4a7600, 0.9.60-0.9.63; without EXPAND_NO_DEFENCE too: 67eb7669, 0.9.54-0.9.59)
OUTPUT_SHA256 = "cc302b0941cc009d950d82e60e863c2833d50ca50feede99fd0e4f6f062b7be4"   # = BASE_SHA256 (no promoted ruleset); T2: merge of T2 with the rule bank, t2-fix 01/11/2026; db81fa2f after the bank ENGINE (t2-fix-2: AI_PLAYER_TUNNEL_AIR_WATCH moved 345 -> 348 behind the three GX_RULE actions; was d0e6d8ce, the registered T2-ALL); C-laser-power on top of it, 06/11/2026
# OUTPUT_SHA256 pins what --out / --root / every packager writes: BASE + rulebank/shipped_ruleset.json once a tuned ruleset is promoted
# (plan 4.4 step 2: then OUTPUT_SHA256 and dataID move, BASE_SHA256 does not). Without that file the two are equal.
SHIPPED_RULESET = os.path.join(HERE, "rulebank", "shipped_ruleset.json")
OUTPUTS = [RETAIL_MEMBER]
RETAIL_BACKUP = ".retail"   # <target>.retail: the pristine source, kept when a deploy overwrites the retail file in place
LADDER = [15000, 30000, 50000, 80000]     # PLAYER_HAS_CREDITS rungs
ESC = [10, 20, 30, 45]                    # _ESCALATION rungs (Hard: +1 per 10 s): ~1.7/3.3/5/7.5 min
SUPPLY_MIN = 1000                         # AI_PLAYER_BUILD_SUPPLY_CENTER's cash argument (only read for cash generators)
# KNOWN LIMIT (accepted): the action builds beside the AI's current warehouse (AIPlayer::buildBySupplies,
# m_curWarehouseID) or, with none yet, beside an UNSERVED dock with >= SUPPLY_MIN boxes (findSupplyCenter). On a map where
# the AI has no such dock a rung does nothing (inert, never harmful). Measured working on CraterRing and Cauldron (production x2).
DOZER_CREDITS = [50000, 80000]            # extra 'Replace Dozer' clones of the retail '... High Money Build Condition 2'
WAVES = 3                                 # teamMaxInstances for the Hard 4TH/5TH wave teams
WAVES5 = 6                                # ... and for the 5TH wave
WAVE_RX = r'^(USA|China|GLA) H (4TH|5TH) Wave'
WAVE5_RX = r'^(USA|China|GLA) H 5TH Wave'
LAUNCH_FIXES = True                       # the four retail launch fixes (launch_fixes below); False = the recipe without them
PTYPES = ['INT', 'REAL', 'SCRIPT', 'TEAM', 'COUNTER', 'FLAG', 'COMPARISON', 'WAYPOINT', 'BOOLEAN', 'TRIGGER_AREA', 'TEXT_STRING', 'SIDE', 'SOUND', 'SCRIPT_SUBROUTINE', 'UNIT', 'OBJECT_TYPE', 'COORD3D', 'ANGLE', 'TEAM_STATE', 'RELATION', 'AI_MOOD', 'DIALOG', 'MUSIC', 'MOVIE', 'WAYPOINT_PATH', 'LOCALIZED_TEXT', 'BRIDGE', 'KIND_OF_PARAM', 'ATTACK_PRIORITY_SET', 'RADAR_EVENT_TYPE', 'SPECIAL_POWER', 'SCIENCE', 'UPGRADE', 'COMMANDBUTTON_ABILITY', 'BOUNDARY', 'BUILDABLE', 'SURFACES_ALLOWED', 'SHAKE_INTENSITY', 'COMMAND_BUTTON', 'FONT_NAME', 'OBJECT_STATUS', 'COMMANDBUTTON_ALL_ABILITIES', 'SKIRMISH_WAYPOINT_PATH', 'COLOR', 'EMOTICON', 'OBJECT_PANEL_FLAG', 'FACTION_NAME', 'OBJECT_TYPE_LIST', 'REVEALNAME', 'SCIENCE_AVAILABILITY', 'LEFT_OR_RIGHT', 'PERCENT', 'NUM_ITEMS']

import struct, re
CONTAINERS = {'PlayerScriptsList', 'ScriptList', 'ScriptGroup', 'Script', 'OrCondition'}

class Node:
    def __init__(s, name, ver, prefix=b'', kids=None, raw=None):
        s.name, s.ver, s.prefix, s.kids, s.raw = name, ver, prefix, kids, raw

class SCB:
    def __init__(s, data):
        assert data[:4] == b'CkMp'
        n = struct.unpack_from('<i', data, 4)[0]; p = 8
        s.toc = []  # ordered (name, id)
        for _ in range(n):
            l = data[p]; p += 1
            nm = data[p:p+l].decode('latin1'); p += l
            s.toc.append((nm, struct.unpack_from('<I', data, p)[0])); p += 4
        s.byid = {i: nm for nm, i in s.toc}; s.byname = {nm: i for nm, i in s.toc}
        s.root = s._chunks(data[p:], top=True)
    def _chunks(s, b, top=False):
        out = []; p = 0
        while p < len(b):
            cid, ver, size = struct.unpack_from('<IHi', b, p); p += 10
            body = b[p:p+size]; p += size
            nm = s.byid.get(cid, cid)
            out.append(s._node(nm, ver, body))
        return out
    def _node(s, nm, ver, body):
        if nm in CONTAINERS:
            pre = 0
            if nm == 'ScriptGroup':
                pre = 2 + struct.unpack_from('<H', body, 0)[0] + 1 + (1 if ver == 2 else 0)
            elif nm == 'Script':
                q = 0
                for _ in range(4): q += 2 + struct.unpack_from('<H', body, q)[0]
                pre = q + 6 + (4 if ver >= 2 else 0)
            return Node(nm, ver, body[:pre], s._chunks(body[pre:]))
        return Node(nm, ver, raw=body)
    def _w(s, kids):
        out = b''
        for k in kids:
            body = k.prefix + s._w(k.kids) if k.kids is not None else k.raw
            out += struct.pack('<IHi', s.byname[k.name], k.ver, len(body)) + body
        return out
    def write(s):
        t = b'CkMp' + struct.pack('<i', len(s.toc))
        for nm, i in s.toc: t += bytes([len(nm)]) + nm.encode('latin1') + struct.pack('<I', i)
        return t + s._w(s.root)

def astr(b, p):
    n = struct.unpack_from('<H', b, p)[0]; return b[p+2:p+2+n].decode('latin1'), p + 2 + n
def enc_astr(x): x = x.encode('latin1'); return struct.pack('<H', len(x)) + x

def script_name(sn): return astr(sn.prefix, 0)[0]
def script_flags(sn):
    q = 0
    for _ in range(4): q += 2 + struct.unpack_from('<H', sn.prefix, q)[0]
    return list(sn.prefix[q:q+6]), (struct.unpack_from('<i', sn.prefix, q+6)[0] if sn.ver >= 2 else 0), q
def set_script(sn, name=None, delay=None, oneshot=None, diff=None, active=None):
    fl, delay0, q = script_flags(sn); pre = sn.prefix
    if name is not None:
        p = astr(pre, 0)[1]; pre = enc_astr(name) + pre[p:]
    fl, d0, q = script_flags(Node(sn.name, sn.ver, pre))
    pre = bytearray(pre)
    if active is not None: pre[q] = active
    if oneshot is not None: pre[q+1] = oneshot
    if diff is not None: pre[q+2:q+5] = bytes(diff)   # (easy, normal, hard)
    if delay is not None: struct.pack_into('<i', pre, q+6, delay)
    sn.prefix = bytes(pre)

def parse_call(scb, raw):
    """condition/action leaf -> (type int, nameKey int, [(ptype,int,real,str,coord)]) """
    p = 0; typ = struct.unpack_from('<i', raw, p)[0]; p += 4
    key = struct.unpack_from('<i', raw, p)[0]; p += 4
    n = struct.unpack_from('<i', raw, p)[0]; p += 4
    ps = []
    for _ in range(n):
        t = struct.unpack_from('<i', raw, p)[0]; p += 4
        if PTYPES[t] == 'COORD3D':
            ps.append((t, struct.unpack_from('<3f', raw, p))); p += 12
        else:
            i, f = struct.unpack_from('<if', raw, p); p += 8; sv, p = astr(raw, p); ps.append((t, i, f, sv))
    assert p == len(raw)
    return typ, key, ps
def enc_call(typ, key, ps):
    out = struct.pack('<iii', typ, key, len(ps))
    for x in ps:
        out += struct.pack('<i', x[0])
        out += struct.pack('<3f', *x[1]) if PTYPES[x[0]] == 'COORD3D' else struct.pack('<if', x[1], x[2]) + enc_astr(x[3])
    return out
def call_name(scb, raw):
    typ, key, ps = parse_call(scb, raw); return scb.byid.get(key >> 8, '?'), ps
def deep_copy(n):
    return Node(n.name, n.ver, n.prefix, [deep_copy(k) for k in n.kids] if n.kids is not None else None, n.raw)
def walk_scripts(scb):
    for li, lst in enumerate([k for k in scb.root if k.name == 'PlayerScriptsList'][0].kids):
        def rec(nodes, grp):
            for i, k in enumerate(nodes):
                if k.name == 'ScriptGroup': yield from rec(k.kids, k)
                elif k.name == 'Script': yield li, grp, nodes, i, k
        yield from rec(lst.kids, None)
def round_trip(path):
    d = open(path, 'rb').read(); return SCB(d).write() == d


# ------------------------------------------------------------------------------------------------- the edits
def easy_plus(scb):
    """B-ai-overhaul's Easy-plus: only flag bytes change. Returns the number of scripts given the Easy flag."""
    rules = [('Build Conditions - Alerts', (0, 1, 1), ''), ('Upgrades', (0, 1, 1), ''), ('Generals Powers', (0, 1, 0), ''),
             ('Escalation', (0, 1, 1), 'Invasion Alert')]
    changed = 0
    for li, grp, nodes, i, sn in walk_scripts(scb):
        if grp is None:
            continue
        gname = grp.prefix[2:2 + int.from_bytes(grp.prefix[:2], 'little')].decode('latin1')
        fl, _, q = script_flags(sn)
        for fam, want, must in rules:
            if gname.endswith(' ' + fam) and tuple(fl[2:5]) == want and must in script_name(sn):
                pre = bytearray(sn.prefix)
                assert pre[q + 2] == 0
                pre[q + 2] = 1
                sn.prefix = bytes(pre)
                changed += 1
                break
    return changed


PROD = re.compile(r'(WarFactory|Barracks|Airfield|ArmsDealer)$')
# GeneralsX: China Tank + China Nuke without the spend patch (box week J2, 19/10: the recipe-step
# ablation, ai/bw/j2). Each step was taken out for one general at a time (that general's list + owned teams from
# the recipe without the step, everything else stock) and played 1v1 against stock: only two removals helped, both of the
# spend patch, both China (Tank +3.7 points, Holm 0.016; Nuke +8.4, Holm 1e-9). NO_SPEND names those ScriptList owners: spend_patch
# leaves their scripts and the teams they own as the earlier steps left them. Its donors still come from the whole file, so
# every other general's bytes are unchanged (PACKAGE_ARMS pins both).
NO_SPEND = ('SkirmishChinaTankGeneral', 'SkirmishChinaNukeGeneral')   # () + NUKE_GENOME False = the 0.9.64-0.9.66 recipe (pin 752d52a6)


def spend_patch(scb, no_spend=None):
    """Money-gated production ladder + capture never gives up + bigger Hard 4TH/5TH waves, for every general but the
    no_spend owners (default NO_SPEND). Returns (added, tech, waves)."""
    skip = NO_SPEND if no_spend is None else no_spend
    assert all(o in LAUNCH_FIX_OWNERS[1:13] for o in skip), 'spend_patch: not a general\'s list: %r' % (skip,)
    skip_li = {LAUNCH_FIX_OWNERS.index(o) for o in skip}
    don = {}
    for li, grp, nodes, i, sn in walk_scripts(scb):
        for o in sn.kids:
            if o.name == 'OrCondition':
                for c in o.kids:
                    nm, ps = call_name(scb, c.raw)
                    if nm == 'PLAYER_HAS_CREDITS' and 'cond' not in don:
                        don['cond'] = c
                    if nm == 'COUNTER' and 'esc' not in don and any(len(p) > 3 and p[3] == '_ESCALATION' for p in ps):
                        don['esc'] = c
            elif o.name == 'ScriptAction':
                nm, ps = call_name(scb, o.raw)
                if nm in ('SKIRMISH_BUILD_STRUCTURE_FLANK', 'SKIRMISH_BUILD_STRUCTURE_FRONT', 'AI_PLAYER_BUILD_SUPPLY_CENTER') and nm not in don:
                    don[nm] = o
    for k in ('cond', 'esc', 'AI_PLAYER_BUILD_SUPPLY_CENTER'):
        assert k in don, 'no donor for ' + k

    def mk_cond(thr, esc):
        # PLAYER_HAS_CREDITS(v, COMPARISON:1) is 'v <= money' (ScriptConditions.cpp evaluatePlayerHasCredits): money >= v
        c0 = don['cond']; typ, key, ps0 = parse_call(scb, c0.raw)
        ps1 = [(p[0], thr, 0.0, '') if PTYPES[p[0]] == 'INT' else ((p[0], 1, 0.0, '') if PTYPES[p[0]] == 'COMPARISON' else p) for p in ps0]
        if esc is None:
            return [Node('Condition', c0.ver, raw=enc_call(typ, key, ps1))]
        e0 = don['esc']; et, ek, ep0 = parse_call(scb, e0.raw)
        # COUNTER(_ESCALATION, COMPARISON:3, v) is 'counter >= v' as the retail scripts use it
        assert [p[1] for p in ep0 if PTYPES[p[0]] == 'COMPARISON'] == [3], 'escalation donor is not COUNTER >= v (GREATER_EQUAL=3)'
        ep1 = [(p[0], esc, 0.0, '') if PTYPES[p[0]] == 'INT' else p for p in ep0]
        return [Node('Condition', c0.ver, raw=enc_call(typ, key, ps1)), Node('Condition', e0.ver, raw=enc_call(et, ek, ep1))]

    def mk_act(kind, obj):
        a0 = don[kind]; typ, key, ps0 = parse_call(scb, a0.raw)
        assert any(PTYPES[p[0]] == 'OBJECT_TYPE' for p in ps0)
        ps1 = [(p[0], p[1], p[2], obj) if PTYPES[p[0]] == 'OBJECT_TYPE' else ((p[0], SUPPLY_MIN, 0.0, '') if PTYPES[p[0]] == 'INT' else p) for p in ps0]
        return Node('ScriptAction', a0.ver, raw=enc_call(typ, key, ps1))

    added = tech = 0
    have = {script_name(x) for _l, _g, _n, _i, x in walk_scripts(scb)}
    work, seen = [], set()
    for li, grp, nodes, i, sn in list(walk_scripts(scb)):
        fl, _, _ = script_flags(sn)
        if fl[4] != 1 or fl[2] or fl[3] or li in skip_li:
            continue  # Hard-only scripts, not a NO_SPEND general's
        types = set()
        for o in sn.kids:
            if o.name == 'ScriptAction':
                nm, ps = call_name(scb, o.raw)
                if nm in ('SKIRMISH_BUILD_BUILDING', 'SKIRMISH_BUILD_STRUCTURE_FLANK', 'SKIRMISH_BUILD_STRUCTURE_FRONT'):
                    types |= {p[3] for p in ps if len(p) > 3 and PROD.search(p[3] or '')}
        for t in sorted(types):
            work.append((li, nodes, sn, t))
    for li, nodes, sn, t in work:
        if (li, t) in seen:
            continue
        seen.add((li, t)); at = nodes.index(sn)
        for k, thr in enumerate(LADDER):
            nm_new = '%s RICH%d %s' % (script_name(sn).split(' - ')[0], k + 1, t)
            if nm_new in have:
                continue
            n = deep_copy(sn)
            set_script(n, name=nm_new, delay=0, oneshot=1)
            n.kids = [Node('OrCondition', 1, kids=mk_cond(thr, ESC[k])),
                      mk_act('AI_PLAYER_BUILD_SUPPLY_CENTER', t)]
            nodes.insert(at + 1 + k, n); added += 1
    for li, grp, nodes, i, sn in list(walk_scripts(scb)):
        if not script_name(sn).endswith('Dozer - High Money Build Condition 2') or li in skip_li:
            continue
        if script_flags(sn)[0][4] != 1:
            continue  # the clone is Hard-only, so the donor must be a Hard script
        at = nodes.index(sn)
        for k, thr in enumerate(DOZER_CREDITS):
            nm_new = '%s DX%d' % (script_name(sn), k + 1)
            if nm_new in have:
                continue
            n = deep_copy(sn)
            set_script(n, name=nm_new, delay=0, oneshot=1, diff=(0, 0, 1))
            n.kids = [Node('OrCondition', 1, kids=mk_cond(thr, None)), deep_copy([x for x in sn.kids if x.name == 'ScriptAction'][0])]
            nodes.insert(at + 1 + k, n); added += 1
    for li, grp, nodes, i, sn in walk_scripts(scb):
        if not script_name(sn).endswith('Tech - Stop Trying') or li in skip_li:
            continue
        for o in sn.kids:
            if o.name != 'OrCondition':
                continue
            for c in o.kids:
                nm, ps = call_name(scb, c.raw)
                if nm == 'COUNTER':
                    typ, key, ps0 = parse_call(scb, c.raw)
                    ps1 = [(p[0], 100000, 0.0, '') if PTYPES[p[0]] == 'INT' and p[1] == 100 else p for p in ps0]
                    if ps1 != ps0:
                        c.raw = enc_call(typ, key, ps1); tech += 1
    tn = next(k for k in scb.root if k.name == 'ScriptTeams'); b = bytearray(tn.raw); p = 0; waves = 0
    while p < len(b):
        cnt = struct.unpack_from('<H', b, p)[0]; p += 2; name = None; off = None; owner = None
        for _ in range(cnt):
            kt = struct.unpack_from('<i', b, p)[0]; p += 4; t = kt & 0xFF; key = scb.byid.get(kt >> 8, '?')
            if t == 0:                      # DICT_BOOL
                p += 1
            elif t == 1:                    # DICT_INT
                if key == 'teamMaxInstances':
                    off = p
                p += 4
            elif t == 2:                    # DICT_REAL
                p += 4
            elif t == 3:                    # DICT_ASCII
                n = struct.unpack_from('<H', b, p)[0]; v = b[p + 2:p + 2 + n].decode('latin1'); p += 2 + n
                if key == 'teamName':
                    name = v
                elif key == 'teamOwner':
                    owner = v
            elif t == 4:                    # DICT_UNICODE
                n = struct.unpack_from('<H', b, p)[0]; p += 2 + 2 * n
            else:
                raise ValueError('bad dict type %d' % t)
        if owner in skip:
            continue                        # a NO_SPEND general's team
        if name and re.search(WAVE_RX, name) and off is not None and struct.unpack_from('<i', b, off)[0] < WAVES:
            struct.pack_into('<i', b, off, WAVES); waves += 1
        if name and re.search(WAVE5_RX, name) and off is not None and struct.unpack_from('<i', b, off)[0] < WAVES5:
            struct.pack_into('<i', b, off, WAVES5); waves += 1
    tn.raw = bytes(b)
    return added, tech, waves


# GeneralsX: Four retail launch bugs park Hard attack teams at home for the whole match (
# wh1ter0se: "attacks while preparing attacks"). Every Hard wave team leaves base through a team script that fires on the attack
# pulse while the team is inside its own base; these teams could not:
#  1. plain China "China Attack Sequence Back" tests TEAM_OUTSIDE_AREA_ENTIRELY(MyOuterPerimeter); every other copy (the three
#     China generals') tests TEAM_INSIDE_AREA_PARTIALLY -> its wave-4 Tanks/Artillery/Air B and wave-5 Artillery B never left.
#  2. "China Behavior: Dragon Tank Attack" exists only in the Infantry General's list; plain/Tank/Nuke China's "5TH Wave Tanks B"
#     name it as their attack hook (a missing hook never runs).
#  3. "GLA Behavior: Bomb Truck Center/Flank/Back" exist only in plain GLA's list; the Demolition/Toxin/Stealth "5TH Wave Bomb
#     Truck C/F/B" teams name them (the "GLA Bomb Truck Attack *" sequences they call are in every GLA list).
#  4. "GLA Behavior: Immediate Attack Center " (trailing space) exists only in plain GLA's list; the three GLA generals'
#     "5W Rebel for Demo C" OnCreate names it, and their own copy has no trailing space.
# Only Hard teams reach the touched scripts (proof: hard-only.py); Normal plays byte-identical ([CRC] 359/359).
LAUNCH_FIX_OWNERS = ['', 'SkirmishAmerica', 'SkirmishChina', 'SkirmishGLA', 'SkirmishAmericaAirForceGeneral',
                     'SkirmishAmericaLaserGeneral', 'SkirmishAmericaSuperWeaponGeneral', 'SkirmishChinaTankGeneral',
                     'SkirmishChinaNukeGeneral', 'SkirmishChinaInfantryGeneral', 'SkirmishGLADemolitionGeneral',
                     'SkirmishGLAToxinGeneral', 'SkirmishGLAStealthGeneral', 'PlyrCivilian']    # ScriptList order in the file
LAUNCH_FIX_COPIES = [  # (source list, script names, target lists, sibling whose group receives the copy)
    ('SkirmishChinaInfantryGeneral', ['China Behavior: Dragon Tank Attack'],
     ['SkirmishChina', 'SkirmishChinaTankGeneral', 'SkirmishChinaNukeGeneral'], 'China Attack Sequence Back'),
    ('SkirmishGLA', ['GLA Behavior: Bomb Truck Center', 'GLA Behavior: Bomb Truck Flank', 'GLA Behavior: Bomb Truck Back'],
     ['SkirmishGLADemolitionGeneral', 'SkirmishGLAToxinGeneral', 'SkirmishGLAStealthGeneral'], 'GLA Bomb Truck Attack Center'),
    ('SkirmishGLA', ['GLA Behavior: Immediate Attack Center '],
     ['SkirmishGLADemolitionGeneral', 'SkirmishGLAToxinGeneral', 'SkirmishGLAStealthGeneral'], 'GLA Behavior: Immediate Attack Center'),
]


def launch_fixes(scb):
    """The four launch fixes; asserts each edit applies exactly once. Returns the number of edits (16)."""
    li = {o: i for i, o in enumerate(LAUNCH_FIX_OWNERS)}

    def scripts(i):
        return {script_name(k): (grp, k) for l, grp, nodes, j, k in walk_scripts(scb) if l == i}

    def conditions(node):
        for k in node.kids or []:
            if k.kids is not None:
                yield from conditions(k)
            elif k.name == 'Condition':
                yield k
    # 1: the type and name key come from an existing TEAM_INSIDE_AREA_PARTIALLY condition in the same file
    inside = next((parse_call(scb, c.raw)[:2] for l, g, n, j, k in walk_scripts(scb) for c in conditions(k)
                   if call_name(scb, c.raw)[0] == 'TEAM_INSIDE_AREA_PARTIALLY'), None)
    assert inside, 'launch_fixes: no TEAM_INSIDE_AREA_PARTIALLY condition to copy'
    back = scripts(li['SkirmishChina'])['China Attack Sequence Back'][1]
    hits = [c for c in conditions(back) if call_name(scb, c.raw)[0] == 'TEAM_OUTSIDE_AREA_ENTIRELY']
    assert len(hits) == 1, 'launch_fixes 1: expected one OUTSIDE condition, found %d' % len(hits)
    typ, key, ps = parse_call(scb, hits[0].raw)
    assert [PTYPES[p[0]] for p in ps] == ['TEAM', 'TRIGGER_AREA', 'SURFACES_ALLOWED'] and ps[1][3] == '[Skirmish]MyOuterPerimeter', ps
    hits[0].raw = enc_call(inside[0], inside[1], ps)
    edits = 1
    # 2-4: copy the missing scripts next to a named sibling in each target list
    for src, names, targets, sibling in LAUNCH_FIX_COPIES:
        have = scripts(li[src])
        for t in targets:
            tgt = scripts(li[t])
            grp = tgt[sibling][0]
            assert grp is not None, 'launch_fixes: sibling %r in %s is not in a group' % (sibling, t)
            for nm in names:
                assert nm in have and nm not in tgt, 'launch_fixes: %r in %s -> %s' % (nm, src, t)
                grp.kids.append(deep_copy(have[nm][1]))
                edits += 1
    return edits


# GeneralsX: A Hard AI never expands to the supplies near its base on 12-seat maps (
# wh1ter0se: "expand to their nearby supplies"). "<Side> Base Expand Start - H" asks SUPPLY_SOURCE_SAFE(25000) and builds with
# AI_PLAYER_BUILD_SUPPLY_CENTER(.., 25000). AIPlayer::findSupplyCenter takes the CLOSEST unserved source holding >= that cash
# and halves the amount only when NO source qualifies; the caller then tests just that one source (isLocationSafe). Crater
# Ring and Cauldron put a $30,000 dock in every base plus unmined centre docks, so the pick is a neighbour's base dock or the
# contested centre - unsafe - while the $3,750 piles 690-830 from the AI's own dock never qualify. EXPAND_MIN lets the closest
# unserved pile be the pick. Hard-only scripts (diff 001); Normal/Easy keep 25000.
EXPAND_NEARBY = True                      # expand_nearby below; False = the recipe without it (pin 1e46dc84)
EXPAND_MIN = 3000                         # < a full SupplyPileSmall ($3,750), so a nearly-drained pile is not worth a supply centre
EXPAND_RX = r'^(USA|China|GLA) Base Expand Start - H$'

def expand_nearby(scb):
    """SUPPLY_SOURCE_SAFE / AI_PLAYER_BUILD_SUPPLY_CENTER 25000 -> EXPAND_MIN in the Hard expansion scripts. Returns the edits (36)."""
    edits, hit = 0, 0
    for li, grp, nodes, i, sn in walk_scripts(scb):
        if not re.match(EXPAND_RX, script_name(sn)):
            continue
        fl, _, _ = script_flags(sn)
        assert tuple(fl[2:5]) == (0, 0, 1), 'expand_nearby: %r is not Hard-only: %r' % (script_name(sn), fl)
        hit += 1
        def leaves(node):
            for k in node.kids or []:
                if k.kids is not None:
                    yield from leaves(k)
                elif k.name in ('Condition', 'ScriptAction'):
                    yield k
        for c in leaves(sn):
            nm, ps = call_name(scb, c.raw)
            if nm not in ('SUPPLY_SOURCE_SAFE', 'AI_PLAYER_BUILD_SUPPLY_CENTER'):
                continue
            typ, key, ps = parse_call(scb, c.raw)
            ints = [j for j, p in enumerate(ps) if PTYPES[p[0]] == 'INT']
            assert len(ints) == 1 and ps[ints[0]][1] == 25000, 'expand_nearby: %s in %r: %r' % (nm, script_name(sn), ps)
            p = ps[ints[0]]; ps[ints[0]] = (p[0], EXPAND_MIN, p[2], p[3])
            c.raw = enc_call(typ, key, ps)
            edits += 1
    assert hit == 12 and edits == 36, 'expand_nearby: %d scripts, %d edits' % (hit, edits)
    return edits


# Capture from home (wh1ter0se: "the oil splits bottom left weren't even taken ... top right and
# left were both taken by the AI on the front"). Every '<side> Tech Building Capture Orders' script first runs
# TEAM_MOVE_TOWARDS_NEAREST_OBJECT_TYPE(<team>, Tech Buildings, CombatZone): a walk to the tech building nearest the team
# INSIDE the map's CombatZone (any owner), and only then TEAM_HUNT_WITH_COMMAND_BUTTON(capture), which takes the nearest
# capturable building from where the team stands. On a big map every AI's team walks to the same middle first, so the
# front seats keep the middle oils and the back corners fall late to whoever survives the walk (6v6 Twin Ridges, measured
# with probe/ai-map-economy: corners at 9.7-17 min, 7 of 12 AIs holding no oil at 20 min; ai-overhaul measured the same
# collapse on Cauldron 29/09). Dropping the walk leaves the hunt, which starts at the team's base: each AI takes the oil
# nearest it first. Only those 13 actions go (12 lists + GLA's inactive '... Orders C'); every other CombatZone use stays.
CAPTURE_HOME = True                       # capture_home below; False = the recipe without it (pin e5db3eca)
CAPTURE_RX = r'^(USA|China|GLA) Tech Building Capture Orders( C)?$'

def capture_home(scb):
    """Remove the CombatZone walk from the tech-capture orders. Returns the number of actions removed (13)."""
    removed = []
    for li, grp, nodes, i, sn in walk_scripts(scb):
        if not re.match(CAPTURE_RX, script_name(sn)):
            continue
        keep = []
        for k in sn.kids:
            if k.name == 'ScriptAction':
                nm, ps = call_name(scb, k.raw)
                if nm == 'TEAM_MOVE_TOWARDS_NEAREST_OBJECT_TYPE':
                    strs = [p[3] for p in ps if PTYPES[p[0]] != 'COORD3D']
                    assert 'Tech Buildings' in strs and 'CombatZone' in strs, 'capture_home: %r: %r' % (script_name(sn), strs)
                    removed.append(script_name(sn))
                    continue
            keep.append(k)
        sn.kids = keep
    assert len(removed) == 13, 'capture_home: removed %d: %r' % (len(removed), removed)
    return len(removed)



# No defence at the dock (C-ai-supply-fix, 06/10/2026; wh1ter0se: "cluster stuff in the middle of the base"). Each Hard "Base Expand Start - H"
# script is re-armed every 150 s while SUPPLY_SOURCE_SAFE and, besides the supply centre, calls AI_PLAYER_BUILD_SUPPLY_CENTER with a
# DEFENCE template (Patriot / Gattling / Stinger). AIPlayer::buildBySupplies puts a non-cash-generator beside m_curWarehouseID (the last
# source a centre was asked for), so every re-arm drops another defence into the same ~400 wu square: measured 9-10 Patriots per AI at
# 15 min on Twin Ridges (ai/supply-ignore/FINDING). The six-script "Base Expansion Base Defense" chain keeps its capped defences.
EXPAND_NO_DEFENCE = True                  # expand_no_defence below; False = the recipe without it (pin 67eb7669)
DEFENCE_RX = r'(PatriotBattery|GattlingCannon|StingerSite)$'

def expand_no_defence(scb):
    """Remove the defence-template AI_PLAYER_BUILD_SUPPLY_CENTER action from the 12 Hard expansion scripts. Returns the number removed (12)."""
    removed = []
    for li, grp, nodes, i, sn in walk_scripts(scb):
        if not re.match(EXPAND_RX, script_name(sn)):
            continue
        keep = []
        for k in sn.kids:
            if k.name == 'ScriptAction':
                nm, ps = call_name(scb, k.raw)
                if nm == 'AI_PLAYER_BUILD_SUPPLY_CENTER' and any(PTYPES[p[0]] == 'OBJECT_TYPE' and re.search(DEFENCE_RX, p[3]) for p in ps):
                    removed.append(script_name(sn))
                    continue
            keep.append(k)
        sn.kids = keep
    assert len(removed) == 12 and set(removed) == {'%s Base Expand Start - H' % s for s in ('USA', 'China', 'GLA')}, 'expand_no_defence: %r' % removed
    return len(removed)


# Superweapons fire when ready (B-scud-fire, 13/10/2026; wh1ter0se 12/10: "scuds arent being launched instantly or at all"). Every Hard
# superweapon script pair ("... Fire ... - H" fires, then hands over to "... AI - H", which re-arms it) ANDs SKIRMISH_SPECIAL_POWER_READY
# with FLAG(_LAUNCH_ATTACK, true). That flag is the AI's attack-wave pulse: up for ~2 frames every 30-45 s, so a ready SCUD Storm,
# Particle Cannon or Neutron Missile sat at 0:00 for up to ~40 s before it fired (measured on 0.9.62 and 0.9.63 alike: ai/scud-fire
# ). Easy/Normal fire the moment the weapon is ready, and so does the Superweapon General's cannon on every difficulty.
# This drops the flag condition from those 22 scripts, so the Hard pair works as the Normal pair does; the general powers keep their
# wave sync (an air strike or a paradrop that lands with the army is the point of it).
SW_FIRE_READY = True                      # sw_fire_ready below; False = the recipe without it (pin cf4a7600, 0.9.60-0.9.63)
SW_POWERS = ('SuperweaponScudStorm', 'SuperweaponParticleUplinkCannon', 'SuperweaponNeutronMissile')
SW_SCRIPTS = {'GLA Fire SCUD Storm - H': 4, 'GLA Fire SCUD Storm AI - H': 4, 'USA Fire Particle Cannon H': 3, 'USA Particle Cannon AI H': 3,
              'China Fire Nuke Missile - H': 4, 'China Fire Nuke Missile AI - H': 4}   # script -> lists holding it (one per general)

def is_wave_flag(scb, leaf):
    nm, ps = call_name(scb, leaf.raw)
    return nm == 'FLAG' and [(PTYPES[p[0]], p[3] if PTYPES[p[0]] == 'FLAG' else p[1]) for p in ps] == [('FLAG', '_LAUNCH_ATTACK'), ('BOOLEAN', 1)]

def sw_fire_ready(scb):
    """Remove FLAG(_LAUNCH_ATTACK, true) from every OR block that also tests SKIRMISH_SPECIAL_POWER_READY for a superweapon. Returns the
    number removed (22: one per script in SW_SCRIPTS, per list)."""
    removed = []
    for li, grp, nodes, i, sn in walk_scripts(scb):
        for oc in sn.kids:
            if oc.name != 'OrCondition':
                continue
            if not any(k.name == 'Condition' and call_name(scb, k.raw)[0] == 'SKIRMISH_SPECIAL_POWER_READY' and
                       any(PTYPES[p[0]] == 'SPECIAL_POWER' and p[3] in SW_POWERS for p in call_name(scb, k.raw)[1]) for k in oc.kids):
                continue
            keep = []
            for k in oc.kids:
                if k.name == 'Condition' and is_wave_flag(scb, k):
                    removed.append(script_name(sn))
                    continue
                keep.append(k)
            assert keep, 'sw_fire_ready emptied an OR block in %r' % script_name(sn)
            oc.kids = keep
    per = {n: removed.count(n) for n in set(removed)}
    assert per == SW_SCRIPTS, 'sw_fire_ready: %r' % per
    return len(removed)


# GeneralsX: China Nuke's W-NUK-12 genome (box week J4, 20-21/10, ai/bw/j4). A registered
# grid of single-gene moves and three forge champions' genomes, built on China Nuke's NO_SPEND scripts by the forge's pack builder
# (forge_pack.py BUILDER 'forge-pack-5'), screened, then re-played on 12 held-out maps: variant 12 (the Air Force
# champion's three moves) won +9.6 points 1v1 vs stock (435 better / 182 worse of 2,640 pairs, sign p 4e-25, better on 10 of 12
# maps) and mirror games still end. Its genes: PLAYER_HAS_CREDITS gates of the Hard base-building scripts x1.435, the _ESCALATION
# threshold of the Hard defence-building ones x0.898, unit counts of the Hard 4TH/5TH wave teams x1.161 (each value
# max(0, round(v x k)), counts at least 1). On the pinned retail input the builder's rules reach exactly the 16 values below, so
# they are written out: China Nuke saves up longer before its high-money builds, air field, defences and speaker towers, starts
# its 2nd / extra gattling / extra bunker defences a little earlier, and lets four late wave teams recruit one more unit. Every
# script is Hard-only and in China Nuke's list; every team is China Nuke's (nuke_genome asserts both, each old value, and the
# exact bytes of China Nuke's list + teams it starts from and ends with: the genome was built and played on those bytes only).
NUKE_GENOME = True                        # nuke_genome below; False = China Nuke plays its NO_SPEND scripts as they are (PACKAGE_ARMS A-SP-NUK)
NUKE_OWNER = 'SkirmishChinaNukeGeneral'
NUKE_GENES = {'PLAYER_HAS_CREDITS': 1.435, 'COUNTER': 0.898, 'team': 1.161}   # the selftest checks every row below against them
NUKE_SCRIPT_EDITS = [     # (script, condition, old INT, new INT); COUNTER = the _ESCALATION counter
    ('China High Money Builds - H', 'PLAYER_HAS_CREDITS', 30000, 43050),
    ('China Air Field - H', 'PLAYER_HAS_CREDITS', 2500, 3588),
    ('China 2nd Defense - H', 'PLAYER_HAS_CREDITS', 1200, 1722),
    ('China 2nd Defense - H', 'COUNTER', 20, 18),
    ('China Build More Gat Defenses - H', 'PLAYER_HAS_CREDITS', 4000, 5740),
    ('China Build More Gat Defenses - H', 'COUNTER', 40, 36),
    ('China Build More Bunker Defenses - H', 'PLAYER_HAS_CREDITS', 6000, 8610),
    ('China Build More Bunker Defenses - H', 'COUNTER', 40, 36),
] + [('China Build Speaker Tower %d - H' % n, 'PLAYER_HAS_CREDITS', 3000, 4305) for n in (1, 2, 3, 4)]
NUKE_TEAM_EDITS = [       # (team, field, old, new)
    ('China H 4TH Wave Tanks F. 2', 'teamUnitMaxCount1', 4, 5), ('China H 4TH Wave Tanks B. 2', 'teamUnitMaxCount1', 4, 5),
    ('China H 5TH Wave Tanks F. 2', 'teamUnitMaxCount1', 4, 5), ('China H 5TH Wave Air 2', 'teamUnitMaxCount1', 4, 5)]
NUKE_PARTS_SHA256 = ('3357e85303cf8932193799d22884031f88d1bbef8e60d0fc9311a296a740d2fe',   # owner_bytes before (= A-SP-NUK's)
                     'cbfb6151ebe7d79d7a57d256239cf92a4ed801fade83b8160f3fea42696f683b')   # and after (= W-NUK-12's)
TNK_NUK = ('SkirmishChinaTankGeneral', NUKE_OWNER)
PACKAGE_ARMS = {    # box-week arm -> (no_spend, genome, sha256 of its SkirmishScripts.scb): each one played on the box (ai/bw)
    'S':        ((), False, '752d52a624467ea57efae4c7ddde1d7d55e10b8c1de942f02df2b5abbc832f7c'),           # stock, 0.9.64-0.9.66
    'A-SP-TNK': (TNK_NUK[:1], False, '43ccf437b2dec339a897f1a5d7e9dba354f62dc62fb887da320abf2dcde80f8e'),   # J2 China Tank
    'A-SP-NUK': (TNK_NUK[1:], False, '13fbfa91a9a77ebd6829fa09eeda32cb2e4ec4a92997a91a5e70da48f092c181'),   # J2 China Nuke = J4's base
    'W-NUK-12': (TNK_NUK[1:], True, '4e5e886d81b7c6538a5b44650a68b8f3c32d5a37c7acfa9f402f92548c8b509b'),    # J4 claim
    'PKG':      (TNK_NUK, True, '14414fcd6ea7c84496fc7d74a43228c41c0f228988a135e613e3529025f58fec'),        # J3 + J1c: the package
}


def team_dicts(scb):
    """ScriptTeams -> (chunk, [(start, end, {key: (dict type, value offset, value)})]); a repeated key keeps its last entry."""
    tn = next(k for k in scb.root if k.name == 'ScriptTeams')
    b, p, out = tn.raw, 0, []
    while p < len(b):
        s = p; cnt = struct.unpack_from('<H', b, p)[0]; p += 2; d = {}
        for _ in range(cnt):
            kt = struct.unpack_from('<i', b, p)[0]; p += 4; t = kt & 0xFF; key = scb.byid.get(kt >> 8, '?')
            if t == 0:                      # DICT_BOOL
                d[key] = (t, p, b[p]); p += 1
            elif t == 1:                    # DICT_INT
                d[key] = (t, p, struct.unpack_from('<i', b, p)[0]); p += 4
            elif t == 2:                    # DICT_REAL
                d[key] = (t, p, struct.unpack_from('<f', b, p)[0]); p += 4
            elif t == 3:                    # DICT_ASCII
                n = struct.unpack_from('<H', b, p)[0]; d[key] = (t, p, b[p + 2:p + 2 + n].decode('latin1')); p += 2 + n
            elif t == 4:                    # DICT_UNICODE
                n = struct.unpack_from('<H', b, p)[0]; d[key] = (t, p, None); p += 2 + 2 * n
            else:
                raise ValueError('bad dict type %d' % t)
        out.append((s, p, d))
    assert p == len(b), 'a team dict overruns ScriptTeams'
    return tn, out


def owner_bytes(scb, owner):
    """A general's ScriptList bytes + the bytes of every team it owns, in file order (what a per-general transplant moves)."""
    lst = next(k for k in scb.root if k.name == 'PlayerScriptsList').kids[LAUNCH_FIX_OWNERS.index(owner)]
    tn, teams = team_dicts(scb)
    return scb._w([lst]) + b''.join(tn.raw[s:e] for s, e, d in teams if d.get('teamOwner', (0, 0, None))[2] == owner)


def nuke_genome(scb, no_spend=None):
    """W-NUK-12: NUKE_SCRIPT_EDITS + NUKE_TEAM_EDITS, each applied exactly once to the value it expects. Returns the edits (16)."""
    assert NUKE_OWNER in (NO_SPEND if no_spend is None else no_spend), \
        'nuke_genome: W-NUK-12 was built and played on China Nuke WITHOUT the spend patch only (NO_SPEND)'
    assert sha256(owner_bytes(scb, NUKE_OWNER)) == NUKE_PARTS_SHA256[0], \
        'nuke_genome: China Nuke\'s list + teams are not the bytes W-NUK-12 was built on: %s' % sha256(owner_bytes(scb, NUKE_OWNER))
    li = LAUNCH_FIX_OWNERS.index(NUKE_OWNER)
    scripts = {}
    for l, grp, nodes, i, sn in walk_scripts(scb):
        if l == li:
            assert script_name(sn) not in scripts, 'nuke_genome: two %r in China Nuke\'s list' % script_name(sn)
            scripts[script_name(sn)] = sn
    edits = 0
    for name, call, old, new in NUKE_SCRIPT_EDITS:
        sn = scripts[name]
        fl = script_flags(sn)[0]
        assert (fl[2], fl[3], fl[4]) == (0, 0, 1), 'nuke_genome: %r is not Hard-only: %r' % (name, fl)
        hits = []
        for c in (c for o in sn.kids if o.name == 'OrCondition' for c in o.kids):
            typ, key, ps = parse_call(scb, c.raw)
            ints = [j for j, p in enumerate(ps) if PTYPES[p[0]] == 'INT']
            if scb.byid.get(key >> 8) == call and len(ints) == 1 and ps[ints[0]][1] == old and \
                    (call != 'COUNTER' or any(len(p) > 3 and p[3] == '_ESCALATION' for p in ps)):
                hits.append((c, typ, key, ps, ints[0]))
        assert len(hits) == 1, 'nuke_genome: %d %s(%d) in %r' % (len(hits), call, old, name)
        c, typ, key, ps, j = hits[0]
        ps[j] = (ps[j][0], new, ps[j][2], ps[j][3]); c.raw = enc_call(typ, key, ps); edits += 1
    tn, teams = team_dicts(scb)
    b = bytearray(tn.raw)
    for name, field, old, new in NUKE_TEAM_EDITS:
        hits = [d for s, e, d in teams if d.get('teamName', (0, 0, None))[2] == name]
        assert len(hits) == 1 and hits[0].get('teamOwner', (0, 0, None))[2] == NUKE_OWNER, 'nuke_genome: team %r' % name
        t, off, v = hits[0][field]
        assert t == 1 and v == old, 'nuke_genome: %s of %r is %r' % (field, name, v)
        struct.pack_into('<i', b, off, new); edits += 1
    tn.raw = bytes(b)
    assert sha256(owner_bytes(scb, NUKE_OWNER)) == NUKE_PARTS_SHA256[1], 'nuke_genome: the result is not W-NUK-12\'s China Nuke'
    return edits


# GeneralsX: ai-wh1ter0se-econ opt-in steps: an engine behaviour that is OFF
# until a player's scripts call its action - so a stock .scb plays as before, and a FOCAL box arm turns it on for ONE general (generals=).
# Each adds, per chosen general list, one group "<group>" holding one Hard-only one-shot script whose condition is CONDITION_TRUE and
# whose single action is the new engine action (its int read from Scripts.h at run time); nothing else in the file changes.
SKIRMISH_GENERALS = [o for o in LAUNCH_FIX_OWNERS if o.startswith('Skirmish')]     # the 12 generals' lists


def engine_enum(name):
    """{internal name: int} of ScriptActionType ('a') or ConditionType ('c'), from the engine header next to this tool."""
    hdr = os.path.join(HERE, '..', '..', 'GeneralsMD', 'Code', 'GameEngine', 'Include', 'GameLogic', 'Scripts.h')
    src = re.sub(r'/\*.*?\*/', '', open(hdr, encoding='latin1').read(), flags=re.S)
    body = re.search(r'enum ScriptActionType\s*\{(.*?)\}' if name == 'a' else r'enum ConditionType\s*\{(.*?)\}', src, re.S).group(1)
    out, i = {}, 0
    for item in re.sub(r'//[^\n]*', '', body).split(','):
        item = item.strip()
        if not item:
            continue
        if '=' in item:
            nm, v = [x.strip() for x in item.split('=')]
            i = int(v, 0)
        else:
            nm = item
        out[nm] = i
        i += 1
    return out


def opt_in_step(scb, group, script_name_, action, ptypes, values, generals=None):
    """Add the one-shot opt-in script to each chosen general's list. Returns the number of lists changed."""
    enums_a, enums_c = engine_enum('a'), engine_enum('c')
    li_of = {o: i for i, o in enumerate(LAUNCH_FIX_OWNERS)}
    vers, true_raw = {}, None
    for _l, _g, _n, _i, sn in walk_scripts(scb):
        for o in sn.kids:
            vers.setdefault(o.name, o.ver)
            for c in (o.kids if o.name == 'OrCondition' else [o]):
                if c.kids is None and c.name in ('Condition', 'ScriptAction'):
                    vers.setdefault(c.name, c.ver)
                    if true_raw is None and c.name == 'Condition' and call_name(scb, c.raw)[0] == 'CONDITION_TRUE':
                        true_raw = c.raw
    assert true_raw is not None, 'opt_in_step: no CONDITION_TRUE donor'

    def key_of(name):
        if name not in scb.byname:
            nid = max(i for _n, i in scb.toc) + 1
            scb.toc.append((name, nid)); scb.byname[name] = nid; scb.byid[nid] = name
        return (scb.byname[name] << 8) | 3     # the low byte every stock call key carries

    ps = [(PTYPES.index(t), 0, 0.0, v) if isinstance(v, str) else (PTYPES.index(t), int(v), 0.0, '') for t, v in zip(ptypes, values)]
    changed = 0
    for owner in SKIRMISH_GENERALS:
        if generals is not None and owner not in generals:
            continue
        lst = [k for k in scb.root if k.name == 'PlayerScriptsList'][0].kids[li_of[owner]]
        assert not any(k.name == 'ScriptGroup' and astr(k.prefix, 0)[0] == group for k in lst.kids), 'opt_in_step: %s already has %s' % (owner, group)
        grp0 = next(k for k in lst.kids if k.name == 'ScriptGroup')
        donor = next(k for k in grp0.kids if k.name == 'Script')
        n = deep_copy(donor)
        set_script(n, name=script_name_, delay=0)
        _fl, _d, q = script_flags(n)
        pre = bytearray(n.prefix)
        pre[q:q + 6] = bytes((1, 1, 0, 0, 1, 0))      # active, one-shot, Hard only, not a subroutine - set explicitly
        n.prefix = bytes(pre)
        n.kids = [Node('OrCondition', vers['OrCondition'], kids=[Node('Condition', vers['Condition'], raw=true_raw)]),
                  Node('ScriptAction', vers['ScriptAction'], raw=enc_call(enums_a[action], key_of(action), ps))]
        g = deep_copy(grp0)
        nm_end = astr(g.prefix, 0)[1]
        g.prefix = enc_astr(group) + bytes([1]) + g.prefix[nm_end + 1:]
        g.kids = [n]
        lst.kids.append(g)
        changed += 1
    return changed


def opt_in_selftest(base, out, group, script_name_, action, generals=None):
    """Only the chosen lists gained one group (one script calling the action); every other byte of the file is base's."""
    a, b = SCB(base), SCB(out)
    assert b.toc[:len(a.toc)] == a.toc and [n for n, _i in b.toc[len(a.toc):]] == [action], b.toc[len(a.toc):]
    ra = [(k.name, k.ver, a._w([k])) for k in a.root if k.name != 'PlayerScriptsList']
    rb = [(k.name, k.ver, b._w([k])) for k in b.root if k.name != 'PlayerScriptsList']
    assert ra == rb, '%s changed a chunk outside the script lists' % group
    la = [k for k in a.root if k.name == 'PlayerScriptsList'][0].kids
    lb = [k for k in b.root if k.name == 'PlayerScriptsList'][0].kids
    want = {i for i, o in enumerate(LAUNCH_FIX_OWNERS) if o in SKIRMISH_GENERALS and (generals is None or o in generals)}
    for i, (x, y) in enumerate(zip(la, lb)):
        if i not in want:
            assert a._w([x]) == b._w([y]), '%s changed list %d' % (group, i)
            continue
        assert len(y.kids) == len(x.kids) + 1 and all(a._w([p]) == b._w([q]) for p, q in zip(x.kids, y.kids))
        g = y.kids[-1]
        assert astr(g.prefix, 0)[0] == group and [script_name(k) for k in g.kids] == [script_name_]
        assert script_flags(g.kids[0])[0] == [1, 1, 0, 0, 1, 0]
    print("  %s: +1 Hard one-shot script in %d lists, nothing else (on: %s)" % (group, len(want), sha256(out)))

# GeneralsX: ai-wh1ter0se-econ R (6; wh1ter0se 22/10: "building rotation should always be
# rotated to where it matters"): every Hard general calls AI_PLAYER_FACE_FRONT once - its barracks / war factory / arms dealer doors face
# its base front, its base defences face out. Engine-gated by the action (a stock .scb never calls it).
FACE_FRONT = True                         # False = the 0.9.67 package (FACE_FRONT_OFF_SHA256)
FACE_FRONT_OFF_SHA256 = "14414fcd6ea7c84496fc7d74a43228c41c0f228988a135e613e3529025f58fec"
FACE_FRONT_ARGS = ('GX Face Front', 'GX Face Front', 'AI_PLAYER_FACE_FRONT', ['SIDE'], ['<This Player>'])


# GeneralsX: ai-wh1ter0se-econ: a small editor for the arms' recipe steps (U, T): calls built from stock donors (their
# ints checked against Scripts.h) or from NEW engine names, scripts with explicitly set header flags, one appended group per list, and
# team dicts copied from a donor team of the same owner with the arm's keys set. Same shape as gla_dozer_steal's helpers.
def gx_engine_enum(name):
    """{internal name: int} of ScriptActionType ('a') or ConditionType ('c'), from the engine header next to this tool."""
    hdr = os.path.join(HERE, '..', '..', 'GeneralsMD', 'Code', 'GameEngine', 'Include', 'GameLogic', 'Scripts.h')
    src = re.sub(r'/\*.*?\*/', '', open(hdr, encoding='latin1').read(), flags=re.S)
    body = re.search(r'enum ScriptActionType\s*\{(.*?)\}' if name == 'a' else r'enum ConditionType\s*\{(.*?)\}', src, re.S).group(1)
    out, i = {}, 0
    for item in re.sub(r'//[^\n]*', '', body).split(','):
        item = item.strip()
        if not item:
            continue
        if '=' in item:
            nm, v = [x.strip() for x in item.split('=')]
            i = int(v, 0)
        else:
            nm = item
        out[nm] = i
        i += 1
    return out


class GXEditor:
    def __init__(s, scb, new_calls=None):
        s.scb, s.new = scb, dict(new_calls or {})
        s.enums = {'a': gx_engine_enum('a'), 'c': gx_engine_enum('c')}
        s.donors, s.vers = {}, {}
        for _l, _g, _n, _i, sn in walk_scripts(scb):
            for o in sn.kids:
                s.vers.setdefault(o.name, o.ver)
                for c in (o.kids if o.name == 'OrCondition' else [o]):
                    if c.kids is None and c.name in ('Condition', 'ScriptAction'):
                        s.vers.setdefault(c.name, c.ver)
                        nm = call_name(scb, c.raw)[0]
                        if nm not in s.donors:
                            typ, key, ps = parse_call(scb, c.raw)
                            s.donors[nm] = (c.name, typ, [p[0] for p in ps])
        s.li_of = {o: i for i, o in enumerate(LAUNCH_FIX_OWNERS)}

    def key_of(s, name):
        scb = s.scb
        if name not in scb.byname:
            nid = max(i for _n, i in scb.toc) + 1
            scb.toc.append((name, nid)); scb.byname[name] = nid; scb.byid[nid] = name
        return (scb.byname[name] << 8) | 3     # the low byte every stock call key carries

    def call(s, name, *values):
        if name in s.new:
            kind, ptypes = s.new[name]
            node, typ = ('ScriptAction' if kind == 'a' else 'Condition'), s.enums[kind][name]
            ptypes = [PTYPES.index(t) for t in ptypes]
        else:
            node, typ, ptypes = s.donors[name]
            assert typ == s.enums['a' if node == 'ScriptAction' else 'c'][name], 'engine enum moved for %s' % name
        assert len(values) == len(ptypes), (name, values)
        ps = []
        for t, v in zip(ptypes, values):
            if isinstance(v, str):
                ps.append((t, 0, 0.0, v))
            elif isinstance(v, float):
                ps.append((t, 0, v, ''))
            else:
                ps.append((t, int(v), 0.0, ''))
        return Node(node, s.vers[node], raw=enc_call(typ, s.key_of(name), ps))

    def lst(s, owner):
        return [k for k in s.scb.root if k.name == 'PlayerScriptsList'][0].kids[s.li_of[owner]]

    def script(s, owner, name, flags, delay, ors, actions):
        """flags = (active, oneshot, easy, normal, hard, subroutine), set explicitly (copied stock headers carry other flags)."""
        grp0 = next(k for k in s.lst(owner).kids if k.name == 'ScriptGroup')
        n = deep_copy(next(k for k in grp0.kids if k.name == 'Script'))
        set_script(n, name=name, delay=delay)
        _fl, _d, q = script_flags(n)
        pre = bytearray(n.prefix)
        pre[q:q + 6] = bytes(flags)
        n.prefix = bytes(pre)
        n.kids = [Node('OrCondition', s.vers['OrCondition'], kids=list(o)) for o in ors] + list(actions)
        return n

    def add_group(s, owner, group, scripts):
        lst = s.lst(owner)
        assert not any(k.name == 'ScriptGroup' and astr(k.prefix, 0)[0] == group for k in lst.kids), '%s already has %s' % (owner, group)
        grp0 = next(k for k in lst.kids if k.name == 'ScriptGroup')
        g = deep_copy(grp0)
        nm_end = astr(g.prefix, 0)[1]
        g.prefix = enc_astr(group) + bytes([1]) + g.prefix[nm_end + 1:]   # name, active=1, rest (subroutine byte) kept
        g.kids = list(scripts)
        lst.kids.append(g)
        return len(scripts)

    def add_team(s, owner, team, units, extra):
        """Append a ScriptTeams dict: keys copied from the owner's first team, the arm's keys set (unit slots 1-7, the 10 hooks, extra)."""
        tn = next(k for k in s.scb.root if k.name == 'ScriptTeams')
        b, p, dicts = tn.raw, 0, []
        while p < len(b):
            cnt = struct.unpack_from('<H', b, p)[0]; p += 2; ent = []
            for _ in range(cnt):
                kt = struct.unpack_from('<i', b, p)[0]; p += 4; t = kt & 0xFF; key = s.scb.byid.get(kt >> 8, '?')
                if t == 0:
                    v = b[p]; p += 1
                elif t in (1, 2):
                    v = b[p:p + 4]; p += 4
                elif t == 3:
                    n = struct.unpack_from('<H', b, p)[0]; v = b[p + 2:p + 2 + n].decode('latin1'); p += 2 + n
                elif t == 4:
                    n = struct.unpack_from('<H', b, p)[0]; v = b[p:p + 2 + 2 * n]; p += 2 + 2 * n
                else:
                    raise ValueError('bad dict type %d' % t)
                ent.append((key, t, v))
            dicts.append(ent)
        donor = next(d for d in dicts if dict((k, v) for k, _t, v in d).get('teamOwner') == owner)
        want = {'teamName': (3, team), 'teamOwner': (3, owner), 'teamIsSingleton': (0, 0), 'exportWithScript': (0, 1),
                'teamInitialIdleFrames': (1, 0), 'teamOnCreateScript': (3, ''), 'teamOnDestroyedScript': (3, ''), 'teamOnIdleScript': (3, '')}
        for k in range(1, 8):
            tpl, n = units[k - 1] if k <= len(units) else ('', 0)
            want['teamUnitType%d' % k] = (3, tpl)
            want['teamUnitMinCount%d' % k] = (1, n)
            want['teamUnitMaxCount%d' % k] = (1, n)
        for k in range(10):
            want['teamGenericScriptHook%d' % k] = (3, '')
        want.update(extra)
        out, seen = [], set()
        for key, t, v in donor:
            if key in want:
                t2, v2 = want[key]
                assert t2 == t, 'add_team: %s type %d != %d' % (key, t, t2)
                out.append((key, t, v2)); seen.add(key)
            else:
                out.append((key, t, v))
        for key in sorted(set(want) - seen):
            out.append((key,) + want[key])
        raw = struct.pack('<H', len(out))
        for key, t, v in out:
            raw += struct.pack('<i', (s.key_of(key) & ~0xFF) | t)
            if t == 0:
                raw += bytes([int(v)])
            elif t == 1:
                raw += v if isinstance(v, bytes) else struct.pack('<i', v)
            elif t == 2:
                raw += v if isinstance(v, bytes) else struct.pack('<f', v)
            elif t == 3:
                raw += enc_astr(v)
            else:
                raw += v
        tn.raw = b + raw
        return 1


def gx_arm_selftest(base, out, owners, label):
    """The arm's step only ADDED: groups at the end of the chosen owners' lists, team dicts at the end of ScriptTeams, new chunk-table
    names; every other byte is base's."""
    a, b = SCB(base), SCB(out)
    assert b.toc[:len(a.toc)] == a.toc, '%s reordered the chunk table' % label
    ra = [(k.name, k.ver, a._w([k])) for k in a.root if k.name not in ('PlayerScriptsList', 'ScriptTeams')]
    rb = [(k.name, k.ver, b._w([k])) for k in b.root if k.name not in ('PlayerScriptsList', 'ScriptTeams')]
    assert ra == rb, '%s changed a chunk outside the script lists and teams' % label
    ta = next(k for k in a.root if k.name == 'ScriptTeams').raw
    tb = next(k for k in b.root if k.name == 'ScriptTeams').raw
    assert tb[:len(ta)] == ta, '%s changed a stock team' % label
    la = [k for k in a.root if k.name == 'PlayerScriptsList'][0].kids
    lb = [k for k in b.root if k.name == 'PlayerScriptsList'][0].kids
    want = {i for i, o in enumerate(LAUNCH_FIX_OWNERS) if o in owners}
    for i, (x, y) in enumerate(zip(la, lb)):
        if i not in want:
            assert a._w([x]) == b._w([y]), '%s changed list %d' % (label, i)
            continue
        assert len(y.kids) == len(x.kids) + 1 and all(a._w([p]) == b._w([q]) for p, q in zip(x.kids, y.kids)), '%s list %d' % (label, i)
    print("  %s: +1 group in %d lists, +%d team bytes, +%d chunk names, nothing else (on: %s)" % (
        label, len(want), len(tb) - len(ta), len(b.toc) - len(a.toc), sha256(out)))

# GeneralsX: ai-wh1ter0se-econ T (7; wh1ter0se 22/10: "keeping gla troops like 4-6 in the
# tunnel mostly should be anti air units like quads to pop when it detects incoming airplanes/migs/raptors and then hide back instantly").
# Per GLA general (Hard only):
#  * "GX Tunnel AA Watch <side>" (one-shot): AI_PLAYER_TUNNEL_AIR_WATCH(700) - the engine names incoming aircraft as the tunnels' nemesis;
#  * team "GLA Tunnel AA <side>" = 4 Quad Cannons + 2 Tunnel Defenders (RPG) on the STOCK team path: production condition "GX Tunnel AA
#    Cond <side>" = own tunnel network AND the current enemy owns an airfield ('GX AA Threats'), priority 500, at most 1, not recruitable,
#    team-common-target off; on creation it guards in the tunnel network (TEAM_GUARD_IN_TUNNEL_NETWORK). 6 of the network's 10 places;
#  * the general's two STOCK tunnel-guard teams (Hard 3W and 5W, on-create "GLA Behavior: Guard Tunnel", 10 units each) become 1 Quad
#    Cannon + 1 Tunnel Defender each, so 6 + 2 + 2 = 10 always fits the whole network (TunnelTracker::isValidContainerFor refuses a unit
#    when it is full: an airfield built after the stock team left the AA team outside). wh1ter0se's "4-6 in the
#    tunnel, mostly AA": 4 without an enemy airfield, up to 10 with one; every one of them can fire at aircraft.
TUNNEL_AA = True                           # False = the 0.9.68 package (TUNNEL_AA_OFF_SHA256 = R on all twelve generals)
TUNNEL_AA_OFF_SHA256 = "14d03e02a70e4f6c02eaa30b2b3735929b4a673e21cc52ec28efa9f35c96620d"
TUNNEL_SIDES = [('SkirmishGLA', ''), ('SkirmishGLADemolitionGeneral', 'Demo_'), ('SkirmishGLAToxinGeneral', 'Chem_'),
                ('SkirmishGLAStealthGeneral', 'Slth_')]
TUNNEL_AA_RADIUS = 700                     # Quad AA range 350 + ~0.5 s scan + ~1 s exit at a Raptor's 175/s, rounded up
TUNNEL_AA_NEW_CALLS = {'AI_PLAYER_TUNNEL_AIR_WATCH': ('a', ['SIDE', 'INT', 'TEAM'])}
TUNNEL_STOCK_GUARD_CREATE = 'GLA Behavior: Guard Tunnel'   # the on-create script of the stock 3W / 5W tunnel-guard teams (2 per general)
TUNNEL_UNIT_KEYS = ['teamUnit%s%d' % (f, k) for k in range(1, 8) for f in ('Type', 'MinCount', 'MaxCount')]


def gx_teams_read(scb, b):
    """ScriptTeams bytes -> [[(key, type, value)]] in file order (the dict format GXEditor.add_team reads and writes)."""
    p, dicts = 0, []
    while p < len(b):
        cnt = struct.unpack_from('<H', b, p)[0]; p += 2; ent = []
        for _ in range(cnt):
            kt = struct.unpack_from('<i', b, p)[0]; p += 4; t = kt & 0xFF; key = scb.byid[kt >> 8]
            if t == 0:
                v = b[p]; p += 1
            elif t in (1, 2):
                v = b[p:p + 4]; p += 4
            elif t == 3:
                n = struct.unpack_from('<H', b, p)[0]; v = b[p + 2:p + 2 + n].decode('latin1'); p += 2 + n
            elif t == 4:
                n = struct.unpack_from('<H', b, p)[0]; v = b[p:p + 2 + 2 * n]; p += 2 + 2 * n
            else:
                raise ValueError('bad dict type %d' % t)
            ent.append((key, t, v))
        dicts.append(ent)
    return dicts


def gx_teams_write(scb, dicts):
    out = b''
    for d in dicts:
        out += struct.pack('<H', len(d))
        for key, t, v in d:
            out += struct.pack('<i', (scb.byname[key] << 8) | t)
            out += bytes([int(v)]) if t == 0 else enc_astr(v) if t == 3 else v
    return out


def tunnel_resize_guards(scb, owners):
    """Each chosen general's stock tunnel-guard teams -> 1 Quad Cannon + 1 Tunnel Defender (unit slots 3-7 emptied). Returns how many."""
    tn = next(k for k in scb.root if k.name == 'ScriptTeams')
    dicts = gx_teams_read(scb, tn.raw)
    pre_of = dict(TUNNEL_SIDES)
    n = 0
    for d in dicts:
        kv = {k: v for k, _t, v in d}
        o = kv.get('teamOwner')
        if o not in owners or kv.get('teamOnCreateScript') != TUNNEL_STOCK_GUARD_CREATE:
            continue
        slots = [(pre_of[o] + 'GLAVehicleQuadCannon', 1), (pre_of[o] + 'GLAInfantryTunnelDefender', 1)]
        want = {}
        for k in range(1, 8):
            tpl, c = slots[k - 1] if k <= len(slots) else ('', 0)
            want['teamUnitType%d' % k], want['teamUnitMinCount%d' % k], want['teamUnitMaxCount%d' % k] = tpl, c, c
        seen = set()
        for i, (key, t, v) in enumerate(d):
            if key in want:
                assert t == (3 if key.startswith('teamUnitType') else 1), (key, t)
                d[i] = (key, t, want[key] if t == 3 else struct.pack('<i', want[key]))
                seen.add(key)
        assert {'teamUnitType1', 'teamUnitType2', 'teamUnitMinCount1', 'teamUnitMinCount2', 'teamUnitMaxCount1',
                'teamUnitMaxCount2'} <= seen, 'tunnel_resize_guards: %s lacks a unit slot' % kv.get('teamName')
        n += 1
    tn.raw = gx_teams_write(scb, dicts)
    return n


def tunnel_resize_selftest(base, resized, owners):
    """base -> resized changed exactly the chosen generals' stock tunnel-guard teams, and only their unit slots."""
    a, b = SCB(base), SCB(resized)
    assert a.toc == b.toc and [(k.name, a._w([k])) for k in a.root if k.name != 'ScriptTeams'] == \
        [(k.name, b._w([k])) for k in b.root if k.name != 'ScriptTeams'], 'tunnel_resize changed something outside ScriptTeams'
    da = gx_teams_read(a, next(k for k in a.root if k.name == 'ScriptTeams').raw)
    db = gx_teams_read(b, next(k for k in b.root if k.name == 'ScriptTeams').raw)
    assert len(da) == len(db)
    n = 0
    for x, y in zip(da, db):
        if x == y:
            continue
        kv = {k: v for k, _t, v in x}
        assert kv.get('teamOwner') in owners and kv.get('teamOnCreateScript') == TUNNEL_STOCK_GUARD_CREATE, kv.get('teamName')
        assert [e for e in x if e[0] not in TUNNEL_UNIT_KEYS] == [e for e in y if e[0] not in TUNNEL_UNIT_KEYS], kv.get('teamName')
        n += 1
    assert n == 2 * len(owners), 'tunnel_resize: %d teams changed for %d generals' % (n, len(owners))
    return n


# GeneralsX 27/10/2026 ai-pro-rules-2 T2: the engine now gates the team itself (built only while a
# HOSTILE player - any of them, also in FFA - owns an airfield, reinforced only while that or an aircraft seen in the last 180 s holds,
# released when neither does: AIPlayer::gxAirTeamAllowed / AISkirmishPlayer::gxAirLifetime), so the production condition keeps only "own
# tunnel network" (the old "<This Player's Enemy> owns a 'GX AA Threats' airfield" saw the current enemy only). watch=False is the
# ablation arm T0: the same team and recipe with the air watch's radius NEGATIVE = observe only (the engine logs what T would see and
# assigns nothing) - the two arms differ in that one INT.
TUNNEL_AA_WATCH = True                     # False = T0 (the selftest checks it differs from T only in the radius' sign)


def tunnel_aa(scb, generals=None, watch=None):
    """Add the air watch + the AA guard team to each chosen GLA general. Returns the number of lists changed."""
    watch = TUNNEL_AA_WATCH if watch is None else watch
    e = GXEditor(scb, TUNNEL_AA_NEW_CALLS)
    ME, T = '<This Player>', '<This Team>'
    GE = 3                                 # Parameter::COMPARISON
    changed = 0
    assert tunnel_resize_guards(scb, [o for o, _p in TUNNEL_SIDES if generals is None or o in generals]) == \
        2 * len([o for o, _p in TUNNEL_SIDES if generals is None or o in generals])
    for owner, pre in TUNNEL_SIDES:
        if generals is not None and owner not in generals:
            continue
        side = owner[len('Skirmish'):]
        cond, guard = 'GX Tunnel AA Cond %s' % side, 'GX Tunnel AA Guard %s' % side
        scripts = [
            e.script(owner, 'GX Tunnel AA Watch %s' % side, (1, 1, 0, 0, 1, 0), 0, [[e.call('CONDITION_TRUE')]],
                     [e.call('AI_PLAYER_TUNNEL_AIR_WATCH', ME, TUNNEL_AA_RADIUS if watch else -TUNNEL_AA_RADIUS, 'GLA Tunnel AA %s' % side)]),
            # a production condition: a Hard-only subroutine with no actions (TeamPrototype::evaluateProductionCondition); the hostile
            # airfield term is the engine's (above)
            e.script(owner, cond, (1, 0, 0, 0, 1, 1), 5,
                     [[e.call('PLAYER_HAS_OBJECT_COMPARISON', ME, GE, 1, pre + 'GLATunnelNetwork')]], []),
            # the team's on-create: a subroutine, every difficulty (like the steal's team hooks)
            e.script(owner, guard, (1, 0, 1, 1, 1, 1), 0, [[e.call('CONDITION_TRUE')]], [e.call('TEAM_GUARD_IN_TUNNEL_NETWORK', T)]),
        ]
        e.add_group(owner, 'GX Tunnel AA', scripts)
        e.add_team(owner, 'GLA Tunnel AA %s' % side, [(pre + 'GLAVehicleQuadCannon', 4), (pre + 'GLAInfantryTunnelDefender', 2)],
                   {'teamProductionPriority': (1, 500), 'teamMaxInstances': (1, 1), 'teamProductionCondition': (3, cond),
                    'teamIsAIRecruitable': (0, 0), 'teamOnCreateScript': (3, guard), 'teamAttackCommonTarget': (0, 0),
                    'teamAutoReinforce': (0, 1)})   # refilled below six (AIPlayer::selectTeamToReinforce)
        changed += 1
    return changed

# GeneralsX @bugfix C-laser-power 04/11/2026 (wh1ter0se's $200k games): Crusader and Paladin are the
# only units in the game that draw power, so a Laser General under power disables its whole tank army, and the tanks cannot leave a door or
# go and fight, so the AI has no way to lower the load. A plant is forced only into an empty build-list slot (Laser has 6), and past that the
# only source is "USA 7+ Power Plants Flank/Front - H", whose SKIRMISH_BUILD_STRUCTURE_* walks an arc shared with base defences that runs out
# for good (AISkirmishPlayer.cpp, `angle > PI/3`). spend_patch clones factories with money and no plant. So those two scripts keep their arc
# call and also ask for a plant beside the warehouse, the action spend_patch's rungs use: same conditions, same 40 s toggle, Laser list only.
# The other generals have no unit that draws power (210 seats measured, handback section 3), so this stays Laser's.
LASER_POWER = True                        # False = T2C's package, TUNNEL_AA on (LASER_POWER_OFF_SHA256); with TUNNEL_AA off too = 14d03e02, 0.9.68-0.9.69
LASER_POWER_OFF_SHA256 = "db81fa2f2be4f4b7c831cbc375853a8fced1af2ba5d5460df5150f80079d462d"   # T2C on 6afae411ea, 06/11/2026: Laser is the last step, so its off pin is T2C's output (was 14d03e02 before T2C)
LASER_POWER_OWNER = 'SkirmishAmericaLaserGeneral'
LASER_POWER_PLANT = 'Lazr_AmericaPowerPlant'
LASER_POWER_SCRIPTS = ('USA 7+ Power Plants Flank - H', 'USA 7+ Power Plants Front - H')

def laser_power_step(scb):
    """Add AI_PLAYER_BUILD_SUPPLY_CENTER(plant) right after the arc call of Laser's two 7+ plant scripts. Returns the number of scripts changed (2)."""
    li = LAUNCH_FIX_OWNERS.index(LASER_POWER_OWNER)
    donor = next(o for l, _g, _n, _i, sn in walk_scripts(scb) if l == li for o in sn.kids
                 if o.name == 'ScriptAction' and call_name(scb, o.raw)[0] == 'AI_PLAYER_BUILD_SUPPLY_CENTER')
    typ, key, ps0 = parse_call(scb, donor.raw)
    ps1 = [(p[0], p[1], p[2], LASER_POWER_PLANT) if PTYPES[p[0]] == 'OBJECT_TYPE' else ((p[0], SUPPLY_MIN, 0.0, '') if PTYPES[p[0]] == 'INT' else p) for p in ps0]
    done = []
    for l, _g, _n, _i, sn in walk_scripts(scb):
        if l != li or script_name(sn) not in LASER_POWER_SCRIPTS:
            continue
        at = next(k for k, o in enumerate(sn.kids) if o.name == 'ScriptAction' and call_name(scb, o.raw)[0].startswith('SKIRMISH_BUILD_STRUCTURE'))
        sn.kids.insert(at + 1, Node('ScriptAction', donor.ver, raw=enc_call(typ, key, ps1)))
        done.append(script_name(sn))
    assert sorted(done) == sorted(LASER_POWER_SCRIPTS), 'laser_power_step: %r' % done
    return len(done)


def laser_power_selftest(base, out):
    """Exactly one action was added to each of the two scripts; every other byte of the file is base's, Laser's other scripts included."""
    a, b = SCB(base), SCB(out)
    assert a.toc == b.toc, 'laser_power changed the chunk table'
    ra = [(k.name, k.ver, a._w([k])) for k in a.root if k.name != 'PlayerScriptsList']
    rb = [(k.name, k.ver, b._w([k])) for k in b.root if k.name != 'PlayerScriptsList']
    assert ra == rb, 'laser_power changed a chunk outside the script lists'
    la = [k for k in a.root if k.name == 'PlayerScriptsList'][0].kids
    lb = [k for k in b.root if k.name == 'PlayerScriptsList'][0].kids
    want = LAUNCH_FIX_OWNERS.index(LASER_POWER_OWNER)
    assert len(la) == len(lb)
    for i, (x, y) in enumerate(zip(la, lb)):
        if i != want:
            assert a._w([x]) == b._w([y]), 'laser_power changed list %d' % i
    wa, wb = list(walk_scripts(a)), list(walk_scripts(b))
    assert len(wa) == len(wb), 'laser_power changed the script count'
    changed = []
    for (l, _g, _n, _i, sa), (m, _h, _o, _j, sb) in zip(wa, wb):
        assert l == m and script_name(sa) == script_name(sb) and sa.prefix == sb.prefix
        if a._w([sa]) == b._w([sb]):
            continue
        assert l == want and script_name(sa) in LASER_POWER_SCRIPTS, 'laser_power changed %s' % script_name(sa)
        extra = [k for k in range(len(sb.kids)) if k >= len(sa.kids) or a._w([sa.kids[k]]) != b._w([sb.kids[k]])][0]
        rest = sb.kids[:extra] + sb.kids[extra + 1:]
        assert len(rest) == len(sa.kids) and all(a._w([p]) == b._w([q]) for p, q in zip(sa.kids, rest))
        nm, ps = call_name(b, sb.kids[extra].raw)
        assert nm == 'AI_PLAYER_BUILD_SUPPLY_CENTER' and [p[3] for p in ps if PTYPES[p[0]] == 'OBJECT_TYPE'] == [LASER_POWER_PLANT], (nm, ps)
        assert sb.kids[extra - 1].name == 'ScriptAction' and call_name(b, sb.kids[extra - 1].raw)[0].startswith('SKIRMISH_BUILD_STRUCTURE'), 'laser_power: the action must follow the arc call'
        del sb.kids[extra]
        changed.append(script_name(sa))
    assert sorted(changed) == sorted(LASER_POWER_SCRIPTS), changed
    assert b.write() == base, 'laser_power: the file without the two actions is not the base, byte for byte'
    print("  laser_power: +1 AI_PLAYER_BUILD_SUPPLY_CENTER(%s) in %d Laser scripts, every other byte unchanged (on: %s)" % (LASER_POWER_PLANT, len(changed), sha256(out)))


# USA Search and Destroy (USA_SD): every USA general's "USA Use Battle Plans" picks Hold the Line for good, and the
# pro plan is Search and Destroy (+20% ground range and sight, stealth detection at the Strategy Center). That one script is diff=111 and only Hard is
# ever measured, so stock keeps Easy + Normal and a Hard copy, named USA_SD_HARD, carries the button. Delay 0, not stock's 5 or 3: the plan then waits
# only for its 7 s unpack, and it must be 0: a script with a delay draws from the shared logic RNG at map load (ScriptEngine.cpp, setFrameToEvaluate), so
# one more delayed script reshuffles the whole match from frame 1 and the A/B's same seed no longer means the same game.
# The stock script is a one-shot, so a rebuilt centre never got a plan: 90-520 s without one in 4 of 46 seats (ai/usa-sd). USA_SD_REARM starts off, the Hard
# copy switches it on, and with no finished centre standing it switches the Hard copy back on: two one-shots, each firing once per loss, no order every frame.
# No switching back when attacked: a change of plan paralyses the whole army for 5 s (BattlePlanUpdate::setBattlePlan).
USA_SD = True                             # False = LASER_POWER's package (USA_SD_OFF_SHA256)
USA_SD_OFF_SHA256 = "e5badd871d137cda878c1ee5f528e7f24c495b15ab3970f0189856668ddbf07c"
USA_SD_OWNERS = ('SkirmishAmerica', 'SkirmishAmericaAirForceGeneral', 'SkirmishAmericaLaserGeneral', 'SkirmishAmericaSuperWeaponGeneral')
USA_SD_STOCK = 'USA Use Battle Plans'
USA_SD_HARD = 'USA Use Battle Plans - H'
USA_SD_REARM = 'USA Re-arm Battle Plan - H'
USA_SD_HOLD = 'Command_InitiateBattlePlanHoldTheLine'
USA_SD_BUTTON = 'Command_InitiateBattlePlanSearchAndDestroy'
USA_SD_DELAY = 0                          # keep 0: see above

def usa_sd_step(scb, button=None):
    """Split each USA general's stock plan script into Easy + Normal (stock) and a Hard copy with USA_SD_BUTTON, then add its re-arm. button = USA_SD_HOLD: the inert A/B arm."""
    owners = [LAUNCH_FIX_OWNERS.index(o) for o in USA_SD_OWNERS]
    found = [(l, n, sn) for l, _g, n, _i, sn in walk_scripts(scb) if l in owners and script_name(sn) == USA_SD_STOCK]
    assert sorted(l for l, _n, _s in found) == sorted(owners), 'usa_sd_step: %r' % [l for l, _n, _s in found]
    donor = next(o for _l, _g, _n, _i, sn in walk_scripts(scb) for o in sn.kids if o.name == 'ScriptAction' and call_name(scb, o.raw)[0] == 'ENABLE_SCRIPT')
    etyp, ekey, eps = parse_call(scb, donor.raw)
    def enable(script):
        return Node('ScriptAction', donor.ver, raw=enc_call(etyp, ekey, [(eps[0][0], eps[0][1], eps[0][2], script)]))
    for _l, nodes, sn in found:
        assert script_flags(sn)[0][2:5] == [1, 1, 1], 'usa_sd_step: the stock script is not diff=111'
        hard = deep_copy(sn)
        set_script(sn, diff=(1, 1, 0))
        set_script(hard, name=USA_SD_HARD, diff=(0, 0, 1), delay=USA_SD_DELAY)
        act = [o for o in hard.kids if o.name == 'ScriptAction']
        assert len(act) == 1 and call_name(scb, act[0].raw)[0] == 'TEAM_USE_COMMANDBUTTON_ABILITY', 'usa_sd_step: not the one stock action'
        typ, key, ps = parse_call(scb, act[0].raw)
        assert ps[1][3] == USA_SD_HOLD, ps
        act[0].raw = enc_call(typ, key, [ps[0], (ps[1][0], ps[1][1], ps[1][2], button or USA_SD_BUTTON)])
        rearm = deep_copy(hard)
        hard.kids.append(enable(USA_SD_REARM))
        set_script(rearm, name=USA_SD_REARM, active=0)
        cond = [c for o in rearm.kids if o.name == 'OrCondition' for c in o.kids]
        assert len(cond) == 1 and call_name(scb, cond[0].raw)[0] == 'PLAYER_HAS_OBJECT_COMPARISON', 'usa_sd_step: not the one stock condition'
        ctyp, ckey, cps = parse_call(scb, cond[0].raw)
        assert (cps[1][1], cps[2][1]) == (3, 1), cps                  # >= 1 becomes == 0 (Parameter::EQUAL), the same object type
        cond[0].raw = enc_call(ctyp, ckey, [cps[0], (cps[1][0], 2, cps[1][2], cps[1][3]), (cps[2][0], 0, cps[2][2], cps[2][3]), cps[3]])
        rearm.kids = [o for o in rearm.kids if o.name != 'ScriptAction'] + [enable(USA_SD_HARD)]
        at = nodes.index(sn); nodes[at + 1:at + 1] = [hard, rearm]
    return len(found)


def usa_sd_selftest(base, out):
    """Each USA list gained the Hard copy and its stock script lost Hard, nothing else: without those two edits the file is base, byte for byte."""
    a, b = SCB(base), SCB(out)
    assert a.toc == b.toc, 'usa_sd changed the chunk table'
    assert [(k.name, k.ver, a._w([k])) for k in a.root if k.name != 'PlayerScriptsList'] == [(k.name, k.ver, b._w([k])) for k in b.root if k.name != 'PlayerScriptsList'], \
        'usa_sd changed a chunk outside the script lists'
    owners = [LAUNCH_FIX_OWNERS.index(o) for o in USA_SD_OWNERS]
    la, lb = [[k for k in s.root if k.name == 'PlayerScriptsList'][0].kids for s in (a, b)]
    assert len(la) == len(lb)
    for i, (x, y) in enumerate(zip(la, lb)):
        assert (a._w([x]) != b._w([y])) == (i in owners), 'usa_sd: list %d' % i
    hard = [(l, n, sn) for l, _g, n, _i, sn in walk_scripts(b) if script_name(sn) == USA_SD_HARD]
    assert sorted(l for l, _n, _s in hard) == sorted(owners), 'usa_sd: the Hard copies are not one per USA list'
    for _l, nodes, sn in hard:
        at = nodes.index(sn); stock, rearm = nodes[at - 1], nodes[at + 1]
        fl, delay, _q = script_flags(sn); rf, rdelay, _q = script_flags(rearm)
        assert script_name(stock) == USA_SD_STOCK and script_flags(stock)[0][2:5] == [1, 1, 0] and fl == [1, 1, 0, 0, 1, 0] and delay == USA_SD_DELAY, 'usa_sd: flags'
        assert script_name(rearm) == USA_SD_REARM and rf == [0, 1, 0, 0, 1, 0] and rdelay == USA_SD_DELAY, 'usa_sd: the re-arm starts off, Hard only, no delay'
        acts = [call_name(b, o.raw) for o in sn.kids if o.name == 'ScriptAction']
        assert [n for n, _p in acts] == ['TEAM_USE_COMMANDBUTTON_ABILITY', 'ENABLE_SCRIPT'] and acts[0][1][1][3] == USA_SD_BUTTON and acts[1][1][0][3] == USA_SD_REARM, 'usa_sd: the Hard copy'
        racts = [call_name(b, o.raw) for o in rearm.kids if o.name == 'ScriptAction']
        assert [n for n, _p in racts] == ['ENABLE_SCRIPT'] and racts[0][1][0][3] == USA_SD_HARD, 'usa_sd: the re-arm'
        conds = [call_name(b, c.raw) for o in rearm.kids if o.name == 'OrCondition' for c in o.kids]
        assert len(conds) == 1 and conds[0][0] == 'PLAYER_HAS_OBJECT_COMPARISON' and [conds[0][1][1][1], conds[0][1][2][1]] == [2, 0], 'usa_sd: the re-arm reads no centre standing'
        set_script(stock, diff=(1, 1, 1))
        nodes.remove(sn); nodes.remove(rearm)
    assert b.write() == base, 'usa_sd: the file without the Hard copies is not the base, byte for byte'
    print("  usa_sd: %s split Easy + Normal / Hard + re-arm in %d USA lists, every other byte unchanged (on: %s)" % (USA_SD_STOCK, len(hard), sha256(out)))


# SEAM: extra (scb) -> int steps, run after easy_plus and before spend_patch (spend_patch touches none of their scripts).
# The "False = ... (pin X)" pins in the step switches above are historical: each was the output when that step was the newest, so
# it needs every LATER step and the China package off too (NO_SPEND = (), NUKE_GENOME = False; the selftest rebuilds 67eb7669 and
# cf4a7600 that way, the older pins are not re-checked). With
# the package on, a step switched off changes China Nuke's list and nuke_genome refuses those bytes.
FIXUP_STEPS = ([launch_fixes] if LAUNCH_FIXES else []) + ([expand_nearby] if EXPAND_NEARBY else []) + \
    ([capture_home] if CAPTURE_HOME else []) + ([expand_no_defence] if EXPAND_NO_DEFENCE else []) + \
    ([sw_fire_ready] if SW_FIRE_READY else [])

def generate(source_bytes, no_spend=None, genome=None, focal=None, taa_focal=None, taa_watch=None, sd_button=None):
    """retail bytes -> GeneralsX bytes. Every step is checked; a source that is not the pinned release never gets here.
    no_spend / genome override NO_SPEND / NUKE_GENOME (the selftest builds every PACKAGE_ARMS arm with them)."""
    no_spend = NO_SPEND if no_spend is None else no_spend
    scb = SCB(source_bytes)
    assert scb.write() == source_bytes, 'round-trip failed on the source'
    ez = easy_plus(scb)
    for step in FIXUP_STEPS:
        step(scb)
    added, tech, waves = spend_patch(scb, no_spend)
    if NUKE_GENOME if genome is None else genome:
        nuke_genome(scb, no_spend)
    if FACE_FRONT:
        opt_in_step(scb, *FACE_FRONT_ARGS, generals=focal)   # focal: a FOCAL box arm's one general (DESIGN 8.2); None = all twelve
    if TUNNEL_AA:
        tunnel_aa(scb, taa_focal, taa_watch)   # taa_focal: a FOCAL T arm's one GLA general (FACE_FRONT stays on all twelve); None = all
                                               # four. taa_watch False = the ablation arm T0
    if LASER_POWER:
        laser_power_step(scb)                  # USA Laser's list only; TUNNEL_AA touches GLA lists and the toc
    if USA_SD:
        usa_sd_step(scb, sd_button)            # the last step; sd_button = USA_SD_HOLD is the inert A/B arm
    out = scb.write()
    assert SCB(out).write() == out, 'round-trip failed on the output'
    return out, (ez, added, tech, waves)


# ------------------------------------------------------------------------------------------------- source / output
def sha256(b):
    return hashlib.sha256(b).hexdigest()


def env_path(name):
    v = os.environ.get(name, '').strip()
    if len(v) >= 2 and v[0] == v[-1] and v[0] in '"\'':
        v = v[1:-1]
    return v


def find_source(search):
    """First candidate that IS the pinned retail file. A candidate that is not (a previous recipe output in a
    deploy folder, a modded release) is skipped, not edited; pinned_source then explains what it saw."""
    cands = list(search) + [env_path(n) for n in ('CNC_GENERALS_ZH_PATH', 'GX_GAME_DATA', 'GX_GAME_SRC')] + \
        [os.path.join(os.path.expanduser('~'), 'GeneralsX', 'GeneralsZH')]
    for d in cands:
        base = os.path.join(d, *RETAIL_MEMBER.split('/')) if d else ''
        for p in ((base, base + RETAIL_BACKUP) if base else ()):
            if os.path.isfile(p):
                try:
                    if sha256(open(p, 'rb').read()) == SOURCE_SHA256:
                        return p
                except OSError:
                    pass
    return None


def pinned_source(search):
    p = find_source(search)
    if p is None:
        raise SystemExit("ERROR: no retail %s (sha256 %s) in: %s, $CNC_GENERALS_ZH_PATH, $GX_GAME_DATA, $GX_GAME_SRC, "
                         "~/GeneralsX/GeneralsZH - set GX_GAME_DATA to a retail Zero Hour install"
                         % (RETAIL_MEMBER, SOURCE_SHA256[:12], ", ".join(search) or "(no --search)"))
    print("  source     %s" % p)
    return open(p, 'rb').read()


def already_done(path):
    return OUTPUT_SHA256 != "TBD" and os.path.isfile(path) and sha256(open(path, 'rb').read()) == OUTPUT_SHA256


def build_base(search):
    """the recipe's own output, checked against BASE_SHA256 (what the rule bank compiles over)"""
    out, counts = generate(pinned_source(search))
    if BASE_SHA256 != "TBD" and sha256(out) != BASE_SHA256:
        raise SystemExit("ERROR: recipe output hashes to %s, not the pinned %s" % (sha256(out), BASE_SHA256))
    print("  generated  easy flags +%d, scripts +%d, tech gates %d, wave teams %d -> %s" % (counts + (sha256(out)[:12],)))
    return out


def build_bytes(search, rule_ids=None):
    """the shipped file: the recipe's output plus the promoted ruleset, if any (Codex rbd-r6-lensA 2), checked against OUTPUT_SHA256"""
    out = build_base(search)
    if os.path.isfile(SHIPPED_RULESET):
        import importlib.util
        spec = importlib.util.spec_from_file_location("gx_rulebank_compiler", os.path.join(HERE, "rulebank", "compiler.py"))
        C = importlib.util.module_from_spec(spec); spec.loader.exec_module(C)
        try:
            # this module as the compiler's recipe; loaded by path (make-weapon-ini.py, the selftests) it is not in sys.modules
            me = sys.modules.get(__name__) or types.SimpleNamespace(**globals())
            out, _man = C.compile_ruleset(me, out, open(SHIPPED_RULESET, 'rb').read(), tier_gate='on',
                                          rule_ids=rule_ids if rule_ids is not None else C.load_rule_ids())
        except C.Refused as e:
            raise SystemExit("ERROR: the promoted %s no longer compiles: %s" % (SHIPPED_RULESET, e))
        print("  ruleset    %s -> %s" % (os.path.basename(SHIPPED_RULESET), sha256(out)[:12]))
    if OUTPUT_SHA256 != "TBD" and sha256(out) != OUTPUT_SHA256:
        raise SystemExit("ERROR: shipped output hashes to %s, not the pinned %s" % (sha256(out), OUTPUT_SHA256))
    return out


def keep_pristine(path):
    """Before an in-place overwrite of the retail original (a --root deploy), keep it as <path>.retail: written to a
    temp file and renamed, and an existing backup that is not the pinned release is replaced (Codex r4 P1)."""
    bak = path + RETAIL_BACKUP
    if not (os.path.isfile(path) and sha256(open(path, 'rb').read()) == SOURCE_SHA256):
        return
    if os.path.isfile(bak) and sha256(open(bak, 'rb').read()) == SOURCE_SHA256:
        return
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(os.path.abspath(path)), suffix='.tmp')
    with os.fdopen(fd, 'wb') as f:
        f.write(open(path, 'rb').read())
    os.replace(tmp, bak)
    print("  kept       %s" % bak)


def write(path, data, keep_source=False):
    if os.path.isfile(path) and open(path, 'rb').read() == data:
        print("  unchanged  %s" % path)
        return
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    if keep_source:
        keep_pristine(path)
        if os.path.isfile(path):   # a modded file is not ours to destroy: keep it beside the target
            old = open(path, 'rb').read(); h = sha256(old)
            if h not in (SOURCE_SHA256, BASE_SHA256, OUTPUT_SHA256):
                um = '%s.user-%s' % (path, h[:8])
                if not os.path.isfile(um):
                    with open(um, 'wb') as f:
                        f.write(old)
                print("  kept       %s (the file being replaced was not the retail release)" % um)
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(os.path.abspath(path)))
    with os.fdopen(fd, 'wb') as f:
        f.write(data)
    os.replace(tmp, path)
    print("  wrote      %s" % path)


M_SP_SHA256 = "2ece564512ce15a66a877f4c6efecedc226c07a5b7d59a96c336699dd86ed540"   # the recipe with no spend_patch at all (the ablation's M-SP)


def package_selftest(data, out):
    """The China package (NO_SPEND + NUKE_GENOME), proven on written bytes. (1) Every box arm is a switch setting
    (PACKAGE_ARMS), and NO_SPEND naming all 12 generals == the recipe with no spend_patch at all (M-SP). (2) Scope: outside China
    Tank's and China Nuke's lists and owned teams the output is stock's bytes; China Tank's are M-SP's; China Nuke's are M-SP's
    but for exactly the 16 genome rows, each = round(old x its gene). (3) Fail arms: the genome on
    an SP-on China Nuke refuses, by switch and by bytes; a row off by one and a stale row refuse; one flipped byte is not PKG."""
    global spend_patch
    arms = {a: generate(data, no_spend=ns, genome=g)[0] for a, (ns, g, want) in PACKAGE_ARMS.items()}
    for a, (ns, g, want) in PACKAGE_ARMS.items():
        assert sha256(arms[a]) == want, "package arm %s: %s" % (a, sha256(arms[a]))
    assert (out == arms['PKG']) == ((NO_SPEND, NUKE_GENOME) == PACKAGE_ARMS['PKG'][:2]), "the switches do not match the output"
    every = tuple(LAUNCH_FIX_OWNERS[1:13])
    keep = spend_patch
    try:
        spend_patch = lambda scb, no_spend=None: (0, 0, 0)
        msp, _ = generate(data, no_spend=every, genome=False)
    finally:
        spend_patch = keep
    assert sha256(msp) == M_SP_SHA256 and generate(data, no_spend=every, genome=False)[0] == msp, "NO_SPEND of all 12 != no spend_patch"

    def parts(b):
        x = SCB(b); tn, teams = team_dicts(x)
        return (x.toc, [(k.name, x._w([k])) for k in x.root if k.name not in ('PlayerScriptsList', 'ScriptTeams')],
                [x._w([k]) for k in next(k for k in x.root if k.name == 'PlayerScriptsList').kids],
                [(d.get('teamOwner', (0, 0, ''))[2], tn.raw[s:e]) for s, e, d in teams])
    S, P, M = parts(arms['S']), parts(arms['PKG']), parts(msp)
    assert S[0] == P[0] == M[0] and S[1] == P[1] == M[1], "the package changed the chunk table or a chunk outside the lists and teams"
    tnk, nuk = (LAUNCH_FIX_OWNERS.index(o) for o in TNK_NUK)
    for i in range(len(S[2])):
        if i != nuk:
            assert P[2][i] == (M[2][i] if i == tnk else S[2][i]), "list %d (%s) is not %s" % (i, LAUNCH_FIX_OWNERS[i], 'M-SP' if i == tnk else 'stock')
    assert [o for o, _ in S[3]] == [o for o, _ in P[3]] == [o for o, _ in M[3]], "the team owners moved"
    for (o, s_), (_, p_), (_, m_) in zip(S[3], P[3], M[3]):
        if o != NUKE_OWNER:
            assert p_ == (m_ if o == TNK_NUK[0] else s_), "a team of %s is not %s" % (o, 'M-SP' if o == TNK_NUK[0] else 'stock')
    xm, xp, edits = SCB(msp), SCB(arms['PKG']), []

    def cmp(na, nb, script):
        assert len(na) == len(nb), "the genome added or removed a node in %r" % script
        for p, q in zip(na, nb):
            assert (p.name, p.ver, p.prefix) == (q.name, q.ver, q.prefix), "the genome changed a header or the order"
            s = script_name(p) if p.name == 'Script' else script
            if p.kids is not None:
                cmp(p.kids, q.kids, s)
            elif p.raw != q.raw:
                (t0, k0, a0), (t1, k1, a1) = parse_call(xm, p.raw), parse_call(xp, q.raw)
                ch = [j for j in range(len(a0)) if a0[j] != a1[j]]
                assert (t0, k0, len(a0)) == (t1, k1, len(a1)) and len(ch) == 1 and PTYPES[a0[ch[0]][0]] == 'INT', "genome leaf in %r" % s
                assert a1[ch[0]][2:] == a0[ch[0]][2:], "genome leaf in %r" % s
                edits.append((s, xm.byid.get(k0 >> 8), a0[ch[0]][1], a1[ch[0]][1]))
    lm, lp = (next(k for k in x.root if k.name == 'PlayerScriptsList').kids[nuk] for x in (xm, xp))
    cmp([lm], [lp], None)
    assert sorted(edits) == sorted(NUKE_SCRIPT_EDITS), "China Nuke's list differs from M-SP's by %r" % edits
    tedits = []
    for (s0, e0, d0), (s1, e1, d1) in zip(team_dicts(xm)[1], team_dicts(xp)[1]):   # China Nuke's teams (the rest: proven above)
        if d0.get('teamOwner', (0, 0, ''))[2] != NUKE_OWNER:
            continue
        assert [(k, t, o - s0) for k, (t, o, v) in d0.items()] == [(k, t, o - s1) for k, (t, o, v) in d1.items()], "team layout"
        tedits += [(d0['teamName'][2], k, v, d1[k][2]) for k, (t, o, v) in d0.items() if d1[k][2] != v]
    assert sorted(tedits) == sorted(NUKE_TEAM_EDITS), "China Nuke's teams differ from M-SP's by %r" % tedits
    # every row is the genome's value: round(old x its gene), a unit count at least 1 (forge_pack.py scale())
    for name, call, old, new in NUKE_SCRIPT_EDITS:
        assert new == max(0, int(round(old * NUKE_GENES[call]))) and (call == 'PLAYER_HAS_CREDITS' or 'Defense' in name), name
    for name, field, old, new in NUKE_TEAM_EDITS:
        assert new == max(1, int(round(old * NUKE_GENES['team']))) and re.match(r'China H (4|5)TH Wave', name), name
    try:
        generate(data, no_spend=TNK_NUK[:1], genome=True)
    except AssertionError as e:
        assert 'WITHOUT the spend patch only' in str(e), e     # the switch guard itself, not the bytes one behind it
    else:
        raise AssertionError("the genome ran on a China Nuke that has the spend patch")
    try:    # the same with the switch claiming NO_SPEND: the bytes refuse (A-SP-TNK's China Nuke has the spend patch)
        nuke_genome(SCB(arms['A-SP-TNK']), no_spend=TNK_NUK)
    except AssertionError as e:
        assert 'not the bytes W-NUK-12 was built on' in str(e), e
    else:
        raise AssertionError("the genome ran on spend-patched China Nuke bytes")
    def refused(rows):
        global NUKE_SCRIPT_EDITS
        keep = NUKE_SCRIPT_EDITS
        try:
            NUKE_SCRIPT_EDITS = rows
            generate(data)
            return 'applied'
        except AssertionError as e:
            return 'refused' if 'nuke_genome' in str(e) else str(e)
        finally:
            NUKE_SCRIPT_EDITS = keep
    one_off = refused([r if r[0] != 'China Air Field - H' else r[:3] + (r[3] + 1,) for r in NUKE_SCRIPT_EDITS])   # a new value off by one
    stale = refused([r if r[0] != 'China Air Field - H' else r[:2] + (r[2] + 1, r[3]) for r in NUKE_SCRIPT_EDITS])  # an old value it cannot find
    flipped = bytearray(arms['PKG']); flipped[len(flipped) // 2] ^= 1
    assert one_off == stale == 'refused' and sha256(bytes(flipped)) != PACKAGE_ARMS['PKG'][2], (one_off, stale)
    print("  china package: S, A-SP-TNK, A-SP-NUK, W-NUK-12, PKG = their box bytes; NO_SPEND of all 12 = no spend_patch (%s); outside"
          " China Tank/Nuke = stock; China Nuke = M-SP + exactly the 16 genome rows; refused: a row off by one, a stale row, the genome"
          " on spend-patched Nuke (by switch and by bytes)" % M_SP_SHA256[:8])


def selftest():
    p = find_source([])
    if p is None:
        print("selftest: no retail source on this box - recipe internals only")
    tiny = SCB.__new__(SCB)
    assert PTYPES[16] == 'COORD3D' and PTYPES[0] == 'INT'
    if p is not None:
        data = open(p, 'rb').read()
        if sha256(data) != SOURCE_SHA256:
            print("selftest: %s is not the pinned retail release - skipped generation" % p)
            return
        # GeneralsX 22/10/2026: every check below is about the 0.9.67 package (FACE_FRONT and TUNNEL_AA off); the opt-in steps are checked
        # against it at the end (opt_in_selftest; the tunnel step against the 0.9.68 package = FACE_FRONT on), and the pin covers whichever
        # the switches ship
        global FACE_FRONT, TUNNEL_AA, LASER_POWER, USA_SD
        ff_on, taa_on = FACE_FRONT, TUNNEL_AA
        lp_on, sd_on = LASER_POWER, USA_SD
        LASER_POWER = USA_SD = False   # restored at the end of selftest: every check down to them is about the package from before the steps
        try:
            FACE_FRONT, TUNNEL_AA = True, False
            ff_out, _ = generate(data)
            assert ff_out == generate(data)[0], "face-front step not deterministic"
            ff_one = generate(data, focal=['SkirmishGLAStealthGeneral'])[0]
            TUNNEL_AA = True
            taa_out, _ = generate(data)
            assert taa_out == generate(data)[0], "tunnel AA step not deterministic"
            taa_one = generate(data, taa_focal=['SkirmishGLADemolitionGeneral'], taa_watch=True)[0]	# explicit
            taa_t0 = generate(data, taa_focal=['SkirmishGLADemolitionGeneral'], taa_watch=False)[0]
            FACE_FRONT, TUNNEL_AA = ff_on, taa_on
            shipped, _ = generate(data)
            FACE_FRONT, TUNNEL_AA = False, False
            out, counts = generate(data)
        finally:
            FACE_FRONT, TUNNEL_AA = ff_on, taa_on
        assert sha256(out) == FACE_FRONT_OFF_SHA256, "face-front off must be the 0.9.67 package: " + sha256(out)
        assert sha256(ff_out) == TUNNEL_AA_OFF_SHA256, "tunnel AA off (face-front on) must be the 0.9.68 package: " + sha256(ff_out)
        FACE_FRONT, TUNNEL_AA = False, False   # restored at the end of selftest (the legacy checks below regenerate without the steps)
        out2, _ = generate(data)
        assert out == out2, "not deterministic"
        # idempotence: re-applying the spend patch to its own output changes nothing
        scb = SCB(out)
        assert spend_patch(scb) == (0, 0, 0), "spend patch is not idempotent"
        assert scb.write() == out
        assert counts[0] > 0 and counts[1] > 0 and counts[3] > 0, counts
        if BASE_SHA256 != "TBD":
            # T2: the recipe's own output is the TUNNEL_AA package (shipped); ff_out is the 0.9.68 package (TUNNEL_AA off) - t2-fix merge.
            # shipped is built with LASER_POWER off (held off above), so with Laser on it is LASER_POWER_OFF_SHA256; the Laser block at the end pins BASE
            assert sha256(shipped) == (LASER_POWER_OFF_SHA256 if lp_on else BASE_SHA256), "output pin: %s" % sha256(shipped)
        package_selftest(data, out)
        # the step proofs below pin the recipe from before the China package (752d52a6 and older): they run with its switches off.
        # The shipped bytes are covered by composition: package_selftest proved PKG == this output outside China Tank's and China
        # Nuke's lists + teams, and those == M-SP's (+ the 16 genome rows); M-SP runs the same FIXUP_STEPS. A step-off arm cannot
        # carry the genome (every step below touches China Nuke's list, and nuke_genome refuses any other bytes).
        PRE = dict(no_spend=(), genome=False)
        full, out = out, generate(data, **PRE)[0]
        assert sha256(out) == PACKAGE_ARMS['S'][2], "package off: %s" % sha256(out)
        if LAUNCH_FIXES:
            # the launch-fix step touches exactly its own scripts: 1 condition changed, 15 copies added, nothing else
            global FIXUP_STEPS
            keep, FIXUP_STEPS = FIXUP_STEPS, [f for f in FIXUP_STEPS if f is not launch_fixes]
            try:
                without, _ = generate(data, **PRE)
            finally:
                FIXUP_STEPS = keep
            def index(b):
                d, x = {}, SCB(b)
                for li, grp, nodes, i, sn in walk_scripts(x):
                    k, n = (li, script_name(sn)), 0
                    while k + (n,) in d:
                        n += 1
                    d[k + (n,)] = x._w([sn])
                return d, [(k.name, x._w([k])) for k in x.root if k.name != 'PlayerScriptsList']
            (a, ra), (b, rb) = index(without), index(out)
            changed = sorted(k[1] for k in a if k in b and a[k] != b[k])
            added = sorted(k[1] for k in b if k not in a)
            assert ra == rb, "the launch-fix step changed a chunk outside the script lists"
            assert not [k for k in a if k not in b], "the launch-fix step removed a script"
            assert changed == ['China Attack Sequence Back'], changed
            want = sorted(['China Behavior: Dragon Tank Attack'] * 3 + ['GLA Behavior: Immediate Attack Center '] * 3 +
                          ['GLA Behavior: Bomb Truck %s' % w for w in ('Center', 'Flank', 'Back')] * 3)
            assert added == want, added
            print("  launch fixes: 1 script changed, %d added, nothing else (without them: %s)" % (len(added), sha256(without)))
        # expand_nearby, checked whether or not it is switched on: it changes exactly the 12 Hard expansion scripts
        # (FIXUP_STEPS is declared global above; one declaration covers the whole function)
        keep = FIXUP_STEPS
        try:
            FIXUP_STEPS = [f for f in keep if f is not expand_nearby]
            off, _ = generate(data, **PRE)
            FIXUP_STEPS = [expand_nearby] + FIXUP_STEPS   # before expand_no_defence, which removes 12 of the 36 leaves it edits
            on, _ = generate(data, **PRE)
        finally:
            FIXUP_STEPS = keep
        # Walk both trees node by node (names, versions, prefixes = group/script headers, order, every
        # leaf): the only bytes allowed to differ are the 36 SUPPLY_SOURCE_SAFE / AI_PLAYER_BUILD_SUPPLY_CENTER leaves.
        xa, xb, changed = SCB(off), SCB(on), []
        def cmp(na, nb, script):
            assert len(na) == len(nb), "expand_nearby added or removed a node"
            for p, q in zip(na, nb):
                assert (p.name, p.ver, p.prefix) == (q.name, q.ver, q.prefix), "expand_nearby changed a header or the order"
                s = script_name(p) if p.name == 'Script' else script
                if p.kids is not None:
                    cmp(p.kids, q.kids, s)
                elif p.raw != q.raw:
                    assert p.name in ('Condition', 'ScriptAction') and call_name(xa, p.raw)[0] in (
                        'SUPPLY_SOURCE_SAFE', 'AI_PLAYER_BUILD_SUPPLY_CENTER'), "expand_nearby changed %s in %r" % (p.name, s)
                    changed.append(s)
        assert xa.toc == xb.toc, "expand_nearby changed the chunk table"
        cmp(xa.root, xb.root, None)
        per = {s: changed.count(s) for s in set(changed)}   # 4 lists per side x (1 condition + 2 actions)
        assert per == {'%s Base Expand Start - H' % s: (8 if EXPAND_NO_DEFENCE else 12) for s in ('USA', 'China', 'GLA')}, per   # expand_no_defence removes 4 leaves per side
        assert (sha256(out) == sha256(on)) == EXPAND_NEARBY, "EXPAND_NEARBY does not match the generated output"
        print("  expand_nearby: 12 Hard expansion scripts changed, nothing else (on: %s)" % sha256(on))
        # capture_home, checked whether or not it is switched on: with it, exactly 13 TEAM_MOVE_TOWARDS_NEAREST_OBJECT_TYPE
        # actions are gone from the 13 capture-order scripts; every header, every other leaf and the chunk table are identical
        keep = FIXUP_STEPS
        try:
            FIXUP_STEPS = [f for f in keep if f is not capture_home]
            off, _ = generate(data, **PRE)
            FIXUP_STEPS = FIXUP_STEPS + [capture_home]
            on, _ = generate(data, **PRE)
        finally:
            FIXUP_STEPS = keep
        xa, xb, gone = SCB(off), SCB(on), []
        def cmp2(na, nb, script):
            nb = list(nb)
            for p in na:
                s = script_name(p) if p.name == 'Script' else script
                if (p.kids is None and p.name == 'ScriptAction' and re.match(CAPTURE_RX, s or '')
                        and call_name(xa, p.raw)[0] == 'TEAM_MOVE_TOWARDS_NEAREST_OBJECT_TYPE'):
                    gone.append(s)
                    continue
                q = nb.pop(0)
                assert (p.name, p.ver, p.prefix) == (q.name, q.ver, q.prefix), "capture_home changed a header or the order"
                if p.kids is not None:
                    cmp2(p.kids, q.kids, s)
                else:
                    assert p.raw == q.raw, "capture_home changed %s in %r" % (p.name, s)
            assert not nb, "capture_home added a node"
        assert xa.toc == xb.toc, "capture_home changed the chunk table"
        cmp2(xa.root, xb.root, None)
        assert len(gone) == 13 and set(gone) == {'%s Tech Building Capture Orders' % s for s in ('USA', 'China', 'GLA')} | \
            {'GLA Tech Building Capture Orders C'}, gone
        assert (sha256(out) == sha256(on)) == CAPTURE_HOME, "CAPTURE_HOME does not match the generated output"
        print("  capture_home: 13 CombatZone walks removed from the capture orders, nothing else (on: %s, off: %s)" % (sha256(on), sha256(off)))
        # expand_no_defence: without it the output is the previous pin; with it exactly 12 actions are gone from the 12 Hard expansion scripts
        keep = FIXUP_STEPS
        try:
            FIXUP_STEPS = [f for f in keep if f is not expand_no_defence and f is not sw_fire_ready]   # sw_fire_ready came later
            off, _ = generate(data, **PRE)
            FIXUP_STEPS = [f for f in keep if f is not sw_fire_ready]
            pre_sw, _ = generate(data, **PRE)
        finally:
            FIXUP_STEPS = keep
        assert (sha256(off) == "67eb766957cfe6f8949bd84c1801bace5816c7ef1acb9b2bc1d76ad7412f1074") == True, "step off must equal the 0.9.54 recipe: " + sha256(off)
        assert expand_no_defence(SCB(off)) == 12 and (sha256(pre_sw) == sha256(off)) == (not EXPAND_NO_DEFENCE)
        print("  expand_no_defence: 12 defence calls removed from the Hard expansion scripts (off = 67eb7669, on: %s)" % sha256(pre_sw))
        # sw_fire_ready: without it the output is the 0.9.60-0.9.63 pin; with it exactly 22 wave-flag conditions are gone from the 22
        # Hard superweapon scripts, and every header, every other leaf and the chunk table are identical
        assert (sha256(pre_sw) == "cf4a76003ab3e11e47992c36d84712b09725e1f1d22308e0420c50e26fca8f8c") == EXPAND_NO_DEFENCE, "step off must equal the 0.9.60 recipe: " + sha256(pre_sw)
        try:
            FIXUP_STEPS = [f for f in keep if f is not sw_fire_ready] + [sw_fire_ready]
            on, _ = generate(data, **PRE)
        finally:
            FIXUP_STEPS = keep
        xa, xb, flag_gone = SCB(pre_sw), SCB(on), []
        def cmp3(na, nb, script):
            nb = list(nb)
            for p in na:
                s = script_name(p) if p.name == 'Script' else script
                if p.kids is None and p.name == 'Condition' and s in SW_SCRIPTS and is_wave_flag(xa, p):
                    flag_gone.append(s)
                    continue
                assert nb, "sw_fire_ready removed a node from %r" % s
                q = nb.pop(0)
                assert (p.name, p.ver, p.prefix) == (q.name, q.ver, q.prefix), "sw_fire_ready changed a header or the order"
                if p.kids is not None:
                    cmp3(p.kids, q.kids, s)
                else:
                    assert p.raw == q.raw, "sw_fire_ready changed %s in %r" % (p.name, s)
            assert not nb, "sw_fire_ready added a node"
        assert xa.toc == xb.toc, "sw_fire_ready changed the chunk table"
        cmp3(xa.root, xb.root, None)
        assert {n: flag_gone.count(n) for n in set(flag_gone)} == SW_SCRIPTS, flag_gone
        assert sw_fire_ready(SCB(pre_sw)) == 22 and (sha256(out) == sha256(on)) == SW_FIRE_READY and (sha256(out) == sha256(pre_sw)) == (not SW_FIRE_READY)
        print("  sw_fire_ready: 22 wave-flag conditions removed from the Hard superweapon scripts, nothing else (off = cf4a7600, on: %s)" % sha256(on))
        print("  counts easy=%d added=%d tech=%d waves=%d out=%s" % (counts + (sha256(full),)))
        FACE_FRONT, TUNNEL_AA = ff_on, taa_on
        opt_in_selftest(full, ff_out, 'GX Face Front', 'GX Face Front', 'AI_PLAYER_FACE_FRONT')
        opt_in_selftest(full, ff_one, 'GX Face Front', 'GX Face Front', 'AI_PLAYER_FACE_FRONT', ['SkirmishGLAStealthGeneral'])
        # the tunnel step = resize the stock tunnel guards (checked alone) + additions only (gx_arm_selftest), both against the 0.9.68
        # package (face-front on all twelve)
        for taa_b, taa_owners, taa_label in ((taa_out, [o for o, _p in TUNNEL_SIDES], 'tunnel_aa'),
                                              (taa_one, ['SkirmishGLADemolitionGeneral'], 'tunnel_aa (focal Demo)')):
            rs = SCB(ff_out)
            tunnel_resize_guards(rs, taa_owners)
            print("  %s: %d stock tunnel-guard teams -> 1 Quad + 1 Tunnel Defender, nothing else" % (
                taa_label, tunnel_resize_selftest(ff_out, rs.write(), taa_owners)))
            gx_arm_selftest(rs.write(), taa_b, taa_owners, taa_label)
        # T0 = T with the watch's radius negated, nothing else: the same length, one 4-byte INT differs, 700 -> -700
        assert len(taa_t0) == len(taa_one), 'T0 changed the length'
        dif = [i for i in range(len(taa_one)) if taa_one[i] != taa_t0[i]]
        assert dif and dif[-1] - dif[0] < 4, 'T0 differs from T beyond one INT: %r' % dif[:8]
        at = dif[0]
        while at > dif[-1] - 3 and struct.unpack_from('<i', taa_one, at)[0] != TUNNEL_AA_RADIUS:
            at -= 1
        assert struct.unpack_from('<i', taa_one, at)[0] == TUNNEL_AA_RADIUS and struct.unpack_from('<i', taa_t0, at)[0] == -TUNNEL_AA_RADIUS, \
            'T0: the differing bytes are not the radius 700 -> -700'
        print("  tunnel_aa T0 (focal Demo): the air watch radius %d -> %d, nothing else (on: %s)" % (TUNNEL_AA_RADIUS, -TUNNEL_AA_RADIUS, sha256(taa_t0)))
        # the Laser step, the last one: against the shipped package without it (TUNNEL_AA as shipped)
        LASER_POWER = lp_on
        if lp_on:
            lp_out = generate(data)[0]
            assert lp_out == generate(data)[0], "laser_power step not deterministic"
            assert sha256(lp_out) == USA_SD_OFF_SHA256, "laser output pin: %s" % sha256(lp_out)
            laser_power_selftest(shipped, lp_out)   # T2C on 6afae411ea: Laser's base is the shipped package without it (TUNNEL_AA on), not ff_out
        # the USA plan step, the last one: against the package without it
        sd_base = generate(data)[0]
        USA_SD = sd_on
        if sd_on:
            sd_out = generate(data)[0]
            assert sd_out == generate(data)[0], "usa_sd step not deterministic"
            assert sha256(sd_out) == BASE_SHA256, "output pin: %s" % sha256(sd_out)
            usa_sd_selftest(sd_base, sd_out)
            sd_inert = generate(data, sd_button=USA_SD_HOLD)[0]
            assert sd_inert != sd_out and len(sd_out) - len(sd_inert) == 4 * (len(USA_SD_BUTTON) - len(USA_SD_HOLD)), 'usa_sd inert arm: more than the button differs'
            print("  usa_sd inert arm (Hard copy keeps Hold the Line): %s" % sha256(sd_inert))
    print("selftest OK")


def rulebank_main(a):
    """--ruleset / --promote-default: the rule bank's compiler, loaded by path (scripts/tooling/rulebank)"""
    import importlib.util, json
    spec = importlib.util.spec_from_file_location("gx_rulebank_compiler", os.path.join(HERE, "rulebank", "compiler.py"))
    C = importlib.util.module_from_spec(spec); spec.loader.exec_module(C)
    me = sys.modules[__name__]
    try:
        if a.promote_default:
            if not (a.ruleset and a.scb):
                raise C.Refused("--promote-default needs --ruleset and --scb")
            C.promote_default(json.load(open(a.promote_default)), open(a.ruleset, 'rb').read(), open(a.scb, 'rb').read(),
                              os.path.join(HERE, "rulebank", "shipped_ruleset.json"), mss=me, base=C.base_bytes(me, a.search),
                              rule_ids=json.load(open(a.rule_ids)) if a.rule_ids else C.load_rule_ids())
            print("  promoted   %s -> %s: --out / --root / the packagers now write it; set OUTPUT_SHA256 to %s (BASE_SHA256 stays)"
                  % (a.ruleset, os.path.join(HERE, "rulebank", "shipped_ruleset.json"), json.load(open(a.promote_default))['output']['sha256']))
            return
        if not a.out:
            raise C.Refused("--ruleset needs --out")
        rule_ids = json.load(open(a.rule_ids)) if a.rule_ids else C.load_rule_ids()
        data, man = C.compile_ruleset(me, C.base_bytes(me, a.search), open(a.ruleset, 'rb').read(), tier_gate=a.tier_gate, rule_ids=rule_ids)
    except C.Refused as e:
        raise SystemExit("REFUSED: %s" % e)
    write(a.out, data)
    mpath = a.manifest or a.out + ".manifest.json"
    tmp = mpath + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(man, f, indent=1, sort_keys=True); f.write("\n")
    os.replace(tmp, mpath)
    print("  manifest   %s (tier_gate=%s, %d entries, %d marks, scb %s bytes=%d crc=%s)" % (
        mpath, man['tier_gate'], len(man['entries']), len(man['marks']), man['output']['sha256'][:12], man['output']['bytes'],
        man['output']['fnv1a32']))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out")
    ap.add_argument("--search", action="append", default=[])
    ap.add_argument("--game-dir", action="append", default=[], help="a retail game folder holding Data/Scripts (same meaning as build-overlay-generated.py)")
    ap.add_argument("--root", action="append", default=[])
    ap.add_argument("--if-source", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--list", action="store_true", help="print every generated path below the game folder")
    ap.add_argument("--output-sha256", action="store_true", help="print the pinned sha256 of the generated file (a verifier with no retail copy checks against it)")
    ap.add_argument("--ruleset", help="compile a rule-bank ruleset (gx-ruleset/1) on top of the recipe's output into --out")
    ap.add_argument("--tier-gate", choices=("on", "off"), help="tier marks (default: the ruleset's, else on); off = edit in place, proxy runs only")
    ap.add_argument("--rule-ids", help="the target binary's rule_ids.json (default: scripts/ai/rulebank/rule_ids.json when present)")
    ap.add_argument("--manifest", help="where the compile manifest goes (default <out>.manifest.json)")
    ap.add_argument("--promote-default", metavar="MANIFEST", help="make --ruleset the shipped default (needs an on manifest of --scb)")
    ap.add_argument("--scb", help="with --promote-default: the scb that manifest describes")
    a = ap.parse_args()
    a.search = a.search + a.game_dir
    if a.list:
        print("\n".join(OUTPUTS)); return
    if a.output_sha256:
        print(OUTPUT_SHA256); return
    if a.selftest:
        selftest(); return
    if a.ruleset or a.promote_default:
        return rulebank_main(a)
    if a.out:
        if already_done(a.out):
            print("  unchanged  %s" % a.out); return
        write(a.out, build_bytes(a.search)); return
    if a.root:
        for r in a.root:
            if a.if_source and find_source(a.search + [r]) is None:
                print("WARNING: no retail %s for %s - skipped" % (RETAIL_MEMBER, r)); continue
            t = os.path.join(r, *RETAIL_MEMBER.split('/'))
            if already_done(t):
                print("  unchanged  %s" % t); continue
            write(t, build_bytes(a.search + [r]), keep_source=True)
        return
    ap.error("--out, --root, --list or --selftest")


if __name__ == "__main__":
    main()
