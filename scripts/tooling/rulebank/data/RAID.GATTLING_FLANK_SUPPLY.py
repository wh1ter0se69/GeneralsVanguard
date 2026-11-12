# Copyright 2026 GeneralsX contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# rule bank DATA rule RAID.GATTLING_FLANK_SUPPLY: China sends Gattling raiders round
# the flank at dozers, trucks and Chinooks from the first minutes. Stock China Hard's first raiders are the "1ST Wave Supply Hunters"
# (2 Gattlings), formed only when its own wave clock says wave 1. Per China general this adds team "GX Flank Raiders <g>" (raid_team
# Gattling Tanks, one at a time, re-formed when lost) from _GX_CLOCK >= ceil(raid_start_t / 10); on creation it stops and runs the stock
# flank attack ("China Attack Enemy Base Flank": the Flank approach path, then attack). No stock script or team changes; no condition
# reads the enemy (NO_ENEMY_OBJECTS: the target is the attack sequence's). China only: GLA's raider is RAID.GLA_TECHNICAL_DUTY, USA's
# RAID.USA_LASER_RAIDER.
RULE_ID = 'RAID.GATTLING_FLANK_SUPPLY'
SCOPE = ('SkirmishChina', 'SkirmishChinaTankGeneral', 'SkirmishChinaNukeGeneral', 'SkirmishChinaInfantryGeneral')
PARAMS = ('raid_team', 'raid_start_t')
UNUSED = {'path_enum': 'one value (0 = Flank): the box holds nothing to vary'}
GEN = {'SkirmishChina': ('China', '', 'China H 1ST Wave Supply Hunters'), 'SkirmishChinaTankGeneral': ('Tank', 'Tank_', 'China H 1ST Wave Supply Hunters. 3'),
       'SkirmishChinaNukeGeneral': ('Nuke', 'Nuke_', 'China H 1ST Wave Supply Hunters. 2'),
       'SkirmishChinaInfantryGeneral': ('Infantry', 'Infa_', 'China H 1ST Wave Supply Hunters. 1')}
TEAM, COND, START, GROUP = 'GX Flank Raiders %s', 'GX Flank Raiders Cond %s - H', 'GX Flank Raiders Start %s - H', 'GX RAID.GATTLING_FLANK_SUPPLY'
PRIO = 'China Apply Supply Lines Priority'                  # the stock hook that points a raider at supply lines (it only applies a stock table)
ATTACK, STOCK_GATE = 'China Attack Enemy Base Flank', '_CHINA_1ST_WAVE_H'
CLAIMS = {'scripts': [[o, n % g] for o, (g, _p, _d) in GEN.items() for n in (COND, START)] + [[o, ATTACK] for o in GEN] + [[o, PRIO] for o in GEN],
          'groups': [[o, GROUP] for o in GEN], 'teams': sorted(TEAM % g for g, _p, _d in GEN.values())}
UNTIL = 240                 # rb-data-11 (POST-HOC, after smoke8): stock China's War Factory completes at 114-122 s; 120 s saw no Gattling


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
        _x, _k, s0 = rb.find_script(scb, o, STOCK_GATE)              # the production-condition shape: its team action
        team_act = [mss.deep_copy(k) for k in s0.kids if k.name == 'ScriptAction']
        kids = [rb.new_script(scb, START % g, [[rb.cond(scb, 'CONDITION_TRUE')]],
                              [rb.act(scb, 'TEAM_STOP', ('TEAM', '<This Team>')),
                               rb.act(scb, 'TEAM_EXECUTE_SEQUENTIAL_SCRIPT', ('TEAM', '<This Team>'), ('SCRIPT', ATTACK))], sub=True),
                rb.new_script(scb, COND % g, [[rb.clock_at_least(scb, -(-p['raid_start_t'] // 10))]], team_act, sub=True)]
        rb.owner_list(scb, o).kids.append(rb.new_group(GROUP, kids))
        donor = rb.team(ts, donor_name)
        assert donor is not None and rb.team_get(donor, 'teamOwner') == o, donor_name
        rb.add_new_team(ts, ts.index(donor) + 1, _team(scb, donor, TEAM % g, o, pre + 'ChinaTankGattling', p['raid_team'], COND % g, START % g))
    rb.put_teams(scb, ts)


def _gats(obs, me):
    return ob.built_by(obs, me, ob.ends_with('ChinaTankGattling'), UNTIL * ob.hz(obs)) if obs else None


def activation(obs, me, params=None):
    """triggered: own War Factory complete by 150 s and the game ran past 240 s; fired: 2 own Gattling Tanks by 240 s"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    fw = ob.first_frame(obs, me, ob.ends_with('ChinaWarFactory'), 1, complete=True)
    n = _gats(obs, me)
    triggered = fw is not None and fw <= 150 * hz and n is not None
    return {'triggered': bool(triggered), 'fired': bool(triggered and n >= 2), 'detail': 'warfactory_f=%s gattlings_by_240s=%s' % (fw, n)}


CONTRAST = {'what': 'own Gattling Tanks built by 240 s', 'direction': 1, 'min_delta': 1, 'post_hoc_seeds': (1, 2)}


def measure(obs, me, params=None):
    return _gats(obs, me)
