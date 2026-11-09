# GeneralsX @feature Claude 01/11/2026 rule bank DATA rule OPEN.CHINA_BUNKER_WF (C·rb-data-6): the lean China opening - ONE Barracks, ONE
# War Factory (Dominator stand-in, medium). Map of stock Hard (owners.txt, 14d03e02): the first Barracks ("China Barracks - H", CONDITION_TRUE)
# and the first War Factory (after the first Supply Center) are the lean opening already. The extras are two plain build scripts:
#   "China War Factory 2 - H"  (SkirmishChina, SkirmishChinaNukeGeneral): the instant a first War Factory stands
#   "China Barracks 2 - H"     (SkirmishChinaInfantryGeneral):            the instant the first Barracks stands
# warfactory_n / barracks_n = 1 holds each back until the player owns a SECOND Supply Center, or _GX_CLOCK reaches 36 (360 s, the floor
# so it always opens); 2 = stock. Own state only, no enemy term, no gift: the RICH / High Money Build scripts (30000+ credits) that add
# factories when rich are untouched, so this is a deferral, never a cap. The Tank general's own factory chain (WF 2-4) is not in scope.
# NOT here (UNUSED, each says why): the early front bunker (stock's "China Build 1st Bunker - H" already follows the first Gattling), the
# loaded Listening Outpost first (no stock script builds one: a new team + load behaviour), two dozers (stock is already two; no scb
# action removes one), helix_delay_esc (OIL.COUNT_GATES' gate).
RULE_ID = 'OPEN.CHINA_BUNKER_WF'
# rb-data-7 (lead ruling): NUKE ONLY. Smoke 5 (n=2 per arm, thin): Nuke 2 vs 3 buildings at 300 s in both seeds; vanilla China 4 vs 3 (WF2 later but an
# extra Barracks appears - the engine adds a producer when teams starve, INFERRED) and Infantry MIXED, so those two are OUT, not tuned.
SCOPE = ('SkirmishChinaNukeGeneral',)
PARAMS = ('barracks_n', 'warfactory_n')
UNUSED = {'barracks_n': 'the only Barracks it held ("China Barracks 2 - H", Infantry) is out of scope since rb-data-7; the Nuke general has no Barracks hold',
          'dozers': 'stock Hard already builds two dozers; no scb action removes one (the 3rd comes only from the 50000-credit gift-era scripts)',
          'early_bunker': 'stock "China Build 1st Bunker - H" already builds a front bunker right after the first Gattling Cannon',
          'outpost_loaded': 'no stock script builds a Listening Outpost (a team unit): a new team and load behaviour, not a re-timing',
          'helix_delay_esc': "the Helix gate is OIL.COUNT_GATES' (it waits on the Capture research, not on _ESCALATION)"}
NK, = SCOPE
WF2 = {NK: 'China War Factory 2 - H'}
BX2 = {}                                      # "China Barracks 2 - H" (Infantry) is out of scope
SUPPLY_SCRIPT = 'China Supply Center - H'
FLOOR = 36                                    # _GX_CLOCK ticks (360 s): the gate always opens
CLAIMS = {'scripts': [[NK, WF2[NK]]]}
DEFAULTS = {'barracks_n': 1, 'warfactory_n': 1}


def _supply(scb, owner):
    _g, _k, s = rb.find_script(scb, owner, SUPPLY_SCRIPT)
    hits = [v for l in rb.leaves(s) for n, ps in [rb.decode(scb, l)] if n == 'AI_PLAYER_BUILD_SUPPLY_CENTER' for t, v in ps if t == 'OBJECT_TYPE']
    assert len(hits) == 1 and hits[0].endswith('SupplyCenter'), (owner, hits)
    return hits[0]


def _hold(scb, owner, name):
    _g, _k, s = rb.find_script(scb, owner, name)
    ors = [k for k in s.kids if k.name == 'OrCondition']
    assert len(ors) == 1 and len(ors[0].kids) == 1, (owner, name, ors)
    g = ors[0].kids[0]
    new = [mss.Node('OrCondition', ors[0].ver, kids=[mss.deep_copy(g), rb.own_count(scb, rb.GE, 2, _supply(scb, owner))]),
           mss.Node('OrCondition', ors[0].ver, kids=[mss.deep_copy(g), rb.clock_at_least(scb, FLOOR)])]
    i = s.kids.index(ors[0]); s.kids[i:i + 1] = new


def apply(scb, scope, p):
    for o in scope:
        if p['warfactory_n'] == 1 and o in WF2:
            _hold(scb, o, WF2[o])
        if p['barracks_n'] == 1 and o in BX2:
            _hold(scb, o, BX2[o])


def _production(obs, me, at):
    hz = ob.hz(obs)
    return ob.built_by(obs, me, ob.ends_with('WarFactory', 'Barracks'), at * hz)


def activation(obs, me, params=None):
    """triggered: an own War Factory complete by 200 s and the game ran past 300 s; fired: no more than 2 own production buildings (War
    Factories + Barracks) at 300 s (stock Hard has 3: Barracks + 2 War Factories, or 2 Barracks + 1 War Factory)"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    fw = ob.first_frame(obs, me, ob.ends_with('WarFactory'), 1, complete=True)
    end = ob.last_frame(obs)
    triggered = fw is not None and fw <= 200 * hz and end is not None and end >= 300 * hz
    n = _production(obs, me, 300)
    return {'triggered': bool(triggered), 'fired': bool(triggered and n is not None and n <= 2),
            'detail': 'warfactory_f=%s production_at_300s=%s' % (fw, n)}


# the stock arm builds the extra factory/barracks by itself, so the measure is how many production buildings were built by 300 s (a lost one
# still counts; Codex rbd6-r1); fewer is the rule
CONTRAST = {'what': 'own War Factories + Barracks at 300 s', 'direction': -1, 'min_delta': 1}


def measure(obs, me, params=None):
    return _production(obs, me, 300)
