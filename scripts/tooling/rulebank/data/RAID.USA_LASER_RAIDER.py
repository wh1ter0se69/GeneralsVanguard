# Copyright 2026 GeneralsX contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# rule bank DATA rule RAID.USA_LASER_RAIDER: USA sends an early Humvee raider team at
# dozers and forward buildings. Stock USA Hard has no raider before its wave-1 teams. Per USA general this adds team "GX Humvee Raiders
# <g>" (raid_size of the general's own Humvee, one at a time, re-formed when lost) from _GX_CLOCK >= raid_start_esc (the tip counts in
# escalation steps; a rule may not read _ESCALATION, so one step is one clock tick, 10 s); on creation it stops and runs the stock back-door
# attack ("USA Attack Enemy Base Back"). No stock script or team changes; no condition reads the enemy. target_enum (dozers first) is a
# priority set: a 7-minute game seldom shows a target choice (as RAID.PRIO_ECONOMY), so it is not built.
RULE_ID = 'RAID.USA_LASER_RAIDER'
PARAMS = ('raid_start_esc', 'raid_size')
UNUSED = {'target_enum': 'one value (0 = dozer): the box holds nothing to vary; a dozer-first priority set is PARKED with RAID.PRIO_ECONOMY'}
GEN = {'SkirmishAmerica': ('USA', '', 'USA H 1ST Wave Supply Hunters'), 'SkirmishAmericaAirForceGeneral': ('AirForce', 'AirF_', 'USA H 1ST Wave Supply Hunters. 1'),
       'SkirmishAmericaLaserGeneral': ('Laser', 'Lazr_', 'USA H 1ST Wave Supply Hunters. 2'),
       'SkirmishAmericaSuperWeaponGeneral': ('Superweapon', 'SupW_', 'USA H 1W Guard Combat Zone. 3')}
TEAM, COND, START, GROUP = 'GX Humvee Raiders %s', 'GX Humvee Raiders Cond %s - H', 'GX Humvee Raiders Start %s - H', 'GX RAID.USA_LASER_RAIDER'
PRIO = 'USA Apply Supply Lines Priority'                  # the stock hook that points a raider at supply lines (it only applies a stock table)
ATTACK, STOCK_GATE = 'USA Attack Enemy Base Back', '_USA_1ST_WAVE_H'
CLAIMS = {'scripts': [[o, n % g] for o, (g, _p, _d) in GEN.items() for n in (COND, START)] + [[o, ATTACK] for o in GEN] + [[o, PRIO] for o in GEN],
          'groups': [[o, GROUP] for o in GEN], 'teams': sorted(TEAM % g for g, _p, _d in GEN.values())}
UNTIL = 150


def _team(scb, donor, name, owner, typ, n, cond, start):
    t = {'_e': [list(e) for e in donor['_e']], 'teamName': None}
    _bare(scb, t)
    for k, v in (('teamGenericScriptHook0', PRIO), ('teamName', name), ('teamUnitType1', typ), ('teamUnitMinCount1', n), ('teamUnitMaxCount1', n), ('teamUnitMinCount2', 0),
                 ('teamUnitMaxCount2', 0), ('teamUnitMinCount3', 0), ('teamUnitMaxCount3', 0), ('teamMaxInstances', 1),
                 ('teamProductionCondition', cond), ('teamOnCreateScript', start), ('teamInitialIdleFrames', 600)):
        rb.team_set(scb, t, k, v)
    assert rb.team_get(t, 'teamOwner') == owner
    return t


def _bare(scb, t):
    """a copied donor team keeps no hook of its own: every script field but the production condition is blanked (a hook would name stock
    logic this rule does not claim)"""
    for e in t['_e']:
        if e[0] != 'teamProductionCondition' and e[0].startswith('team') and 'Script' in e[0] and e[2]:
            rb.team_set(scb, t, e[0], '')


def apply(scb, scope, p):
    ts = rb.teams(scb)
    for o in scope:
        g, pre, donor_name = GEN[o]
        _x, _k, s0 = rb.find_script(scb, o, STOCK_GATE)
        team_act = [mss.deep_copy(k) for k in s0.kids if k.name == 'ScriptAction']
        kids = [rb.new_script(scb, START % g, [[rb.cond(scb, 'CONDITION_TRUE')]],
                              [rb.act(scb, 'TEAM_STOP', ('TEAM', '<This Team>')),
                               rb.act(scb, 'TEAM_EXECUTE_SEQUENTIAL_SCRIPT', ('TEAM', '<This Team>'), ('SCRIPT', ATTACK))], sub=True),
                rb.new_script(scb, COND % g, [[rb.clock_at_least(scb, p['raid_start_esc'])]], team_act, sub=True)]
        rb.owner_list(scb, o).kids.append(rb.new_group(GROUP, kids))
        donor = rb.team(ts, donor_name)
        assert donor is not None and rb.team_get(donor, 'teamOwner') == o, donor_name
        rb.add_new_team(ts, ts.index(donor) + 1, _team(scb, donor, TEAM % g, o, pre + 'AmericaVehicleHumvee', p['raid_size'], COND % g, START % g))
    rb.put_teams(scb, ts)


def _humvees(obs, me):
    return ob.built_by(obs, me, ob.ends_with('AmericaVehicleHumvee'), UNTIL * ob.hz(obs)) if obs else None


def activation(obs, me, params=None):
    """triggered: own War Factory complete by 100 s and the game ran past 150 s; fired: 2 own Humvees by 150 s"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    fw = ob.first_frame(obs, me, ob.ends_with('AmericaWarFactory'), 1, complete=True)
    n = _humvees(obs, me)
    triggered = fw is not None and fw <= 100 * hz and n is not None
    return {'triggered': bool(triggered), 'fired': bool(triggered and n >= 2), 'detail': 'warfactory_f=%s humvees_by_150s=%s' % (fw, n)}


CONTRAST = {'what': 'own Humvees built by 150 s', 'direction': 1, 'min_delta': 1}


def measure(obs, me, params=None):
    return _humvees(obs, me)
