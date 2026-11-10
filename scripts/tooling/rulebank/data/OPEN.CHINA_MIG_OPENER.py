# GeneralsX: rule bank DATA rule OPEN.CHINA_MIG_OPENER: China Infantry opens Barracks, supply,
# Air Field and four MiGs, with the War Factory late. Stock Infantry Hard builds its War Factory at the first supply centre and its Air
# Field only at _ESCALATION >= 10 ("China Air Field - H", enemy-gated: a rule may neither retime it nor read that clock). Here:
#   a one-shot "GX MiG Opener Stock Air Field Off - H" switches the stock Air Field script off at the start (the one reference the
#   compiler allows to enemy-gated stock logic), and the one-shot "GX MiG Opener Air Field - H" builds the Air Field once the Barracks
#   and a supply centre are complete;
#   "China War Factory - H" (own state: the supply centre) also waits for _GX_CLOCK >= ceil(wf_delay_s / 10) or mig_open_n own MiGs;
#   a one-time team "GX MiG Opener" (mig_open_n MiGs) once the Air Field is complete, guarding the own outer perimeter like the stock
#   wave-1 guards; its creation sets the counter its production condition waits on, so it comes once.
# Own state only. armor_upg_after_migs is UNUSED: the MiG Armor upgrade script is TECH.CHINA_UPGRADE_GATES'.
RULE_ID = 'OPEN.CHINA_MIG_OPENER'
PARAMS = ('mig_open_n', 'wf_delay_s')
UNUSED = {'armor_upg_after_migs': 'the MiG Armor upgrade script is TECH.CHINA_UPGRADE_GATES\''}
IG, AF, WF, BX, SCN, MG = 'SkirmishChinaInfantryGeneral', 'Infa_ChinaAirfield', 'Infa_ChinaWarFactory', 'Infa_ChinaBarracks', 'Infa_ChinaSupplyCenter', 'Infa_ChinaJetMIG'
STOCK_AF, STOCK_WF, OFF, NEW = 'China Air Field - H', 'China War Factory - H', 'GX MiG Opener Stock Air Field Off - H', 'GX MiG Opener Air Field - H'
TEAM, COND, START, COUNT, GROUP, DONOR = 'GX MiG Opener', 'GX MiG Opener Cond - H', 'GX MiG Opener Start - H', 'GX MiG Opener', 'GX OPEN.CHINA_MIG_OPENER', 'China H 2W Guard Combat Zone. 1'
OUTER = '[Skirmish]MyOuterPerimeter'
CLAIMS = {'scripts': [[IG, n] for n in (STOCK_AF, STOCK_WF, OFF, NEW, COND, START)], 'groups': [[IG, GROUP]], 'teams': [TEAM]}


def _bare(scb, t):
    """a copied donor team keeps no hook of its own: every script field but the production condition is blanked"""
    for e in t['_e']:
        if e[0] != 'teamProductionCondition' and e[0].startswith('team') and 'Script' in e[0] and e[2]:
            rb.team_set(scb, t, e[0], '')


def apply(scb, scope, p):
    if IG not in scope:
        return
    me = ('SIDE', rb.SIDE_ME)
    built = lambda t: rb.cond(scb, 'BUILT_BY_PLAYER', ('OBJECT_TYPE', t), me)
    kids = [rb.new_script(scb, OFF, [[rb.cond(scb, 'CONDITION_TRUE')]], [rb.act(scb, 'DISABLE_SCRIPT', ('SCRIPT', STOCK_AF))], oneshot=True),
            rb.new_script(scb, NEW, [[built(BX), built(SCN)]], [rb.act(scb, 'SKIRMISH_BUILD_BUILDING', ('OBJECT_TYPE', AF))], oneshot=True),
            rb.new_script(scb, START, [[rb.cond(scb, 'CONDITION_TRUE')]],
                          [rb.act(scb, 'INCREMENT_COUNTER', ('INT', 1), ('COUNTER', COUNT)),
                           rb.act(scb, 'TEAM_GUARD_AREA', ('TEAM', '<This Team>'), ('TRIGGER_AREA', OUTER))], sub=True),
            rb.new_script(scb, COND, [[built(AF), rb.cond(scb, 'COUNTER', ('COUNTER', COUNT), ('COMPARISON', rb.LT), ('INT', 1))]], [], sub=True)]
    rb.owner_list(scb, IG).kids.append(rb.new_group(GROUP, kids))
    if p['wf_delay_s']:
        _g, _k, s = rb.find_script(scb, IG, STOCK_WF)
        ors = [x for x in s.kids if x.name == 'OrCondition']
        assert len(ors) == 1 and [rb.decode(scb, l)[0] for l in ors[0].kids] == ['BUILT_BY_PLAYER'], ors
        g = ors[0].kids[0]
        new = [mss.Node('OrCondition', ors[0].ver, kids=[mss.deep_copy(g), rb.clock_at_least(scb, -(-p['wf_delay_s'] // 10))]),
               mss.Node('OrCondition', ors[0].ver, kids=[mss.deep_copy(g), rb.own_count(scb, rb.GE, p['mig_open_n'], MG)])]
        i = s.kids.index(ors[0]); s.kids[i:i + 1] = new
    ts = rb.teams(scb)
    donor = rb.team(ts, DONOR)
    assert donor is not None and rb.team_get(donor, 'teamOwner') == IG and rb.team_get(donor, 'teamUnitType1') == MG, DONOR
    t = {'_e': [list(e) for e in donor['_e']], 'teamName': None}
    _bare(scb, t)
    for k, v in (('teamName', TEAM), ('teamUnitMinCount1', p['mig_open_n']), ('teamUnitMaxCount1', p['mig_open_n']), ('teamUnitMinCount2', 0),
                 ('teamUnitMaxCount2', 0), ('teamUnitMinCount3', 0), ('teamUnitMaxCount3', 0), ('teamMaxInstances', 1),
                 ('teamProductionCondition', COND), ('teamOnCreateScript', START)):
        rb.team_set(scb, t, k, v)
    rb.add_new_team(ts, ts.index(donor) + 1, t)
    rb.put_teams(scb, ts)


def _lead(obs, me):
    """seconds the Air Field (any build state) was seen before the War Factory; None when either never came"""
    if not obs:
        return None
    hz = ob.hz(obs)
    fa, fw = ob.first_frame(obs, me, ob.ends_with('ChinaAirfield')), ob.first_frame(obs, me, ob.ends_with('ChinaWarFactory'))
    return None if fa is None or fw is None else (fw - fa) // hz


def activation(obs, me, params=None):
    """triggered: both buildings appear; fired: the Air Field before the War Factory (stock builds the War Factory at the supply centre)"""
    lead = _lead(obs, me)
    return {'triggered': lead is not None, 'fired': lead is not None and lead > 0, 'detail': 'airfield_leads_warfactory_s=%s' % lead}


CONTRAST = {'what': 'seconds the Air Field is seen before the War Factory', 'direction': 1, 'min_delta': 20}


def measure(obs, me, params=None):
    return _lead(obs, me)
