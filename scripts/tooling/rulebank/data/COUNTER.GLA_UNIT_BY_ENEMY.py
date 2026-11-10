# GeneralsX: rule bank DATA rule COUNTER.GLA_UNIT_BY_ENEMY (C·rb-data-16): GLA picks its follow-up tank by the
# China GENERAL it meets (row 1893: "Pick the follow-up by enemy: ... Quads vs vanilla China, Scorpions (after rocket upgrade) vs Nuke or Tank
# China"; rows 1874-1875: "Do not build Scorpions against China Infantry ... build mostly Quad Cannons"). Stock Hard GLA sends Scorpion
# waves at everyone. This, per general:
#   MATCHUP 'China': the SHARED latch (rb.matchup) sets gxrb_mu_china once AI_PLAYER_GX_SENSE says my enemy is China (lobby at once; a
#              Random enemy after 3 different units of one army were fairly seen); it decides ONCE;
#   a GENERAL read of its own (rb.general_latch, in the frame the key is set): gxrb_gube_inf once the sensor says the Infantry general,
#              gxrb_gube_nt for the Nuke or the Tank general (one flag: both keep their Scorpions) - the sensor names a general only when
#              the LOBBY showed it - and gxrb_gube_done either way;
#   each Hard wave 2-5 team of nothing but this general's Scorpions (the teams COUNTER.GLA_VS_USA switches for USA: the two keys share
#              them) waits on its off gate and gets three variants, each `share` per mille of its Scorpions (rounded half up), the rest
#              Quad Cannons (same Arms Dealer, INIZH: no building to wait on):
#     "GX GUBQ <team>" vs vanilla China, or a China enemy whose general is not known (a Random one): share 1000 - quad_vs_china_pm;
#     "GX GUBI <team>" vs the Infantry general: share scorp_vs_inf_pm;
#     "GX GUBS <team>" vs the Nuke or the Tank general: share scorp_vs_nuke_pm;
#   each waits through its wave's OWN-CLOCK twin (scbx.wave_window) and hunts once the next wave would launch (wave 5 at once). No stock
#   hook is named (the stock attack hooks wait on _LAUNCH_ATTACK, enemy state).
#   rb-data-17 (the lead's stock-copy ruling): a variant whose share keeps the stock slots (scorp_vs_nuke_pm 1000, the default; also
#   quad_vs_china_pm 0 or scorp_vs_inf_pm 1000) is NOT built - a copy on the own clock fielded 40 vs stock's 57 Scorpions by 600 s
#   (rb-data-16 s5). The STOCK team itself stays on instead: its off gate gains the keep block [stock gate, gxrb_mu_china ON, that
#   variant's own flags] (rb.matchup_keep), so against the Nuke and Tank generals the waves are stock's, gate, hooks and all.
# The Stealth general fields no Scorpion (Combat Bikes and Quads): out of SCOPE. No stock script changes.
# Not built (UNUSED, defaults only): tech_vs_usa_pm - "only Technicals vs USA" is key USA, which COUNTER.GLA_VS_USA (a declared conflict)
# owns for these generals; a rule holds one key.
RULE_ID = 'COUNTER.GLA_UNIT_BY_ENEMY'
MATCHUP = 'China'
PARAMS = ('quad_vs_china_pm', 'scorp_vs_inf_pm', 'scorp_vs_nuke_pm')
SCOPE = ('SkirmishGLA', 'SkirmishGLADemolitionGeneral', 'SkirmishGLAToxinGeneral')
PRE = {'SkirmishGLA': '', 'SkirmishGLADemolitionGeneral': 'Demo_', 'SkirmishGLAToxinGeneral': 'Chem_'}
UNUSED = {'tech_vs_usa_pm': 'not built: "only Technicals vs USA" is key USA, which COUNTER.GLA_VS_USA (a declared conflict) owns for these '
                            'generals; one rule holds one key'}
TAG, GROUP = 'GX GUB', 'GX COUNTER.GLA_UNIT_BY_ENEMY'
INF, NT, DONE = 'gxrb_gube_inf', 'gxrb_gube_nt', 'gxrb_gube_done'
FLAGS = {'ChinaInfantryGeneral': INF, 'ChinaNukeGeneral': NT, 'ChinaTankGeneral': NT}
WAVE = {'_GLA_2ND_WAVE_H': 'B', '_GLA_3RD_WAVE_H': 'C', '_GLA_4TH_WAVE_H': 'D', '_GLA_5TH_WAVE_H': 'E'}
LETTER_AT = {None: 'Now', 25: 'C', 40: 'D', 60: 'E'}
NONE = '<none>'
N_TEAMS = {'SkirmishGLA': 7, 'SkirmishGLADemolitionGeneral': 15, 'SkirmishGLAToxinGeneral': 17}   # = COUNTER.GLA_VS_USA's STOCK_TEAMS
VARIANTS = (('Q', 'GUBQ', 'quad'), ('I', 'GUBI', 'scorp_vs_inf_pm'), ('S', 'GUBS', 'scorp_vs_nuke_pm'))
_STOCK = ('GLA H 2ND Wave Tanks', 'GLA H 3RD Wave Tanks F', 'GLA H 3RD Wave Tanks C', 'GLA H 4TH Wave Tanks F', 'GLA H 4TH Wave Tanks B',
          'GLA H 5TH Wave Tanks F', 'GLA H 5TH Wave Tanks C') + \
    tuple('GLA H %s. %d' % (w, k) for k in (1, 3) for w in ('2ND Wave Tanks', '3RD Wave Tanks F', '3RD Wave Tanks C', '4TH Wave Tanks F',
                                                           '4TH Wave Tanks F 2', '4TH Wave Tanks F 3', '4TH Wave Tanks B', '4TH Wave Tanks B 2',
                                                           '4TH Wave Tanks B 3', '5TH Wave Tanks F', '5TH Wave Tanks F 2', '5TH Wave Tanks F 3',
                                                           '5TH Wave Tanks C', '5TH Wave Tanks C 2', '5TH Wave Tanks C 3')) + \
    tuple('GLA H %s. 3' % w for w in ('5TH Wave Tanks F 4', '5TH Wave Tanks C 4'))
CLAIMS = rb.matchup_claims(SCOPE, _STOCK,
                           scripts=[[o, '%s Gen %s' % (TAG, g)] for o in SCOPE for g in FLAGS] + [[o, TAG + ' Gen Done'] for o in SCOPE] +
                           [[o, 'GLA %s %s %s' % (TAG, k, w)] for o in SCOPE for k, _t, _p in VARIANTS for w in 'BCDE'] +
                           [[o, 'GLA %s Launch %s%s' % (TAG, w, f)] for o in SCOPE for w in ('C', 'D', 'E', 'Now') for f in ('', rb.FORMED)],
                           groups=[[o, GROUP] for o in SCOPE], teams=['GX %s %s' % (t, n) for _k, t, _p in VARIANTS for n in _STOCK])
TEAM_UNIT_WAIVERS = {pre + 'GLAVehicleQuadCannon': 'INIZH: the Arms Dealer the Scorpions it replaces need' for pre in PRE.values()}


def _half_up(x):
    return int(x) + (1 if x - int(x) >= 0.5 else 0)


def _slots(t):
    return [[rb.team_get(t, 'teamUnitType%d' % i), rb.team_get(t, 'teamUnitMinCount%d' % i), rb.team_get(t, 'teamUnitMaxCount%d' % i)]
            for i in (1, 2, 3)]


def _scorpions(o, slots):
    """(min, max) of a team of nothing but this general's Scorpions (a slot with max 0 is empty), else None"""
    full = [s for s in slots if s[2] > 0 and s[0] not in (NONE, None)]
    return (full[0][1], full[0][2]) if len(full) == 1 and full[0][0] == PRE[o] + 'GLATankScorpion' else None


def split(o, mn, mx, share):
    """`share` per mille of a [Scorpion, mn, mx] slot stays Scorpions (rounded half up), the rest Quad Cannons; empty slots dropped"""
    s0, s1 = _half_up(mn * share / 1000.0), _half_up(mx * share / 1000.0)
    out = [[PRE[o] + 'GLATankScorpion', s0, s1], [PRE[o] + 'GLAVehicleQuadCannon', mn - s0, mx - s1]]
    return ([s for s in out if s[2] > 0] + [[NONE, 0, 0]] * 3)[:3]


def _plan(scb, o):
    """[(stock team name, its stock wave gate, (min, max))] of one general's Hard wave 2-5 Scorpion teams"""
    out = []
    for d in rb.teams_ro(scb):
        n = d['teamName']
        if rb.team_get(d, 'teamOwner') != o or not n.startswith('GLA H '):
            continue
        g = rb.mu_stock_gate(scb, o, rb.team_get(d, 'teamProductionCondition'), WAVE)     # another key may have switched it
        mm = None if g is None else _scorpions(o, _slots(d))
        if mm is not None:
            out.append((n, g, mm))
    return out


def apply(scb, scope, p):
    ts = rb.teams(scb)
    share = {'quad': 1000 - p['quad_vs_china_pm'], 'scorp_vs_inf_pm': p['scorp_vs_inf_pm'], 'scorp_vs_nuke_pm': p['scorp_vs_nuke_pm']}
    for o in scope:
        plan = _plan(scb, o)
        assert len(plan) == N_TEAMS[o] and {n for n, *_x in plan} <= set(_STOCK), (o, sorted(n for n, *_x in plan))
        rb.matchup(scb, o)
        f = lambda n, v: rb.flag_is(scb, n, v)
        key = lambda: f(rb.mu_flag(MATCHUP), True)
        # each half's leaves exclude every other half's by a leaf of its own (S: NT ON + INF OFF - the general latch sets
        # one flag, so INF OFF adds nothing there), so a kept stock team and a variant are never producible together (guards.keep_blocks)
        leaves = {'Q': lambda: [key(), f(DONE, True), f(INF, False), f(NT, False)], 'I': lambda: [key(), f(INF, True)],
                  'S': lambda: [key(), f(NT, True), f(INF, False)]}
        kids, made = rb.general_latch(scb, TAG, MATCHUP, FLAGS, DONE), set()
        for n, g, (mn, mx) in plan:
            lo, hi = rb.wave_window(scb, o, g)
            launch = 'GLA %s Launch %s' % (TAG, LETTER_AT[hi])
            if launch not in made:
                kids += rb.launch_scripts(scb, launch, 0 if hi is None else hi)
                made.add(launch)
            rb.matchup_off(scb, ts, o, n, MATCHUP)
            stock, keep = rb.team_slots(rb.team(ts, n)), []
            for k, tag, pk in VARIANTS:
                if rb.same_slots(split(o, mn, mx, share[pk]), stock):        # rb-data-17: keep the stock team, never a copy
                    keep.append(leaves[k])
                    continue
                on = 'GLA %s %s %s' % (TAG, k, WAVE[g])
                if on not in made:
                    kids.append(rb.wave_gate(scb, o, g, on, leaves[k]))
                    made.add(on)
                rb.variant_team(scb, ts, n, 'GX %s %s' % (tag, n), on, split(o, mn, mx, share[pk]), hooks=rb.launch_fields(launch, 0 if hi is None else hi))
            if keep:
                rb.matchup_keep(scb, ts, o, n, MATCHUP, keep)
        rb.owner_list(scb, o).kids.append(rb.new_group(GROUP, kids))
    rb.put_teams(scb, ts)


READ_S = 600
# every variant replaces Scorpions with Quads, so the CONTRAST reads FEWER own Scorpions; against the Nuke or Tank general (default) the
# stock team itself runs (rb-data-17 keep block): no contrast, by design - ON tracks OFF there
CONTRAST = {'what': 'own Scorpions built by 600 s (fewer: the variants field Quad Cannons instead vs vanilla China, an unknown China general '
                    'and the Infantry general; stock Hard GLA builds Scorpions in every wave 2-5)', 'direction': -1, 'min_delta': 3,
            'holds': 'quad_vs_china_pm >= 500 (vanilla / unknown China), scorp_vs_inf_pm <= 500 (Infantry)'}


def measure(obs, me, params=None):
    return ob.built_by(obs, me, ob.ends_with('GLATankScorpion'), READ_S * ob.hz(obs)) if obs else None


def _quads(obs, me):
    return ob.built_by(obs, me, ob.ends_with('GLAVehicleQuadCannon'), READ_S * ob.hz(obs))


def activation(obs, me, params=None):
    """triggered: an own Arms Dealer complete by 300 s and the game ran past 600 s; fired: 3 own Quad Cannons by 600 s and Quads at least 3/4
    of Scorpions. Stock Hard GLA fields Scorpions in every wave 2-5 team: its Quad / Scorpion ratio by 600 s is 0.51 at most (smoke11-13's
    12 OFF games: 9-23 Quads vs 42-63 Scorpions); a half share plus stock wave-1 Scorpions still fires (Codex rbd16-r1 / r2 P2). The
    CONTRAST - ON - OFF of the same seed - is the reading"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    ad = ob.first_frame(obs, me, ob.ends_with('GLAArmsDealer'), 1, complete=True)
    end = ob.last_frame(obs)
    triggered = ad is not None and ad <= 300 * hz and end is not None and end > READ_S * hz
    s, q = measure(obs, me, params), _quads(obs, me)
    return {'triggered': bool(triggered), 'fired': bool(triggered and (q or 0) >= 3 and 4 * (q or 0) >= 3 * (s or 0)),
            'detail': 'arms_dealer_f=%s scorpions_600s=%s quads_600s=%s' % (ad, s, q)}
