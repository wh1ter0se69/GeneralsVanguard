# Copyright 2026 GeneralsX contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# rule bank DATA rule OPEN.SECOND_SUPPLY_T: a second supply source by N seconds, every
# general. Stock Hard gets its second centre only from "<Side> Base Expansion Check" (money <= 7000 and _ESCALATION >= 25, ~250 s, or
# when an ENEMY holds 2+ supply buildings - read through fog); that script reaches the $4,000 expansion gift, so it stays frozen. This
# adds one Hard-only one-shot script per general instead: once the first centre stands and _GX_CLOCK >= ceil(t/10), build another
# centre with the stock first-centre action (AIPlayer::findSupplyCenter skips a pile that already has one of ours, so it goes to a
# different pile). Own state only; no enemy term, no gift, no stock script edited.
RULE_ID = 'OPEN.SECOND_SUPPLY_T'
PARAMS = ('second_supply_t',)
SIDES = {'SkirmishAmerica': 'USA', 'SkirmishAmericaAirForceGeneral': 'USA', 'SkirmishAmericaLaserGeneral': 'USA',
         'SkirmishAmericaSuperWeaponGeneral': 'USA', 'SkirmishChina': 'China', 'SkirmishChinaTankGeneral': 'China',
         'SkirmishChinaNukeGeneral': 'China', 'SkirmishChinaInfantryGeneral': 'China', 'SkirmishGLA': 'GLA',
         'SkirmishGLADemolitionGeneral': 'GLA', 'SkirmishGLAToxinGeneral': 'GLA', 'SkirmishGLAStealthGeneral': 'GLA'}
GROUP = 'GX OPEN.SECOND_SUPPLY_T'
SCRIPT = '%s GX Second Supply - H'
CLAIMS = {'scripts': [[o, SCRIPT % s] for o, s in SIDES.items()], 'groups': [[o, GROUP] for o in SIDES]}
DEFAULTS = {'second_supply_t': 45}
SUPPLY = ('SupplyCenter', 'SupplyStash')


def ticks(seconds):
    return -(-seconds // 10)          # ceil: never earlier than asked (one _GX_CLOCK tick = 10 s)


def first_centre(scb, owner):
    """the general's own supply template, read from its stock '<Side> Supply Center - H' (the first-centre build)"""
    _g, _k, s = rb.find_script(scb, owner, '%s Supply Center - H' % SIDES[owner])
    hits = [v for l in rb.leaves(s) for n, ps in [rb.decode(scb, l)] if n == 'AI_PLAYER_BUILD_SUPPLY_CENTER'
            for t, v in ps if t == 'OBJECT_TYPE']
    assert len(hits) == 1 and hits[0].endswith(SUPPLY), '%s: first-centre template %r' % (owner, hits)
    return hits[0]


def apply(scb, scope, params):
    for o in scope:
        tmpl = first_centre(scb, o)
        s = rb.new_script(scb, SCRIPT % SIDES[o], [[rb.clock_at_least(scb, ticks(params['second_supply_t'])),
                                                    rb.cond(scb, 'BUILT_BY_PLAYER', ('OBJECT_TYPE', tmpl), ('SIDE', rb.SIDE_ME))]],
                          [rb.act(scb, 'AI_PLAYER_BUILD_SUPPLY_CENTER', ('SIDE', rb.SIDE_ME), ('OBJECT_TYPE', tmpl), ('INT', 10000))],
                          oneshot=True)
        rb.owner_list(scb, o).kids.append(rb.new_group(GROUP, [s]))


def activation(obs, me, params=None):
    """triggered: the first own supply building is complete and the clock passed the threshold; fired: a second own supply building
    (any build state) appears within 60 s after that moment (stock's comes ~250 s in, from the escalation-gated expansion)"""
    p = dict(DEFAULTS, **(params or {}))
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    f1 = ob.first_frame(obs, me, ob.ends_with(*SUPPLY), 1, complete=True)
    at = None if f1 is None else max(f1, ticks(p['second_supply_t']) * 10 * hz)
    end = ob.last_frame(obs)
    triggered = at is not None and end is not None and end >= at + 60 * hz
    f2 = ob.first_frame(obs, me, ob.ends_with(*SUPPLY), 2)
    fired = triggered and f2 is not None and f2 <= at + 60 * hz
    return {'triggered': bool(triggered), 'fired': bool(fired), 'detail': 'first_complete_f=%s due_f=%s second_f=%s' % (f1, at, f2)}
