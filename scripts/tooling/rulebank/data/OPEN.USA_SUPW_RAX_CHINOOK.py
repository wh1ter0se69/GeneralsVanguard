# GeneralsX: rule bank DATA rule OPEN.USA_SUPW_RAX_CHINOOK (C·rb-data-10): USA Superweapon opens on two
# Barracks. Stock Superweapon Hard builds one ("USA Barracks - H"; the build list holds one, the High Money copy waits for big cash). Here
# one-shot scripts "GX Barracks <k> - H", k = 2..rax_n, each place Barracks k once the first is complete, the player owns k-1 and holds 1500,
# the way the stock RICH scripts place an extra building (AI_PLAYER_BUILD_SUPPLY_CENTER, minimum 1000 - ENGINE_READS, listed in the
# manifest). Own state only; rax_n = 1 is stock. The Chinook and oil halves are other rules' (UNUSED says whose).
RULE_ID = 'OPEN.USA_SUPW_RAX_CHINOOK'
PARAMS = ('rax_n',)
UNUSED = {'chinooks_per_supply': 'Chinooks per supply centre is the pinned ENGINE rule USA2.CHINOOKS', 'oil_cap_t': 'oil capture timing is OIL.CAPTURE_TEAMS'}
SG, BX = 'SkirmishAmericaSuperWeaponGeneral', 'SupW_AmericaBarracks'
NAME, GROUP, KMAX, CASH = 'GX Barracks %d - H', 'GX OPEN.USA_SUPW_RAX_CHINOOK', 3, 1500
CLAIMS = {'scripts': [[SG, NAME % k] for k in range(2, KMAX + 1)], 'groups': [[SG, GROUP]]}
UNTIL = 400


def apply(scb, scope, p):
    if SG not in scope or p['rax_n'] < 2:
        return
    me = ('SIDE', rb.SIDE_ME)
    kids = [rb.new_script(scb, NAME % k, [[rb.cond(scb, 'BUILT_BY_PLAYER', ('OBJECT_TYPE', BX), me), rb.own_count(scb, rb.EQ, k - 1, BX),
                                           rb.credits_at_least(scb, CASH)]],
                          [rb.act(scb, 'AI_PLAYER_BUILD_SUPPLY_CENTER', me, ('OBJECT_TYPE', BX), ('INT', 1000))], oneshot=True)
            for k in range(2, p['rax_n'] + 1)]
    rb.owner_list(scb, SG).kids.append(rb.new_group(GROUP, kids))


def _barracks(obs, me):
    return ob.built_by(obs, me, ob.ends_with('AmericaBarracks'), UNTIL * ob.hz(obs)) if obs else None


def activation(obs, me, params=None):
    """triggered: own Barracks complete by 240 s and the game ran past 400 s; fired: a second own Barracks by 400 s"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    fb = ob.first_frame(obs, me, ob.ends_with('AmericaBarracks'), 1, complete=True)
    n = _barracks(obs, me)
    triggered = fb is not None and fb <= 240 * hz and n is not None
    return {'triggered': bool(triggered), 'fired': bool(triggered and n >= 2), 'detail': 'barracks_f=%s barracks_by_400s=%s' % (fb, n)}


CONTRAST = {'what': 'own Barracks built by 400 s', 'direction': 1, 'min_delta': 1}


def measure(obs, me, params=None):
    return _barracks(obs, me)
