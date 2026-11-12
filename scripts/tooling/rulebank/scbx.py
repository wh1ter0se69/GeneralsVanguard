# Copyright 2026 GeneralsX contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# rule bank, DATA half: scb helpers for the ruleset compiler and its modules.
# Works on the recipe's own parse (make-skirmish-scripts.py: SCB / Node / parse_call / enc_call), which the compiler binds in as `mss`
# before any module runs. A module sees this file as `rb`.
#
# Conventions every builder here keeps (each one a measured trap):
#  * a new script's six header bytes are set explicitly (active, oneshot, easy, normal, hard, subroutine): a deep-copied stock header
#    once carried "inactive" into a new script that never ran (memory scb-script-acts-needs-rng-identical-control);
#  * a new leaf's parameter types must equal those of a stock leaf of the same name (a mis-typed call loads and silently does nothing);
#    a GX leaf (GX_LEAVES: no stock file can hold one) takes its types from the engine's own template in ScriptEngine.cpp instead;
#  * PLAYER_HAS_CREDITS is `param OP money` (reversed): use credits_at_least(), never a raw COMPARISON;
#  * names are compared byte for byte (stock names carry trailing spaces).
import os, re, struct, types

mss = None                    # bound by the compiler: the recipe module
SIDE_ME = '<This Player>'
SIDE_ENEMY = "<This Player's Enemy>"
# rb-data-12: the GX leaves a DATA rule may emit, each with the rule_ids.json capability that builds it (gen-rule-ids.py CAP_SOURCES)
GX_LEAVES = {('Condition', 'AI_PLAYER_GX_SENSE'): 'SENSE.ENEMY_FACTION',
             ('Condition', 'AI_PLAYER_GX_SEEN_COUNT'): 'SENSE.FOG_CONDITIONS', ('Condition', 'AI_PLAYER_GX_SEEN_VALUE'): 'SENSE.FOG_CONDITIONS'}
# SENSE.FOG_CONDITIONS: every ask walks the partition (~9 us), so a script reading one runs on at least this
# script delay (seconds); a seen value only of MY perimeter; an OBJECT_TYPE list a rule builds carries the prefix and is built by a seen_list_script
SEEN_MIN_DELAY = 2
SEEN_AREAS = ('[Skirmish]MyInnerPerimeter', '[Skirmish]MyOuterPerimeter')
SEEN_LIST = 'GX Seen '
SEEN_LIST_SCRIPT = 'GX Seen List '
# the lists stock defines on its first pass ("USA Define Object Lists" etc.; selftest pins this against the file): seen_count reads them, nothing writes them
STOCK_OBJECT_LISTS = ('Tanks', 'Aircraft', 'Base Defenses', 'Infantry', 'Artillery', 'Super Weapons', 'Tech Buildings', 'Spy', 'Base Factories',
                      'Base Buildings', 'Anti-Air Vehicle', 'Vehicle', 'Utility', 'Tech Center', 'Supply Lines', 'Supply Lines - Buildings', 'Power')
# the strings AI_PLAYER_GX_SENSE answers for: a BASE side (from the lobby or from 3 different units
# seen) or a lobby side (only when the lobby showed it - a Random enemy is never known by its general). PlayerTemplate.ini Side/BaseSide.
SENSE_BASE = ('USA', 'China', 'GLA')
SENSE_FACTIONS = SENSE_BASE + ('America', 'AmericaSuperWeaponGeneral', 'AmericaLaserGeneral', 'AmericaAirForceGeneral', 'ChinaTankGeneral',
                              'ChinaInfantryGeneral', 'ChinaNukeGeneral', 'GLAToxinGeneral', 'GLADemolitionGeneral', 'GLAStealthGeneral')
LT, LE, EQ, GE, GT, NE = 0, 1, 2, 3, 4, 5   # Scripts.h Parameter::COMPARISON (LESS_THAN .. NOT_EQUAL)
GENERALS = ('SkirmishAmerica', 'SkirmishChina', 'SkirmishGLA', 'SkirmishAmericaAirForceGeneral', 'SkirmishAmericaLaserGeneral',
            'SkirmishAmericaSuperWeaponGeneral', 'SkirmishChinaTankGeneral', 'SkirmishChinaNukeGeneral', 'SkirmishChinaInfantryGeneral',
            'SkirmishGLADemolitionGeneral', 'SkirmishGLAToxinGeneral', 'SkirmishGLAStealthGeneral')
_cache = {}


def lists(scb):
    return [k for k in scb.root if k.name == 'PlayerScriptsList'][0].kids


def owner_list(scb, owner):
    """the ScriptList node of a general (index in the recipe's LAUNCH_FIX_OWNERS)"""
    assert owner in GENERALS, 'not a general: %r' % (owner,)
    return lists(scb)[mss.LAUNCH_FIX_OWNERS.index(owner)]


def node_name(n):
    return mss.astr(n.prefix, 0)[0] if n.name in ('Script', 'ScriptGroup') else None


def scripts_of(scb, owner):
    """[(group node or None, parent kids list, script node)] in file order"""
    out = []
    for k in owner_list(scb, owner).kids:
        if k.name == 'Script':
            out.append((None, owner_list(scb, owner).kids, k))
        elif k.name == 'ScriptGroup':
            out += [(k, k.kids, s) for s in k.kids if s.name == 'Script']
    return out


def find_script(scb, owner, name):
    hits = [(g, kids, s) for g, kids, s in scripts_of(scb, owner) if node_name(s) == name]
    assert len(hits) == 1, '%s: %d scripts named %r' % (owner, len(hits), name)
    return hits[0]


def enums():
    if 'c' not in _cache:
        _cache['c'], _cache['a'] = mss.engine_enum('c'), mss.engine_enum('a')
    return _cache['c'], _cache['a']


def key_of(scb, name):
    """the call key of a condition/action name; a name the file lacks is appended to its table (as opt_in_step does)"""
    if name not in scb.byname:
        nid = max(i for _n, i in scb.toc) + 1
        scb.toc.append((name, nid)); scb.byname[name] = nid; scb.byid[nid] = name
    return (scb.byname[name] << 8) | 3     # the low byte every stock call key carries (18,924 of 18,924)


def stock_types(scb, kind, name):
    """the parameter type list of the first stock leaf `name` (kind 'Condition' or 'ScriptAction'), or None"""
    k = (id(scb), kind, name)
    if k not in _cache:
        _cache[k] = None
        for _l, _g, _n, _i, sn in mss.walk_scripts(scb):
            for o in sn.kids:
                for c in (o.kids if o.name == 'OrCondition' else [o]):
                    if c.kids is None and c.name == kind and scb.byid.get(mss.parse_call(scb, c.raw)[1] >> 8) == name:
                        _cache[k] = [mss.PTYPES[p[0]] for p in mss.parse_call(scb, c.raw)[2]]
                        return _cache[k]
    return _cache[k]


def engine_types(kind, name):
    """the parameter type list of the engine's template `name` (kind 'Condition' or 'ScriptAction'), read from ScriptEngine.cpp next to
    the recipe, or None when the engine has no such template"""
    k = ('engine', kind, name)
    if k not in _cache:
        if 'src' not in _cache:
            path = os.path.join(mss.HERE, '..', '..', 'GeneralsMD', 'Code', 'GameEngine', 'Source', 'GameLogic', 'ScriptEngine', 'ScriptEngine.cpp')
            _cache['src'] = open(path, encoding='latin1').read()
        arr = 'm_conditionTemplates[Condition::' if kind == 'Condition' else 'm_actionTemplates[ScriptAction::'
        at = _cache['src'].find('curTemplate = &%s%s];' % (arr, name))
        if at < 0:
            _cache[k] = None
        else:
            body = _cache['src'][at:].split('\n', 1)[1]
            nxt = body.find('curTemplate = &')
            body = body if nxt < 0 else body[:nxt]
            n = int(re.search(r'curTemplate->m_numParameters\s*=\s*(\d+)\s*;', body).group(1))
            got = dict((int(i), t) for i, t in re.findall(r'curTemplate->m_parameters\[(\d+)\]\s*=\s*Parameter::(\w+)\s*;', body))
            assert sorted(got) == list(range(n)), '%s %s: the engine template sets parameters %r of %d' % (kind, name, sorted(got), n)
            _cache[k] = [got[i] for i in range(n)]
    return _cache[k]


def leaf_types(scb, kind, name):
    """the parameter types a new leaf `name` must carry: a stock leaf's, or for a GX leaf (GX_LEAVES) the engine template's; None if neither"""
    want = stock_types(scb, kind, name)
    if want is None and (kind, name) in GX_LEAVES:
        want = engine_types(kind, name)
    return want


def _leaf(scb, kind, name, params, ver):
    types = [t for t, _v in params]
    want = leaf_types(scb, kind, name)
    assert want is not None, '%s %s: no stock leaf to check its parameter types against' % (kind, name)
    assert types == want, '%s %s: parameter types %r, the stock leaf (or GX template) has %r' % (kind, name, types, want)
    c, a = enums()
    typ = (c if kind == 'Condition' else a)[name]
    ps = []
    for t, v in params:
        ti = mss.PTYPES.index(t)
        if t == 'REAL':
            ps.append((ti, 0, float(v), ''))
        elif isinstance(v, str):
            ps.append((ti, 0, 0.0, v))
        else:
            assert isinstance(v, int) and not isinstance(v, bool), '%s %s: %s value %r is not an int' % (kind, name, t, v)
            ps.append((ti, v, 0.0, ''))
    return mss.Node(kind, ver, raw=mss.enc_call(typ, key_of(scb, name), ps))


def cond(scb, name, *params):
    """a Condition leaf: cond(scb, 'COUNTER', ('COUNTER', '_GX_CLOCK'), ('COMPARISON', GE), ('INT', 6))"""
    return _leaf(scb, 'Condition', name, list(params), 4)


def sense_enemy(scb, faction):
    """AI_PLAYER_GX_SENSE(<This Player>, <This Player's Enemy>, faction): TRUE once this AI fairly knows its enemy plays `faction` - from
    the lobby, or (base sides only) from 3 different units or buildings of one army seen; a Random enemy is unknown until then"""
    assert faction in SENSE_FACTIONS, 'AI_PLAYER_GX_SENSE: faction %r is not one the sensor answers (%s)' % (faction, ', '.join(SENSE_FACTIONS))
    return cond(scb, 'AI_PLAYER_GX_SENSE', ('SIDE', SIDE_ME), ('SIDE', SIDE_ENEMY), ('FACTION_NAME', faction))


def seen_count(scb, op, n, what):
    """AI_PLAYER_GX_SEEN_COUNT(<This Player>, <This Player's Enemy>, op, n, what): TRUE when as many of my enemy's units and buildings of `what` as
    my OWN units and buildings see NOW compare so with n. `what` = a template (a typo counts 0 for ever: nothing here can check it), a stock
    object-type list ('Aircraft', 'Tanks', ...; STOCK_OBJECT_LISTS) or a list of seen_list_script. "Seen so far" is the rule's own latch: a one-shot setting a
    flag. The script reading it needs delay >= SEEN_MIN_DELAY and is no team's generic hook; a list is never asked anything TRUE at a count of 0 (guards.seen_why)"""
    assert op in (LT, LE, EQ, GE, GT, NE), 'AI_PLAYER_GX_SEEN_COUNT: comparison %r is none of Parameter::COMPARISON' % (op,)
    assert isinstance(n, int) and n >= 0, 'AI_PLAYER_GX_SEEN_COUNT: threshold %r' % (n,)
    return cond(scb, 'AI_PLAYER_GX_SEEN_COUNT', ('SIDE', SIDE_ME), ('SIDE', SIDE_ENEMY), ('COMPARISON', op), ('INT', n), ('OBJECT_TYPE', what))


def seen_value(scb, op, v, area):
    """AI_PLAYER_GX_SEEN_VALUE(<This Player>, <This Player's Enemy>, op, v, area): the list price of my enemy's ARMED units (no dozers or harvesters)
    my own units and buildings see NOW inside MY perimeter (SEEN_AREAS). The script reading it needs delay >= SEEN_MIN_DELAY"""
    assert op in (LT, LE, EQ, GE, GT, NE), 'AI_PLAYER_GX_SEEN_VALUE: comparison %r is none of Parameter::COMPARISON' % (op,)
    assert isinstance(v, int) and v >= 0, 'AI_PLAYER_GX_SEEN_VALUE: threshold %r' % (v,)
    assert area in SEEN_AREAS, 'AI_PLAYER_GX_SEEN_VALUE: area %r, only my own perimeter (%s)' % (area, ', '.join(SEEN_AREAS))
    return cond(scb, 'AI_PLAYER_GX_SEEN_VALUE', ('SIDE', SIDE_ME), ('SIDE', SIDE_ENEMY), ('COMPARISON', op), ('INT', v), ('TRIGGER_AREA', area))


def seen_list_script(scb, name, list_name, templates):
    """the script that builds an OBJECT_TYPE list for seen_count: Hard-only, one-shot, always TRUE. add_script it at the TOP LEVEL of the general's list:
    the engine runs a list's top-level scripts before its groups, and an ask before the list exists would count its NAME as a template.
    Lists are GLOBAL (unqualified): every general building `list_name` must build the same templates (guards.seen_list_why)"""
    assert name.startswith(SEEN_LIST_SCRIPT), 'seen list script %r: the name needs the prefix %r' % (name, SEEN_LIST_SCRIPT)
    assert list_name.startswith(SEEN_LIST), 'seen list %r: the name needs the prefix %r (stock lists are read only)' % (list_name, SEEN_LIST)
    assert templates, 'seen list %r: no template' % list_name
    return new_script(scb, name, [[cond(scb, 'CONDITION_TRUE')]],
                      [act(scb, 'OBJECTLIST_ADDOBJECTTYPE', ('OBJECT_TYPE_LIST', list_name), ('OBJECT_TYPE', t)) for t in templates], oneshot=True)


def flag_is(scb, name, value):
    """FLAG(name) == value: a rule's own flag"""
    return cond(scb, 'FLAG', ('FLAG', name), ('BOOLEAN', int(bool(value))))


def matchup_latch(scb, name, knows, flag, decided, faction):
    """[the latch, then one "<knows> <base side>" script per base side]: one-shot Hard-only scripts that DECIDE ONCE, on the first faction this
    AI fairly knows for its enemy (sense_enemy) - in FFA the script's enemy can change, and RULE_BRIEFS 7 locks the variant to the first.
    The latch (first in the list, so it runs first in a frame) sets `flag` and `decided` when the enemy is `faction` and nothing was decided;
    each base-side script only sets `decided`. A Random enemy decides nothing until it is fairly seen"""
    undecided = lambda: flag_is(scb, decided, False)
    done = lambda: act(scb, 'SET_FLAG', ('FLAG', decided), ('BOOLEAN', 1))
    out = [new_script(scb, name, [[sense_enemy(scb, faction), undecided()]], [act(scb, 'SET_FLAG', ('FLAG', flag), ('BOOLEAN', 1)), done()],
                      oneshot=True)]
    for f in SENSE_BASE:
        out.append(new_script(scb, '%s %s' % (knows, f), [[sense_enemy(scb, f), undecided()]], [done()], oneshot=True))
    return out


# rb-data-13 (the lead's ruling): ONE matchup latch per general, shared by every rule that declares a MATCHUP key. Two rules may claim the
# same stock team only when their keys differ: the latch sets at most one gxrb_mu_<side>, so their variants never run together.
MU_GROUP, MU_OFF, MU_DECIDED = 'GX Matchup', 'GX MU Off ', 'gxrb_mu_decided'


def mu_flag(key):
    """the shared latch's flag for a matchup key (a base side the sensor answers)"""
    assert key in SENSE_BASE, 'matchup key %r: one of %s' % (key, ', '.join(SENSE_BASE))
    return 'gxrb_mu_' + key.lower()


MU_FLAGS = tuple(mu_flag(k) for k in SENSE_BASE)


def matchup_latch_scripts(scb):
    """the canonical latch: per base side F (SENSE_BASE order) a one-shot Hard-only script "GX Matchup F": AI_PLAYER_GX_SENSE says my enemy
    is F and nothing was decided -> gxrb_mu_<F> and gxrb_mu_decided ON. Each script sets its flag only with `decided` and in the same
    run, and the scripts run one after another, so at most one gxrb_mu_ flag is ever set: the first side this AI fairly knows for its
    enemy decides once (RULE_BRIEFS 7; in FFA the enemy can change). A Random enemy decides nothing until seen"""
    return [new_script(scb, '%s %s' % (MU_GROUP, f), [[sense_enemy(scb, f), flag_is(scb, MU_DECIDED, False)]],
                       [act(scb, 'SET_FLAG', ('FLAG', mu_flag(f)), ('BOOLEAN', 1)), act(scb, 'SET_FLAG', ('FLAG', MU_DECIDED), ('BOOLEAN', 1))],
                       oneshot=True) for f in SENSE_BASE]


def matchup(scb, owner):
    """the shared latch group of `owner` (created once, at the end of its list, by the first rule that asks): the latch scripts, then the
    off gates matchup_off() writes"""
    g = [k for k in owner_list(scb, owner).kids if k.name == 'ScriptGroup' and node_name(k) == MU_GROUP]
    if g:
        return g[0]
    grp = new_group(MU_GROUP, matchup_latch_scripts(scb))
    owner_list(scb, owner).kids.append(grp)
    return grp


def matchup_claims(owners, stock_teams, scripts=(), groups=(), teams=()):
    """a MATCHUP rule's CLAIMS: its own, plus the shared latch group and scripts and the off gate of each stock team it switches"""
    sc = [list(x) for x in scripts] + [[o, '%s %s' % (MU_GROUP, f)] for o in owners for f in SENSE_BASE] + \
        [[o, MU_OFF + t] for o in owners for t in stock_teams]
    return {'scripts': sc, 'groups': [list(x) for x in groups] + [[o, MU_GROUP] for o in owners], 'teams': sorted(set(teams) | set(stock_teams))}


def base_side(faction):
    """the base side (SENSE_BASE) of a faction the sensor answers: 'AmericaSuperWeaponGeneral' -> 'USA'"""
    assert faction in SENSE_FACTIONS, faction
    return faction if faction in SENSE_BASE else {'America': 'USA', 'China': 'China', 'GLA': 'GLA'}[re.match(r'America|China|GLA', faction).group(0)]


def general_latch(scb, prefix, key, flags, done):
    """rb-data-15: a GENERAL-level read for a MATCHUP rule, as the rule's own one-shot Hard-only scripts. The sensor names a general only
    when the lobby showed it (a Random enemy is never known by its general), so this decides ONCE, in the first frame the rule's key is
    ON: per general G (flags = {G: the rule's own flag}) "<prefix> Gen <G>": key ON, AI_PLAYER_GX_SENSE says G, `done` OFF -> G's flag and
    `done`; then "<prefix> Gen Done": key ON, `done` OFF -> `done`. Called after rb.matchup(), the scripts come after the shared latch in
    the list, so they run in the frame the key is set and read the enemy the key decided on. Each G's base side must be the key"""
    for g in flags:
        assert base_side(g) == key and g not in SENSE_BASE, '%s: %r is not a %s general' % (prefix, g, key)
    k, und = (lambda: flag_is(scb, mu_flag(key), True)), (lambda: flag_is(scb, done, False))
    on = lambda f: act(scb, 'SET_FLAG', ('FLAG', f), ('BOOLEAN', 1))
    return [new_script(scb, '%s Gen %s' % (prefix, g), [[k(), sense_enemy(scb, g), und()]], [on(flags[g]), on(done)], oneshot=True)
            for g in sorted(flags)] + [new_script(scb, '%s Gen Done' % prefix, [[k(), und()]], [on(done)], oneshot=True)]


def mu_leaf(scb, leaf):
    """(flag, value) of a FLAG condition on a shared latch flag, else None"""
    if leaf.name != 'Condition':
        return None
    nm, ps = decode(scb, leaf)
    return (ps[0][1], ps[1][1]) if nm == 'FLAG' and ps[0] == ('FLAG', ps[0][1]) and ps[0][1] in MU_FLAGS + (MU_DECIDED,) else None


def is_keep(scb, block):
    """an off gate's KEEP block (rb-data-17): an OR block holding a shared latch flag ON (OFF blocks hold them OFF only)"""
    return any(mu_leaf(scb, l) in {(f, 1) for f in MU_FLAGS} for l in block.kids)


def _off_gate(scb, owner, stock_team):
    hits = [k for k in matchup(scb, owner).kids if node_name(k) == MU_OFF + stock_team]
    assert len(hits) == 1, '%s: %d off gates %r' % (owner, len(hits), MU_OFF + stock_team)
    return hits[0]


def _keep_order(scb, s):
    """OFF blocks (as they stand), then the KEEP blocks in canonical order (by their decoded leaves: the stock prefix, the key, the rule's
    own leaves), then the actions - so the order rules apply in does not change the bytes"""
    blk = [k for k in s.kids if k.name == 'OrCondition']
    keeps = sorted((b for b in blk if is_keep(scb, b)), key=lambda b: repr([decode(scb, l) for l in b.kids]))
    s.kids = [b for b in blk if not is_keep(scb, b)] + keeps + [k for k in s.kids if k.name != 'OrCondition']


def matchup_keep(scb, ts, owner, stock_team, key, blocks):
    """rb-data-17 (the lead's stock-copy ruling): keep the STOCK team on, under key `key`, while the rule's own leaves hold - instead of a
    variant with the stock slots on the rule's own clock (rb-data-16 s5: such a copy fields ~17 fewer Scorpions by 600 s than stock).
    After matchup_off(): per stock OR block and per entry of `blocks` (a function returning the leaves: FLAG(gxrb_mu_<key>) == 1 and the
    rule's own state - a flag only it writes, an own building count), the off gate gains the KEEP block [stock block, those leaves].
    The stock team then runs exactly as stock (its gate, hooks, slots) whenever one keep block holds. guards.matchup_final checks the form"""
    t = team(ts, stock_team)
    assert t is not None and team_get(t, 'teamProductionCondition') == MU_OFF + stock_team, \
        '%s: matchup_keep(%r) before matchup_off()' % (owner, stock_team)
    s = _off_gate(scb, owner, stock_team)
    stock = [[l for l in b.kids if mu_leaf(scb, l) is None] for b in s.kids if b.name == 'OrCondition' and not is_keep(scb, b)]
    have = {repr([decode(scb, l) for l in b.kids]) for b in s.kids if b.name == 'OrCondition' and is_keep(scb, b)}
    for f in blocks:
        tail = list(f())
        assert [mu_leaf(scb, l) for l in tail if mu_leaf(scb, l)] == [(mu_flag(key), 1)], \
            '%s %s: a keep block holds its key ON and no other latch flag' % (owner, stock_team)
        for sb in stock:
            b = mss.Node('OrCondition', 1, kids=[mss.deep_copy(l) for l in sb] + tail)
            r = repr([decode(scb, l) for l in b.kids])
            assert r not in have, '%s %s: the keep block %s twice' % (owner, stock_team, r)
            have.add(r)
            s.kids.insert(0, b)
            tail = [mss.deep_copy(l) for l in tail]
    _keep_order(scb, s)
    return s


def matchup_off(scb, ts, owner, stock_team, key):
    """the stock team waits on its OWN off gate "GX MU Off <team>": a copy of its stock gate (flags, delay, actions; the stock script stays
    as it is - other teams share it) whose every OR block gains FLAG(gxrb_mu_<k>) == 0 for every key k a rule switched this team for,
    keys in name order. A second rule composes into the same gate in place, so the order the rules apply in does not change the bytes.
    KEEP blocks (matchup_keep) stay as they are: their key ON excludes every other key already"""
    grp = matchup(scb, owner)
    t = team(ts, stock_team)
    assert t is not None and team_get(t, 'teamOwner') == owner, '%s: no team %r' % (owner, stock_team)
    name, cur = MU_OFF + stock_team, team_get(t, 'teamProductionCondition')
    if cur == name:
        hits = [k for k in grp.kids if node_name(k) == name]
        assert len(hits) == 1, '%s: %d off gates %r' % (owner, len(hits), name)
        s = hits[0]
    else:
        assert cur and not cur.startswith(MU_OFF), '%s: team %r has production condition %r' % (owner, stock_team, cur)
        s = gate_copy(scb, owner, cur, name, lambda: [])
        at = [i for i, k in enumerate(grp.kids) if node_name(k).startswith(MU_OFF) and node_name(k) > name]
        grp.kids.insert(at[0] if at else len(grp.kids), s)
        team_set(scb, t, 'teamProductionCondition', name)
    for b in s.kids:
        if b.name == 'OrCondition' and not is_keep(scb, b):
            keys = sorted({k for k in SENSE_BASE for l in b.kids if mu_leaf(scb, l) == (mu_flag(k), 0)} | {key})
            b.kids = [l for l in b.kids if mu_leaf(scb, l) is None] + [flag_is(scb, mu_flag(k), False) for k in keys]
    return name


def mu_stock_gate(scb, owner, gate, candidates):
    """the stock gate (one of `candidates`) a team waits on: `gate` itself, or - when another matchup rule already switched the team to its
    off gate - the candidate whose OR blocks are the off gate's minus its matchup leaves; None when neither"""
    if gate in candidates:
        return gate
    if not gate or not gate.startswith(MU_OFF):
        return None
    blocks = lambda s, keep: [tuple(canon(scb, l) for l in b.kids if keep(l)) for b in s.kids if b.name == 'OrCondition' and not is_keep(scb, b)]
    want = blocks(find_script(scb, owner, gate)[2], lambda l: mu_leaf(scb, l) is None)
    hits = [c for c in candidates if any(node_name(x) == c for _g, _k, x in scripts_of(scb, owner))
            and blocks(find_script(scb, owner, c)[2], lambda l: True) == want]
    assert len(hits) <= 1, '%s: the off gate %r matches the stock gates %r' % (owner, gate, hits)
    return hits[0] if hits else None


def gate_copy(scb, owner, stock_gate, name, extra):
    """a copy of the stock production-condition script `stock_gate` named `name`: its flags, delay, OR blocks and actions, each OR block
    gaining the leaves extra() returns (called once per block). The stock script itself stays as it is (other teams share it)"""
    _g, _k, s0 = find_script(scb, owner, stock_gate)
    s = mss.deep_copy(s0)
    mss.set_script(s, name=name)
    for b in s.kids:
        if b.name == 'OrCondition':
            b.kids = list(b.kids) + list(extra())
    return s


def wave_window(scb, owner, stock_gate):
    """(lo, hi) in _GX_CLOCK ticks: the own-clock twin of a stock Hard wave gate `stock_gate` (one OR block, one COUNTER(_ATTACK_WAVE, op,
    k) leaf). Stock Hard sets _ATTACK_WAVE = j once _ESCALATION >= t_j (its "<S> H Attack Wave j Set" scripts, read here, never typed in);
    "<S> Time Escalation - H" adds 1 every 10 s, as _GX_CLOCK does. The window drops only the enemy term (stock adds +30 / +50 to
    _ESCALATION for enemy tech it never saw). None = unbounded."""
    _g, _k, s0 = find_script(scb, owner, stock_gate)
    ors = [b for b in s0.kids if b.name == 'OrCondition']
    assert len(ors) == 1 and len(ors[0].kids) == 1, '%s %s: not a one-leaf wave gate' % (owner, stock_gate)
    name, ps = decode(scb, ors[0].kids[0])
    assert name == 'COUNTER' and ps[0] == ('COUNTER', '_ATTACK_WAVE'), '%s %s: not an _ATTACK_WAVE gate: %r' % (owner, stock_gate, (name, ps))
    op, k = ps[1][1], ps[2][1]
    th = {}
    for _g, _k, s in scripts_of(scb, owner):
        fl = mss.script_flags(s)[0]
        if not fl[4] or fl[3] or fl[2]:                 # Hard-only scripts
            continue
        sets = [ps_[1][1] for nm, ps_ in (decode(scb, l) for l in leaves(s) if l.name == 'ScriptAction')
                if nm == 'SET_COUNTER' and ps_[0] == ('COUNTER', '_ATTACK_WAVE')]
        esc = [ps_[2][1] for nm, ps_ in (decode(scb, l) for l in leaves(s) if l.name == 'Condition')
               if nm == 'COUNTER' and ps_[0] == ('COUNTER', '_ESCALATION') and ps_[1][1] == GE]
        for j in sets:
            if j >= 1:
                assert len(esc) == 1 and th.get(j, esc[0]) == esc[0], '%s: wave %d set twice or not by an escalation threshold' % (owner, j)
                th[j] = esc[0]
    assert sorted(th) == list(range(1, len(th) + 1)), '%s: Hard wave thresholds %r' % (owner, th)
    lo = lambda w: th.get(w) if w >= 1 else None
    hi = lambda w: th.get(w + 1)
    if op == EQ:
        return lo(k), hi(k)
    if op == LE:
        return None, hi(k)
    if op == LT:
        return None, hi(k - 1)
    if op == GE:
        return lo(k), None
    if op == GT:
        return lo(k + 1), None
    raise AssertionError('%s %s: comparison %d' % (owner, stock_gate, op))


def wave_gate(scb, owner, stock_gate, name, extra):
    """a new production-condition script for a variant team: the stock wave gate's header (flags, delay) and actions, its one OR block
    replaced by the own-clock window wave_window() gives plus the leaves extra() returns - no stock (enemy-tainted) leaf is read"""
    lo, hi = wave_window(scb, owner, stock_gate)
    _g, _k, s0 = find_script(scb, owner, stock_gate)
    s = mss.deep_copy(s0)
    mss.set_script(s, name=name)
    blk = [b for b in s.kids if b.name == 'OrCondition'][0]
    blk.kids = ([clock_at_least(scb, lo)] if lo else []) + \
        ([cond(scb, 'COUNTER', ('COUNTER', GX_CLOCK), ('COMPARISON', LT), ('INT', hi))] if hi else []) + list(extra())
    assert blk.kids, '%s %s: an unbounded window and nothing else' % (owner, stock_gate)
    return s


TEAM_HOOKS = ('teamOnCreateScript', 'teamOnDestroyedScript', 'teamOnIdleScript', 'teamOnUnitDestroyedScript', 'teamEnemySightedScript',
              'teamAllClearScript', 'teamGenericScriptHook0', 'teamGenericScriptHook1', 'teamGenericScriptHook2', 'teamGenericScriptHook3')


def _launch_hook(scb, name, tick):
    """a one-shot subroutine for a variant team's generic hook: once _GX_CLOCK >= tick (0: at once), TEAM_HUNT - the team runs it once
    (Team::updateGenericScripts drops a one-shot hook after its first run); no stock script is named"""
    c = [clock_at_least(scb, tick)] if tick else [cond(scb, 'CONDITION_TRUE')]
    return new_script(scb, name, [c], [act(scb, 'TEAM_HUNT', ('TEAM', '<This Team>'))], oneshot=True, sub=True)


FORMED = ' Formed'


def launch_scripts(scb, name, tick):
    """rb-data-18: a team that hunts from _GX_CLOCK >= tick (0: as soon as it forms). Player::update runs a team's generic hooks whether
    it has formed or not, and a one-shot hook is spent on its first true evaluation: a generic hook alone, at tick 0, hunts with an EMPTY team and its units
    never get the order (smoke14: USA_VS_GLA's Burton stayed near home for the whole game). So: "<name> Formed" on teamOnCreateScript, which runs when the team
    forms, with the same condition; it must NOT be one-shot (ScriptEngine::executeScript deactivates a one-shot script for every team of that player). And,
    with a tick, the one-shot generic hook "<name>" for a team that formed before it. launch_fields(name, tick) gives the team's fields; guards.team_hooks refuses any
    other shape"""
    formed = new_script(scb, name + FORMED, [[clock_at_least(scb, tick)] if tick else [cond(scb, 'CONDITION_TRUE')]],
                        [act(scb, 'TEAM_HUNT', ('TEAM', '<This Team>'))], sub=True)
    return [_launch_hook(scb, name, tick), formed] if tick else [formed]


def launch_fields(name, tick):
    """the hooks= of a team launched by launch_scripts(name, tick)"""
    return dict({'teamOnCreateScript': name + FORMED}, **({'teamGenericScriptHook0': name} if tick else {}))


# rb-data-17 (provenance by construction): what a new team copies is recorded when it is built, never
# inferred from its name, its place or its fields. {team: its stock donor, or None when declared new}; the compiler resets it per tier;
# guards.team_provenance and the keep-block overlap scan read this table alone
BUILT = {}


def build_reset():
    BUILT.clear()


def add_new_team(ts, at, d):
    """a team that stands in for no stock team, put at list index `at`: declared new in the build table (a module's raid, guard or capture
    team). A team that replaces a stock team goes through variant_team instead"""
    BUILT[d['teamName']] = None
    ts.insert(at, d)


def team_slots(t):
    """a team's three unit slots [[type, min, max], ...]"""
    return [[team_get(t, 'teamUnitType%d' % i), team_get(t, 'teamUnitMinCount%d' % i), team_get(t, 'teamUnitMaxCount%d' % i)] for i in (1, 2, 3)]


def same_slots(a, b):
    """two slot lists name the same units in the same order (empty slots - max 0 or no type - dropped): a variant with `a` built for a
    stock team with `b` would be a STOCK COPY - rb-data-17: keep the stock team on instead (matchup_keep)"""
    full = lambda s: [list(x) for x in s if x[2] > 0 and x[0] not in (None, '<none>')]
    return full(a) == full(b)


def variant_team(scb, ts, stock_team, name, gate, slots, stock_gate=None, hooks=None):
    """a copy of the stock team `stock_team` (owner, priority, instances, idle frames...) named `name`, produced by `gate`, with its three
    unit slots [[type, min, max], ...] and ONLY the script hooks given ({field: script}; every other hook field emptied - a stock hook can
    reach enemy-gated logic, _LAUNCH_ATTACK's attack sequences do); it goes right after the stock team. stock_gate: the stock team's new
    production condition. Records stock_team -> name in BUILT (a copy of a copy: its stock team)"""
    import copy
    t = team(ts, stock_team)
    assert t is not None, 'no team %r' % stock_team
    assert team(ts, name) is None, 'a team %r exists' % name
    assert set(hooks or {}) <= set(TEAM_HOOKS), sorted(set(hooks or {}) - set(TEAM_HOOKS))
    d = copy.deepcopy({'teamName': t['teamName'], '_e': [list(e) for e in t['_e']]})
    team_set(scb, d, 'teamName', name)
    d['teamName'] = name
    team_set(scb, d, 'teamProductionCondition', gate)
    for k in TEAM_HOOKS:
        if team_get(d, k) is not None or k in (hooks or {}):
            team_set(scb, d, k, (hooks or {}).get(k, ''))
    for i, (typ, mn, mx) in enumerate(slots, 1):
        team_set(scb, d, 'teamUnitType%d' % i, typ)
        team_set(scb, d, 'teamUnitMinCount%d' % i, mn)
        team_set(scb, d, 'teamUnitMaxCount%d' % i, mx)
    at = ts.index(t) + 1
    while at < len(ts) and ts[at]['teamName'].startswith('GX ') and ts[at]['teamName'] < name:     # variants of one team in name order
        at += 1                                                                                       # (rb-data-13: rules compose)
    ts.insert(at, d)
    BUILT[name] = BUILT.get(stock_team, stock_team)
    if stock_gate is not None:
        team_set(scb, t, 'teamProductionCondition', stock_gate)
    return d


def act(scb, name, *params):
    return _leaf(scb, 'ScriptAction', name, list(params), 2)


def new_script(scb, name, or_blocks, actions, delay=0, oneshot=False, active=True, sub=False, easy=0, normal=0, hard=1):
    """a Script node: or_blocks = [[cond, ...], ...] (each inner list ANDed, the blocks ORed). Hard-only unless told."""
    pre = mss.enc_astr(name) + mss.enc_astr('') * 3 + bytes((int(active), int(oneshot), easy, normal, hard, int(sub))) + \
        struct.pack('<i', int(delay))
    kids = [mss.Node('OrCondition', 1, kids=list(b)) for b in or_blocks] + list(actions)
    return mss.Node('Script', 2, pre, kids)


def add_script(scb, owner, script, group=None):
    """append a script to a general's list: at its top level, or at the end of the named stock group"""
    if group is None:
        owner_list(scb, owner).kids.append(script)
        return
    gs = [k for k in owner_list(scb, owner).kids if k.name == 'ScriptGroup' and node_name(k) == group]
    assert len(gs) == 1, '%s: %d groups named %r' % (owner, len(gs), group)
    gs[0].kids.append(script)


def leaves(node):
    """every Condition / ScriptAction / ScriptActionFalse leaf under a node, in file order"""
    for k in node.kids or []:
        if k.kids is not None:
            yield from leaves(k)
        elif k.name in ('Condition', 'ScriptAction', 'ScriptActionFalse'):
            yield k


def decode(scb, leaf):
    """(call name, [(param type, value)]) - value = the int for INT/COMPARISON/BOOLEAN..., the string for named types"""
    typ, key, ps = mss.parse_call(scb, leaf.raw)
    out = []
    for p in ps:
        t = mss.PTYPES[p[0]]
        out.append((t, p[1] if t == 'COORD3D' else (p[2] if t == 'REAL' else (p[3] if p[3] or t in NAMED else p[1]))))
    return scb.byid.get(key >> 8, '?'), out


NAMED = {'SCRIPT', 'TEAM', 'COUNTER', 'FLAG', 'WAYPOINT', 'TRIGGER_AREA', 'TEXT_STRING', 'SIDE', 'SOUND', 'SCRIPT_SUBROUTINE', 'UNIT',
         'OBJECT_TYPE', 'TEAM_STATE', 'DIALOG', 'MUSIC', 'MOVIE', 'WAYPOINT_PATH', 'LOCALIZED_TEXT', 'BRIDGE', 'ATTACK_PRIORITY_SET',
         'SPECIAL_POWER', 'SCIENCE', 'UPGRADE', 'COMMANDBUTTON_ABILITY', 'BOUNDARY', 'COMMAND_BUTTON', 'FONT_NAME', 'EMOTICON',
         'FACTION_NAME', 'OBJECT_TYPE_LIST', 'REVEALNAME', 'SKIRMISH_WAYPOINT_PATH', 'COMMANDBUTTON_ALL_ABILITIES', 'OBJECT_PANEL_FLAG',
         'OBJECT_STATUS', 'SCIENCE_AVAILABILITY'}


# --------------------------------------------------------------------------- own-state conditions (the fair vocabulary)
GX_CLOCK = '_GX_CLOCK'          # +1 per 10 s from the start of the game, no enemy term (the compiler adds the clock where it is read)
CLOCK_GROUP = 'GX Clock'
CLOCK_SCRIPT = 'GX Clock'


def credits_at_least(scb, v):
    """money >= v: PLAYER_HAS_CREDITS is `v OP money`, so LESS_EQUAL (memory scb-player-has-credits-is-reversed)"""
    return cond(scb, 'PLAYER_HAS_CREDITS', ('INT', v), ('COMPARISON', LE), ('SIDE', SIDE_ME))


def credits_below(scb, v):
    """money < v  <=>  v > money: GREATER_THAN"""
    return cond(scb, 'PLAYER_HAS_CREDITS', ('INT', v), ('COMPARISON', GT), ('SIDE', SIDE_ME))


def credits_at_most(scb, v):
    """money <= v  <=>  v >= money: GREATER_EQUAL"""
    return cond(scb, 'PLAYER_HAS_CREDITS', ('INT', v), ('COMPARISON', GE), ('SIDE', SIDE_ME))


def clock_at_least(scb, ticks):
    """_GX_CLOCK >= ticks (one tick = 10 s)"""
    return cond(scb, 'COUNTER', ('COUNTER', GX_CLOCK), ('COMPARISON', GE), ('INT', ticks))


def own_count(scb, cmp, n, object_type):
    """own objects of a template or a stock object-type list, COMPARISON the normal way round (count OP n)"""
    return cond(scb, 'PLAYER_HAS_OBJECT_COMPARISON', ('SIDE', SIDE_ME), ('COMPARISON', cmp), ('INT', n), ('OBJECT_TYPE', object_type))


def new_group(name, kids, active=True):
    """a ScriptGroup node (version 2: name, active, subroutine=0)"""
    return mss.Node('ScriptGroup', 2, mss.enc_astr(name) + bytes((int(active), 0)), list(kids))


def ensure_clock(scb, owner):
    """add the GX Clock group (one repeating Hard-only script, every 10 s: INCREMENT_COUNTER(1, _GX_CLOCK)) to a list once; the stock
    'Time Escalation - H' shape on its own counter. Returns True when it added it."""
    lst = owner_list(scb, owner)
    if any(k.name == 'ScriptGroup' and node_name(k) == CLOCK_GROUP for k in lst.kids):
        return False
    s = new_script(scb, CLOCK_SCRIPT, [[cond(scb, 'CONDITION_TRUE')]], [act(scb, 'INCREMENT_COUNTER', ('INT', 1), ('COUNTER', GX_CLOCK))],
                   delay=10, oneshot=False)
    lst.kids.append(new_group(CLOCK_GROUP, [s]))
    return True


# --------------------------------------------------------------------------- teams (ScriptTeams: one Dict per team)
DICT_BOOL, DICT_INT, DICT_REAL, DICT_ASCII, DICT_UNICODE = range(5)


def teams(scb):
    """[{'teamName': ..., '_e': [[key, type, value], ...]}] in file order; unicode values stay raw bytes"""
    raw = next(k for k in scb.root if k.name == 'ScriptTeams').raw
    out, p = [], 0

    def need(n):                         # every field inside the chunk: the engine's reader refuses a short one (DataChunk.cpp:1054;
        if p + n > len(raw):             # a Python slice past the end would decode it as shorter instead)
            raise ValueError('ScriptTeams: a field at byte %d needs %d bytes, %d are left' % (p, n, len(raw) - p))
    while p < len(raw):
        need(2); cnt = struct.unpack_from('<H', raw, p)[0]; p += 2; es = []
        for _ in range(cnt):
            need(4); kt = struct.unpack_from('<i', raw, p)[0]; p += 4; t = kt & 0xFF
            if (kt >> 8) not in scb.byid:
                raise ValueError('ScriptTeams: key id %d is not in the name table' % (kt >> 8))
            key = scb.byid[kt >> 8]
            if t == DICT_BOOL:
                need(1); v = raw[p]; p += 1
            elif t in (DICT_INT, DICT_REAL):
                need(4); v = raw[p:p + 4]; p += 4
                v = struct.unpack('<i', v)[0] if t == DICT_INT else v
            elif t == DICT_ASCII:
                need(2); n = struct.unpack_from('<H', raw, p)[0]; need(2 + n); v = raw[p + 2:p + 2 + n].decode('latin1'); p += 2 + n
            elif t == DICT_UNICODE:
                need(2); n = struct.unpack_from('<H', raw, p)[0]; need(2 + 2 * n); v = raw[p + 2:p + 2 + 2 * n]; p += 2 + 2 * n
            else:
                raise ValueError('ScriptTeams: bad dict type %d' % t)
            es.append([key, t, v])
        d = {'_e': es}
        d['teamName'] = team_get(d, 'teamName')
        out.append(d)
    return out


_TRO = {}


def teams_ro(scb):
    """teams() for READING only, cached on the ScriptTeams bytes (and the name table they decode with). The view is immutable
    (tuples, read-only mappings): a write through it would change every later reader's copy, the guard's "before" included, and so
    hide the edit - write through teams() + put_teams()"""
    raw = next(k for k in scb.root if k.name == 'ScriptTeams').raw
    k = (raw, tuple(scb.toc))            # the table's CONTENTS: two compiles can give different keys one id
    if k not in _TRO:
        if len(_TRO) > 32:
            _TRO.clear()
        _TRO[k] = tuple(types.MappingProxyType({'teamName': d['teamName'], '_e': tuple(tuple(e) for e in d['_e'])}) for d in teams(scb))
    return _TRO[k]


def encode_teams(scb, ts):
    b = bytearray()
    for d in ts:
        b += struct.pack('<H', len(d['_e']))
        for key, t, v in d['_e']:
            if key not in scb.byname:
                nid = max(i for _n, i in scb.toc) + 1
                scb.toc.append((key, nid)); scb.byname[key] = nid; scb.byid[nid] = key
            b += struct.pack('<i', (scb.byname[key] << 8) | t)
            if t == DICT_BOOL:
                b.append(v)
            elif t == DICT_INT:
                b += struct.pack('<i', v)
            elif t == DICT_REAL:
                b += v
            elif t == DICT_ASCII:
                b += mss.enc_astr(v)
            else:
                b += struct.pack('<H', len(v) // 2) + v
    return bytes(b)


def put_teams(scb, ts):
    next(k for k in scb.root if k.name == 'ScriptTeams').raw = encode_teams(scb, ts)


def team(ts, name):
    hits = [d for d in ts if d['teamName'] == name]
    assert len(hits) <= 1, '%d teams named %r' % (len(hits), name)
    return hits[0] if hits else None


def team_get(d, key):
    for k, _t, v in d['_e']:
        if k == key:
            return v
    return None


def team_set(scb, d, key, value):
    """set a field; a new key is appended with the type of its value (int -> DICT_INT, str -> DICT_ASCII, bool -> DICT_BOOL)"""
    for e in d['_e']:
        if e[0] == key:
            want = {DICT_BOOL: bool, DICT_INT: int, DICT_ASCII: str}.get(e[1])
            assert want is not None and type(value) is want, 'team %r: %s is type %d, got %r' % (d['teamName'], key, e[1], value)
            e[2] = int(value) if want is bool else value
            break
    else:
        t = DICT_BOOL if type(value) is bool else DICT_INT if type(value) is int else DICT_ASCII if type(value) is str else None
        assert t is not None, 'team %r: %s=%r has no dict type' % (d['teamName'], key, value)
        d['_e'].append([key, t, int(value) if t == DICT_BOOL else value])
    if key == 'teamName':
        d['teamName'] = value


# --------------------------------------------------------------------------- canonical per-side view (toc-independent)
def canon(scb, n):
    if n.kids is not None:
        return (n.name, n.ver, n.prefix, tuple(canon(scb, k) for k in n.kids))
    if n.name in ('Condition', 'ScriptAction', 'ScriptActionFalse'):
        try:
            cnt = struct.unpack_from('<i', n.raw, 8)[0]
            typ, key, ps = mss.parse_call(scb, n.raw)
        except Exception as x:
            raise ValueError('a malformed %s leaf: %s' % (n.name, x))
        # the engine's reader, not Python's: a negative count reads no parameters here but turns the leaf FALSE there
        # (Scripts.cpp:1724), a negative tag indexes PTYPES from the end here but is no type there
        if cnt < 0 or any(not 0 <= p[0] < len(mss.PTYPES) for p in ps):
            raise ValueError('a malformed %s leaf: parameter count %d, tags %s (the engine reads a count >= 0 and tags 0..%d)'
                             % (n.name, cnt, [p[0] for p in ps], len(mss.PTYPES) - 1))
        # floats as their bytes: -0.0 == 0.0 in Python, not in the file
        ps = tuple((p[0], struct.pack('<3f', *p[1])) if len(p) == 2 else (p[0], p[1], struct.pack('<f', p[2]), p[3]) for p in ps)
        return (n.name, n.ver, typ, scb.byid.get(key >> 8), key & 0xFF, ps)
    return (n.name, n.ver, n.raw)


def side_dump(scb, owner):
    """what a player of this general reads: its ScriptList + the teams it owns, with every name key resolved to its name"""
    tl = tuple((d['teamName'], tuple((k, t, v) for k, t, v in d['_e'])) for d in teams_ro(scb) if team_get(d, 'teamOwner') == owner)
    return canon(scb, owner_list(scb, owner)), tl


def canon_team(d):
    return tuple((k, t, v) for k, t, v in d['_e'])


def uses_clock(scb, owner):
    return any(t == 'COUNTER' and v == GX_CLOCK for _g, _k, s in scripts_of(scb, owner) for l in leaves(s) for t, v in decode(scb, l)[1])


def call_leaf(scb, kind, name, typ, params):
    """a leaf with an explicit enum int and parameter types (the bank's GX actions have no stock leaf to copy types from)"""
    ps = [(mss.PTYPES.index(t), 0, float(v), '') if t == 'REAL' else (mss.PTYPES.index(t), 0, 0.0, v) if isinstance(v, str)
          else (mss.PTYPES.index(t), int(v), 0.0, '') for t, v in params]
    return mss.Node(kind, 4 if kind == 'Condition' else 2, raw=mss.enc_call(typ, key_of(scb, name), ps))
