# Copyright 2026 GeneralsX contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# rule bank DATA rule ARMY.CHINA_AIRFIELD_SCALE: China Infantry adds Air Fields as its
# MiG count grows. An Air Field parks 4 MiGs, and stock Infantry Hard builds one ("China Air Field - H"; the build list holds one, the
# RICH copies wait for 15000+ cash), so its MiG teams stall at 4. Here one-shot scripts "GX Air Field <k> - H", k = 2..airfield_max, each
# build Air Field k once the player owns k-1 Air Fields and mig_per_airfield * (k-1) MiGs, the way the stock RICH scripts place an extra
# one (AI_PLAYER_BUILD_SUPPLY_CENTER, minimum 1000 - ENGINE_READS, listed in the manifest). Own counts only. airfield_max = 1 is stock.
RULE_ID = 'ARMY.CHINA_AIRFIELD_SCALE'
PARAMS = ('airfield_max', 'mig_per_airfield')
IG, AF, MG = 'SkirmishChinaInfantryGeneral', 'Infa_ChinaAirfield', 'Infa_ChinaJetMIG'
NAME, GROUP, KMAX = 'GX Air Field %d - H', 'GX ARMY.CHINA_AIRFIELD_SCALE', 5
CLAIMS = {'scripts': [[IG, NAME % k] for k in range(2, KMAX + 1)], 'groups': [[IG, GROUP]]}
UNTIL = 400


def apply(scb, scope, p):
    if IG not in scope or p['airfield_max'] < 2:
        return
    me = ('SIDE', rb.SIDE_ME)
    kids = [rb.new_script(scb, NAME % k, [[rb.own_count(scb, rb.GE, p['mig_per_airfield'] * (k - 1), MG), rb.own_count(scb, rb.EQ, k - 1, AF)]],
                          [rb.act(scb, 'AI_PLAYER_BUILD_SUPPLY_CENTER', me, ('OBJECT_TYPE', AF), ('INT', 1000))], oneshot=True)
            for k in range(2, p['airfield_max'] + 1)]
    rb.owner_list(scb, IG).kids.append(rb.new_group(GROUP, kids))


def _airfields(obs, me):
    return ob.built_by(obs, me, ob.ends_with('ChinaAirfield'), UNTIL * ob.hz(obs)) if obs else None


def activation(obs, me, params=None):
    """triggered: 4 own MiGs at once by 400 s (the first Air Field full); fired: a second own Air Field by 400 s"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    f4 = ob.first_frame(obs, me, ob.ends_with('ChinaJetMIG'), 4)
    n = _airfields(obs, me)
    triggered = f4 is not None and f4 <= UNTIL * hz and n is not None
    return {'triggered': bool(triggered), 'fired': bool(triggered and n >= 2), 'detail': 'four_migs_f=%s airfields_by_400s=%s' % (f4, n)}


CONTRAST = {'what': 'own Air Fields built by 400 s', 'direction': 1, 'min_delta': 1}


def measure(obs, me, params=None):
    return _airfields(obs, me)
