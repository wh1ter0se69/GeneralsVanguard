# Copyright 2026 GeneralsX contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# rule bank DATA rule COUNTER.GLA_VS_GLA: the Toxin and Stealth generals meet each other
# (rows 328, 1168, 1762-1763, 2052: "Vs Stealth, Toxin has the stronger early army ... get into the enemy base early with a lot of Scorpions";
# rows 306, 308: "Vs Toxin ... Rocket Buggies pick off Toxin units well ... mass Buggies and survive the early game"; Dominator: "Toxin vs
# Stealth: push early with double-Dealer Scorpions. Stealth vs Toxin: defend into Buggies/Mobs"). This, per general:
#   MATCHUP 'GLA': the SHARED latch (rb.matchup) sets gxrb_mu_gla once AI_PLAYER_GX_SENSE says my enemy is GLA (lobby at once; a Random enemy
#              after 3 different units of one army were fairly seen); it decides ONCE;
#   a GENERAL read of its own (rb.general_latch, in the frame the key is set): Toxin's gxrb_ggl_st for the Stealth general, Stealth's gxrb_ggl_tx
#              for the Toxin general - the sensor names a general only when the LOBBY showed it - and gxrb_ggl_done either way;
#   Toxin:     its wave-1 Rush (3 Scorpions + 1 Quad) gives way to "GX GGL <team>": 3 to rush_scorpions Scorpions, open until rush_t, one priority
#              above every wave-1 team (AIPlayer::selectTeamToBuild takes the highest, at random among equals: rb-data-16). It hunts at rush_t, or
#              when it forms if that is later (rb.launch_scripts). It forms when all are built, or 100 s after it starts (teamInitialIdleFrames) with
#              what it holds, once the builds in flight finish; under its minimum of 3 by then it disbands. Stock has two Arms Dealers by 84-127 s and
#              3-6 Scorpions by 240 s (17 OFF games, smoke13-15). The STOCK Rush stays on vs any other GLA enemy, or a Random one (rb.matchup_keep);
#   Stealth:   ADDS one team of Rocket Buggies, half of buggy_count at least, and one of 1 to vs_toxin_mobs Angry Mobs, behind an own Palace (the
#              Buggy: Arms Dealer + Palace, the Mob: Barracks + Palace, INIZH). A priority above every wave team, one instance each, re-formed
#              when lost; each hunts when it forms. Nothing stock changes: stock Hard banks $7k-$29k from about 420 s, so these spend that bank
#              before they take from the waves.
# Not built (UNUSED, defaults only): bus_count, switch_to_buggy_t - row 307, the Stealth mirror (UNTESTED_MIRROR); rpg_per_tunnel - row 271's tunnel
# break needs a target choice, a NEW priority set no player's state may switch (names are global).
# PARKED beside this module (smoke17): the rush leaves home but adds only 3 Scorpions by 240 s, Stealth's Buggies die near home, and neither half showed an effect on kills or buildings.
RULE_ID = 'COUNTER.GLA_VS_GLA'
MATCHUP = 'GLA'
PARAMS = ('rush_scorpions', 'rush_t', 'buggy_count', 'vs_toxin_mobs')
T, ST = 'SkirmishGLAToxinGeneral', 'SkirmishGLAStealthGeneral'
SCOPE = (T, ST)
UNUSED = {'bus_count': 'row 307, the Stealth mirror, stays off (UNTESTED_MIRROR)', 'switch_to_buggy_t': 'row 307, the Stealth mirror, stays off (UNTESTED_MIRROR)',
          'rpg_per_tunnel': 'not built: row 271 needs a target choice, a NEW priority set no player\'s state may switch'}
TAG, GROUP = 'GX GGL', 'GX COUNTER.GLA_VS_GLA'
ST_F, TX_F, DONE = 'gxrb_ggl_st', 'gxrb_ggl_tx', 'gxrb_ggl_done'
RUSH, WAVE1, WAVE5 = 'GLA H 1ST Wave Rush. 3', '_GLA_1ST_WAVE_H', '_GLA_5TH_WAVE_H'
WAVES = ('_GLA_1ST_WAVE_H', '_GLA_2ND_WAVE_H', '_GLA_3RD_WAVE_H', '_GLA_4TH_WAVE_H', '_GLA_5TH_WAVE_H')
RUSH_GATE, RUSH_LAUNCH, NOW = 'GLA GX GGL Rush', 'GLA GX GGL Launch Rush', 'GLA GX GGL Launch Now'
FORMED = rb.FORMED
BUGGIES, MOBS, DONOR = 'GX GGL Buggies', 'GX GGL Mobs', 'GLA H 5TH Wave Tanks B. 2'
SCORPION, QUAD, NONE = 'Chem_GLATankScorpion', 'Chem_GLAVehicleQuadCannon', '<none>'
BUGGY, MOB, PALACE, BARRACKS = 'Slth_GLAVehicleRocketBuggy', 'Slth_GLAInfantryAngryMobNexus', 'Slth_GLAPalace', 'Slth_GLABarracks'
RUSH_MIN = 3                                                      # stock's own Rush holds 3 Scorpions at least
_TOXIN = rb.matchup_claims((T,), [RUSH], scripts=[[T, TAG + ' Gen GLAStealthGeneral'], [T, TAG + ' Gen Done'], [T, RUSH_GATE], [T, RUSH_LAUNCH],
                                                     [T, RUSH_LAUNCH + FORMED]],
                           groups=[[T, GROUP]], teams=['GX GGL ' + RUSH])
_STEALTH = rb.matchup_claims((ST,), [], scripts=[[ST, TAG + ' Gen GLAToxinGeneral'], [ST, TAG + ' Gen Done'], [ST, 'GLA GX GGL Buggies'],
                                                 [ST, 'GLA GX GGL Mobs'], [ST, NOW + FORMED]], groups=[[ST, GROUP]], teams=[BUGGIES, MOBS])
CLAIMS = {'scripts': _TOXIN['scripts'] + _STEALTH['scripts'], 'groups': _TOXIN['groups'] + _STEALTH['groups'],
          'teams': sorted(set(_TOXIN['teams']) | set(_STEALTH['teams']))}


def _gate(scb, o, stock_gate, name, leaves):
    """a production condition: the stock gate's header and actions (rb.wave_gate), its one OR block replaced by `leaves` - no stock leaf"""
    s = rb.wave_gate(scb, o, stock_gate, name, lambda: [])
    blk = [b for b in s.kids if b.name == 'OrCondition'][0]
    blk.kids = list(leaves)
    return s


def _tick(t):
    return -(-t // 10)


def _toxin(scb, ts, p, f, key):
    t = rb.team(ts, RUSH)
    assert t is not None and rb.team_get(t, 'teamOwner') == T and rb.mu_stock_gate(scb, T, rb.team_get(t, 'teamProductionCondition'), (WAVE1,)) == WAVE1, RUSH
    tick = _tick(p['rush_t'])
    rb.matchup_off(scb, ts, T, RUSH, MATCHUP)
    rb.matchup_keep(scb, ts, T, RUSH, MATCHUP, [lambda: [key(), f(DONE, True), f(ST_F, False)]])
    # the wave-1 teams, read before the variant joins them
    w1 = [rb.team_get(d, 'teamProductionPriority') for d in ts if rb.team_get(d, 'teamOwner') == T
          and rb.mu_stock_gate(scb, T, rb.team_get(d, 'teamProductionCondition'), (WAVE1,)) == WAVE1]
    v = rb.variant_team(scb, ts, RUSH, 'GX GGL ' + RUSH, RUSH_GATE, [[SCORPION, RUSH_MIN, p['rush_scorpions']], [NONE, 0, 0], [NONE, 0, 0]],
                        hooks=rb.launch_fields(RUSH_LAUNCH, tick))
    rb.team_set(scb, v, 'teamProductionPriority', max(w1) + 1)
    return [_gate(scb, T, WAVE1, RUSH_GATE, [rb.cond(scb, 'COUNTER', ('COUNTER', rb.GX_CLOCK), ('COMPARISON', rb.LT), ('INT', tick)), key(), f(ST_F, True)])] + \
        rb.launch_scripts(scb, RUSH_LAUNCH, tick)


def _new_team(scb, ts, donor, name, gate, unit, mn, mx, priority):
    """a new team on `donor`'s fields (owner, idle frames), no hook of the donor's: it stands in for no stock team (rb.add_new_team)"""
    d = {'_e': [list(e) for e in donor['_e']], 'teamName': None}
    for e in d['_e']:
        if e[0] != 'teamProductionCondition' and e[0].startswith('team') and 'Script' in e[0] and e[2]:
            rb.team_set(scb, d, e[0], '')
    for k, v in (('teamName', name), ('teamUnitType1', unit), ('teamUnitMinCount1', mn), ('teamUnitMaxCount1', mx), ('teamUnitType2', NONE),
                 ('teamUnitMinCount2', 0), ('teamUnitMaxCount2', 0), ('teamUnitType3', NONE), ('teamUnitMinCount3', 0), ('teamUnitMaxCount3', 0),
                 ('teamMaxInstances', 1), ('teamProductionPriority', priority), ('teamProductionCondition', gate), ('teamOnCreateScript', NOW + FORMED)):
        rb.team_set(scb, d, k, v)
    d['teamName'] = name
    rb.add_new_team(ts, ts.index(donor) + 1, d)


def _stealth(scb, ts, p, f, key):
    donor = rb.team(ts, DONOR)
    assert donor is not None and rb.team_get(donor, 'teamOwner') == ST, DONOR
    # above every wave team, below stock's reactive teams (counter teams 170-215, Air Defenders 300+, Base Expanders 500, Alert Invasion 1000)
    top = 1 + max(rb.team_get(d, 'teamProductionPriority') for d in ts if rb.team_get(d, 'teamOwner') == ST and rb.team_get(d, 'teamProductionCondition') in WAVES)
    assert top < min(rb.team_get(d, 'teamProductionPriority') for d in ts if rb.team_get(d, 'teamName').startswith('GLA CT - ') and rb.team_get(d, 'teamOwner') == ST), top
    has = lambda b: rb.own_count(scb, rb.GE, 1, b)
    kids = [_gate(scb, ST, WAVE5, 'GLA GX GGL Buggies', [key(), f(TX_F, True), has(PALACE)])] + rb.launch_scripts(scb, NOW, 0)
    _new_team(scb, ts, donor, BUGGIES, 'GLA GX GGL Buggies', BUGGY, -(-p['buggy_count'] // 2), p['buggy_count'], top)
    if p['vs_toxin_mobs']:
        kids.insert(1, _gate(scb, ST, WAVE5, 'GLA GX GGL Mobs', [key(), f(TX_F, True), has(PALACE), has(BARRACKS)]))
        _new_team(scb, ts, donor, MOBS, 'GLA GX GGL Mobs', MOB, 1, p['vs_toxin_mobs'], top)
    return kids


def apply(scb, scope, p):
    ts = rb.teams(scb)
    f = lambda n, v: rb.flag_is(scb, n, v)
    key = lambda: f(rb.mu_flag(MATCHUP), True)
    for o in scope:
        rb.matchup(scb, o)
        latch = rb.general_latch(scb, TAG, MATCHUP, {'GLAStealthGeneral': ST_F} if o == T else {'GLAToxinGeneral': TX_F}, DONE)
        rb.owner_list(scb, o).kids.append(rb.new_group(GROUP, latch + (_toxin if o == T else _stealth)(scb, ts, p, f, key)))
    rb.put_teams(scb, ts)


RUSH_READ_S, BUGGY_READ_S, BUGGY_MIN, RUSH_FIRED = 240, 600, 3, 8
# a rush reads 240 s at every rush_t (built_by counts a team still forming; a read that moves with rush_t 300 reaches 360 s, where stock's
# waves 2-3 hold 8 Scorpions too). Stock Toxin holds 3-6 by 240 s (17 OFF games,
# smoke13-15); stock Stealth's Rocket Buggies come from its wave 5 teams (two artillery teams, the tunnel guards)
CONTRAST = {'what': 'own Toxin Scorpions built by 240 s, or own Stealth Rocket Buggies built by 600 s (by the general the player plays)', 'direction': 1,
            'min_delta': 3, 'holds': 'rush_scorpions >= 8; buggy_count >= 4 (its box)'}


def _is(name):
    return lambda n: n == name


def _scorpions(obs, me):
    return ob.built_by(obs, me, _is(SCORPION), RUSH_READ_S * ob.hz(obs))


def _buggies(obs, me):
    return ob.built_by(obs, me, _is(BUGGY), BUGGY_READ_S * ob.hz(obs))


def _plays_stealth(obs, me):
    """this player plays the Stealth general: template names are per general, and a game that ended early must not read as 0 of the other's unit"""
    return ob.first_frame(obs, me, lambda n: n.startswith('Slth_')) is not None


def measure(obs, me, params=None):
    if not obs:
        return None
    return _buggies(obs, me) if _plays_stealth(obs, me) else _scorpions(obs, me)


def activation(obs, me, params=None):
    """Toxin - triggered: an own Arms Dealer by 240 s and the game ran past the read; fired: 8 own Scorpions by 240 s (stock: 6 at most).
    Stealth - triggered: an own Palace complete by 540 s and the game ran past 600 s; fired: 3 own Rocket Buggies by 600 s. Stock's wave 5 can
    reach 3 too once it starts: the CONTRAST (ON - OFF, paired, against an inert arm) reads it. The templates are per general, so a game has one"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz, end = ob.hz(obs), ob.last_frame(obs)
    if _plays_stealth(obs, me):
        pal = ob.first_frame(obs, me, _is(PALACE), 1, complete=True)
        trig = pal is not None and pal <= 540 * hz and end is not None and end > BUGGY_READ_S * hz
        n = _buggies(obs, me)
        return {'triggered': bool(trig), 'fired': bool(trig and (n or 0) >= BUGGY_MIN), 'detail': 'palace_f=%s buggies_600s=%s' % (pal, n)}
    dealer = ob.first_frame(obs, me, _is('Chem_GLAArmsDealer'), 1, complete=True)
    trig = dealer is not None and dealer <= RUSH_READ_S * hz and end is not None and end > RUSH_READ_S * hz
    n = _scorpions(obs, me)
    return {'triggered': bool(trig), 'fired': bool(trig and (n or 0) >= RUSH_FIRED), 'detail': 'dealer_f=%s scorpions_240s=%s' % (dealer, n)}
