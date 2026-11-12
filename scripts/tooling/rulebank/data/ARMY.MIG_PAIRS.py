# Copyright 2026 GeneralsX contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# rule bank DATA rule ARMY.MIG_PAIRS: vanilla China keeps its MiGs in pairs. Stock Hard
# vanilla China's air waves ("3RD Wave Air" up to 4, "4TH Wave Air B" up to 2) and the 3rd/4th/5th-wave Guard Combat Zone teams (up to 2)
# all have a MiG minimum of 0, so a lone MiG is a whole team and flies alone. Here every such team's MiG minimum becomes
# min(mig_team_size, its own maximum): the team is not formed until a pair (or a team of that size) exists. Maximums, the 2nd-wave guard
# (already 2/2), Normal difficulty and the Nuke/Infantry generals' copies are untouched. The tip's "avoid Avengers" half is not scripted.
RULE_ID = 'ARMY.MIG_PAIRS'
PARAMS = ('mig_team_size',)
CN, MG, NONE = 'SkirmishChina', 'ChinaJetMIG', '<none>'
TEAMS = ('China H 3RD Wave Air', 'China H 4TH Wave Air B', 'China H 3W Guard Combat Zone', 'China H 4W Guard Combat Zone',
         'China H 5W Guard Combat Zone')
CLAIMS = {'teams': sorted(TEAMS)}


def apply(scb, scope, p):
    if CN not in scope:
        return
    ts = rb.teams(scb)
    for n in TEAMS:
        t = rb.team(ts, n)
        assert t is not None and rb.team_get(t, 'teamOwner') == CN, n
        for i in (1, 2, 3):
            if rb.team_get(t, 'teamUnitType%d' % i) == MG:
                mx = rb.team_get(t, 'teamUnitMaxCount%d' % i)
                rb.team_set(scb, t, 'teamUnitMinCount%d' % i, max(rb.team_get(t, 'teamUnitMinCount%d' % i), min(p['mig_team_size'], mx)))
    rb.put_teams(scb, ts)


def activation(obs, me, params=None):
    """triggered: own Air Field complete by 240 s and the game ran past 420 s; fired: at least 4 own MiGs by 420 s and none of them arrived
    alone (arrivals within 20 s of each other form a group; every group holds 2+). Stock arrivals often pair up by themselves (the
    30/10 smoke: 7 of 8 in pairs, one lone MiG at 302 s in one game), so an OFF arm that fires too is the expected common case -
    the rule only separates from stock in a game where stock fields a lone MiG: judge it on that, never on a PASS alone."""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    fa = ob.first_frame(obs, me, ob.ends_with('ChinaAirfield'), 1, complete=True)
    end = ob.last_frame(obs)
    triggered = fa is not None and fa <= 240 * hz and end is not None and end >= 420 * hz
    ts, n = [], 1
    while True:
        f = ob.first_frame(obs, me, ob.ends_with('ChinaJetMIG'), n)
        if f is None or f > 420 * hz:
            break
        ts.append(f); n += 1
    groups = []
    for f in ts:
        if groups and f - groups[-1][-1] <= 20 * hz:
            groups[-1].append(f)
        else:
            groups.append([f])
    fired = triggered and len(ts) >= 4 and all(len(g) >= 2 for g in groups)
    return {'triggered': bool(triggered), 'fired': bool(fired), 'detail': 'airfield_f=%s migs=%d groups=%s' % (fa, len(ts), [len(g) for g in groups])}


# The rule only removes LONE arrivals (stock pairs most MiGs by itself), so measure() counts them; fewer is better. A stock game
# with 0 has nothing to remove: smoke-read.py reports that arm as UNOBSERVABLE-IN-STOCK instead of calling the rule broken.
CONTRAST = {'what': 'own MiG arrivals by 420 s that formed no group of 2+ (arrivals within 20 s group)', 'direction': -1, 'min_delta': 1}


def measure(obs, me, params=None):
    hz = ob.hz(obs)
    end = ob.last_frame(obs)
    if end is None or end < 420 * hz:
        return None
    ts = ob.arrivals(obs, me, ob.ends_with('ChinaJetMIG'), 420 * hz)       # distinct objects: a MiG that died does not hide a later arrival
    groups = []
    for f in ts:
        if groups and f - groups[-1][-1] <= 20 * hz:
            groups[-1].append(f)
        else:
            groups.append([f])
    return sum(1 for g in groups if len(g) == 1)
