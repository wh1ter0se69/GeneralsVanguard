# GeneralsX: rule bank DATA rule COUNTER.CHINA_VS_CHINA: the Nuke general meets China with the
# "Nuke mix" - Overlords, ECM tanks and Gattlings (rows 212, 220, 248, 487, 497, 498, 1044, 1260, 1262, 1983; "it tends to be a battle of
# helixes against overlords, ECMs and Gatlings mixed in ... the Nuke mix, there's very little China can do to break through"). Stock Hard
# Nuke sends Battlemaster waves at everyone. This:
#   MATCHUP 'China': the SHARED latch (rb.matchup, group "GX Matchup") sets gxrb_mu_china once AI_PLAYER_GX_SENSE says my enemy is China
#              (any China general) - from the lobby at once, a Random enemy only after 3 different units of one army were fairly seen; it
#              decides ONCE;
#   each Hard wave team of the Nuke general made of Battlemasters (and Overlords) only, with no minimum, gets a variant "GX CVC <team>":
#              its tanks become ecm_share % ECM tanks, gattling_share % Gattlings (each rounded half up) and Overlords for the rest. The
#              Overlord and the ECM tank need a Propaganda Center (INIZH) and the stock team waits on its off gate "GX MU Off <team>",
#              so the variant waits on an own Propaganda Center and the STOCK team stays on while there is none (rb-data-17, the lead's
#              stock-copy ruling: the off gate's keep block [stock gate, gxrb_mu_china ON, no own Center] - until rb-data-16 a "GX CVCE"
#              copy of its slots on the own clock) - only for a team of Battlemasters alone: a stock team that names an Overlord needs the
#              Center already (wave 4), so it keeps nothing and, like stock, fields nothing while the Center is down;
#   each variant waits on gxrb_mu_china ON through the wave's OWN-CLOCK twin (scbx.wave_window); its launch (rb.launch_scripts): TEAM_HUNT once the next
#              wave would launch. No stock hook is named.
# Not built (UNUSED, defaults only): migs_vs_emperor and nukemig_upgrade (Nuke MiGs vs the TANK general) and ecm_n (the Tank general's mass
# ECM vs the NUKE general) - each aims at one enemy GENERAL, which the shared latch (base sides only) cannot tell apart; the Tank general's
# half is out of SCOPE for the same reason. The wave-5 team with required minimums (Battlemaster 2, Overlord 1, ECM 1) stays stock.
RULE_ID = 'COUNTER.CHINA_VS_CHINA'
MATCHUP = 'China'
PARAMS = ('ecm_share', 'gattling_share')
SCOPE = ('SkirmishChinaNukeGeneral',)
PRE = {'SkirmishChinaNukeGeneral': 'Nuke_'}
UNUSED = {k: 'not built: aimed at one enemy GENERAL (Nuke MiGs vs Tank / mass ECM vs Nuke); the shared latch knows base sides only'
          for k in ('migs_vs_emperor', 'nukemig_upgrade', 'ecm_n')}
TAG, GROUP = 'CVC', 'GX COUNTER.CHINA_VS_CHINA'
WAVE = {'_CHINA_1ST_WAVE_H': 'A', '_CHINA_2ND_WAVE_H': 'B', '_CHINA_3RD_WAVE_H': 'C', '_CHINA_4TH_WAVE_H': 'D', '_CHINA_5TH_WAVE_H': 'E'}
LETTER_AT = {None: 'Now', 15: 'B', 25: 'C', 40: 'D', 60: 'E'}
NONE = '<none>'
STOCK_TEAMS = {'SkirmishChinaNukeGeneral': ('China H 2ND Wave Tanks. 2', 'China H 3RD Wave Tanks F. 2', 'China H 3RD Wave Tanks C. 2',
                                            'China H 4TH Wave Tanks F. 2', 'China H 4TH Wave Tanks B. 2')}
_ALL = sorted({n for ns in STOCK_TEAMS.values() for n in ns})
CLAIMS = rb.matchup_claims(SCOPE, _ALL,
                           scripts=[[o, 'China GX %s Mix %s' % (TAG, w)] for o in SCOPE for w in 'BCD'] +
                           [[o, 'China GX %s Launch %s%s' % (TAG, w, f)] for o in SCOPE for w in ('C', 'D', 'E') for f in ('', rb.FORMED)],
                           groups=[[o, GROUP] for o in SCOPE],
                           teams=['GX %s %s' % (TAG, n) for n in _ALL])
TEAM_UNIT_WAIVERS = {'Nuke_ChinaTankOverlord': 'INIZH: War Factory + Propaganda Center - the variant waits on an own Propaganda Center',
                     'Nuke_ChinaTankECM': 'INIZH: War Factory + Propaganda Center - the variant waits on an own Propaganda Center',
                     'Nuke_ChinaTankGattling': 'INIZH: the War Factory the Battlemasters it replaces need'}


def _half_up(n, pct):
    """round(n * pct / 100), halves up"""
    return (2 * n * pct + 100) // 200


def _swap(o, slots, p):
    """(mix slots, early slots) of a team of nothing but Battlemasters and Overlords with no minimum, or None when the team stays stock.
    early slots None: the stock team names an Overlord, so it needs the Propaganda Center already - an early copy could never be built
    (AIPlayer::isPossibleToBuildTeam checks every type, whatever its minimum; Codex rbd14-r1 P2)"""
    pre = PRE[o]
    BM, OL, ECM, G = pre + 'ChinaTankBattleMaster', pre + 'ChinaTankOverlord', pre + 'ChinaTankECM', pre + 'ChinaTankGattling'
    slots = [s if s[2] > 0 else [NONE, 0, 0] for s in slots]       # a slot with max 0 is empty
    full = [s for s in slots if s[0] not in (NONE, None)]
    if BM not in [s[0] for s in full] or any(s[0] not in (BM, OL) for s in full) or any(s[1] for s in full):
        return None
    n = sum(s[2] for s in full)
    e = _half_up(n, p['ecm_share'])
    g = min(_half_up(n, p['gattling_share']), n - e)
    out = [[t, 0, k] for t, k in ((OL, n - e - g), (ECM, e), (G, g)) if k > 0]
    return (out + [[NONE, 0, 0]] * 3)[:3], (None if OL in [s[0] for s in full] else [list(s) for s in slots])


def _slots(t):
    return [[rb.team_get(t, 'teamUnitType%d' % i), rb.team_get(t, 'teamUnitMinCount%d' % i), rb.team_get(t, 'teamUnitMaxCount%d' % i)]
            for i in (1, 2, 3)]


def _plan(scb, o, p):
    """[(stock team name, its stock wave gate, (mix slots, early slots))] of the general's Hard Battlemaster wave teams"""
    out = []
    for d in rb.teams_ro(scb):
        n = d['teamName']
        if rb.team_get(d, 'teamOwner') != o or not n.startswith('China H '):
            continue
        g = rb.mu_stock_gate(scb, o, rb.team_get(d, 'teamProductionCondition'), WAVE)    # another key may have switched it
        if g is None:
            continue
        sw = _swap(o, _slots(d), p)
        if sw is not None:
            out.append((n, g, sw))
    return out


def apply(scb, scope, p):
    ts = rb.teams(scb)
    for o in scope:
        plan = _plan(scb, o, p)
        assert sorted(n for n, *_x in plan) == sorted(STOCK_TEAMS[o]), (o, sorted(n for n, *_x in plan))
        rb.matchup(scb, o)
        pc = PRE[o] + 'ChinaPropagandaCenter'
        key = lambda: rb.flag_is(scb, rb.mu_flag(MATCHUP), True)
        phases = [('Mix', TAG, lambda: [key(), rb.own_count(scb, rb.GE, 1, pc)]), ('Early', TAG + 'E', lambda: [key(), rb.own_count(scb, rb.EQ, 0, pc)])]
        kids, made = [], set()
        for n, g, sw in plan:
            lo, hi = rb.wave_window(scb, o, g)
            launch = 'China GX %s Launch %s' % (TAG, LETTER_AT[hi])
            if launch not in made:
                kids += rb.launch_scripts(scb, launch, 0 if hi is None else hi)
                made.add(launch)
            rb.matchup_off(scb, ts, o, n, MATCHUP)
            keep = []
            for (k, tag, extra), slots in zip(phases, sw):
                if slots is None:
                    continue
                if rb.same_slots(slots, rb.team_slots(rb.team(ts, n))):       # rb-data-17: keep the stock team on, never a copy
                    keep.append(extra)
                    continue
                on = 'China GX %s %s %s' % (TAG, k, WAVE[g])
                if on not in made:
                    kids.append(rb.wave_gate(scb, o, g, on, extra))
                    made.add(on)
                rb.variant_team(scb, ts, n, 'GX %s %s' % (tag, n), on, slots, hooks=rb.launch_fields(launch, 0 if hi is None else hi))
            if keep:
                rb.matchup_keep(scb, ts, o, n, MATCHUP, keep)
        rb.owner_list(scb, o).kids.append(rb.new_group(GROUP, kids))
    rb.put_teams(scb, ts)


READ_S = 600
CONTRAST = {'what': 'own Overlords + ECM tanks built by 600 s (stock: wave 4\'s 0-1 Overlords and the 1-ECM guard teams)', 'direction': 1,
            'min_delta': 3}


def _ol_ecm(name):
    return name.endswith('ChinaTankOverlord') or name.endswith('ChinaTankECM')


def measure(obs, me, params=None):
    return ob.built_by(obs, me, _ol_ecm, READ_S * ob.hz(obs)) if obs else None


def activation(obs, me, params=None):
    """triggered: an own Propaganda Center complete by 540 s and the game ran to 600 s; fired: 3 own Overlords / ECM tanks by 600 s - the
    activation alone cannot tell ON from OFF (stock fields a few of each): the CONTRAST reads it"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    pc = ob.first_frame(obs, me, ob.ends_with('ChinaPropagandaCenter'), 1, complete=True)
    end = ob.last_frame(obs)
    triggered = pc is not None and pc <= 540 * hz and end is not None and end > READ_S * hz      # built_by needs a frame past READ_S
    n = measure(obs, me, params)
    fired = triggered and (n or 0) >= 3
    return {'triggered': bool(triggered), 'fired': bool(fired), 'detail': 'propaganda_f=%s overlord_ecm_600s=%s' % (pc, n)}
