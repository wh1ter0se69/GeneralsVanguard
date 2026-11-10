# GeneralsX: rule bank, DATA half: the tier marks, the compiler's LAST step (
# 2.8; format agreed with 30/10). Hard, Brutal and Sentient share one script list, and wh1ter0se keeps
# Hard as it is, so a ruleset's changes never sit in a stock entry:
#   * every stock script and team stays byte-identical;
#   * a changed stock script / team gets a copy named GXT.<T>.<name> right AFTER it, <T> = SENTIENT, BRUTAL or SENTIENT+BRUTAL;
#   * a new script, group or team is named GXT.<T>.<name> where the rule put it (a marked group's scripts go or stay with it);
#   * two tiers needing different versions of one entry get two marks (SENTIENT first) - never two kept marks of one name;
#   * references always use the plain name.
# The engine's tier filter (rb-core) gives a player of tier T, on its own copy after duplicateAndQualify, every mark whose tiers hold T
# in its stock twin's place under the plain name, and drops every other mark. filter_for() below is this file's model of that filter;
# the selftest proves filter(HARD) == the shipped file and filter(T) == T's own compile, per general.
import copy

PREFIX = 'GXT.'
TIER_ORDER = ('SENTIENT', 'BRUTAL')
TOKENS = {'SENTIENT': ('SENTIENT',), 'BRUTAL': ('BRUTAL',), 'SENTIENT+BRUTAL': ('SENTIENT', 'BRUTAL')}


def mark(tiers, name):
    return '%s%s.%s' % (PREFIX, '+'.join(t for t in TIER_ORDER if t in tiers), name)


def parse(name):
    """None for a plain name; (tiers or None when malformed, plain name) for a mark"""
    if not name.startswith(PREFIX):
        return None
    tok, dot, rest = name[len(PREFIX):].partition('.')
    return (TOKENS.get(tok) if dot else None), rest


def _rename(rb, mss, n, name):
    m = copy.copy(n)
    if n.name == 'Script':
        m = mss.deep_copy(n)
        mss.set_script(m, name=name)
    else:                                        # ScriptGroup: name, then the rest of its header
        m = mss.deep_copy(n)
        end = mss.astr(n.prefix, 0)[1]
        m.prefix = mss.enc_astr(name) + n.prefix[end:]
    return m


def merge_order(seqs):
    """one order of the union of the tiers' sequences in which EVERY tier's own sequence is a subsequence (a
    union that only respects the shared names reorders a tier), or None when none exists. Deterministic: Kahn's algorithm, ties to the
    earliest tier and position."""
    nodes, rank = [], {}
    for ti, sq in enumerate(seqs):
        for pi, x in enumerate(sq):
            if x not in rank:
                rank[x] = (ti, pi); nodes.append(x)
    succ, indeg = {x: set() for x in nodes}, {x: 0 for x in nodes}
    for sq in seqs:
        for a, b in zip(sq, sq[1:]):
            if b not in succ[a]:
                succ[a].add(b); indeg[b] += 1
    out, ready = [], sorted((x for x in nodes if indeg[x] == 0), key=lambda x: rank[x])
    while ready:
        x = ready.pop(0); out.append(x)
        for y in sorted(succ[x], key=lambda y: rank[y]):
            indeg[y] -= 1
            if indeg[y] == 0:
                ready.append(y); ready.sort(key=lambda z: rank[z])
    if len(out) != len(nodes):
        return None
    for sq in seqs:
        if [x for x in out if x in set(sq)] != list(sq):
            return None
    return out


def same_order(seqs):
    return merge_order(seqs) is not None


def _key(rb, n):
    return (n.name, rb.node_name(n))


def _merge_kids(rb, mss, base_kids, tier_kids, scbs, where, marks, Refused):
    """the merged child list of one parent (a ScriptList or a stock group)"""
    tiers = list(tier_kids)
    stock = {}
    for n in base_kids:
        stock.setdefault(_key(rb, n), []).append(n)
    out = []

    def keyed(kids):
        seen, res = {}, []
        for n in kids:
            k = _key(rb, n)
            res.append((k + (seen.get(k, 0),), n))
            seen[k] = seen.get(k, 0) + 1
        return res

    bkeys = keyed(base_kids)
    tk = {t: keyed(tier_kids[t]) for t in tiers}
    for t in tiers:
        for k, n in tk[t]:
            if k[1] is not None and k[1].startswith(PREFIX):
                raise Refused('%s: %s %r carries a tier mark - only the compiler writes them' % (where, n.name, k[1]))
    tmap = {t: dict(tk[t]) for t in tiers}

    def news_after(anchor):
        """per tier: the new nodes right after base key `anchor` (None = before the first base node)"""
        res = {}
        bset = {k for k, _n in bkeys}
        for t in tiers:
            cur, lst = None, []
            for k, n in tk[t]:
                if k in bset:
                    cur = k
                elif cur == anchor:
                    if k[2] != 0 or k[:2] in stock:
                        raise Refused('%s: a new %s reuses the stock name %r - a mark would replace that stock entry' % (where, n.name, k[1]))
                    lst.append((k, n))
            res[t] = lst
        return res

    def emit_marks(versions, ref):
        """versions: {tier: node} that differ from the stock `ref` (None for a new entry); one mark per distinct version"""
        groups = []
        for t in TIER_ORDER:
            if t not in versions:
                continue
            c = rb.canon(scbs[t], versions[t])
            for g in groups:
                if g[0] == c:
                    g[1].append(t)
                    break
            else:
                groups.append([c, [t], versions[t]])
        for _c, ts, node in groups:
            nm = rb.node_name(node)
            out.append(_rename(rb, mss, node, mark(ts, nm)))
            marks.append({'where': where, 'kind': node.name, 'name': nm, 'tiers': ts, 'replaces_stock': ref is not None})

    def emit_news(anchor):
        res = news_after(anchor)
        order = merge_order([[k for k, _n in res.get(t, [])] for t in tiers])
        if order is not None:
            for k in order:
                emit_marks({t: dict(res[t])[k] for t in tiers if k in dict(res[t])}, None)
        else:                                    # the tiers put their new entries in different orders: each keeps its own
            for t in TIER_ORDER:
                for k, n in res.get(t, []):
                    emit_marks({t: n}, None)

    emit_news(None)
    for k, b in bkeys:
        for t in tiers:
            if k not in tmap[t]:
                raise Refused('%s: the %s compile lost the stock %s %r' % (where, t, b.name, k[1]))
        if b.name == 'ScriptGroup':
            g = mss.deep_copy(b)
            g.kids = _merge_kids(rb, mss, b.kids, {t: tmap[t][k].kids for t in tiers}, scbs, '%s/%s' % (where, k[1]), marks, Refused)
            out.append(g)
        else:
            out.append(b)
            changed = {t: tmap[t][k] for t in tiers if rb.canon(scbs[t], tmap[t][k]) != rb.canon(scbs['_base'], b)}
            if changed and len(stock[k[:2]]) > 1:
                raise Refused('%s: %r is twice in its parent: the filter cannot tell which one a mark replaces' % (where, k[1]))
            emit_marks(changed, b)
        emit_news(k)
    return out


def merge(rb, mss, base_bytes, works, Refused):
    """works: {tier: SCB} compiled from the same base with one shared, growing name table (the last tier's is the superset).
    -> (bytes, marks)"""
    tiers = [t for t in TIER_ORDER if t in works]
    last = works[tiers[-1]]
    out = mss.SCB(base_bytes)
    out.toc = list(last.toc)
    out.byname = {n: i for n, i in out.toc}
    out.byid = {i: n for n, i in out.toc}
    scbs = dict(works)
    scbs['_base'] = out
    marks = []
    lists = rb.lists(out)
    for i, owner in enumerate(mss.LAUNCH_FIX_OWNERS):
        if owner not in rb.GENERALS:
            continue
        tl = {t: rb.lists(works[t])[i].kids for t in tiers}
        lists[i].kids = _merge_kids(rb, mss, lists[i].kids, tl, scbs, owner, marks, Refused)
    # teams: one parent, matched by name within the same owner
    bt = rb.teams(out)
    tt = {t: rb.teams(works[t]) for t in tiers}
    names = {d['teamName'] for d in bt}
    merged = []

    def tmark(versions, ref):
        groups = []
        for t in TIER_ORDER:
            if t in versions:
                c = rb.canon_team(versions[t])
                for g in groups:
                    if g[0] == c:
                        g[1].append(t)
                        break
                else:
                    groups.append([c, [t], versions[t]])
        for _c, ts, d in groups:
            m = {'_e': [list(e) for e in d['_e']], 'teamName': d['teamName']}
            if ref is not None and rb.team_get(m, 'teamOwner') != rb.team_get(ref, 'teamOwner'):
                raise Refused('team %r: a rule moved it to another owner' % d['teamName'])
            rb.team_set(out, m, 'teamName', mark(ts, d['teamName']))
            merged.append(m)
            marks.append({'where': rb.team_get(d, 'teamOwner'), 'kind': 'Team', 'name': d['teamName'], 'tiers': ts,
                          'replaces_stock': ref is not None})

    after = {t: {} for t in tiers}                  # tier -> anchor (base team name or None) -> [new team dicts], one pass each
    for t in tiers:
        cur = None
        for d in tt[t]:
            if d['teamName'] in names:
                cur = d['teamName']
            else:
                if d['teamName'].startswith(PREFIX):
                    raise Refused('team %r carries a tier mark - only the compiler writes them' % d['teamName'])
                after[t].setdefault(cur, []).append(d)

    def tnews(anchor):
        got = {t: {} for t in tiers}
        for t in tiers:
            for d in after[t].get(anchor, []):
                got[t][d['teamName']] = d
        order = merge_order([[d['teamName'] for d in after[t].get(anchor, [])] for t in tiers])
        if order is not None:
            for n in order:
                tmark({t: got[t][n] for t in tiers if n in got[t]}, None)
        else:
            for t in TIER_ORDER:
                for d in after.get(t, {}).get(anchor, []):
                    tmark({t: d}, None)

    tnews(None)
    tmaps = {t: {d['teamName']: d for d in tt[t]} for t in tiers}
    for d in bt:
        merged.append(d)
        tmark({t: tmaps[t][d['teamName']] for t in tiers if rb.canon_team(tmaps[t][d['teamName']]) != rb.canon_team(d)}, d)
        tnews(d['teamName'])
    rb.put_teams(out, merged)
    return out.write(), marks


# --------------------------------------------------------------------------- the filter's model (tests, tools)
def _filter_kids(rb, mss, kids, tier):
    """one parent: a kept mark (its tiers hold `tier`, first of its name wins) takes its stock twin's place under the plain name, or
    stays where it is when it has none; every other mark is dropped; groups are filtered inside"""
    plain = {(n.name, rb.node_name(n)) for n in kids if parse(rb.node_name(n) or '') is None}
    kept = {}
    for n in kids:
        p = parse(rb.node_name(n) or '')
        if p and p[0] and tier in p[0] and (n.name, p[1]) not in kept:
            kept[(n.name, p[1])] = (n, _rename(rb, mss, n, p[1]))
    res, placed = [], set()
    for n in kids:
        p = parse(rb.node_name(n) or '')
        k = (n.name, rb.node_name(n)) if p is None else (n.name, p[1])
        if p is None:
            if k in kept and k not in placed:
                res.append(kept[k][1]); placed.add(k)
            else:
                res.append(n)
        elif k in kept and kept[k][0] is n and k not in plain:
            res.append(kept[k][1]); placed.add(k)
    for i, n in enumerate(res):
        if n.name == 'ScriptGroup':
            g = mss.deep_copy(n)
            g.kids = _filter_kids(rb, mss, n.kids, tier)
            res[i] = g
    return res


def filter_for(mss, rb, scb_bytes, tier):
    """what a player of `tier` (HARD, SENTIENT, BRUTAL) reads, as a file: rb-core's filter, modelled"""
    x = mss.SCB(scb_bytes)
    for lst in rb.lists(x):
        lst.kids = _filter_kids(rb, mss, lst.kids, tier)
    ts = rb.teams(x)
    own = lambda d: rb.team_get(d, 'teamOwner')             # a mark replaces its twin of the SAME owner
    kept = {}
    for d in ts:
        p = parse(d['teamName'])
        if p and p[0] and tier in p[0] and (own(d), p[1]) not in kept:
            m = {'_e': [list(e) for e in d['_e']], 'teamName': d['teamName']}
            rb.team_set(x, m, 'teamName', p[1])
            kept[(own(d), p[1])] = m
    pn = {(own(d), d['teamName']) for d in ts if parse(d['teamName']) is None}
    res = []
    for d in ts:
        p = parse(d['teamName'])
        if p is None:
            res.append(kept.get((own(d), d['teamName']), d))
        elif p[0] and tier in p[0] and (own(d), p[1]) not in pn and kept.get((own(d), p[1])) is not None and \
                (own(d), p[1]) not in {(own(r), r['teamName']) for r in res}:
            res.append(kept[(own(d), p[1])])
    rb.put_teams(x, res)
    return x.write()


def strip_marks(mss, rb, scb_bytes):
    """per general side view with every mark dropped (what HARD reads) - for the 'stock is byte-identical' check"""
    x = mss.SCB(filter_for(mss, rb, scb_bytes, 'HARD'))
    return {o: rb.side_dump(x, o) for o in rb.GENERALS}


def kept_dups(mss, rb, scb_bytes, tier):
    """[(parent, name)] where a player of `tier` would keep two marks of one name (rb-core's ev=tier_dup) - the compiler emits none"""
    x, out = mss.SCB(scb_bytes), []

    def scan(kids, where):
        seen = set()
        for n in kids:
            nm = rb.node_name(n)
            p = parse(nm) if nm else None
            if p and p[0] and tier in p[0]:
                if (n.name, p[1]) in seen:
                    out.append((where, p[1]))
                seen.add((n.name, p[1]))
            if n.name == 'ScriptGroup':
                scan(n.kids, '%s/%s' % (where, nm))
    for o in rb.GENERALS:
        scan(rb.owner_list(x, o).kids, o)
    return out
