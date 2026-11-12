# Copyright 2026 GeneralsX contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# rule bank DATA rule OPEN.GLA_SUPPLY_COUNT: how many supply stashes GLA opens with, and
# an early Technical raid. Stock Hard GLA builds one stash at 2 workers; more come only from the escalation-gated, gift-carrying Base
# Expansion Check (frozen). Its one early raider, the "GLA H 1ST Wave Supply Hunters" team, is queued while _ATTACK_WAVE <= 1 - and every
# _ATTACK_WAVE writer gates on _ESCALATION, which stock raises for enemy tech: an enemy read through fog. So:
#   2nd stash: as soon as the first stands (pros place the stash first); 3rd (supply_n = 3): once two stand and the Arms Dealer is up;
#   raid: the Supply Hunters team's production condition becomes a new own-clock window, _GX_CLOCK in [ceil(t/10), ceil(t/10) + 18)
#   (3 min), and technical_count Technicals join its slot 2 (0 = the slot stays stock).
# Own state only; no faction gate.
RULE_ID = 'OPEN.GLA_SUPPLY_COUNT'
PARAMS = ('supply_n', 'tnt_raid_t', 'technical_count')
GEN = {'SkirmishGLA': '', 'SkirmishGLADemolitionGeneral': 'Demo_', 'SkirmishGLAToxinGeneral': 'Chem_', 'SkirmishGLAStealthGeneral': 'Slth_'}
TEAM = 'GLA H 1ST Wave Supply Hunters'
GATE, STOCK_GATE = 'GLA GX Raid Gate - H', '_GLA_1ST_WAVE_H'
GROUP = 'GX OPEN.GLA_SUPPLY_COUNT'
S2, S3 = 'GLA GX 2nd Supply - H', 'GLA GX 3rd Supply - H'
WINDOW = 18                                  # _GX_CLOCK ticks the raid team may be produced in (stock: until the 2nd wave)
TEAMS = {'SkirmishGLA': TEAM, 'SkirmishGLADemolitionGeneral': TEAM + '. 1', 'SkirmishGLAStealthGeneral': TEAM + '. 2',
         'SkirmishGLAToxinGeneral': TEAM + '. 3'}
CLAIMS = {'scripts': [[o, n] for o in GEN for n in (S2, S3, GATE)], 'groups': [[o, GROUP] for o in GEN], 'teams': sorted(TEAMS.values())}
TEAM_UNIT_WAIVERS = {o + 'GLAVehicleTechnical': 'INIZH: no prerequisite beyond the Arms Dealer, which also builds the stock Scorpion / Quad Cannon of these teams' for o in GEN.values() if o}
DEFAULTS = {'supply_n': 2, 'tnt_raid_t': 90, 'technical_count': 1}


def ticks(seconds):
    return -(-seconds // 10)


def apply(scb, scope, params):
    me = ('SIDE', rb.SIDE_ME)
    ts = rb.teams(scb)
    for o in scope:
        st, ad = GEN[o] + 'GLASupplyStash', GEN[o] + 'GLAArmsDealer'
        build = lambda: rb.act(scb, 'AI_PLAYER_BUILD_SUPPLY_CENTER', me, ('OBJECT_TYPE', st), ('INT', 10000))
        kids = [rb.new_script(scb, S2, [[rb.cond(scb, 'BUILT_BY_PLAYER', ('OBJECT_TYPE', st), me)]], [build()], oneshot=True)]
        if params['supply_n'] >= 3:
            kids.append(rb.new_script(scb, S3, [[rb.own_count(scb, rb.GE, 2, st), rb.cond(scb, 'BUILT_BY_PLAYER', ('OBJECT_TYPE', ad), me)]],
                                      [build()], oneshot=True))
        # the raid gate: the stock production-condition script's shape (a repeating Hard subroutine + its team action), own clock only
        _g, _k, s0 = rb.find_script(scb, o, STOCK_GATE)
        t0 = ticks(params['tnt_raid_t'])
        acts = [mss.deep_copy(k) for k in s0.kids if k.name == 'ScriptAction']
        kids.append(rb.new_script(scb, GATE, [[rb.clock_at_least(scb, t0),
                                               rb.cond(scb, 'COUNTER', ('COUNTER', rb.GX_CLOCK), ('COMPARISON', rb.LT), ('INT', t0 + WINDOW))]],
                                  acts, oneshot=False, sub=True))
        rb.owner_list(scb, o).kids.append(rb.new_group(GROUP, kids))
        t = rb.team(ts, TEAMS[o])
        assert t is not None and rb.team_get(t, 'teamOwner') == o, '%s: no team %r' % (o, TEAMS[o])
        assert rb.team_get(t, 'teamProductionCondition') == STOCK_GATE, (o, rb.team_get(t, 'teamProductionCondition'))
        rb.team_set(scb, t, 'teamProductionCondition', GATE)
        if params['technical_count'] > 0:
            rb.team_set(scb, t, 'teamUnitType2', GEN[o] + 'GLAVehicleTechnical')
            rb.team_set(scb, t, 'teamUnitMinCount2', params['technical_count'])
            rb.team_set(scb, t, 'teamUnitMaxCount2', params['technical_count'])
    rb.put_teams(scb, ts)


def activation(obs, me, params=None):
    """triggered: the first own stash is complete; fired: a second own stash (any build state) within 60 s of it (stock's comes ~250 s
    in). The raid part is in the detail: the first own Technical's frame."""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz, is_st = ob.hz(obs), ob.ends_with('GLASupplyStash')
    f1 = ob.first_frame(obs, me, is_st, 1, complete=True)
    end = ob.last_frame(obs)
    triggered = f1 is not None and end is not None and end >= f1 + 60 * hz
    f2 = ob.first_frame(obs, me, is_st, 2)
    fired = triggered and f2 is not None and f2 <= f1 + 60 * hz
    ft = ob.first_frame(obs, me, ob.ends_with('GLAVehicleTechnical'))
    return {'triggered': bool(triggered), 'fired': bool(fired), 'detail': 'first_complete_f=%s second_f=%s technical_f=%s' % (f1, f2, ft)}
