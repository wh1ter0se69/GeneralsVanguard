# Copyright 2026 GeneralsX contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# rule bank DATA rule ARMY.GLA_TOXIN_ECON: Toxin puts money into army - Angry Mobs out
# early and on guard, more Barracks once rich (row 323). Stock Hard Toxin makes Angry Mobs only in its 5th-wave team (10 min+, a wave
# gate that reads _ESCALATION) and that team attacks as soon as it exists. This: the Mob team "GLA H 5TH Wave Mob C. 3" is produced
# from _GX_CLOCK 15 (150 s, a design constant), keeps only its stock OnCreate "GLA Guard Base" (no attack hook: guard mode), and at
# most mob_barracks such teams exist; with my money >= bank_trigger, Barracks 2..mob_barracks are added one at a time.
# NOT here: max_black_markets (row 321) - Black Markets 3 and 4 are gift scripts ($3,000 each), so capping them needs those switched
# off: ENGINE (a BUILDORDER hook).
RULE_ID = 'ARMY.GLA_TOXIN_ECON'
PARAMS = ('mob_barracks', 'bank_trigger')
UNUSED = {'max_black_markets': 'no cap is the answer (Dominator stand-in 31/10); a cap would need the gift-carrying Black Market 3/4 scripts switched off: ENGINE'}
TX = 'SkirmishGLAToxinGeneral'
MOB_TEAM, EARLY, STOCK_GATE, GROUP = 'GLA H 5TH Wave Mob C. 3', 'GX GLA Toxin Mob Early - H', '_GLA_5TH_WAVE_H', 'GX ARMY.GLA_TOXIN_ECON'
BARRACKS, RAX = 'GX GLA Mob Barracks %d - H', 'Chem_GLABarracks'
MOB_FROM = 15                                 # _GX_CLOCK ticks (150 s)
# the Mob team's build priority: stock 140 loses to a dozen Toxin teams (500 tech capture / expanders, 310-300 defenders, 215-210 CT
# stealth), so eligible at 150 s it came at 416 s or never in the smoke; 400 builds it ahead of the defenders, behind capture,
# expansion and the invasion alerts (1000)
MOB_PRIORITY = 400
# the Mob's prerequisites are Chem_GLABarracks + Chem_GLAPalace (INIZH ChemicalGeneral.ini, Chem_GLAInfantryAngryMobNexus) and stock's
# "GLA Palace - H" waits for _ESCALATION 30 (Palace done at 385-395 s in the smokes): an early Palace from the same clock, once the
# Arms Dealer (the Palace's own prerequisite, $2500 / 45 s) stands. Stock's gift script is untouched; its later SKIRMISH_BUILD_BUILDING
# finds the Palace built and does nothing (AISkirmishPlayer::buildSpecificAIBuilding: "already built or queued"). Black Market 1's stock
# gate (BUILT_BY_PLAYER Palace, a frozen gift script) therefore opens earlier too - its text and gift are unchanged.
# Black Market 1's $3,000 gift therefore comes earlier - about Palace time + 45 s, not ~390 s. Any
# Toxin A/B, tune campaign or smoke reads that gift as PART OF THIS ARM (the arm = early Palace + early Mob + the earlier gift); do not
# attribute a Toxin income change to the Mob alone. max_black_markets: Dominator's stand-in answer is "no cap", so UNUSED below is now
# the intended behaviour (RULES.stand-in.tsv default 5 = the registry maximum), not a missing lever.
PALACE, DEALER = 'GX GLA Toxin Palace Early - H', 'Chem_GLAArmsDealer'
CLAIMS = {'scripts': [[TX, EARLY], [TX, PALACE]] + [[TX, BARRACKS % j] for j in (2, 3, 4)], 'groups': [[TX, GROUP]], 'teams': [MOB_TEAM]}


def apply(scb, scope, p):
    if TX not in scope:
        return
    me = ('SIDE', rb.SIDE_ME)
    _g, _k, s0 = rb.find_script(scb, TX, STOCK_GATE)               # the stock production-condition shape: its team action, our gate
    acts = [mss.deep_copy(k) for k in s0.kids if k.name == 'ScriptAction']
    kids = [rb.new_script(scb, EARLY, [[rb.clock_at_least(scb, MOB_FROM)]], acts, oneshot=False, sub=True)]
    kids.append(rb.new_script(scb, PALACE, [[rb.clock_at_least(scb, MOB_FROM), rb.cond(scb, 'BUILT_BY_PLAYER', ('OBJECT_TYPE', DEALER), me)]],
                              [rb.act(scb, 'SKIRMISH_BUILD_BUILDING', ('OBJECT_TYPE', 'Chem_GLAPalace'))], oneshot=True))
    for j in range(2, p['mob_barracks'] + 1):
        kids.append(rb.new_script(scb, BARRACKS % j, [[rb.credits_at_least(scb, p['bank_trigger']), rb.own_count(scb, rb.EQ, j - 1, RAX)]],
                                  [rb.act(scb, 'AI_PLAYER_BUILD_SUPPLY_CENTER', me, ('OBJECT_TYPE', RAX), ('INT', 1000))],
                                  oneshot=True, delay=5))
    rb.owner_list(scb, TX).kids.append(rb.new_group(GROUP, kids))
    ts = rb.teams(scb)
    t = rb.team(ts, MOB_TEAM)
    assert t is not None and rb.team_get(t, 'teamOwner') == TX and rb.team_get(t, 'teamProductionCondition') == STOCK_GATE, MOB_TEAM
    rb.team_set(scb, t, 'teamProductionCondition', EARLY)
    rb.team_set(scb, t, 'teamGenericScriptHook0', '')
    rb.team_set(scb, t, 'teamMaxInstances', p['mob_barracks'])
    rb.team_set(scb, t, 'teamProductionPriority', MOB_PRIORITY)
    rb.put_teams(scb, ts)


def activation(obs, me, params=None):
    """triggered: own Barracks complete by 60 s and the game ran past 360 s; fired: an own Angry Mob by 360 s (stock: wave 5, 10 min+)"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    fb = ob.first_frame(obs, me, ob.ends_with('GLABarracks'), 1, complete=True)
    end = ob.last_frame(obs)
    triggered = fb is not None and fb <= 60 * hz and end is not None and end >= 360 * hz
    fm = ob.first_frame(obs, me, ob.ends_with('AngryMobNexus'))
    fired = triggered and fm is not None and fm <= 360 * hz
    return {'triggered': bool(triggered), 'fired': bool(fired), 'detail': 'barracks_f=%s mob_f=%s' % (fb, fm)}
