# GeneralsX @feature Claude 30/10/2026 rule bank DATA rule OIL.COUNT_GATES, CHINA PART (C·rb-data): China's Helix teams wait until the
# Capture Building research (Upgrade_InfantryCaptureBuilding) is done - research capture before the Helix, WITHOUT awaiting captures
# (Dominator's answer, 31/10; rb-data-4 rewrote the gate from "own derricks >= oils_before_helix"). oils_before_helix is now an on/off
# switch (0 = stock, any other value = gate on the research); the oil_wait_s clock floor still opens the gate if the research never lands. Stock Hard China builds its Helix teams from the
# 3rd wave on, gated only by the wave counter. This replaces those four teams' production conditions with copies of the stock wave
# gate AND (Capture Building researched OR _GX_CLOCK >= 25 + ceil(oil_wait_s/10), wave 3's nominal 250 s plus the wait) - an
# own-state gate on top of the stock one. The copied wave gate reads _ATTACK_WAVE (stock raises it from _ESCALATION, which enemy tech
# feeds): the compiler accepts that only as the stock gate itself, copied unchanged with the stock script's own actions.
# oils_before_helix = 0 is stock (nothing emitted).
# NOT here: the one-centre swap (oil_swap_min_oils) needs the gift-carrying Base Expansion Check switched off - ENGINE (a BUILDORDER
# hook); the USA part is the challenger of wh1ter0se's pinned two centres (plan 3.4); China builds no refinery; no scb action removes a dozer.
RULE_ID = 'OIL.COUNT_GATES'
SCOPE = ('SkirmishChina',)
PARAMS = ('oils_before_helix', 'oil_wait_s')
UNUSED = {'oil_swap_min_oils': 'the one-centre swap needs the gift-carrying Base Expansion Check switched off: ENGINE',
          'refinery_min_oils': 'China has no buildable refinery (TechOilRefinery is a capture-only tech building)',
          'max_dozers': 'no scb action removes a dozer'}
CN = 'SkirmishChina'
WAVE = {3: '_CHINA_3RD_WAVE_H', 4: '_CHINA_4TH_WAVE_H', 5: '_CHINA_5TH_WAVE_H'}
TEAMS = {'China H 3W Base Guards': 3, 'China H 4W Base Guards': 4, 'China H 5W Base Guards': 5, 'China H 5W Helix Attackers': 5}
DONE, LATCH = 'GX China Capture Researched', 'GX China Capture Research Latch'
COND, GROUP = 'GX China Helix Cond %d', 'GX OIL.COUNT_GATES'
WAVE3_NOMINAL = 25                            # _GX_CLOCK ticks: stock wave 3 sets at _ESCALATION 25 (~250 s without enemy tech)
CLAIMS = {'scripts': [[CN, COND % n] for n in WAVE] + [[CN, LATCH]], 'groups': [[CN, GROUP]], 'teams': sorted(TEAMS)}
CAPTURE_UPGRADE = 'Upgrade_InfantryCaptureBuilding'    # INIZH Upgrade.ini: the Capture Building research (Red Guard / Ranger / Rebel), 1000 cash, 30 s
DEFAULTS = {'oils_before_helix': 2, 'oil_wait_s': 60}


def apply(scb, scope, params):
    if CN not in scope or params['oils_before_helix'] == 0:
        return
    # PLAYER_BUILT_UPGRADE is consumed by the first script that reads it (ScriptConditions.cpp:1562, removeFromList = TRUE): one latch
    # script reads it and raises my own flag; the gates read the flag (Codex rbd4-r1 P2)
    kids = [rb.new_script(scb, LATCH, [[rb.cond(scb, 'PLAYER_BUILT_UPGRADE', ('SIDE', rb.SIDE_ME), ('UPGRADE', CAPTURE_UPGRADE))]],
                          [rb.act(scb, 'SET_FLAG', ('FLAG', DONE), ('BOOLEAN', 1))], oneshot=True, sub=False)]
    floor = WAVE3_NOMINAL + -(-params['oil_wait_s'] // 10)
    for n, stock in sorted(WAVE.items()):
        _g, _k, s0 = rb.find_script(scb, CN, stock)
        ors = [k for k in s0.kids if k.name == 'OrCondition']
        assert len(ors) == 1, '%s: %d OR blocks' % (stock, len(ors))
        g = ors[0].kids
        acts = [mss.deep_copy(k) for k in s0.kids if k.name == 'ScriptAction']
        kids.append(rb.new_script(scb, COND % n, [[mss.deep_copy(x) for x in g] + [rb.cond(scb, 'FLAG', ('FLAG', DONE), ('BOOLEAN', 1))],
                                                  [mss.deep_copy(x) for x in g] + [rb.clock_at_least(scb, floor)]],
                                  acts, oneshot=False, sub=True))
    rb.owner_list(scb, CN).kids.append(rb.new_group(GROUP, kids))
    ts = rb.teams(scb)
    for name, n in TEAMS.items():
        t = rb.team(ts, name)
        assert t is not None and rb.team_get(t, 'teamOwner') == CN and rb.team_get(t, 'teamProductionCondition') == WAVE[n], name
        rb.team_set(scb, t, 'teamProductionCondition', COND % n)
    rb.put_teams(scb, ts)


def activation(obs, me, params=None):
    """(the research itself is not in the gym stream, so the oil count stays a proxy for "capture teams ran": UNOBSERVABLE exactly)
    triggered: own Air Field complete by 240 s, fewer than oils_before_helix own derricks at 330 s, game ran past 330 s;
    fired: no own Helix by 330 s (stock's 3rd-wave Helix comes ~250-300 s in, once the Air Field and money allow)"""
    p = dict(DEFAULTS, **(params or {}))
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    fa = ob.first_frame(obs, me, ob.ends_with('ChinaAirfield'), 1, complete=True)
    end = ob.last_frame(obs)
    oils = ob.first_frame(obs, me, lambda n: n == 'TechOilDerrick', max(1, p['oils_before_helix']))
    poor = oils is None or oils > 330 * hz
    triggered = fa is not None and fa <= 240 * hz and end is not None and end >= 330 * hz and poor and p['oils_before_helix'] > 0
    fh = ob.first_frame(obs, me, ob.ends_with('ChinaVehicleHelix'))
    fired = triggered and (fh is None or fh > 330 * hz)
    return {'triggered': bool(triggered), 'fired': bool(fired), 'detail': 'UNOBSERVABLE: the Capture research is not in the obs stream; airfield_f=%s oils_f=%s helix_f=%s' % (fa, oils, fh)}


# rb-data-6: no reader can tell this rule's ON from OFF - the gate waits on a research, and the research is not in the gym stream (the 31/10
# smoke4 games never reached the situation: Helix at 264-295 s in both arms). smoke-read.py prints UNOBSERVABLE, never PASS or FAIL.
CONTRAST = {'unobservable': 'the Capture research the gate waits on is not in the obs stream; the smoke games never reach Helix-before-research'}
