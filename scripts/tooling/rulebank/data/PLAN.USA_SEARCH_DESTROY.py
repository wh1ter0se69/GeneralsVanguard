# GeneralsX: rule bank DATA rule PLAN.USA_SEARCH_DESTROY: run the Strategy Center on Search and
# Destroy (+range, stealth detection) instead of Hold the Line, and build it right after the War Factory (rows 35, 97). Stock Hard USA
# and Air Force build the Strategy Center at War Factory (Airfield) + _ESCALATION >= 30 and their "USA Use Battle Plans" (Easy/Normal/
# Hard) picks Hold the Line. This adds Hard-only one-shots: one switches the stock plan script off at the start, one picks Search and
# Destroy once a Strategy Center of mine stands (and _GX_CLOCK >= ceil(plan_switch_t/10); 0 = no clock gate), and with sc_after_wf = 1
# one queues the Strategy Center once my War Factory is built (the stock build then finds it queued and does nothing; the Air Force
# general's stock build still carries its $3,000 gift, untouched). No enemy input (NO_ENEMY_OBJECTS, NO_ENEMY_FACTION: row 97's
# vs-China-Tank gate waits for SENSE.ENEMY_FACTION).
RULE_ID = 'PLAN.USA_SEARCH_DESTROY'
PARAMS = ('plan_sd_on', 'plan_switch_t', 'sc_after_wf')
GEN = {'SkirmishAmerica': ('', 'teamSkirmishAmerica'), 'SkirmishAmericaAirForceGeneral': ('AirF_', 'teamSkirmishAmericaAirForceGeneral')}
STOCK_PLAN = 'USA Use Battle Plans'
HARD_PLAN = 'USA Use Battle Plans - H'                # the recipe's USA_SD Hard copy: left on, it picks the plan at once and bypasses plan_switch_t
STOP, SD, SC, GROUP = 'GX Plan Stop Hold', 'GX Plan Search And Destroy', 'GX Plan Strategy Center After WF', 'GX PLAN.USA_SEARCH_DESTROY'
BUTTON = 'Command_InitiateBattlePlanSearchAndDestroy'
CLAIMS = {'scripts': [[o, n] for o in GEN for n in (STOP, SD, SC, STOCK_PLAN, HARD_PLAN)], 'groups': [[o, GROUP] for o in GEN]}   # STOCK_PLAN, HARD_PLAN: switched off


def ticks(seconds):
    return -(-seconds // 10)


def apply(scb, scope, params):
    me = ('SIDE', rb.SIDE_ME)
    for o in scope:
        p, team = GEN[o]
        rb.find_script(scb, o, STOCK_PLAN)                    # the scripts we switch off must be there
        rb.find_script(scb, o, HARD_PLAN)
        kids = []
        if params['plan_sd_on']:
            kids.append(rb.new_script(scb, STOP, [[rb.cond(scb, 'CONDITION_TRUE')]], [rb.act(scb, 'DISABLE_SCRIPT', ('SCRIPT', STOCK_PLAN)), rb.act(scb, 'DISABLE_SCRIPT', ('SCRIPT', HARD_PLAN))],
                                      oneshot=True))
            gate = [rb.own_count(scb, rb.GE, 1, p + 'AmericaStrategyCenter')]
            if params['plan_switch_t'] > 0:
                gate.append(rb.clock_at_least(scb, ticks(params['plan_switch_t'])))
            kids.append(rb.new_script(scb, SD, [gate], [rb.act(scb, 'TEAM_USE_COMMANDBUTTON_ABILITY', ('TEAM', team),
                                                               ('COMMANDBUTTON_ALL_ABILITIES', BUTTON))], oneshot=True))
        if params['sc_after_wf']:
            kids.append(rb.new_script(scb, SC, [[rb.cond(scb, 'BUILT_BY_PLAYER', ('OBJECT_TYPE', p + 'AmericaWarFactory'), me)]],
                                      [rb.act(scb, 'SKIRMISH_BUILD_BUILDING', ('OBJECT_TYPE', p + 'AmericaStrategyCenter'))], oneshot=True))
        if kids:
            rb.owner_list(scb, o).kids.append(rb.new_group(GROUP, kids))


def activation(obs, me, params=None):
    """triggered: own War Factory complete by 240 s and the game ran past 300 s; fired: an own Strategy Center (any build state) within
    60 s of that War Factory (stock orders it at _ESCALATION 30: ~300 s, earlier when the enemy's tech raises it - 291 s in the 30/10
    smoke). The plan pick itself is not in the obs stream."""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    fw = ob.first_frame(obs, me, ob.ends_with('AmericaWarFactory'), 1, complete=True)
    end = ob.last_frame(obs)
    triggered = fw is not None and fw <= 240 * hz and end is not None and end >= 300 * hz
    fs = ob.first_frame(obs, me, ob.ends_with('AmericaStrategyCenter'))
    fired = triggered and fs is not None and fs <= fw + 60 * hz
    return {'triggered': bool(triggered), 'fired': bool(fired), 'detail': 'warfactory_f=%s strategy_center_f=%s' % (fw, fs)}
