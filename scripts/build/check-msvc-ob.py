#!/usr/bin/env python3
"""check-msvc-ob.py <build dir> [config=RelWithDebInfo]   (perf32 A1a, 09/11/2026)

Reads <build dir>/build.ninja (and CMakeFiles/impl-<config>.ninja under Ninja Multi-Config) of an MSVC build and checks the EFFECTIVE inlining level (the last /Ob on each compile edge's FLAGS) per
target: Zero Hour's compiled targets and the GXRender libraries must be /Ob2, every other target (Generals g_*, the shared core_*, the
tools) /Ob1. SimID cannot see compile flags, so this is the check that the Windows build carries the inlining the measurements were taken
on, and that Generals did not get it. Exit 0 = every edge as expected; 1 = a wrong level or an expected target with no edge; 2 = not an
MSVC build of that config (no /Ob anywhere), which must not read as a pass.
"""
import collections
import os
import re
import sys

OB2 = {'z_gameengine', 'z_gameenginedevice', 'z_ww3d2', 'z_wwaudio', 'z_wwdownload', 'z_generals',
       'gx_device', 'gx_record', 'gx_pipeline', 'gx_res', 'gx_vkctx'}
MUST = {'z_gameengine', 'z_gameenginedevice', 'z_generals', 'g_gameengine', 'g_generals'}
EDGE = re.compile(r'^build [^:]+\.(?:obj|o): (?:C|CXX)_COMPILER__(\w+)_(Debug|Release|RelWithDebInfo|MinSizeRel)\b')


def main():
    config = sys.argv[2] if len(sys.argv) > 2 else 'RelWithDebInfo'
    # Ninja (macOS, Linux) keeps the compile edges in build.ninja; Ninja Multi-Config (the win64 preset) keeps them in
    # CMakeFiles/impl-<config>.ninja and build.ninja holds only aliases (dm-ac42606563 P1).
    paths = [p for p in (os.path.join(sys.argv[1], 'build.ninja'), os.path.join(sys.argv[1], 'CMakeFiles', 'impl-%s.ninja' % config))
             if os.path.exists(p)]
    path = ' + '.join(paths) or os.path.join(sys.argv[1], 'build.ninja')
    levels = collections.defaultdict(collections.Counter)
    target = None
    edges = 0
    for line in (l for p in paths for l in open(p, encoding='utf-8', errors='replace')):
        m = EDGE.match(line)
        if m:
            target = re.sub(r'_unscanned$', '', m.group(1)) if m.group(2) == config else None
            edges += target is not None
            continue
        if not line.startswith('  '):
            target = None
            continue
        if target and line.startswith('  FLAGS = '):
            obs = re.findall(r'(?<!\S)[/-]Ob([0-3])(?!\S)', line)
            levels[target]['/Ob' + obs[-1] if obs else 'none'] += 1
    if not edges:
        print('NO %s COMPILE EDGES in %s' % (config, path))
        return 2
    if not any(lv for t in levels for lv in levels[t] if lv != 'none'):
        print('NOT AN MSVC %s BUILD: no /Ob flag on any compile edge in %s' % (config, path))
        return 2
    bad = []
    for t in sorted(levels):
        want = '/Ob2' if t in OB2 else '/Ob1'
        got = dict(levels[t])
        ok = set(got) == {want}
        print('%-4s %-28s want %s got %s' % ('ok' if ok else 'BAD', t, want, got))
        if not ok:
            bad.append(t)
    missing = sorted(MUST - set(levels))
    for t in missing:
        print('MISSING %s: no %s compile edge' % (t, config))
    print('RESULT %s: %d targets, %d wrong, %d missing' % ('PASS' if not bad and not missing else 'FAIL', len(levels), len(bad), len(missing)))
    return 0 if not bad and not missing else 1


if __name__ == '__main__':
    sys.exit(main())
