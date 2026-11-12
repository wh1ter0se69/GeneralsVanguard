# Copyright 2026 GeneralsX contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# rule bank DATA rule TECH.GLA_ARMS_DEALER_CAP: GLA caps its Arms Dealers. Stock GLA
# Hard queues three early - "GLA Arms Dealer - H", "2 - H" and "3 - H" wait for an own Supply Stash (Demolition and Stealth: #3 for two
# own Arms Dealers); the build list holds three, none AutomaticallyBuild. Here the scripts past arms_dealer_max get the condition "fewer than 0 own Arms Dealers", which never
# holds (the scripts run in one frame, so a real count cap could not tell them apart; CONDITION_FALSE has no stock leaf), so they never queue theirs:
# max 2 drops the third, max 1 the second too. Own build state only; the scripts carry no gift. Vanilla, Demolition
# and Stealth: Toxin's list names two different scripts "GLA Arms Dealer 2 - H", which the compiler refuses to edit.
# rb-data-11: stock's 4th Arms Dealer (smoke8: 393 s, cash 19123; ON at cap 2 it was the 3rd) comes from "GLA High Money Builds RICH<j>
# <pre>GLAArmsDealer" (cash >= 15000/30000/50000/80000 and _ESCALATION >= 10/20/30/45: enemy-gated, so not editable): RICH<j> is dealer
# 3 + j, and the one-shot "GX Stock RICH Arms Dealers Off - H" DISABLEs (the one reference allowed to enemy-gated logic) every RICH<j>
# with 3 + j > arms_dealer_max at the start. "GLA High Money Builds - H" (cash 30000 and exactly one Supply Stash; clean) places a flank
# Arms Dealer beside a Tunnel, a Stinger Site and a Barracks: past every cap in the box (the three stock scripts fire at the Supply
# Stash, long before 30000), so its Arms Dealer action goes and the other three stay.
RULE_ID = 'TECH.GLA_ARMS_DEALER_CAP'
SCOPE = ('SkirmishGLA', 'SkirmishGLADemolitionGeneral', 'SkirmishGLAStealthGeneral')
PRE = {'SkirmishGLA': '', 'SkirmishGLADemolitionGeneral': 'Demo_', 'SkirmishGLAStealthGeneral': 'Slth_'}
PARAMS = ('arms_dealer_max',)
UNUSED = {'post_open_choice_enum': '0 (a second Arms Dealer) is stock; 1 (Black Markets) runs through gift-paying Black Market scripts (ENGINE)'}
NTH = {2: 'GLA Arms Dealer 2 - H', 3: 'GLA Arms Dealer 3 - H'}
RICH, OFF, GROUP = 'GLA High Money Builds RICH%d %sGLAArmsDealer', 'GX Stock RICH Arms Dealers Off - H', 'GX TECH.GLA_ARMS_DEALER_CAP'
HIGH = 'GLA High Money Builds - H'
CLAIMS = {'scripts': [[o, n] for o in SCOPE for n in NTH.values()] + [[o, RICH % (j, PRE[o])] for o in SCOPE for j in (1, 2, 3, 4)] +
          [[o, OFF] for o in SCOPE] + [[o, HIGH] for o in SCOPE], 'groups': [[o, GROUP] for o in SCOPE]}
UNTIL = 400


def apply(scb, scope, p):
    for o in scope:
        for k, n in NTH.items():
            if k <= p['arms_dealer_max']:
                continue
            _g, _k, s = rb.find_script(scb, o, n)
            ors = [x for x in s.kids if x.name == 'OrCondition']
            assert len(ors) == 1, (o, n)               # own build state only: the Supply Stash, or (Demolition, Stealth) Arms Dealer #2
            ors[0].kids = [rb.own_count(scb, rb.LT, 0, PRE[o] + 'GLAArmsDealer')]
        off = [rb.act(scb, 'DISABLE_SCRIPT', ('SCRIPT', RICH % (j, PRE[o]))) for j in (1, 2, 3, 4) if 3 + j > p['arms_dealer_max']]
        _g, _k, hs = rb.find_script(scb, o, HIGH)
        ad = [k for k in hs.kids if k.name == 'ScriptAction' and rb.decode(scb, k) == ('SKIRMISH_BUILD_STRUCTURE_FLANK', [('OBJECT_TYPE', PRE[o] + 'GLAArmsDealer')])]
        assert len(ad) == 1, (o, HIGH)
        hs.kids.remove(ad[0])
        rb.owner_list(scb, o).kids.append(rb.new_group(GROUP, [rb.new_script(scb, OFF, [[rb.cond(scb, 'CONDITION_TRUE')]], off, oneshot=True)]))


def _ads(obs, me):
    return ob.built_by(obs, me, ob.ends_with('GLAArmsDealer'), UNTIL * ob.hz(obs)) if obs else None


def activation(obs, me, params=None):
    """triggered: an own Arms Dealer by 150 s and the game ran past 400 s; fired: no more than 2 (the default cap) by 400 s"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    fa = ob.first_frame(obs, me, ob.ends_with('GLAArmsDealer'), 1)
    n = _ads(obs, me)
    triggered = fa is not None and fa <= 150 * hz and n is not None
    return {'triggered': bool(triggered), 'fired': bool(triggered and n <= 2), 'detail': 'armsdealer_f=%s arms_dealers_by_400s=%s' % (fa, n)}


CONTRAST = {'what': 'own Arms Dealers built by 400 s', 'direction': -1, 'min_delta': 1}


def measure(obs, me, params=None):
    return _ads(obs, me)
