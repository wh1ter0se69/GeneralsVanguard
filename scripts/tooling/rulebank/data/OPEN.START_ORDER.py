# GeneralsX: rule bank DATA rule OPEN.START_ORDER (C·rb-data-7): GLA opens with 5 workers. Stock Hard: the singleton
# team "GLA Worker" (and the Demolition / Stealth / Toxin copies "GLA Worker. 1/2/3") is 4 Workers min 4 max 4, built by "GLA Dozer - Build
# Condition" at the start. gla_workers_first sets that team's min and max (4 = stock). The expansion worker teams (1 Worker) are untouched.
# dozer_first is UNUSED: every first build order of every general needs a Dozer to exist, so a Dozer-first opening is already stock.
RULE_ID = 'OPEN.START_ORDER'
SCOPE = ('SkirmishGLA', 'SkirmishGLADemolitionGeneral', 'SkirmishGLAToxinGeneral', 'SkirmishGLAStealthGeneral')
PARAMS = ('gla_workers_first',)
UNUSED = {'dozer_first': 'INFERRED: every stock first build order needs a Dozer to exist (USA "1st Power Plant" even waits for the second), so Dozer first is stock'}
TEAMS = {'SkirmishGLA': 'GLA Worker', 'SkirmishGLADemolitionGeneral': 'GLA Worker. 1', 'SkirmishGLAStealthGeneral': 'GLA Worker. 2',
         'SkirmishGLAToxinGeneral': 'GLA Worker. 3'}
CLAIMS = {'teams': sorted(TEAMS.values())}
DEFAULTS = {'gla_workers_first': 5}


def apply(scb, scope, p):
    ts = rb.teams(scb)
    for o in scope:
        t = rb.team(ts, TEAMS[o])
        assert t is not None and rb.team_get(t, 'teamOwner') == o and rb.team_get(t, 'teamUnitType1').endswith('GLAInfantryWorker'), (o, TEAMS[o])
        rb.team_set(scb, t, 'teamUnitMinCount1', p['gla_workers_first'])
        rb.team_set(scb, t, 'teamUnitMaxCount1', p['gla_workers_first'])
    rb.put_teams(scb, ts)


def _workers(obs, me):
    """distinct own Workers seen by 120 s (the first 5 are built in the first minute or so); None when the game never reached 120 s"""
    if not obs:
        return None
    hz = ob.hz(obs)
    return ob.built_by(obs, me, ob.ends_with('GLAInfantryWorker'), 120 * hz)


def activation(obs, me, params=None):
    """triggered: the game ran past 120 s; fired: at least gla_workers_first own Workers by 120 s (stock Hard: 4)"""
    n = _workers(obs, me)
    want = dict(DEFAULTS, **(params or {}))['gla_workers_first']
    return {'triggered': n is not None, 'fired': n is not None and n >= want, 'detail': 'workers_by_120s=%s' % n}


CONTRAST = {'what': 'own GLA Workers built by 120 s', 'direction': 1, 'min_delta': 1}


def measure(obs, me, params=None):
    return _workers(obs, me)
