# GeneralsX: rule bank DATA rule ARMY.CHINA_GENERAL_MIX (C·rb-data-4): Dominator's answer - Nuke: mostly
# Battlemasters and Gattling Tanks; Tank: Battlemaster + Gattling + ECM, Emperors stay rare (in this game version Subliminal does not
# boost the Emperor). The Nuke and Tank generals' Hard wave tank teams keep their total of Battlemasters + Emperors, T, and re-split it:
#   Gattlings G = round(T * gattling_share_pm / 1000), Emperors E = min(stock Emperors, round(T * emperor_share_pm / 1000)),
#   Battlemasters = T - G - E. A team with no slot for the Gattling (every slot holds a unit the answer keeps) stays stock.
# Slots with a stock maximum of 0 count as free. Overlords, Dragons, ECM, MiGs and the Nuke Cannon waves are not touched.
# battlemaster_share_pm is the remainder (UNUSED); subliminal_gate is TECH.CHINA_UPGRADE_GATES' job (UNUSED here).
RULE_ID = 'ARMY.CHINA_GENERAL_MIX'
SCOPE = ('SkirmishChinaNukeGeneral', 'SkirmishChinaTankGeneral')
PARAMS = ('gattling_share_pm', 'emperor_share_pm')
UNUSED = {'battlemaster_share_pm': 'the remainder after the Gattling and Emperor shares',
          'subliminal_gate': 'the Subliminal gate is TECH.CHINA_UPGRADE_GATES (no second owner of that script)'}
NONE = '<none>'
PRE = {'SkirmishChinaNukeGeneral': 'Nuke_', 'SkirmishChinaTankGeneral': 'Tank_'}
SUF = {'SkirmishChinaNukeGeneral': '. 2', 'SkirmishChinaTankGeneral': '. 3'}
TEAMS = {'SkirmishChinaNukeGeneral': ('China H 2ND Wave Tanks', 'China H 3RD Wave Tanks F', 'China H 3RD Wave Tanks C',
                                      'China H 4TH Wave Tanks F', 'China H 4TH Wave Tanks B', 'China H 5TH Wave Tanks F'),
         'SkirmishChinaTankGeneral': ('China H 2ND Wave Tanks', 'China H 3RD Wave Tanks F', 'China H 3RD Wave Tanks C',
                                      'China H 3RD Wave Tanks', 'China H 4TH Wave Tanks F', 'China H 4TH Wave Tanks B',
                                      'China H 5TH Wave Tanks F', 'China H 5TH Wave Tanks B', 'China H 5TH Wave Tanks C')}
TEAM_UNIT_WAIVERS = {p + 'ChinaTankGattling': 'INIZH: the general\'s own War Factory only, which the Battlemaster of these teams needs anyway' for p in ('Nuke_', 'Tank_')}
CLAIMS = {'teams': sorted(n + SUF[o] for o, ns in TEAMS.items() for n in ns)}
DEFAULTS = {'gattling_share_pm': 300, 'emperor_share_pm': 100}


def _rnd(x):
    return int((2 * x + 1) // 2)


def _mix(slots, pre, g_pm, e_pm):
    """new slots for one team, or None when it stays stock"""
    bm, ga, em = pre + 'ChinaTankBattleMaster', pre + 'ChinaTankGattling', pre + 'ChinaTankEmperor'
    free = lambda s: s[0] == NONE or s[2] == 0
    ib = [i for i, s in enumerate(slots) if s[0] == bm and s[2] > 0]
    ie = [i for i, s in enumerate(slots) if s[0] == em and s[2] > 0]
    ig = [i for i, s in enumerate(slots) if s[0] == ga and s[2] > 0]
    T = sum(slots[i][2] for i in ib + ie)
    if T < 2 or len(ib) > 1 or len(ie) > 1 or len(ig) > 1:
        return None
    G = _rnd(T * g_pm / 1000.0)
    E = min(slots[ie[0]][2], _rnd(T * e_pm / 1000.0)) if ie else 0
    if ig:
        G = max(G, slots[ig[0]][2])
    G = min(G, T)
    E = min(E, T - G)                                     # the shares may not add up to more units than the team had
    B = T - G - E
    fixed = all(slots[i][1] == slots[i][2] for i in ib + ie)   # a team that required all its heavy units still requires all of them
    keep = {i for i, s in enumerate(slots) if not free(s) and i not in ib + ie + ig}
    want = [(bm, B, ib), (em, E, ie), (ga, G, ig)]
    out = [list(s) for s in slots]
    used = set(keep)
    for typ, n, own in want:                     # own slots first, so a unit type keeps its place
        if own:
            out[own[0]] = [typ, n if fixed else min(slots[own[0]][1], n), n] if n > 0 else [NONE, 0, 0]
            if n > 0:
                used.add(own[0])
    for typ, n, own in want:
        if n > 0 and not own:
            spare = [i for i in range(3) if i not in used and i not in keep]
            if not spare:
                return None
            used.add(spare[0]); out[spare[0]] = [typ, n if fixed else 0, n]
    return [tuple(s) for s in out]


def apply(scb, scope, p):
    ts = rb.teams(scb)
    for owner in scope:
        for n in TEAMS[owner]:
            t = rb.team(ts, n + SUF[owner])
            assert t is not None and rb.team_get(t, 'teamOwner') == owner, n
            slots = [(rb.team_get(t, 'teamUnitType%d' % i), rb.team_get(t, 'teamUnitMinCount%d' % i), rb.team_get(t, 'teamUnitMaxCount%d' % i))
                     for i in (1, 2, 3)]
            new = _mix(slots, PRE[owner], p['gattling_share_pm'], p['emperor_share_pm'])
            if new is None:
                continue
            for i, (typ, mn, mx) in enumerate(new, 1):
                rb.team_set(scb, t, 'teamUnitType%d' % i, typ)
                rb.team_set(scb, t, 'teamUnitMinCount%d' % i, mn)
                rb.team_set(scb, t, 'teamUnitMaxCount%d' % i, mx)
    rb.put_teams(scb, ts)


def activation(obs, me, params=None):
    """triggered: own War Factory complete by 200 s and the game ran past 400 s; fired: three own Gattling Tanks by 400 s (the stock Hard
    Nuke/Tank waves field Gattlings only in the 2-unit fast-attack teams)"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    fw = ob.first_frame(obs, me, ob.ends_with('ChinaWarFactory'), 1, complete=True)
    end = ob.last_frame(obs)
    triggered = fw is not None and fw <= 200 * hz and end is not None and end >= 400 * hz
    g3 = ob.first_frame(obs, me, ob.ends_with('ChinaTankGattling'), 3)
    fired = triggered and g3 is not None and g3 <= 400 * hz
    return {'triggered': bool(triggered), 'fired': bool(fired), 'detail': 'warfactory_f=%s gattling3_f=%s' % (fw, g3)}


# rb-data-6: stock already fields Gattlings, so activation() fires on OFF and cannot tell ON from OFF. smoke-read.py judges measure() of an ON
# game against the OFF game of the same seed: the Gattling share of the tank census at 400 s must rise by at least min_delta.
CONTRAST = {'what': 'Gattling / (Gattling + Battlemaster + Emperor) own tanks at 400 s', 'direction': 1, 'min_delta': 0.05}


def measure(obs, me, params=None):
    hz = ob.hz(obs)
    n = lambda suffix: ob.census(obs, me, ob.ends_with(suffix), 400 * hz)
    g, b, e = n('ChinaTankGattling'), n('ChinaTankBattleMaster'), n('ChinaTankEmperor')
    if g is None or g + b + e == 0:
        return None
    return g / float(g + b + e)
