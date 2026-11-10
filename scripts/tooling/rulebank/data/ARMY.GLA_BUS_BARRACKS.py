# GeneralsX: rule bank DATA rule ARMY.GLA_BUS_BARRACKS (C·rb-data-10): Stealth GLA runs a second Barracks so
# its Battle Buses and Jarmen Kell army fill faster. Stock Stealth Hard builds one ("GLA Barracks - H"; the build list holds one, the RICH
# copies wait for big cash). Here one-shot scripts "GX Barracks <k> - H", k = 2..barracks_n, each place Barracks k once the first one is
# complete, the player owns k-1 and holds 1500, the way the stock RICH scripts place an extra one (AI_PLAYER_BUILD_SUPPLY_CENTER, minimum
# 1000 - ENGINE_READS, listed in the manifest). Own state only. barracks_n = 1 is stock.
RULE_ID = 'ARMY.GLA_BUS_BARRACKS'
PARAMS = ('barracks_n',)
SG, BX = 'SkirmishGLAStealthGeneral', 'Slth_GLABarracks'
NAME, GROUP, KMAX, CASH = 'GX Barracks %d - H', 'GX ARMY.GLA_BUS_BARRACKS', 3, 1500
CLAIMS = {'scripts': [[SG, NAME % k] for k in range(2, KMAX + 1)], 'groups': [[SG, GROUP]]}
UNTIL = 400


def apply(scb, scope, p):
    if SG not in scope or p['barracks_n'] < 2:
        return
    me = ('SIDE', rb.SIDE_ME)
    kids = [rb.new_script(scb, NAME % k, [[rb.cond(scb, 'BUILT_BY_PLAYER', ('OBJECT_TYPE', BX), me), rb.own_count(scb, rb.EQ, k - 1, BX),
                                           rb.credits_at_least(scb, CASH)]],
                          [rb.act(scb, 'AI_PLAYER_BUILD_SUPPLY_CENTER', me, ('OBJECT_TYPE', BX), ('INT', 1000))], oneshot=True)
            for k in range(2, p['barracks_n'] + 1)]
    rb.owner_list(scb, SG).kids.append(rb.new_group(GROUP, kids))


def _barracks(obs, me):
    return ob.built_by(obs, me, ob.ends_with('GLABarracks'), UNTIL * ob.hz(obs)) if obs else None


def activation(obs, me, params=None):
    """triggered: own Barracks complete by 240 s and the game ran past 400 s; fired: a second own Barracks by 400 s"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    fb = ob.first_frame(obs, me, ob.ends_with('GLABarracks'), 1, complete=True)
    n = _barracks(obs, me)
    triggered = fb is not None and fb <= 240 * hz and n is not None
    return {'triggered': bool(triggered), 'fired': bool(triggered and n >= 2), 'detail': 'barracks_f=%s barracks_by_400s=%s' % (fb, n)}


CONTRAST = {'what': 'own Barracks built by 400 s', 'direction': 1, 'min_delta': 1}


def measure(obs, me, params=None):
    return _barracks(obs, me)
