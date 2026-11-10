# GeneralsX: rule bank DATA rule COUNTER.GLA_VS_USA: GLA meets USA with Rocket Buggies once its
# Palace stands (rows 273, 284, 435, 500, 620, 1015, 1785; Dominator: "against Humvees, hold with Tunnels/Technicals into Palace Buggies.
# Use Quads against aircraft"). Stock Hard GLA sends Scorpion waves at everyone; its only Rocket Buggies are wave 4's two artillery ones.
# This, per general:
#   MATCHUP 'USA': the SHARED latch (rb.matchup, group "GX Matchup") sets gxrb_mu_usa once AI_PLAYER_GX_SENSE says my enemy is USA - from
#              the lobby at once, a Random enemy only after 3 different units of one army were fairly seen; it decides ONCE;
#   each Hard wave 2-5 team of nothing but this general's Scorpions gets a variant "GX GVU <team>": its Scorpions become buggy_n Rocket
#              Buggies at most, the rest Quads (anti-air vs USA). The Rocket Buggy needs an Arms Dealer AND a Palace (INIZH), and the
#              stock team waits on its off gate "GX MU Off <team>", so with palace_gate 1 the buggy variant waits on an own Palace and the
#              STOCK team stays on while there is none (rb-data-17, the lead's stock-copy ruling: the off gate's keep block [stock gate,
#              gxrb_mu_usa ON, no own Palace] - until rb-data-16 a "GX GVUE" copy of its slots on the own clock); palace_gate 0: the buggy
#              variant alone (AIPlayer::isPossibleToBuildTeam skips it until the Palace stands - no Scorpion wave team before that);
#   each variant waits on gxrb_mu_usa ON through the wave's OWN-CLOCK twin (scbx.wave_window: stock waves follow _ESCALATION, an enemy
#              read); its launch (rb.launch_scripts): TEAM_HUNT once the next wave would launch (at once in wave 5). No stock hook is named.
# The Stealth general fields no Scorpion (its waves are Combat Bikes and Quads): out of SCOPE. A Demolition team that names the plain
# GLATankScorpion (stock data; that general never builds one) stays stock.
# Not built (UNUSED, defaults only): scud_esc_vs_usaaf - an earlier Scud Storm vs the Air Force GENERAL needs a general-level key, and the
# Dominator review dropped it ("without an automatic earlier Air Force timing"); behind_income_ratio - its box is closed at 100 (no gate):
# "behind" would read the enemy economy, which no fair sensor gives.
RULE_ID = 'COUNTER.GLA_VS_USA'
MATCHUP = 'USA'
PARAMS = ('buggy_n', 'palace_gate')
SCOPE = ('SkirmishGLA', 'SkirmishGLADemolitionGeneral', 'SkirmishGLAToxinGeneral')
PRE = {'SkirmishGLA': '', 'SkirmishGLADemolitionGeneral': 'Demo_', 'SkirmishGLAToxinGeneral': 'Chem_'}
UNUSED = {'scud_esc_vs_usaaf': 'not built: an earlier Scud Storm vs the Air Force general needs a general-level key (the latch has base '
                               'sides only), and the Dominator review dropped the automatic earlier Air Force timing',
          'behind_income_ratio': 'closed at 100 (no gate): "behind" reads the enemy economy, which no fair sensor gives'}
TAG, GROUP = 'GVU', 'GX COUNTER.GLA_VS_USA'
WAVE = {'_GLA_2ND_WAVE_H': 'B', '_GLA_3RD_WAVE_H': 'C', '_GLA_4TH_WAVE_H': 'D', '_GLA_5TH_WAVE_H': 'E'}
LETTER_AT = {None: 'Now', 25: 'C', 40: 'D', 60: 'E'}
NONE = '<none>'
STOCK_TEAMS = {'SkirmishGLA': ('GLA H 2ND Wave Tanks', 'GLA H 3RD Wave Tanks F', 'GLA H 3RD Wave Tanks C', 'GLA H 4TH Wave Tanks F',
                               'GLA H 4TH Wave Tanks B', 'GLA H 5TH Wave Tanks F', 'GLA H 5TH Wave Tanks C'),
               'SkirmishGLADemolitionGeneral': ('GLA H 2ND Wave Tanks. 1', 'GLA H 3RD Wave Tanks F. 1', 'GLA H 3RD Wave Tanks C. 1',
                                                'GLA H 4TH Wave Tanks F. 1', 'GLA H 4TH Wave Tanks F 2. 1', 'GLA H 4TH Wave Tanks F 3. 1',
                                                'GLA H 4TH Wave Tanks B. 1', 'GLA H 4TH Wave Tanks B 2. 1', 'GLA H 4TH Wave Tanks B 3. 1',
                                                'GLA H 5TH Wave Tanks F. 1', 'GLA H 5TH Wave Tanks F 2. 1', 'GLA H 5TH Wave Tanks F 3. 1',
                                                'GLA H 5TH Wave Tanks C. 1', 'GLA H 5TH Wave Tanks C 2. 1', 'GLA H 5TH Wave Tanks C 3. 1'),
               'SkirmishGLAToxinGeneral': ('GLA H 2ND Wave Tanks. 3', 'GLA H 3RD Wave Tanks F. 3', 'GLA H 3RD Wave Tanks C. 3',
                                           'GLA H 4TH Wave Tanks F. 3', 'GLA H 4TH Wave Tanks F 2. 3', 'GLA H 4TH Wave Tanks F 3. 3',
                                           'GLA H 4TH Wave Tanks B. 3', 'GLA H 4TH Wave Tanks B 2. 3', 'GLA H 4TH Wave Tanks B 3. 3',
                                           'GLA H 5TH Wave Tanks F. 3', 'GLA H 5TH Wave Tanks F 2. 3', 'GLA H 5TH Wave Tanks F 3. 3',
                                           'GLA H 5TH Wave Tanks F 4. 3', 'GLA H 5TH Wave Tanks C. 3', 'GLA H 5TH Wave Tanks C 2. 3',
                                           'GLA H 5TH Wave Tanks C 3. 3', 'GLA H 5TH Wave Tanks C 4. 3')}
_ALL = sorted({n for ns in STOCK_TEAMS.values() for n in ns})
CLAIMS = rb.matchup_claims(SCOPE, _ALL,
                           scripts=[[o, 'GLA GX %s Buggy %s' % (TAG, w)] for o in SCOPE for w in 'BCDE'] +
                           [[o, 'GLA GX %s Launch %s%s' % (TAG, w, f)] for o in SCOPE for w in ('C', 'D', 'E', 'Now') for f in ('', rb.FORMED)],
                           groups=[[o, GROUP] for o in SCOPE], teams=['GX %s %s' % (TAG, n) for n in _ALL])
TEAM_UNIT_WAIVERS = {pre + 'GLAVehicleRocketBuggy': 'INIZH: Arms Dealer + Palace - the variant waits on an own Palace (palace_gate 1), and '
                     'AIPlayer::isPossibleToBuildTeam skips it while the Arms Dealer is down' for pre in PRE.values()}
TEAM_UNIT_WAIVERS.update({pre + 'GLAVehicleQuadCannon': 'INIZH: the Arms Dealer the Scorpions it replaces need' for pre in PRE.values()})


def _swap(o, slots, b):
    """(buggy slots, early slots) of a team of nothing but this general's Scorpions, or None when the team stays stock"""
    pre = PRE[o]
    S, RB, Q = pre + 'GLATankScorpion', pre + 'GLAVehicleRocketBuggy', pre + 'GLAVehicleQuadCannon'
    slots = [s if s[2] > 0 else [NONE, 0, 0] for s in slots]       # a slot with max 0 is empty
    full = [s for s in slots if s[0] not in (NONE, None)]
    if len(full) != 1 or full[0][0] != S:
        return None
    (_t, mn, mx) = full[0]
    out = [[RB, min(mn, b), min(mx, b)]] + ([[Q, mn - min(mn, b), mx - min(mx, b)]] if mx > b else [])
    return (out + [[NONE, 0, 0]] * 3)[:3], [list(s) for s in slots]


def _slots(t):
    return [[rb.team_get(t, 'teamUnitType%d' % i), rb.team_get(t, 'teamUnitMinCount%d' % i), rb.team_get(t, 'teamUnitMaxCount%d' % i)]
            for i in (1, 2, 3)]


def _plan(scb, o, p):
    """[(stock team name, its stock wave gate, (buggy slots, early slots))] of one general's Hard wave 2-5 Scorpion teams"""
    out = []
    for d in rb.teams_ro(scb):
        n = d['teamName']
        if rb.team_get(d, 'teamOwner') != o or not n.startswith('GLA H '):
            continue
        g = rb.mu_stock_gate(scb, o, rb.team_get(d, 'teamProductionCondition'), WAVE)    # another key may have switched it
        if g is None:
            continue
        sw = _swap(o, _slots(d), p['buggy_n'])
        if sw is not None:
            out.append((n, g, sw))
    return out


def apply(scb, scope, p):
    ts = rb.teams(scb)
    for o in scope:
        plan = _plan(scb, o, p)
        assert sorted(n for n, *_x in plan) == sorted(STOCK_TEAMS[o]), (o, sorted(n for n, *_x in plan))
        rb.matchup(scb, o)
        palace = PRE[o] + 'GLAPalace'
        key = lambda: rb.flag_is(scb, rb.mu_flag(MATCHUP), True)
        phases = [('Buggy', TAG, lambda: [key(), rb.own_count(scb, rb.GE, 1, palace)]), ('Early', TAG + 'E', lambda: [key(), rb.own_count(scb, rb.EQ, 0, palace)])] \
            if p['palace_gate'] else [('Buggy', TAG, lambda: [key()])]
        kids, made = [], set()
        for n, g, sw in plan:
            lo, hi = rb.wave_window(scb, o, g)
            launch = 'GLA GX %s Launch %s' % (TAG, LETTER_AT[hi])
            if launch not in made:
                kids += rb.launch_scripts(scb, launch, 0 if hi is None else hi)
                made.add(launch)
            rb.matchup_off(scb, ts, o, n, MATCHUP)
            keep = []
            for (k, tag, extra), slots in zip(phases, sw):
                if rb.same_slots(slots, rb.team_slots(rb.team(ts, n))):       # rb-data-17: keep the stock team on, never a copy
                    keep.append(extra)
                    continue
                on = 'GLA GX %s %s %s' % (TAG, k, WAVE[g])
                if on not in made:
                    kids.append(rb.wave_gate(scb, o, g, on, extra))
                    made.add(on)
                rb.variant_team(scb, ts, n, 'GX %s %s' % (tag, n), on, slots, hooks=rb.launch_fields(launch, 0 if hi is None else hi))
            if keep:
                rb.matchup_keep(scb, ts, o, n, MATCHUP, keep)
        rb.owner_list(scb, o).kids.append(rb.new_group(GROUP, kids))
    rb.put_teams(scb, ts)


# stock Hard GLA's Palace stands at ~392-398 s (smoke11's four OFF games, vanilla GLA vs China): the buggy waves start after it
READ_S = 600
CONTRAST = {'what': 'own Rocket Buggies built by 600 s (stock: wave 4\'s artillery team of 2, more only when a destroyed team is rebuilt)',
            'direction': 1, 'min_delta': 3}


def _rb(name):
    return name.endswith('GLAVehicleRocketBuggy')


def measure(obs, me, params=None):
    return ob.built_by(obs, me, _rb, READ_S * ob.hz(obs)) if obs else None


def activation(obs, me, params=None):
    """triggered: an own Palace complete by 540 s and the game ran past 600 s; fired: 3 own Rocket Buggies by 600 s. Stock's wave 4
    artillery team holds 2, but a destroyed team is rebuilt, so stock can reach 3 too (Codex rbd14-r1 P2): the activation alone cannot
    tell ON from OFF - the CONTRAST (ON - OFF of the same seed) reads it"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    pal = ob.first_frame(obs, me, ob.ends_with('GLAPalace'), 1, complete=True)
    end = ob.last_frame(obs)
    triggered = pal is not None and pal <= 540 * hz and end is not None and end > READ_S * hz     # built_by needs a frame past READ_S
    n = measure(obs, me, params)
    fired = triggered and (n or 0) >= 3
    return {'triggered': bool(triggered), 'fired': bool(fired), 'detail': 'palace_f=%s buggies_600s=%s' % (pal, n)}
