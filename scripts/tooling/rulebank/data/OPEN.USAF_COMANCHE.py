# GeneralsX: rule bank DATA rule OPEN.USAF_COMANCHE (C·rb-data-7): the Air Force General opens with Comanches. Stock
# Hard (owners.txt, 14d03e02): the first Air Field comes after the first Supply Center; the 1st-wave teams are jets (Stealth Rush, Raptor
# Supply Hunters, priority 100) and the first Comanche team, "USA H 2W Comanche Guards. 1" (2 Comanches, priority 110, so it already outranks
# the jets), waits for the 2nd attack wave (_USA_2ND_WAVE_H). Here that team is produced the moment the first Air Field is built: a new
# sub-routine gate "GX USAF Comanche First - H" (own Air Field complete) carries the stock gate's guard-the-perimeter action. Own state only,
# no enemy term, no gift; the 3rd/4th-wave Comanche teams and every jet team are untouched.
RULE_ID = 'OPEN.USAF_COMANCHE'
SCOPE = ('SkirmishAmericaAirForceGeneral',)
PARAMS = ('comanche_first',)
AF = SCOPE[0]
TEAM, STOCK_GATE, GATE, GROUP = 'USA H 2W Comanche Guards. 1', '_USA_2ND_WAVE_H', 'GX USAF Comanche First - H', 'GX OPEN.USAF_COMANCHE'
AIRFIELD = 'AirF_AmericaAirfield'
CLAIMS = {'scripts': [[AF, GATE]], 'groups': [[AF, GROUP]], 'teams': [TEAM]}
DEFAULTS = {'comanche_first': 1}


def apply(scb, scope, p):
    if p['comanche_first'] != 1:
        return
    _g, _k, s0 = rb.find_script(scb, AF, STOCK_GATE)                 # the stock production-condition shape: its team action, our gate
    acts = [mss.deep_copy(k) for k in s0.kids if k.name == 'ScriptAction']
    gate = rb.new_script(scb, GATE, [[rb.cond(scb, 'BUILT_BY_PLAYER', ('OBJECT_TYPE', AIRFIELD), ('SIDE', rb.SIDE_ME))]], acts, oneshot=False, sub=True)
    rb.owner_list(scb, AF).kids.append(rb.new_group(GROUP, [gate]))
    ts = rb.teams(scb)
    t = rb.team(ts, TEAM)
    assert t is not None and rb.team_get(t, 'teamOwner') == AF and rb.team_get(t, 'teamProductionCondition') == STOCK_GATE, TEAM
    rb.team_set(scb, t, 'teamProductionCondition', GATE)
    rb.put_teams(scb, ts)


JETS = ('JetRaptor', 'JetStealthFighter', 'JetAurora')


def _lead(obs, me):
    """seconds the first Comanche was seen before the first jet; None when either never appeared"""
    if not obs:
        return None
    hz = ob.hz(obs)
    fc = ob.first_frame(obs, me, ob.ends_with('VehicleComanche'), 1)
    fj = ob.first_frame(obs, me, ob.ends_with(*JETS), 1)
    return None if fc is None or fj is None else (fj - fc) // hz


def activation(obs, me, params=None):
    """triggered: an own Comanche and an own jet both appear; fired: the first Comanche came before the first jet (stock Hard builds the
    1st-wave jets first)"""
    lead = _lead(obs, me)
    return {'triggered': lead is not None, 'fired': lead is not None and lead > 0, 'detail': 'comanche_leads_jet_s=%s' % lead}


CONTRAST = {'what': 'seconds the first Comanche is seen before the first jet', 'direction': 1, 'min_delta': 5}


def measure(obs, me, params=None):
    return _lead(obs, me)
