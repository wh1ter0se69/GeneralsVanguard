#!/usr/bin/env python3
"""Checks for pull requests. Standard library and git only.

    pr_checks.py files   BASE HEAD   binaries, executables, large files, symlinks, submodules, automation changes
    pr_checks.py needles BASE HEAD   tool signatures and private details in added lines and commit messages

Exit 1 when something must be fixed, 0 otherwise. Warnings point the reviewer at a change without failing.
Findings name the rule, file and line only; text from the pull request is never echoed, so it cannot inject
workflow commands into the log.
"""
import os
import re
import subprocess
import sys

MAX_BYTES = 1024 * 1024

# Paths only the maintainer changes; a pull request that edits them fails and needs a direct review.
OWNER_ONLY = (".github/", ".gitmodules", ".gitattributes")

# Paths where a bad change does the most harm; flagged for a careful review, not failed.
SENSITIVE = (
    "CMakeLists.txt", "CMakePresets.json", "cmake/", "triplets/", "vcpkg.json", "vcpkg-lock.json", "Patches/",
    "Dependencies/", "resources/", "scripts/build/", "scripts/get-assets.sh", "ios/",
)
SENSITIVE_RE = re.compile(r"(^|/)(GameNetwork/|GXRelay|GXLobby|GXPlayerIdentity|GXEd25519|GXSha512|GXIdentityAssertion|GXSignOutFiles|GXDiscord)")

# File types that are never accepted: compiled code, installers, archives, retail game archives.
REFUSED_EXT = re.compile(
    r"\.(exe|dll|so|dylib|a|lib|obj|o|pdb|msi|pkg|dmg|ipa|apk|app|jar|class|wasm|bin|big|zip|7z|rar|gz|tgz|bz2|xz|zst"
    r"|cab|iso)$", re.I)
EXEC_MAGIC = (b"\x7fELF", b"MZ", b"\xfe\xed\xfa\xce", b"\xfe\xed\xfa\xcf", b"\xce\xfa\xed\xfe", b"\xcf\xfa\xed\xfe",
              b"\xca\xfe\xba\xbe", b"\x00asm", b"PK\x03\x04", b"7z\xbc\xaf", b"Rar!", b"\x1f\x8b", b"BIGF", b"BIG4")
# Binary = a NUL or control byte anywhere in the file (tab, line and page breaks excepted). No text file in the tree
# has one. Images are binary too: contributors show them in the pull request description, the maintainer adds files.
BINARY_BYTES = re.compile(rb"[\x00-\x08\x0e-\x1f\x7f]")
SVG_EXT = re.compile(r"\.svg$", re.I)

AI_NAMES = r"(claude|anthropic|chatgpt|openai|copilot|codex|gemini|cursor|devin|aider|llm|gpt-?\d)"
# Tool or assistant signatures: fail in added lines and in commit messages.
SIGNATURES = [
    ("co-author line naming a tool", re.compile(r"co-authored-by:[^\n]*" + AI_NAMES, re.I)),
    ("'generated with/by' line", re.compile(r"generated (with|by)\b[^\n]{0,40}\b(" + AI_NAMES[1:-1] + r"|ai)\b", re.I)),
    ("assistant or vendor name", re.compile(r"\b(claude|anthropic|chatgpt|openai|copilot)\b", re.I)),
    ("assistant name", re.compile(r"\bCodex\b")),
    ("assistant phrasing", re.compile(r"\bas an ai( language)? model\b", re.I)),
    ("robot emoji signature", re.compile("\U0001F916")),
]
# Private details: warn in added lines; the reviewer decides.
PRIVATE = [
    ("home directory path", re.compile(
        r"(/Users/|/home/|[A-Za-z]:\\{1,2}Users\\{1,2})"
        r"(?!((user|username|you|yourname|me|runner|public|default|shared)([/\\\s\"'`)]|$)|[$<{%]))"
        r"[A-Za-z0-9._-]+", re.I)),
    ("private network address", re.compile(
        r"\b(10\.\d{1,3}\.\d{1,3}\.\d{1,3}|192\.168\.\d{1,3}\.\d{1,3}|172\.(1[6-9]|2\d|3[01])\.\d{1,3}\.\d{1,3}"
        r"|100\.(6[4-9]|[7-9]\d|1[01]\d|12[0-7])\.\d{1,3}\.\d{1,3})\b")),
    ("private network host name", re.compile(r"\.ts\.net\b|\.tailscale\b|\.internal\b|\.lan\b", re.I)),
    ("e-mail address", re.compile(
        r"[A-Za-z0-9._%+-]+@(?!(users\.noreply\.github\.com|example\.(com|org|net)|fsf\.org)\b)"
        r"[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}")),
]


def git(*args, text=True):
    return subprocess.run(["git", "-c", "core.quotePath=false", *args], check=True, capture_output=True,
                          text=text).stdout


def esc_data(s):
    return s.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def esc_prop(s):
    return esc_data(s).replace(":", "%3A").replace(",", "%2C")


class Report:
    def __init__(self, title):
        self.title, self.errors, self.warnings = title, [], []

    def add(self, level, path, line, msg):
        (self.errors if level == "error" else self.warnings).append((path, line, msg))
        loc = "file=" + esc_prop(path) + ("," + "line=%d" % line if line else "") if path else ""
        print("::%s %s::%s" % (level, loc, esc_data(msg)) if loc else "::%s::%s" % (level, esc_data(msg)))

    def finish(self):
        md = ["### " + self.title, ""]
        if not self.errors and not self.warnings:
            md.append("Nothing found.")
        for kind, rows in (("Must fix", self.errors), ("Review carefully", self.warnings)):
            if rows:
                md += ["**%s**" % kind, "", "| File | Line | Finding |", "|---|---|---|"]
                md += ["| %s | %s | %s |" % (cell(p or "(commit message)"), l or "", cell(m)) for p, l, m in rows]
                md.append("")
        out = os.environ.get("GITHUB_STEP_SUMMARY")
        if out:
            with open(out, "a", encoding="utf-8") as fh:
                fh.write("\n".join(md) + "\n")
        print("%s: %d to fix, %d to review" % (self.title, len(self.errors), len(self.warnings)))
        return 1 if self.errors else 0


def cell(s):
    return s.replace("\\", "\\\\").replace("|", "\\|").replace("`", "'").replace("\n", " ")[:200]


def changed(base, head):
    """(status, old_mode, new_mode, new_sha, path, old_path) for every change; renames and copies are detected, so a
    file that only moved is not checked again, but both of its paths are."""
    raw = git("diff", "--raw", "-z", "-M", "-C", "--no-abbrev", base, head, text=False).split(b"\0")
    out, i = [], 0
    while i < len(raw) - 1:
        meta = raw[i].decode().lstrip(":").split()
        status = meta[4]
        old_path = raw[i + 1].decode("utf-8", "replace")
        if status[0] in "RC":
            path = raw[i + 2].decode("utf-8", "replace")
            i += 3
        else:
            path = old_path
            i += 2
        out.append((status, meta[0], meta[1], meta[3], path, old_path))
    return out


def check_files(base, head):
    rep = Report("Files")
    for status, old_mode, new_mode, sha, path, old_path in changed(base, head):
        for p in {path, old_path}:
            if p.startswith(OWNER_ONLY) or os.path.basename(p) == ".gitattributes":
                rep.add("error", p, 0, "changes automation, attributes or submodules; only the maintainer changes these")
            elif p.startswith(SENSITIVE) or SENSITIVE_RE.search(p):
                rep.add("warning", p, 0, "build, packaging, installer or network code: review line by line")
        if status[0] == "D":
            continue
        if new_mode == "120000":
            rep.add("error", path, 0, "symbolic link")
            continue
        if new_mode == "160000":
            rep.add("error", path, 0, "submodule pointer")
            continue
        if status == "R100":
            continue
        size = int(git("cat-file", "-s", sha))
        data = subprocess.run(["git", "cat-file", "blob", sha], check=True, capture_output=True).stdout
        if REFUSED_EXT.search(path) or data.startswith(EXEC_MAGIC):
            rep.add("error", path, 0, "binary, executable or archive: build inputs must be source")
        elif BINARY_BYTES.search(data):
            rep.add("error", path, 0, "binary file (images too): show images in the pull request description; "
                                      "the maintainer adds binary files")
        elif SVG_EXT.search(path):
            rep.add("warning", path, 0, "SVG image: check it is the contributor's own work and has no script")
        if size > MAX_BYTES:
            rep.add("error", path, 0, "file larger than 1 MiB (%d bytes)" % size)
        if new_mode == "100755" and old_mode != "100755":
            rep.add("warning", path, 0, "file made executable")
    return rep.finish()


def added_lines(base, head):
    """(path, line number, text) for every added line of every text file."""
    # --text and --no-textconv: a pull request's .gitattributes must not turn its own changes into "Binary files
    # differ" and hide them from this scan.
    path, num, old_left, new_left = None, 0, 0, 0
    diff = git("diff", "-U0", "--text", "--no-textconv", "--no-ext-diff", "--no-color", "-M", base, head, text=False)
    for raw in diff.split(b"\n"):
        line = raw.decode("utf-8", "replace")
        if old_left or new_left:  # inside a hunk: "+++ x" here is an added line that starts with "++"
            if line.startswith("+"):
                if path:
                    yield path, num, line[1:]
                num += 1
                new_left -= 1
            elif line.startswith("-"):
                old_left -= 1
            elif line.startswith(" "):
                old_left, new_left, num = old_left - 1, new_left - 1, num + 1
        elif line.startswith("+++ "):
            name = line[4:]
            if len(name) > 1 and name[0] == name[-1] == '"':
                name = name[1:-1]
            path = name[2:] if name.startswith("b/") else None
        elif line.startswith("@@"):
            m = re.match(r"@@ -\d+(?:,(\d+))? \+(\d+)(?:,(\d+))? @@", line)
            if m:
                old_left = int(m.group(1)) if m.group(1) is not None else 1
                num = int(m.group(2))
                new_left = int(m.group(3)) if m.group(3) is not None else 1


def check_needles(base, head, messages):
    rep = Report("Signatures and private details")
    for path, num, text in added_lines(base, head):
        for name, rx in SIGNATURES:
            if rx.search(text):
                rep.add("error", path, num, name + ": remove tool signatures")
                break
        for name, rx in PRIVATE:
            if rx.search(text):
                rep.add("warning", path, num, name + ": check it is not personal or private")
                break
    for i, msg in enumerate(messages, 1):
        for name, rx in SIGNATURES:
            if rx.search(msg):
                rep.add("error", "", 0, "commit %d of the pull request: %s: remove tool signatures" % (i, name))
                break
    return rep.finish()


def read_messages(path):
    if not path:
        return []
    with open(path, encoding="utf-8", errors="replace") as fh:
        return [m for m in fh.read().split("\0") if m.strip()]


def main(argv):
    if len(argv) < 4 or argv[1] not in ("files", "needles"):
        print(__doc__.strip())
        return 2
    base, head = argv[2], argv[3]
    if argv[1] == "files":
        return check_files(base, head)
    return check_needles(base, head, read_messages(argv[4] if len(argv) > 4 else ""))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
