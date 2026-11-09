#!/usr/bin/env python3
"""Generate and check THIRD_PARTY_NOTICES.txt.

  third_party_notices.py generate                 write THIRD_PARTY_NOTICES.txt at the repo root
  third_party_notices.py check                    fail when that file is not exactly what `generate` would write, or
                                                  when crates.json no longer matches scripts/launcher-rs/Cargo.lock
  third_party_notices.py check-path [--no-host-versions] [--platform P] PATH..  like check-listing, on a built payload: a directory, .zip, .tar.gz or
                                                  .app/.AppDir tree (run by the packagers on what they produced)
  third_party_notices.py check-listing [--platform P] LISTING..  fail when a library-like file in a payload listing (one path per
                                                  line: unzip -Z1, tar tf, find, otool) belongs to no component
  third_party_notices.py check-packagers          fail when a packager script names a library that no component owns,
                                                  or does not copy THIRD_PARTY_NOTICES.txt into its payload

Inputs are committed under resources/legal/third-party/ (components.json is hand-written; crates.json, debian.json and
texts/ come from collect_third_party_data.py plus a copy of each component's own licence file). Stdlib only.
"""
import fnmatch, hashlib, json, os, re, sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
DATA = os.path.join(ROOT, "resources", "legal", "third-party")
OUTFILE = os.path.join(ROOT, "THIRD_PARTY_NOTICES.txt")

# Every packager that builds a player payload. Each must copy THIRD_PARTY_NOTICES.txt next to LICENSE.md.
PACKAGERS = [
    "scripts/build/macos/bundle-macos-zh.sh",
    "scripts/build/macos/bundle-macos-generals.sh",
    "scripts/build/linux/bundle-linux.sh",
    "scripts/build/linux/build-linux-appimage-generals.sh",
    "scripts/build/ios/package-ios-zh.sh",
    "scripts/build/linux/bundle-linux-zh.sh",
    "scripts/build/linux/build-linux-appimage-zh.sh",
    "scripts/build/windows/package-windows-share.ps1",
    "scripts/build/windows/installer/GeneralsX.iss",
    "scripts/build/launcher/package-launcher-linux.sh",
    "scripts/build/launcher/package-launcher-macos.sh",
    "scripts/build/launcher/package-launcher-windows.sh",
    ".github/workflows/build-linux.yml",
    "flatpak/com.fbraz3.GeneralsX.yml",
    "flatpak/com.fbraz3.GeneralsXZH.yml",
]

# Libraries the operating system provides (never shipped by us).
SYSTEM = ["libc.so*", "libm.so*", "libpthread.so*", "librt.so*", "libdl.so*", "ld-linux*", "linux-vdso*",
          "libvulkan_lvp.so", "libSystem*", "libc++*", "libobjc*", "libiconv*", "libcharset*", "libz.1.dylib",
          "kernel32.dll", "user32.dll", "d3d11.dll", "dxgi.dll", "d3dcompiler*.dll", "opengl32.dll", "advapi32.dll",
          "shell32.dll", "ole32.dll", "ws2_32.dll", "winmm.dll", "gdi32.dll", "version.dll", "imm32.dll",
          "dbghelp.dll", "msvcrt.dll", "vcruntime*.dll", "msvcp*.dll", "ucrtbase.dll", "vulkan-1.dll"]


HEADER = """
GeneralsX is licensed under the GNU General Public License, version 3 or later,
with Electronic Arts' additional terms (see LICENSE.md, shipped next to this
file). It includes, links, or ships alongside the third-party software listed
below. Each component stays under its own licence; this file reproduces the
required copyright notices and licence texts. Every licence text appears once,
in section 4, and is referenced by number.

This file is generated (scripts/legal/third_party_notices.py generate) from
resources/legal/third-party/. Do not edit it by hand.

SOURCE CODE. The complete corresponding source of GeneralsX is available from
the project's source repository named in the release notes of the version you
installed. For the GPL/LGPL-licensed libraries below that GeneralsX ships as
binaries but does not build from its own tree (FFmpeg and its codec libraries
on macOS; the Debian/Ubuntu system libraries bundled in the Linux tarball and
the Linux launcher AppImage), the corresponding source is the upstream release
named for each component, and for the Debian/Ubuntu packages the Ubuntu source
package and version named in section 2 (https://packages.ubuntu.com/ and
https://launchpad.net/ubuntu), which also carries Ubuntu's patches. On request to the project, the same source
will be provided on a physical medium for the cost of that medium, for three
years from the date of your download.

LGPL libraries (OpenAL Soft, mpg123, LAME, FFmpeg's LGPL build on iOS and the
LGPL Debian libraries) are used as separate shared libraries that you may
replace with your own build, except where the component entry says it is
statically linked; GeneralsX as a whole is GPL-3.0-or-later and its full source
is available, which is what the LGPL's relinking requirement asks for.
"""


def read(p):
    with open(p, encoding="utf-8", errors="replace") as f:
        return f.read().replace("\r\n", "\n")


def load():
    comps = json.load(open(os.path.join(DATA, "components.json")))["components"]
    crates = json.load(open(os.path.join(DATA, "crates.json")))
    debian = json.load(open(os.path.join(DATA, "debian.json")))
    return comps, crates, debian


def soname(b):
    """('libfoo.so', '60') for libfoo.so.60.31.102; ('libfoo.so', '') for the unversioned symlink name."""
    m = re.match(r"^(.*\.so)(?:\.(\d+))?(?:\..*)?$", b)
    return (m.group(1), m.group(2) or "") if m else None


def matcher(platform=None, allow_system=False):
    """owner(path) -> component id or None. With a platform, only components shipped there (and Debian data only
    for linux) can own a file, so a macOS FFmpeg entry never vouches for a library found in a Linux payload."""
    comps, _, debian = load()
    pats = [(c["id"], p) for c in comps if platform is None or platform in c["platforms"] for p in c["files"]]
    if allow_system:
        pats += [("system", p) for p in SYSTEM]  # only for names a script or an otool row REFERENCES, never files in a payload
    deb = {}
    if platform in (None, "linux"):
        for pk, v in debian.items():
            for f in v["files"]:
                sn = soname(f)
                if sn:
                    deb.setdefault(sn, pk)
                    deb.setdefault((sn[0], ""), pk)

    def owner(path):
        b = os.path.basename(path.rstrip("/"))
        sn0 = soname(b)
        if platform == "linux" and sn0 and sn0[1] and sn0 not in deb and (sn0[0], "") in deb:
            return None  # a SONAME major no recorded Ubuntu package has (libz.so.2 vs libz.so.1): the notices do not cover it
        if platform == "linux" and sn0 and sn0 in deb:
            # a Linux payload's Ubuntu libraries are the Debian packages recorded in debian.json, even when a component
            # glob (zlib, libpng...) would also match: only that owner lets the build-host version gate cover them
            return "debian:" + deb[sn0]
        for cid, p in pats:
            if fnmatch.fnmatchcase(b.lower(), p.lower()):
                return cid
        sn = soname(b)
        # the same soname major as a recorded package file (an apt point release keeps it); an unversioned
        # name (libfoo.so) is a development symlink to some recorded major
        return "debian:" + deb[sn] if sn and sn in deb else None
    return owner


LIB_RE = re.compile(r"(\.so(\.[0-9A-Za-z_]+)*|\.dylib|\.dll|\.exe|\.AppImage|\.ttf|\.otf|\.woff2?)$", re.I)


def is_lib(path):
    b = os.path.basename(path.rstrip("/"))
    if LIB_RE.search(b):
        return True
    if b in ("GeneralsX", "GeneralsXZH"):
        return True  # the game executables themselves (extensionless on macOS, Linux and iOS)
    # extensionless framework binary: Foo.framework/Foo or Foo.framework/Versions/A/Foo
    parts = path.rstrip("/").split("/")
    return any(q.endswith(".framework") and q[:-len(".framework")] == b for q in parts[:-1])


def clean_row(line):
    """A listing row is a path, or an `otool -L` row ('\t@rpath/libx.dylib (compatibility version ...)')."""
    line = line.strip()
    return re.sub(r"\s+\((?:compatibility|current) version .*$", "", line)


def spdx_ids(text):
    ids = set()
    for m in re.finditer(r"^License:\s*(.+)$", text, re.M):
        for t in re.split(r"\s+(?:or|and|OR|AND)\s+", m.group(1).strip()):
            t = t.strip("() ,")
            if t and len(t) < 40:
                ids.add(t)
    return sorted(ids)


SYNTH_MIT = """Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE."""

SYNTH_BSD3 = """Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."""


# a licence file that contains any of these is a licence, not a header that points at one
FULL_TEXT_MARKERS = ("Permission is hereby granted", "Permission to use, copy", "Redistribution and use", "TERMS AND CONDITIONS FOR USE, REPRODUCTION",
                     "provided 'as-is'", "provided \"as-is\"", "free and unencumbered", "THE SOFTWARE IS PROVIDED", "GNU GENERAL PUBLIC",
                     "Mozilla Public License Version 2.0", "Unicode License", "UNICODE LICENSE")


def synth_text(c):
    """The licence a licence-file-less crate offers, taken from its SPDX field (MIT first, then Apache-2.0, BSD-3-Clause)."""
    toks = set(re.split(r"[\s/()]+", c["license"]))
    if "MIT" in toks:
        return "MIT licence (chosen from the crate's SPDX field %r)\n\n%s\n" % (c["license"], SYNTH_MIT)
    if "Apache-2.0" in toks:
        return "Apache-2.0 (chosen from the crate's SPDX field %r)\n\n%s" % (c["license"], read(os.path.join(DATA, "texts", "common-Apache-2.0.txt")))
    if "MPL-2.0" in toks:
        return "MPL-2.0 (the crate's SPDX field %r); the Mozilla Public License covers the crate's own files, whose source is the crates.io release named above.\n\n%s" % (c["license"], read(os.path.join(DATA, "texts", "common-MPL-2.0.txt")))
    if "BSD-3-Clause" in toks:
        return "BSD-3-Clause (crate's SPDX field %r)\n\n%s\n" % (c["license"], SYNTH_BSD3)
    raise SystemExit("error: crate %s has no licence file and SPDX %r has no synthesis rule" % (c["name"], c["license"]))


def gen():
    comps, crates, debian = load()
    texts, users = {}, {}      # sha -> body ; sha -> [owner names]

    def reg(body, who):
        body = body.rstrip() + "\n"
        k = hashlib.sha256(body.encode()).hexdigest()
        texts.setdefault(k, body)
        users.setdefault(k, [])
        if who not in users[k]:
            users[k].append(who)
        return k

    # pass 1: register every text so licence numbers are known before anything is written
    for c in comps:
        c["_refs"] = [reg(read(os.path.join(DATA, "texts", t)), c["name"]) for t in c["texts"]]
    dtext = {}
    for pk in sorted(debian):
        v = debian[pk]
        dtext[pk] = read(os.path.join(DATA, "texts", "debian", v["text"] + ".txt")) if v["text"] else ""
        v["_ref"] = reg(dtext[pk], "Debian package " + pk) if v["text"] else None
    for c in crates:
        c["_refs"] = [reg(read(os.path.join(DATA, "texts", "crates", t + ".txt")), "Rust crate " + c["name"]) for t in c["texts"]]
    for c in crates:
        # no licence file at all, or only a short header/pointer ("licensed under MIT or Apache-2.0, see ..."): add the full text
        c["_pointer"] = bool(c["_refs"]) and not any(m in texts[k] for k in c["_refs"] for m in FULL_TEXT_MARKERS)
        if not c["_refs"] or c["_pointer"]:
            c["_syn"] = reg(synth_text(c), "Rust crate " + c["name"] + " (synthesised)")
    order = sorted(texts, key=lambda k: (users[k][0].lower(), k))
    num = {k: i + 1 for i, k in enumerate(order)}

    out = []
    w = out.append
    w("THIRD-PARTY NOTICES - GeneralsX")
    w("=" * 78)
    w(HEADER.strip("\n"))
    w("")
    w("1. COMPONENTS BUILT INTO OR SHIPPED WITH THE GAME AND LAUNCHER")
    w("=" * 78)
    for c in comps:
        w("")
        w("%s %s" % (c["name"], c["version"]))
        w("  Licence : %s" % c["license"])
        w("  Project : %s" % c["url"])
        w("  Used in : %s; platforms: %s" % (c["where"], ", ".join(c["platforms"])))
        for line in c["copyright"]:
            w("  " + line)
        if c["_refs"]:
            w("  Text    : " + " ".join("[L%d]" % num[k] for k in c["_refs"]))
        if c["note"]:
            w("  Note    : " + c["note"])
    w("")
    w("2. DEBIAN / UBUNTU PACKAGES BUNDLED IN THE LINUX TARBALL AND THE LINUX LAUNCHER APPIMAGE")
    w("=" * 78)
    w("\nEach package's own copyright file (verbatim, from /usr/share/doc/<package>/copyright of the\n"
      "build host) is reproduced in section 4; packages with identical files share one copy.\n")
    for pk in sorted(debian):
        v = debian[pk]
        lic = spdx_ids(dtext[pk])
        w("%s %s" % (pk, v["version"]))
        w("  Licences named in its copyright file: %s" % ((", ".join(lic[:14]) + (" ..." if len(lic) > 14 else "")) if lic else "see text"))
        if v["homepage"]:
            w("  Project : %s" % v["homepage"])
        w("  Source  : %s %s (Ubuntu source package)" % (v["source"], v["source_version"]))
        w("  Files   : %s" % (", ".join(v["files"][:6]) + (" ..." if len(v["files"]) > 6 else "")))
        w("  Text    : %s" % ("[L%d]" % num[v["_ref"]] if v["_ref"] else "(none shipped by Debian)"))
    w("")
    w("3. RUST CRATES COMPILED INTO THE GENERALSX LAUNCHER (scripts/launcher-rs, Cargo.lock)")
    w("=" * 78)
    w("\nSuperset over all target platforms (the lock file also resolves Windows, macOS and Android-only\n"
      "crates). Where a crate's SPDX expression offers a choice, the choice compatible with GPL-3.0 is\n"
      "taken (MIT or Apache-2.0; for MPL-2.0 crates the MPL source is the crates.io release named here).\n"
      "A crate whose package carries no licence file is marked; its licence is the SPDX field and its\n"
      "copyright holders are the authors named.\n")
    for c in crates:
        auth = "; ".join(c["authors"][:3]) + (" ..." if len(c["authors"]) > 3 else "")
        line = "%s %s - %s" % (c["name"], c["version"], c["license"] or "(none declared)")
        if c["_refs"]:
            refs = sorted(set(c["_refs"]), key=lambda k: num[k])
            if c["_pointer"]:
                refs.append(c["_syn"])
            line += "  " + " ".join("[L%d]" % num[k] for k in refs)
        else:
            line += "  [no licence file in the crate package; text synthesised from its SPDX field] [L%d]" % num[c["_syn"]]
            w(line)
            w("    Copyright (c) %s" % (auth or "the %s authors (see %s)" % (c["name"], c["repository"] or "crates.io")))
            continue
        w(line)
    w("")
    w("4. LICENCE TEXTS (each once)")
    w("=" * 78)
    for k in order:
        w("")
        w("-" * 78)
        names = users[k]
        shown = ", ".join(names[:8]) + (" and %d more" % (len(names) - 8) if len(names) > 8 else "")
        w("[L%d] used by: %s" % (num[k], shown))
        w("-" * 78)
        w(texts[k].rstrip("\n"))
    return "\n".join(out) + "\n"


def cmd_generate():
    body = gen()  # build it all first: a failing generator must not truncate the committed file
    open(OUTFILE, "w", encoding="utf-8", newline="\n").write(body)
    print("wrote", OUTFILE, os.path.getsize(OUTFILE), "bytes")


def cargo_lock_external():
    pk, name, ver, src = set(), None, None, None
    for line in open(os.path.join(ROOT, "scripts", "launcher-rs", "Cargo.lock"), encoding="utf-8"):
        line = line.strip()
        if line == "[[package]]":
            if name and src:
                pk.add((name, ver))
            name = ver = src = None
        elif line.startswith("name = ") and '"' in line:
            name = line.split('"')[1]
        elif line.startswith("version = ") and '"' in line:
            ver = line.split('"')[1]
        elif line.startswith("source = ") and '"' in line:
            src = line.split('"')[1]
    if name and src:
        pk.add((name, ver))
    return pk


def cmd_check():
    _, crates, _ = load()
    have_c, lock = {(c["name"], c["version"]) for c in crates}, cargo_lock_external()
    if have_c != lock:
        sys.exit("FAIL: crates.json differs from scripts/launcher-rs/Cargo.lock (missing from notices: %s; no longer in the lock: %s); "
                 "re-run collect_third_party_data.py crates" % (sorted(lock - have_c)[:8], sorted(have_c - lock)[:8]))
    want = gen()
    have = open(OUTFILE, encoding="utf-8").read() if os.path.exists(OUTFILE) else None
    if have != want:
        sys.exit("FAIL: THIRD_PARTY_NOTICES.txt is %s; run: python3 scripts/legal/third_party_notices.py generate" % ("missing" if have is None else "stale"))
    print("OK: THIRD_PARTY_NOTICES.txt is current (%d bytes)" % len(want))


def check_rows(rows, label, platform):
    owner, owner_ref = matcher(platform), matcher(platform, allow_system=True)
    bad, n = set(), 0
    for raw in rows:
        f = clean_row(raw)
        referenced = f != raw.strip()  # an `otool -L` row names a dependency; it is not a file in the payload
        if referenced and f.startswith(("/System/", "/usr/lib/")):
            continue  # the operating system's own, named by an otool -L row
        if f and is_lib(f):
            n += 1
            if (owner_ref if referenced else owner)(f) is None:
                bad.add(os.path.basename(f.rstrip("/")))
    if bad:
        sys.exit("FAIL: %s: %d library file(s) belong to no component in resources/legal/third-party:\n  %s"
                 % (label, len(bad), "\n  ".join(sorted(bad))))
    if n == 0:
        sys.exit("FAIL: %s: no library-like file recognised - an empty or unparsed listing proves nothing" % label)
    print("OK: %s: %d library-like files, every one owned by a component" % (label, n))


PLATFORMS = ("macos", "ios", "linux", "windows")
HOST_VERSIONS = os.environ.get("GX_NOTICES_HOST_VERSIONS", "1") != "0"  # env 0 (GitHub-hosted CI) or --no-host-versions: skip the build-host Ubuntu version/byte comparison (the GitHub CI fallback bundle)


def split_platform(args):
    global HOST_VERSIONS
    if args[:1] == ["--no-host-versions"]:
        HOST_VERSIONS = False
        args = args[1:]
    if len(args) > 2 and args[0] == "--platform" and args[1] in PLATFORMS:
        return args[1], args[2:]
    return None, args


def cmd_check_listing(args):
    platform, paths = split_platform(args)
    for p in paths:
        check_rows(open(p, errors="replace").read().splitlines(), p, platform)


def cmd_check_path(args):
    import tarfile, zipfile
    platform, paths = split_platform(args)
    for p in paths:
        if os.path.isdir(p):
            rows = [os.path.relpath(os.path.join(d, f), p) for d, ds, fs in os.walk(p) for f in fs + ds]
        elif zipfile.is_zipfile(p):
            rows = zipfile.ZipFile(p).namelist()
        elif tarfile.is_tarfile(p):
            rows = tarfile.open(p).getnames()
        else:
            sys.exit("FAIL: %s is not a directory, zip or tar archive" % p)
        check_rows(rows, p, platform)
        if platform == "linux" and os.path.isdir(p) and HOST_VERSIONS:
            check_host_versions(p)
        if platform == "macos" and os.path.isdir(p) and HOST_VERSIONS:
            check_host_brew(p)


PACKAGER_PLATFORM = {"macos": "macos", "ios": "ios", "linux": "linux", "windows": "windows", "flatpak": "linux"}


def packager_platform(rel):
    for k, v in PACKAGER_PLATFORM.items():
        if k in rel:
            return v
    return None


def check_host_versions(d):
    """On a Debian/Ubuntu build host, the package behind each bundled library must be the version debian.json (and so
    the notices' corresponding-source line) names. Skipped where there is no dpkg (a non-Debian host cannot supply them)."""
    import shutil, subprocess
    if not shutil.which("dpkg-query"):
        print("note: no dpkg on this host; Debian package versions not compared")
        return
    _, _, debian = load()
    owner = matcher("linux")
    paths = sorted(os.path.join(dp, f) for dp, _, fs in os.walk(d) for f in fs
                   if soname(f) is not None and (owner(f) or "").startswith("debian:"))
    # an unversioned libfoo.so is a development symlink to a versioned sibling in the same payload: that sibling carries the check
    versioned = {soname(os.path.basename(x))[0] for x in paths if soname(os.path.basename(x))[1]}
    paths = [x for x in paths if soname(os.path.basename(x))[1] or soname(os.path.basename(x))[0] not in versioned]
    names = sorted({os.path.basename(x) for x in paths})
    pkgs, bad = set(), []
    host = {}  # basename -> [(package, path on this host)]
    if names:
        # ONE dpkg -S for all of them: each call scans every package's file list
        r = subprocess.run(["dpkg", "-S"] + ["*/" + f for f in names], capture_output=True, text=True)
        for line in r.stdout.splitlines():
            pk, _, path = line.partition(": ")
            pk = pk.split(":")[0]
            if pk in debian:
                host.setdefault(os.path.basename(path), []).append((pk, path))
    # fail closed, and by CONTENT: each bundled file must be byte-identical to a file a recorded package installed on this
    # host (a same-named copy from /usr/local would otherwise be accepted as the Ubuntu package's)
    import filecmp
    unowned = []
    for x in paths:
        hit = [pk for pk, hp in host.get(os.path.basename(x), []) if os.path.exists(hp) and filecmp.cmp(x, hp, shallow=False)]
        if hit:
            pkgs.add(hit[0])
        else:
            unowned.append(os.path.relpath(x, d))
    if unowned:
        sys.exit("FAIL: these bundled libraries are not byte-identical to a file of any Ubuntu package recorded in the notices "
                 "(built from somewhere else, or the lookup failed):\n  " + "\n  ".join(unowned))
    for pk in sorted(pkgs):
        have = subprocess.run(["dpkg-query", "-W", "-f=${Version}", pk], capture_output=True, text=True).stdout
        if have != debian[pk]["version"]:
            bad.append("%s: host has %s, notices name %s" % (pk, have, debian[pk]["version"]))
    if bad:
        sys.exit("FAIL: bundled Ubuntu packages differ from the versions in the notices (re-run collect_third_party_data.py debian "
                 "for this host, then generate):\n  " + "\n  ".join(bad))
    print("OK: %d bundled Ubuntu packages match the versions in the notices" % len(pkgs))


def check_host_brew(d):
    """macOS payloads copy Homebrew dylibs by name; the component data records the formula version. The Homebrew formula
    installed on THIS host must be the recorded one, or the notice would name the wrong version and source."""
    import shutil, subprocess
    if not shutil.which("brew"):
        print("note: no brew on this host; Homebrew versions not compared")
        return
    comps, _, _ = load()
    byid = {c["id"]: c for c in comps if c.get("brew")}
    owner = matcher("macos")
    used = set()
    staged = []  # (component id, staged dylib path)
    for dp, _, fs in os.walk(d):
        for f in fs:
            o = owner(f)
            if o in byid:
                used.add(o)
                if f.lower().endswith(".dylib"):  # a symlink is compared by what it resolves to (macho_identity follows it)
                    staged.append((o, os.path.join(dp, f)))
    bad = []
    kegs = {}
    for cid in sorted(used):
        formula, prefix = byid[cid]["brew"]
        have = subprocess.run(["brew", "list", "--versions", formula], capture_output=True, text=True).stdout.split()[1:]
        # the complete upstream version, allowing Homebrew's "_N" revision suffix; a recorded value ending in "." is a series
        hit = [v for v in have if (v.startswith(prefix) if prefix.endswith(".") else re.fullmatch(re.escape(prefix) + r"(_[0-9]+)?", v))]
        cellar = subprocess.run(["brew", "--cellar", formula], capture_output=True, text=True).stdout.strip()
        # the kegs of the RECORDED version only: the linked keg (brew --prefix) may be a different, newer install
        kegs[cid] = [os.path.join(cellar, v) for v in hit] if cellar else []
        if not hit:
            bad.append("%s: Homebrew has %s, the notices name %s" % (formula, " ".join(have) or "(not installed)", prefix))
    if bad:
        sys.exit("FAIL: Homebrew libraries differ from the versions in the notices (update components.json and regenerate):\n  " + "\n  ".join(bad))
    # PROVENANCE: the formula being installed does not make the staged file that formula's (it may come from /usr/local, the
    # LunarG SDK or an older build). install_name_tool rewrites ids and paths, so bytes cannot be compared; the Mach-O
    # identity that survives is the id's compatibility/current version and the NAMES of the dylibs it links against.
    foreign = []
    for cid, path in sorted(staged):
        formula = byid[cid]["brew"][0]
        ours = macho_identity(path)
        theirs = [macho_identity(k) for k in (os.path.join(kd, "lib", os.path.basename(path)) for kd in kegs[cid]) if os.path.isfile(k)]
        theirs = [t for t in theirs if t is not None]
        if ours is None or not theirs:
            foreign.append("%s: cannot compare with the recorded %s keg (%s)" % (os.path.relpath(path, d), formula,
                           "not a Mach-O dylib with an LC_UUID" if ours is None else "no %s in the recorded-version keg" % os.path.basename(path)))
        elif ours not in theirs:
            foreign.append("%s: not the recorded-version %s keg's library (staged %s)" % (os.path.relpath(path, d), formula, ours))
    if foreign:
        sys.exit("FAIL: staged Homebrew libraries are not the installed formula's (copied from somewhere else?):\n  " + "\n  ".join(foreign))
    print("OK: %d Homebrew formulae match the versions in the notices; %d staged dylibs match their kegs" % (len(used), len(staged)))


def macho_identity(path):
    """(per-architecture LC_UUID list, id compatibility/current version, sorted dependency basenames), or None if the file
    is not a readable dylib with an LC_UUID. The UUID identifies the build and survives install_name_tool, which changes
    only the id and dependency PATHS; versions alone do not (two OpenSSL point releases share them)."""
    import subprocess
    r = subprocess.run(["otool", "-L", path], capture_output=True, text=True)
    rows = [l.strip() for l in r.stdout.splitlines()[1:] if l.strip()]
    if r.returncode != 0 or not rows:
        return None
    m = re.match(r"(\S+) \((compatibility version [0-9.]+, current version [0-9.]+)\)", rows[0])
    l = subprocess.run(["otool", "-l", path], capture_output=True, text=True)
    uuids = tuple(re.findall(r"cmd LC_UUID\s+cmdsize \d+\s+uuid ([0-9A-Fa-f-]+)", l.stdout))
    if not m or l.returncode != 0 or not uuids:
        return None
    return (uuids, m.group(2), tuple(sorted({os.path.basename(x.split(" (")[0]) for x in rows[1:]})))


def cmd_check_packagers():
    problems = []
    lib = re.compile(r"(?<![A-Za-z0-9_.-])((?:lib)?[A-Za-z0-9_+-]+(?:\.[0-9]+)*\.(?:so|dylib|dll)(?:\.[0-9]+)*)")
    for rel in PACKAGERS:
        p = os.path.join(ROOT, rel)
        if not os.path.exists(p):
            problems.append("%s: packager is missing (update PACKAGERS)" % rel)
            continue
        t = read(p)
        owner = matcher(packager_platform(rel), allow_system=True)
        code = re.sub(r"(?m)^\s*(#|;|//).*$", "", t)  # a mention in a comment is not a copy
        acts = [l for l in code.split("\n") if "THIRD_PARTY_NOTICES.txt" in l and not re.match(r"\s*(echo|printf)\b", l)]
        if not acts:
            problems.append("%s: does not copy THIRD_PARTY_NOTICES.txt" % rel)
        for m in sorted(set(lib.findall(code))):
            if "*" in m or "$" in m:
                continue
            if owner(m) is None:
                problems.append("%s: names %s which no component owns" % (rel, m))
    if problems:
        sys.exit("FAIL:\n  " + "\n  ".join(problems))
    print("OK: %d packagers copy the notices; every library they name is owned" % len(PACKAGERS))


if __name__ == "__main__":
    a = sys.argv[1:]
    if a == ["generate"]:
        cmd_generate()
    elif a == ["check"]:
        cmd_check()
    elif a[:1] == ["check-listing"] and len(a) > 1:
        cmd_check_listing(a[1:])
    elif a[:1] == ["check-path"] and len(a) > 1:
        cmd_check_path(a[1:])
    elif a == ["check-packagers"]:
        cmd_check_packagers()
    else:
        sys.exit(__doc__)
