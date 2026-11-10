# GeneralsX: rule bank DATA rule NOFLOAT.AIRFIELD_BY_RAPTORS: Air Force builds its next Air Field
# only once its Raptors fill the ones it has. Stock Air Force Hard builds four fast ("USA Air Field - H" at the first supply centre, then
# "Build Second / Third / Fourth Air Field - H" at 1000 cash and one Air Field fewer each; smoke8: Air Fields 2 and 3 at 98 and 139 s,
# the 4th Raptor at 218-231 s). Here Air Field k (k = 2..4) also waits for raptors_per_airfield * (k - 1) own Raptors - or, at the latest,
# _GX_CLOCK FALLBACK[k] (a field parks 4 jets and Stealth Fighters, from 125-194 s in smoke8, share them, so the Raptors
# alone may never reach the count; the box ends at 4 for the same reason). Own state and clock only; the three scripts are clean. Left stock: "USA Air Field RICH1-4" (cash >= 15000 and _ESCALATION: enemy-gated, so not editable) still
# add Air Fields in a rich game. Claims the same scripts as NOFLOAT.EXTRA_FACTORIES: never both on for one player (the compiler refuses).
RULE_ID = 'NOFLOAT.AIRFIELD_BY_RAPTORS'
SCOPE = ('SkirmishAmericaAirForceGeneral',)
PARAMS = ('raptors_per_airfield',)
AFG, AFLD, RAP = 'SkirmishAmericaAirForceGeneral', 'AirF_AmericaAirfield', 'AirF_AmericaJetRaptor'
NEXT = {2: 'USA Build Second Air Field - H', 3: 'USA Build Third Air Field - H', 4: 'USA Build Fourth Air Field - H'}
FALLBACK = {2: 300, 3: 360, 4: 420}           # seconds: stock builds them at ~98, ~139 and ~290 s (smoke8)
CLAIMS = {'scripts': [[AFG, n] for n in NEXT.values()]}
UNTIL = 400


def apply(scb, scope, p):
    if AFG not in scope:
        return
    for k, n in NEXT.items():
        _g, _k, s = rb.find_script(scb, AFG, n)
        ors = [x for x in s.kids if x.name == 'OrCondition']
        assert len(ors) == 1, n                      # own state only: cash and the Air Field count
        late = mss.Node('OrCondition', ors[0].ver, kids=[mss.deep_copy(x) for x in ors[0].kids] + [rb.clock_at_least(scb, FALLBACK[k] // 10)])
        ors[0].kids.append(rb.own_count(scb, rb.GE, p['raptors_per_airfield'] * (k - 1), RAP))
        s.kids.insert(s.kids.index(ors[0]) + 1, late)


def _second(obs, me):
    """frame of the 2nd own Air Field, UNTIL s when none came by then; None if the game ended earlier"""
    hz, end = ob.hz(obs), ob.last_frame(obs)
    if end is None or end < UNTIL * hz:
        return None
    f = ob.first_frame(obs, me, ob.ends_with('AmericaAirfield'), 2)
    return f if f is not None and f < UNTIL * hz else UNTIL * hz


def activation(obs, me, params=None):
    """triggered: an own Air Field by 120 s and the game ran past 400 s; fired: no 2nd Air Field before the raptors_per_airfield-th own
    Raptor (stock builds it at ~98 s, before any Raptor)"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz, n = ob.hz(obs), (params or {}).get('raptors_per_airfield', 4)
    f1 = ob.first_frame(obs, me, ob.ends_with('AmericaAirfield'))
    f2, fr = _second(obs, me), ob.first_frame(obs, me, ob.ends_with('AmericaJetRaptor'), n)
    triggered = f1 is not None and f1 <= 120 * hz and f2 is not None
    fired = triggered and (f2 >= UNTIL * hz or (fr is not None and f2 >= fr))
    return {'triggered': bool(triggered), 'fired': bool(fired), 'detail': 'airfield_f=%s second_airfield_f=%s raptor%d_f=%s' % (f1, f2, n, fr)}


CONTRAST = {'what': 'seconds to the 2nd own Air Field (400 = none by 400 s)', 'direction': 1, 'min_delta': 30}


def measure(obs, me, params=None):
    f = _second(obs, me) if obs else None
    return None if f is None else f // ob.hz(obs)
