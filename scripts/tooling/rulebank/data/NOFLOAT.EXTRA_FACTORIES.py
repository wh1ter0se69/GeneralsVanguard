# GeneralsX: rule bank DATA rule NOFLOAT.EXTRA_FACTORIES: Air Force spends spare money on War
# Factories, not on more Air Fields. Stock Air Force Hard builds up to four Air Fields fast ("USA Air Field - H" at the first supply
# centre, then "Build Second / Third / Fourth Air Field - H" at 1000 cash each) and one War Factory (the High Money copy waits for 30000).
# Here the Air Field scripts past airfields_max get the never-true condition "fewer than 0 own Air Fields", and one-shot scripts
# "GX Extra War Factory <n> - H", n = 2..1 + wf_extra, each place War Factory n the way the stock RICH scripts place an extra building
# (AI_PLAYER_BUILD_SUPPLY_CENTER, minimum 1000 - ENGINE_READS, listed in the manifest) once the first is complete, cash >= bank_min,
# own oil derricks >= oil_min and the player owns fewer than n (one order per number: a repeating order over-built). Own state only. Air Force only: stock China Tank Hard already builds
# three War Factories ("China War Factory" 1-3) and one Air Field, which is this rule's default.
# rb-data-11: under the cap stock still placed one more Air Field (smoke8: 243 s, cash 17000): "USA Air Field RICH<j> AirF_AmericaAirfield"
# (cash >= 15000/30000/50000/80000 and _ESCALATION >= 10/20/30/45: enemy-gated, not editable). RICH<j> is field 4 + j, past every
# airfields_max in the box, so the one-shot "GX Stock RICH Air Fields Off - H" DISABLEs all four at the start.
RULE_ID = 'NOFLOAT.EXTRA_FACTORIES'
SCOPE = ('SkirmishAmericaAirForceGeneral',)
PARAMS = ('wf_extra', 'airfields_max', 'bank_min', 'oil_min')
AFG, AFLD, WF, DK = 'SkirmishAmericaAirForceGeneral', 'AirF_AmericaAirfield', 'AirF_AmericaWarFactory', 'TechOilDerrick'
FIELDS = ('USA Air Field - H', 'USA Build Second Air Field - H', 'USA Build Third Air Field - H', 'USA Build Fourth Air Field - H')
EXTRA, GROUP, WFMAX = 'GX Extra War Factory %d - H', 'GX NOFLOAT.EXTRA_FACTORIES', 5
RICH, OFF = tuple('USA Air Field RICH%d AirF_AmericaAirfield' % j for j in (1, 2, 3, 4)), 'GX Stock RICH Air Fields Off - H'
CLAIMS = {'scripts': [[AFG, n] for n in FIELDS + RICH + (OFF,)] + [[AFG, EXTRA % n] for n in range(2, WFMAX + 1)], 'groups': [[AFG, GROUP]]}
UNTIL = 400


def apply(scb, scope, p):
    if AFG not in scope:
        return
    for k, n in enumerate(FIELDS, 1):
        if k <= p['airfields_max']:
            continue
        _g, _k, s = rb.find_script(scb, AFG, n)
        ors = [x for x in s.kids if x.name == 'OrCondition']
        assert len(ors) == 1, n                      # own state only: the first supply centre / cash and the Air Field count
        ors[0].kids = [rb.own_count(scb, rb.LT, 0, AFLD)]
    me = ('SIDE', rb.SIDE_ME)
    kids = [rb.new_script(scb, EXTRA % n, [[rb.cond(scb, 'BUILT_BY_PLAYER', ('OBJECT_TYPE', WF), me), rb.credits_at_least(scb, p['bank_min']),
                                           rb.own_count(scb, rb.GE, p['oil_min'], DK), rb.own_count(scb, rb.LT, n, WF)]],
                          [rb.act(scb, 'AI_PLAYER_BUILD_SUPPLY_CENTER', me, ('OBJECT_TYPE', WF), ('INT', 1000))], oneshot=True)
            for n in range(2, 2 + p['wf_extra'])]
    kids.append(rb.new_script(scb, OFF, [[rb.cond(scb, 'CONDITION_TRUE')]], [rb.act(scb, 'DISABLE_SCRIPT', ('SCRIPT', n)) for n in RICH], oneshot=True))
    rb.owner_list(scb, AFG).kids.append(rb.new_group(GROUP, kids))


def _fields(obs, me):
    return ob.built_by(obs, me, ob.ends_with('AmericaAirfield'), UNTIL * ob.hz(obs)) if obs else None


def activation(obs, me, params=None):
    """triggered: an own Air Field by 240 s and the game ran past 400 s; fired: no more than 1 (the default cap) by 400 s"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    fa = ob.first_frame(obs, me, ob.ends_with('AmericaAirfield'))
    n = _fields(obs, me)
    triggered = fa is not None and fa <= 240 * hz and n is not None
    return {'triggered': bool(triggered), 'fired': bool(triggered and n <= 1), 'detail': 'airfield_f=%s airfields_by_400s=%s' % (fa, n)}


CONTRAST = {'what': 'own Air Fields built by 400 s', 'direction': -1, 'min_delta': 1}


def measure(obs, me, params=None):
    return _fields(obs, me)
