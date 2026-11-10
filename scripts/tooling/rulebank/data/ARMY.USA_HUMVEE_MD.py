# GeneralsX: rule bank DATA rule ARMY.USA_HUMVEE_MD: USA can stay on Humvees + Missile Defenders
# (+1 Ambulance) all game (rows 6, 27, 28); the Air Force mixes Raptors with that ground team and keeps one Comanche home (row 70).
# Team edits only, Hard wave teams of USA and the Air Force:
#   wave 1 Rush: md Missile Defenders, humvees Humvees, ambulance Ambulances (stock: 4 MD + 2 Crusaders);
#   wave 2 Fast Attack: humvees Humvees + md MD; wave 2 Tanks and wave 3 Tanks F/C: each team's stock total T split as
#   h = round(T * humvee_share_pm / 1000) Humvees, m = round(T * md_share_pm / 1000) MD (m <= T - h), the rest its stock unit;
#   Air Force wave 2 Fast Attack stays 2 Raptors (rb-data-9: Humvees + MD + Ambulance needed a War Factory and Barracks that
#   AIPlayer::isPossibleToBuildTeam wants standing, and the Air Force opens on Airfields); its 2W-4W Comanche guards keep
#   comanche_home Comanches (0 = stock: an empty one-slot team has no tested meaning).
# A stock minimum of 0 stays 0 (an optional slot); otherwise min = max. Waves 4-5 keep their heavy tech. No condition, no enemy input.
RULE_ID = 'ARMY.USA_HUMVEE_MD'
PARAMS = ('humvee_share_pm', 'md_share_pm', 'humvees', 'md', 'ambulance', 'comanche_home')
US, AF = 'SkirmishAmerica', 'SkirmishAmericaAirForceGeneral'
H, MD, MED = 'AmericaVehicleHumvee', 'AmericaInfantryMissileDefender', 'AmericaVehicleMedic'
SPLIT = ('USA H 2ND Wave Tanks', 'USA H 3RD Wave Tanks F', 'USA H 3RD Wave Tanks C')
GUARDS = ('USA H 2W Comanche Guards. 1', 'USA H 3W Comanche Guards. 1', 'USA H 4W Comanche Guards. 1')
TEAMS = {US: ('USA H 1ST Wave Rush', 'USA H 2ND Wave Fast Attack') + SPLIT, AF: GUARDS}
CLAIMS = {'teams': sorted(TEAMS[US] + TEAMS[AF])}
TEAM_UNIT_WAIVERS = {H: 'INIZH: War Factory only, which the stock Crusader / Paladin / Tomahawk of these teams need anyway',
                     MED: 'INIZH: War Factory only, which the team\'s Humvees need anyway',
                     MD: 'Barracks: the stock wave 1 Rush already fields Missile Defenders, so Barracks is first in the build order (INFERRED)'}


def _slots(t):
    return [[rb.team_get(t, 'teamUnitType%d' % i), rb.team_get(t, 'teamUnitMinCount%d' % i), rb.team_get(t, 'teamUnitMaxCount%d' % i)]
            for i in (1, 2, 3)]


def _put(scb, t, slots):
    for i, (typ, mn, mx) in enumerate(slots, 1):
        if typ is not None:
            rb.team_set(scb, t, 'teamUnitType%d' % i, typ)
        rb.team_set(scb, t, 'teamUnitMinCount%d' % i, mn)
        rb.team_set(scb, t, 'teamUnitMaxCount%d' % i, mx)


def _team(ts, name, owner):
    t = rb.team(ts, name)
    assert t is not None and rb.team_get(t, 'teamOwner') == owner, '%s: no team %r' % (owner, name)
    return t


def apply(scb, scope, p):
    ts = rb.teams(scb)
    if US in scope:
        t = _team(ts, 'USA H 1ST Wave Rush', US)
        s = _slots(t)
        assert [x[0] for x in s] == [MD, H, 'AmericaTankCrusader'], s
        _put(scb, t, [[MD, p['md'], p['md']], [H, p['humvees'], p['humvees']], [MED, p['ambulance'], p['ambulance']]])
        t = _team(ts, 'USA H 2ND Wave Fast Attack', US)
        s = _slots(t)
        assert [x[0] for x in s[:2]] == [H, MD], s
        _put(scb, t, [[H, p['humvees'], p['humvees']], [MD, p['md'], p['md']], s[2]])
        for name in SPLIT:
            t = _team(ts, name, US)
            s = _slots(t)
            total = sum(x[2] for x in s)
            h = (total * p['humvee_share_pm'] + 500) // 1000
            m = min((total * p['md_share_pm'] + 500) // 1000, total - h)
            first = next(i for i, x in enumerate(s) if x[2] > 0)          # the stock unit keeps the rest, in its own slot
            rest, optional = total - h - m, s[first][1] == 0
            new = [None, None, None]
            new[first] = [s[first][0], 0 if optional else rest, rest]
            free = [i for i in range(3) if i != first]
            new[free[0]] = [H, 0 if optional else h, h]
            new[free[1]] = [MD, 0 if optional else m, m]
            _put(scb, t, new)
    if AF in scope:
        if p['comanche_home'] > 0:
            for name in GUARDS:
                t = _team(ts, name, AF)
                s = _slots(t)
                assert s[0][0] == 'AirF_AmericaVehicleComanche', (name, s)
                _put(scb, t, [[s[0][0], p['comanche_home'], p['comanche_home']], s[1], s[2]])
    rb.put_teams(scb, ts)


def activation(obs, me, params=None):
    """triggered: own War Factory complete by 240 s and the game ran past 300 s; fired: an own Ambulance by 300 s (no stock team names
    one) - or, with ambulance = 0, 6+ own Humvees by 300 s"""
    p = dict({'ambulance': 1}, **(params or {}))
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    af = ob.first_frame(obs, me, ob.ends_with('AirF_AmericaAirfield'), 1, complete=True)
    if af is not None:                    # Air Force: the rule only keeps comanche_home Comanches at home, so the signal is the first Comanche
        end = ob.last_frame(obs)
        triggered = af <= 300 * hz and end is not None and end >= 360 * hz
        fc = ob.first_frame(obs, me, ob.ends_with('AirF_AmericaVehicleComanche'))
        return {'triggered': bool(triggered), 'fired': bool(triggered and fc is not None and fc <= 360 * hz),
                'detail': 'airfield_f=%s comanche_f=%s' % (af, fc)}
    fw = ob.first_frame(obs, me, ob.ends_with('AmericaWarFactory'), 1, complete=True)
    end = ob.last_frame(obs)
    triggered = fw is not None and fw <= 240 * hz and end is not None and end >= 300 * hz
    if p['ambulance'] > 0:
        f = ob.first_frame(obs, me, ob.ends_with(MED))
    else:
        f = ob.first_frame(obs, me, ob.ends_with(H), 6)
    fired = triggered and f is not None and f <= 300 * hz
    return {'triggered': bool(triggered), 'fired': bool(fired), 'detail': 'warfactory_f=%s signal_f=%s' % (fw, f)}
