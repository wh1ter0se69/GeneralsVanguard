# Copyright 2026 GeneralsX contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# rule bank DATA rule COUNTER.CHINA_VS_GLA: China fights GLA with Flamers and Gattlings
# (rows 165: "vs GLA keep both War Factories spamming: Flamers from one, Gats from the other", 169, 262; RULE_BRIEFS 7 COUNTER-PICK). Stock
# Hard vanilla and Tank China send Battlemaster waves at everyone and field no Dragon before wave 5. This, per general:
#   MATCHUP 'GLA': the SHARED latch (rb.matchup, group "GX Matchup") sets gxrb_mu_gla once AI_PLAYER_GX_SENSE says my enemy is GLA - from
#              the lobby at once, a Random enemy only after 3 different units of one army were fairly seen; it decides ONCE, on the first
#              faction known for my enemy. COUNTER.CHINA_VS_USA reads the same latch with key USA and switches the same teams: the two
#              never run together, and each team's off gate holds both keys (rb.matchup_off);
#   each Hard wave team holding Battlemasters and nothing heavier (no Overlord / Emperor) gets a variant "GX CVG <team>": its
#              Battlemasters become Dragons and Gattlings, flamer_gat_ratio Dragons per Gattling (rounded half up), merged into a Dragon /
#              Gattling slot it already has; with no room for both beside its infantry (the vanilla Rush) all become Dragons (ratio 0:
#              Gattlings). The stock team waits on its off gate, the variant on gxrb_mu_gla ON through the wave's OWN-CLOCK twin;
#   the variant's launch (rb.launch_scripts): TEAM_HUNT once the next wave would launch - at once for a Rush (stock hunts on creation).
# Not built here (UNUSED, defaults only): helix_count (the mass-Helix army needs its own team design and air hooks), mig_team (the default
# 4 is stock's wave-3 MiG team already; the 2-MiG teams are base guards). The Infantry general is not in the registry's scope.
# ARMY.CHINA_VANILLA_MIX / ARMY.CHINA_GENERAL_MIX switch the same teams and declare no MATCHUP key: the CLAIMS overlap keeps them apart.
RULE_ID = 'COUNTER.CHINA_VS_GLA'
MATCHUP = 'GLA'
PARAMS = ('flamer_gat_ratio',)
SCOPE = ('SkirmishChina', 'SkirmishChinaTankGeneral')
PRE = {'SkirmishChina': '', 'SkirmishChinaTankGeneral': 'Tank_'}
UNUSED = {'helix_count': 'not built in rb-data-13 (the wave swap only): a Helix army needs its own team and air hooks',
          'mig_team': 'not built in rb-data-13: the default 4 is stock wave 3\'s MiG team; the 2-MiG teams are base guards'}
TAG, GROUP = 'CVG', 'GX COUNTER.CHINA_VS_GLA'
WAVE = {'_CHINA_1ST_WAVE_H': 'A', '_CHINA_2ND_WAVE_H': 'B', '_CHINA_3RD_WAVE_H': 'C', '_CHINA_4TH_WAVE_H': 'D', '_CHINA_5TH_WAVE_H': 'E'}
LETTER_AT = {None: 'Now', 0: 'Now', 15: 'B', 25: 'C', 40: 'D', 60: 'E'}
HEAVY = ('ChinaTankOverlord', 'ChinaTankEmperor', 'ChinaVehicleNukeLauncher', 'ChinaVehicleInfernoCannon')
NONE = '<none>'
STOCK_TEAMS = {'SkirmishChina': ('China H 1ST Wave Rush', 'China H 2ND Wave Tanks', 'China H 3RD Wave Tanks F', 'China H 3RD Wave Tanks C'),
               'SkirmishChinaTankGeneral': ('China H 1ST Wave Rush. 3', 'China H 2ND Wave Tanks. 3', 'China H 3RD Wave Tanks F. 3',
                                            'China H 3RD Wave Tanks C. 3', 'China H 3RD Wave Tanks. 3', 'China H 4TH Wave Tanks F2. 3',
                                            'China H 4TH Wave Tanks B2. 3', 'China H 5TH Wave Tanks B. 3', 'China H 5TH Wave Tanks C2. 3',
                                            'China H 5TH Wave Tanks F2. 3', 'China H 5TH Wave Tanks F. 3', 'China H 5TH Wave Tanks B2. 3')}
CLAIMS = rb.matchup_claims(SCOPE, sorted({n for ns in STOCK_TEAMS.values() for n in ns}),
                           scripts=[[o, 'China GX %s On %s' % (TAG, w)] for o in SCOPE for w in 'ABCDE'] +
                           [[o, 'China GX %s Launch %s%s' % (TAG, w, f)] for o in SCOPE for w in ('Now', 'B', 'C', 'D', 'E') for f in ('', rb.FORMED)],
                           groups=[[o, GROUP] for o in SCOPE], teams=['GX CVG ' + n for ns in STOCK_TEAMS.values() for n in ns])
TEAM_UNIT_WAIVERS = {pre + u: 'INIZH: the War Factory the Battlemasters it replaces need' for pre in ('', 'Tank_')
                     for u in ('ChinaTankDragon', 'ChinaTankGattling')}


def _half_up(n, r):
    """round(n * r / (r + 1)), halves up"""
    return (2 * n * r + r + 1) // (2 * (r + 1))


def _swap(o, slots, r):
    """the variant's three slots, or None when the team stays stock"""
    pre = PRE[o]
    BM, G, DR = pre + 'ChinaTankBattleMaster', pre + 'ChinaTankGattling', pre + 'ChinaTankDragon'
    slots = [s if s[2] > 0 else [NONE, 0, 0] for s in slots]       # a slot with max 0 is empty
    types = [t for t, _mn, _mx in slots if t not in (NONE, None)]
    if BM not in types or any(t.endswith(h) for t in types for h in HEAVY):
        return None
    (_t, mn, mx) = [s for s in slots if s[0] == BM][0]
    rest = [list(s) for s in slots if s[0] not in (NONE, None, BM)]
    add = {DR: [_half_up(mn, r), _half_up(mx, r)], G: [mn - _half_up(mn, r), mx - _half_up(mx, r)]}
    new = [t for t in (DR, G) if add[t][1] > 0 and t not in [s[0] for s in rest]]
    if len(rest) + len(new) > 3:                    # no room for both: all to the ratio's side
        one = DR if r >= 1 else G
        add = {one: [mn, mx], (G if one == DR else DR): [0, 0]}
        new = [one] if one not in [s[0] for s in rest] else []
    for s in rest:
        if s[0] in add:
            s[1] += add[s[0]][0]
            s[2] += add[s[0]][1]
    out = [[t, add[t][0], add[t][1]] for t in new] + rest
    return (out + [[NONE, 0, 0]] * 3)[:3]


def _slots(t):
    return [[rb.team_get(t, 'teamUnitType%d' % i), rb.team_get(t, 'teamUnitMinCount%d' % i), rb.team_get(t, 'teamUnitMaxCount%d' % i)]
            for i in (1, 2, 3)]


def _plan(scb, o, p):
    """[(stock team name, its stock wave gate, variant slots, on-create hook)] of one general's Hard wave teams (a team another matchup
    rule already switched waits on its off gate: its stock gate is read back from it)"""
    out = []
    for d in rb.teams_ro(scb):
        n = d['teamName']
        if rb.team_get(d, 'teamOwner') != o or not n.startswith('China H '):
            continue
        g = rb.mu_stock_gate(scb, o, rb.team_get(d, 'teamProductionCondition'), WAVE)
        if g is None:
            continue
        sw = _swap(o, _slots(d), p['flamer_gat_ratio'])
        if sw is not None:
            out.append((n, g, sw, rb.team_get(d, 'teamOnCreateScript')))
    return out


def apply(scb, scope, p):
    ts = rb.teams(scb)
    for o in scope:
        plan = _plan(scb, o, p)
        assert sorted(n for n, *_x in plan) == sorted(STOCK_TEAMS[o]), (o, sorted(n for n, *_x in plan))
        rb.matchup(scb, o)
        kids, made, on_flag = [], set(), lambda: [rb.flag_is(scb, rb.mu_flag(MATCHUP), True)]
        for n, g, sw, oncreate in plan:
            w = WAVE[g]
            on = 'China GX %s On %s' % (TAG, w)
            lo, hi = rb.wave_window(scb, o, g)
            hunt_now = oncreate and [rb.decode(scb, l)[0] for l in rb.leaves(rb.find_script(scb, o, oncreate)[2])
                                     if l.name == 'ScriptAction'] == ['TEAM_HUNT']
            launch = 'China GX %s Launch %s' % (TAG, 'Now' if hunt_now or hi is None else LETTER_AT[hi])
            if on not in made:
                kids.append(rb.wave_gate(scb, o, g, on, on_flag))
                made.add(on)
            if launch not in made:
                kids += rb.launch_scripts(scb, launch, 0 if launch.endswith('Now') else hi)
                made.add(launch)
            rb.matchup_off(scb, ts, o, n, MATCHUP)
            rb.variant_team(scb, ts, n, 'GX %s %s' % (TAG, n), on, sw, hooks=rb.launch_fields(launch, 0 if launch.endswith('Now') else hi))
        rb.owner_list(scb, o).kids.append(rb.new_group(GROUP, kids))
    rb.put_teams(scb, ts)


READ_S = 400
CONTRAST = {'what': 'own Dragon Tanks built by 400 s (stock fields its first in wave 5, at 600 s)', 'direction': 1, 'min_delta': 2}


def _dr(name):
    return name.endswith('ChinaTankDragon')


def measure(obs, me, params=None):
    return ob.built_by(obs, me, _dr, READ_S * ob.hz(obs)) if obs else None


def activation(obs, me, params=None):
    """triggered: an own War Factory complete by 200 s and the game ran to 400 s; fired: an own Dragon by 400 s (no stock Hard China team
    names one before wave 5) - with flamer_gat_ratio 0 the variants field no Dragon, and the CONTRAST cannot see the swap"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    wf = ob.first_frame(obs, me, ob.ends_with('ChinaWarFactory'), 1, complete=True)
    end = ob.last_frame(obs)
    triggered = wf is not None and wf <= 200 * hz and end is not None and end >= READ_S * hz
    dr = measure(obs, me, params)
    fired = triggered and (dr or 0) >= 1
    return {'triggered': bool(triggered), 'fired': bool(fired), 'detail': 'warfactory_f=%s dragons_400s=%s' % (wf, dr)}
