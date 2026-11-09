#!/usr/bin/env python3
"""One-off data collector for THIRD_PARTY_NOTICES.txt (re-run when a lock file or a bundled Debian package set changes).

  collect_third_party_data.py crates  <cargo-metadata.json>
  collect_third_party_data.py debian  <debian-copyright.json> <debian-source.json> <pkg-file-map.txt>...

crates : `cargo metadata --offline --locked --format-version 1` run in scripts/launcher-rs. Writes
         resources/legal/third-party/crates.json and texts/crates/<sha>.txt (each licence file once).
debian : debian-copyright.json is {pkg: {ver, home, copyright}} read from /usr/share/doc/<pkg>/copyright on the
         build host (the VPS) for every package whose library lands in the Linux tarball or the launcher AppImage;
         debian-source.json is {pkg: {source, source_version}} from `dpkg-query -W -f='${source:Package}|${source:Version}'`;
         each pkg-file-map is lines "libfoo.so.1|debian-package" (soname -> `dpkg -S`). Writes debian.json and
         texts/debian/<sha>.txt.
The generator (third_party_notices.py) reads only the committed output, so it needs neither cargo nor a Debian host.
"""
import hashlib, json, os, sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
OUT = os.path.join(ROOT, "resources", "legal", "third-party")
LICENSE_FILE_PREFIXES = ("LICENSE", "LICENCE", "COPYING", "NOTICE", "UNLICENSE")


def put_text(sub, text):
    key = hashlib.sha256(text.encode("utf-8")).hexdigest()[:12]
    d = os.path.join(OUT, "texts", sub)
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, key + ".txt")
    if not os.path.exists(p):
        with open(p, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    return key


def crates(meta_path):
    meta = json.load(open(meta_path))
    out = []
    for p in sorted(meta["packages"], key=lambda p: (p["name"], p["version"])):
        if p["source"] is None:
            continue  # the workspace's own crates are GPL-3 like the rest of the repo
        d = os.path.dirname(p["manifest_path"])
        ids = []
        for fn in sorted(os.listdir(d)) if os.path.isdir(d) else []:
            if fn.upper().startswith(LICENSE_FILE_PREFIXES) and os.path.isfile(os.path.join(d, fn)):
                ids.append(put_text("crates", open(os.path.join(d, fn), errors="replace").read()))
        out.append({"name": p["name"], "version": p["version"], "license": p.get("license") or "",
                    "authors": p.get("authors") or [], "repository": p.get("repository") or "", "texts": ids})
    with open(os.path.join(OUT, "crates.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(out, f, indent=1, sort_keys=True)
        f.write("\n")
    print("crates:", len(out), "without a licence file:", sum(1 for c in out if not c["texts"]))


def debian(cr_path, src_path, *map_paths):
    data = json.load(open(cr_path))
    src = json.load(open(src_path))
    files = {}
    for mp in map_paths:
        for line in open(mp):
            if "|" in line:
                f, pk = line.rstrip("\n").split("|", 1)
                if pk:
                    files.setdefault(pk, set()).add(f)
    # every mapped package must have copyright data and a source package: a silently dropped library is a missing notice
    missing = sorted(pk for pk in files if pk not in data or not data[pk]["copyright"] or pk not in src)
    if missing:
        sys.exit("error: no copyright text or source package for mapped Debian package(s): " + ", ".join(missing))
    out = {}
    for pk, v in sorted(data.items()):
        out[pk] = {"version": v["ver"], "homepage": v["home"], "files": sorted(files.get(pk, [])),
                   "source": src[pk]["source"], "source_version": src[pk]["source_version"],
                   "text": put_text("debian", v["copyright"]) if v["copyright"] else ""}
    with open(os.path.join(OUT, "debian.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(out, f, indent=1, sort_keys=True)
        f.write("\n")
    print("debian packages:", len(out))


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "crates":
        crates(sys.argv[2])
    elif len(sys.argv) >= 5 and sys.argv[1] == "debian":
        debian(sys.argv[2], *sys.argv[3:])
    else:
        sys.exit(__doc__)
