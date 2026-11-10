# GeneralsX: rule bank DATA rule OPEN.GLA_TECH_ORDER: the partial Toxin "save for the Palace" lever
# (lead ruling rb-data-7 step 3), its own rule so it can be switched off alone. Stock Hard Toxin (owners.txt, 14d03e02) spends on defences
# the moment its Barracks / Arms Dealer stand: "GLA Build 1st Defense - H" (3 base defences), "GLA Build Tunnel Network 2 - H" and "GLA Supply
# Defense - H" (a Stinger Site at the supply pile). The Palace ("GLA Palace - H": Arms Dealer built AND _ESCALATION >= 30, a $3,000 gift
# script, frozen) comes at ~385-395 s. Here each of the three waits until the Palace is built, or my money reaches palace_before_ad2_cash,
# or _GX_CLOCK >= 54 (540 s, the floor so it always opens). Own state only, no enemy term, no gift, no stock script but these three edited.
# It is a DEFERRAL: no money is reserved (that is ENGINE) - Base Expander and wave teams still spend; the smoke must read whether the Palace
# comes sooner and whether the base is left open. NOT here: Arms Dealer 2 (two scripts named "GLA Arms Dealer 2 - H" share one parent group,
# and the compiler refuses to edit a doubled name).
RULE_ID = 'OPEN.GLA_TECH_ORDER'
SCOPE = ('SkirmishGLAToxinGeneral',)
PARAMS = ('palace_before_ad2_cash',)
UNUSED = {'black_market_max_early': 'Black Market 1-4 are $3,000 gift scripts waiting on the Palace: frozen, and nothing before the Palace builds one',
          'radar_van_min_esc': 'no stock Toxin script or team builds a Radar Van (none named in the Hard scripts): nothing to delay',
          'worker_shoes_first': 'INFERRED: "GLA Worker Shoes Upgrade" already fires on money > 1100 or a Black Market, whichever comes first'}
TX = SCOPE[0]
HOLD = ('GLA Build 1st Defense - H', 'GLA Build Tunnel Network 2 - H', 'GLA Supply Defense - H')
PALACE, FLOOR = 'Chem_GLAPalace', 54            # _GX_CLOCK ticks (540 s)
CLAIMS = {'scripts': [[TX, n] for n in HOLD]}
DEFAULTS = {'palace_before_ad2_cash': 2000}


def apply(scb, scope, p):
    me = ('SIDE', rb.SIDE_ME)
    for o in scope:
        for n in HOLD:
            _g, _k, s = rb.find_script(scb, o, n)
            ors = [k for k in s.kids if k.name == 'OrCondition']
            assert len(ors) == 1, (o, n, len(ors))
            b = ors[0]
            adds = (rb.cond(scb, 'BUILT_BY_PLAYER', ('OBJECT_TYPE', PALACE), me), rb.credits_at_least(scb, p['palace_before_ad2_cash']),
                    rb.clock_at_least(scb, FLOOR))
            new = [mss.Node('OrCondition', b.ver, kids=[mss.deep_copy(k) for k in b.kids] + [a]) for a in adds]
            i = s.kids.index(b); s.kids[i:i + 1] = new


def _defence(obs, me):
    """seconds the first own Palace is seen before the first own Stinger Site; None when either never appeared"""
    if not obs:
        return None
    hz = ob.hz(obs)
    fp = ob.first_frame(obs, me, ob.ends_with('GLAPalace'), 1)
    fd = ob.first_frame(obs, me, ob.ends_with('GLAStingerSite'), 1)
    return None if fp is None or fd is None else (fd - fp) // hz


def activation(obs, me, params=None):
    """triggered: an own Palace and an own Stinger Site both appear; fired: the Palace was seen before the first Stinger Site (stock Hard builds
    the base defences at ~100 s)"""
    lead = _defence(obs, me)
    return {'triggered': lead is not None, 'fired': lead is not None and lead > 0, 'detail': 'palace_leads_first_stinger_s=%s' % lead}


CONTRAST = {'what': 'seconds the Palace is seen before the first Stinger Site (a base defence)', 'direction': 1, 'min_delta': 5}


def measure(obs, me, params=None):
    return _defence(obs, me)
