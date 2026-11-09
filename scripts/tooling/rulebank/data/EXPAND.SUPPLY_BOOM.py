# GeneralsX @feature Claude 01/11/2026 rule bank DATA rule EXPAND.SUPPLY_BOOM (C·rb-data-10): an early supply-centre boom on the own clock.
# Stock Hard expands through the gift-carrying "Base Expansion Check" (an ENGINE hook: EXPAND.TIMING_COND); this rule leaves it alone and
# adds, per general, one ONE-SHOT script per centre number n, "GX Supply Boom <n> - H", that orders one more supply centre / stash the way
# the stock expansion does (AI_PLAYER_BUILD_SUPPLY_CENTER on a pile of 5000+ - ENGINE_READS, listed in the manifest) while the player owns
# fewer than n: from _GX_CLOCK 30 (300 s) for n <= supply_count_t300, else from 48 (480 s) for n <= supply_count_t480; the 4th also from
# ceil(supply4_t / 10) while the player owns at most oil_max oil derricks (no oils: the 4th comes fast). One order per number, so an
# unfinished centre (the count does not see it) is never ordered twice (Codex rbd10-r1 P2: a repeating order did).
# Own counts and the own clock only (NO_ENEMY_OBJECTS).
RULE_ID = 'EXPAND.SUPPLY_BOOM'
PARAMS = ('supply_count_t300', 'supply_count_t480', 'supply4_t', 'oil_max')
SC = {'SkirmishAmerica': 'AmericaSupplyCenter', 'SkirmishAmericaAirForceGeneral': 'AirF_AmericaSupplyCenter',
      'SkirmishAmericaLaserGeneral': 'Lazr_AmericaSupplyCenter', 'SkirmishAmericaSuperWeaponGeneral': 'SupW_AmericaSupplyCenter',
      'SkirmishChina': 'ChinaSupplyCenter', 'SkirmishChinaTankGeneral': 'Tank_ChinaSupplyCenter', 'SkirmishChinaNukeGeneral': 'Nuke_ChinaSupplyCenter',
      'SkirmishChinaInfantryGeneral': 'Infa_ChinaSupplyCenter', 'SkirmishGLA': 'GLASupplyStash', 'SkirmishGLADemolitionGeneral': 'Demo_GLASupplyStash',
      'SkirmishGLAToxinGeneral': 'Chem_GLASupplyStash', 'SkirmishGLAStealthGeneral': 'Slth_GLASupplyStash'}
NAME, GROUP, DK, PILE, NMAX = 'GX Supply Boom %d - H', 'GX EXPAND.SUPPLY_BOOM', 'TechOilDerrick', 5000, 5
CLAIMS = {'scripts': [[o, NAME % n] for o in SC for n in range(2, NMAX + 1)], 'groups': [[o, GROUP] for o in SC]}
UNTIL = 400
IS_SC = lambda n: n.endswith(('SupplyCenter', 'SupplyStash'))


def apply(scb, scope, p):
    t300, t480, s4 = p['supply_count_t300'], p['supply_count_t480'], -(-p['supply4_t'] // 10)
    for o in scope:
        t, me = SC[o], ('SIDE', rb.SIDE_ME)
        kids = []
        for n in range(2, max(t300, t480, 4) + 1):
            blocks = []
            if n <= t300:
                blocks.append([rb.clock_at_least(scb, 30), rb.own_count(scb, rb.LT, n, t)])
            elif n <= t480:
                blocks.append([rb.clock_at_least(scb, 48), rb.own_count(scb, rb.LT, n, t)])
            if n == 4:
                blocks.append([rb.clock_at_least(scb, s4), rb.own_count(scb, rb.LE, p['oil_max'], DK), rb.own_count(scb, rb.LT, 4, t)])
            if blocks:
                kids.append(rb.new_script(scb, NAME % n, blocks, [rb.act(scb, 'AI_PLAYER_BUILD_SUPPLY_CENTER', me, ('OBJECT_TYPE', t), ('INT', PILE))],
                                          oneshot=True))
        rb.owner_list(scb, o).kids.append(rb.new_group(GROUP, kids))


def _centres(obs, me):
    return ob.built_by(obs, me, IS_SC, UNTIL * ob.hz(obs)) if obs else None


def activation(obs, me, params=None):
    """triggered: an own supply centre by 120 s and the game ran past 400 s; fired: 3 own supply centres by 400 s"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    f1 = ob.first_frame(obs, me, IS_SC)
    n = _centres(obs, me)
    triggered = f1 is not None and f1 <= 120 * hz and n is not None
    return {'triggered': bool(triggered), 'fired': bool(triggered and n >= 3), 'detail': 'first_centre_f=%s centres_by_400s=%s' % (f1, n)}


CONTRAST = {'what': 'own supply centres built by 400 s', 'direction': 1, 'min_delta': 1}


def measure(obs, me, params=None):
    return _centres(obs, me)
