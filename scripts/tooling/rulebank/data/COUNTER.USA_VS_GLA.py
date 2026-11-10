# GeneralsX: rule bank DATA rule COUNTER.USA_VS_GLA: USA Air Force / Superweapon meet GLA with an
# early King Raptor opener and an earlier Colonel Burton (row 50: "a few early King Raptors; they shut down Technicals and force him into
# Quad Cannons"; row 121: "As USA Superweapon always build Colonel Burton: at $1,200 he pays for himself with one Demo buggy kill";
# Dominator: "make a few King Raptors, stop the initial technical"). This, per general:
#   MATCHUP 'GLA': the SHARED latch (rb.matchup) sets gxrb_mu_gla once AI_PLAYER_GX_SENSE says my enemy is GLA (lobby at once; a Random
#              enemy after 3 different units of one army were fairly seen); it decides ONCE;
#   Air Force: its wave-1 "Supply Hunters" team (2 King Raptors) gives way to "GX UVG <team>": `raptors` King Raptors on a NEW gate - its
#              OWN clock from wave1_t (_GX_CLOCK >= ceil(wave1_t / 10)) until stock wave 3 would start (_GX_CLOCK < 25), not the stock
#              wave-1 window (wave1_t may lie past it: its box reaches 180 s, wave 1 ends at 150 s); it hunts at once; its priority is one
#              above every wave-1 team (stock picks at random between Rush and Supply Hunters, both 100: rb-data-16);
#   both: the wave-4 Burton team ("4TH Wave Burton", 1 Burton at 400-600 s) gives way to "GX UVG <team>": burton_count Burtons from stock
#              wave 2's start (_GX_CLOCK >= 15) to wave 4's end (< 60); Burton needs a Strategy Center, which AIPlayer::
#              isPossibleToBuildTeam waits for. It runs stock's own Burton orders ("USA Commando Orders": the back-door path, then a
#              hunt with his demo charge) at once - not the stock hook "USA Commando Raid Sequence", which waits on _LAUNCH_ATTACK (enemy
#              state). burton_count 0 leaves the Burton team stock; 1 and 2 field ONE Burton: he is one per player (MaxSimultaneousOfType 1),
#              so a team needing 2 could never complete and would disband - the team re-forms when he dies.
# No stock script changes; each switched stock team waits on its off gate "GX MU Off <team>".
# Not built (UNUSED, defaults only): engage_after_cm, min_vet (row 72: "engage only after Countermeasures and some veterancy", with
# Comanches sent at the Arms Dealer, tunnel and supply) - a target choice needs a NEW attack-priority set per general against a pool whose
# flags already hold under 14 players, and no condition reads own veterancy. The Humvee follow-up (row 50) has no box: stock waves 4-5
# field the Humvees.
RULE_ID = 'COUNTER.USA_VS_GLA'
MATCHUP = 'GLA'
PARAMS = ('raptors', 'wave1_t', 'burton_count')
AF, SW = 'SkirmishAmericaAirForceGeneral', 'SkirmishAmericaSuperWeaponGeneral'
SCOPE = (AF, SW)
PRE = {AF: 'AirF_', SW: 'SupW_'}
UNUSED = {'engage_after_cm': 'not built: a target choice (Arms Dealer, tunnel, supply) needs a NEW attack-priority set per general, '
                             'and the flag pool already holds under 14 players with the matchup latch',
          'min_vet': 'not built: no condition reads own veterancy'}
TAG, GROUP = 'UVG', 'GX COUNTER.USA_VS_GLA'
RAPTORS = {AF: 'USA H 1ST Wave Supply Hunters. 1'}
BURTON = {AF: 'USA H 4TH Wave Burton. 1', SW: 'USA H 4TH Wave Burton. 3'}
WAVE1, WAVE4 = '_USA_1ST_WAVE_H', '_USA_4TH_WAVE_H'
ORDERS = 'USA Commando Orders'
UNTIL_RAPTOR, BURTON_FROM, BURTON_UNTIL = 25, 15, 60      # _GX_CLOCK ticks: stock wave 3's start, wave 2's start, wave 4's end
WAVE2_PRIORITY = 105                                      # the lowest priority of the Air Force's stock wave-2 attack teams (Fast Attack)
_STOCK = sorted(set(RAPTORS.values()) | set(BURTON.values()))
CLAIMS = rb.matchup_claims(SCOPE, _STOCK,
                           scripts=[[AF, 'USA GX UVG Raptors'], [AF, 'USA GX UVG Launch Now' + rb.FORMED]] +
                           [[o, s] for o in SCOPE for s in ('USA GX UVG Burton', 'USA GX UVG Burton Orders', ORDERS)],
                           groups=[[o, GROUP] for o in SCOPE], teams=['GX %s %s' % (TAG, n) for n in _STOCK])


def _gate(scb, o, stock_gate, name, lo, hi, extra):
    """a production condition for a variant: the stock wave gate's header and actions (rb.wave_gate) with its one OR block replaced by
    _GX_CLOCK >= lo, < hi and extra - a window of the rule's own, no stock leaf"""
    s = rb.wave_gate(scb, o, stock_gate, name, lambda: [])
    blk = [b for b in s.kids if b.name == 'OrCondition'][0]
    blk.kids = [rb.clock_at_least(scb, lo), rb.cond(scb, 'COUNTER', ('COUNTER', rb.GX_CLOCK), ('COMPARISON', rb.LT), ('INT', hi))] + list(extra)
    return s


def _team_gate(scb, ts, o, n, want):
    t = rb.team(ts, n)
    assert t is not None and rb.team_get(t, 'teamOwner') == o, (o, n)
    assert rb.mu_stock_gate(scb, o, rb.team_get(t, 'teamProductionCondition'), (want,)) == want, (o, n)
    return t


def apply(scb, scope, p):
    ts = rb.teams(scb)
    key = lambda: rb.flag_is(scb, rb.mu_flag(MATCHUP), True)
    for o in scope:
        if o not in RAPTORS and not p['burton_count']:
            continue                                 # nothing to switch: no latch either (it costs four flags per player)
        rb.matchup(scb, o)
        kids = []
        if o in RAPTORS:
            n = RAPTORS[o]
            _team_gate(scb, ts, o, n, WAVE1)
            t0 = -(-p['wave1_t'] // 10)
            assert t0 < UNTIL_RAPTOR, (o, t0)
            kids += [_gate(scb, o, WAVE1, 'USA GX UVG Raptors', t0, UNTIL_RAPTOR, [key()])] + rb.launch_scripts(scb, 'USA GX UVG Launch Now', 0)
            rb.matchup_off(scb, ts, o, n, MATCHUP)
            v = rb.variant_team(scb, ts, n, 'GX %s %s' % (TAG, n), 'USA GX UVG Raptors', [[PRE[o] + 'AmericaJetRaptor', p['raptors'], p['raptors']],
                                ['<none>', 0, 0], ['<none>', 0, 0]], hooks=rb.launch_fields('USA GX UVG Launch Now', 0))
            # stock wave 1 picks AT RANDOM among its top-priority teams (AIPlayer::selectTeamToBuild): Rush and Supply Hunters are both 100,
            # so a variant at 100 opened only when it won the draw. One above every wave-1 team: it opens first, Rush follows; wave 2's
            # 105 / 110 teams still come before it (rb-data-16)
            w1 = [rb.team_get(t, 'teamProductionPriority') for t in ts if t is not v and rb.team_get(t, 'teamOwner') == o
                  and rb.mu_stock_gate(scb, o, rb.team_get(t, 'teamProductionCondition'), (WAVE1,)) == WAVE1]
            assert w1 and max(w1) + 1 < WAVE2_PRIORITY, (o, w1)
            rb.team_set(scb, v, 'teamProductionPriority', max(w1) + 1)
        if p['burton_count']:
            n = BURTON[o]
            _team_gate(scb, ts, o, n, WAVE4)
            go = rb.new_script(scb, 'USA GX UVG Burton Orders', [[rb.cond(scb, 'CONDITION_TRUE')]],
                               [rb.act(scb, 'TEAM_EXECUTE_SEQUENTIAL_SCRIPT', ('TEAM', '<This Team>'), ('SCRIPT', ORDERS))], sub=True)
            kids += [_gate(scb, o, WAVE4, 'USA GX UVG Burton', BURTON_FROM, BURTON_UNTIL, [key()]), go]
            rb.matchup_off(scb, ts, o, n, MATCHUP)
            rb.variant_team(scb, ts, n, 'GX %s %s' % (TAG, n), 'USA GX UVG Burton',
                            [[PRE[o] + 'AmericaInfantryColonelBurton', 1, 1], ['<none>', 0, 0], ['<none>', 0, 0]],
                            hooks={'teamOnCreateScript': 'USA GX UVG Burton Orders'})
        if kids:
            rb.owner_list(scb, o).kids.append(rb.new_group(GROUP, kids))
    rb.put_teams(scb, ts)


TEAM_UNIT_WAIVERS = {'SupW_AmericaInfantryColonelBurton': 'INIZH: Barracks + Strategy Center - AIPlayer::isPossibleToBuildTeam skips the '
                     'team until they stand; it is a single-type team, so nothing else waits on it',
                     'AirF_AmericaInfantryColonelBurton': 'INIZH: Barracks + Strategy Center - as the Superweapon Burton'}
RAPTOR_S, BURTON_S = 200, 350
# rb-data-16: the reads moved BEFORE smoke14's games, from smoke13's POST-HOC result. The Air Force's
# Airfield stands at ~99 s and a King Raptor takes ~20 s: the first came at 121-162 s, so 150 s showed nothing and by 400 s stock wave 2's
# Raptors buried the opener (7-8 vs 5-8); by 200 s ON had 2/3/3 vs OFF 1/1/0. The Superweapon Burton came at 273-310 s ON vs 376-412 s
# OFF: by 400 s both arms have him, by 350 s only ON does.


def _raptor_s(params):
    """the Raptor read: 100 s past the opener's first clock tick, at least 200 s (wave1_t reaches 180 s). Read both arms
    of a CONTRAST with the ON arm's params"""
    t = (params or {}).get('wave1_t', 100)
    return max(RAPTOR_S, 10 * -(-t // 10) + 100)


def _raptors(obs, me, params=None):
    return ob.built_by(obs, me, ob.ends_with('AmericaJetRaptor'), _raptor_s(params) * ob.hz(obs))


def _burtons(obs, me):
    return ob.built_by(obs, me, ob.ends_with('AmericaInfantryColonelBurton'), BURTON_S * ob.hz(obs))


CONTRAST = {'what': 'own Raptors built by wave1_t + 100 s (200 s at least) plus own Colonel Burtons by 350 s (stock: AF\'s 2 wave-1 Supply Hunter King '
                    'Raptors, SW none; no Burton before wave 4 at 400 s)', 'direction': 1, 'min_delta': 1,
            # an opener of 1-2 Raptors is FEWER than the stock Supply Hunters' 2 - it reads at or below OFF
            'holds': 'raptors >= 3, or burton_count >= 1 with raptors >= 2'}


def measure(obs, me, params=None):
    return (_raptors(obs, me, params) or 0) + (_burtons(obs, me) or 0) if obs else None


def activation(obs, me, params=None):
    """triggered: an own Strategy Center complete by 310 s and the game ran past 350 s; fired: an own Burton by 350 s (stock fields
    none before 400 s). The Air Force opener reads in measure()"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    sc = ob.first_frame(obs, me, ob.ends_with('AmericaStrategyCenter'), 1, complete=True)
    end = ob.last_frame(obs)
    triggered = sc is not None and sc <= (BURTON_S - 40) * hz and end is not None and end > BURTON_S * hz
    b = _burtons(obs, me)
    return {'triggered': bool(triggered), 'fired': bool(triggered and (b or 0) >= 1),
            'detail': 'strategy_f=%s raptors_%ds=%s burtons_%ds=%s' % (sc, _raptor_s(params), _raptors(obs, me, params), BURTON_S, b)}
