# Copyright 2026 GeneralsX contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# rule bank DATA rule OPEN.GLA_DEMO_HARASS: Demolition GLA's opening harass ends in a
# Bomb Truck. Stock Demolition Hard's first Bomb Truck is optional in the 3rd-wave guard team. This adds team "GX Demo Bomb Truck" (one
# Bomb Truck, one at a time) once an own Palace is complete (the truck needs one, INIZH) and from _GX_CLOCK >= ceil(bomb_truck_t / 10); on
# creation it stops and runs the stock flank attack ("GLA Attack Enemy Base Flank"; the Bomb Truck attack sequences read the enemy and may
# not be named). No stock script or team changes; no condition reads the enemy. The other two parts are other rules': the harassing
# Technical is RAID.GLA_TECHNICAL_DUTY, three oils OIL.COUNT_GATES / OIL.CAPTURE_TEAMS.
RULE_ID = 'OPEN.GLA_DEMO_HARASS'
PARAMS = ('bomb_truck_t',)
UNUSED = {'opening_technical': 'the harassing Technical is RAID.GLA_TECHNICAL_DUTY (Demolition in its scope)',
          'oil_count': 'the oil count is OIL.COUNT_GATES / OIL.CAPTURE_TEAMS'}
DG, BT, PAL = 'SkirmishGLADemolitionGeneral', 'Demo_GLAVehicleBombTruck', 'Demo_GLAPalace'
TEAM, COND, START, GROUP, DONOR = 'GX Demo Bomb Truck', 'GX Demo Bomb Truck Cond - H', 'GX Demo Bomb Truck Start - H', 'GX OPEN.GLA_DEMO_HARASS', 'GLA H 1ST Wave Supply Hunters. 1'
ATTACK, STOCK_GATE = 'GLA Attack Enemy Base Flank', '_GLA_1ST_WAVE_H'
CLAIMS = {'scripts': [[DG, COND], [DG, START], [DG, ATTACK]], 'groups': [[DG, GROUP]], 'teams': [TEAM]}
UNTIL = 400


def _bare(scb, t):
    """a copied donor team keeps no hook of its own: every script field but the production condition is blanked (a hook would name stock
    logic this rule does not claim)"""
    for e in t['_e']:
        if e[0] != 'teamProductionCondition' and e[0].startswith('team') and 'Script' in e[0] and e[2]:
            rb.team_set(scb, t, e[0], '')


def apply(scb, scope, p):
    if DG not in scope:
        return
    _x, _k, s0 = rb.find_script(scb, DG, STOCK_GATE)
    team_act = [mss.deep_copy(k) for k in s0.kids if k.name == 'ScriptAction']
    kids = [rb.new_script(scb, START, [[rb.cond(scb, 'CONDITION_TRUE')]],
                          [rb.act(scb, 'TEAM_STOP', ('TEAM', '<This Team>')),
                           rb.act(scb, 'TEAM_EXECUTE_SEQUENTIAL_SCRIPT', ('TEAM', '<This Team>'), ('SCRIPT', ATTACK))], sub=True),
            rb.new_script(scb, COND, [[rb.cond(scb, 'BUILT_BY_PLAYER', ('OBJECT_TYPE', PAL), ('SIDE', rb.SIDE_ME)),
                                       rb.clock_at_least(scb, -(-p['bomb_truck_t'] // 10))]], team_act, sub=True)]
    rb.owner_list(scb, DG).kids.append(rb.new_group(GROUP, kids))
    ts = rb.teams(scb)
    donor = rb.team(ts, DONOR)
    assert donor is not None and rb.team_get(donor, 'teamOwner') == DG, DONOR
    t = {'_e': [list(e) for e in donor['_e']], 'teamName': None}
    _bare(scb, t)
    for k, v in (('teamName', TEAM), ('teamUnitType1', BT), ('teamUnitMinCount1', 1), ('teamUnitMaxCount1', 1), ('teamUnitMinCount2', 0),
                 ('teamUnitMaxCount2', 0), ('teamUnitMinCount3', 0), ('teamUnitMaxCount3', 0), ('teamMaxInstances', 1),
                 ('teamProductionCondition', COND), ('teamOnCreateScript', START), ('teamInitialIdleFrames', 600)):
        rb.team_set(scb, t, k, v)
    rb.add_new_team(ts, ts.index(donor) + 1, t)
    rb.put_teams(scb, ts)


def _trucks(obs, me):
    return ob.built_by(obs, me, ob.ends_with('GLAVehicleBombTruck'), UNTIL * ob.hz(obs)) if obs else None


def activation(obs, me, params=None):
    """triggered: own Palace complete by 300 s and the game ran past 400 s; fired: an own Bomb Truck by 400 s"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    fp = ob.first_frame(obs, me, ob.ends_with('GLAPalace'), 1, complete=True)
    n = _trucks(obs, me)
    triggered = fp is not None and fp <= 300 * hz and n is not None
    return {'triggered': bool(triggered), 'fired': bool(triggered and n >= 1), 'detail': 'palace_f=%s bomb_trucks_by_400s=%s' % (fp, n)}


CONTRAST = {'what': 'own Bomb Trucks built by 400 s', 'direction': 1, 'min_delta': 1}


def measure(obs, me, params=None):
    return _trucks(obs, me)
