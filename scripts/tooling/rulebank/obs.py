# GeneralsX @feature Claude 30/10/2026 rule bank, DATA half: reading a gym observation stream for a module's activation(obs, me, params)
# (C·rb-data). obs = the records of one game's obs.ndjson (scripts/ai/gym/SCHEMA.md v1: start, tmpl, obs). A module sees this file
# as `ob`. Activation reads only the focal player's OWN rows - it is a measurement, never an input to the AI.
import json


def load(path):
    out = []
    with open(path, encoding='latin-1') as f:
        for line in f:
            line = line.strip()
            if line:
                out.append(json.loads(line))
    return out


def hz(recs):
    for r in recs:
        if r.get('t') == 'start':
            return int(r.get('hz') or 30)
    return 30


def names(recs):
    return {r['id']: r['name'] for r in recs if r.get('t') == 'tmpl'}


def last_frame(recs):
    fs = [r['f'] for r in recs if r.get('t') == 'obs']
    return max(fs) if fs else None


def first_frame(recs, me, pred, n=1, complete=False, after=0):
    """first obs frame >= after at which player `me` owns >= n objects whose template name satisfies pred (complete: built 100%)"""
    nm = names(recs)
    for r in recs:
        if r.get('t') != 'obs' or r['f'] < after:
            continue
        c = sum(1 for o in r.get('o', []) if o[1] == me and pred(nm.get(o[2], '')) and (not complete or o[10] >= 100))
        if c >= n:
            return r['f']
    return None


def ends_with(*suffixes):
    return lambda name: name.endswith(suffixes)


def money_at_least(recs, me, v, after=0):
    """first obs frame >= after at which player `me` holds >= v cash (its own pl row: [i, alive, money, ...])"""
    for r in recs:
        if r.get('t') != 'obs' or r['f'] < after:
            continue
        for p in r.get('pl', []):
            if p[0] == me and p[2] >= v:
                return r['f']
    return None


def census(recs, me, pred, at):
    """how many objects satisfying pred player `me` owns at the first obs frame >= at; None when the game never reached `at`"""
    nm = names(recs)
    for r in recs:
        if r.get('t') == 'obs' and r['f'] >= at:
            return sum(1 for o in r.get('o', []) if o[1] == me and pred(nm.get(o[2], '')))
    return None


def built_by(recs, me, pred, until):
    """how many DISTINCT objects satisfying pred player `me` owned at any obs frame <= until (a lost object still counts; one first seen
    after `until` does not); None when the game never reached `until`"""
    nm = names(recs)
    seen, reached = set(), False
    for r in recs:
        if r.get('t') != 'obs':
            continue
        if r['f'] > until:
            reached = True
            break
        seen.update(o[0] for o in r.get('o', []) if o[1] == me and pred(nm.get(o[2], '')))
    return len(seen) if reached else None


def arrivals(recs, me, pred, until):
    """first-seen frame of every distinct object satisfying pred that `me` owned at an obs frame <= until, ascending"""
    nm = names(recs)
    first = {}
    for r in recs:
        if r.get('t') != 'obs':
            continue
        if r['f'] > until:
            break
        for o in r.get('o', []):
            if o[1] == me and pred(nm.get(o[2], '')):
                first.setdefault(o[0], r['f'])
    return sorted(first.values())
