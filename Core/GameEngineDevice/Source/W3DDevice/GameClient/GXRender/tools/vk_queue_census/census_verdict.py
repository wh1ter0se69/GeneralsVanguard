#!/usr/bin/env python3
# GeneralsX @feature B · dlss-fg-1 14/10/2026 census_verdict.py: C1/C2/C3 of the DLSS Frame Generation queue contract (PREREG A39a,
# modernize/render-dlss/DESIGN_dlss-fg-P0.md §6) from a VK_LAYER_GX_queue_census summary.
#   C1  every operation on a queue the EXECUTABLE fetched comes from ONE thread, the one the executable's own operations on that
#       queue come from. Streamline's work inside our calls is therefore on that thread; anything else is a foreign op.
#   C2  (report) operations of other modules on queues that are not ours: Streamline's own queues, its own threads.
#   C3  overlaps == 0: no two threads inside operations on one queue at once.
# Exit 0 = C1 and C3 PASS. Usage: census_verdict.py <summary> [--exe <name>]
import re
import sys
from collections import defaultdict


def parse(path):
    header, getq, ops, other = {}, [], [], []
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.rstrip("\n")
        if line.startswith("census v1"):
            header = dict(kv.split("=", 1) for kv in line.split()[2:] if "=" in kv)
            continue
        fields = dict(kv.split("=", 1) for kv in line.split()[1:] if "=" in kv)
        kind = line.split(" ", 1)[0]
        if kind == "getqueue":
            getq.append(fields)
        elif kind == "op":
            ops.append(fields)
        else:
            other.append((kind, fields, line))
    return header, getq, ops, other


def innermost(chain):
    return chain.split("<", 1)[0]


def outermost_known(chain):
    return chain.split("<")


def main():
    if len(sys.argv) < 2:
        print(__doc__ or "usage: census_verdict.py <summary> [--exe <name>]")
        return 2
    path = sys.argv[1]
    header, getq, ops, other = parse(path)
    exe = header.get("exe", "?")
    if "--exe" in sys.argv:
        exe = sys.argv[sys.argv.index("--exe") + 1]
    ours = sorted({g["q"] for g in getq if innermost(g.get("module", "")) == exe})
    print(f"summary {path}: exe={exe} ops={header.get('ops')} overlaps={header.get('overlaps')} our queues={ours or 'NONE'}")
    if not ours:
        print("C1 UNKNOWN: the executable fetched no queue (wrong --exe?)")
        return 1

    c1 = True
    for q in ours:
        qops = [o for o in ops if o.get("q") == q]
        exe_tids = {o["tid"] for o in qops if exe in outermost_known(o.get("module", ""))}
        all_tids = defaultdict(int)
        for o in qops:
            all_tids[o["tid"]] += int(o["n"])
        print(f"queue {q}: threads {dict(all_tids)} (threads carrying {exe} calls: {sorted(exe_tids)})")
        by_module = defaultdict(int)
        for o in qops:
            by_module[(o["tid"], o["fn"], o["module"])] += int(o["n"])
        for (tid, fn, mod), n in sorted(by_module.items()):
            mark = "" if tid in exe_tids else "   <-- FOREIGN THREAD"
            print(f"   tid={tid} {fn} {mod} n={n}{mark}")
        if len(exe_tids) != 1 or set(all_tids) != exe_tids:
            c1 = False
    print("C1", "PASS" if c1 else "FAIL", "- one owner thread per queue the game uses")

    print("C2 (report) other modules' operations on queues that are not ours:")
    seen = False
    for o in ops:
        if o.get("q") not in ours:
            seen = True
            print(f"   q={o['q']} tid={o['tid']} {o['fn']} {o['module']} n={o['n']}")
    if not seen:
        print("   none")

    overlaps = int(header.get("overlaps", "0") or 0)
    lines = [l for k, _, l in other if k == "overlap"]
    c3 = overlaps == 0 and not lines
    print("C3", "PASS" if c3 else "FAIL", f"- overlaps={overlaps}")
    for l in lines:
        print("   " + l)

    for k, _, l in other:
        if k in ("acquire", "dev", "swapchain", "image", "device_queues", "plant"):
            print("info " + l)
    return 0 if (c1 and c3) else 1


if __name__ == "__main__":
    sys.exit(main())
