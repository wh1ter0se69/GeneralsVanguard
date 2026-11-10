# GeneralsX: rule bank DATA rule TECH.CHINA_UPGRADE_GATES (C·rb-data): buy China's key upgrades on the army
# they help, and Black Napalm only in a safe window (rows 132, 134, 180, 254). Stock Hard China buys Black Napalm and MiG Armor at 4
# Dragons / MiGs, Subliminal Messaging on a Speaker Tower alone, the nuke upgrades behind 2 Overlords, and Tank's Autoloader the moment
# its Propaganda Center stands. Per China general the stock upgrade scripts change in place:
#   Black Napalm: napalm_with_dragons Dragons or migs_min MiGs, $2,000, and _GX_CLOCK >= napalm_min_esc (the safe window: own clock only,
#   no_enemy_near stays closed at 0 - no enemy-area condition exists here); MiG Armor: migs_min MiGs;
#   Subliminal Messaging: the Speaker Tower and $2,000 AND (overlords_min Overlords OR helix_min Helixes); Tank and Infantry have no
#   Overlord term, Tank keeps the stock gate (Dominator stand-in answer 31/10: not for Emperor towers);
#   Autoloader (Tank): also autoloader_cash; Uranium Shells / Nuke Tanks (vanilla, Nuke): overlords_min Overlords, and nukeupg = 1 drops
#   the Overlord gate from Nuke Tanks (the Nuclear Missile building's upgrade early, row 254).
# The Black Napalm / MiG Armor / Subliminal / Autoloader scripts are shared with Easy and Normal: exact under the tier gate, Normal's too
# under --tier-gate off (normal_also). Own state only. Activation: upgrades are not in the gym stream, so "fired" is UNOBSERVABLE.
RULE_ID = 'TECH.CHINA_UPGRADE_GATES'
PARAMS = ('migs_min', 'napalm_with_dragons', 'overlords_min', 'helix_min', 'napalm_min_esc', 'autoloader_cash', 'nukeupg')
UNUSED = {'no_enemy_near': 'closed at 0: no enemy-area condition until SENSE.FOG_CONDITIONS'}
P = {'SkirmishChina': '', 'SkirmishChinaTankGeneral': 'Tank_', 'SkirmishChinaNukeGeneral': 'Nuke_', 'SkirmishChinaInfantryGeneral': 'Infa_'}
NAPALM, ARMOR, SUBLIM, AUTO, URANIUM, NUKETANKS = 'China Upgrade Black Napalm', 'China Upgrade MiG Armor', 'China Upgrade Subliminal Messaging', \
    'China Upgrade Auto Loader', 'China Upgrade Uranium Shells', 'China Upgrade Nuke Tanks'
HAS = {'SkirmishChina': (NAPALM, ARMOR, SUBLIM, URANIUM, NUKETANKS), 'SkirmishChinaNukeGeneral': (NAPALM, ARMOR, SUBLIM, URANIUM, NUKETANKS),
       'SkirmishChinaInfantryGeneral': (NAPALM, ARMOR, SUBLIM), 'SkirmishChinaTankGeneral': (NAPALM, SUBLIM, AUTO)}
CLAIMS = {'scripts': [[o, n] for o, ns in HAS.items() for n in ns]}
DEFAULTS = {'migs_min': 2, 'napalm_with_dragons': 3}


def _overlord(o):
    # Tank and Infantry: no Overlord term (Dominator stand-in answer 31/10: Subliminal is for speaker support, not Emperor towers)
    return {'SkirmishChinaTankGeneral': None, 'SkirmishChinaInfantryGeneral': None}.get(o, P[o] + 'ChinaTankOverlord')


def _helix(o):
    return None if o == 'SkirmishChinaTankGeneral' else P[o] + 'ChinaVehicleHelix'


def _blocks(s):
    return [k for k in s.kids if k.name == 'OrCondition']


def _set_count(scb, block, suffix, n):
    """the block's own-count leaf of a template ending in suffix gets count n; returns True when it found one"""
    for i, l in enumerate(block.kids):
        nm, ps = rb.decode(scb, l)
        if nm == 'PLAYER_HAS_OBJECT_COMPARISON' and ps[3][1].endswith(suffix):
            assert ps[0] == ('SIDE', rb.SIDE_ME) and ps[1] == ('COMPARISON', rb.GE), ps
            block.kids[i] = rb.own_count(scb, rb.GE, n, ps[3][1])
            return True
    return False


def apply(scb, scope, p):
    for o in scope:
        get = lambda n: rb.find_script(scb, o, n)[2]
        s = get(NAPALM)
        for b in _blocks(s):
            hit = _set_count(scb, b, 'ChinaTankDragon', p['napalm_with_dragons']) or _set_count(scb, b, 'ChinaJetMIG', p['migs_min'])
            assert hit, (o, NAPALM)
            b.kids.append(rb.clock_at_least(scb, p['napalm_min_esc']))
        if ARMOR in HAS[o]:
            for b in _blocks(get(ARMOR)):
                assert _set_count(scb, b, 'ChinaJetMIG', p['migs_min']), (o, ARMOR)
        s = get(SUBLIM)
        (b0,) = _blocks(s)
        army = [(t, n) for t, n in ((_overlord(o), p['overlords_min']), (_helix(o), p['helix_min'])) if t]
        new = [mss.Node('OrCondition', b0.ver, kids=[mss.deep_copy(x) for x in b0.kids] + [rb.own_count(scb, rb.GE, n, t)]) for t, n in army]
        if not army:
            new = [b0]                                    # Tank: the stock gate (Speaker Tower and cash) stays
        i = s.kids.index(b0); s.kids[i:i + 1] = new
        if AUTO in HAS[o]:
            (b,) = _blocks(get(AUTO))
            b.kids.append(rb.credits_at_least(scb, p['autoloader_cash']))
        if URANIUM in HAS[o]:
            for b in _blocks(get(URANIUM)):
                assert _set_count(scb, b, 'ChinaTankOverlord', p['overlords_min']), (o, URANIUM)
            s = get(NUKETANKS)
            for b in _blocks(s):
                if p['nukeupg']:
                    keep = [l for l in b.kids if not (rb.decode(scb, l)[0] == 'PLAYER_HAS_OBJECT_COMPARISON' and
                                                       rb.decode(scb, l)[1][3][1].endswith('ChinaTankOverlord'))]
                    assert len(keep) == len(b.kids) - 1, (o, NUKETANKS)
                    b.kids = keep
                else:
                    assert _set_count(scb, b, 'ChinaTankOverlord', p['overlords_min']), (o, NUKETANKS)


def activation(obs, me, params=None):
    """triggered: own MiGs reached migs_min or Dragons reached napalm_with_dragons, with 60 s of game after; fired: UNOBSERVABLE - the gym
    stream carries no upgrades (a script trace or an obs upgrade field would show it), so it is reported False with that reason"""
    p = dict(DEFAULTS, **(params or {}))
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    fm = ob.first_frame(obs, me, ob.ends_with('ChinaJetMIG'), p['migs_min'])
    fd = ob.first_frame(obs, me, ob.ends_with('ChinaTankDragon'), p['napalm_with_dragons'])
    f = min(x for x in (fm, fd) if x is not None) if (fm is not None or fd is not None) else None
    end = ob.last_frame(obs)
    triggered = f is not None and end is not None and end >= f + 60 * hz
    return {'triggered': bool(triggered), 'fired': False,
            'detail': 'UNOBSERVABLE: upgrades are not in the obs stream; migs_f=%s dragons_f=%s' % (fm, fd)}
