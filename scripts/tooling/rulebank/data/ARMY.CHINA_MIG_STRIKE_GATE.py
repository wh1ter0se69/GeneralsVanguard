# GeneralsX: rule bank DATA rule ARMY.CHINA_MIG_STRIKE_GATE: China Infantry launches its MiG
# raid only at a set MiG count. Stock Infantry Hard's air raid teams "3RD Wave Air. 1" (0-4 MiGs) and "4TH Wave Air B. 1" (0-2) have a
# MiG minimum of 0, so a raid can leave with whatever is ready. Here each team's minimum becomes min(mig_strike_count, its maximum): the
# raid forms only once that many MiGs exist (one Air Field parks 4, so the default 5 means the full 4 and 2). Team counts only; the
# wave gates stay stock. Infantry's own copy of ARMY.MIG_PAIRS' idea, at raid size.
RULE_ID = 'ARMY.CHINA_MIG_STRIKE_GATE'
PARAMS = ('mig_strike_count',)
IG, MG = 'SkirmishChinaInfantryGeneral', 'Infa_ChinaJetMIG'
TEAMS = ('China H 3RD Wave Air. 1', 'China H 4TH Wave Air B. 1')
CLAIMS = {'teams': sorted(TEAMS)}
UNTIL, OUT = 400, 1000


def apply(scb, scope, p):
    if IG not in scope:
        return
    ts = rb.teams(scb)
    for n in TEAMS:
        t = rb.team(ts, n)
        assert t is not None and rb.team_get(t, 'teamOwner') == IG and rb.team_get(t, 'teamUnitType1') == MG, n
        mx = rb.team_get(t, 'teamUnitMaxCount1')
        rb.team_set(scb, t, 'teamUnitMinCount1', max(rb.team_get(t, 'teamUnitMinCount1'), min(p['mig_strike_count'], mx)))
    rb.put_teams(scb, ts)


def _biggest(obs, me):
    """the most own MiGs out at once by 400 s - farther than 1000 from the own Command Center (Codex rbd10-r1 P2: first sightings are
    production times, not raids); None when the game ended earlier or no Command Center was seen"""
    if not obs:
        return None
    hz, nm = ob.hz(obs), ob.names(obs)
    end = ob.last_frame(obs)
    if end is None or end < UNTIL * hz:
        return None
    cc, best = None, 0
    for r in obs:
        if r.get('t') != 'obs' or r['f'] > UNTIL * hz:
            continue
        mine = [o for o in r.get('o', []) if o[1] == me]
        if cc is None:
            cc = next(((o[3], o[4]) for o in mine if nm.get(o[2], '').endswith('ChinaCommandCenter')), None)
        if cc is not None:
            best = max(best, sum(1 for o in mine if nm.get(o[2], '').endswith('ChinaJetMIG') and (o[3] - cc[0]) ** 2 + (o[4] - cc[1]) ** 2 > OUT ** 2))
    return best if cc is not None else None


def activation(obs, me, params=None):
    """triggered: own Air Field complete by 300 s and the game ran past 400 s; fired: 4 own MiGs out at once by 400 s"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    fa = ob.first_frame(obs, me, ob.ends_with('ChinaAirfield'), 1, complete=True)
    n = _biggest(obs, me)
    triggered = fa is not None and fa <= 300 * hz and n is not None
    return {'triggered': bool(triggered), 'fired': bool(triggered and n >= 4), 'detail': 'airfield_f=%s biggest_mig_group=%s' % (fa, n)}


CONTRAST = {'what': 'most own MiGs out at once by 400 s (farther than 1000 from the own Command Center)', 'direction': 1, 'min_delta': 1}


def measure(obs, me, params=None):
    return _biggest(obs, me)
