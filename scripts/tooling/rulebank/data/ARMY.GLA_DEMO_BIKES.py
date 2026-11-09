# GeneralsX @feature Claude 01/11/2026 rule bank DATA rule ARMY.GLA_DEMO_BIKES (C·rb-data-10): Demolition GLA mid game rides Combat
# Bikes with its tanks. Stock Demolition Hard fields bikes only in wave 5 ("5TH Wave Bikes F/C/B"); its 2nd- and 3rd-wave Scorpion
# teams ("2ND Wave Tanks. 1", "3RD Wave Tanks F. 1", "3RD Wave Tanks C. 1": 0-8 Scorpions, slot 2 empty) never do. Here each gains
# [Combat Bike, b, b] in slot 2, b = bike_share_pm of the team's 8 (rounded). The bike needs only the Demolition Arms Dealer (INIZH),
# which the team's Scorpions need anyway. Team counts only. bike_share_pm = 0 is stock.
RULE_ID = 'ARMY.GLA_DEMO_BIKES'
PARAMS = ('bike_share_pm',)
UNUSED = {'trap_n': 'demo traps are structures a Worker places at a spot; no DATA action places one'}
DG, B, SC = 'SkirmishGLADemolitionGeneral', 'Demo_GLAVehicleCombatBike', 'Demo_GLATankScorpion'
TEAMS = ('GLA H 2ND Wave Tanks. 1', 'GLA H 3RD Wave Tanks F. 1', 'GLA H 3RD Wave Tanks C. 1')
CLAIMS = {'teams': sorted(TEAMS)}
TEAM_UNIT_WAIVERS = {B: 'INIZH: Demolition Arms Dealer only, which the Scorpions of these teams need anyway'}
UNTIL = 400


def apply(scb, scope, p):
    if DG not in scope:
        return
    ts = rb.teams(scb)
    for n in TEAMS:
        t = rb.team(ts, n)
        assert t is not None and rb.team_get(t, 'teamOwner') == DG and rb.team_get(t, 'teamUnitType1') == SC, n
        assert rb.team_get(t, 'teamUnitMaxCount2') == 0, n
        b = (rb.team_get(t, 'teamUnitMaxCount1') * p['bike_share_pm'] + 500) // 1000
        if b:
            for k, v in (('teamUnitType2', B), ('teamUnitMinCount2', b), ('teamUnitMaxCount2', b)):
                rb.team_set(scb, t, k, v)
    rb.put_teams(scb, ts)


def _bikes(obs, me):
    return ob.built_by(obs, me, lambda n: 'GLAVehicleCombatBike' in n, UNTIL * ob.hz(obs)) if obs else None


def activation(obs, me, params=None):
    """triggered: own Arms Dealer complete by 150 s and the game ran past 400 s; fired: an own Combat Bike by 400 s (stock's first is in
    wave 5)"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    fa = ob.first_frame(obs, me, ob.ends_with('GLAArmsDealer'), 1, complete=True)
    n = _bikes(obs, me)
    triggered = fa is not None and fa <= 150 * hz and n is not None
    return {'triggered': bool(triggered), 'fired': bool(triggered and n >= 1), 'detail': 'armsdealer_f=%s bikes_by_400s=%s' % (fa, n)}


CONTRAST = {'what': 'own Combat Bikes built by 400 s', 'direction': 1, 'min_delta': 1}


def measure(obs, me, params=None):
    return _bikes(obs, me)
