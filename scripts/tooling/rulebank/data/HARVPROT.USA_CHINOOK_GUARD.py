# GeneralsX: rule bank DATA rule HARVPROT.USA_CHINOOK_GUARD (C·rb-data-10): USA keeps a Humvee at home guarding
# its Chinooks. Every stock USA Hard Humvee joins a wave. Per USA general this adds team "GX Chinook Guard <g>" (home_guard_n of the
# general's own Humvee, one at a time, re-formed when lost) once an own War Factory is complete; on creation it guards the player's own
# inner perimeter ([Skirmish]MyInnerPerimeter, the stock base-guard area). No stock script or team changes; no condition reads the
# enemy. home_guard_n = 0 is stock. The "spare" half of the tip (a spare Chinook) is USA2.CHINOOKS' (pinned, ENGINE).
RULE_ID = 'HARVPROT.USA_CHINOOK_GUARD'
PARAMS = ('home_guard_n',)
GEN = {'SkirmishAmerica': ('USA', '', 'USA H 1ST Wave Supply Hunters'), 'SkirmishAmericaAirForceGeneral': ('AirForce', 'AirF_', 'USA H 1ST Wave Supply Hunters. 1'),
       'SkirmishAmericaLaserGeneral': ('Laser', 'Lazr_', 'USA H 1ST Wave Supply Hunters. 2'),
       'SkirmishAmericaSuperWeaponGeneral': ('Superweapon', 'SupW_', 'USA H 1W Guard Combat Zone. 3')}
TEAM, COND, START, GROUP = 'GX Chinook Guard %s', 'GX Chinook Guard Cond %s - H', 'GX Chinook Guard Start %s - H', 'GX HARVPROT.USA_CHINOOK_GUARD'
HOME = '[Skirmish]MyInnerPerimeter'
CLAIMS = {'scripts': [[o, n % g] for o, (g, _p, _d) in GEN.items() for n in (COND, START)], 'groups': [[o, GROUP] for o in GEN],
          'teams': sorted(TEAM % g for g, _p, _d in GEN.values())}
FROM, UNTIL, NEAR = 200, 400, 600            # seconds; world units from the own Command Center


def _bare(scb, t):
    """a copied donor team keeps no hook of its own: every script field but the production condition is blanked (a hook would name stock
    logic this rule does not claim)"""
    for e in t['_e']:
        if e[0] != 'teamProductionCondition' and e[0].startswith('team') and 'Script' in e[0] and e[2]:
            rb.team_set(scb, t, e[0], '')


def apply(scb, scope, p):
    n = p['home_guard_n']
    if n == 0:
        return
    ts = rb.teams(scb)
    for o in scope:
        g, pre, donor_name = GEN[o]
        guard = rb.act(scb, 'TEAM_GUARD_AREA', ('TEAM', '<This Team>'), ('TRIGGER_AREA', HOME))
        kids = [rb.new_script(scb, START % g, [[rb.cond(scb, 'CONDITION_TRUE')]], [guard], sub=True),
                rb.new_script(scb, COND % g, [[rb.cond(scb, 'BUILT_BY_PLAYER', ('OBJECT_TYPE', pre + 'AmericaWarFactory'), ('SIDE', rb.SIDE_ME))]],
                              [mss.deep_copy(guard)], sub=True)]
        rb.owner_list(scb, o).kids.append(rb.new_group(GROUP, kids))
        donor = rb.team(ts, donor_name)
        assert donor is not None and rb.team_get(donor, 'teamOwner') == o, donor_name
        t = {'_e': [list(e) for e in donor['_e']], 'teamName': None}
        _bare(scb, t)
        for k, v in (('teamName', TEAM % g), ('teamUnitType1', pre + 'AmericaVehicleHumvee'), ('teamUnitMinCount1', n), ('teamUnitMaxCount1', n),
                     ('teamUnitMinCount2', 0), ('teamUnitMaxCount2', 0), ('teamUnitMinCount3', 0), ('teamUnitMaxCount3', 0), ('teamMaxInstances', 1),
                     ('teamProductionCondition', COND % g), ('teamOnCreateScript', START % g), ('teamInitialIdleFrames', 600)):
            rb.team_set(scb, t, k, v)
        rb.add_new_team(ts, ts.index(donor) + 1, t)
    rb.put_teams(scb, ts)


def _home(obs, me):
    """(seconds, mean count) over [200 s, 400 s): seconds during which an own Humvee stands within 600 of the own Command Center, and the
    mean number of own Humvees there per sample; None if the game ended earlier"""
    if not obs:
        return None
    hz, nm = ob.hz(obs), ob.names(obs)
    end = ob.last_frame(obs)
    if end is None or end < UNTIL * hz:
        return None
    cc, fs, home, total, samples = None, [], 0, 0, 0
    for r in obs:
        if r.get('t') != 'obs':
            continue
        fs.append(r['f'])
        mine = [o for o in r.get('o', []) if o[1] == me]
        if cc is None:
            cc = next(((o[3], o[4]) for o in mine if nm.get(o[2], '').endswith('AmericaCommandCenter')), None)
        if cc is not None and FROM * hz <= r['f'] < UNTIL * hz:
            n = sum(1 for o in mine if nm.get(o[2], '').endswith('AmericaVehicleHumvee') and (o[3] - cc[0]) ** 2 + (o[4] - cc[1]) ** 2 <= NEAR ** 2)
            home, total, samples = home + (n > 0), total + n, samples + 1
    step = fs[1] - fs[0] if len(fs) > 1 else 30
    return int(round(home * step / hz)), (total / samples if samples else 0.0)


def _home_s(obs, me):
    h = _home(obs, me)
    return None if h is None else h[0]


def activation(obs, me, params=None):
    """triggered: own War Factory complete by 150 s and the game ran past 400 s; fired: an own Humvee at home for at least 100 s of
    200..400 s (stock sends every Humvee out with a wave)"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    fw = ob.first_frame(obs, me, ob.ends_with('AmericaWarFactory'), 1, complete=True)
    s = _home_s(obs, me)
    triggered = fw is not None and fw <= 150 * hz and s is not None
    return {'triggered': bool(triggered), 'fired': bool(triggered and s >= 100), 'detail': 'warfactory_f=%s humvee_home_s=%s' % (fw, s)}


# rb-data-11 (POST-HOC, after smoke8): stock always keeps some Humvee at home, so the seconds-with-one reader read 200 vs 200 in every game;
# the reader is the mean count at home. min_delta 0.5 = half the default guard (home_guard_n 1)
CONTRAST = {'what': 'mean own Humvees within 600 of the own Command Center over 200..400 s', 'direction': 1, 'min_delta': 0.5, 'post_hoc_seeds': (1, 2)}


def measure(obs, me, params=None):
    h = _home(obs, me)
    return None if h is None else h[1]
