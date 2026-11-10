# GeneralsX: rule bank DATA rule OPEN.CHINA_2ND_SUPPLY: the pros' China opening, a second (and
# optionally third) Supply Center early - PP > Barracks > Supply > Supply > War Factory. Stock Hard China gets its second centre only
# from the escalation-gated, gift-carrying Base Expansion Check (~250 s), which stays frozen; this adds Hard-only one-shot scripts:
#   2nd centre: once the first stands and _GX_CLOCK >= supply2_early_esc (0 = no clock gate);
#   3rd centre (supply_centers = 3): once two stand and the War Factory is up;
#   warfactory_n = 1: the stock "China War Factory 2 - H" waits for the 2nd centre, or for the clock + 120 s (it never hangs on a map
#   with no second pile); Infantry has no 2nd War Factory script. warfactory_n = 2 keeps it stock.
# Own state only; no faction gate (NO_ENEMY_FACTION: row 241's vs-USA Air Field is left out).
RULE_ID = 'OPEN.CHINA_2ND_SUPPLY'
PARAMS = ('supply_centers', 'supply2_early_esc', 'warfactory_n')
UNUSED = {'trucks_first': 'no scripted truck order exists: the centre spawns its trucks (AIData)',
          'airfield_early': 'vanilla/Nuke/Infantry already build the Air Field right after the first centre; Tank has no MiG team'}
GEN = {'SkirmishChina': '', 'SkirmishChinaTankGeneral': 'Tank_', 'SkirmishChinaNukeGeneral': 'Nuke_', 'SkirmishChinaInfantryGeneral': 'Infa_'}
WF2 = 'China War Factory 2 - H'
GROUP = 'GX OPEN.CHINA_2ND_SUPPLY'
S2, S3 = 'China GX 2nd Supply - H', 'China GX 3rd Supply - H'
GRACE = 12                                   # _GX_CLOCK ticks (120 s) after the 2nd centre was due: then the 2nd War Factory goes anyway
CLAIMS = {'scripts': [[o, n] for o in GEN for n in (S2, S3)] + [[o, WF2] for o in GEN if o != 'SkirmishChinaInfantryGeneral'],
          'groups': [[o, GROUP] for o in GEN]}
DEFAULTS = {'supply_centers': 2, 'supply2_early_esc': 15, 'warfactory_n': 1}


def apply(scb, scope, params):
    me = ('SIDE', rb.SIDE_ME)
    for o in scope:
        sc, wf = GEN[o] + 'ChinaSupplyCenter', GEN[o] + 'ChinaWarFactory'
        gate = [rb.clock_at_least(scb, params['supply2_early_esc'])] if params['supply2_early_esc'] > 0 else []
        build = lambda: rb.act(scb, 'AI_PLAYER_BUILD_SUPPLY_CENTER', me, ('OBJECT_TYPE', sc), ('INT', 10000))
        kids = [rb.new_script(scb, S2, [gate + [rb.cond(scb, 'BUILT_BY_PLAYER', ('OBJECT_TYPE', sc), me)]], [build()], oneshot=True)]
        if params['supply_centers'] >= 3:
            kids.append(rb.new_script(scb, S3, [[rb.own_count(scb, rb.GE, 2, sc), rb.cond(scb, 'BUILT_BY_PLAYER', ('OBJECT_TYPE', wf), me)]],
                                      [build()], oneshot=True))
        rb.owner_list(scb, o).kids.append(rb.new_group(GROUP, kids))
        if params['warfactory_n'] == 1 and o != 'SkirmishChinaInfantryGeneral':
            _g, _k, s = rb.find_script(scb, o, WF2)
            ors = [k for k in s.kids if k.name == 'OrCondition']
            assert len(ors) == 1 and len(ors[0].kids) == 1, '%s %s: expected one stock gate, found %r' % (o, WF2, ors)
            g = ors[0].kids[0]
            due = rb.clock_at_least(scb, params['supply2_early_esc'] + GRACE)
            new = [mss.Node('OrCondition', ors[0].ver, kids=[mss.deep_copy(g), rb.own_count(scb, rb.GE, 2, sc)]),
                   mss.Node('OrCondition', ors[0].ver, kids=[mss.deep_copy(g), due])]
            i = s.kids.index(ors[0])
            s.kids[i:i + 1] = new


def activation(obs, me, params=None):
    """triggered: the first own China supply centre is complete and the clock passed supply2_early_esc; fired: a second own centre
    (any build state) within 60 s of that moment (stock's comes ~250 s in)"""
    p = dict(DEFAULTS, **(params or {}))
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz, is_sc = ob.hz(obs), ob.ends_with('ChinaSupplyCenter')
    f1 = ob.first_frame(obs, me, is_sc, 1, complete=True)
    at = None if f1 is None else max(f1, p['supply2_early_esc'] * 10 * hz)
    end = ob.last_frame(obs)
    triggered = at is not None and end is not None and end >= at + 60 * hz
    f2 = ob.first_frame(obs, me, is_sc, 2)
    f3 = ob.first_frame(obs, me, is_sc, 3)
    fired = triggered and f2 is not None and f2 <= at + 60 * hz
    return {'triggered': bool(triggered), 'fired': bool(fired), 'detail': 'first_complete_f=%s due_f=%s second_f=%s third_f=%s' % (f1, at, f2, f3)}
