# GeneralsX: rule bank DATA rule OPEN.USA_SC_AFTER_OILS: vanilla USA builds its Strategy Center
# once its oil income is up. Stock "USA Strategy Center - H" waits for a War Factory and _ESCALATION >= 30 (enemy tech adds to it), and a
# rule may neither retime nor read that clock. So a one-shot "GX Stock Strategy Center Off - H" switches the stock script off at the start
# (a DISABLE is the one reference the compiler allows to enemy-gated stock logic), and the one-shot "GX Strategy Center - H" builds it
# once a War Factory is complete and the player owns oils_for_sc oil derricks - or from _GX_CLOCK 60 (600 s), so it always comes.
# Own state only. humvee_start_oils is UNUSED: Humvee production is ARMY.USA_HUMVEE_MD's team edit.
RULE_ID = 'OPEN.USA_SC_AFTER_OILS'
PARAMS = ('oils_for_sc',)
UNUSED = {'humvee_start_oils': 'Humvee production belongs to the wave teams ARMY.USA_HUMVEE_MD edits; no oil gate is built here'}
US, SC, WF, DK = 'SkirmishAmerica', 'AmericaStrategyCenter', 'AmericaWarFactory', 'TechOilDerrick'
STOCK, OFF, NEW, GROUP, FLOOR = 'USA Strategy Center - H', 'GX Stock Strategy Center Off - H', 'GX Strategy Center - H', 'GX OPEN.USA_SC_AFTER_OILS', 60
CLAIMS = {'scripts': [[US, STOCK], [US, OFF], [US, NEW]], 'groups': [[US, GROUP]]}


def apply(scb, scope, p):
    if US not in scope:
        return
    wf = lambda: rb.cond(scb, 'BUILT_BY_PLAYER', ('OBJECT_TYPE', WF), ('SIDE', rb.SIDE_ME))
    kids = [rb.new_script(scb, OFF, [[rb.cond(scb, 'CONDITION_TRUE')]], [rb.act(scb, 'DISABLE_SCRIPT', ('SCRIPT', STOCK))], oneshot=True),
            rb.new_script(scb, NEW, [[wf(), rb.own_count(scb, rb.GE, p['oils_for_sc'], DK)], [wf(), rb.clock_at_least(scb, FLOOR)]],
                          [rb.act(scb, 'SKIRMISH_BUILD_BUILDING', ('OBJECT_TYPE', SC))], oneshot=True)]
    rb.owner_list(scb, US).kids.append(rb.new_group(GROUP, kids))


def _ref(obs, me, params):
    """(the frame the Strategy Center should follow, the Strategy Center frame): the oils_for_sc-th own oil derrick, or the 600 s floor
    when the player never owned that many (Codex rbd10-r1 P2); None for either that never came (the floor: a game shorter than 600 s)"""
    hz = ob.hz(obs)
    n = (params or {}).get('oils_for_sc', 2)
    fs = ob.first_frame(obs, me, ob.ends_with(SC))
    fn = ob.first_frame(obs, me, ob.ends_with(DK), n) if n > 0 else ob.first_frame(obs, me, ob.ends_with(WF), 1, complete=True)
    if fn is None or (fs is not None and fn > fs and fs >= FLOOR * 10 * hz):
        end = ob.last_frame(obs)
        fn = FLOOR * 10 * hz if end is not None and end >= FLOOR * 10 * hz else None
    return fn, fs


def _gap(obs, me, params=None):
    """seconds between that reference and the first own Strategy Center (absolute); None when either never came"""
    if not obs:
        return None
    fn, fs = _ref(obs, me, params)
    return None if fn is None or fs is None else abs(fs - fn) // ob.hz(obs)


def activation(obs, me, params=None):
    """triggered: the reference (oils_for_sc derricks, else the 600 s floor) and a Strategy Center both come; fired: the Strategy Center
    within 30 s after the reference"""
    if not obs:
        return {'triggered': False, 'fired': False, 'detail': 'no obs'}
    hz = ob.hz(obs)
    fn, fs = _ref(obs, me, params)
    triggered = fn is not None and fs is not None
    return {'triggered': bool(triggered), 'fired': bool(triggered and 0 <= fs - fn <= 30 * hz),
            'detail': 'reference_f=%s strategy_center_f=%s' % (fn, fs)}


CONTRAST = {'what': 'seconds between the oils_for_sc-th own oil derrick (or the 600 s floor) and the Strategy Center', 'direction': -1, 'min_delta': 20}


def measure(obs, me, params=None):
    return _gap(obs, me, params)
