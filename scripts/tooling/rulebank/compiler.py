# GeneralsX: rule bank, DATA half: the ruleset compiler (2.2.9,
# CONTRACT v1 in 2.6). A ruleset file (JSON, schema gx-ruleset/1) compiles to a loose SkirmishScripts.scb plus a manifest:
#
#   base   = the recipe's own output (make-skirmish-scripts.py build_base, BASE_SHA256) - never edited in place
#   DATA   = one module per rule, scripts/tooling/rulebank/data/<RULE_ID>.py: apply(scb, scope, params), CLAIMS, activation(obs, me)
#   ENGINE = one-shot AI_PLAYER_GX_RULE* scripts per in-scope general (ids and keys from the bank's rule_ids.json)
#   last   = the tier marks (tier_gate on): stock entries stay byte-identical, every change rides a GXT.<tiers>.<name> entry that the
#            engine's tier filter swaps in for Sentient / Brutal players only (Hard plays today's lists)
#
# An empty ruleset gives the shipped bytes exactly. Entries apply in registry order, so a reordered file gives the same bytes.
# Called from make-skirmish-scripts.py --ruleset; stdlib only; every file is loaded by path (python3 -I safe).
import hashlib, importlib.util, json, os, sys

sys.dont_write_bytecode = True     # modules are loaded by path from the repo: leave no __pycache__ beside them

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLING = os.path.dirname(HERE)
DATA_DIR = os.path.join(HERE, 'data')
REGISTRY = os.path.join(HERE, 'registry.json')
PARKED = os.path.join(HERE, 'parked.json')      # rule id -> why it was parked (the registry keeps the tag, not the note)
RULE_IDS = os.path.join(TOOLING, '..', 'ai', 'rulebank', 'rule_ids.json')    # rb-core's, generated from GXRuleTable.inl
SCHEMA = 'gx-ruleset/1'
MANIFEST_SCHEMA = 'gx-ruleset-manifest/1'
TIERS = ('SENTIENT', 'BRUTAL')               # the levels a ruleset may target, in mark order; HARD keeps the registered defaults
RULESET_KEYS = {'schema', 'base', 'tier_gate', 'rules', 'reopen'}
ENTRY_KEYS = {'id', 'on', 'generals', 'tiers', 'params', 'refs'}
CODE_FILES = ('compiler.py', 'scbx.py', 'guards.py', 'tiermark.py', 'obs.py')


class Refused(Exception):
    """the ruleset (or a module) asks for something the compiler will not build; the text says what and why"""


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def load_recipe():
    return _load('gx_make_skirmish_scripts', os.path.join(TOOLING, 'make-skirmish-scripts.py'))


SCBX = _load('gx_rulebank_scbx', os.path.join(HERE, 'scbx.py'))
GUARDS = _load('gx_rulebank_guards', os.path.join(HERE, 'guards.py'))
TIERMARK = _load('gx_rulebank_tiermark', os.path.join(HERE, 'tiermark.py'))
OBS = _load('gx_rulebank_obs', os.path.join(HERE, 'obs.py'))


def load_registry(path=REGISTRY):
    return json.load(open(path, encoding='utf-8'))


def load_parked(path=PARKED):
    return json.load(open(path, encoding='utf-8'))


def load_rule_ids(path=RULE_IDS):
    """rb-core's rule_ids.json, or None when this tree has no rule bank yet"""
    return json.load(open(path, encoding='utf-8')) if os.path.isfile(path) else None


def fnv1a32(b):
    """the engine's [SIMID] loose crc= (SimulationId.cpp fnv1a from FNV_SEED over the file bytes)"""
    h = 2166136261
    for x in b:
        h = ((h ^ x) * 16777619) & 0xFFFFFFFF
    return h


def sha256(b):
    return hashlib.sha256(b).hexdigest()


def base_bytes(mss, search):
    """the shipped scb: the recipe's output from the player's own retail copy, checked against its pin"""
    out = mss.build_base(search) if mss.BASE_SHA256 != 'TBD' else mss.generate(mss.pinned_source(search))[0]   # never the promoted default
    return out


# --------------------------------------------------------------------------- the ruleset
def parse_ruleset(data):
    try:
        rs = json.loads(data.decode('utf-8'))
    except (UnicodeDecodeError, ValueError) as e:
        raise Refused('ruleset: not JSON (%s)' % e)
    if not isinstance(rs, dict):
        raise Refused('ruleset: not a JSON object')
    if rs.get('schema') != SCHEMA:
        raise Refused('ruleset: schema %r, want %r' % (rs.get('schema'), SCHEMA))
    bad = sorted(set(rs) - RULESET_KEYS)
    if bad:
        raise Refused('ruleset: unknown key(s) %s' % bad)
    if not isinstance(rs.get('rules', []), list):
        raise Refused('ruleset: rules is not a list')
    return rs


def resolve_gate(cli, rs):
    f = rs.get('tier_gate')
    for v, where in ((cli, '--tier-gate'), (f, 'tier_gate')):
        if v is not None and v not in ('on', 'off'):
            raise Refused('%s %r: on or off' % (where, v))
    if cli is not None and f is not None and cli != f:
        raise Refused('tier gate: --tier-gate %s but the ruleset says %s' % (cli, f))
    return cli or f or 'on'


def _int(v):
    return isinstance(v, int) and not isinstance(v, bool)


def reopened(rs, registry):
    """-> {rule id: reason} for the ruleset's `reopen` list. A PARKED rule (registry tag) is refused unless this list names it with a
    reason; nothing outside the ruleset (no environment variable, no flag) can stand in. Each reopen opens one rule that the ruleset
    lists: a reopen of a live rule, an unknown rule, or one the rules never name is refused, so a stale or mistyped one is no free pass."""
    lst = rs.get('reopen', [])
    if not isinstance(lst, list):
        raise Refused('ruleset: reopen is not a list of {"id", "reason"}')
    R, named = registry['rules'], {e.get('id') for e in rs.get('rules', []) if isinstance(e, dict) and isinstance(e.get('id'), str)}
    out = {}
    for i, r in enumerate(lst):
        where = 'ruleset reopen %d' % (i + 1)
        if not isinstance(r, dict):
            raise Refused('%s: not an object' % where)
        bad = sorted(set(r) - {'id', 'reason'})
        if bad:
            raise Refused('%s: unknown key(s) %s (one rule and one reason, no wildcards)' % (where, bad))
        rid = r.get('id')
        if not isinstance(rid, str) and rid is not None:
            raise Refused('%s: id must be a rule id string' % where)
        if rid not in R:
            raise Refused('%s: id %r is not in the registry' % (where, rid) if rid is not None else '%s: names no rule (id)' % where)
        why = r.get('reason')
        if not isinstance(why, str) or len(why.strip()) < 8:
            raise Refused('%s (%s): reason must say why the parked rule is reopened (a string of 8 characters or more)' % (where, rid))
        if 'PARKED' not in R[rid].get('tags', ()):
            raise Refused('%s (%s): not parked - nothing to reopen' % (where, rid))
        if rid in out:
            raise Refused('%s (%s): reopened twice' % (where, rid))
        if rid not in named:
            raise Refused('%s (%s): the ruleset does not name it in its rules' % (where, rid))
        out[rid] = why.strip()
    return out


def validate(rs, registry, gate, rule_ids):
    """-> resolved entries, registry order: {id, kind, on, generals, tiers, params (defaults filled), refs}"""
    R = registry['rules']
    reopen = reopened(rs, registry)
    out = []
    for i, e in enumerate(rs.get('rules', [])):
        where = 'ruleset entry %d' % (i + 1)
        if not isinstance(e, dict):
            raise Refused('%s: not an object' % where)
        bad = sorted(set(e) - ENTRY_KEYS)
        if bad:
            raise Refused('%s: unknown key(s) %s' % (where, bad))
        rid = e.get('id')
        if not isinstance(rid, str) or rid not in R:
            raise Refused('%s: %r is not in the registry' % (where, rid))
        reg = R[rid]
        where = '%s (%s)' % (where, rid)
        if 'PARKED' in reg.get('tags', ()) and rid not in reopen:
            try:
                why = load_parked().get(rid, '(no reason recorded in parked.json)')
            except OSError:
                why = '(parked.json is missing)'
            raise Refused('%s: %s is PARKED - %s. A parked rule compiles only when this ruleset reopens it: '
                          '"reopen": [{"id": "%s", "reason": "<why>"}]' % (where, rid, why, rid))
        if reg.get('capability'):
            raise Refused('%s: a capability of the binary, not a rule (no on/off)' % where)
        if reg['scope'] is None:
            raise Refused('%s: the registry could not parse its scope' % where)
        if not isinstance(e.get('on'), bool):
            raise Refused("%s: 'on' must be true or false" % where)
        gens = e.get('generals', reg['scope'])
        if not isinstance(gens, list) or not gens or len(set(gens)) != len(gens):
            raise Refused('%s: generals must be a non-empty list without repeats' % where)
        outside = [g for g in gens if g not in reg['scope']]
        if outside:
            raise Refused('%s: generals %s outside its scope %s' % (where, outside, reg['scope']))
        allowed = [t for t in reg['tiers'] if t in TIERS]
        tiers = e.get('tiers', allowed)
        if not isinstance(tiers, list) or not tiers or len(set(tiers)) != len(tiers):
            raise Refused('%s: tiers must be a non-empty list without repeats' % where)
        if 'HARD' in tiers:
            raise Refused('%s: tiers name HARD - Hard keeps the registered defaults (plan 2.8); tune SENTIENT/BRUTAL' % where)
        if [t for t in tiers if t not in allowed]:
            raise Refused('%s: tiers %s not allowed for this rule (allowed %s)' % (where, tiers, allowed))
        params = e.get('params', {})
        if not isinstance(params, dict):
            raise Refused('%s: params must be an object' % where)
        full = {}
        for name, box in reg['params'].items():
            full[name] = box['default']
        for name, v in params.items():
            if name not in reg['params']:
                raise Refused('%s: unknown param %r (known: %s)' % (where, name, sorted(reg['params'])))
            if not _int(v):
                raise Refused('%s: param %s=%r is not an integer (CONTRACT v1: integers only)' % (where, name, v))
            box = reg['params'][name]
            if not box['lo'] <= v <= box['hi']:
                raise Refused('%s: param %s=%d outside its box [%d..%d]' % (where, name, v, box['lo'], box['hi']))
            full[name] = v
        refs = e.get('refs', {})
        if refs and reg['kind'] == 'DATA':
            raise Refused('%s: refs are for ENGINE rules (a DATA rule takes integer params only)' % where)
        if not isinstance(refs, dict) or not all(isinstance(k, str) and isinstance(v, str) for k, v in refs.items()):
            raise Refused('%s: refs must map names to strings' % where)
        if reg['kind'] in ('ENGINE', 'VISION'):
            engine_entry_checks(where, rid, e, params, refs, tiers, gate, rule_ids, reg['params'])
        out.append({'id': rid, 'kind': reg['kind'], 'on': e['on'], 'generals': [g for g in reg['scope'] if g in gens],
                    'explicit': sorted(params), 'explicit_generals': 'generals' in e,
                    'tiers': [t for t in TIERS if t in tiers], 'params': full, 'refs': dict(sorted(refs.items())),
                    'order': reg['order'], 'capabilities': list(reg.get('capabilities', []))})
    by = {}
    for e in out:
        by.setdefault(e['id'], []).append(e)
    # one rule twice: told apart by tier, or (07/11, rb-tune c6 core #1) by DISJOINT generals - every entry is applied to its own generals only (DATA apply, ENGINE
    # one script per general), so a direct game can seat one rule at two settings. A general named by two entries of one rule is still refused: in a shared tier, and always
    # under --tier-gate off, where there are no tiers to tell them apart
    for rid, es in by.items():
        if len(es) < 2:
            continue
        seen = {}
        for e in es:
            for g in e['generals']:
                if g in seen and (gate == 'off' or seen[g] & set(e['tiers'])):
                    if gate == 'off':
                        raise Refused('%s twice for %s: under --tier-gate off there are no tiers to tell the entries apart' % (rid, g))
                    raise Refused('%s twice for %s with overlapping tiers %s' % (rid, g, sorted(seen[g] & set(e['tiers']))))
            for g in e['generals']:
                seen.setdefault(g, set()).update(e['tiers'])
    out.sort(key=lambda e: (e['order'], [TIERS.index(t) for t in e['tiers']]))
    return out


GX_ACTIONS = ('AI_PLAYER_GX_RULE', 'AI_PLAYER_GX_RULE_TEMPLATE', 'AI_PLAYER_GX_RULE_TEAM')   # CONTRACT v1.1, appended once by rb-core
ENGINE_GROUP = 'GX Rule %s'                     # one group, one Hard-only one-shot script per in-scope general (the opt_in_step idiom)


def engine_entry_checks(where, rid, e, params, refs, tiers, gate, rule_ids, reg_params):
    if rule_ids is None:
        raise Refused('%s: an ENGINE rule needs the bank\'s rule_ids.json (this tree has none: --rule-ids)' % where)
    if rid not in rule_ids:
        raise Refused('%s: not in rule_ids.json - the target binary does not know this rule' % where)
    ri = rule_ids[rid]
    for name, box in reg_params.items():
        eb = ri.get('params', {}).get(name)
        if eb is None:
            raise Refused('%s: registry param %r has no key in rule_ids.json - the target bank does not take it (re-export the registry)'
                          % (where, name))
        if eb['default'] != box['default']:
            raise Refused('%s: param %s has default %d in the registry but %d in rule_ids.json - an omitted param would run the engine\'s'
                          ' number while the manifest records the registry\'s; re-export the registry' % (where, name, box['default'], eb['default']))
    for name, v in params.items():
        if name not in ri.get('params', {}):
            raise Refused('%s: param %r has no key in rule_ids.json' % (where, name))
        box = ri['params'][name]
        if not box['lo'] <= v <= box['hi']:
            raise Refused('%s: param %s=%d outside the engine\'s box [%d..%d]' % (where, name, v, box['lo'], box['hi']))
    for name, v in refs.items():
        if not isinstance(v, str) or '\x00' in v:
            raise Refused('%s: ref %s=%r holds a NUL (or is not a string) - the engine\'s C string ends there (Codex rbd-r4-lensA 3)'
                          % (where, name, v))
        if name not in ri.get('refs', {}):
            raise Refused('%s: unknown ref %r (rule_ids.json has %s)' % (where, name, sorted(ri.get('refs', {}))))
    if gate == 'on' and [t for t in tiers if t not in ri.get('tiers', [])]:
        raise Refused('%s: tiers %s, but rule_ids.json allows %s' % (where, tiers, ri.get('tiers')))


def engine_lines(mss, scb, e, rule_ids, action_ints, tiers):
    """write (or rewrite) each in-scope general's 'GX Rule <id>' group; returns the manifest lines"""
    rb = SCBX
    ints = dict(action_ints) if action_ints is not None else {k: v for k, v in rb.enums()[1].items() if k in GX_ACTIONS}
    for a in GX_ACTIONS:
        if a not in ints:
            raise Refused('%s: the engine has no %s action (Scripts.h on this tree) - ENGINE rules need the rule bank' % (e['id'], a))
    ri = rule_ids[e['id']]
    me = ('SIDE', rb.SIDE_ME)
    if e['on']:
        acts = [('AI_PLAYER_GX_RULE', 0, 1)] + [('AI_PLAYER_GX_RULE', ri['params'][n]['key'], e['params'][n])
                                               for n in sorted(e['explicit'], key=lambda n: ri['params'][n]['key'])]
        acts += [('AI_PLAYER_GX_RULE_TEMPLATE' if ri['refs'][n]['type'] == 'TEMPLATE' else 'AI_PLAYER_GX_RULE_TEAM', ri['refs'][n]['key'],
                  e['refs'][n]) for n in sorted(e['refs'], key=lambda n: ri['refs'][n]['key'])]
    else:
        acts = [('AI_PLAYER_GX_RULE', 0, 0)]
    out = []
    for o in e['generals']:
        lst = rb.owner_list(scb, o)
        name = ENGINE_GROUP % e['id']
        have = [k for k in lst.kids if k.name == 'ScriptGroup' and rb.node_name(k) == name]
        if len(have) > 1:
            raise Refused('%s: %s has %d groups named %r' % (e['id'], o, len(have), name))
        if not e['on'] and not have:
            continue                        # absent = off: nothing shipped to switch off
        leaves = [rb.cond(scb, 'CONDITION_TRUE')]
        for a, key, v in acts:
            last = ('TEXT_STRING', v) if a.endswith('TEMPLATE') else ('TEAM', v) if a.endswith('TEAM') else ('INT', v)
            leaves.append(rb.call_leaf(scb, 'ScriptAction', a, ints[a], [me, ('INT', ri['id']), ('INT', key), last]))
        script = rb.new_script(scb, name, [leaves[:1]], leaves[1:], oneshot=True)
        if have:
            have[0].kids = [script]
        else:
            lst.kids.append(rb.new_group(name, [script]))
        out.append({'general': o, 'rule': e['id'], 'id': ri['id'], 'tiers': list(tiers), 'actions': [[a, k, v] for a, k, v in acts]})
    return out


def load_module(rid, dirs):
    for d in dirs:
        p = os.path.join(d, rid + '.py')
        if os.path.isfile(p):
            spec = importlib.util.spec_from_file_location('gx_rulebank_data_' + rid.replace('.', '_'), p)
            mod = importlib.util.module_from_spec(spec)
            mod.rb, mod.mss, mod.ob = SCBX, SCBX.mss, OBS
            spec.loader.exec_module(mod)
            if getattr(mod, 'RULE_ID', None) != rid:
                raise Refused('%s: module %s declares RULE_ID %r' % (rid, p, getattr(mod, 'RULE_ID', None)))
            for k in ('CLAIMS', 'apply', 'activation'):
                if not hasattr(mod, k):
                    raise Refused('%s: module %s has no %s' % (rid, p, k))
            mod.SHA256 = sha256(open(p, 'rb').read())
            return mod
    raise Refused('%s: no module %s.py in %s' % (rid, rid, dirs))


def matchup_key(mod):
    """rb-data-13: a module's MATCHUP key (the enemy base side its variants run against, read from the shared latch), or None"""
    k = getattr(mod, 'MATCHUP', None)
    if k is not None and k not in SCBX.SENSE_BASE:
        raise Refused('%s: MATCHUP %r is not a base side the shared latch decides (%s)' % (mod.RULE_ID, k, ', '.join(SCBX.SENSE_BASE)))
    return k


def claims_of(mod):
    c = mod.CLAIMS
    out = {k: sorted([list(x) for x in c.get(k, [])]) if k in ('scripts', 'groups') else sorted(c.get(k, []))
           for k in ('scripts', 'groups', 'teams', 'priority_sets')}
    bad = sorted(set(c) - set(out))
    if bad:
        raise Refused('%s: CLAIMS has unknown kind(s) %s' % (mod.RULE_ID, bad))
    return out


# --------------------------------------------------------------------------- compile
def _match(pattern, rid):
    return rid.startswith(pattern[:-1]) if pattern.endswith('*') else rid == pattern


def _together(a, b, gate):
    """two entries that run for the same player: a shared tier (or off) and a shared general"""
    return (gate == 'off' or set(a['tiers']) & set(b['tiers'])) and set(a['generals']) & set(b['generals'])


def static_checks(entries, registry, rule_ids, mods, gate, groups=None, stock_teams=None):
    """capabilities, registry conflicts and dependencies, CLAIMS overlap - before anything is applied"""
    R = registry['rules']
    caps = set((rule_ids or {}).get('_capabilities', []))
    on = [e for e in entries if e['on']]
    for e in on:
        for c in R[e['id']]['capabilities']:
            if c not in caps:
                raise Refused('%s needs the capability %s, which the target binary lacks (rule_ids.json _capabilities %s)'
                              % (e['id'], c, sorted(caps)))
    for i, a in enumerate(on):
        for b in on[i + 1:]:
            if a['id'] == b['id'] or not _together(a, b, gate):
                continue
            if any(_match(p, b['id']) for p in R[a['id']]['conflicts']) or any(_match(p, a['id']) for p in R[b['id']]['conflicts']):
                raise Refused('%s conflicts with %s (registry) for %s' % (a['id'], b['id'], sorted(set(a['generals']) & set(b['generals']))))
            if a['id'] in mods and b['id'] in mods:
                ca, cb, shared = claims_of(mods[a['id']]), claims_of(mods[b['id']]), set(a['generals']) & set(b['generals'])
                ka, kb = matchup_key(mods[a['id']]), matchup_key(mods[b['id']])
                # a stock group's claim covers its member scripts: switching the group off reaches every one
                cover = lambda c: {tuple(x) for x in c['scripts'] + c['groups'] if x[0] in shared} | \
                    {(o, m) for o, g in c['groups'] if o in shared for m in (groups or {}).get(o, {}).get(g, [])}
                hit = sorted(cover(ca) & cover(cb))
                hit += sorted(set(ca['teams']) & set(cb['teams'])) + sorted(set(ca['priority_sets']) & set(cb['priority_sets']))
                why = GUARDS.overlap_refusal(SCBX, a['id'], b['id'], ka, kb, hit, stock_teams or ())
                if why:
                    raise Refused(why)
    for e in on:
        for d in R[e['id']]['depends']:
            if d in R and R[d].get('capability'):
                continue
            need = set(e['generals']) & set(R[d]['scope'] or []) if d in R else set(e['generals'])
            ok = [x for x in on if _match(d, x['id']) and need <= set(x['generals'])
                  and (gate == 'off' or set(e['tiers']) <= set(x['tiers']))]
            if not ok:
                raise Refused('%s depends on %s, which is not on for %s in the same tiers' % (e['id'], d, sorted(need)))


def apply_tier(mss, base, entries, mods, ctx, audit, toc=None):
    """one tier's (or off's) result: every on DATA module in registry order, each judged by the guards, then the ENGINE lines and
    the clock. toc: the name table to start from (tiers share one growing table, so their nodes compare byte for byte)"""
    scb = mss.SCB(base)
    SCBX.build_reset()
    if toc is not None:
        scb.toc = list(toc)
        scb.byname = {n: i for n, i in scb.toc}
        scb.byid = {i: n for n, i in scb.toc}
    for e in entries:
        if not (e['on'] and e['kind'] == 'DATA'):
            continue
        before = mss.SCB(scb.write())
        try:
            mods[e['id']].apply(scb, list(e['generals']), dict(e['params']))
        except AssertionError as x:
            raise Refused('%s: the module refused to apply: %s' % (e['id'], x))
        except Exception as x:                  # e.g. a write into the read-only team view
            raise Refused('%s: the module crashed: %s: %s' % (e['id'], type(x).__name__, x))
        scb = mss.SCB(scb.write())          # judge what was written, not the module's objects
        try:
            GUARDS.team_provenance(SCBX, before, scb, e, Refused)
            rec = GUARDS.check_module(SCBX, before, scb, e, mods[e['id']], ctx)
            GUARDS.matchup_module(SCBX, before, scb, e, matchup_key(mods[e['id']]), ctx)
            GUARDS.team_hooks(SCBX, before, scb, e, Refused)      # last: an allow-list refuses what the guards above have more to say about
        except ValueError as x:             # a chunk the engine would refuse to read (the team decoder checks every bound)
            raise Refused('%s: writes a malformed chunk: %s' % (e['id'], x))
        for x in rec['normal_also']:
            if x not in audit['normal_also']:
                audit['normal_also'].append(x)
        audit.setdefault('engine_hidden_reads', []).extend(x for x in rec.get('engine_hidden_reads', []) if x not in audit.get('engine_hidden_reads', []))
        for o, fl in rec['flag_audit'].items():
            for v, r in fl.items():
                audit['flag_audit'].setdefault(o, {})[v] = dict(r, read_by=sorted(set(audit['flag_audit'].get(o, {}).get(v, {}).get('read_by', [])) | {e['id']}))
    for e in entries:
        if e['kind'] in ('ENGINE', 'VISION'):
            audit['engine_lines'] += engine_lines(mss, scb, e, audit['rule_ids'], audit['action_ints'], audit['tiers'])
    for o in SCBX.GENERALS:
        if SCBX.uses_clock(scb, o):
            SCBX.ensure_clock(scb, o)
    on = [e for e in entries if e['on'] and e['kind'] == 'DATA' and e['id'] in mods]
    keys = {e['id']: matchup_key(mods[e['id']]) for e in on}
    GUARDS.final_claims_overlap(SCBX, scb, on, ctx['claims'], Refused, keys, ctx['stock_teams'])
    GUARDS.matchup_final(SCBX, mss.SCB(base), scb, on, keys, ctx['claims'], Refused)
    sets = audit.setdefault('new_priority_sets', {})                  # every tier's, not the last one's
    for v, owners in GUARDS.whole_file(SCBX, scb, ctx).items():
        sets[v] = sorted(set(sets.get(v, [])) | set(owners))
    return scb


_CTX = {}


def base_ctx(mss, base, mods):
    k = sha256(base)
    if k not in _CTX:
        _CTX[k] = GUARDS.base_context(SCBX, mss.SCB(base), Refused, mss.LAUNCH_FIX_OWNERS)
    ctx = dict(_CTX[k], seen_sets=set(), claims={rid: claims_of(m) for rid, m in mods.items()}, reads=[], new_named=set(), copies={},
               stock_teams=frozenset(d['teamName'] for d in SCBX.teams_ro(mss.SCB(base))))
    return ctx


def compile_ruleset(mss, base, ruleset_data, tier_gate=None, registry=None, rule_ids=None, module_dirs=None, action_ints=None):
    """-> (scb bytes, manifest dict). Raises Refused."""
    SCBX.mss = mss
    registry = registry if registry is not None else load_registry()
    rs = parse_ruleset(ruleset_data)
    gate = resolve_gate(tier_gate, rs)
    entries = validate(rs, registry, gate, rule_ids)
    reopen = reopened(rs, registry)
    mods = {e['id']: load_module(e['id'], module_dirs or [DATA_DIR]) for e in entries if e['kind'] == 'DATA'}
    for e in entries:
        m = mods.get(e['id'])
        if m is None:
            continue
        scope = getattr(m, 'SCOPE', None)            # a module may implement only part of the registry's scope
        if scope is not None:
            out_of = [g for g in e['generals'] if g not in scope]
            if out_of and e['explicit_generals']:
                raise Refused('%s: not implemented for %s (the module covers %s)' % (e['id'], out_of, list(scope)))
            e['generals'] = [g for g in e['generals'] if g in scope]
            if not e['generals']:
                raise Refused('%s: no general left that the module covers' % e['id'])
        for k, why in getattr(m, 'UNUSED', {}).items():
            if k in e['explicit'] and e['params'][k] != registry['rules'][e['id']]['params'][k]['default']:
                raise Refused('%s: param %s=%d has no effect (%s) - only its default %d is accepted'
                              % (e['id'], k, e['params'][k], why, registry['rules'][e['id']]['params'][k]['default']))
    ctx = base_ctx(mss, base, mods)
    static_checks(entries, registry, rule_ids, mods, gate, ctx['groups'], ctx['stock_teams'])
    audit = {'normal_also': [], 'flag_audit': {}, 'engine_lines': [], 'rule_ids': rule_ids, 'action_ints': action_ints, 'tiers': []}
    marks = []
    if gate == 'off':
        out = apply_tier(mss, base, entries, mods, dict(ctx, reads=[], new_named=set(), copies={}), audit).write()
    else:
        works, toc = {}, None
        for t in TIERS:
            es = [e for e in entries if t in e['tiers']]
            if not es:
                continue
            audit['tiers'] = [t]
            works[t] = apply_tier(mss, base, es, mods, dict(ctx, seen_sets=set(), reads=[], new_named=set(), copies={}), audit, toc=toc)
            toc = works[t].toc
        if works:
            out, marks = TIERMARK.merge(SCBX, mss, base, works, Refused)
            views = {t: mss.SCB(TIERMARK.filter_for(mss, SCBX, out, t)) for t in works}
            GUARDS.merged_priority_sets(SCBX, views, ctx['sets'], Refused)
            GUARDS.check_names(SCBX, [views[t] for t in sorted(views)], ctx, 'the tier-merged file')
        else:
            out = base
        if marks and 'TIER.FILTER' not in set((rule_ids or {}).get('_capabilities', [])):
            raise Refused('tier gate on: %d marked entries need the engine\'s tier filter (rule_ids.json _capabilities TIER.FILTER), which'
                          ' the target binary lacks - Hard would run them too. Use --tier-gate off for a binary without it' % len(marks))
        if not marks:
            out = base                      # nothing differs from stock: the shipped bytes, name table included
        # one ENGINE line per (general, rule, actions), with the tiers that carry it
        merged = []
        for l in audit['engine_lines']:
            for m in merged:
                if (m['general'], m['rule'], m['actions']) == (l['general'], l['rule'], l['actions']):
                    m['tiers'] = [t for t in TIERS if t in m['tiers'] + l['tiers']]
                    break
            else:
                merged.append(dict(l))
        audit['engine_lines'] = merged
    assert mss.SCB(out).write() == out, 'round-trip failed on the output'
    GUARDS.seen_final(SCBX, [views[t] for t in sorted(views)] if gate != 'off' and marks else [mss.SCB(out)], Refused)
    touched = {g for e in entries if e['on'] for g in e['generals']}
    pool = GUARDS.pool_budget(SCBX, [views[t] for t in sorted(views)] if gate != 'off' and marks else [mss.SCB(out)], ctx, touched)
    man = {
        'schema': MANIFEST_SCHEMA, 'ruleset_sha256': sha256(ruleset_data), 'base': rs.get('base'),
        'ruleset_canonical_sha256': sha256(json.dumps([{k: e[k] for k in ('id', 'on', 'generals', 'tiers', 'params', 'refs')}
                                                       for e in entries], sort_keys=True).encode()),
        'base_scb_sha256': sha256(base), 'retail_source_sha256': mss.SOURCE_SHA256, 'compiler_sha256': compiler_sha256(),
        'tier_gate': gate,
        **({'reopened': [{'id': k, 'reason': v} for k, v in sorted(reopen.items())]} if reopen else {}),
        'entries': [{'id': e['id'], 'kind': e['kind'], 'on': e['on'], 'generals': e['generals'], 'tiers': e['tiers'],
                     'params': e['params'], 'refs': e['refs'],
                     'claims': claims_of(mods[e['id']]) if e['id'] in mods else None,
                     'module_sha256': mods[e['id']].SHA256 if e['id'] in mods else None} for e in entries],
        'engine_lines': audit['engine_lines'], 'marks': marks, 'requires_capabilities': ['TIER.FILTER'] if marks else [],
        'normal_also': sorted(audit['normal_also']), 'flag_audit': audit['flag_audit'],
        'new_priority_sets': audit.get('new_priority_sets', {}),
        'pool_budget': pool,         # the engine pools this file can fill: tables global, counters / flags per player (max_players)
        'engine_hidden_reads': sorted(audit.get('engine_hidden_reads', [])),   # for rb-sentient's fog audit (actions that read hidden state)
        'output': {'sha256': sha256(out), 'bytes': len(out), 'fnv1a32': '%08X' % fnv1a32(out),
                   'script_count': len(SCBX.lists(mss.SCB(out)))},     # = [SKIRMISH_DIAG] parsed scriptCount= (ScriptLists read)
    }
    return out, man


def promote_default(manifest, ruleset_data, scb_data, dst, mss=None, base=None, registry=None, rule_ids=None, module_dirs=None):
    """plan 4.4 step 2: a frozen champion becomes the recipe's shipped default - only a ruleset that resolves to tier_gate on and that
    RECOMPILES here to exactly these scb bytes (a relabelled manifest proves nothing)"""
    if manifest.get('tier_gate') != 'on':
        raise Refused('promote: the manifest says tier_gate=%s - only a tier_gate=on compile can become the shipped default'
                      % manifest.get('tier_gate'))
    if sha256(ruleset_data) != manifest.get('ruleset_sha256'):
        raise Refused('promote: this ruleset is not the one the manifest compiled')
    if sha256(scb_data) != manifest.get('output', {}).get('sha256'):
        raise Refused('promote: this scb is not the manifest\'s output')
    if resolve_gate(None, parse_ruleset(ruleset_data)) != 'on':
        raise Refused('promote: the ruleset itself resolves to tier_gate off')
    if mss is None or base is None:
        raise Refused('promote: needs the recipe and the shipped base to recompile the ruleset')
    out, _man = compile_ruleset(mss, base, ruleset_data, tier_gate='on', registry=registry, rule_ids=rule_ids, module_dirs=module_dirs)
    if out != scb_data:
        raise Refused('promote: the ruleset recompiles to %s, not the scb given (%s)' % (sha256(out)[:12], sha256(scb_data)[:12]))
    for k in ('counters', 'flags'):                  # a shipped default meets every lobby, not a retail map
        mp = _man['pool_budget'][k]['max_players']
        if mp < GUARDS.LOBBY_PLAYERS:
            raise Refused('promote: %d players of the heaviest general fill the engine\'s %s pool (%d per player, %d slots), a lobby holds'
                          ' %d (MAX_SLOTS) - raise MAX_COUNTERS / MAX_FLAGS in the engine (ScriptEngine.h:221) first'
                          % (mp + 1, k, _man['pool_budget'][k]['heaviest_general'], GUARDS.POOL, GUARDS.LOBBY_PLAYERS))
    tmp = dst + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(ruleset_data)
    os.replace(tmp, dst)


def compiler_sha256():
    h = hashlib.sha256()
    for n in CODE_FILES + ('../make-skirmish-scripts.py',):
        p = os.path.join(HERE, n)
        if os.path.isfile(p):
            h.update(n.encode() + b'\0' + open(p, 'rb').read() + b'\0')
    return h.hexdigest()


# --------------------------------------------------------------------------- data_claims.json (what the tuner reads)
CLAIMS_JSON = os.path.join(HERE, 'data_claims.json')


def export_claims(mss):
    """{rule_id: CLAIMS} of every module in data/, as written to data_claims.json (overlap = same owner + same name; rb-data-13: two
    rules whose `matchup` keys differ may share stock teams, the "GX Matchup" group and scripts and "GX MU Off <team>" - guards.mu_shareable)"""
    SCBX.mss = mss
    out = {}
    for f in sorted(os.listdir(DATA_DIR)) if os.path.isdir(DATA_DIR) else []:
        if f.endswith('.py'):
            m = load_module(f[:-3], [DATA_DIR])
            out[m.RULE_ID] = dict(claims_of(m), scope=list(getattr(m, 'SCOPE', [])) or None,   # None = the registry's whole scope
                                  matchup=matchup_key(m),                                          # the shared latch's key, or None
                                  unused=sorted(getattr(m, 'UNUSED', {})))                       # params the tuner must leave at default
    return json.dumps({'schema': 'gx-rulebank-data-claims/1', 'rules': out}, indent=1, sort_keys=True) + '\n'


def main(argv):
    if argv in (['--write-claims'], ['--check-claims']):
        text = export_claims(load_recipe())
        have = open(CLAIMS_JSON, encoding='utf-8').read() if os.path.isfile(CLAIMS_JSON) else None
        if argv[0] == '--check-claims':
            print('data_claims.json %s' % ('FRESH' if have == text else 'STALE: run compiler.py --write-claims'))
            return 0 if have == text else 1
        tmp = CLAIMS_JSON + '.tmp'
        with open(tmp, 'w', encoding='utf-8') as f:
            f.write(text)
        os.replace(tmp, CLAIMS_JSON)
        print('wrote %s (%d rules)' % (CLAIMS_JSON, len(json.loads(text)['rules'])))
        return 0
    print('compiler.py --write-claims | --check-claims   (compile with make-skirmish-scripts.py --ruleset)')
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
