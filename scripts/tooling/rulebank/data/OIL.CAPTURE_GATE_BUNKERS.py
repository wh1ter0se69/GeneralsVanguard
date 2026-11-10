# GeneralsX: rule bank DATA rule OIL.CAPTURE_GATE_BUNKERS (C·rb-data-11): GLA holds its oil capture until its
# defences stand and spends the opening on its army. Stock GLA Hard's capture team (one Rebel) is produced by "GLA Tech Capture Build"
# as soon as a tech building lies near its base (smoke8: first own derrick at 130-143 s, every general). Here that script's gate also
# waits for _GX_CLOCK >= ceil(cap_t / 10) and bunker_min own Tunnel Networks (GLA's garrisonable defence; 0 = no Tunnel gate). The stock
# gate leaves stay as they are, in the same script (the compiler accepts a tainted stock read only that way). Own state and clock only.
# The opposite start rule to OIL.CAPTURE_TEAMS, which edits the same script: the registry conflict keeps them apart (the tuner picks one).
RULE_ID = 'OIL.CAPTURE_GATE_BUNKERS'
SCOPE = ('SkirmishGLA', 'SkirmishGLADemolitionGeneral', 'SkirmishGLAToxinGeneral', 'SkirmishGLAStealthGeneral')
PARAMS = ('cap_t', 'bunker_min')
PRE = {'SkirmishGLA': '', 'SkirmishGLADemolitionGeneral': 'Demo_', 'SkirmishGLAToxinGeneral': 'Chem_', 'SkirmishGLAStealthGeneral': 'Slth_'}
BUILD, DERRICK = 'GLA Tech Capture Build', 'TechOilDerrick'
CLAIMS = {'scripts': [[o, BUILD] for o in SCOPE]}
UNTIL = 400


def apply(scb, scope, p):
    for o in scope:
        _g, _k, b = rb.find_script(scb, o, BUILD)
        ors = [k for k in b.kids if k.name == 'OrCondition']
        assert len(ors) == 1, '%s %s: %d OR blocks' % (o, BUILD, len(ors))
        ors[0].kids.append(rb.clock_at_least(scb, -(-p['cap_t'] // 10)))
        if p['bunker_min'] > 0:
            ors[0].kids.append(rb.own_count(scb, rb.GE, p['bunker_min'], PRE[o] + 'GLATunnelNetwork'))


def _until(params):
    """the reader's window in seconds: 400, or cap_t + 100 for a later cap_t (Codex rbd11-r1 P2)"""
    return max(UNTIL, (params or {}).get('cap_t', 300) + 100)


def _first(obs, me, params=None):
    """frame of the first own oil derrick, the window end when none came by then; None if the game ended earlier"""
    hz, end, w = ob.hz(obs), ob.last_frame(obs), _until(params)
    if end is None or end < w * hz:
        return None
    f = ob.first_frame(obs, me, lambda n: n == DERRICK)
    return f if f is not None and f < w * hz else w * hz


def activation(obs, me, params=None):
    """triggered: an own Tunnel Network by cap_t (none needed at bunker_min 0) and the game ran past the window; fired: no own oil derrick
    before cap_t"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz, t, need = ob.hz(obs), (params or {}).get('cap_t', 300), (params or {}).get('bunker_min', 1)
    ft, fd = ob.first_frame(obs, me, ob.ends_with('GLATunnelNetwork')), _first(obs, me, params)
    triggered = (need == 0 or (ft is not None and ft <= t * hz)) and fd is not None
    return {'triggered': bool(triggered), 'fired': bool(triggered and fd >= t * hz), 'detail': 'tunnel_f=%s first_derrick_f=%s' % (ft, fd)}


CONTRAST = {'what': 'seconds to the first own oil derrick (the window end, 400 s at the default, = none by then)', 'direction': 1, 'min_delta': 60}


def measure(obs, me, params=None):
    f = _first(obs, me, params) if obs else None
    return None if f is None else f // ob.hz(obs)
