# GeneralsX @feature Claude 30/10/2026 rule bank DATA rule ARMY.CHINA_VANILLA_MIX (C·rb-data): vanilla China's pro army - Outposts +
# Gattlings + Flamers, few Battlemasters, variety, the 2nd War Factory after the oils (rows 154, 159, 161, 162). Stock Hard vanilla China
# fields Battlemasters in every tank wave (3, then up to 5 per team), never an Outpost or (before wave 5) a Dragon. Team edits:
#   bm_cap B caps the Battlemaster count of the wave teams (wave 1 Rush, 2/3/4/5 Tanks; a stock minimum of 0 stays 0);
#   variety_units V adds the tip's missing types in its order [Gattling, Outpost, Flamer, MiG, Helix, Inferno, ECM, Nuke Cannon]:
#   Outposts (index 2) from V >= 2, Flamers (3) from V >= 3, Infernos (6) from V >= 6 (2 per team, a design constant);
#   in the 5-unit 2nd/3rd wave Tanks teams the added type takes min(outposts or flamers, 5 - B) and Gattlings the rest;
#   wf2_after_oils = 1: the stock "China War Factory 2 - H" also waits for my first oil derrick, or _GX_CLOCK 24 (240 s) at the latest.
# Own state only. The 2nd War Factory script is OPEN.CHINA_2ND_SUPPLY's too: the CLAIMS overlap keeps the two apart.
RULE_ID = 'ARMY.CHINA_VANILLA_MIX'
PARAMS = ('outposts', 'flamers', 'bm_cap', 'variety_units', 'wf2_after_oils')
CN = 'SkirmishChina'
BM, OP, DR, GT, IN = 'ChinaTankBattleMaster', 'ChinaVehicleListeningOutpost', 'ChinaTankDragon', 'ChinaTankGattling', 'ChinaVehicleInfernoCannon'
NONE = '<none>'
INFERNOS = 0       # rb-data-9: an Inferno Cannon needs a Propaganda Center (INIZH), which AIPlayer::isPossibleToBuildTeam would wait for in the team
WF2, WF2_DUE = 'China War Factory 2 - H', 24
TEAMS = ('China H 1ST Wave Rush', 'China H 2ND Wave Tanks', 'China H 3RD Wave Tanks F', 'China H 3RD Wave Tanks C',
         'China H 4TH Wave Tanks F', 'China H 4TH Wave Tanks B', 'China H 5TH Wave Tanks F', 'China H 5TH Wave Tanks C')
UNUSED = {'variety_units': 'every accepted value (3..8) now adds the same Outposts and Flamers; the Inferno tier it used to gate was dropped (rb-data-9)'}
CLAIMS = {'scripts': [[CN, WF2]], 'teams': sorted(TEAMS)}
TEAM_UNIT_WAIVERS = {u: 'INIZH: War Factory only, which the Battlemaster of these teams needs anyway' for u in (GT, OP, DR)}


def _slots(t):
    return [[rb.team_get(t, 'teamUnitType%d' % i), rb.team_get(t, 'teamUnitMinCount%d' % i), rb.team_get(t, 'teamUnitMaxCount%d' % i)]
            for i in (1, 2, 3)]


def _put(scb, t, slots):
    for i, (typ, mn, mx) in enumerate(slots, 1):
        rb.team_set(scb, t, 'teamUnitType%d' % i, typ if mx > 0 or typ != NONE else NONE)
        rb.team_set(scb, t, 'teamUnitMinCount%d' % i, mn)
        rb.team_set(scb, t, 'teamUnitMaxCount%d' % i, mx)


def _opt(typ, n):
    return [typ, 0, n] if n > 0 else [NONE, 0, 0]


def apply(scb, scope, p):
    if CN not in scope:
        return
    ts = rb.teams(scb)
    team = {}
    for n in TEAMS:
        t = rb.team(ts, n)
        assert t is not None and rb.team_get(t, 'teamOwner') == CN, n
        team[n] = (t, _slots(t))
    B, V = p['bm_cap'], p['variety_units']
    op = p['outposts'] if V >= 2 else 0
    fl = p['flamers'] if V >= 3 else 0
    t, s = team['China H 1ST Wave Rush']
    assert s[0][0] == BM, s
    _put(scb, t, [[BM, min(s[0][1], B), min(s[0][2], B)], s[1], s[2]])
    for n, extra in (('China H 2ND Wave Tanks', (OP, op)), ('China H 3RD Wave Tanks F', (DR, fl)), ('China H 3RD Wave Tanks C', (OP, op))):
        t, s = team[n]
        assert s[0][0] == BM and s[0][1] == 0 and s[1][0] == s[2][0] == NONE, (n, s)
        total = s[0][2]
        b = min(total, B)
        x = min(extra[1], total - b)
        _put(scb, t, [[BM, 0, b], _opt(extra[0], x), _opt(GT, total - b - x)])
    for n, extra in (('China H 4TH Wave Tanks F', (IN, 0)), ('China H 4TH Wave Tanks B', (DR, fl))):
        t, s = team[n]
        assert s[0][0] == BM and s[2][0] == NONE, (n, s)
        _put(scb, t, [[BM, s[0][1], min(s[0][2], B)], s[1], _opt(*extra) if extra[1] > 0 else s[2]])
    for n in ('China H 5TH Wave Tanks F', 'China H 5TH Wave Tanks C'):
        t, s = team[n]
        assert s[0][0] == BM, (n, s)
        _put(scb, t, [[BM, min(s[0][1], B), min(s[0][2], B)], s[1], s[2]])
    rb.put_teams(scb, ts)
    if p['wf2_after_oils']:
        _g, _k, s = rb.find_script(scb, CN, WF2)
        ors = [k for k in s.kids if k.name == 'OrCondition']
        assert len(ors) == 1 and len(ors[0].kids) == 1, (WF2, ors)
        g = ors[0].kids[0]
        new = [mss.Node('OrCondition', ors[0].ver, kids=[mss.deep_copy(g), rb.own_count(scb, rb.GE, 1, 'TechOilDerrick')]),
               mss.Node('OrCondition', ors[0].ver, kids=[mss.deep_copy(g), rb.clock_at_least(scb, WF2_DUE)])]
        i = s.kids.index(ors[0]); s.kids[i:i + 1] = new


def activation(obs, me, params=None):
    """triggered: own War Factory complete by 200 s and the game ran past 270 s; fired: an own Listening Outpost (or, with no Outposts in
    the mix, a Dragon) by 270 s - no stock Hard wave team names an Outpost, and Dragons come only in wave 5"""
    p = dict({'outposts': 2, 'variety_units': 6, 'flamers': 2}, **(params or {}))
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    fw = ob.first_frame(obs, me, ob.ends_with('ChinaWarFactory'), 1, complete=True)
    end = ob.last_frame(obs)
    triggered = fw is not None and fw <= 200 * hz and end is not None and end >= 270 * hz
    sig = OP if p['variety_units'] >= 2 and p['outposts'] > 0 else DR
    f = ob.first_frame(obs, me, ob.ends_with(sig))
    fired = triggered and f is not None and f <= 270 * hz
    return {'triggered': bool(triggered), 'fired': bool(fired), 'detail': 'warfactory_f=%s %s_f=%s' % (fw, sig, f)}
