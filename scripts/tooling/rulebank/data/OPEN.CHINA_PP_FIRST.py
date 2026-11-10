# GeneralsX: rule bank DATA rule OPEN.CHINA_PP_FIRST (C·rb-data-8): every China general builds its Power Plant before
# its Barracks (the OPEN.USA_PP_FIRST pattern). Stock Hard: "China Barracks - H" is CONDITION_TRUE (the Barracks is the first order),
# "China 1st Power Plant - H" waits for the second Dozer. Here the Barracks waits until an own Power Plant stands (any build state), or
# _GX_CLOCK >= 12 (120 s, the floor so the Barracks always comes). Own state only, no enemy term, no gift.
RULE_ID = 'OPEN.CHINA_PP_FIRST'
SCOPE = ('SkirmishChina', 'SkirmishChinaTankGeneral', 'SkirmishChinaNukeGeneral', 'SkirmishChinaInfantryGeneral')
PARAMS = ('pp_first_enum',)
BARRACKS = 'China Barracks - H'
PP = {'SkirmishChina': 'ChinaPowerPlant', 'SkirmishChinaTankGeneral': 'Tank_ChinaPowerPlant',
      'SkirmishChinaNukeGeneral': 'Nuke_ChinaPowerPlant', 'SkirmishChinaInfantryGeneral': 'Infa_ChinaPowerPlant'}
FLOOR = 12                                    # _GX_CLOCK ticks (120 s)
CLAIMS = {'scripts': [[o, BARRACKS] for o in SCOPE]}
DEFAULTS = {'pp_first_enum': 1}


def apply(scb, scope, p):
    if p['pp_first_enum'] != 1:
        return
    for o in scope:
        _g, _k, s = rb.find_script(scb, o, BARRACKS)
        ors = [k for k in s.kids if k.name == 'OrCondition']
        assert len(ors) == 1 and [rb.decode(scb, l)[0] for l in ors[0].kids] == ['CONDITION_TRUE'], (o, ors)
        new = [mss.Node('OrCondition', ors[0].ver, kids=[rb.own_count(scb, rb.GE, 1, PP[o])]),
               mss.Node('OrCondition', ors[0].ver, kids=[rb.clock_at_least(scb, FLOOR)])]
        i = s.kids.index(ors[0]); s.kids[i:i + 1] = new


def _lead(obs, me):
    """seconds the first Power Plant (any build state) was seen before the first Barracks; None when either never appeared"""
    if not obs:
        return None
    hz = ob.hz(obs)
    fp = ob.first_frame(obs, me, ob.ends_with('ChinaPowerPlant'), 1)
    fb = ob.first_frame(obs, me, ob.ends_with('ChinaBarracks'), 1)
    return None if fp is None or fb is None else (fb - fp) // hz


def activation(obs, me, params=None):
    """triggered: both buildings appear in the game; fired: the Power Plant was seen before the Barracks (stock Hard starts the Barracks first)"""
    lead = _lead(obs, me)
    return {'triggered': lead is not None, 'fired': lead is not None and lead > 0, 'detail': 'power_plant_leads_barracks_s=%s' % lead}


CONTRAST = {'what': 'seconds the Power Plant is seen before the Barracks', 'direction': 1, 'min_delta': 5}


def measure(obs, me, params=None):
    return _lead(obs, me)
