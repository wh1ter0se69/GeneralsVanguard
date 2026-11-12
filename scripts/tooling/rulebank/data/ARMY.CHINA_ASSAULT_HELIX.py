# Copyright 2026 GeneralsX contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# rule bank DATA rule ARMY.CHINA_ASSAULT_HELIX: China Infantry adds an assault Helix
# group to its waves. Stock Infantry Hard's only Helix is a 3rd-wave base guard (0-1). A Helix needs an Air Field (INIZH), which no stock
# ground wave team can assume standing (a stock team with one would never form: AIPlayer::isPossibleToBuildTeam), so this adds its own
# team "GX Assault Helix" (assault_helix_n Helix, one at a time, re-formed when lost) once an own Air Field is complete and from
# _GX_CLOCK >= 24 (240 s, wave-3 time - a rule may not read the stock wave clock); on creation it stops and runs the stock centre attack
# ("China Attack Enemy Base Center"). No stock script or team changes; no condition reads the enemy. assault_helix_n = 0 is stock.
RULE_ID = 'ARMY.CHINA_ASSAULT_HELIX'
PARAMS = ('assault_helix_n',)
IG, HX, AF = 'SkirmishChinaInfantryGeneral', 'Infa_ChinaVehicleHelix', 'Infa_ChinaAirfield'
TEAM, COND, START, GROUP, DONOR = 'GX Assault Helix', 'GX Assault Helix Cond - H', 'GX Assault Helix Start - H', 'GX ARMY.CHINA_ASSAULT_HELIX', 'China H 3W Base Guards. 1'
ATTACK, STOCK_GATE, FROM = 'China Attack Enemy Base Center', '_CHINA_1ST_WAVE_H', 24
CLAIMS = {'scripts': [[IG, COND], [IG, START], [IG, ATTACK]], 'groups': [[IG, GROUP]], 'teams': [TEAM]}
UNTIL = 400


def _bare(scb, t):
    """a copied donor team keeps no hook of its own: every script field but the production condition is blanked (a hook would name stock
    logic this rule does not claim)"""
    for e in t['_e']:
        if e[0] != 'teamProductionCondition' and e[0].startswith('team') and 'Script' in e[0] and e[2]:
            rb.team_set(scb, t, e[0], '')


def apply(scb, scope, p):
    n = p['assault_helix_n']
    if IG not in scope or n == 0:
        return
    _x, _k, s0 = rb.find_script(scb, IG, STOCK_GATE)
    team_act = [mss.deep_copy(k) for k in s0.kids if k.name == 'ScriptAction']
    kids = [rb.new_script(scb, START, [[rb.cond(scb, 'CONDITION_TRUE')]],
                          [rb.act(scb, 'TEAM_STOP', ('TEAM', '<This Team>')),
                           rb.act(scb, 'TEAM_EXECUTE_SEQUENTIAL_SCRIPT', ('TEAM', '<This Team>'), ('SCRIPT', ATTACK))], sub=True),
            rb.new_script(scb, COND, [[rb.cond(scb, 'BUILT_BY_PLAYER', ('OBJECT_TYPE', AF), ('SIDE', rb.SIDE_ME)), rb.clock_at_least(scb, FROM)]],
                          team_act, sub=True)]
    rb.owner_list(scb, IG).kids.append(rb.new_group(GROUP, kids))
    ts = rb.teams(scb)
    donor = rb.team(ts, DONOR)
    assert donor is not None and rb.team_get(donor, 'teamOwner') == IG and rb.team_get(donor, 'teamUnitType1') == HX, DONOR
    t = {'_e': [list(e) for e in donor['_e']], 'teamName': None}
    _bare(scb, t)
    for k, v in (('teamName', TEAM), ('teamUnitType1', HX), ('teamUnitMinCount1', n), ('teamUnitMaxCount1', n), ('teamUnitMinCount2', 0),
                 ('teamUnitMaxCount2', 0), ('teamUnitMinCount3', 0), ('teamUnitMaxCount3', 0), ('teamMaxInstances', 1),
                 ('teamProductionCondition', COND), ('teamOnCreateScript', START), ('teamInitialIdleFrames', 600)):
        rb.team_set(scb, t, k, v)
    rb.add_new_team(ts, ts.index(donor) + 1, t)
    rb.put_teams(scb, ts)


def _helix(obs, me):
    return ob.built_by(obs, me, ob.ends_with('ChinaVehicleHelix'), UNTIL * ob.hz(obs)) if obs else None


def activation(obs, me, params=None):
    """triggered: own Air Field complete by 240 s and the game ran past 400 s; fired: 2 own Helix by 400 s"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    fa = ob.first_frame(obs, me, ob.ends_with('ChinaAirfield'), 1, complete=True)
    n = _helix(obs, me)
    triggered = fa is not None and fa <= 240 * hz and n is not None
    return {'triggered': bool(triggered), 'fired': bool(triggered and n >= 2), 'detail': 'airfield_f=%s helix_by_400s=%s' % (fa, n)}


CONTRAST = {'what': 'own Helix built by 400 s', 'direction': 1, 'min_delta': 1}


def measure(obs, me, params=None):
    return _helix(obs, me)
