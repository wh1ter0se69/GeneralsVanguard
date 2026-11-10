# GeneralsX: rule bank DATA rule COUNTER.CHINA_VS_USA: China does not feed Battlemasters to USA
# (RULE_BRIEFS 7, Dominator: "do not build Battlemasters, Humvees shred them"; rows 126, 168, 170, 200). Stock Hard vanilla and Tank China
# send Battlemaster waves at everyone. This, per general:
#   MATCHUP 'USA' (rb-data-13): the SHARED latch (rb.matchup, group "GX Matchup") sets gxrb_mu_usa once AI_PLAYER_GX_SENSE says my enemy
#              is USA - from the lobby at once, a Random enemy only after 3 different units of one army were fairly seen (rb-sense-1); it
#              decides ONCE, on the first faction known for my enemy whichever it is, since in FFA the enemy can change.
#              Other generals' COUNTER rules (vs GLA, vs China) read the same latch with their own key and may share these teams;
#   each Hard wave team holding Battlemasters and nothing heavier (no Overlord / Emperor) gets a variant "GX CVU <team>": bm_vs_usa
#              Battlemasters, the rest Tank Hunters (tank_hunter_share %, rounded half up) and Gattlings; a Rush team's Battlemasters
#              become Gattlings (min = max kept, its infantry stays). The stock team waits on its own off gate "GX MU Off <team>" (a copy
#              of its wave gate + every key's flag OFF, rb.matchup_off), the variant on gxrb_mu_usa ON through the wave's OWN-CLOCK twin
#              (scbx.wave_window: stock waves follow _ESCALATION, an enemy read);
#   the variant's launch (rb.launch_scripts): TEAM_HUNT once the next wave would launch (stock's _LAUNCH_ATTACK pulse) - at once for a Rush (stock hunts on
#              creation). No stock hook is named (the attack sequences behind _LAUNCH_ATTACK reach enemy-gated logic).
# Not built here (UNUSED, defaults only): usa_gat_share, napalm_esc_vs_usa, migs_wave, helix_vs_usa - NEXT_rb-data-13. The Infantry general
# fields no Battlemaster wave (out of SCOPE). The same wave teams are ARMY.CHINA_VANILLA_MIX's / ARMY.CHINA_GENERAL_MIX's: those
# declare no MATCHUP key, so the CLAIMS overlap still keeps them apart per player.
RULE_ID = 'COUNTER.CHINA_VS_USA'
MATCHUP = 'USA'
PARAMS = ('bm_vs_usa', 'tank_hunter_share')
SCOPE = ('SkirmishChina', 'SkirmishChinaTankGeneral')
PRE = {'SkirmishChina': '', 'SkirmishChinaTankGeneral': 'Tank_'}
UNUSED = {k: 'not built in rb-data-12 (the wave swap only; NEXT_rb-data-13)' for k in ('usa_gat_share', 'napalm_esc_vs_usa', 'migs_wave', 'helix_vs_usa')}
TAG, GROUP = 'CVU', 'GX COUNTER.CHINA_VS_USA'
WAVE = {'_CHINA_1ST_WAVE_H': 'A', '_CHINA_2ND_WAVE_H': 'B', '_CHINA_3RD_WAVE_H': 'C', '_CHINA_4TH_WAVE_H': 'D', '_CHINA_5TH_WAVE_H': 'E'}
LETTER_AT = {None: 'Now', 0: 'Now', 15: 'B', 25: 'C', 40: 'D', 60: 'E'}
HEAVY = ('ChinaTankOverlord', 'ChinaTankEmperor', 'ChinaVehicleNukeLauncher', 'ChinaVehicleInfernoCannon')
NONE = '<none>'


def _swap(o, slots, p):
    """the variant's three slots, or None when the team stays stock"""
    pre = PRE[o]
    BM, G, TH = pre + 'ChinaTankBattleMaster', pre + 'ChinaTankGattling', pre + 'ChinaInfantryTankHunter'
    slots = [s if s[2] > 0 else [NONE, 0, 0] for s in slots]       # a slot with max 0 is empty
    types = [t for t, _mn, mx in slots if t not in (NONE, None)]
    if BM not in types or any(t.endswith(h) for t in types for h in HEAVY):
        return None
    b = p['bm_vs_usa']
    if types == [BM] and slots[0][1] == 0:      # a pure Battlemaster wave team (a required minimum goes the Rush way below)
        (_t, mn, n) = [x for x in slots if x[0] == BM][0]
        bb = min(b, n)
        th = (2 * (n - bb) * p['tank_hunter_share'] + 100) // 200
        g = n - bb - th
        out = [[G, 0, g], [TH, 0, th], [BM, 0, bb] if bb else [NONE, 0, 0]]
        return [s if s[2] > 0 or s[0] == NONE else [NONE, 0, 0] for s in out]
    # a Rush: its Battlemasters (min = max) become Gattlings, merged into a Gattling slot it already has
    i = [k for k, s in enumerate(slots) if s[0] == BM][0]
    (_t, mn, mx) = slots[i]
    keep = min(b, mx)
    rest = [list(s) for k, s in enumerate(slots) if k != i and s[0] not in (NONE, None)]
    gi = [k for k, s in enumerate(rest) if s[0] == G]
    lead = []
    if gi:
        rest[gi[0]][1] += max(mn - keep, 0)
        rest[gi[0]][2] += mx - keep
    elif mx > keep:                             # no empty Gattling slot when every Battlemaster stays
        lead = [[G, max(mn - keep, 0), mx - keep]]
    out = ([[BM, min(mn, keep), keep]] if keep else []) + lead + rest
    return (out + [[NONE, 0, 0]] * 3)[:3]      # a Rush keeping Battlemasters with no Gattling slot drops its last infantry slot


def _slots(t):
    return [[rb.team_get(t, 'teamUnitType%d' % i), rb.team_get(t, 'teamUnitMinCount%d' % i), rb.team_get(t, 'teamUnitMaxCount%d' % i)]
            for i in (1, 2, 3)]


def _plan(scb, o, p):
    """[(stock team name, stock gate, variant slots)] of one general's Hard wave teams"""
    out = []
    for d in rb.teams_ro(scb):
        n = d['teamName']
        if rb.team_get(d, 'teamOwner') != o or not n.startswith('China H '):
            continue
        g = rb.mu_stock_gate(scb, o, rb.team_get(d, 'teamProductionCondition'), WAVE)    # rb-data-13: another key may have switched it
        if g is None:
            continue
        sw = _swap(o, _slots(d), p)
        if sw is not None:
            out.append((n, g, sw, rb.team_get(d, 'teamOnCreateScript')))
    return out


STOCK_TEAMS = {'SkirmishChina': ('China H 1ST Wave Rush', 'China H 2ND Wave Tanks', 'China H 3RD Wave Tanks F', 'China H 3RD Wave Tanks C'),
               'SkirmishChinaTankGeneral': ('China H 1ST Wave Rush. 3', 'China H 2ND Wave Tanks. 3', 'China H 3RD Wave Tanks F. 3',
                                            'China H 3RD Wave Tanks C. 3', 'China H 3RD Wave Tanks. 3', 'China H 4TH Wave Tanks F2. 3',
                                            'China H 4TH Wave Tanks B2. 3', 'China H 5TH Wave Tanks B. 3', 'China H 5TH Wave Tanks C2. 3',
                                            'China H 5TH Wave Tanks F2. 3', 'China H 5TH Wave Tanks F. 3', 'China H 5TH Wave Tanks B2. 3')}
CLAIMS = rb.matchup_claims(SCOPE, sorted({n for ns in STOCK_TEAMS.values() for n in ns}),
                           scripts=[[o, 'China GX %s On %s' % (TAG, w)] for o in SCOPE for w in 'ABCDE'] +
                           [[o, 'China GX %s Launch %s%s' % (TAG, w, f)] for o in SCOPE for w in ('Now', 'B', 'C', 'D', 'E') for f in ('', rb.FORMED)],
                           groups=[[o, GROUP] for o in SCOPE], teams=['GX CVU ' + n for ns in STOCK_TEAMS.values() for n in ns])
TEAM_UNIT_WAIVERS = {pre + 'ChinaInfantryTankHunter': 'INIZH: a Barracks, which stock China builds first (its wave-1 Rush fields Tank Hunters)'
                     for pre in ('', 'Tank_')}
TEAM_UNIT_WAIVERS.update({pre + 'ChinaTankGattling': 'INIZH: the War Factory the Battlemasters it replaces need' for pre in ('', 'Tank_')})


def apply(scb, scope, p):
    ts = rb.teams(scb)
    for o in scope:
        plan = _plan(scb, o, p)
        assert plan, '%s: no Battlemaster wave team' % o
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


# smoke10 (rb-data-12): read at 420 s = the games' last frame, it found no frame past it; now 400 s - so seeds 1-4 of smoke10 are POST-HOC
READ_S = 400
CONTRAST = {'what': 'own Battlemasters built by 400 s (vs USA: the variants field none at the default)', 'direction': -1, 'min_delta': 2,
            'post_hoc_seeds': (1, 2, 3, 4)}


def _bm(name):
    return name.endswith('ChinaTankBattleMaster')


def measure(obs, me, params=None):
    return ob.built_by(obs, me, _bm, READ_S * ob.hz(obs)) if obs else None


def activation(obs, me, params=None):
    """triggered: an own War Factory complete by 200 s and the game ran to 400 s; fired: an own Tank Hunter or Gattling by 400 s and
    at most 1 Battlemaster - the activation alone cannot tell ON from OFF (stock fields Gattlings too): the CONTRAST reads it"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    wf = ob.first_frame(obs, me, ob.ends_with('ChinaWarFactory'), 1, complete=True)
    end = ob.last_frame(obs)
    triggered = wf is not None and wf <= 200 * hz and end is not None and end >= READ_S * hz
    bm = measure(obs, me, params)
    th = ob.built_by(obs, me, ob.ends_with('ChinaInfantryTankHunter'), READ_S * hz)
    gt = ob.built_by(obs, me, ob.ends_with('ChinaTankGattling'), READ_S * hz)
    fired = triggered and bm is not None and bm <= 1 and (th or 0) + (gt or 0) >= 1      # a Tank Hunter OR a Gattling
    return {'triggered': bool(triggered), 'fired': bool(fired), 'detail': 'warfactory_f=%s bm_420s=%s th_420s=%s gat_420s=%s' % (wf, bm, th, gt)}
