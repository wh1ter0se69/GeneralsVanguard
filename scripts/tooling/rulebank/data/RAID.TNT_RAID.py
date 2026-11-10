# GeneralsX: rule bank DATA rule RAID.TNT_RAID: the Demolition general strikes the USA
# Superweapon GENERAL early with Terrorists at its dozers, derricks and War Factory (row 293: "TNT raids killed Dozers, 2 oil derricks and
# the War Factory and opened a huge income gap; the late game favours USA"; rows 720, 849, 877 the same raid by other names). Stock Hard
# Demolition fields no Terrorist before its wave-5 "Suicide" teams (600 s). This, for the Demolition general:
#   MATCHUP 'USA': the SHARED latch (rb.matchup) sets gxrb_mu_usa once AI_PLAYER_GX_SENSE says my enemy is USA;
#   a GENERAL read of its own (rb.general_latch, in the frame the key is set): gxrb_tnt_supw once the sensor says my enemy is the
#              Superweapon general - which it says only when the LOBBY showed that general (a Random enemy is never known by its
#              general: against a Random draw the rule stays off for the whole game, by design);
#   team "GX TNT Raid": tnt_raiders Terrorists (Barracks only), one team at a time, re-formed when lost (rows 1472: "keep raiding"),
#              produced once _GX_CLOCK >= ceil(raid_t / 10) behind the key and the general flag; on creation it stops, takes the stock
#              supply-lines priority ("GLA Apply Supply Lines Priority": stock's own supply hunters' table, Supply Lines first) and runs
#              the stock centre attack ("GLA Attack Enemy Base Center": the approach path, then TEAM_HUNT). No stock script or team
#              changes; the only enemy input is the fair sensor.
# Not a target choice by enemy value (guards.HIDDEN): the dozers / derricks come from the stock table and the hunt, as for stock's own
# supply hunters.
RULE_ID = 'RAID.TNT_RAID'
MATCHUP = 'USA'
PARAMS = ('raid_t', 'tnt_raiders')
O = 'SkirmishGLADemolitionGeneral'
SCOPE = (O,)
TAG, GROUP = 'GX TNT', 'GX RAID.TNT_RAID'
TEAM, COND, START = 'GX TNT Raid', 'GX TNT Raid Cond - H', 'GX TNT Raid Start - H'
FLAG, DONE = 'gxrb_tnt_supw', 'gxrb_tnt_done'
GEN = {'AmericaSuperWeaponGeneral': FLAG}
PRIO, ATTACK, STOCK_GATE = 'GLA Apply Supply Lines Priority', 'GLA Attack Enemy Base Center', '_GLA_1ST_WAVE_H'
DONOR = 'GLA H 5TH Suicide. 1'          # stock's own Terrorist team: its owner, priority and idle frames (its hooks are blanked)
CLAIMS = rb.matchup_claims(SCOPE, (), scripts=[[O, '%s Gen %s' % (TAG, g)] for g in GEN] + [[O, TAG + ' Gen Done'], [O, COND], [O, START],
                                                [O, PRIO], [O, ATTACK]],
                           groups=[[O, GROUP]], teams=[TEAM])
READ_S = 300


def apply(scb, scope, p):
    assert tuple(scope) == SCOPE, scope
    ts = rb.teams(scb)
    rb.matchup(scb, O)
    key = lambda: rb.flag_is(scb, rb.mu_flag(MATCHUP), True)
    kids = rb.general_latch(scb, TAG, MATCHUP, GEN, DONE)
    _x, _k, s0 = rb.find_script(scb, O, STOCK_GATE)           # its actions, as the raider rules copy them (stock: guard the perimeter)
    team_act = [mss.deep_copy(k) for k in s0.kids if k.name == 'ScriptAction']
    kids += [rb.new_script(scb, COND, [[key(), rb.flag_is(scb, FLAG, True), rb.clock_at_least(scb, -(-p['raid_t'] // 10))]], team_act, sub=True),
             rb.new_script(scb, START, [[rb.cond(scb, 'CONDITION_TRUE')]],
                           [rb.act(scb, 'TEAM_STOP', ('TEAM', '<This Team>')),
                            rb.act(scb, 'TEAM_EXECUTE_SEQUENTIAL_SCRIPT', ('TEAM', '<This Team>'), ('SCRIPT', ATTACK))], sub=True)]
    rb.owner_list(scb, O).kids.append(rb.new_group(GROUP, kids))
    donor = rb.team(ts, DONOR)
    assert donor is not None and rb.team_get(donor, 'teamOwner') == O, DONOR
    t = {'_e': [list(e) for e in donor['_e']], 'teamName': None}
    for e in t['_e']:                    # a copied donor keeps no hook of its own (a hook would name stock logic this rule does not claim)
        if e[0] != 'teamProductionCondition' and e[0].startswith('team') and 'Script' in e[0] and e[2]:
            rb.team_set(scb, t, e[0], '')
    for k, v in (('teamName', TEAM), ('teamUnitType1', 'Demo_GLAInfantryTerrorist'), ('teamUnitMinCount1', p['tnt_raiders']),
                 ('teamUnitMaxCount1', p['tnt_raiders']), ('teamUnitType2', '<none>'), ('teamUnitMinCount2', 0), ('teamUnitMaxCount2', 0),
                 ('teamUnitType3', '<none>'), ('teamUnitMinCount3', 0), ('teamUnitMaxCount3', 0), ('teamMaxInstances', 1),
                 ('teamProductionCondition', COND), ('teamOnCreateScript', START), ('teamGenericScriptHook0', PRIO)):
        rb.team_set(scb, t, k, v)
    t['teamName'] = TEAM
    assert rb.team_get(t, 'teamOwner') == O
    rb.add_new_team(ts, ts.index(donor) + 1, t)
    rb.put_teams(scb, ts)


CONTRAST = {'what': 'own Terrorists built by 300 s (stock Hard Demolition: its first are wave 5\'s Suicide teams, at 600 s)',
            'direction': 1, 'min_delta': 1}


def measure(obs, me, params=None):
    return ob.built_by(obs, me, ob.ends_with('GLAInfantryTerrorist'), READ_S * ob.hz(obs)) if obs else None


def activation(obs, me, params=None):
    """triggered: an own Barracks complete by 240 s and the game ran past 300 s; fired: 1 own Terrorist by 300 s (stock has none before
    600 s, so the activation alone tells ON from OFF in a game that reaches the raid)"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    bar = ob.first_frame(obs, me, ob.ends_with('GLABarracks'), 1, complete=True)
    end = ob.last_frame(obs)
    triggered = bar is not None and bar <= 240 * hz and end is not None and end > READ_S * hz
    n = measure(obs, me, params)
    return {'triggered': bool(triggered), 'fired': bool(triggered and (n or 0) >= 1), 'detail': 'barracks_f=%s terrorists_300s=%s' % (bar, n)}
