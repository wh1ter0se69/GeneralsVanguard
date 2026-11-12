# Copyright 2026 GeneralsX contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# rule bank DATA rule ARMY.CHINA_CRAWLER_DECOY: China Tank puts a Troop Crawler in
# its tank waves to soak the first enemy shots. Stock Tank Hard's 2nd- and 3rd-wave Battlemaster teams ("2ND Wave Tanks", "3RD Wave
# Tanks F/C": 0-5 Battlemasters, slots 2 and 3 empty) never field a Crawler. Here each gains [Troop Crawler, crawler_n, crawler_n] in
# slot 2. The Crawler needs only the Tank War Factory (INIZH), which the team's Battlemasters need anyway. Team counts only: no
# script, no enemy term; the wave gates stay stock. crawler_n = 0 is stock.
RULE_ID = 'ARMY.CHINA_CRAWLER_DECOY'
PARAMS = ('crawler_n',)
TK, CR, BM = 'SkirmishChinaTankGeneral', 'Tank_ChinaVehicleTroopCrawler', 'Tank_ChinaTankBattleMaster'
TEAMS = ('China H 2ND Wave Tanks. 3', 'China H 3RD Wave Tanks F. 3', 'China H 3RD Wave Tanks C. 3')
CLAIMS = {'teams': sorted(TEAMS)}
TEAM_UNIT_WAIVERS = {CR: 'INIZH: Tank War Factory only, which the Battlemasters of these teams need anyway'}
UNTIL = 400                                   # seconds: the smoke game ends at 420 s


def apply(scb, scope, p):
    if TK not in scope or p['crawler_n'] == 0:
        return
    ts = rb.teams(scb)
    for n in TEAMS:
        t = rb.team(ts, n)
        assert t is not None and rb.team_get(t, 'teamOwner') == TK, n
        assert rb.team_get(t, 'teamUnitType1') == BM and rb.team_get(t, 'teamUnitMaxCount2') == 0, n
        for k, v in (('teamUnitType2', CR), ('teamUnitMinCount2', p['crawler_n']), ('teamUnitMaxCount2', p['crawler_n'])):
            rb.team_set(scb, t, k, v)
    rb.put_teams(scb, ts)


def _crawlers(obs, me):
    return ob.built_by(obs, me, ob.ends_with('ChinaVehicleTroopCrawler'), UNTIL * ob.hz(obs)) if obs else None


def activation(obs, me, params=None):
    """triggered: own War Factory complete by 240 s and the game ran past 400 s; fired: an own Troop Crawler by 400 s (no stock Tank Hard
    wave team holds one)"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    fw = ob.first_frame(obs, me, ob.ends_with('ChinaWarFactory'), 1, complete=True)
    n = _crawlers(obs, me)
    triggered = fw is not None and fw <= 240 * hz and n is not None
    return {'triggered': bool(triggered), 'fired': bool(triggered and n >= 1), 'detail': 'warfactory_f=%s crawlers_by_400s=%s' % (fw, n)}


CONTRAST = {'what': 'own Troop Crawlers built by 400 s', 'direction': 1, 'min_delta': 1}


def measure(obs, me, params=None):
    return _crawlers(obs, me)
