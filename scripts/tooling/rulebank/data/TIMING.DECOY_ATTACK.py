# GeneralsX: rule bank DATA rule TIMING.DECOY_ATTACK: Stealth throws a small, doomed Quad Cannon
# wave at the enemy to keep it busy until its own Palace is up. Stock Stealth Hard keeps every Quad home for the first seven minutes
# (smoke8: at most 2 of 10 ever farther than 1500 from its Command Center; Palace at ~321 s). This adds team "GX Decoy Quads" (decoy_quad_n
# Quad Cannons, one at a time, re-formed when lost) once an own Arms Dealer stands and - with decoy_until_palace 1 - while the player owns
# no Palace; on creation it stops and runs the stock "GLA Attack Enemy Base Center" sequence. No stock script or team changes; no
# condition reads the enemy (NO_ENEMY_OBJECTS: the target is the attack sequence's).
RULE_ID = 'TIMING.DECOY_ATTACK'
SCOPE = ('SkirmishGLAStealthGeneral',)
PARAMS = ('decoy_quad_n', 'decoy_until_palace')
SG, Q, AD, PAL = 'SkirmishGLAStealthGeneral', 'Slth_GLAVehicleQuadCannon', 'Slth_GLAArmsDealer', 'Slth_GLAPalace'
TEAM, COND, START, GROUP, DONOR = 'GX Decoy Quads', 'GX Decoy Quads Cond - H', 'GX Decoy Quads Start - H', 'GX TIMING.DECOY_ATTACK', 'GLA H 1ST Wave Supply Hunters. 2'
ATTACK, STOCK_GATE = 'GLA Attack Enemy Base Center', '_GLA_1ST_WAVE_H'
CLAIMS = {'scripts': [[SG, COND], [SG, START], [SG, ATTACK]], 'groups': [[SG, GROUP]], 'teams': [TEAM]}
UNTIL, OUT = 400, 1500          # seconds; world units from the own Command Center


def _bare(scb, t):
    """a copied donor team keeps no hook of its own: every script field but the production condition is blanked (a hook would name stock
    logic this rule does not claim)"""
    for e in t['_e']:
        if e[0] != 'teamProductionCondition' and e[0].startswith('team') and 'Script' in e[0] and e[2]:
            rb.team_set(scb, t, e[0], '')


def apply(scb, scope, p):
    if SG not in scope:
        return
    _x, _k, s0 = rb.find_script(scb, SG, STOCK_GATE)                 # the production-condition shape: its team action
    team_act = [mss.deep_copy(k) for k in s0.kids if k.name == 'ScriptAction']
    gate = [rb.cond(scb, 'BUILT_BY_PLAYER', ('OBJECT_TYPE', AD), ('SIDE', rb.SIDE_ME))]
    if p['decoy_until_palace']:
        gate.append(rb.own_count(scb, rb.LT, 1, PAL))
    kids = [rb.new_script(scb, START, [[rb.cond(scb, 'CONDITION_TRUE')]],
                          [rb.act(scb, 'TEAM_STOP', ('TEAM', '<This Team>')),
                           rb.act(scb, 'TEAM_EXECUTE_SEQUENTIAL_SCRIPT', ('TEAM', '<This Team>'), ('SCRIPT', ATTACK))], sub=True),
            rb.new_script(scb, COND, [gate], team_act, sub=True)]
    rb.owner_list(scb, SG).kids.append(rb.new_group(GROUP, kids))
    ts = rb.teams(scb)
    donor = rb.team(ts, DONOR)
    assert donor is not None and rb.team_get(donor, 'teamOwner') == SG, DONOR
    t = {'_e': [list(e) for e in donor['_e']], 'teamName': None}
    _bare(scb, t)
    n = p['decoy_quad_n']
    for k, v in (('teamName', TEAM), ('teamUnitType1', Q), ('teamUnitMinCount1', n), ('teamUnitMaxCount1', n), ('teamUnitMinCount2', 0),
                 ('teamUnitMaxCount2', 0), ('teamUnitMinCount3', 0), ('teamUnitMaxCount3', 0), ('teamMaxInstances', 1),
                 ('teamProductionCondition', COND), ('teamOnCreateScript', START), ('teamInitialIdleFrames', 600)):
        rb.team_set(scb, t, k, v)
    rb.add_new_team(ts, ts.index(donor) + 1, t)
    rb.put_teams(scb, ts)


def _out(obs, me, before=None):
    """the most own Quad Cannons out at once (farther than OUT from the own Command Center) in samples before `before` (a frame) and 400 s;
    None if the game ended before 400 s"""
    hz, nm, end = ob.hz(obs), ob.names(obs), ob.last_frame(obs)
    if end is None or end < UNTIL * hz:
        return None
    stop = min(UNTIL * hz, before) if before is not None else UNTIL * hz
    cc, most = None, 0
    for r in obs:
        if r.get('t') != 'obs' or r['f'] >= stop:
            continue
        mine = [o for o in r.get('o', []) if o[1] == me]
        if cc is None:
            cc = next(((o[3], o[4]) for o in mine if nm.get(o[2], '').endswith('GLACommandCenter')), None)
        if cc is not None:
            most = max(most, sum(1 for o in mine if nm.get(o[2], '').endswith('GLAVehicleQuadCannon') and
                                 (o[3] - cc[0]) ** 2 + (o[4] - cc[1]) ** 2 > OUT ** 2))
    return most


def activation(obs, me, params=None):
    """triggered: an own Arms Dealer by 150 s and the game ran past 400 s; fired: decoy_quad_n own Quads out at once before the own Palace
    (by 400 s with decoy_until_palace 0: the decoys keep coming - Codex rbd11-r1 P2)"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz, n = ob.hz(obs), (params or {}).get('decoy_quad_n', 3)
    fa, fp = ob.first_frame(obs, me, ob.ends_with('GLAArmsDealer')), ob.first_frame(obs, me, ob.ends_with('GLAPalace'))
    k = _out(obs, me, fp if (params or {}).get('decoy_until_palace', 1) else None)
    triggered = fa is not None and fa <= 150 * hz and k is not None
    return {'triggered': bool(triggered), 'fired': bool(triggered and k >= n), 'detail': 'armsdealer_f=%s palace_f=%s quads_out_before_palace=%s' % (fa, fp, k)}


CONTRAST = {'what': 'most own Quad Cannons out at once by 400 s (farther than 1500 from the own Command Center)', 'direction': 1, 'min_delta': 2}


def measure(obs, me, params=None):
    return _out(obs, me) if obs else None
