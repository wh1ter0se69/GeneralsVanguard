# Copyright 2026 GeneralsX contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# rule bank DATA rule NOFLOAT.CLONE_GATES: never float a bank - keep adding producers
# (rows 22, 65, 148). Stock Hard adds a 2nd Barracks / War Factory / Air Field only at $15,000 (the recipe's RICH1 clones) or $30,000.
# For USA, the Air Force and China vanilla (China Tank and Nuke stay EXEMPT: the recipe's NO_SPEND, their clones hurt them):
#   clone_cash   -> the RICH1 clones' credit gate (their _ESCALATION gate is left exactly as it is);
#   a ladder of extra_prod Hard one-shots per producer type T: rung k builds one more T once money >= max(float_cash_max, cost_T)
#   + chinook_reserve + (k-1) * bank_max, _GX_CLOCK >= esc_min (0 = no clock gate) and exactly base_T + k - 1 of T stand - so it is
#   self-limiting and keeps chinook_reserve in the bank (a lost Chinook - 950 or 1200 on the Air Force - can be replaced).
# NOT here: "research at one Air Field while the others produce" - no scb lever reserves a producer: ENGINE.
# Not for USA on a base built with the generator's USA_GENOME: that scales America's RICH1 credit gates x1.4 (15,000 -> 21,000), and
# clone_cash would overwrite the tuned value. apply() refuses America by name there; the Air Force and China apply as before.
RULE_ID = 'NOFLOAT.CLONE_GATES'
SCOPE = ('SkirmishAmerica', 'SkirmishAmericaAirForceGeneral', 'SkirmishChina')
PARAMS = ('float_cash_max', 'chinook_reserve', 'bank_max', 'esc_min', 'clone_cash', 'extra_prod')
TYPES = {'SkirmishAmerica': ('AmericaBarracks', 'AmericaWarFactory', 'AmericaAirfield'),
         'SkirmishAmericaAirForceGeneral': ('AirF_AmericaBarracks', 'AirF_AmericaWarFactory', 'AirF_AmericaAirfield'),
         'SkirmishChina': ('ChinaBarracks', 'ChinaWarFactory', 'ChinaAirfield')}
COST = {'Barracks': 600, 'WarFactory': 2000, 'Airfield': 1000}         # ZH build costs (INFERRED; China's Barracks is 500, close enough)
BASE = {('SkirmishChina', 'ChinaWarFactory'): 2}                        # stock China already builds its 2nd War Factory at once
MAX_RUNGS = 5
LADDER, GROUP = 'GX NoFloat %d %s', 'GX NOFLOAT.CLONE_GATES'
STOCK_RICH1 = 15000
GENOME_RICH1 = 21000        # America's RICH1 gates on a USA_GENOME base: round(15000 x 1.4)


def _rich1(scb, owner):
    """the recipe's RICH1 clones of this list (one per producer type), with their stock credit leaf"""
    out = []
    for _g, _k, s in rb.scripts_of(scb, owner):
        if ' RICH1 ' in (rb.node_name(s) or ''):
            out.append(s)
    return out


CLAIMS = {'scripts': [[o, LADDER % (k, t)] for o, ts in TYPES.items() for t in ts for k in range(1, MAX_RUNGS + 1)] +
                     [[o, n] for o, ns in {'SkirmishAmerica': ('USA Barracks RICH1 AmericaBarracks', 'USA War Factory RICH1 AmericaWarFactory',
                                                              'USA Air Field RICH1 AmericaAirfield'),
                                           'SkirmishAmericaAirForceGeneral': ('USA High Money Barracks RICH1 AirF_AmericaBarracks',
                                                                              'USA High Money War Factory 2 RICH1 AirF_AmericaWarFactory',
                                                                              'USA Air Field RICH1 AirF_AmericaAirfield'),
                                           'SkirmishChina': ('China High Money Builds RICH1 ChinaWarFactory', 'China High Money Builds RICH1 ChinaBarracks',
                                                             'China Air Field RICH1 ChinaAirfield')}.items() for n in ns],
          'groups': [[o, GROUP] for o in TYPES]}
DEFAULTS = {'float_cash_max': 1500, 'chinook_reserve': 1200, 'bank_max': 2500, 'esc_min': 10, 'extra_prod': 3}


def threshold(p, t, k):
    cost = next(v for key, v in COST.items() if t.endswith(key))
    return max(p['float_cash_max'], cost) + p['chinook_reserve'] + (k - 1) * p['bank_max']


def apply(scb, scope, p):
    me = ('SIDE', rb.SIDE_ME)
    if 'SkirmishAmerica' in scope:                  # checked before anything is written
        for s in _rich1(scb, 'SkirmishAmerica'):
            for l in rb.leaves(s):
                n, ps = rb.decode(scb, l)
                assert not (n == 'PLAYER_HAS_CREDITS' and ps[0] == ('INT', GENOME_RICH1)), (
                    'not for SkirmishAmerica on a base with USA_GENOME on (its RICH1 credit gates are %d, not %d, and clone_cash would '
                    'overwrite them); pass generals without SkirmishAmerica' % (GENOME_RICH1, STOCK_RICH1))
    for o in scope:
        rich = _rich1(scb, o)
        assert len(rich) == 3, '%s: %d RICH1 clones' % (o, len(rich))
        for s in rich:
            hit = 0
            for l in rb.leaves(s):
                n, ps = rb.decode(scb, l)
                if n == 'PLAYER_HAS_CREDITS':
                    assert ps[0] == ('INT', STOCK_RICH1), (rb.node_name(s), ps)
                    l.raw = rb.credits_at_least(scb, p['clone_cash']).raw; hit += 1
            assert hit == 1, (rb.node_name(s), hit)
        kids = []
        for t in TYPES[o]:
            for k in range(1, p['extra_prod'] + 1):
                gate = [rb.credits_at_least(scb, threshold(p, t, k))]
                if p['esc_min'] > 0:
                    gate.append(rb.clock_at_least(scb, p['esc_min']))
                gate.append(rb.own_count(scb, rb.EQ, BASE.get((o, t), 1) + k - 1, t))
                kids.append(rb.new_script(scb, LADDER % (k, t), [gate],
                                          [rb.act(scb, 'AI_PLAYER_BUILD_SUPPLY_CENTER', me, ('OBJECT_TYPE', t), ('INT', 1000))], oneshot=True))
        rb.owner_list(scb, o).kids.append(rb.new_group(GROUP, kids))


def activation(obs, me, params=None):
    """triggered: my money reached the first rung (Barracks/Air Field threshold) by 240 s and the game ran past 300 s; fired: a second
    own Barracks or Air Field (any build state) by 300 s (stock needs $15,000 for either)"""
    p = dict(DEFAULTS, **(params or {}))
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    thr = threshold(p, 'Barracks', 1)
    fm = ob.money_at_least(obs, me, thr)
    end = ob.last_frame(obs)
    triggered = fm is not None and fm <= 240 * hz and end is not None and end >= 300 * hz
    fb = ob.first_frame(obs, me, ob.ends_with('Barracks'), 2)
    fa = ob.first_frame(obs, me, ob.ends_with('Airfield'), 2)
    f2 = min([f for f in (fb, fa) if f is not None] or [None]) if (fb or fa) else None
    fired = triggered and f2 is not None and f2 <= 300 * hz
    return {'triggered': bool(triggered), 'fired': bool(fired), 'detail': 'money_%d_f=%s second_barracks_f=%s second_airfield_f=%s' % (thr, fm, fb, fa)}
