# GeneralsX: rule bank DATA rule OPEN.USA_PP_FIRST: vanilla USA builds its Power Plant before its Barracks.
# Stock Hard (owners.txt, 14d03e02): "USA Barracks - H" is CONDITION_TRUE (the Barracks is the very first order), "USA 1st Power Plant - H" waits
# for the second Dozer, "USA Supply Center - H" for the Power Plant. Here the Barracks waits until an own Power Plant stands (any build state:
# the dozer has started it), or _GX_CLOCK >= 12 (120 s, the floor so the Barracks always comes). Own state only, no enemy term, no gift; the
# Barracks RICH1-4 / High Money Barracks scripts are untouched, so this defers the first Barracks and never caps them.
RULE_ID = 'OPEN.USA_PP_FIRST'
SCOPE = ('SkirmishAmerica',)
PARAMS = ('power_first',)
OWNER, BARRACKS, PP = 'SkirmishAmerica', 'USA Barracks - H', 'AmericaPowerPlant'
FLOOR = 12                                    # _GX_CLOCK ticks (120 s)
CLAIMS = {'scripts': [[OWNER, BARRACKS]]}
DEFAULTS = {'power_first': 1}


def apply(scb, scope, p):
    if p['power_first'] != 1:
        return
    for o in scope:
        _g, _k, s = rb.find_script(scb, o, BARRACKS)
        ors = [k for k in s.kids if k.name == 'OrCondition']
        assert len(ors) == 1 and [rb.decode(scb, l)[0] for l in ors[0].kids] == ['CONDITION_TRUE'], (o, ors)
        new = [mss.Node('OrCondition', ors[0].ver, kids=[rb.own_count(scb, rb.GE, 1, PP)]),
               mss.Node('OrCondition', ors[0].ver, kids=[rb.clock_at_least(scb, FLOOR)])]
        i = s.kids.index(ors[0]); s.kids[i:i + 1] = new


def _lead(obs, me):
    """seconds the first Power Plant (any build state) was seen before the first Barracks; None when either never appeared"""
    if not obs:
        return None
    hz = ob.hz(obs)
    fp = ob.first_frame(obs, me, ob.ends_with(PP), 1)
    fb = ob.first_frame(obs, me, ob.ends_with('AmericaBarracks'), 1)
    return None if fp is None or fb is None else (fb - fp) // hz


def activation(obs, me, params=None):
    """triggered: both buildings appear in the game; fired: the Power Plant was seen before the Barracks (stock Hard starts the Barracks
    first, at once)"""
    lead = _lead(obs, me)
    return {'triggered': lead is not None, 'fired': lead is not None and lead > 0, 'detail': 'power_plant_leads_barracks_s=%s' % lead}


CONTRAST = {'what': 'seconds the Power Plant is seen before the Barracks', 'direction': 1, 'min_delta': 5}


def measure(obs, me, params=None):
    return _lead(obs, me)
