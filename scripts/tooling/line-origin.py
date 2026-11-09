#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Attribute every line of this tree to the earliest of several base trees that already has it.

Usage (from the repository root, with the base commits fetched - see docs/LINES_BY_ORIGIN.md):

    python3 scripts/tooling/line-origin.py HEAD \
        EA=0a05454d8574207440a5fb15241b98ad0b435590 \
        TheSuperHackers=f51b28047a8f6f1e3e6f227c32aa96e52d360fef,3fcbbb4dca \
        lineage=c5c8c4d3e7

For each base, `git diff -U0 -w -M <base> <tree>` lists the lines of <tree> that the base does not have
(whitespace-only edits keep the older owner, as `git blame -w` does).
A line counts toward the FIRST base (in argument order) that has it; a label with several commits
(comma-separated) has a line if any of them does. A base also has a line that a LATER base carries
unchanged from it (one hop: EA code that upstream moved into Core/ and this fork then grew past git's
rename detection still counts as EA). Lines no base has are counted as "this fork".
Only text files are counted; blank lines are counted separately.
"""
import re
import subprocess
import sys
from collections import defaultdict

CODE_EXT = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inl", ".m", ".mm"}
HUNK = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")


def git(*args):
    return subprocess.run(["git", "-c", "core.quotePath=false", *args], check=True,
                          capture_output=True).stdout


def unquote(p):
    p = p.rstrip("\t")
    if p.startswith('"'):
        p = p[1:-1].encode("latin-1", "backslashreplace").decode("unicode_escape")
    return p


def diff_hunks(base, tree):
    """{tree path: (base path or None, [(old_start, old_count, new_start, new_count)])} for changed files."""
    out = git("diff", "-U0", "-w", "-M", "-l0", "--no-color", "--no-ext-diff", base, tree)
    files = {}
    old = path = None
    for raw in out.split(b"\n"):
        if raw.startswith(b"--- "):
            o = unquote(raw[4:].decode("utf-8", "replace"))
            old = None if o == "/dev/null" else o[2:]
        elif raw.startswith(b"+++ "):
            p = unquote(raw[4:].decode("utf-8", "replace"))
            path = None if p == "/dev/null" else p[2:]
            if path is not None:
                files[path] = (old, [])
        elif raw.startswith(b"@@") and path is not None:
            m = HUNK.match(raw.decode("utf-8", "replace"))
            files[path][1].append((int(m.group(1)), int(m.group(2) or 1), int(m.group(3)), int(m.group(4) or 1)))
    return files


def added_lines(hunks):
    """{path: set(line numbers in tree that the base lacks)}."""
    return {p: {n for _, _, s, c in h for n in range(s, s + c)} for p, (_, h) in hunks.items()}


def base_line(hunks, path, n):
    """(base path, base line) that tree line `n` of `path` is unchanged from, or None if it was added."""
    if path not in hunks:
        return path, n
    old, h = hunks[path]
    shift = 0
    for os_, oc, ns, nc in h:
        if n < (ns if nc else ns + 1):
            break
        if nc and n < ns + nc:
            return None
        shift = (os_ + oc if oc else os_ + 1) - (ns + nc if nc else ns + 1)
    return (old, n + shift) if old else None


def kind(path):
    """What an engine file of this fork is: vendored, generated, tests or hand-written code."""
    if "/third_party/" in path or re.search(r"/dr_(wav|mp3)\.h$", path):
        return "vendored third-party"
    if re.search(r"/shaders/.*\.(frag|vert|comp|geom)\.h$", path):
        return "generated shader bytecode"
    if re.search(r"/tests?/", path) or re.search(r"_test\w*\.(cpp|h)$", path):
        return "tests"
    return "hand-written"


def area(path):
    if not path.startswith(("Core/", "Generals/", "GeneralsMD/", "Dependencies/")):
        return "other"
    dot = path.rfind(".")
    return "engine" if dot >= 0 and path[dot:].lower() in CODE_EXT else "other"


def main():
    tree, specs = sys.argv[1], sys.argv[2:]
    bases = [(spec.split("=", 1)[0], spec.split("=", 1)[1].split(",")) for spec in specs]
    direct = {c: diff_hunks(c, tree) for _, commits in bases for c in commits}
    # One hop: base i also owns a line that a LATER base j has unchanged from base i (a file
    # upstream moved and the fork then grew past rename detection still traces back to EA).
    hop = {(c, d): added_lines(diff_hunks(c, d))
           for i, (_, ci) in enumerate(bases) for _, cj in bases[i + 1:] for c in ci for d in cj}
    direct_added = {c: added_lines(h) for c, h in direct.items()}

    def owns(i, path, n):
        _, commits = bases[i]
        if any(n not in direct_added[c].get(path, ()) for c in commits):
            return True
        for _, later in bases[i + 1:]:
            for d in later:
                at = base_line(direct[d], path, n)
                if at and any(at[1] not in hop[(c, d)].get(at[0], ()) for c in commits):
                    return True
        return False

    labels = [b[0] for b in bases] + ["this fork"]
    totals = {a: defaultdict(int) for a in ("engine", "other")}
    blanks = {a: defaultdict(int) for a in ("engine", "other")}
    fork_kinds = defaultdict(int)
    entries = []
    for e in git("ls-tree", "-r", "-z", tree).split(b"\0"):
        if e:
            meta, name = e.split(b"\t", 1)
            if meta.split()[1] == b"blob":  # skips submodules
                entries.append((meta.split()[2].decode(), name.decode("utf-8", "replace")))
    cat = subprocess.Popen(["git", "cat-file", "--batch"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    for sha, path in entries:
        cat.stdin.write(sha.encode() + b"\n")
        cat.stdin.flush()
        size = int(cat.stdout.readline().split()[2])
        blob = cat.stdout.read(size)
        cat.stdout.read(1)
        if b"\0" in blob[:8000]:
            continue  # binary
        lines = blob.split(b"\n")
        if lines and lines[-1] == b"":
            lines.pop()
        a = area(path)
        for n, text in enumerate(lines, 1):
            owner = next((bases[i][0] for i in range(len(bases)) if owns(i, path, n)), "this fork")
            totals[a][owner] += 1
            if a == "engine" and owner == "this fork":
                fork_kinds[kind(path)] += 1
            if not text.strip():
                blanks[a][owner] += 1
            if PER_FILE is not None:
                PER_FILE[path][owner] += 1
    for a, title in (("engine", "Engine C/C++ (Core/, Generals/, GeneralsMD/, Dependencies/)"),
                     ("other", "Everything else (build system, scripts, launcher, data generators, docs)")):
        total = sum(totals[a].values())
        print(f"{title}: {total} lines")
        for label in labels:
            n = totals[a][label]
            pct = 100.0 * n / total if total else 0.0
            print(f"  {label:16} {n:9} lines {pct:5.1f}%  ({n - blanks[a][label]} non-blank)")
        if a == "engine":
            for k in ("vendored third-party", "generated shader bytecode", "tests", "hand-written"):
                print(f"    this fork, {k}: {fork_kinds[k]}")


PER_FILE = None

if __name__ == "__main__":
    main()
