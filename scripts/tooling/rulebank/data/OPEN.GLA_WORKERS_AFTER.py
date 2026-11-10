# GeneralsX: rule bank DATA rule OPEN.GLA_WORKERS_AFTER: GLA trains more Workers once its Arms
# Dealer stands and again after its first Tunnel Network. Stock GLA Hard's opening Worker teams ("GLA Worker": 4, or OPEN.START_ORDER's
# count) come before both. Per GLA general this adds two one-time teams, copied from the general's own stock Worker team: "GX Workers After
# Dealer <g>" once an own Arms Dealer is complete and, with after_tunnel = 1, "GX Workers After Tunnel <g>" once an own Tunnel Network is
# complete, worker_n Workers each; each team's creation sets its own counter, which its production condition waits on, so each comes once,
# and then runs the stock Worker team's own on-create ("GLA Disband") as its orders, which frees the Workers to gather.
# Own state only. Composes with the pinned W2P.GLA_WORKERS (workers per stash), as its note says.
RULE_ID = 'OPEN.GLA_WORKERS_AFTER'
PARAMS = ('worker_n', 'after_tunnel')
GEN = {'SkirmishGLA': ('GLA', '', 'GLA Worker'), 'SkirmishGLADemolitionGeneral': ('Demo', 'Demo_', 'GLA Worker. 1'),
       'SkirmishGLAToxinGeneral': ('Toxin', 'Chem_', 'GLA Worker. 3'), 'SkirmishGLAStealthGeneral': ('Stealth', 'Slth_', 'GLA Worker. 2')}
DISBAND = 'GLA Disband'    # the stock Worker team's own on-create: it frees the Workers to gather
WHAT = (('Dealer', 'GLAArmsDealer'), ('Tunnel', 'GLATunnelNetwork'))
TEAM, COND, START, GROUP, COUNT = 'GX Workers After %s %s', 'GX Workers After %s Cond %s - H', 'GX Workers After %s Start %s - H', \
    'GX OPEN.GLA_WORKERS_AFTER', 'GX Workers After %s'
CLAIMS = {'scripts': [[o, n % (w, g)] for o, (g, _p, _d) in GEN.items() for w, _b in WHAT for n in (COND, START)] + [[o, DISBAND] for o in GEN],
          'groups': [[o, GROUP] for o in GEN], 'teams': sorted(TEAM % (w, g) for g, _p, _d in GEN.values() for w, _b in WHAT)}
UNTIL = 300


def _bare(scb, t):
    """a copied donor team keeps no hook of its own: every script field but the production condition is blanked (a hook would name stock
    logic this rule does not claim)"""
    for e in t['_e']:
        if e[0] != 'teamProductionCondition' and e[0].startswith('team') and 'Script' in e[0] and e[2]:
            rb.team_set(scb, t, e[0], '')


def apply(scb, scope, p):
    ts = rb.teams(scb)
    for o in scope:
        g, pre, donor_name = GEN[o]
        donor = rb.team(ts, donor_name)
        assert donor is not None and rb.team_get(donor, 'teamOwner') == o and rb.team_get(donor, 'teamUnitType1') == pre + 'GLAInfantryWorker', donor_name
        kids, at = [], ts.index(donor) + 1
        for w, b in WHAT:
            if w == 'Tunnel' and not p['after_tunnel']:
                continue
            c = COUNT % w
            kids.append(rb.new_script(scb, START % (w, g), [[rb.cond(scb, 'CONDITION_TRUE')]],
                                      [rb.act(scb, 'INCREMENT_COUNTER', ('INT', 1), ('COUNTER', c)),
                                       rb.act(scb, 'TEAM_EXECUTE_SEQUENTIAL_SCRIPT', ('TEAM', '<This Team>'), ('SCRIPT', DISBAND))], sub=True))
            kids.append(rb.new_script(scb, COND % (w, g), [[rb.cond(scb, 'BUILT_BY_PLAYER', ('OBJECT_TYPE', pre + b), ('SIDE', rb.SIDE_ME)),
                                                             rb.cond(scb, 'COUNTER', ('COUNTER', c), ('COMPARISON', rb.LT), ('INT', 1))]], [], sub=True))
            t = {'_e': [list(e) for e in donor['_e']], 'teamName': None}
            _bare(scb, t)
            for k, v in (('teamName', TEAM % (w, g)), ('teamUnitMinCount1', p['worker_n']), ('teamUnitMaxCount1', p['worker_n']),
                         ('teamMaxInstances', 1), ('teamIsSingleton', False),   # the donor is one: its empty map-load instance would fill the 1
                         ('teamProductionCondition', COND % (w, g)), ('teamOnCreateScript', START % (w, g))):
                rb.team_set(scb, t, k, v)
            rb.add_new_team(ts, at, t); at += 1
        rb.owner_list(scb, o).kids.append(rb.new_group(GROUP, kids))
    rb.put_teams(scb, ts)


def _workers(obs, me):
    return ob.built_by(obs, me, ob.ends_with('GLAInfantryWorker'), UNTIL * ob.hz(obs)) if obs else None


def activation(obs, me, params=None):
    """triggered: own Arms Dealer complete by 150 s and the game ran past 300 s; fired: 14 own Workers by 300 s (stock opens with 11-13)"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    fa = ob.first_frame(obs, me, ob.ends_with('GLAArmsDealer'), 1, complete=True)
    n = _workers(obs, me)
    triggered = fa is not None and fa <= 150 * hz and n is not None
    return {'triggered': bool(triggered), 'fired': bool(triggered and n >= 14), 'detail': 'armsdealer_f=%s workers_by_300s=%s' % (fa, n)}


CONTRAST = {'what': 'own Workers built by 300 s', 'direction': 1, 'min_delta': 2}


def measure(obs, me, params=None):
    return _workers(obs, me)
