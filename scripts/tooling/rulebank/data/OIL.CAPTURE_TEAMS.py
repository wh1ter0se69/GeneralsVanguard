# GeneralsX: rule bank DATA rule OIL.CAPTURE_TEAMS: take the oil early and in numbers. Stock Hard
# captures tech buildings only when one lies within 500 of its inner perimeter (the cached "<S> Tech Building Detect" test), with ONE
# team of 2 capturers (GLA: 1). This, per general:
#   capture_radius   -> the Detect test's distance (a tech building near MY area, cached at its first evaluation: map layout);
#   capture_start_t  -> "<S> Tech Capture Build" (the teams' production condition) also waits for _GX_CLOCK >= ceil(t/10);
#   oil_count_target -> ... and produces capturers only while I own fewer derricks than that, unless
#   cash_starved_cash-> my money is at most this (a starved AI keeps capturing);
#   ranger_count, capture_teams -> the capture team's size (min = max) and how many may exist at once;
#   refinery_capture_t -> the capture hunt uses a NEW per-general priority set (a copy of the stock "<S> Tech Building Priority"
#                       definition, same trigger), and for REFINERY_WINDOW_S from _GX_CLOCK >= ceil(t/10) the oil refinery outranks
#                       every derrick in it, then drops back to the stock value (the 30/10 smoke: kept up, it sent every team at the
#                       ENEMY's refinery across the map, past the derricks). Clock only: every player of a general shares the table, so
#                       no player's own state may move it.
# Stock sets are never defined further (set names are global); the stock gate leaves copied into the new OR blocks are the stock gate
# itself (the compiler accepts a tainted stock read only that way: same script, same actions). Detect / Build / Orders are 011 scripts:
# exact under the tier gate, also Normal's under --tier-gate off (normal_also in the manifest).
RULE_ID = 'OIL.CAPTURE_TEAMS'
PARAMS = ('capture_radius', 'capture_start_t', 'ranger_count', 'oil_count_target', 'capture_teams', 'refinery_capture_t', 'cash_starved_cash')
SIDES = {'SkirmishAmerica': 'USA', 'SkirmishAmericaAirForceGeneral': 'USA', 'SkirmishAmericaLaserGeneral': 'USA',
         'SkirmishAmericaSuperWeaponGeneral': 'USA', 'SkirmishChina': 'China', 'SkirmishChinaTankGeneral': 'China',
         'SkirmishChinaNukeGeneral': 'China', 'SkirmishChinaInfantryGeneral': 'China', 'SkirmishGLA': 'GLA',
         'SkirmishGLADemolitionGeneral': 'GLA', 'SkirmishGLAToxinGeneral': 'GLA', 'SkirmishGLAStealthGeneral': 'GLA'}
TEAM = {'USA': 'USA Tech Capture Team', 'China': 'China Tech Capture Team', 'GLA': 'GLA Tech Building Capture'}
DETECT, BUILD, ORDERS, STOCK_DEFINE = '%s Tech Building Detect', '%s Tech Capture Build', '%s Tech Building Capture Orders', '%s Capture Tech Building'
DEFINE, REFINERY, END, GROUP = '%s GX Oil Priority Define', '%s GX Oil Priority Refinery', '%s GX Oil Priority Refinery End', \
    'GX OIL.CAPTURE_TEAMS'
REFINERY_WINDOW_S = 90          # the 30/10 smoke: our refinery was taken 53 s after the raise
STOCK_SET, NEW_SET = '%s Tech Building Priority', 'GX Oil Priority %s'
DERRICK, REFINERY_T, REFINERY_PRIO = 'TechOilDerrick', 'TechOilRefinery', 5500   # 5500 - d/100 beats any derrick's 5000 within 50,000 wu
def _teams(scb):
    """{owner: its capture team name}: the team of that owner whose production condition is the side's Capture Build"""
    out = {}
    for d in rb.teams_ro(scb):
        o = rb.team_get(d, 'teamOwner')
        if o in SIDES and d['teamName'].startswith(TEAM[SIDES[o]]) and rb.team_get(d, 'teamProductionCondition') == BUILD % SIDES[o]:
            assert o not in out, '%s: two capture teams %r %r' % (o, out[o], d['teamName'])
            out[o] = d['teamName']
    return out


CLAIMS = {'scripts': [[o, n % s] for o, s in SIDES.items() for n in (DETECT, BUILD, ORDERS, DEFINE, REFINERY, END)],
          'groups': [[o, GROUP] for o in SIDES],
          'teams': sorted('%s%s' % (TEAM[s], x) for s in ('USA', 'China', 'GLA') for x in ('', '. 1', '. 2', '. 3')),
          'priority_sets': sorted(NEW_SET % o for o in SIDES)}
DEFAULTS = {'capture_start_t': 20}


def ticks(seconds):
    return -(-seconds // 10)


def apply(scb, scope, params):
    me = ('SIDE', rb.SIDE_ME)
    names = _teams(scb)
    ts = rb.teams(scb)
    for o in scope:
        s = SIDES[o]
        # radius: the Detect test's REAL distance
        _g, _k, det = rb.find_script(scb, o, DETECT % s)
        hit = 0
        for l in rb.leaves(det):
            n, ps = rb.decode(scb, l)
            if n == 'SKIRMISH_TECH_BUILDING_WITHIN_DISTANCE':
                typ, key, raw = mss.parse_call(scb, l.raw)
                raw = [(p[0], p[1], float(params['capture_radius']), p[3]) if mss.PTYPES[p[0]] == 'REAL' else p for p in raw]
                l.raw = mss.enc_call(typ, key, raw); hit += 1
        assert hit == 1, '%s %s: %d distance tests' % (o, DETECT % s, hit)
        # production gate: stock gate G AND clock AND (fewer derricks than the target OR starved)
        _g, _k, b = rb.find_script(scb, o, BUILD % s)
        ors = [k for k in b.kids if k.name == 'OrCondition']
        assert len(ors) == 1, '%s %s: %d OR blocks' % (o, BUILD % s, len(ors))
        g = ors[0].kids
        clock = lambda: rb.clock_at_least(scb, ticks(params['capture_start_t']))
        new = [mss.Node('OrCondition', ors[0].ver, kids=[mss.deep_copy(x) for x in g] + [clock(), rb.own_count(scb, rb.LT, params['oil_count_target'], DERRICK)]),
               mss.Node('OrCondition', ors[0].ver, kids=[mss.deep_copy(x) for x in g] + [clock(), rb.credits_at_most(scb, params['cash_starved_cash'])])]
        i = b.kids.index(ors[0]); b.kids[i:i + 1] = new
        # the hunt applies this general's own copy of the priority set
        _g, _k, od = rb.find_script(scb, o, ORDERS % s)
        hit = 0
        for l in rb.leaves(od):
            n, ps = rb.decode(scb, l)
            if n == 'TEAM_APPLY_ATTACK_PRIORITY_SET' and (('ATTACK_PRIORITY_SET', STOCK_SET % s) in ps):
                l.raw = rb.act(scb, n, *[(t, NEW_SET % o) if t == 'ATTACK_PRIORITY_SET' else (t, v) for t, v in ps]).raw; hit += 1
        assert hit == 1, '%s %s: %d set applications' % (o, ORDERS % s, hit)
        # the new set: the stock definition copied under the new name (same trigger), then the timed refinery raise
        _g, _k, sd = rb.find_script(scb, o, STOCK_DEFINE % s)
        conds = [mss.deep_copy(c) for c in sd.kids if c.name == 'OrCondition']
        acts, stock_v = [], None
        for l in sd.kids:
            if l.name == 'ScriptAction':
                n, ps = rb.decode(scb, l)
                assert n.startswith('SET_ATTACK_PRIORITY') and ps[0] == ('ATTACK_PRIORITY_SET', STOCK_SET % s), (o, n, ps)
                acts.append(rb.act(scb, n, ('ATTACK_PRIORITY_SET', NEW_SET % o), *ps[1:]))
                stock_v = ps[2][1] if n == 'SET_ATTACK_PRIORITY_THING' and stock_v is None else stock_v
        assert stock_v is not None, '%s: no stock tech-building priority' % o
        define = rb.new_script(scb, DEFINE % s, [], acts, oneshot=True)
        define.kids = conds + acts
        t0 = ticks(params['refinery_capture_t'])
        refinery = rb.new_script(scb, REFINERY % s, [[rb.clock_at_least(scb, t0)]],
                                 [rb.act(scb, 'SET_ATTACK_PRIORITY_THING', ('ATTACK_PRIORITY_SET', NEW_SET % o), ('OBJECT_TYPE', REFINERY_T),
                                         ('INT', REFINERY_PRIO))], oneshot=True)
        end = rb.new_script(scb, END % s, [[rb.clock_at_least(scb, t0 + ticks(REFINERY_WINDOW_S))]],
                            [rb.act(scb, 'SET_ATTACK_PRIORITY_THING', ('ATTACK_PRIORITY_SET', NEW_SET % o), ('OBJECT_TYPE', REFINERY_T),
                                    ('INT', stock_v))], oneshot=True)
        rb.owner_list(scb, o).kids.append(rb.new_group(GROUP, [define, refinery, end]))
        # the capture team: size and how many at once
        t = rb.team(ts, names[o])
        rb.team_set(scb, t, 'teamUnitMinCount1', params['ranger_count'])
        rb.team_set(scb, t, 'teamUnitMaxCount1', params['ranger_count'])
        rb.team_set(scb, t, 'teamMaxInstances', params['capture_teams'])
    rb.put_teams(scb, ts)


def activation(obs, me, params=None):
    """triggered: own Barracks complete and the game ran to capture_start_t + 180 s; fired: >= 2 own oil derricks by then (stock takes
    only derricks within 500 of its base, with one 2-man team)"""
    p = dict(DEFAULTS, **(params or {}))
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    fb = ob.first_frame(obs, me, ob.ends_with('Barracks'), 1, complete=True)
    due = (ticks(p['capture_start_t']) * 10 + 180) * hz
    end = ob.last_frame(obs)
    triggered = fb is not None and end is not None and end >= due
    f2 = ob.first_frame(obs, me, lambda n: n == DERRICK, 2)
    fired = triggered and f2 is not None and f2 <= due
    return {'triggered': bool(triggered), 'fired': bool(fired), 'detail': 'barracks_f=%s due_f=%s second_derrick_f=%s' % (fb, due, f2)}
