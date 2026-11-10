# GeneralsX: rule bank DATA rule RAID.GLA_TECHNICAL_DUTY (C·rb-data): a Technical raider kept on duty, hunting
# workers, trucks and dozers round the back of the enemy base, plus later diversion raiders (rows 276, 310). Stock Hard GLA's early
# raider is 2 Quad Cannons; a Technical only comes inside the 2nd-wave Fast Attack team. Per GLA general this adds:
#   team "GX Raider A <g>" (tech_n Technicals, at most 1 at a time) and, with raiders >= 2, "GX Raider B <g>" (raiders - 1 at a time,
#   from _GX_CLOCK >= ceil(diversion_t/10)); on creation each stops and runs the stock back-door attack ("GLA Attack Enemy Base Back":
#   Backdoor approach path, then hunt), with a NEW per-general priority set "GX Raider Prio <g>" (Supply Lines 150, Utility 149);
#   duty = 1 re-forms a raider whenever there are fewer than allowed; duty = 0 counts launches (GX Raids) - one A raid, raiders in all.
# No stock script or team changes; no condition reads the enemy (NO_ENEMY_OBJECTS: the target is the priority set's job;
# NO_ENEMY_FACTION: row 310's vs-flamer-China gate waits for SENSE.ENEMY_FACTION).
RULE_ID = 'RAID.GLA_TECHNICAL_DUTY'
PARAMS = ('tech_n', 'duty', 'diversion_t', 'raiders')
GEN = {'SkirmishGLA': ('GLA', ''), 'SkirmishGLADemolitionGeneral': ('Demo', 'Demo_'), 'SkirmishGLAToxinGeneral': ('Toxin', 'Chem_'),
       'SkirmishGLAStealthGeneral': ('Stealth', 'Slth_')}
DONOR = {'SkirmishGLA': 'GLA H 1ST Wave Supply Hunters', 'SkirmishGLADemolitionGeneral': 'GLA H 1ST Wave Supply Hunters. 1',
         'SkirmishGLAStealthGeneral': 'GLA H 1ST Wave Supply Hunters. 2', 'SkirmishGLAToxinGeneral': 'GLA H 1ST Wave Supply Hunters. 3'}
SET, SETS, APPLY, START, ACOND, BCOND = 'GX Raider Prio %s', 'GX Raider Prio Set %s - H', 'GX Raider Prio Apply %s - H', \
    'GX Raider Start %s - H', 'GX Raider A Cond %s - H', 'GX Raider B Cond %s - H'
TEAM_A, TEAM_B, GROUP, RAIDS = 'GX Raider A %s', 'GX Raider B %s', 'GX RAID.GLA_TECHNICAL_DUTY', 'GX Raids'
ATTACK, STOCK_GATE = 'GLA Attack Enemy Base Back', '_GLA_1ST_WAVE_H'
CLAIMS = {'scripts': [[o, n % g] for o, (g, _p) in GEN.items() for n in (SETS, APPLY, START, ACOND, BCOND)] +
                     [[o, ATTACK] for o in GEN],       # the stock attack sequence its raiders run (another rule editing it overlaps)
          'groups': [[o, GROUP] for o in GEN], 'teams': sorted(n % g for g, _p in GEN.values() for n in (TEAM_A, TEAM_B)),
          'priority_sets': sorted(SET % g for g, _p in GEN.values())}


def ticks(seconds):
    return -(-seconds // 10)


def _team(scb, donor, name, owner, p, typ, cond, g, n):
    t = {'_e': [list(e) for e in donor['_e']], 'teamName': None}
    for k, v in (('teamName', name), ('teamUnitType1', typ), ('teamUnitMinCount1', p['tech_n']), ('teamUnitMaxCount1', p['tech_n']),
                 ('teamUnitMinCount2', 0), ('teamUnitMaxCount2', 0), ('teamUnitMinCount3', 0), ('teamUnitMaxCount3', 0),
                 ('teamMaxInstances', n), ('teamProductionCondition', cond), ('teamOnCreateScript', START % g),
                 ('teamGenericScriptHook0', APPLY % g), ('teamInitialIdleFrames', 600)):
        rb.team_set(scb, t, k, v)
    assert rb.team_get(t, 'teamOwner') == owner
    return t


def apply(scb, scope, p):
    me = ('SIDE', rb.SIDE_ME)
    ts = rb.teams(scb)
    for o in scope:
        g, pre = GEN[o]
        _x, _k, s0 = rb.find_script(scb, o, STOCK_GATE)            # the production-condition shape: its team action
        team_act = [mss.deep_copy(k) for k in s0.kids if k.name == 'ScriptAction']
        _x, _k, define = rb.find_script(scb, o, 'GLA Capture Tech Building')   # the stock "object lists are ready" trigger
        trigger = [mss.deep_copy(c) for c in define.kids if c.name == 'OrCondition']
        kids = []
        sd = rb.new_script(scb, SETS % g, [], [rb.act(scb, 'SET_ATTACK_PRIORITY_THING', ('ATTACK_PRIORITY_SET', SET % g), ('OBJECT_TYPE', 'Supply Lines'), ('INT', 150)),
                                              rb.act(scb, 'SET_ATTACK_PRIORITY_THING', ('ATTACK_PRIORITY_SET', SET % g), ('OBJECT_TYPE', 'Utility'), ('INT', 149))],
                           oneshot=True)
        sd.kids = trigger + sd.kids
        kids.append(sd)
        kids.append(rb.new_script(scb, APPLY % g, [[rb.cond(scb, 'TEAM_HAS_UNITS', ('TEAM', '<This Team>'))]],
                                  [rb.act(scb, 'TEAM_APPLY_ATTACK_PRIORITY_SET', ('TEAM', '<This Team>'), ('ATTACK_PRIORITY_SET', SET % g))],
                                  oneshot=True, sub=True))
        acts = ([rb.act(scb, 'INCREMENT_COUNTER', ('INT', 1), ('COUNTER', RAIDS))] if not p['duty'] else []) + \
               [rb.act(scb, 'TEAM_STOP', ('TEAM', '<This Team>')), rb.act(scb, 'TEAM_EXECUTE_SEQUENTIAL_SCRIPT', ('TEAM', '<This Team>'), ('SCRIPT', ATTACK))]
        kids.append(rb.new_script(scb, START % g, [[rb.cond(scb, 'CONDITION_TRUE')]], acts, oneshot=False, sub=True))
        a_gate = [rb.cond(scb, 'CONDITION_TRUE')] if p['duty'] else [rb.cond(scb, 'COUNTER', ('COUNTER', RAIDS), ('COMPARISON', rb.LT), ('INT', 1))]
        kids.append(rb.new_script(scb, ACOND % g, [a_gate], [mss.deep_copy(x) for x in team_act], oneshot=False, sub=True))
        if p['raiders'] >= 2:
            b_gate = [rb.clock_at_least(scb, ticks(p['diversion_t']))] + \
                ([] if p['duty'] else [rb.cond(scb, 'COUNTER', ('COUNTER', RAIDS), ('COMPARISON', rb.LT), ('INT', p['raiders']))])
            kids.append(rb.new_script(scb, BCOND % g, [b_gate], [mss.deep_copy(x) for x in team_act], oneshot=False, sub=True))
        rb.owner_list(scb, o).kids.append(rb.new_group(GROUP, kids))
        donor = rb.team(ts, DONOR[o])
        assert donor is not None and rb.team_get(donor, 'teamOwner') == o, DONOR[o]
        at = ts.index(donor) + 1
        rb.add_new_team(ts, at, _team(scb, donor, TEAM_A % g, o, p, pre + 'GLAVehicleTechnical', ACOND % g, g, 1))
        if p['raiders'] >= 2:
            rb.add_new_team(ts, at + 1, _team(scb, donor, TEAM_B % g, o, p, pre + 'GLAVehicleTechnical', BCOND % g, g, p['raiders'] - 1))
    rb.put_teams(scb, ts)


def activation(obs, me, params=None):
    """triggered: own Arms Dealer complete by 150 s and the game ran past 180 s; fired: an own Technical by 180 s (stock's first one is
    in the 2nd-wave Fast Attack team, which waits for wave 2 and its Scorpions)"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    fa = ob.first_frame(obs, me, ob.ends_with('GLAArmsDealer'), 1, complete=True)
    end = ob.last_frame(obs)
    triggered = fa is not None and fa <= 150 * hz and end is not None and end >= 180 * hz
    ft = ob.first_frame(obs, me, lambda n: 'GLAVehicleTechnical' in n)       # it spawns as ...TechnicalChassisOne/Two/Three (smoke 30/10)
    fired = triggered and ft is not None and ft <= 180 * hz
    return {'triggered': bool(triggered), 'fired': bool(fired), 'detail': 'armsdealer_f=%s technical_f=%s' % (fa, ft)}
