# GeneralsX: rule bank DATA rule ARMY.GLA_EARLY_QUAD_MASS (C·rb-data-10): Stealth GLA masses Quad Cannons
# very early. Stock Stealth Hard's first wave holds 4 Quads: "1ST Wave Supply Hunters. 2" and "1W Guard Combat Zone. 2", 2 each. Here the
# two teams share early_quad_count between them (the Supply Hunters take the odd one), minimum = maximum, so wave 1 waits for the whole
# mass. Team counts only: the Quad is the team's own unit (Arms Dealer), the wave gate stays stock. early_quad_count = 4 is stock.
RULE_ID = 'ARMY.GLA_EARLY_QUAD_MASS'
PARAMS = ('early_quad_count',)
UNUSED = {'early_quad_t_max': 'the deadline the tip names is when stock wave 1 forms anyway; the rule moves only its size'}
SG, Q = 'SkirmishGLAStealthGeneral', 'Slth_GLAVehicleQuadCannon'
SH, GZ = 'GLA H 1ST Wave Supply Hunters. 2', 'GLA H 1W Guard Combat Zone. 2'
CLAIMS = {'teams': sorted((SH, GZ))}
UNTIL = 300


def apply(scb, scope, p):
    n = p['early_quad_count']
    if SG not in scope or n == 4:
        return
    ts = rb.teams(scb)
    for name, k in ((SH, n - n // 2), (GZ, n // 2)):
        t = rb.team(ts, name)
        assert t is not None and rb.team_get(t, 'teamOwner') == SG and rb.team_get(t, 'teamUnitType1') == Q, name
        rb.team_set(scb, t, 'teamUnitMinCount1', k)
        rb.team_set(scb, t, 'teamUnitMaxCount1', k)
    rb.put_teams(scb, ts)


def _quads(obs, me):
    return ob.built_by(obs, me, ob.ends_with('GLAVehicleQuadCannon'), UNTIL * ob.hz(obs)) if obs else None


def activation(obs, me, params=None):
    """triggered: own Arms Dealer complete by 150 s and the game ran past 300 s; fired: 8 own Quads (the default mass) by 300 s"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    fa = ob.first_frame(obs, me, ob.ends_with('GLAArmsDealer'), 1, complete=True)
    n = _quads(obs, me)
    triggered = fa is not None and fa <= 150 * hz and n is not None
    return {'triggered': bool(triggered), 'fired': bool(triggered and n >= 8), 'detail': 'armsdealer_f=%s quads_by_300s=%s' % (fa, n)}


CONTRAST = {'what': 'own Quad Cannons built by 300 s', 'direction': 1, 'min_delta': 2}


def measure(obs, me, params=None):
    return _quads(obs, me)
