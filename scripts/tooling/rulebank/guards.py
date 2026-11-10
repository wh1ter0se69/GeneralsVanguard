# GeneralsX: rule bank, DATA half: the refusals.
# Run after EACH module on the scb it left behind, against the scb it was given; every refusal names the module, the script and why.
#  scope       only the entry's generals' lists and the teams they own move; nothing outside the script lists and ScriptTeams
#  difficulty  a changed stock script must be a Hard script (a 011 one is listed as normal_also); a new script is Hard-only; no stock
#              script loses its name, difficulty or subroutine bits; nothing is deleted; a name twice in its parent is not editable
#  gifts       no script that reaches PLAYER_GIVE_MONEY (directly or through any SCRIPT / SCRIPT_SUBROUTINE parameter, or a group
#              holding one, transitively) is changed, and no new or changed script or team field names one (Hard's gifts stay frozen)
#  priority    attack-priority set names are GLOBAL: a stock set is never defined further, its defining scripts are frozen like the
#              gifts (their conditions decide when the shared table exists); a new set name lives in one general
#  fog         an ADDED condition reads only this player's own state (allow-list); every SIDE argument is <This Player>; the cached
#              tech-building test only of MY area and only where its first evaluation is the first frame (map layout)
#  names       script and group names share the engine's lookups (CALL_SUBROUTINE finds a group first, ENABLE_SCRIPT both): no new
#              name equals another of its list; no two names of one kind alias once a start index is appended, across every
#              general and every tier of the compile (players of all of them share one match)
#  escalation  no added, removed or changed leaf names _ESCALATION (stock adds enemy tech to it); new time gates use _GX_CLOCK
#  taint       a stock FLAG / COUNTER / timer / team state an added condition reads is traced to its writers (transitively, through
#              ENABLE/CALL and team hooks behind a tainted production condition); a tainted one is accepted only as the stock gate itself,
#              copied unchanged with that script's own actions; rechecked once every module has written; the manifest records every read
#  actions     no money action, no action that picks its target by enemy value, no SIDE other than <This Player>, no write of _GX_CLOCK;
#              a stock script that writes tainted state is never changed, one gated by another player's state gets no new effect;
#              no team gains an enemy-sighted / all-clear hook (stock uses none: they fire on undetected stealthed enemies)
#  scope of a STOCK dependency: a rule may KEEP a stock gate's enemy input - the copied stock gate, a changed
#              own-state threshold beside it, a stock action whose engine code searches the map unfiltered (ENGINE_READS: listed in
#              the manifest for rb-sentient, whose job is making stock Hard's omniscience fair) - and may never ADD one
#  leaves      every added leaf has the stock signature and no cached fields; no qualified name aliases another once an index is appended
#  claims      what the module touched, and every stock script or group its new leaves and team fields name, is inside its CLAIMS
import collections, struct

SIDE_ME = '<This Player>'
FAIR = {'CONDITION_TRUE', 'CONDITION_FALSE', 'COUNTER', 'FLAG', 'TIMER_EXPIRED', 'PLAYER_HAS_CREDITS', 'PLAYER_HAS_OBJECT_COMPARISON',
        'PLAYER_HAS_N_OR_FEWER_BUILDINGS', 'PLAYER_HAS_N_OR_FEWER_FACTION_BUILDINGS', 'PLAYER_HAS_POWER', 'PLAYER_HAS_NO_POWER',
        'PLAYER_POWER_COMPARE_PERCENT', 'PLAYER_EXCESS_POWER_COMPARE_VALUE', 'PLAYER_ACQUIRED_SCIENCE', 'PLAYER_HAS_SCIENCEPURCHASEPOINTS',
        'PLAYER_CAN_PURCHASE_SCIENCE', 'PLAYER_BUILT_UPGRADE', 'SKIRMISH_SPECIAL_POWER_READY', 'TEAM_HAS_UNITS', 'TEAM_DESTROYED',
        'BUILT_BY_PLAYER',          # own completed objects when its SIDE is <This Player> (another side: reads_other refuses it)
        # a tech building within distance of an area that is neither mine nor an ally's (ScriptConditions.cpp:2264-2297): the result is
        # cached at its first evaluation - a map-layout query (derrick positions are public) only of MY area, evaluated on the first
        # frame; check_module enforces both (tech_near_ok)
        'SKIRMISH_TECH_BUILDING_WITHIN_DISTANCE',
        'TEAM_CREATED', 'TEAM_STATE_IS', 'TEAM_STATE_IS_NOT',
        # rb-data-12: the fair enemy-faction sensor (rb-sense-1) - only as <This Player> asking about <This Player's Enemy> for one of
        # the factions it answers (reads_other refuses every other form); a rule that emits it must declare SENSE.ENEMY_FACTION
        'AI_PLAYER_GX_SENSE',
        # rb-sense-3: the fair twins of PLAYER_HAS_OBJECT_COMPARISON and SKIRMISH_VALUE_IN_AREA (rb-sense-2) - the same one form, and seen_why's rules
        'AI_PLAYER_GX_SEEN_COUNT', 'AI_PLAYER_GX_SEEN_VALUE'}
SENSE = 'AI_PLAYER_GX_SENSE'
SEEN = {'AI_PLAYER_GX_SEEN_COUNT', 'AI_PLAYER_GX_SEEN_VALUE'}
# conditions that read another player (or hidden state) whatever their SIDE argument says - the reason goes into the refusal
OTHER = {'SKIRMISH_PLAYER_FACTION': 'reads the RESOLVED side: it reveals an unscouted Random enemy (plan 1.3)',
         'NAMED_DISCOVERED': "exempts a disguised object and reads its real identity (plan 1.3)",
         'TEAM_DISCOVERED': "exempts a disguised object and reads its real identity (plan 1.3)",
         'SKIRMISH_PLAYER_HAS_DISCOVERED_PLAYER': 'tests shroud only: an undetected stealthed unit counts as discovered (plan 1.3)',
         'SKIRMISH_VALUE_IN_AREA': 'sums object value in an area through fog (plan 1.3)',
         'SKIRMISH_PLAYER_HAS_UNITS_IN_AREA': 'counts a player\'s units in an area through fog (plan 1.3)',
         'SUPPLY_SOURCE_SAFE': 'tests for enemies near a supply source through fog',
         'SUPPLY_SOURCE_ATTACKED': 'reacts to enemy attacks on a supply source',
         'ENEMY_SIGHTED': 'reads enemy positions', 'TYPE_SIGHTED': 'reads enemy positions',
         'PLAYER_HAS_COMPARISON_UNIT_TYPE_IN_TRIGGER_AREA': 'counts units in an area through fog',
         'PLAYER_HAS_COMPARISON_UNIT_KIND_IN_TRIGGER_AREA': 'counts units in an area through fog',
         'SKIRMISH_PLAYER_HAS_BEEN_ATTACKED_BY_PLAYER': 'reads another player', 'TEAM_ATTACKED_BY_PLAYER': 'reads another player',
         'NAMED_ATTACKED_BY_PLAYER': 'reads another player', 'SKIRMISH_UNOWNED_FACTION_UNIT_EXISTS': 'reads map objects through fog',
         'SKIRMISH_SUPPLIES_VALUE_WITHIN_DISTANCE': 'reads supply boxes left through fog',
         'PLAYER_DESTROYED_N_BUILDINGS_PLAYER': 'reads another player', 'MULTIPLAYER_PLAYER_DEFEAT': 'reads another player'}
PRIORITY_DEFINE = {'SET_ATTACK_PRIORITY_THING', 'SET_ATTACK_PRIORITY_KIND_OF', 'SET_DEFAULT_ATTACK_PRIORITY'}
SWITCH = {'ENABLE_SCRIPT', 'DISABLE_SCRIPT'}       # the actions that change another script's (or group's) state for the whole list
STATE_TYPES = ('FLAG', 'COUNTER', 'TEAM_STATE')   # flags, counters (timers share them) and team states carry decisions between scripts
MONEY = {'PLAYER_GIVE_MONEY', 'PLAYER_SET_MONEY', 'WAREHOUSE_SET_VALUE'}      # every action that makes money
# The line between the two lists (README "What a DATA rule may KEEP"): an action whose OWN code picks a specific object or place by
# enemy value or by proximity among objects a human may not see is HIDDEN - never ADDED by a DATA rule (1,
# rbd-r3-lensB 3); an order to my own units or buildings whose engine side (unit AI, placement) is fog-blind is ENGINE_READS - the stock
# AI's own omniscience (AIUpdate adds UNFOGGED only for humans, plan 4.3), allowed and listed for rb-sentient's audit.
HIDDEN = {'SKIRMISH_ATTACK_NEAREST_GROUP_WITH_VALUE': 'targets the most valuable enemy group through fog',
          'TEAM_ALL_USE_COMMANDBUTTON_ON_NEAREST_ENEMY_UNIT': 'targets the nearest enemy unit through fog',
          'TEAM_ALL_USE_COMMANDBUTTON_ON_NEAREST_ENEMY_BUILDING': 'targets the nearest enemy building through fog',
          'TEAM_ALL_USE_COMMANDBUTTON_ON_NEAREST_ENEMY_BUILDING_CLASS': 'targets the nearest enemy building through fog',
          'SKIRMISH_FIRE_SPECIAL_POWER_AT_MOST_COST': 'aims at the most valuable enemy area (AIPlayer::computeSuperweaponTarget scans'
                                                      ' every enemy object, no visibility filter)',
          'SKIRMISH_PERFORM_COMMANDBUTTON_ON_MOST_VALUABLE_OBJECT': 'targets enemies in descending value order through fog',
          'TEAM_ALL_USE_COMMANDBUTTON_ON_NEAREST_OBJECTTYPE': 'targets the nearest enemy or neutral object of a type map-wide through fog',
          'TEAM_MOVE_TOWARDS_NEAREST_OBJECT_TYPE': 'heads for the nearest object of a type, any owner, through fog',
          'TEAM_GARRISON_NEAREST_BUILDING': 'picks the nearest building no enemy garrisons, seen or not',
          'TEAM_CAPTURE_NEAREST_UNOWNED_FACTION_UNIT': 'picks the nearest unowned unit through fog'}
ENGINE_READS = {'AI_PLAYER_BUILD_SUPPLY_CENTER': 'AIPlayer::findSupplyCenter rejects piles near enemy structures (no fog filter)',
                'TEAM_HUNT': 'hunts map-wide (the unit AI ignores fog)', 'TEAM_HUNT_WITH_COMMAND_BUTTON': 'hunts map-wide (the unit AI ignores fog)',
                'AI_PLAYER_BUILD_TYPE_NEAREST_TEAM': 'places by my team\'s position (placement checks are fog-blind)'}
# The ACTION allow-list (a deny-list of fog-reading actions never closes): an added action is one of these, each
# an order to my own units, buildings, scripts, state or a NEW priority table, its engine side read for hidden-state use. Script
# targets (ENABLE / DISABLE / CALL / sequential) are checked by the reference rule; ENGINE_READS entries are listed in the manifest.
ACTIONS_OK = {'NO_OP': 'nothing', 'SET_FLAG': 'my own new flag', 'SET_COUNTER': 'my own new counter',
              'INCREMENT_COUNTER': 'my own new counter', 'DECREMENT_COUNTER': 'my own new counter',
              'ENABLE_SCRIPT': 'a claimed script (reference rule)', 'DISABLE_SCRIPT': 'a claimed script (reference rule)',
              'CALL_SUBROUTINE': 'a clean or own subroutine (reference rule)',
              'TEAM_EXECUTE_SEQUENTIAL_SCRIPT': 'a clean or own script as my team\'s orders (reference rule)',
              'SET_ATTACK_PRIORITY_THING': 'a NEW, claimed, per-general table', 'SET_ATTACK_PRIORITY_KIND_OF': 'a NEW, claimed table',
              'SET_DEFAULT_ATTACK_PRIORITY': 'a NEW, claimed table', 'TEAM_APPLY_ATTACK_PRIORITY_SET': 'a table onto my team (reads it only)',
              'SKIRMISH_BUILD_BUILDING': 'AIPlayer::buildSpecificBuilding - my build list\'s own spot',
              'AI_PLAYER_BUILD_UPGRADE': 'my own upgrade', 'TEAM_STOP': 'my team stops', 'TEAM_GUARD_AREA': 'my team guards a named area',
              'TEAM_USE_COMMANDBUTTON_ABILITY': 'my team presses a button that takes no target'}
ACTIONS_OK.update(ENGINE_READS)
# actions that write GLOBAL, unqualified state other personalities and stock tables read (setPriorityThing expands a list name first,
# ScriptEngine.cpp:5966; enemy-count gates use the lists) - never added
GLOBAL_WRITE = {'OBJECTLIST_ADDOBJECTTYPE', 'OBJECTLIST_REMOVEOBJECTTYPE'}
RESERVED = ('GX Rule ', 'GX Clock', 'GXT.')                  # names only the compiler writes
QUALIFIED = ('COUNTER', 'FLAG', 'SCRIPT', 'SCRIPT_SUBROUTINE', 'TEAM')   # Parameter::qualify appends the start index to these
TEAM_SCRIPT_FIELDS = lambda k: k == 'teamProductionCondition' or (k.startswith('team') and 'Script' in k)
# team hooks that fire on enemies: Team.cpp:1879's search checks relationship, life and map status, not stealth detection
ENEMY_EVENT_FIELDS = {'teamEnemySightedScript': 'enemy-sighted', 'teamAllClearScript': 'all-clear'}
MY_AREAS = ('[Skirmish]MyInnerPerimeter', '[Skirmish]MyOuterPerimeter')      # Scripts.h:63-64, qualified to MY start index
ENEMY_AREAS = ('[Skirmish]EnemyInnerPerimeter', '[Skirmish]EnemyOuterPerimeter')   # the CURRENT enemy's start (ScriptEngine.cpp:6008)
# the engine's script pools (ScriptEngine.h:221: MAX_COUNTERS = MAX_FLAGS = MAX_ATTACK_PRIORITIES = 256; counter / flag slot 0 means
# "unassigned", table 0 is the default): 255 usable each. Counters and flags are per player (qualified names), tables are global.
POOL, POOL_PLAYERS = 255, 8          # POOL_PLAYERS: the most start positions a retail map holds
LOBBY_PLAYERS = 14                   # MAX_SLOTS (GameNetwork/NetworkDefs.h:81): what a SHIPPED default must fit
TECH_NEAR = 'SKIRMISH_TECH_BUILDING_WITHIN_DISTANCE'
ESCALATION = '_ESCALATION'
GX_CLOCK = '_GX_CLOCK'


class Index:
    """one scb's view per general: scripts keyed (group name or None, script name, occurrence), groups, teams"""

    def __init__(s, rb, scb, owners):
        s.rb, s.scb = rb, scb
        s.scripts, s.groups, s.order, s.parent = {}, {}, {}, {}
        for o in owners:
            occ, gocc, top = collections.Counter(), collections.Counter(), []
            for k in rb.owner_list(scb, o).kids:
                if k.name == 'ScriptGroup':
                    gn = rb.node_name(k)
                    gocc[gn] += 1
                    gk = gkey(gn, gocc[gn] - 1)          # a second group of one name is its own parent
                    s.groups[(o, gn, gocc[gn] - 1)] = k
                    top.append(('G', gn, gocc[gn] - 1))
                    inner = []
                    for x in k.kids:
                        if x.name == 'Script':
                            key = (gk, rb.node_name(x))
                            occ[key] += 1
                            s.scripts[(o,) + key + (occ[key] - 1,)] = x
                            s.parent[(o,) + key + (occ[key] - 1,)] = k
                            inner.append(key + (occ[key] - 1,))
                    s.order[(o, gk)] = inner
                elif k.name == 'Script':
                    key = (None, rb.node_name(k))
                    occ[key] += 1
                    s.scripts[(o,) + key + (occ[key] - 1,)] = k
                    top.append(('S',) + key[1:] + (occ[key] - 1,))
            s.order[(o, None)] = top
        s.team_list = rb.teams_ro(scb)
        s.teams = {d['teamName']: d for d in s.team_list}

    def canon(s, node):
        return s.rb.canon(s.scb, node)


def gkey(name, occ):
    return name if occ == 0 else '%s#%d' % (name, occ)


def leaf_list(rb, scb, script, kinds=('Condition',)):
    return [l for l in rb.leaves(script) if l.name in kinds]


def leaf_multiset(rb, scb, script, kinds):
    return collections.Counter(rb.canon(scb, l) for l in leaf_list(rb, scb, script, kinds))


def refs_of(rb, scb, script):
    """script names this script names (any SCRIPT / SCRIPT_SUBROUTINE parameter)"""
    out = set()
    for l in rb.leaves(script):
        for t, v in rb.decode(scb, l)[1]:
            if t in ('SCRIPT', 'SCRIPT_SUBROUTINE'):
                out.add(v)
    return out


def walk(node):
    yield node
    for k in node.kids or ():
        yield from walk(k)


def refs_acts(rb, scb, script):
    """{(leaf name, script name)} - every SCRIPT / SCRIPT_SUBROUTINE parameter with the call or condition that names it"""
    out = set()
    for l in rb.leaves(script):
        nm, ps = rb.decode(scb, l)
        out |= {(nm, v) for t, v in ps if t in ('SCRIPT', 'SCRIPT_SUBROUTINE')}
    return out


def enemy_reach(rb, scb, owner, taint, writes=False):
    """{name: why} of the scripts and groups of a list whose RUNNING can depend on another player: a condition that reads one (or a
    tainted state), a HIDDEN action, or a call / enable / sequential reference or group membership leading to one (a DISABLE does
    not run its target). writes=True: also every script that writes a stock flag or counter - a new reference must not run stock
    state writers either (a sequential script runs the actions without the gate)"""
    scripts = [s for _g, _k, s in rb.scripts_of(scb, owner)]
    reach = {}
    for s in scripts:
        n = rb.node_name(s)
        for l in rb.leaves(s):
            nm, ps = rb.decode(scb, l)
            if l.name == 'Condition':
                why = reads_other(rb, scb, l) or next(('reads %s (%s)' % (v, taint[v]) for t, v in ps if t in STATE_TYPES and v in taint), None)
            else:
                why = ('runs %s, which %s (hidden information)' % (nm, HIDDEN[nm])) if nm in HIDDEN else None
                if why is None and writes:
                    why = next(('writes the stock state %r (%s)' % (v, nm) for t, v in ps if t in ('COUNTER', 'FLAG')), None)
            if why and n not in reach:
                reach[n] = why
    runs = {rb.node_name(s): {x for a, x in refs_acts(rb, scb, s) if a != 'DISABLE_SCRIPT'} for s in scripts}
    groups = groups_of(rb, scb, owner)
    grew = True
    while grew:
        grew = False
        for n, r in runs.items():
            hit = sorted(x for x in r if x in reach)
            if n not in reach and hit:
                reach[n] = 'runs %r, which %s' % (hit[0], reach[hit[0]])
                grew = True
        for g, members in groups.items():
            hit = sorted(m for m in members if m in reach)
            if g not in reach and hit:
                reach[g] = 'holds %r, which %s' % (hit[0], reach[hit[0]])
                grew = True
    return reach


def frozen_inputs(rb, scb, owner, frozen):
    """{writer: why} - every script of a list that writes a flag / counter / team state a frozen script (a gift, a table definer)
    reads, transitively through those writers' own reads: switching one off or retiming it moves the gift"""
    info = []
    for _g, _k, s in rb.scripts_of(scb, owner):
        reads = {v for l in leaf_list(rb, scb, s) for t, v in rb.decode(scb, l)[1] if t in STATE_TYPES}
        writes = {v for l in leaf_list(rb, scb, s, ('ScriptAction', 'ScriptActionFalse')) for t, v in rb.decode(scb, l)[1] if t in STATE_TYPES}
        info.append((rb.node_name(s), reads, writes))
    need = {}
    for n, reads, _w in info:
        if n in frozen:
            for v in sorted(reads):
                need.setdefault(v, 'the frozen %r waits on it' % n)
    out, grew = {}, True
    while grew:
        grew = False
        for n, reads, writes in info:
            hit = sorted(writes & set(need))
            if hit and n not in out and n not in frozen:
                out[n] = 'writes %r - %s' % (hit[0], need[hit[0]])
                for v in sorted(reads):
                    need.setdefault(v, '%r writes what a frozen script waits on, and reads it' % n)
                grew = True
    return out


def groups_of(rb, scb, owner):
    """{group name: [member script names]} of a list (a CALL_SUBROUTINE / ENABLE_SCRIPT of the name reaches every member)"""
    out = collections.defaultdict(list)
    for k in rb.owner_list(scb, owner).kids:
        if k.name == 'ScriptGroup':
            out[rb.node_name(k)] += [rb.node_name(x) for x in k.kids if x.name == 'Script']
    return out


def gift_reach(rb, scb, owner):
    """names of the scripts (and groups) in a list that reach PLAYER_GIVE_MONEY, with one path each (for the refusal text)"""
    scripts = [s for _g, _k, s in rb.scripts_of(scb, owner)]
    reach = {}
    for s in scripts:
        if any(rb.decode(scb, l)[0] in MONEY for l in rb.leaves(s)):
            reach[rb.node_name(s)] = 'gives money itself'
    refs = {rb.node_name(s): refs_of(rb, scb, s) for s in scripts}
    groups = groups_of(rb, scb, owner)
    # a team whose hook gives money (a stock Hacker team pays $750 on create) makes its production condition a gift: when it opens,
    # a team - and the gift - is made
    made = [(rb.team_get(d, 'teamProductionCondition'), d['teamName'],
             [(k, v) for k, _t, v in d['_e'] if TEAM_SCRIPT_FIELDS(k) and k != 'teamProductionCondition' and v])
            for d in rb.teams_ro(scb) if rb.team_get(d, 'teamOwner') == owner and rb.team_get(d, 'teamProductionCondition')]
    grew = True
    while grew:
        grew = False
        for prod, tname, hooks in made:
            hit = sorted('%s %r' % (k, v) for k, v in hooks if v in reach)
            if prod not in reach and hit:
                reach[prod] = 'makes team %r, whose %s gives money' % (tname, hit[0])
                grew = True
        for n, r in refs.items():
            if n not in reach:
                hit = sorted(x for x in r if x in reach)
                if hit:
                    reach[n] = 'names %r' % hit[0]
                    grew = True
        for g, members in groups.items():                 # a group holding a gift script is a gift
            hit = sorted(m for m in members if m in reach)
            if g not in reach and hit:
                reach[g] = 'the group holds %r' % hit[0]
                grew = True
    return reach


def reads_other(rb, scb, leaf):
    """None when a condition leaf reads only this player's own state, else the reason"""
    name, ps = rb.decode(scb, leaf)
    if name in OTHER:
        return '%s %s' % (name, OTHER[name])
    if name == SENSE:                   # the one exception to "every SIDE is mine": the sensor's asked seat is my enemy (rb-data-12)
        sides = [v for t, v in ps if t == 'SIDE']
        bad_f = [v for t, v in ps if t == 'FACTION_NAME' and v not in rb.SENSE_FACTIONS]
        if sides != [SIDE_ME, rb.SIDE_ENEMY] or bad_f:
            return '%s%r reads another player: only %s asking about %s for one of the sensor\'s factions (%s) is fair' % (
                name, tuple(v for _t, v in ps), SIDE_ME, rb.SIDE_ENEMY, ', '.join(rb.SENSE_FACTIONS))
        return None                     # its types are checked by the signature rule
    if name in SEEN:                    # the same exception: own sight only, so only my own enemy is asked about (rb-sense-3)
        sides = [v for t, v in ps if t == 'SIDE']
        if sides != [SIDE_ME, rb.SIDE_ENEMY]:
            return '%s%r reads another player: only %s asking about %s is fair' % (name, tuple(sides), SIDE_ME, rb.SIDE_ENEMY)
        return None                     # seen_why checks the rest
    for t, v in ps:
        if t == 'SIDE' and v != SIDE_ME:
            return '%s reads another player (SIDE %r)' % (name, v)
    return None


def seen_lists(rb, scb):
    """{list name: {(owner, builder script): frozenset of templates}} of every seen list script, whatever tier mark its name carries: a top-level
    script of a general's list named SEEN_LIST_SCRIPT..., its true branch only (a false branch never runs on CONDITION_TRUE)"""
    out = collections.defaultdict(dict)
    for o in rb.GENERALS:
        for s in rb.owner_list(scb, o).kids:
            n = rb.node_name(s) if s.name == 'Script' else ''
            if rb.SEEN_LIST_SCRIPT in n:
                got = collections.defaultdict(set)
                for l in leaf_list(rb, scb, s, ('ScriptAction',)):
                    a, ps = rb.decode(scb, l)
                    if a == 'OBJECTLIST_ADDOBJECTTYPE':
                        got[ps[0][1]].add(ps[1][1])
                for k, v in got.items():
                    out[k][(o, n[n.index(rb.SEEN_LIST_SCRIPT):])] = frozenset(v)
    return out


def seen_true_at_zero(rb, op, n):
    """a count compared so is TRUE with nothing seen - and also before its list exists, when the engine counts the list's NAME as a template"""
    return {rb.LT: n > 0, rb.LE: n >= 0, rb.EQ: n == 0, rb.GE: n <= 0, rb.GT: False, rb.NE: n != 0}[op]


def seen_why(rb, scb, owner, script, leaf):
    """None when a SEEN leaf that passed reads_other is one a rule may add, else the reason (a mistyped leaf: the signature rule's, after this)"""
    name, ps = rb.decode(scb, leaf)
    if [t for t, _v in ps] != rb.engine_types('Condition', name):
        return None
    _s, _a, op, n, what = [v for _t, v in ps]
    if op not in (rb.LT, rb.LE, rb.EQ, rb.GE, rb.GT, rb.NE):
        return '%s: comparison %r is none of Parameter::COMPARISON (gxrb::compareSeen answers FALSE for ever)' % (name, op)
    if n < 0:
        return '%s: threshold %r: a count or a value is never negative, so the answer is decided before anything is seen' % (name, n)
    delay = rb.mss.script_flags(script)[1]
    if delay < rb.SEEN_MIN_DELAY:
        return '%s on a script delay of %d s: each ask walks the partition, so its script needs a delay of at least %d s' % (name, delay, rb.SEEN_MIN_DELAY)
    if name == 'AI_PLAYER_GX_SEEN_VALUE':
        if what not in rb.SEEN_AREAS:
            return '%s area %r: only my own perimeter (%s)' % (name, what, ', '.join(rb.SEEN_AREAS))
        return None
    is_list = what.startswith(rb.SEEN_LIST) or what in rb.STOCK_OBJECT_LISTS
    if is_list and seen_true_at_zero(rb, op, n):
        return '%s of the list %r is TRUE at a count of 0, so also before the list exists (a first-pass one-shot would latch it): ask >= or > and latch that' % (name, what)
    if what.startswith(rb.SEEN_LIST) and not any(k[0] == owner for k in seen_lists(rb, scb).get(what, {})):
        return '%s reads the list %r, which no script of this general builds: it would count the name as a template' % (name, what)
    return None


def seen_list_why(rb, scb, owner, script, name, ps):
    """None when a write to an object-type list is one a rule may add - only the one form scbx.seen_list_script builds - else the reason"""
    if name != 'OBJECTLIST_ADDOBJECTTYPE' or not ps[0][1].startswith(rb.SEEN_LIST):
        return 'object-type lists are GLOBAL and unqualified - stock priority setters expand a list name and stock counts read them, for every personality;' \
            ' a rule only ADDs to its own list, named %s...' % rb.SEEN_LIST
    n = rb.node_name(script)
    if not n.startswith(rb.SEEN_LIST_SCRIPT) or not any(k is script for k in rb.owner_list(scb, owner).kids):
        return 'a seen list is built by a script named %s... at the top level of the list: the engine runs those before its groups, and an ask before' \
            ' the list exists counts its name as a template' % rb.SEEN_LIST_SCRIPT
    fl = rb.mss.script_flags(script)[0]
    conds = [rb.decode(scb, l)[0] for l in leaf_list(rb, scb, script)]
    if conds != ['CONDITION_TRUE'] or not fl[0] or not fl[1] or fl[5] or leaf_list(rb, scb, script, ('ScriptActionFalse',)):
        return 'a seen list is built at the first pass: one active, one-shot, non-subroutine script on CONDITION_TRUE alone, no false branch (got %r)' % conds
    return None


def seen_final(rb, scbs, Refused):
    """over the whole compile, every module and tier view: each list has ONE contents - lists are GLOBAL, so a second definition would
    be counted by every reader - and no SEEN script is a team's generic hook: Team::updateGenericScripts evaluates a hook every frame, delay or not"""
    built = collections.defaultdict(set)
    where = collections.defaultdict(list)
    for scb in scbs:
        for k, by in seen_lists(rb, scb).items():
            for b, v in by.items():
                built[k].add(v)
                where[k].append(b)
        hooks = {(rb.team_get(t, 'teamOwner'), rb.team_get(t, 'teamGenericScriptHook%d' % i)): rb.team_get(t, 'teamName')
                 for t in rb.teams_ro(scb) for i in range(4) if rb.team_get(t, 'teamGenericScriptHook%d' % i)}
        for o in rb.GENERALS:
            for _g, _k, s in rb.scripts_of(scb, o):
                if (o, rb.node_name(s)) in hooks and any(rb.decode(scb, l)[0] in SEEN for l in leaf_list(rb, scb, s)):
                    raise Refused('%s/%r: a SEEN script as the generic hook of team %r: Team::updateGenericScripts evaluates a hook every frame whatever'
                                  ' its delay, and each ask walks the partition' % (o, rb.node_name(s), hooks[(o, rb.node_name(s))]))
    for k, v in built.items():
        if len(v) > 1:
            raise Refused('the list %r has other contents in another builder (%s): lists are GLOBAL, so every reader would count the union' % (k, sorted(set(where[k]))))


def state_taint(rb, scb, owner):
    """({state name: why} for every FLAG / COUNTER of a list whose value can depend on another player, {state: writer scripts}).
    Fixpoint: a script is tainted when one of its conditions reads another player or a tainted state, or when a tainted script names
    it (ENABLE_SCRIPT, CALL_SUBROUTINE, ...); every state a tainted script writes is tainted. So _ESCALATION (an enemy Tech Center
    adds to it) taints _ATTACK_WAVE (its writers gate on _ESCALATION)."""
    info = []
    for _g, _k, s in rb.scripts_of(scb, owner):
        conds = leaf_list(rb, scb, s)
        acts = leaf_list(rb, scb, s, ('ScriptAction', 'ScriptActionFalse'))
        other = [reads_other(rb, scb, l) for l in conds]
        reads = {v for l in conds for t, v in rb.decode(scb, l)[1] if t in STATE_TYPES}
        writes = {v for l in acts for t, v in rb.decode(scb, l)[1] if t in STATE_TYPES}
        names = {v for l in acts for t, v in rb.decode(scb, l)[1] if t in ('SCRIPT', 'SCRIPT_SUBROUTINE')}
        info.append((rb.node_name(s), next((x for x in other if x), None), reads, writes, names))
    writers = collections.defaultdict(set)
    for n, _o, _r, w, _x in info:
        for v in w:
            writers[v].add(n)
    # a team's production condition gates its creation, so every other script the team names runs behind that gate;
    # an enemy-sighted / all-clear hook runs on an enemy event itself
    links, taint, bad_scripts = [], {}, {}
    for d in rb.teams_ro(scb):
        if rb.team_get(d, 'teamOwner') == owner:
            prod = rb.team_get(d, 'teamProductionCondition')
            others = [v for k, _t, v in d['_e'] if TEAM_SCRIPT_FIELDS(k) and k != 'teamProductionCondition' and v]
            if prod:
                links.append((prod, others, d['teamName']))
            for k, what in ENEMY_EVENT_FIELDS.items():
                v = rb.team_get(d, k)
                if v and v not in bad_scripts:
                    bad_scripts[v] = '%r is team %r\'s %s hook (no stealth-detection filter)' % (v, d['teamName'], what)
    groups = groups_of(rb, scb, owner)
    grew = True
    while grew:
        grew = False
        for g, members in groups.items():                 # a tainted group name runs every member
            if g in bad_scripts:
                for m in members:
                    if m not in bad_scripts:
                        bad_scripts[m] = '%r is in the group %r, which %s' % (m, g, bad_scripts[g])
                        grew = True
        for prod, others, tname in links:
            if prod in bad_scripts:
                for x in others:
                    if x not in bad_scripts:
                        bad_scripts[x] = '%r runs when team %r is made, behind %s' % (x, tname, bad_scripts[prod])
                        grew = True
        for n, direct, reads, writes, names in info:
            if n not in bad_scripts:
                hit = sorted(reads & set(taint))
                why = ('%r: %s' % (n, direct)) if direct else ('%r reads %s (%s)' % (n, hit[0], taint[hit[0]])) if hit else None
                if why:
                    bad_scripts[n] = why
                    grew = True
            if n in bad_scripts:
                for v in writes:
                    if v not in taint:
                        taint[v] = bad_scripts[n]
                        grew = True
                for x in names:
                    if x not in bad_scripts:
                        bad_scripts[x] = '%r is run by %s' % (x, bad_scripts[n])
                        grew = True
    return taint, writers, bad_scripts       # bad_scripts: every script whose RUNNING an enemy read can trigger


def enemy_blocks(rb, scb, script, taint):
    """per OR block, the Counter of its conditions that read another player or a tainted state"""
    out = []
    for b in script.kids:
        if b.name == 'OrCondition':
            out.append(collections.Counter(rb.canon(scb, l) for l in rb.leaves(b) if l.name == 'Condition' and
                                           (reads_other(rb, scb, l) or any(t in STATE_TYPES and v in taint for t, v in rb.decode(scb, l)[1]))))
    return out


def gate_kept(new, stock):
    """every new OR block holds all the enemy-reading conditions of SOME stock block: the new condition implies the stock enemy
    gate (a stock block with none - an own-only path - already let the script run without enemy input)"""
    return all(any(not (s - b) for s in stock) for b in new)


def tech_near_ok(rb, A, scb, key, script, leaf):
    """None when an added SKIRMISH_TECH_BUILDING_WITHIN_DISTANCE is a map-layout query: it caches its FIRST result
    (ScriptConditions.cpp:2270-2271, 2291-2294), so that evaluation must be the first frame - the first condition of the first OR
    block (evaluation short-circuits) of an active, undelayed, non-subroutine script in an active non-subroutine group (or none),
    which nothing else names (no switch, call or team hook can move it) - and of MY area"""
    o, _g, n, _occ = key
    area = [v for t, v in rb.decode(scb, leaf)[1] if t == 'TRIGGER_AREA']
    if not area or any(a not in MY_AREAS for a in area):
        return '%s reads the area %r: only my own perimeter %s is a map-layout query' % (TECH_NEAR, area, MY_AREAS)
    ors = [k for k in script.kids if k.name == 'OrCondition']
    fl, delay, _q = rb.mss.script_flags(script)
    grp = A.parent.get(key)
    gfl = tuple(grp.prefix[-2:]) if grp is not None else (1, 0)
    late = None
    if not ors or not ors[0].kids or ors[0].kids[0] is not leaf:
        late = 'it is not the first condition of the first OR block (an earlier one can postpone it)'
    elif not fl[0] or fl[5] or delay:
        late = 'its script is %s' % ('inactive' if not fl[0] else 'a subroutine' if fl[5] else 'delayed %ds' % delay)
    elif gfl != (1, 0):
        late = 'its group %r is %s' % (rb.node_name(grp), 'inactive' if not gfl[0] else 'a subroutine')
    else:
        mine = {n} | ({rb.node_name(grp)} if grp is not None else set())        # its group too
        users = sorted(rb.node_name(x) for _gg, _k, x in rb.scripts_of(scb, o) if x is not script and mine & refs_of(rb, scb, x))
        users += sorted(d['teamName'] for d in A.team_list if rb.team_get(d, 'teamOwner') == o
                        and any(TEAM_SCRIPT_FIELDS(k) and v in mine for k, _t, v in d['_e']))
        if users:
            late = '%r names its script or group (it can switch or run it later)' % users[0]
    if late:
        return '%s caches its first evaluation, which must be the first frame (map layout): %s' % (TECH_NEAR, late)
    return None


def check_module(rb, before, after, entry, mod, base_ctx):
    """raise Refused (passed in base_ctx['Refused']) when module `mod` broke a rule; returns the module's audit records"""
    Refused = base_ctx['Refused']
    rid = entry['id']
    owners = rb.GENERALS
    gens = set(entry['generals'])
    B, A = Index(rb, before, owners), Index(rb, after, owners)
    bad = lambda msg: Refused('%s: %s' % (rid, msg))

    # nothing outside the twelve lists and ScriptTeams; the empty-owner and civilian lists stay as they were
    for nb, na in zip(before.root, after.root):
        if (nb.name, nb.ver) != (na.name, na.ver) or (nb.name not in ('PlayerScriptsList', 'ScriptTeams') and rb.canon(before, nb) != rb.canon(after, na)):
            raise bad('changed the %s chunk, outside the script lists and teams' % na.name)
    if len(before.root) != len(after.root):
        raise bad('added or removed a top-level chunk, outside the script lists and teams')
    # the name table only grows: an existing entry renamed would change what Hard's stock bytes mean
    if after.toc[:len(before.toc)] != before.toc:
        raise bad('rewrites an existing entry of the name table - existing names are immutable, new ones are appended')
    old_n, old_i = {n for n, _i in before.toc}, {i for _n, i in before.toc}
    new_t = after.toc[len(before.toc):]
    for n_, i_ in new_t:                                # team keys and call names are name-table entries (A1/A2, B1)
        if '\x00' in n_:
            raise bad('appends the name-table entry %r: a NUL inside a name - the engine compares it as the C string %r'
                      % (n_, n_.split('\x00')[0]))
    if len({n for n, _i in new_t}) != len(new_t) or {n for n, _i in new_t} & old_n or \
            len({i for _n, i in new_t}) != len(new_t) or {i for _n, i in new_t} & old_i:
        raise bad('appends a name or id the name table already holds - every chunk name written through it would change')
    lb, la = rb.lists(before), rb.lists(after)
    if len(la) != len(lb):
        raise bad('changes the number of ScriptLists (%d -> %d) - a rule edits the generals\' lists, it never adds one' % (len(lb), len(la)))
    for i, o in enumerate(base_ctx['owners']):
        if (lb[i].name, lb[i].ver) != (la[i].name, la[i].ver):     # the engine reads only ScriptList chunks
            raise bad('changes the chunk of the %r ScriptList (%s v%s -> %s v%s)' % (o, lb[i].name, lb[i].ver, la[i].name, la[i].ver))
        if o not in owners and rb.canon(before, lb[i]) != rb.canon(after, la[i]):
            raise bad('changed the %r list, outside the script lists of the generals' % o)

    # every chunk sits where the shipped file puts its kind: the engine's readers register only those (a group nested in a group is
    # skipped whole, Scripts.cpp:906) - and Index sees only them
    for i, o in enumerate(base_ctx['owners']):
        if o in owners:
            for x in walk(la[i]):
                for k in x.kids or ():
                    if k.name not in base_ctx['kids_of'].get(x.name, ()):
                        raise bad('puts a %s inside a %s of %s (%r): the engine reads only %s there'
                                  % (k.name, x.name, o, rb.node_name(x), sorted(base_ctx['kids_of'].get(x.name, ()))))

    changed, added = [], []
    for k, s in A.scripts.items():
        if k not in B.scripts:
            added.append((k, s))
        elif A.canon(s) != B.canon(B.scripts[k]):
            changed.append((k, B.scripts[k], s))
    gone = [k for k in B.scripts if k not in A.scripts]
    if gone:
        raise bad('deletes or renames %s/%r - change or deactivate a stock script, never remove it (a removal has no tier mark)'
                  % (gone[0][0], gone[0][2]))
    ggone = [k for k in B.groups if k not in A.groups]
    if ggone:
        raise bad('deletes or renames the group %s/%r' % (ggone[0][0], ggone[0][1]))
    gadded = [k for k in A.groups if k not in B.groups]
    for k in A.groups:
        if k in B.groups and A.groups[k].prefix != B.groups[k].prefix:
            raise bad('changed the header of the stock group %s/%r' % (k[0], k[1]))
    # names: never a tier mark (only the compiler's last step writes them), never a stock name reused in its parent
    for (o, g, n, occ), _s in added:
        if '\x00' in n:
            raise bad('%s/%r: a NUL inside a name - the engine\'s C string ends there, so it is another name' % (o, n))
        if n.startswith(RESERVED):
            raise bad('%s/%r takes a reserved name (%s are the compiler\'s: tier marks, the clock, ENGINE enable lines)' % (o, n, RESERVED))
        if occ > 0:
            raise bad('a new script %s/%r reuses a stock name in its parent' % (o, n))
    for (o, gn, occ) in gadded:
        if '\x00' in gn:
            raise bad('the group %s/%r: a NUL inside a name - the engine\'s C string ends there' % (o, gn))
        if gn.startswith(RESERVED):
            raise bad('the group %s/%r takes a reserved name (%s are the compiler\'s)' % (o, gn, RESERVED))
        if occ > 0:
            raise bad('a new group %s/%r reuses a stock name in its parent' % (o, gn))
    # order: stock entries keep their relative order in every parent (a reorder has no mark form)
    for par, seq in B.order.items():
        now = [x for x in A.order.get(par, []) if x in set(seq)]
        if now != seq:
            raise bad('reorders the stock entries of %s/%s' % (par[0], par[1] or '(top level)'))
    tnames = [d['teamName'] for d in A.team_list]
    if len(set(tnames)) != len(tnames):
        raise bad('names the team %r twice - team names are unique in ScriptTeams' % collections.Counter(tnames).most_common(1)[0][0])
    border = [d['teamName'] for d in B.team_list]
    if [n for n in tnames if n in set(border)] != border:
        raise bad('reorders the stock teams')
    touched_owners = {k[0] for k, *_ in changed} | {k[0] for k, _ in added} | {k[0] for k in gadded}
    out_of = sorted(touched_owners - gens)
    if out_of:
        raise bad('edits %s, outside the entry\'s generals %s' % (out_of, sorted(gens)))

    normal_also = []
    for k, sb, sa in changed:
        o, g, n, occ = k
        if base_ctx['dup'].get((o, g, n), 1) > 1:
            raise bad('%s/%r is twice in its parent in the shipped file: not editable (the tier filter matches by name)' % (o, n))
        fb, db, _ = rb.mss.script_flags(sb)
        fa, da, _ = rb.mss.script_flags(sa)
        if fb[2:6] != fa[2:6]:
            raise bad('%s/%r: changes its difficulty or subroutine bits %r -> %r' % (o, n, fb[2:6], fa[2:6]))
        if fb[4] != 1:
            raise bad('%s/%r is not a Hard script (difficulty %r): Easy/Normal stay stock' % (o, n, fb[2:5]))
        if fb[2] or fb[3]:
            normal_also.append([o, n])
    for k, s in added:
        fl = rb.mss.script_flags(s)[0]
        if (fl[2], fl[3], fl[4]) != (0, 0, 1):
            raise bad('new script %s/%r must be Hard-only (difficulty %r)' % (k[0], k[2], fl[2:5]))

    # teams: only the entry's generals' teams move; none disappears
    tchanged, team_refs = [], []
    for n, d in A.teams.items():
        if n not in B.teams or rb.canon_team(d) != rb.canon_team(B.teams[n]):
            keys = [k for k, _t, _v in d['_e']]
            nul = [k for k, _t, v in d['_e'] if isinstance(v, str) and '\x00' in v]
            if nul or '\x00' in n:
                raise bad('team %r: a NUL inside %s - the engine\'s C string ends there' % (n, nul[0] if nul else 'its name'))
            if len(set(keys)) != len(keys):
                raise bad('team %r carries the field %r twice (the engine reads the last one)' % (n, collections.Counter(keys).most_common(1)[0][0]))
            tchanged.append(n)
            own = {rb.team_get(d, 'teamOwner')} | ({rb.team_get(B.teams[n], 'teamOwner')} if n in B.teams else set())
            if not own <= gens:
                raise bad('team %r of %s is outside the entry\'s generals %s' % (n, sorted(own), sorted(gens)))
            if n in B.teams:       # AIPlayer::isPossibleToBuildTeam wants a standing factory for EVERY unit type of a team, whatever its
                waived = getattr(mod, 'TEAM_UNIT_WAIVERS', {})        # minimum: a new type whose prerequisite may be missing stops the team
                slot = lambda t: {rb.team_get(t, 'teamUnitType%d' % i) for i in (1, 2, 3)} - {'<none>', None}
                new_units = sorted(u for u in slot(d) - slot(B.teams[n]) if u not in waived)
                if new_units:
                    raise bad('team %r gains the unit type %s: AIPlayer::isPossibleToBuildTeam needs a standing factory for every type of a team, '
                              'whatever its minimum, so the team is never picked while the prerequisite is missing. Re-kind the rule ENGINE, or '
                              'name the type in TEAM_UNIT_WAIVERS with the reason its prerequisite always stands' % (n, new_units))
            o = rb.team_get(d, 'teamOwner')
            reach = gift_reach(rb, after, o) if o in rb.GENERALS else {}
            if n in B.teams:                          # a stock team that runs a gift: its size, count and timing are the gift's
                reach_b = base_ctx['gift'].get(o, {})
                pays = sorted((k, v) for k, _t, v in B.teams[n]['_e'] if TEAM_SCRIPT_FIELDS(k) and v in reach_b)
                if pays:
                    raise bad('team %r runs a script that reaches PLAYER_GIVE_MONEY (%s %r: %s) - every team made pays: Hard\'s cash'
                              ' gifts stay frozen' % (n, pays[0][0], pays[0][1], reach_b[pays[0][1]]))
            for k, _t, v in d['_e']:
                old_v = rb.team_get(B.teams[n], k) if n in B.teams else None
                if TEAM_SCRIPT_FIELDS(k) and v != old_v and v in reach:
                    raise bad('team %r: %s names %r, which reaches PLAYER_GIVE_MONEY (%s) - Hard\'s cash gifts stay frozen'
                              % (n, k, v, reach[v]))
                if k in ENEMY_EVENT_FIELDS and v and v != old_v:
                    raise bad('team %r: %s names %r - an %s hook fires on enemies a human may not see (Team.cpp:1879 has no'
                              ' stealth-detection filter; stock uses none)' % (n, k, v, ENEMY_EVENT_FIELDS[k]))
                if TEAM_SCRIPT_FIELDS(k) and v and v != old_v:
                    base_ctx['new_named'].add((o, v))
                    team_refs.append((o, k, v, 'team %r' % n, n))
                    prod0 = rb.team_get(B.teams[n], 'teamProductionCondition') if n in B.teams else None
                    if k != 'teamProductionCondition' and prod0 and prod0 in base_ctx['enemy_reach'].get(o, {}):
                        raise bad('team %r is an enemy-gated team (its production condition %r %s): its hooks are that gate\'s'
                                  ' effects - a rule may change its unit counts, never give it a new %s'
                                  % (n, prod0, base_ctx['enemy_reach'][o][prod0], k))
                if TEAM_SCRIPT_FIELDS(k) and v and v != old_v and v in base_ctx['definers'].get(o, {}):
                    raise bad('team %r: %s names %r, which defines the shared attack-priority set %r (frozen like the gifts)'
                              % (n, k, v, base_ctx['definers'][o][v]))
    if [n for n in B.teams if n not in A.teams]:
        raise bad('deletes or renames a team')

    # gifts: frozen, and never named by a new or changed script
    for (o, g, n, occ), s in [(k, s) for k, _b, s in changed] + added:
        reach_b, reach_a = base_ctx['gift'][o], gift_reach(rb, after, o)
        if n in reach_b or n in reach_a:
            why = reach_b.get(n) or reach_a.get(n)
            raise bad('%s/%r reaches PLAYER_GIVE_MONEY (%s): Hard\'s cash gifts stay frozen - add a new script instead' % (o, n, why))
        named = sorted(x for x in refs_of(rb, after, s) if x in reach_b)
        if named:
            raise bad('%s/%r names %r, which reaches PLAYER_GIVE_MONEY: Hard\'s cash gifts stay frozen' % (o, n, named[0]))

    # shared priority tables: a stock script that defines one is frozen like a gift and never newly named - its conditions and timing
    # decide when a GLOBAL table exists, for every personality that applies it
    defs = base_ctx['definers']
    switched = []                                     # (owner, name, by): every script / group an added ENABLE / DISABLE switches
    new_refs = []                                     # (owner, via, name, by): every script / group a new or changed leaf newly names
    for (o, g, n, occ), sb, s in [(k, b, a) for k, b, a in changed] + [(k, None, s) for k, s in added]:
        if n in defs[o]:
            raise bad('%s/%r defines the shared attack-priority set %r: its conditions and timing decide when that GLOBAL table exists'
                      ' for every personality - frozen like the gifts' % (o, n, defs[o][n]))
        for via, x in sorted(refs_acts(rb, after, s) - (refs_acts(rb, before, sb) if sb is not None else set())):
            base_ctx['new_named'].add((o, x))
            if x in defs[o]:
                raise bad('%s/%r names %r, which defines the shared attack-priority set %r (frozen like the gifts)' % (o, n, x, defs[o][x]))
            new_refs.append((o, via, x, '%s/%r' % (o, n)))
        mb = leaf_multiset(rb, before, sb, ('ScriptAction', 'ScriptActionFalse')) if sb is not None else collections.Counter()
        for c in leaf_multiset(rb, after, s, ('ScriptAction', 'ScriptActionFalse')) - mb:
            if c[3] in SWITCH:
                switched += [(o, p[3], '%s/%r' % (o, n)) for p in c[5] if len(p) > 3 and rb.mss.PTYPES[p[0]] in ('SCRIPT', 'SCRIPT_SUBROUTINE')]

    # a changed stock script that writes enemy-derived state (or _ESCALATION) is not edited at all - its timing is the leak;
    # one gated by another player's state gains no new effect: no added action, no header change
    for key, sb, sa in changed:
        o, g, n, occ = key
        taint0, _w, invoked0 = base_ctx['taint'][o]
        writes = {v for l in leaf_list(rb, before, sb, ('ScriptAction', 'ScriptActionFalse')) for t, v in rb.decode(before, l)[1] if t in STATE_TYPES}
        hit = sorted(writes & (set(taint0) | {ESCALATION, GX_CLOCK}))
        if hit:
            raise bad('%s/%r writes %s, which carries another player\'s state (or is the clock): stock timing of it is never changed'
                      % (o, n, ESCALATION if ESCALATION in hit else hit[0]))
        gated = [reads_other(rb, before, l) for l in leaf_list(rb, before, sb)]
        gated = next((x for x in gated if x), None) or next(('reads %s (%s)' % (v, taint0[v]) for l in leaf_list(rb, before, sb)
                                                             for t, v in rb.decode(before, l)[1] if t in STATE_TYPES and v in taint0), None)
        if gated:
            ma = leaf_multiset(rb, after, sa, ('ScriptAction', 'ScriptActionFalse'))
            mb = leaf_multiset(rb, before, sb, ('ScriptAction', 'ScriptActionFalse'))
            if ma != mb or rb.mss.script_flags(sa)[:2] != rb.mss.script_flags(sb)[:2]:
                raise bad('%s/%r is gated on a condition that reads another player (%s): a rule may change its own-state thresholds, never'
                          ' give it a new effect or new timing' % (o, n, gated))
            if not gate_kept(enemy_blocks(rb, after, sa, taint0), enemy_blocks(rb, before, sb, taint0)):
                raise bad('%s/%r regroups its enemy gate (%s): every OR block must keep all the enemy-reading conditions of one stock'
                          ' block, so the new condition still implies the stock enemy gate (Codex rbd-r3-lensA 3)' % (o, n, gated))
        elif n in invoked0:
            # run behind an enemy gate (a team hook behind a tainted production condition, a call from a gated script) though its own
            # conditions read none: no new action and no new timing; an existing action's parameter may change only to point a team at
            # this rule's own NEW table
            ma = leaf_multiset(rb, after, sa, ('ScriptAction', 'ScriptActionFalse'))
            mb = leaf_multiset(rb, before, sb, ('ScriptAction', 'ScriptActionFalse'))
            kinds = lambda m: collections.Counter(c[3] for c in m.elements())
            ok = kinds(ma) == kinds(mb) and rb.mss.script_flags(sa)[:2] == rb.mss.script_flags(sb)[:2]
            for c in (ma - mb).elements() if ok else ():
                twins = [d for d in (mb - ma).elements() if d[3] == c[3] and len(d[5]) == len(c[5])]
                ok = any(all(p == q or (len(p) > 3 and rb.mss.PTYPES[p[0]] == 'ATTACK_PRIORITY_SET' and p[0] == q[0] and p[3] not in base_ctx['sets'])
                             for p, q in zip(c[5], d[5])) for d in twins)
                if not ok:
                    break
            if not ok:
                raise bad('%s/%r is run behind an enemy-gated trigger (%s): it gains no new action and no new timing - only a team may'
                          ' be pointed at the rule\'s own new table' % (o, n, invoked0[n]))

    for (o, g, n, occ), sb, sa in changed:              # what a frozen gift or table definer waits on
        if n in base_ctx['frozen_inputs'][o]:
            raise bad('%s/%r %s: frozen like the gifts it feeds' % (o, n, base_ctx['frozen_inputs'][o][n]))

    # team production conditions this module re-pointed: new script -> the stock condition script it replaces
    replaced = {}
    for tn in tchanged:
        if tn in B.teams:
            nv, ov = rb.team_get(A.teams[tn], 'teamProductionCondition'), rb.team_get(B.teams[tn], 'teamProductionCondition')
            if nv != ov and ov:
                replaced.setdefault((rb.team_get(A.teams[tn], 'teamOwner'), nv), set()).add(ov)

    # every node of a changed or added script / group carries a version the shipped file uses for that chunk (the engine's reader
    # picks the payload layout by it: a version-3 Condition is read without its name key, Scripts.cpp:1692)
    for node in [s for _k, _b, s in changed] + [s for _k, s in added] + [A.groups[k] for k in gadded]:
        for x in walk(node):
            if x.ver not in base_ctx['vers'].get(x.name, ()):
                raise bad('%s chunk version %r in %r: the shipped file writes %s as %s' % (x.name, x.ver, rb.node_name(node), x.name,
                                                                                        sorted(base_ctx['vers'].get(x.name, ()))))

    # every changed / added leaf: signature, cached fields, money, fog, escalation, clock, aliases, priority sets
    audits, hidden_reads = {}, []
    for key, sb, sa in [(k, b, a) for k, b, a in changed] + [(k, None, s) for k, s in added]:
        o, g, n, occ = key
        for kinds in (('Condition',), ('ScriptAction', 'ScriptActionFalse')):
            ma = leaf_multiset(rb, after, sa, kinds)
            mb = leaf_multiset(rb, before, sb, kinds) if sb is not None else collections.Counter()
            plus, minus = ma - mb, mb - ma
            for c in list(plus) + list(minus):
                if any(p[3] == ESCALATION for p in c[5] if len(p) > 3):
                    raise bad('%s/%r adds, removes or changes a leaf naming _ESCALATION (stock adds enemy tech and superweapons to it'
                              ' from unfiltered counts): gate new timing on _GX_CLOCK' % (o, n))
                if c[3] in PRIORITY_DEFINE:
                    sets = [p[3] for p in c[5] if len(p) > 3 and rb.mss.PTYPES[p[0]] == 'ATTACK_PRIORITY_SET']
                    if any(x in base_ctx['sets'] for x in sets):
                        raise bad('%s/%r edits the existing attack-priority set %r - set names are GLOBAL (a family shares one, GLA reads'
                                  ' China\'s): ADD a per-general set and point this general\'s teams at it' % (o, n, sets[0]))
            for l in leaf_list(rb, after, sa, kinds):
                if rb.canon(after, l) not in plus:
                    continue
                name, ps = rb.decode(after, l)
                for t, v in ps:
                    if isinstance(v, str) and '\x00' in v:
                        raise bad('%s/%r: %s parameter %s %r holds a NUL - the engine\'s C string ends there (Parameter::qualify'
                                  ' appends at it), so it names %r' % (o, n, name, t, v, v.split('\x00')[0]))
                    if t in QUALIFIED and isinstance(v, str):
                        alias = base_ctx['alias'](t, v)
                        if alias:
                            raise bad('%s/%r: %s name %r is an alias of %r once a start index is appended (qualification adds digits'
                                      ' without a separator)' % (o, n, t, v, alias))
                if kinds == ('Condition',):
                    cap = rb.GX_LEAVES.get(('Condition', name))
                    if cap and cap not in entry.get('capabilities', ()):
                        raise bad('%s/%r adds %s, but the rule does not declare %s (registry capabilities %r): the compiler checks the target'
                                  ' binary only for the capabilities a rule declares' % (o, n, name, cap, list(entry.get('capabilities', ()))))
                    why = reads_other(rb, after, l)
                    if why is None and name not in FAIR:
                        why = '%s is not on the own-state allow-list' % name
                    if why is None and name == TECH_NEAR:
                        why = tech_near_ok(rb, A, after, key, sa, l)
                    if why is None and name in SEEN:
                        why = seen_why(rb, after, o, sa, l)
                    if why is None:
                        for t, v in ps:
                            if t == 'TEAM' and v != '<This Team>' and rb.team_get(A.teams.get(v, {'_e': []}), 'teamOwner') != o:
                                why = '%s names team %r, not one of %s\'s own' % (name, v, o)
                    if why:
                        raise bad('%s/%r adds a condition that breaks fog: %s (only SENSE.* may read other players)' % (o, n, why))
                    for t, v in ps:
                        if t in STATE_TYPES and v != GX_CLOCK:
                            audits.setdefault(o, {}).setdefault(v, []).append((n, l, sb, sa))
                else:
                    if name in MONEY:
                        raise bad('%s/%r adds %s - a rule never makes money (Hard\'s gifts stay frozen)' % (o, n, name))
                    if name in HIDDEN:
                        raise bad('%s/%r adds %s, which %s (hidden information)' % (o, n, name, HIDDEN[name]))
                    if name in GLOBAL_WRITE:
                        why = seen_list_why(rb, after, o, sa, name, ps)
                        if why:
                            raise bad('%s/%r adds %s: %s' % (o, n, name, why))
                    elif name not in ACTIONS_OK:
                        raise bad('%s/%r adds %s, which is not on the reviewed action allow-list (guards.ACTIONS_OK: orders to my own'
                                  ' units, buildings, scripts and state whose engine side was read for hidden-state use)' % (o, n, name))
                    for t, v in ps:
                        if t == 'SIDE' and v != SIDE_ME:
                            raise bad('%s/%r adds %s on SIDE %r - it reads another player' % (o, n, name, v))
                        if t in STATE_TYPES and v == GX_CLOCK:
                            raise bad('%s/%r writes _GX_CLOCK - only the compiler\'s clock does' % (o, n))
                        if t in ('COUNTER', 'FLAG') and v in base_ctx['names'].get(t, ()):
                            raise bad('%s/%r %s writes the stock state %r - stock flags and counters are stock logic\'s (a frozen gift or'
                                      ' table definer may wait on one); a rule writes only its own new state (Codex rbd-r3-lensB 5)'
                                      % (o, n, name, v))
                    if name in ENGINE_READS:
                        hidden_reads.append([o, n, name, ENGINE_READS[name]])
                    tgt = [v for t, v in ps if (t == 'TRIGGER_AREA' and v in ENEMY_AREAS) or t == 'SKIRMISH_WAYPOINT_PATH']
                    if tgt:                             # a target, not a read (plan 1.3: NO_ENEMY_OBJECTS) - listed
                        hidden_reads.append([o, n, name, 'targets the current enemy\'s start (%s): public in 1v1 and with chosen'
                                             ' starts, unscouted with random starts on larger maps' % tgt[0]])
                typ, _key, raw = rb.mss.parse_call(after, l.raw)
                if _key & 0xFF != 3:                    # every stock call key's low byte (18,924 of 18,924; DataChunk.cpp:994)
                    raise bad('%s/%r: %s carries the name-key low byte %d, every stock key carries 3' % (o, n, name, _key & 0xFF))
                want = rb.leaf_types(base_ctx['base_scb'], l.name if l.name != 'ScriptActionFalse' else 'ScriptAction', name)
                got = [rb.mss.PTYPES[p[0]] for p in raw]
                if want is None or got != want:
                    raise bad('%s/%r: %s with parameter signature %r, the stock signature is %r' % (o, n, name, got, want))
                expect = rb.enums()[0 if kinds == ('Condition',) else 1].get(name)
                if typ != expect:
                    raise bad('%s/%r: %s carries type %r, the engine\'s is %r (signature)' % (o, n, name, typ, expect))
                for p in raw:
                    t = rb.mss.PTYPES[p[0]]
                    if t == 'COORD3D':
                        continue
                    if t in rb.NAMED and (p[1] != 0 or struct.pack('<f', p[2]) != b'\0\0\0\0'):
                        raise bad('%s/%r: %s parameter %s %r carries cached values (%r, %r) - compiled parameters hold none'
                                  % (o, n, name, t, p[3], p[1], p[2]))
                    if t not in rb.NAMED and p[3]:
                        raise bad('%s/%r: %s numeric parameter %s carries the string %r (cached)' % (o, n, name, t, p[3]))

    # reads of stock state: tainted ones only as the stock gate itself, copied - in a changed script whose actions and timing are
    # unchanged, or in a new team production condition that keeps the stock one's leaves and actions (no waivers)
    for o, names in audits.items():
        taint, writers, _inv = state_taint(rb, after, o)
        for v, uses in list(names.items()):
            ok_copy = True
            for (n, l, sb, sa) in uses:
                if v not in taint:
                    continue
                cl = rb.canon(after, l)
                if sb is not None:
                    same = leaf_multiset(rb, after, sa, ('ScriptAction', 'ScriptActionFalse')) == leaf_multiset(rb, before, sb, ('ScriptAction', 'ScriptActionFalse'))
                    ok = same and cl in {rb.canon(before, x) for x in leaf_list(rb, before, sb)}
                else:
                    oks = []                            # a copy must keep EVERY condition it stands in for
                    for stock in replaced.get((o, n), ()):
                        hits = [x for _g, _k, x in rb.scripts_of(before, o) if rb.node_name(x) == stock]
                        s0 = hits[0] if len(hits) == 1 else None
                        oks.append(s0 is not None and cl in {rb.canon(before, x) for x in leaf_list(rb, before, s0)} and
                                   leaf_multiset(rb, after, sa, ('ScriptAction', 'ScriptActionFalse')) == leaf_multiset(rb, before, s0, ('ScriptAction', 'ScriptActionFalse')) and
                                   gate_kept(enemy_blocks(rb, after, sa, taint), enemy_blocks(rb, before, s0, taint)))
                    ok = bool(oks) and all(oks)
                    if ok:                              # {(owner, copy): the stock conditions it was checked against}
                        base_ctx['copies'].setdefault((o, n), set()).update(replaced.get((o, n), ()))
                ok_copy = ok_copy and ok
                if not ok:
                    raise bad('%s/%r reads %r, which carries an enemy read (fog): %s - only the stock gate itself may be kept, copied'
                              ' unchanged with its own actions' % (o, n, v, taint[v]))
            names[v] = {'writers': sorted(writers.get(v, ())), 'tainted': v in taint, 'why': taint.get(v), 'stock_gate_copy': v in taint}
            base_ctx['reads'].append((o, v, entry['id'], v in taint))

    # references: a new reference (leaf or team field) reaches only clean stock logic, a group of none of the frozen kind, or this
    # compile's own scripts - a new trigger of stock logic that reads another player adds an enemy input
    er, dg = base_ctx['run_reach'], base_ctx['definer_groups']
    for o, via, x, by, tn in [r + (None,) for r in new_refs] + team_refs:
        if x.startswith(RESERVED):                    # the clock and the ENGINE lines are generated after every module
            raise bad('%s names %s/%r through %s: %s names are reserved for what the compiler generates (the clock, ENGINE enable'
                      ' lines, tier marks) - a rule never switches, calls or hooks them' % (by, o, x, via, RESERVED))
        if x in dg.get(o, {}):
            raise bad('%s names the group %s/%r, which holds a script that defines the shared attack-priority set %r (frozen like the gifts)'
                      % (by, o, x, dg[o][x]))
        if x in base_ctx['list_names'][o]:
            if x in er[o] and via != 'DISABLE_SCRIPT':
                raise bad('%s names %s/%r through %s: it %s - a new trigger of stock logic that reads another player adds an enemy input'
                          ' (only a DISABLE may name it)' % (by, o, x, via, er[o][x]))
            if x in base_ctx['frozen_inputs'].get(o, {}):
                raise bad('%s names %s/%r through %s: it %s - frozen like the gifts it feeds (no switch, call or hook may move it)'
                          % (by, o, x, via, base_ctx['frozen_inputs'][o][x]))
            # a stock script a rule switches, calls, runs as sequential orders or hooks is that rule's too: two rules driving one
            # stock script are an overlap (a sequential run ignores the script's active bit)
            claimed = {tuple(c) for c in base_ctx['claims'][rid]['scripts'] + base_ctx['claims'][rid]['groups']}
            if (o, x) not in claimed:
                raise bad('%s names the stock %s/%r through %s, which its CLAIMS do not name' % (by, o, x, via))
        elif (o, x) in base_ctx['copies'] and not (via == 'teamProductionCondition' and tn in B.teams and
                                                   rb.team_get(B.teams[tn], 'teamProductionCondition') in replaced.get((o, x), ())):
            raise bad('%s names %s/%r, a copied stock enemy gate - it may only stand in for that team\'s own production condition'
                      % (by, o, x))

    # claims: what the module touched is what it declared
    cl = base_ctx['claims'][rid]
    cs, cg, ct = {tuple(x) for x in cl['scripts']}, {tuple(x) for x in cl['groups']}, set(cl['teams'])
    for (o, g, n, occ) in [k for k, *_ in changed] + [k for k, _ in added]:
        if (o, n) not in cs:
            raise bad('touched %s/%r, which its CLAIMS do not name' % (o, n))
    for (o, gn, occ) in gadded:
        if (o, gn) not in cg:
            raise bad('added the group %s/%r, which its CLAIMS do not name' % (o, gn))
    for n in tchanged:
        if n not in ct:
            raise bad('touched team %r, which its CLAIMS do not name' % n)
    # a script or group a new ENABLE / DISABLE switches is this module's too: two modules switching one script are an overlap
    #
    for o, x, by in switched:
        if (o, x) not in cs and (o, x) not in cg:
            raise bad('%s switches %s/%r (ENABLE_SCRIPT / DISABLE_SCRIPT), which its CLAIMS do not name' % (by, o, x))
    # a new set this module defines (or defines further) is its claim, whoever defined it first
    mine = set()
    for key, sb, sa in [(k, b, a) for k, b, a in changed] + [(k, None, s) for k, s in added]:
        mb = leaf_multiset(rb, before, sb, ('ScriptAction', 'ScriptActionFalse')) if sb is not None else collections.Counter()
        for c in leaf_multiset(rb, after, sa, ('ScriptAction', 'ScriptActionFalse')) - mb:
            if c[3] in PRIORITY_DEFINE:
                mine |= {p[3] for p in c[5] if len(p) > 3 and rb.mss.PTYPES[p[0]] == 'ATTACK_PRIORITY_SET' and p[3] not in base_ctx['sets']}
    if mine - set(cl['priority_sets']):
        raise bad('defines the attack-priority set %r, which its CLAIMS do not name' % sorted(mine - set(cl['priority_sets']))[0])
    return {'normal_also': normal_also, 'flag_audit': audits, 'engine_hidden_reads': hidden_reads}


def priority_definitions(rb, scb, base_sets):
    """{new set name: {owner: its definition}} - every define leaf (in file order) naming a set the shipped file does not have"""
    where = collections.defaultdict(dict)
    for o in rb.GENERALS:
        for _g, _k, s in rb.scripts_of(scb, o):
            for l in leaf_list(rb, scb, s, ('ScriptAction', 'ScriptActionFalse')):
                nm, ps = rb.decode(scb, l)
                if nm in PRIORITY_DEFINE:
                    for t, v in ps:
                        if t == 'ATTACK_PRIORITY_SET' and v not in base_sets:
                            # with its whole defining script: WHEN a global table is set is part of it
                            where[v].setdefault(o, []).append((nm, tuple(ps), rb.canon(scb, s)))
    return where


def list_names(rb, scb, owner):
    """Counter of every script AND group name of a list, all parents: the engine's name lookups see one namespace"""
    c = collections.Counter()
    for k in rb.owner_list(scb, owner).kids:
        if k.name in ('Script', 'ScriptGroup'):
            c[rb.node_name(k)] += 1
        if k.name == 'ScriptGroup':
            c.update(rb.node_name(x) for x in k.kids if x.name == 'Script')
    return c


def qualified_names(rb, scb):
    """{kind: names} of every name Parameter::qualify suffixes, in every general's list and ScriptTeams (one match holds any mix)"""
    names = collections.defaultdict(set)
    for o in rb.GENERALS:
        names['SCRIPT'] |= set(list_names(rb, scb, o))
        for _g, _k, x in rb.scripts_of(scb, o):
            for l in rb.leaves(x):
                for t, v in rb.decode(scb, l)[1]:
                    if t in QUALIFIED and isinstance(v, str):
                        names['SCRIPT' if t == 'SCRIPT_SUBROUTINE' else t].add(v)
    names['TEAM'] |= {d['teamName'] for d in rb.teams_ro(scb)}
    for d in rb.teams_ro(scb):
        names['SCRIPT'] |= {v for k, _t, v in d['_e'] if TEAM_SCRIPT_FIELDS(k) and v}
    return names


def alias_pairs(names, base):
    """[(kind, a, b)]: a == b + decimal digits, at least one of them new (not in base) - qualification appends the start index
    without a separator, so a at index i and b at index (digits + i) share one engine name"""
    out = []
    for kind, ns in sorted(names.items()):
        stems = collections.defaultdict(list)
        for n in ns:
            stems[n.rstrip('0123456789')].append(n)
        for group in stems.values():
            for a in group:
                for b in group:
                    if a != b and a.startswith(b) and a[len(b):].isdigit() and (a not in base.get(kind, ()) or b not in base.get(kind, ())):
                        out.append((kind, a, b))
    return sorted(out)


def check_names(rb, scbs, base_ctx, where):
    """new names of the finished file(s) neither shadow a name of their list nor alias another name"""
    Refused = base_ctx['Refused']
    union = collections.defaultdict(set)
    for scb in scbs:
        for o in rb.GENERALS:
            for n, c in list_names(rb, scb, o).items():
                # the compiler's own group + script pairs (a module never takes a reserved name: check_module)
                if c > 1 and c > base_ctx['list_names'][o].get(n, 0) and not (c == 2 and n.startswith(RESERVED[:2])):
                    raise Refused('%s: %s/%r shadows another script or group of its list: the engine looks names up in one namespace'
                                  ' (CALL_SUBROUTINE finds a group first, ENABLE_SCRIPT switches both)' % (where, o, n))
        for k, v in qualified_names(rb, scb).items():
            union[k] |= v
    hits = alias_pairs(union, base_ctx['names'])
    if hits:
        kind, a, b = hits[0]
        raise Refused('%s: the %s names %r and %r alias once a start index is appended (%r at index 0 and %r at index %s are both %r):'
                      ' every general and tier of a compile can meet in one match' % (where, kind, a, b, a, b, a[len(b):] + '0', a + '0'))


def pool_names(rb, scb, owner):
    """{'COUNTER': names, 'FLAG': names, 'ATTACK_PRIORITY_SET': names} a list makes the engine allocate"""
    out = {'COUNTER': set(), 'FLAG': set(), 'ATTACK_PRIORITY_SET': set()}
    for _g, _k, s in rb.scripts_of(scb, owner):
        for l in rb.leaves(s):
            for t, v in rb.decode(scb, l)[1]:
                if t in out:
                    out[t].add(v)
    return out


def pool_budget(rb, scbs, base_ctx, touched):
    """the engine pools a compile can fill: global tables over every general and tier view, and the heaviest
    general's counters / flags, which a match holds once per player - refused when a retail map's 8 players could overflow them"""
    Refused = base_ctx['Refused']
    sets, heavy = set(), {'COUNTER': 0, 'FLAG': 0}
    for scb in scbs:
        for o in rb.GENERALS:
            p = pool_names(rb, scb, o) if o in touched else base_ctx['pool'][o]
            sets |= p['ATTACK_PRIORITY_SET']
            for t in heavy:
                heavy[t] = max(heavy[t], len(p[t]))
    out = {'priority_sets': {'used': len(sets), 'limit': POOL}}
    if len(sets) > POOL:
        raise Refused('the compile names %d attack-priority tables: the engine\'s pool holds %d besides the default (ScriptEngine.h:221),'
                      ' and a full pool drops stock Hard\'s own tables' % (len(sets), POOL))
    for t, key in (('COUNTER', 'counters'), ('FLAG', 'flags')):
        mp = POOL // max(heavy[t], 1)
        out[key] = {'heaviest_general': heavy[t], 'max_players': mp, 'limit': POOL}
        if mp < POOL_PLAYERS:
            raise Refused('a general\'s list names %d %s names: %d players of it fill the engine\'s %d-slot %s pool (per player,'
                          ' ScriptEngine.h:221) - a retail map holds %d' % (heavy[t], t.lower(), mp + 1, POOL, t.lower(), POOL_PLAYERS))
    return out


# --------------------------------------------------------------------------- rb-data-13: matchup keys (the lead's ruling)
def mu_shareable(rb, item, stock_teams):
    """an item two MATCHUP rules with DIFFERENT keys may both claim: a stock team (each switches it only through its off gate), the shared
    latch group and its scripts, a stock team's off gate"""
    if isinstance(item, str):
        return item in stock_teams
    n = item[1]
    return n == rb.MU_GROUP or n.startswith(rb.MU_OFF) or n in {'%s %s' % (rb.MU_GROUP, f) for f in rb.SENSE_BASE}


def overlap_refusal(rb, a, b, ka, kb, hit, stock_teams):
    """None when two entries' CLAIMS overlap is proven harmless (no overlap, or two MATCHUP keys that differ sharing only what
    mu_shareable allows: the latch sets one key at most, so the two never run together), else the refusal text"""
    if not hit:
        return None
    if ka and kb and ka != kb:
        rest = [x for x in hit if not mu_shareable(rb, x, stock_teams)]
        if not rest:
            return None
        return ('%s and %s: CLAIMS overlap on %r - a DATA conflict, never both on for one player (MATCHUP %s and %s may share only stock'
                ' teams, the shared latch and the off gates)' % (a, b, rest[0], ka, kb))
    if ka and ka == kb:
        return ('%s and %s: CLAIMS overlap on %r and both declare MATCHUP %s - one rule per matchup key: the compiler cannot prove two rules'
                ' of one key exclusive' % (a, b, hit[0], ka))
    why = ' and '.join(r for r, k in ((a, ka), (b, kb)) if not k)
    return ('%s and %s: CLAIMS overlap on %r - a DATA conflict, never both on for one player (%s declares no MATCHUP key, so the compiler'
            ' cannot prove them mutually exclusive)' % (a, b, hit[0], why))


def _leaf_bag(rb, scb):
    c = collections.Counter()
    for o in rb.GENERALS:
        for _g, _k, s in rb.scripts_of(scb, o):
            n = rb.node_name(s)
            for l in rb.leaves(s):
                nm, ps = rb.decode(scb, l)
                c[(o, n, l.name, nm, tuple(ps))] += 1
    return c


def _gated_on(rb, scb, s, flag):
    blocks = [b for b in s.kids if b.name == 'OrCondition']
    return bool(blocks) and all(any(rb.mu_leaf(scb, l) == (flag, 1) for l in b.kids) for b in blocks)


def matchup_module(rb, before, after, entry, key, ctx):
    """one module against the shared latch. Nobody writes a gxrb_mu_ flag but the latch's own scripts (checked canonical at the end). A
    module without MATCHUP reads none. A module with MATCHUP K: reads gxrb_mu_<K> only (OFF only inside an off gate); every script it adds
    (the latch, off gates and subroutines only its new teams hook aside) and the production condition of every team it adds are gated on gxrb_mu_<K> ON in every
    OR block - else it claims a key it does not gate on; it changes a stock team only through its off gate (matchup_off) and changes no
    stock script: anything else would run against every enemy, and the overlap exemption rests on it"""
    rid, Refused = entry['id'], ctx['Refused']
    bad = lambda msg: Refused('%s: %s' % (rid, msg))
    latch = {'%s %s' % (rb.MU_GROUP, f) for f in rb.SENSE_BASE}
    allf = rb.MU_FLAGS + (rb.MU_DECIDED,)
    # every rule, keyed or not: a NEW reference (a leaf's SCRIPT parameter, a team's script field) names stock logic or this rule's own
    # claims - never a script or group another rule added (a GLA rule's team hooking a USA rule's ungated subroutine
    # runs it outside its key, and turning the USA rule off would leave the hook dangling)
    mine = {tuple(x) for x in ctx['claims'][rid]['scripts'] + ctx['claims'][rid]['groups']}
    stock = lambda o, x: x in ctx['list_names'][o]
    tb0 = {d['teamName']: d for d in rb.teams_ro(before)}
    refs = [(o, x, '%s/%r' % (o, n)) for (o, n, kind, nm, ps) in _leaf_bag(rb, after) - _leaf_bag(rb, before)
            for t, x in ps if t in ('SCRIPT', 'SCRIPT_SUBROUTINE')]
    refs += [(rb.team_get(d, 'teamOwner'), v, 'team %r %s' % (d['teamName'], k)) for d in rb.teams_ro(after) for k, _t, v in d['_e']
             if TEAM_SCRIPT_FIELDS(k) and v and (d['teamName'] not in tb0 or v != rb.team_get(tb0[d['teamName']], k))]
    for o, x, by in refs:
        if x and not stock(o, x) and (o, x) not in mine:
            raise bad('%s names %s/%r, a script another rule added, which its CLAIMS do not name' % (by, o, x))
    for (o, n, kind, nm, ps) in _leaf_bag(rb, after) - _leaf_bag(rb, before):
        fl = [v for t, v in ps if t == 'FLAG' and v in allf]
        if not fl or (n in latch):
            continue
        if kind != 'Condition':
            raise bad('%s/%r writes the matchup flag %s - only the shared latch (rb.matchup) does' % (o, n, fl[0]))
        if key is None:
            raise bad('%s/%r reads the matchup latch flag %s without declaring MATCHUP' % (o, n, fl[0]))
        v = ps[1][1] if len(ps) > 1 else None
        if not (fl[0] == rb.mu_flag(key) and (v == 1 or n.startswith(rb.MU_OFF))):
            raise bad('declares MATCHUP %s but gates on %s == %s in %s/%r - a rule gates only on its own key ON (OFF only in an off'
                      ' gate)' % (key, fl[0], v, o, n))
    if key is None:
        return
    flag = rb.mu_flag(key)
    gated = 0
    tb = {d['teamName']: d for d in rb.teams_ro(before)}
    # a subroutine only a NEW team of this rule hooks runs only where that (gated) team runs: e.g. its launch hook
    hooked = {(rb.team_get(d, 'teamOwner'), rb.team_get(d, k)) for d in rb.teams_ro(after) if d['teamName'] not in tb
              for k in rb.TEAM_HOOKS if rb.team_get(d, k)}
    for o in rb.GENERALS:
        old, seen = collections.defaultdict(list), collections.Counter()     # stock names can occur twice in a list: by occurrence
        for _g, _k, x in rb.scripts_of(before, o):
            old[rb.node_name(x)].append(rb.canon(before, x))
        for _g, _k, x in rb.scripts_of(after, o):
            n = rb.node_name(x)
            if n in latch or n.startswith(rb.MU_OFF):
                continue
            seen[n] += 1
            if n in old:
                if seen[n] > len(old[n]) or rb.canon(after, x) != old[n][seen[n] - 1]:
                    raise bad('changes the script %s/%r - a MATCHUP rule changes stock only through a team\'s off gate, and runs its own'
                              ' scripts behind its key' % (o, n))
                continue
            if any(l.name == 'ScriptActionFalse' for l in rb.leaves(x)):
                raise bad('%s/%r holds a false action: it runs exactly when the conditions fail, so behind gxrb_mu_%s ON it runs when the'
                          ' key is OFF' % (o, n, key.lower()))
            if rb.mss.script_flags(x)[0][5] and (o, n) in hooked:
                continue
            if not _gated_on(rb, after, x, flag):
                raise bad('declares MATCHUP %s but %s/%r does not gate on it (an OR block without FLAG(%s) == 1)' % (key, o, n, flag))
            gated += 1
    for d in rb.teams_ro(after):
        n, pc = d['teamName'], rb.team_get(d, 'teamProductionCondition')
        if n in tb:
            if rb.canon_team(d) != rb.canon_team(tb[n]):
                strip = lambda t: [e for e in rb.canon_team(t) if e[0] != 'teamProductionCondition']
                if strip(d) != strip(tb[n]) or pc != rb.MU_OFF + n:
                    raise bad('changes the stock team %r beyond its production condition -> %r: a MATCHUP rule changes a stock team only'
                              ' through its off gate' % (n, rb.MU_OFF + n))
            continue
        o = rb.team_get(d, 'teamOwner')
        hit = [x for _g, _k, x in rb.scripts_of(after, o) if rb.node_name(x) == pc]
        if len(hit) != 1 or not _gated_on(rb, after, hit[0], flag):
            raise bad('declares MATCHUP %s but its team %r (production condition %r) does not gate on it' % (key, n, pc))
    if not gated:
        raise bad('declares MATCHUP %s but does not gate on it (no script reads FLAG(%s) == 1)' % (key, flag))


def team_provenance(rb, before, after, entry, Refused):
    """rb-data-17: every team a module adds was built by rb.variant_team, which clones its donor and records
    donor -> team, or declared by rb.add_new_team. These guards check our own reviewed modules, so a recorded donor
    is enough; what no table can read is intent (a true copy of T' whose slots were meant to replace T)"""
    old = {d['teamName'] for d in rb.teams_ro(before)}
    for d in rb.teams_ro(after):
        v = d['teamName']
        if v not in old and v not in rb.BUILT:
            raise Refused('%s: its team %r was added by hand - a new team goes through rb.variant_team (a copy of a stock team, recorded) or'
                          ' rb.add_new_team (declared new)' % (entry['id'], v))


def hook_shape(rb, scb, x):
    """a hook target as guards.team_hooks reads it: (flags, delay, OR blocks, true actions, false actions), each leaf NAME(params) with what a rule may choose
    blanked to its type (a number, a named script or area, a priority set, a counter other than the clock) and everything else kept (the team it orders, a comparison)"""
    def leaf(l):
        nm, ps = rb.decode(scb, l)
        return '%s(%s)' % (nm, ', '.join(str(v) if t in ('TEAM', 'COMPARISON') or (t, v) == ('COUNTER', GX_CLOCK) else t for t, v in ps))
    fl, delay, _ = rb.mss.script_flags(x)
    return (tuple(fl), delay, tuple(tuple(leaf(l) for l in b.kids) for b in x.kids if b.name == 'OrCondition'),
            tuple(leaf(a) for a in x.kids if a.name == 'ScriptAction'), tuple(leaf(a) for a in x.kids if a.name == 'ScriptActionFalse'))


def team_hooks(rb, before, after, entry, Refused):
    """rb-data-19 (the second round of new routes after r2 and r3): the launch check is an ALLOW-LIST of the exact shapes the shipped modules use, not a model of the engine, and it is
    stricter than the engine BY DESIGN. Anything else is refused and the refusal names the shape, whether or not the engine could run it: a group, a CALL_SUBROUTINE, a false action, a repeating generic hook, a missing
    target. ENABLE_SCRIPT then CALL_SUBROUTINE of an inactive script or group would run, and is refused all the same: no rule uses it. The list is what every shipped module compiles to at its defaults and at each param's lo and hi,
    and every smoke pack; a rule that needs a new shape adds it here, with its red arm.
    Why these shapes (rb-data-18, smoke14: USA_VS_GLA's Burton stayed near home for the whole game): Player::update runs a team's generic hooks whether it has formed or not and a one-shot hook is spent on
    its first true evaluation, so a generic hook either cannot fire on an empty team (TEAM_HAS_UNITS) or has a twin on teamOnCreateScript, which runs when the team forms (rb.launch_scripts: same conditions, same actions);
    ScriptEngine::executeScript switches a one-shot script off for every team of the player and a delay is shared, so a formation script is neither, and no other script may name it.
    A hook is judged when it is the module's: on a new team, a team it moved to another owner, a name it set, or a name whose script now leads to anything else (closure)"""
    was = {d['teamName']: d for d in rb.teams_ro(before)}
    hunt, stop, guard, seq = 'TEAM_HUNT(<This Team>)', 'TEAM_STOP(<This Team>)', 'TEAM_GUARD_AREA(<This Team>, TRIGGER_AREA)', 'TEAM_EXECUTE_SEQUENTIAL_SCRIPT(<This Team>, SCRIPT)'
    add, always, clock = 'INCREMENT_COUNTER(INT, COUNTER)', (('CONDITION_TRUE()',),), (('COUNTER(%s, %d, INT)' % (GX_CLOCK, rb.GE),),)
    FORMED = ((1, 0, 0, 0, 1, 1), 0)                                     # active, not one-shot, Hard only, a subroutine; no delay
    CLOCK_TWIN = FORMED + (clock, (hunt,), ())
    TECH = (1, 0, 0, 1, 1, 1), 0, always                                 # stock's Tech Capture: OIL.CAPTURE_TEAMS changes the priority set of the sequential script it names
    upgrade = 'AI_PLAYER_BUILD_UPGRADE(SIDE, UPGRADE)'
    ON_CREATE = {CLOCK_TWIN} | {FORMED + (always, acts, ()) for acts in ((hunt,), (seq,), (stop, seq), (add, seq), (add, stop, seq), (guard,), (add, guard))} | {TECH + (acts, ()) for acts in ((seq, upgrade), (upgrade, seq))}
    CLOCK_LAUNCH = ((1, 1, 0, 0, 1, 1), 0, clock, (hunt,), ())           # the one-shot half of a clock launch: it needs a CLOCK_TWIN
    PRIORITY = {(fl, 0, (('TEAM_HAS_UNITS(<This Team>)',),), ('TEAM_APPLY_ATTACK_PRIORITY_SET(<This Team>, ATTACK_PRIORITY_SET)',), ()) for fl in ((1, 1, 0, 0, 1, 1), (1, 1, 1, 1, 1, 1))}
    prio, move, follow, button = 'TEAM_APPLY_ATTACK_PRIORITY_SET(<This Team>, ATTACK_PRIORITY_SET)', 'SKIRMISH_MOVE_TO_APPROACH_PATH(<This Team>, SKIRMISH_WAYPOINT_PATH)', 'SKIRMISH_FOLLOW_APPROACH_PATH(<This Team>, SKIRMISH_WAYPOINT_PATH, BOOLEAN)', 'TEAM_HUNT_WITH_COMMAND_BUTTON(<This Team>, COMMANDBUTTON_ALL_ABILITIES)'
    STOCK_SEQ = (1, 0, 1, 1, 1, 1), 0, always
    SEQUENTIAL = {STOCK_SEQ + (acts, ()) for acts in ((move, follow, hunt), (move, follow, button), ('TEAM_LOAD_TRANSPORTS(<This Team>)', move, follow, hunt), ('TEAM_STOP_AND_DISBAND(<This Team>)',))} | \
        {TECH + (acts, ()) for acts in ((prio, button), (prio, 'SKIRMISH_WAIT_FOR_COMMANDBUTTON_AVAILABLE_ALL(SIDE, <This Team>, COMMANDBUTTON_ALL_ABILITIES)', button))}
    GENERIC = [k for k in rb.TEAM_HOOKS if k.startswith('teamGenericScriptHook')]
    NO_GROUND_WEAPON = ('GLAVehicleBattleBus',)                          # INIZH: PRIMARY None and an AntiGround No dummy; only passengers fire, so AIHuntState finds it nothing on the ground. Any slot, not only an all-Bus team: smoke17's mixed teams left their Buses home
    orders = lambda s: [rb.canon(after, k) for k in s.kids]              # OR blocks and actions; the name and the flags are the shape's
    tables, closed, naming = {}, {}, {}

    def lookups(scb, o):
        """(group name -> node, script name -> node)"""
        if (scb is after, o) not in tables:
            tables[(scb is after, o)] = ({rb.node_name(k): k for k in rb.owner_list(scb, o).kids if k.name == 'ScriptGroup'},
                                         {rb.node_name(x): x for _g, _k, x in rb.scripts_of(scb, o)})
        return tables[(scb is after, o)]

    def closure(scb, o, name):
        """everything a hook's name can lead to, comparable across two files: the group and the script of that name, then whatever any of their leaves names, transitively"""
        if (scb is after, o, name) not in closed:
            groups, scripts = lookups(scb, o)
            out, seen, todo = [], set(), [name]
            while todo:
                n = todo.pop()
                if n not in seen:
                    seen.add(n)
                    for x in [m[n] for m in (groups, scripts) if n in m]:
                        out.append(rb.canon(scb, x))
                        todo.extend(sorted(refs_of(rb, scb, x)))
            closed[(scb is after, o, name)] = out
        return closed[(scb is after, o, name)]

    def named_by(o):
        """{name: the scripts of `o` whose leaves name it}"""
        if o not in naming:
            naming[o] = collections.defaultdict(set)
            for _g, _k, y in rb.scripts_of(after, o):
                for v in refs_of(rb, after, y):
                    naming[o][v].add(rb.node_name(y))
        return naming[o]

    def target(n, o, k, v):
        groups, scripts = lookups(after, o)
        if v in groups or v not in scripts:
            raise Refused('%s: team %r hook %s names %r, which is %s: it must name a script (guards.team_hooks allows only the shapes the shipped rules use)' % (entry['id'], n, k, v, 'a group' if v in groups else 'no script'))
        return scripts[v], hook_shape(rb, after, scripts[v])

    def no_shape(n, k, v, shape):
        return Refused('%s: team %r hook %s runs %r, a shape no shipped rule uses: flags %s delay %d if %s then %s else %s (guards.team_hooks allows only the shipped shapes, stricter than the engine by design: use rb.launch_scripts, or add the shape with its red arm)'
                       % ((entry['id'], n, k, v) + shape))

    for d in rb.teams_ro(after):
        n, o = d['teamName'], rb.team_get(d, 'teamOwner')
        old = was.get(n)
        if o not in rb.GENERALS:
            continue

        def fresh(k):
            v = rb.team_get(d, k)
            if not v:
                return False
            if old is None or rb.team_get(old, 'teamOwner') != o or rb.team_get(old, k) != v:      # any move, from a non-general too: a name reads against its owner's list
                return True
            return closure(before, rb.team_get(old, 'teamOwner'), v) != closure(after, o, v)
        for k in ('teamOnDestroyedScript', 'teamOnIdleScript', 'teamOnUnitDestroyedScript'):      # teamEnemySightedScript and teamAllClearScript: check_module
            if fresh(k):
                raise Refused('%s: team %r hook %s runs %r: no shipped rule uses this hook (guards.team_hooks allows only the shipped shapes)' % (entry['id'], n, k, rb.team_get(d, k)))
        groups, scripts = lookups(after, o)
        oc = rb.team_get(d, 'teamOnCreateScript')
        twin = None if oc in groups else scripts.get(oc)
        launches = [scripts[rb.team_get(d, k)] for k in GENERIC if rb.team_get(d, k) in scripts]
        for u, _lo, hi in rb.team_slots(d):
            if (hi or 0) > 0 and str(u).endswith(NO_GROUND_WEAPON):
                for k in ['teamOnCreateScript'] + GENERIC:       # the slots or the hook may be what changed (rbd19 r1)
                    v = rb.team_get(d, k)
                    if v in scripts and hunt in hook_shape(rb, after, scripts[v])[3] and (old is None or rb.team_slots(old) != rb.team_slots(d) or fresh(k)):
                        raise Refused('%s: team %r launches by TEAM_HUNT with %s in it: an empty one has no ground weapon and stays home. Stock runs its Buses down an attack path (rb-data-19, smoke17: 5 of 131 left home)' % (entry['id'], n, u))
        clock_twin = None
        if fresh('teamOnCreateScript'):
            x, shape = target(n, o, 'teamOnCreateScript', oc)
            clock_twin = x if shape == CLOCK_TWIN else None
            if shape not in ON_CREATE:
                raise no_shape(n, 'teamOnCreateScript', oc, shape)
            others = named_by(o)[oc] - {oc}
            if others:
                raise Refused('%s: team %r runs %r when it forms, and %r names it: another script could switch it off or spend it' % (entry['id'], n, oc, sorted(others)[0]))
            for a in x.kids:
                nm, ps = rb.decode(after, a) if a.name == 'ScriptAction' else (None, None)
                if nm == 'TEAM_EXECUTE_SEQUENTIAL_SCRIPT':
                    sq = target(n, o, nm, ps[1][1])[1]
                    if sq not in SEQUENTIAL:
                        raise no_shape(n, nm, ps[1][1], sq)
        for k in GENERIC:
            if not fresh(k):
                continue
            x, shape = target(n, o, k, rb.team_get(d, k))
            if shape in PRIORITY:
                continue
            if shape != CLOCK_LAUNCH:
                raise no_shape(n, k, rb.team_get(d, k), shape)
            if twin is None or orders(twin) != orders(x):
                raise Refused('%s: team %r hook %s runs the one-shot %r with no twin on teamOnCreateScript (same conditions, same actions): units built after it fires never get the order (rb.launch_scripts)' % (entry['id'], n, k, rb.node_name(x)))
        if clock_twin and not any(hook_shape(rb, after, g) == CLOCK_LAUNCH and orders(g) == orders(clock_twin) for g in launches):
            raise Refused('%s: team %r runs %r when it forms, on a clock, with no one-shot generic hook of the same condition: a team formed before the tick never gets the order (rb.launch_fields)' % (entry['id'], n, oc))


def matchup_final(rb, base, scb, entries, keys, claims, Refused):
    """the finished file of one tier: per general at most one shared latch group, its three scripts canonical, then only off gates in
    name order; each off gate "GX MU Off <T>" is T's production condition and equals T's stock gate with every OR block gaining
    FLAG(gxrb_mu_<k>) == 0 for exactly the keys of the entries that claim T, in name order; no gxrb_mu_ flag is written anywhere else"""
    latch = {'%s %s' % (rb.MU_GROUP, f) for f in rb.SENSE_BASE}
    allf = rb.MU_FLAGS + (rb.MU_DECIDED,)
    bteams = {d['teamName']: d for d in rb.teams_ro(base)}
    teams = {d['teamName']: d for d in rb.teams_ro(scb)}
    shared = lambda v: v == rb.MU_GROUP or v in latch or v.startswith(rb.MU_OFF)
    for d in rb.teams_ro(scb):
        for k, _t, v in d['_e']:
            if TEAM_SCRIPT_FIELDS(k) and v and shared(v) and not (k == 'teamProductionCondition' and v == rb.MU_OFF + d['teamName']):
                raise Refused('team %r %s names the shared latch / off gate %r - only a stock team names its own off gate, as its production'
                              ' condition' % (d['teamName'], k, v))
    for o in rb.GENERALS:
        for _g, _k, x in rb.scripts_of(scb, o):
            for l in rb.leaves(x):
                ps = rb.decode(scb, l)[1]
                if l.name != 'Condition' and any(t == 'FLAG' and v in allf for t, v in ps) and rb.node_name(x) not in latch:
                    raise Refused('%s/%r writes a matchup flag - only the shared latch does' % (o, rb.node_name(x)))
                # an ENABLE / CALL / sequential run of a latch script skips its one-shot and `decided` guard
                hit = [v for t, v in ps if t in ('SCRIPT', 'SCRIPT_SUBROUTINE') and v and shared(v)]
                if hit:
                    raise Refused('%s/%r names the shared latch / off gate %r - nothing may switch, call or run it' % (o, rb.node_name(x), hit[0]))
                if l.name == 'ScriptActionFalse' and rb.node_name(x).startswith(rb.MU_OFF):
                    raise Refused('%s/%r: an off gate with a false action runs it whenever a key holds the team off' % (o, rb.node_name(x)))
        gs = [k for k in rb.owner_list(scb, o).kids if k.name == 'ScriptGroup' and rb.node_name(k) == rb.MU_GROUP]
        stray = [n for _g, _k, x in rb.scripts_of(scb, o) for n in [rb.node_name(x)] if (n in latch or n.startswith(rb.MU_OFF))
                 and not any(x is k for g in gs for k in g.kids)]
        if len(gs) > 1 or stray:
            raise Refused('%s: %d shared latch groups, %r outside it - one per general, holding the latch and the off gates' % (o, len(gs), stray[:1]))
        if not gs:
            continue
        kids = gs[0].kids
        want = [rb.canon(scb, x) for x in rb.matchup_latch_scripts(scb)]
        if [rb.canon(scb, x) for x in kids[:3]] != want:
            raise Refused('%s: the shared latch %r is not the canonical one (rb.matchup_latch_scripts)' % (o, rb.MU_GROUP))
        offs = [rb.node_name(x) for x in kids[3:]]
        if any(k.name != 'Script' or not n.startswith(rb.MU_OFF) for k, n in zip(kids[3:], offs)) or offs != sorted(offs):
            raise Refused('%s: the shared latch group holds %r after the latch - only off gates, in name order' % (o, offs))
        for x, n in zip(kids[3:], offs):
            tn = n[len(rb.MU_OFF):]
            t, t0 = teams.get(tn), bteams.get(tn)
            if t is None or t0 is None or rb.team_get(t, 'teamOwner') != o or rb.team_get(t, 'teamProductionCondition') != n:
                raise Refused('%s/%r: the off gate of no stock team of %s that waits on it' % (o, n, o))
            ks = sorted({keys[e['id']] for e in entries if o in e['generals'] and keys.get(e['id']) and tn in claims[e['id']]['teams']})
            hits = [s0 for _g, _k, s0 in rb.scripts_of(base, o) if rb.node_name(s0) == rb.team_get(t0, 'teamProductionCondition')]
            assert len(hits) == 1, (o, tn)
            s0 = hits[0]
            exp = [(b.name, [rb.decode(base, l) for l in b.kids] + ([('FLAG', [('FLAG', rb.mu_flag(k)), ('BOOLEAN', 0)]) for k in ks]
                                                                    if b.name == 'OrCondition' else []))
                   if b.kids is not None else (b.name, rb.decode(base, b)) for b in s0.kids]
            keeps = [b for b in x.kids if b.name == 'OrCondition' and rb.is_keep(scb, b)]
            rest = [b for b in x.kids if not any(b is k for k in keeps)]
            got = [(b.name, [rb.decode(scb, l) for l in b.kids]) if b.kids is not None else (b.name, rb.decode(scb, b)) for b in rest]
            if not ks or got != exp or rb.mss.script_flags(x)[:2] != rb.mss.script_flags(s0)[:2]:
                raise Refused('%s/%r is not %r\'s stock gate %r with FLAG(gxrb_mu_<k>) == 0 for the keys %s of the rules that claim it'
                              % (o, n, tn, rb.node_name(s0), ks))
            nor = len([b for b in rest if b.name == 'OrCondition'])
            if [b.name for b in x.kids[nor:nor + len(keeps)]] != ['OrCondition'] * len(keeps) or \
                    any(not (a is b) for a, b in zip(x.kids[nor:nor + len(keeps)], keeps)):
                raise Refused('%s/%r: keep blocks sit right after the OFF blocks, before the actions (rb.matchup_keep)' % (o, n))
            keep_blocks(rb, base, scb, o, n, tn, s0, keeps, ks, entries, keys, claims, Refused)


def keep_blocks(rb, base, scb, o, n, tn, s0, keeps, ks, entries, keys, claims, Refused):
    """rb-data-17 (the lead's stock-copy ruling): the KEEP blocks of the off gate `n` of stock team `tn` - each re-enables the STOCK team
    under one key. Each is exactly a stock OR block of `s0`, then at once FLAG(gxrb_mu_<K>) == 1 for a key K of the rules that claim `tn`, then
    at least one leaf of that rule's own state: FLAG(f) == 0 / 1 for a flag only scripts that rule claims write, or an own object count
    (PLAYER_HAS_OBJECT_COMPARISON of <This Player>); canonical order (rb.matchup_keep), none twice. Anything else is refused: a keep
    block is the one place a rule turns a stock team back on, and its key ON is what keeps two keys' rules apart"""
    reprs = [repr([rb.decode(scb, l) for l in b.kids]) for b in keeps]
    if reprs != sorted(reprs) or len(set(reprs)) != len(reprs):
        raise Refused('%s/%r: one keep block twice or out of canonical order (rb.matchup_keep writes them)' % (o, n))
    stock = [[rb.decode(base, l) for l in b.kids] for b in s0.kids if b.name == 'OrCondition']
    allf = {rb.mu_flag(k) for k in rb.SENSE_BASE} | {rb.MU_DECIDED}
    for b in keeps:
        got = [rb.decode(scb, l) for l in b.kids]
        pre = [sb for sb in stock if got[:len(sb)] == sb]
        if not pre:
            raise Refused('%s/%r: a keep block without the leaves of a stock block of %r: %r' % (o, n, rb.node_name(s0), got))
        tail = got[len(max(pre, key=len)):]
        mu = [l for l in tail if l[0] == 'FLAG' and l[1][0][1] in allf]
        on = [k for k in ks if mu == [('FLAG', [('FLAG', rb.mu_flag(k)), ('BOOLEAN', 1)])]]
        if len(on) != 1 or tail[0] != mu[0]:
            raise Refused('%s/%r: a keep block must hold FLAG(gxrb_mu_<K>) == 1 for one key K of the rules that claim %r (%s), right after the'
                          ' stock block, and no other latch flag: %r' % (o, n, tn, ks, tail))
        es = [e for e in entries if o in e['generals'] and keys.get(e['id']) == on[0] and tn in claims[e['id']]['teams']]
        assert len(es) == 1, (o, tn, on[0], [e['id'] for e in es])
        rid, c = es[0]['id'], claims[es[0]['id']]
        own = [l for l in tail if l not in mu]
        if not own:
            raise Refused('%s/%r: a keep block of %s with no own leaf keeps %r stock for the whole matchup - such a rule does not switch it'
                          % (o, n, rid, tn))
        mine = {x[1] for x in c['scripts'] if x[0] == o} | {m for g in [x[1] for x in c['groups'] if x[0] == o]
                                                             for m in groups_of(rb, scb, o).get(g, [])}
        for nm, ps in own:
            if nm == 'FLAG' and len(ps) == 2 and ps[0][0] == 'FLAG' and ps[1] in (('BOOLEAN', 0), ('BOOLEAN', 1)):
                w = {rb.node_name(s) for _g, _k, s in rb.scripts_of(scb, o) for l in rb.leaves(s)
                     if l.name != 'Condition' and ('FLAG', ps[0][1]) in rb.decode(scb, l)[1]}
                if w and w <= mine:
                    continue
                raise Refused('%s/%r: a keep block of %s reads the flag %r, which %s - a keep block reads only flags its own rule alone'
                              ' writes' % (o, n, rid, ps[0][1], 'nothing writes' if not w else 'is written by %r' % sorted(w - mine)[:1]))
            if nm == 'PLAYER_HAS_OBJECT_COMPARISON' and ps[:1] == [('SIDE', rb.SIDE_ME)]:
                continue
            raise Refused('%s/%r: a keep block of %s reads %r - only the rule\'s own flags and own object counts' % (o, n, rid, (nm, ps)))
    # a keep block and a variant of the same stock team (any rule's team the build table records as its copy: its name and
    # place say nothing) are never producible together - else both waves build. Proven per pair of OR blocks by one leaf of each
    # that cannot both hold (excludes()). A variant with no gate script has no OR block: the engine never produces it (Team.cpp:1155)
    for d in rb.teams_ro(scb):
        v = d['teamName']
        if rb.BUILT.get(v) != tn:
            continue
        pc = rb.team_get(d, 'teamProductionCondition')
        hits = [x for _g, _k, x in rb.scripts_of(scb, rb.team_get(d, 'teamOwner')) if rb.node_name(x) == pc]
        vbs = [[rb.decode(scb, l) for l in b.kids] for x in hits[:1] for b in x.kids if b.name == 'OrCondition']
        for b in keeps:
            kb = [rb.decode(scb, l) for l in b.kids]
            for vb in vbs:
                if not any(excludes(rb, x, y) for x in kb for y in vb):
                    raise Refused('%s/%r: its keep block %r and the variant %r (production condition %r) can both be producible - no leaf of'
                                  ' one excludes a leaf of the other' % (o, n, kb, v, pc))


def _span(op, k, rb):
    """the counts an own-object comparison `count op k` admits, as (lo, hi); None when not an interval"""
    return {rb.LT: (0, k - 1), rb.LE: (0, k), rb.EQ: (k, k), rb.GE: (k, float('inf')), rb.GT: (k + 1, float('inf'))}.get(op)


def excludes(rb, a, b):
    """two decoded condition leaves that cannot both hold: one flag at both values, two different shared latch keys ON (the canonical
    latch sets one at most), or two own-object comparisons of one type with disjoint counts"""
    (na, pa), (nb, pb) = a, b
    if na == nb == 'FLAG' and len(pa) == len(pb) == 2 and pa[0][0] == pb[0][0] == 'FLAG' and pa[1][0] == pb[1][0] == 'BOOLEAN':
        on = lambda p: p[1][1] != 0                     # ScriptEngine::evaluateFlag reads value != 0: BOOLEAN 2 is ON
        if pa[0] == pb[0]:
            return on(pa) != on(pb)
        mu = {rb.mu_flag(k) for k in rb.SENSE_BASE}
        return {pa[0][1], pb[0][1]} <= mu and on(pa) and on(pb)
    if na == nb == 'PLAYER_HAS_OBJECT_COMPARISON' and len(pa) == len(pb) == 4 and pa[0] == pb[0] and pa[3] == pb[3]:
        x, y = _span(pa[1][1], pa[2][1], rb), _span(pb[1][1], pb[2][1], rb)
        return x is not None and y is not None and (x[1] < y[0] or y[1] < x[0])
    return False


def final_claims_overlap(rb, scb, entries, claims, Refused, keys=None, stock_teams=()):
    """two entries of one tier for one player whose CLAIMS meet once a claimed group counts every member it has at the END of the
    compile - a script another module added into it included; two MATCHUP keys that differ may share what
    mu_shareable allows (rb-data-13)"""
    groups = {o: groups_of(rb, scb, o) for o in rb.GENERALS}
    keys = keys or {}

    def cover(e, gens):
        c = claims[e['id']]
        out = {tuple(x) for x in c['scripts'] + c['groups'] if x[0] in gens}
        return out | {(o, m) for o, g in c['groups'] if o in gens for m in groups[o].get(g, [])}
    for i, a in enumerate(entries):
        for b in entries[i + 1:]:
            shared = set(a['generals']) & set(b['generals'])
            if a['id'] != b['id'] and shared:
                hit = sorted(cover(a, shared) & cover(b, shared))
                why = overlap_refusal(rb, a['id'], b['id'], keys.get(a['id']), keys.get(b['id']), hit, stock_teams)
                if why:
                    raise Refused(why.replace(': CLAIMS overlap on', ': CLAIMS overlap (once the groups\' final members count) on', 1))


def shared_tables(rb, scb, base_ctx):
    """a NEW attack-priority table is ONE global object shared by every player of its general, friend or foe (set names are not
    qualified, findAttackInfo resolves one table, applying hands units a pointer to it): it may be written only behind conditions that
    are the same for every player - _GX_CLOCK thresholds, CONDITION_TRUE, a stock table definer's own (startup-timer) conditions - by a
    script nothing switches, calls or hooks; otherwise one player's state moves another's targets"""
    Refused = base_ctx['Refused']
    for o in rb.GENERALS:
        writers = []
        for _g, _k, s in rb.scripts_of(scb, o):
            sets = sorted({v for l in leaf_list(rb, scb, s, ('ScriptAction', 'ScriptActionFalse')) for nm, ps in [rb.decode(scb, l)]
                           if nm in PRIORITY_DEFINE for t, v in ps if t == 'ATTACK_PRIORITY_SET' and v not in base_ctx['sets']})
            if sets:
                writers.append((_g, s, sets))
        if not writers:
            continue
        for _g, s, sets in writers:
            for l in leaf_list(rb, scb, s):
                nm, ps = rb.decode(scb, l)
                if nm == 'CONDITION_TRUE' or (nm == 'COUNTER' and ('COUNTER', GX_CLOCK) in ps) or rb.canon(scb, l) in base_ctx['definer_conds']:
                    continue
                raise Refused('%s/%r writes the new attack-priority table %r behind a player-dependent condition (%s): every player of'
                              ' %s shares that table - only the clock or the stock startup trigger may time it'
                              % (o, rb.node_name(s), sets[0], nm, o))
        names = {rb.node_name(s) for _g, s, _x in writers} | {rb.node_name(g) for g, _s, _x in writers if g is not None}
        for _g, _k, x in rb.scripts_of(scb, o):
            hit = sorted((refs_of(rb, scb, x) & names) - {rb.node_name(x)})
            if hit:
                raise Refused('%s/%r names %r, which writes a new shared attack-priority table: a player-dependent switch or call'
                              ' (every player of %s shares the table)' % (o, rb.node_name(x), hit[0], o))
        for d in rb.teams_ro(scb):
            hit = sorted(v for k, _t, v in d['_e'] if TEAM_SCRIPT_FIELDS(k) and v in names)
            if hit and rb.team_get(d, 'teamOwner') == o:
                raise Refused('team %r hooks %r, which writes a new shared attack-priority table: a player-dependent trigger' % (d['teamName'], hit[0]))


def whole_file(rb, scb, base_ctx):
    """checks on the finished tier: every recorded read of stock state is still clean now that every module has written (a later
    module can taint an earlier reader); a new priority set name lives in one general only"""
    Refused = base_ctx['Refused']
    taints = {}
    for o, v, rid, copied in base_ctx['reads']:
        if o not in taints:
            taints[o] = state_taint(rb, scb, o)[0]
        if v in taints[o] and not copied:
            raise Refused('%s reads %r in %s, which another rule of this compile made carry an enemy read (fog): %s'
                          % (rid, v, o, taints[o][v]))
    check_names(rb, [scb], base_ctx, 'this compile')
    shared_tables(rb, scb, base_ctx)
    # copied stock gates, on the finished file: a team naming one as its production condition is a stock team whose OWN old condition
    # the copy keeps, and nothing else names it - an earlier module's forward reference included
    if base_ctx['copies']:
        bteams = {d['teamName']: d for d in rb.teams_ro(base_ctx['base_scb'])}
        for d in rb.teams_ro(scb):
            o = rb.team_get(d, 'teamOwner')
            for k, _t, v in d['_e']:
                if TEAM_SCRIPT_FIELDS(k) and (o, v) in base_ctx['copies']:
                    b = bteams.get(d['teamName'])
                    if not (k == 'teamProductionCondition' and b is not None and rb.team_get(b, k) in base_ctx['copies'][(o, v)]):
                        raise Refused('team %r: %s names %s/%r, a copied stock enemy gate kept only for %s - it may stand in for no other'
                                      ' condition' % (d['teamName'], k, o, v, sorted(base_ctx['copies'][(o, v)])))
        for o in sorted({o for o, _x in base_ctx['copies']}):
            for _g, _k, s in rb.scripts_of(scb, o):
                hit = sorted(x for x in refs_of(rb, scb, s) if (o, x) in base_ctx['copies'] and x != rb.node_name(s))
                if hit:
                    raise Refused('%s/%r names %r, a copied stock enemy gate - it may only stand in for its team\'s own production'
                                  ' condition' % (o, rb.node_name(s), hit[0]))
    # the cached tech-building test, wherever a module changed its script or newly named it - a later module can postpone a leaf an
    # earlier one left alone
    for o in rb.GENERALS:
        idx = None
        for _g, _k, s in rb.scripts_of(scb, o):
            tl = [l for l in leaf_list(rb, scb, s) if rb.decode(scb, l)[0] == TECH_NEAR]
            n = rb.node_name(s)
            if tl and (base_ctx['tech_base'][o].get(n) != rb.canon(scb, s) or (o, n) in base_ctx['new_named'] or
                       (_g is not None and (o, rb.node_name(_g)) in base_ctx['new_named'])):
                idx = idx or Index(rb, scb, [o])
                key = next(k for k, x in idx.scripts.items() if x is s)
                for l in tl:
                    why = tech_near_ok(rb, idx, scb, key, s, l)
                    if why:
                        raise Refused('%s/%r: %s' % (o, n, why))
    where = priority_definitions(rb, scb, base_ctx['sets'])
    for v, defs in sorted(where.items()):
        owners = set(defs)
        if len(owners) > 1:
            raise Refused('the new attack-priority set %r is defined in more than one general (%s): names are GLOBAL, so each general'
                          ' needs its own name' % (v, sorted(owners)))
    return dict((v, sorted(d)) for v, d in where.items())


def merged_priority_sets(rb, views, base_sets, Refused):
    """the tier-merged file, as each tier reads it (views: {tier: scb}): players of different tiers run their marks in one match, so a
    global set name has one owner and one definition"""
    seen = {}
    for t, scb in sorted(views.items()):
        for v, defs in priority_definitions(rb, scb, base_sets).items():
            for o, d in defs.items():
                prev = seen.setdefault(v, (o, d, t))
                if prev[0] != o:
                    raise Refused('the new attack-priority set %r is defined in more than one general across tiers (%s, %s)' % (v, prev[0], o))
                if prev[1] != d:
                    raise Refused('the new attack-priority set %r is defined differently by %s and %s for %s - set names are global and both'
                                  ' tiers can play one match: put the tier into its name' % (v, prev[2], t, o))


def base_context(rb, scb, Refused, owners):
    """facts of the shipped file every module is judged against"""
    ctx = {'Refused': Refused, 'owners': owners, 'gift': {}, 'dup': {}, 'sets': set(), 'seen_sets': set(), 'claims': {}, 'taint': {},
           'base_scb': scb, 'reads': [], 'definers': {}, 'list_names': {}, 'tech_base': {}, 'enemy_reach': {}, 'run_reach': {}, 'definer_groups': {}, 'frozen_inputs': {}, 'definer_conds': set(),
           'groups': {}, 'new_named': set(), 'copies': {}, 'vers': collections.defaultdict(set), 'pool': {}}
    ctx['kids_of'] = collections.defaultdict(set)
    for lst in rb.lists(scb):
        for x in walk(lst):
            ctx['vers'][x.name].add(x.ver)
            ctx['kids_of'][x.name] |= {k.name for k in x.kids or ()}
    for o in rb.GENERALS:
        ctx['gift'][o] = gift_reach(rb, scb, o)
        ctx['taint'][o] = state_taint(rb, scb, o)
        ctx['list_names'][o] = list_names(rb, scb, o)
        ctx['enemy_reach'][o] = enemy_reach(rb, scb, o, ctx['taint'][o][0])
        ctx['run_reach'][o] = enemy_reach(rb, scb, o, ctx['taint'][o][0], writes=True)
        ctx['groups'][o] = groups_of(rb, scb, o)
        ctx['pool'][o] = pool_names(rb, scb, o)
        ctx['tech_base'][o] = {rb.node_name(x): rb.canon(scb, x) for _g, _k, x in rb.scripts_of(scb, o)
                               if any(rb.decode(scb, l)[0] == TECH_NEAR for l in leaf_list(rb, scb, x))}
        occ, gocc = collections.Counter(), collections.Counter()
        for k in rb.owner_list(scb, o).kids:
            if k.name == 'ScriptGroup':
                gocc[rb.node_name(k)] += 1
                for x in k.kids:
                    if x.name == 'Script':
                        occ[(o, gkey(rb.node_name(k), gocc[rb.node_name(k)] - 1), rb.node_name(x))] += 1
            elif k.name == 'Script':
                occ[(o, None, rb.node_name(k))] += 1
        ctx['dup'].update({k: v for k, v in occ.items() if v > 1})
    names = ctx['names'] = qualified_names(rb, scb)

    def alias(kind, v):
        """an existing name of this kind that equals v once one of them gets a decimal start index appended"""
        kind = 'SCRIPT' if kind == 'SCRIPT_SUBROUTINE' else kind
        if v in names[kind]:
            return None
        for i in range(len(v) - 1, 0, -1):            # v = existing + digits
            if not v[i:].isdigit():
                break
            if v[:i] in names[kind]:
                return v[:i]
        for n in names[kind]:                          # existing = v + digits
            if n.startswith(v) and n[len(v):].isdigit():
                return n
        return None
    ctx['alias'] = alias
    for _l, _g, _n, _i, s in rb.mss.walk_scripts(scb):
        for l in leaf_list(rb, scb, s, ('ScriptAction', 'ScriptActionFalse')):
            nm, ps = rb.decode(scb, l)
            if nm in PRIORITY_DEFINE or nm in ('TEAM_APPLY_ATTACK_PRIORITY_SET', 'NAMED_APPLY_ATTACK_PRIORITY_SET'):
                ctx['sets'] |= {v for t, v in ps if t == 'ATTACK_PRIORITY_SET'}
    for o in rb.GENERALS:                              # the stock scripts that define a stock (shared) table
        ctx['definers'][o] = {}
        for _g, _k, x in rb.scripts_of(scb, o):
            for l in leaf_list(rb, scb, x, ('ScriptAction', 'ScriptActionFalse')):
                nm, ps = rb.decode(scb, l)
                if nm in PRIORITY_DEFINE:
                    ctx['definers'][o].setdefault(rb.node_name(x), next(v for t, v in ps if t == 'ATTACK_PRIORITY_SET'))
        ctx['definer_groups'][o] = {g: ctx['definers'][o][m] for g, ms in ctx['groups'][o].items() for m in ms if m in ctx['definers'][o]}
        ctx['definer_conds'] |= {rb.canon(scb, l) for _g, _k, x in rb.scripts_of(scb, o) if rb.node_name(x) in ctx['definers'][o]
                                 for l in leaf_list(rb, scb, x)}
        frozen = set(ctx['gift'][o]) | set(ctx['definers'][o])
        frozen |= {m for g, ms in ctx['groups'][o].items() if g in frozen for m in ms}
        fi = frozen_inputs(rb, scb, o, frozen)
        fi.update({g: 'holds %r, which %s' % (m, fi[m]) for g, ms in ctx['groups'][o].items() for m in ms if m in fi and g not in fi})
        ctx['frozen_inputs'][o] = fi
    return ctx
