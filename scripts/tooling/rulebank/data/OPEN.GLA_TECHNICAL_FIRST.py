# GeneralsX: rule bank DATA rule OPEN.GLA_TECHNICAL_FIRST: GLA's first raider pair is
# Technicals. Stock GLA Hard's "1ST Wave Supply Hunters" team is 2 Quads (vanilla, Stealth, Toxin) or 2 Scorpions (Demolition); stock's
# first Technical comes inside the 2nd-wave Fast Attack team. first_unit_enum = 0 (Technical) makes that team 2 of the general's own
# Technical; 1 (Quad) is stock. The Technical has no prerequisite (INIZH) and is built where the Quad was. Team type only; the wave gate
# stays stock. arms_dealers is UNUSED: stock already queues three Arms Dealers at the Supply Stash (TECH.GLA_ARMS_DEALER_CAP caps them).
RULE_ID = 'OPEN.GLA_TECHNICAL_FIRST'
PARAMS = ('first_unit_enum',)
UNUSED = {'arms_dealers': 'stock GLA Hard queues three Arms Dealers the moment its Supply Stash stands; TECH.GLA_ARMS_DEALER_CAP caps them'}
SH = {'SkirmishGLA': ('GLA H 1ST Wave Supply Hunters', ''), 'SkirmishGLADemolitionGeneral': ('GLA H 1ST Wave Supply Hunters. 1', 'Demo_'),
      'SkirmishGLAStealthGeneral': ('GLA H 1ST Wave Supply Hunters. 2', 'Slth_'),
      'SkirmishGLAToxinGeneral': ('GLA H 1ST Wave Supply Hunters. 3', 'Chem_')}
CLAIMS = {'teams': sorted(n for n, _p in SH.values())}
TEAM_UNIT_WAIVERS = {pre + 'GLAVehicleTechnical': 'INIZH: no prerequisite; built at the Arms Dealer the replaced Quad / Scorpion needed'
                     for _n, pre in SH.values()}
UNTIL = 400
TECH = lambda n: 'GLAVehicleTechnical' in n          # it spawns as ...TechnicalChassisOne/Two/Three (smoke 30/10)
COMBAT = lambda n: n.endswith(('GLAVehicleQuadCannon', 'GLATankScorpion')) or 'GLAVehicleTechnical' in n


def apply(scb, scope, p):
    if p['first_unit_enum'] != 0:
        return
    ts = rb.teams(scb)
    for o in scope:
        n, pre = SH[o]
        t = rb.team(ts, n)
        assert t is not None and rb.team_get(t, 'teamOwner') == o and rb.team_get(t, 'teamUnitMinCount1') == 2, n
        rb.team_set(scb, t, 'teamUnitType1', pre + 'GLAVehicleTechnical')
    rb.put_teams(scb, ts)


def _first_tech_s(obs, me):
    """seconds to the first own Technical, 400 when none came by 400 s; None when the game ended earlier"""
    if not obs:
        return None
    hz = ob.hz(obs)
    end = ob.last_frame(obs)
    if end is None or end < UNTIL * hz:
        return None
    f = ob.first_frame(obs, me, TECH)
    return UNTIL if f is None or f > UNTIL * hz else f // hz


def activation(obs, me, params=None):
    """triggered: own Arms Dealer complete by 150 s and the game ran past 400 s; fired: the first own combat vehicle (Quad, Scorpion,
    Technical) is a Technical"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    fa = ob.first_frame(obs, me, ob.ends_with('GLAArmsDealer'), 1, complete=True)
    ft, fc = ob.first_frame(obs, me, TECH), ob.first_frame(obs, me, COMBAT)
    s = _first_tech_s(obs, me)
    triggered = fa is not None and fa <= 150 * hz and s is not None
    return {'triggered': bool(triggered), 'fired': bool(triggered and ft is not None and ft == fc),
            'detail': 'armsdealer_f=%s first_technical_s=%s first_combat_f=%s' % (fa, s, fc)}


CONTRAST = {'what': 'seconds to the first own Technical (400 = none by 400 s)', 'direction': -1, 'min_delta': 20}


def measure(obs, me, params=None):
    return _first_tech_s(obs, me)
