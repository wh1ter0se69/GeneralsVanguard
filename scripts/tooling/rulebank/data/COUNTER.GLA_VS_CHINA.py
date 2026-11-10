# GeneralsX: rule bank DATA rule COUNTER.GLA_VS_CHINA: the Toxin general meets the China
# Infantry and Tank GENERALS without Scorpion waves (row 322: "Vs China Infantry: core army is Battle Buses, more Quads and Angry Mobs ...; cut
# Scud Launchers ~80%"; row 324: "Vs China Tank Emperors: Gamma Battle Buses or Jarmen Kell; Buses beat Emperors"; Dominator: "It should
# have been Battle Buses, more quads and more angry mobs"). Stock Hard Toxin sends Scorpion waves at everyone.
# rb-data-19: THE BUSES ARE OUT, a data bug (smoke17: 5 of 131 left home, JOB HURTS vs Infantry). An empty Bus has no ground weapon, so TEAM_HUNT left it home; the rows mean loaded ones, and a loaded
# one costs $3,500 ($1,100 + 8 Tunnel Defenders at $300), which a wave team's money does not reach. guards.team_hooks now refuses a hunting team with a Bus. This, for Toxin:
#   MATCHUP 'China': the SHARED latch (rb.matchup) sets gxrb_mu_china once AI_PLAYER_GX_SENSE says my enemy is China;
#   a GENERAL read of its own (rb.general_latch, in the frame the key is set): gxrb_gvc_inf / gxrb_gvc_tank once the sensor says my
#              enemy is the Infantry / Tank general - which it says only when the LOBBY showed that general (a Random enemy is never
#              known by its general) - and gxrb_gvc_done either way, so it decides once;
#   each Hard wave 2-5 team of nothing but Scorpions (the 17 COUNTER.GLA_VS_USA switches for USA: the two keys share them) waits on its
#              off gate and gets a variant vs Infantry (Mob and Jarmen = Barracks + Palace, INIZH):
#     vs Infantry "GX GVC <team>": all Quads; mob_wave 1 adds an Angry Mob slot (0-1), behind an own Palace and an own Barracks
#              (AIPlayer::isPossibleToBuildTeam checks every type whatever its minimum);
#     vs Tank the Scorpions stay stock. jarmen 1 adds ONE team "GX GVCK <stock
#              Kell team>" (one instance) of one Jarmen Kell from stock wave 3's start on (own Palace + Barracks, no own Kell alive): the Kell is one per player
#              (MaxSimultaneousOfType 1), so a Kell slot in every Tank team would hold each team its whole teamInitialIdleFrames while
#              one lives. Stock's own wave 4-5 Kell teams stay;
#     the STOCK team itself (rb-data-17, the lead's stock-copy ruling; until rb-data-16 a "GX GVCS" copy of its slots on the own clock,
#              which fielded 40 vs stock's 54 Scorpions vs Nuke by 600 s): its off gate keeps it on (rb.matchup_keep: [stock gate,
#              gxrb_mu_china ON, ...]) against any China enemy but the Infantry general, and while a building
#              the Infantry half waits on is down (one keep block per building), so exactly one of the variant or the stock team is producible
#              at any moment the key is ON;
#   each Scud-only team (waves 4-5) keeps scud_cut % of its Scuds (rounded half up) vs Infantry ("GX GVC <team>"; NO Scud team at all
#              when that rounds to 0 - scud_cut 0, or 20 % of 2: the cut is the point of row 322, read as intended) and
#              stays the stock team against any other China enemy (a keep block).
#   Each variant waits through its wave's OWN-CLOCK twin (scbx.wave_window) and hunts once the next wave would launch (wave 5 at once);
#   the kept stock team runs stock's own gate and hooks.
# No stock script changes. Mixed, wave-1, Bus, Kell and Mob stock teams stay stock.
RULE_ID = 'COUNTER.GLA_VS_CHINA'
MATCHUP = 'China'
PARAMS = ('mob_wave', 'scud_cut', 'jarmen')
O, P = 'SkirmishGLAToxinGeneral', 'Chem_'
SCOPE = (O,)
TAG, GROUP = 'GX GVC', 'GX COUNTER.GLA_VS_CHINA'
FLAGS = {'ChinaInfantryGeneral': 'gxrb_gvc_inf', 'ChinaTankGeneral': 'gxrb_gvc_tank'}
INF, TANK, DONE = 'gxrb_gvc_inf', 'gxrb_gvc_tank', 'gxrb_gvc_done'
WAVE = {'_GLA_2ND_WAVE_H': 'B', '_GLA_3RD_WAVE_H': 'C', '_GLA_4TH_WAVE_H': 'D', '_GLA_5TH_WAVE_H': 'E'}
LETTER_AT = {None: 'Now', 25: 'C', 40: 'D', 60: 'E'}
S, Q, MOB, KELL, SCUD, NONE = (P + 'GLATankScorpion', P + 'GLAVehicleQuadCannon', P + 'GLAInfantryAngryMobNexus',
                               P + 'GLAInfantryJarmenKell', P + 'GLAVehicleScudLauncher', '<none>')
PALACE, BARRACKS = P + 'GLAPalace', P + 'GLABarracks'
SCORP = ('GLA H 2ND Wave Tanks. 3', 'GLA H 3RD Wave Tanks F. 3', 'GLA H 3RD Wave Tanks C. 3', 'GLA H 4TH Wave Tanks F. 3', 'GLA H 4TH Wave Tanks F 2. 3',
         'GLA H 4TH Wave Tanks F 3. 3', 'GLA H 4TH Wave Tanks B. 3', 'GLA H 4TH Wave Tanks B 2. 3', 'GLA H 4TH Wave Tanks B 3. 3', 'GLA H 5TH Wave Tanks F. 3',
         'GLA H 5TH Wave Tanks F 2. 3', 'GLA H 5TH Wave Tanks F 3. 3', 'GLA H 5TH Wave Tanks F 4. 3', 'GLA H 5TH Wave Tanks C. 3', 'GLA H 5TH Wave Tanks C 2. 3',
         'GLA H 5TH Wave Tanks C 3. 3', 'GLA H 5TH Wave Tanks C 4. 3')
SCUDS = ('GLA H 4TH Wave Artillery F. 3', 'GLA H 4TH Wave Artillery B. 3', 'GLA H 5TH Wave Artillery F. 3', 'GLA H 5TH Wave Artillery B. 3')
_ALL = sorted(SCORP + SCUDS)
KELL_DONOR, KELL_GATE = 'GLA H 4TH Wave Kell. 3', 'GLA GX GVC Kell'      # the stock Kell team: owner, priority, idle frames (hooks blanked)
CLAIMS = rb.matchup_claims(SCOPE, _ALL,
                           scripts=[[O, '%s Gen %s' % (TAG, g)] for g in FLAGS] + [[O, TAG + ' Gen Done']] +
                           [[O, 'GLA GX GVC %s %s' % (k, w)] for k in ('Inf', 'Scud Inf') for w in 'BCDE'] +
                           [[O, 'GLA GX GVC Launch %s%s' % (w, f)] for w in ('C', 'D', 'E', 'Now') for f in ('', rb.FORMED)] + [[O, KELL_GATE]],
                           groups=[[O, GROUP]], teams=['GX GVC %s' % n for n in _ALL] + ['GX GVCK ' + KELL_DONOR])
TEAM_UNIT_WAIVERS = {MOB: 'INIZH: Barracks + Palace - behind own-Palace and own-Barracks leaves (Codex rbd15-r1 P1)',
                     KELL: 'INIZH: Barracks + Palace - its one team waits on both buildings', Q: 'INIZH: the Arms Dealer the Scorpions it replaces need'}


def _half_up(x):
    return int(x) + (1 if x - int(x) >= 0.5 else 0)


def _slots(t):
    return [[rb.team_get(t, 'teamUnitType%d' % i), rb.team_get(t, 'teamUnitMinCount%d' % i), rb.team_get(t, 'teamUnitMaxCount%d' % i)]
            for i in (1, 2, 3)]


def _only(t, typ):
    """(min, max) of a team of nothing but `typ` (a slot with max 0 is empty), else None"""
    full = [s for s in _slots(t) if s[2] > 0 and s[0] not in (NONE, None)]
    return (full[0][1], full[0][2]) if len(full) == 1 and full[0][0] == typ else None


def _pad(slots):
    return ([list(s) for s in slots if s[2] > 0] + [[NONE, 0, 0]] * 3)[:3]


def inf_slots(mn, mx, p):
    return _pad([[Q, mn, mx]] + ([[MOB, 0, 1]] if p['mob_wave'] else []))


def scud_slots(mn, mx, p):
    c = p['scud_cut'] / 100.0
    return _pad([[SCUD, _half_up(mn * c), _half_up(mx * c)]])


def _gate(scb, g, name, blocks):
    """a production condition: the stock wave gate's header and actions (rb.wave_gate), one OR block per entry of `blocks`, each the
    wave's own-clock window plus that entry's leaves"""
    lo, hi = rb.wave_window(scb, O, g)
    win = lambda: ([rb.clock_at_least(scb, lo)] if lo else []) + \
        ([rb.cond(scb, 'COUNTER', ('COUNTER', rb.GX_CLOCK), ('COMPARISON', rb.LT), ('INT', hi))] if hi else [])
    s = rb.wave_gate(scb, O, g, name, lambda: [])
    acts = [k for k in s.kids if k.name != 'OrCondition']
    s.kids = [mss.Node('OrCondition', 1, kids=win() + b()) for b in blocks] + acts
    return s


def apply(scb, scope, p):
    assert tuple(scope) == SCOPE, scope
    ts = rb.teams(scb)
    rb.matchup(scb, O)
    f = lambda n, v: rb.flag_is(scb, n, v)
    key = lambda: f(rb.mu_flag(MATCHUP), True)
    has, lost = (lambda b: rb.own_count(scb, rb.GE, 1, b)), (lambda b: rb.own_count(scb, rb.EQ, 0, b))
    # the buildings the NEW unit types need beyond the Arms Dealer the stock Scorpions need too (INIZH)
    inf_need = [PALACE, BARRACKS] if p['mob_wave'] else []
    phases = {      # name: [OR blocks]; each block a function returning its leaves after the window
        'Inf': [lambda: [key(), f(INF, True)] + [has(b) for b in inf_need]],
        # the stock team's keep blocks (rb-data-17)
        'Stock': [lambda: [key(), f(DONE, True), f(INF, False)]] +
                 [(lambda b=b: [key(), f(INF, True), lost(b)]) for b in inf_need],
        'Scud Inf': [lambda: [key(), f(INF, True)]],
        'Scud Stock': [lambda: [key(), f(DONE, True), f(INF, False)]]}
    kids, made = rb.general_latch(scb, TAG, MATCHUP, FLAGS, DONE), set()
    seen = []
    for d in rb.teams_ro(scb):
        n = d['teamName']
        if rb.team_get(d, 'teamOwner') != O or n not in _ALL:
            continue
        g = rb.mu_stock_gate(scb, O, rb.team_get(d, 'teamProductionCondition'), WAVE)     # another key may have switched it
        assert g is not None, n
        t = rb.team(ts, n)
        if n in SCORP:
            mm = _only(t, S)
            want = [('Inf', 'GVC', inf_slots(*mm, p)), ('Stock', None, _pad(_slots(t)))]
        else:
            mm = _only(t, SCUD)
            cut = scud_slots(*mm, p)
            want = ([('Scud Inf', 'GVC', cut)] if cut[0][2] else []) + [('Scud Stock', None, _pad(_slots(t)))]
        assert mm is not None, n
        seen.append(n)
        lo, hi = rb.wave_window(scb, O, g)
        launch = 'GLA GX GVC Launch %s' % LETTER_AT[hi]
        if launch not in made:
            kids += rb.launch_scripts(scb, launch, 0 if hi is None else hi)
            made.add(launch)
        rb.matchup_off(scb, ts, O, n, MATCHUP)
        keep = []
        for ph, tag, slots in want:
            if rb.same_slots(slots, _slots(t)):                     # rb-data-17: keep the stock team on, never a copy
                keep += phases[ph]
                continue
            on = 'GLA GX GVC %s %s' % (ph, WAVE[g])
            if on not in made:
                kids.append(_gate(scb, g, on, phases[ph]))
                made.add(on)
            rb.variant_team(scb, ts, n, 'GX %s %s' % (tag, n), on, slots, hooks=rb.launch_fields(launch, 0 if hi is None else hi))
        if keep:
            rb.matchup_keep(scb, ts, O, n, MATCHUP, keep)
    assert sorted(seen) == _ALL, sorted(set(_ALL) - set(seen))
    if p['jarmen']:
        g = rb.wave_gate(scb, O, '_GLA_3RD_WAVE_H', KELL_GATE, lambda: [])
        # only while no own Kell lives, and ONE instance. rb-data-18: both are belt and braces - a live or queued Kell makes
        # Player::canBuild false (canBuildMoreOfType), so AIPlayer::findFactory finds no factory and the team is no candidate at all
        g.kids = [mss.Node('OrCondition', 1, kids=[rb.clock_at_least(scb, rb.wave_window(scb, O, '_GLA_3RD_WAVE_H')[0]), key(), f(TANK, True),
                                                   has(PALACE), has(BARRACKS), lost(KELL)])] + [k for k in g.kids if k.name != 'OrCondition']
        kids.append(g)
        if 'GLA GX GVC Launch Now' not in made:
            kids += rb.launch_scripts(scb, 'GLA GX GVC Launch Now', 0)
        kt = rb.variant_team(scb, ts, KELL_DONOR, 'GX GVCK ' + KELL_DONOR, KELL_GATE, [[KELL, 1, 1], [NONE, 0, 0], [NONE, 0, 0]],
                             hooks=rb.launch_fields('GLA GX GVC Launch Now', 0))
        rb.team_set(scb, kt, 'teamMaxInstances', 1)
    rb.owner_list(scb, O).kids.append(rb.new_group(GROUP, kids))
    rb.put_teams(scb, ts)


READ_S = 600
# the CONTRAST reads FEWER own Scorpions (vs the Tank or Nuke general or a Random China enemy the stock team itself runs: no contrast, by design)
CONTRAST = {'what': 'own Scorpions built by 600 s (fewer: the variants replace them with Quads and Mobs; stock Hard Toxin builds them in '
                    'every wave)', 'direction': -1, 'min_delta': 3}


def measure(obs, me, params=None):
    return ob.built_by(obs, me, ob.ends_with('GLATankScorpion'), READ_S * ob.hz(obs)) if obs else None


def activation(obs, me, params=None):
    """triggered: an own Palace complete by 540 s and the game ran past 600 s; fired: 27 own Quads or more by 600 s (smoke18, Toxin vs the Infantry general: 32-54 with the rule, 12-22 stock). The
    Tank half changes no wave team: its one early Kell has no contrast, so it never reads fired"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    pal = ob.first_frame(obs, me, ob.ends_with('GLAPalace'), 1, complete=True)
    end = ob.last_frame(obs)
    triggered = pal is not None and pal <= 540 * hz and end is not None and end > READ_S * hz
    n = ob.built_by(obs, me, ob.ends_with('GLAVehicleQuadCannon'), READ_S * hz)
    return {'triggered': bool(triggered), 'fired': bool(triggered and (n or 0) >= 27),
            'detail': 'palace_f=%s quads_600s=%s scorpions_600s=%s' % (pal, n, measure(obs, me, params))}
