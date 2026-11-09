# GeneralsX @feature Claude 01/11/2026 rule bank DATA rule TECH.USA_COMANCHE_ORDER (C·rb-data-11): Air Force researches its Comanche
# upgrades in the order Stealth, Countermeasures, Rocket Pods. Stock Air Force Hard buys Stealth Comanche at its first Air Field
# ("USA Upgrade Stealth Comanche", 2500 cash) and Countermeasures at its second ("USA Upgrade to Counter Measures", 1500), so the order
# is usually but not strictly Stealth first, and it never buys Rocket Pods (no stock script; the Air Field offers them). Here, with
# stealth_first 1 the Countermeasures script also waits for Stealth (0: Stealth waits for Countermeasures), and with pods_after_cm 1
# "GX Comanche Rocket Pods - H" orders the Pods once Countermeasures are done, the cash is 1500 and an own Air Field stands - repeating
# (Codex rbd11-r1 P2: a one-shot fired with no Air Field queued nothing and never retried; AIPlayer::buildUpgrade ignores the request while
# the upgrade is queued or complete). PLAYER_BUILT_UPGRADE is
# consumed by its first reader (ScriptConditions.cpp:1562), so one one-shot latch per upgrade raises an own flag and the gates read the
# flags; no stock script reads either upgrade. Own state only; the two stock scripts are clean one-shots. Upgrades are not in the obs
# stream: a smoke can prove the pack played, not the order (UNOBSERVABLE).
RULE_ID = 'TECH.USA_COMANCHE_ORDER'
SCOPE = ('SkirmishAmericaAirForceGeneral',)
PARAMS = ('stealth_first', 'pods_after_cm')
UNUSED = {'cm_after_stealth_s': '0 (Countermeasures the moment Stealth is done) only: a delay needs a timer started by an upgrade finishing'}
AF = 'SkirmishAmericaAirForceGeneral'
CM, ST, PODS = 'Upgrade_AmericaCountermeasures', 'AirF_Upgrade_StealthComanche', 'Upgrade_ComancheRocketPods'
SCM, SST = 'USA Upgrade to Counter Measures', 'USA Upgrade Stealth Comanche'
LATCH = {ST: ('GX Comanche Stealth Latch - H', 'GX Comanche Stealth Done'), CM: ('GX Comanche CM Latch - H', 'GX Comanche CM Done')}
POD, GROUP = 'GX Comanche Rocket Pods - H', 'GX TECH.USA_COMANCHE_ORDER'
CLAIMS = {'scripts': [[AF, n] for n in (SCM, SST, POD) + tuple(l for l, _f in LATCH.values())], 'groups': [[AF, GROUP]]}


def apply(scb, scope, p):
    if AF not in scope:
        return
    flag = lambda u: rb.cond(scb, 'FLAG', ('FLAG', LATCH[u][1]), ('BOOLEAN', 1))
    first, then = (ST, SCM) if p['stealth_first'] else (CM, SST)       # the upgrade that goes first, and the stock script that waits for it
    _g, _k, s = rb.find_script(scb, AF, then)
    ors = [x for x in s.kids if x.name == 'OrCondition']
    assert len(ors) == 1, then                                            # own state only: the Air Field count and cash
    ors[0].kids.append(flag(first))
    need = {first} | ({CM} if p['pods_after_cm'] else set())
    kids = [rb.new_script(scb, LATCH[u][0], [[rb.cond(scb, 'PLAYER_BUILT_UPGRADE', ('SIDE', rb.SIDE_ME), ('UPGRADE', u))]],
                          [rb.act(scb, 'SET_FLAG', ('FLAG', LATCH[u][1]), ('BOOLEAN', 1))], oneshot=True) for u in (ST, CM) if u in need]
    if p['pods_after_cm']:
        kids.append(rb.new_script(scb, POD, [[flag(CM), rb.credits_at_least(scb, 1500), rb.own_count(scb, rb.GE, 1, 'AirF_AmericaAirfield')]],
                                  [rb.act(scb, 'AI_PLAYER_BUILD_UPGRADE', ('SIDE', rb.SIDE_ME), ('UPGRADE', PODS))], oneshot=False))
    rb.owner_list(scb, AF).kids.append(rb.new_group(GROUP, kids))


def activation(obs, me, params=None):
    """the upgrades are not in the gym stream: UNOBSERVABLE exactly. triggered: an own Air Field by 120 s and the game ran past 400 s;
    fired: never (no reader can see an upgrade)"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz, end = ob.hz(obs), ob.last_frame(obs)
    fa = ob.first_frame(obs, me, ob.ends_with('AmericaAirfield'))
    triggered = fa is not None and fa <= 120 * hz and end is not None and end >= 400 * hz
    return {'triggered': bool(triggered), 'fired': False, 'detail': 'UNOBSERVABLE: upgrades are not in the obs stream; airfield_f=%s' % fa}


CONTRAST = {'unobservable': 'the Comanche upgrades (Stealth, Countermeasures, Rocket Pods) are not in the obs stream'}
