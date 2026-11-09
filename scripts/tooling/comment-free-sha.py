#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Hash a tree's C, C++ and Objective-C sources with comments and layout removed.

Usage:
    python3 scripts/tooling/comment-free-sha.py <commit> [<file-list commit>] [--per-file]

Takes every .c .cc .cpp .cxx .h .hh .hpp .inl .m .mm file of <file-list commit> (default: <commit>),
reads each from <commit>, removes // and /* */ comments (string and character literals, raw strings and
backslash-continued // comments are respected), drops blank lines and trims each line, and prints one
sha256 over "path <tab> sha256 of the result" for all files, sorted by path. Two trees print the same value
only if every one of those files is the same once comments, indentation and blank lines are ignored.
--per-file also prints each file's own hash. SOURCE.md says which value to expect for which tag.
"""
import hashlib
import re
import subprocess
import sys

EXT = re.compile(r"\.(c|cc|cpp|cxx|h|hh|hpp|inl|m|mm)$", re.IGNORECASE)


def git(*args):
    return subprocess.run(["git", "-c", "core.quotePath=false", *args], check=True,
                          capture_output=True).stdout


def strip_comments(src):
    """Return src with every comment replaced by one space; literals are copied untouched."""
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            j = i + 2
            while j < n and src[j] != "\n":
                j += 2 if src[j] == "\\" and j + 1 < n else 1
            out.append(" ")
            i = j
        elif c == "/" and nxt == "*":
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(" ")
            i = j
        elif c == "R" and nxt == '"' and (i == 0 or not (src[i - 1].isalnum() or src[i - 1] == "_")
                                          or src[i - 1] in "uU8L"):
            m = re.match(r'R"([^()\\ \t\n]{0,16})\(', src[i:])
            if not m:
                out.append(c)
                i += 1
                continue
            end = src.find(")" + m.group(1) + '"', i + m.end())
            end = n if end < 0 else end + len(m.group(1)) + 2
            out.append(src[i:end])
            i = end
        elif c in "\"'":
            j = i + 1
            while j < n and src[j] != c and src[j] != "\n":
                j += 2 if src[j] == "\\" else 1
            out.append(src[i:j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def normalise(src):
    lines = (line.strip() for line in strip_comments(src).split("\n"))
    return "\n".join(line for line in lines if line)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    per_file = "--per-file" in sys.argv
    commit = args[0]
    listed = args[1] if len(args) > 1 else commit
    paths = sorted(p for p in git("ls-tree", "-r", "-z", "--name-only", listed).decode("utf-8").split("\0")
                   if p and EXT.search(p))
    total = hashlib.sha256()
    for path in paths:
        text = git("cat-file", "blob", f"{commit}:{path}").decode("utf-8", "surrogateescape")
        h = hashlib.sha256(normalise(text).encode("utf-8", "surrogateescape")).hexdigest()
        total.update(f"{path}\t{h}\n".encode("utf-8"))
        if per_file:
            print(h, path)
    print(f"{total.hexdigest()}  {len(paths)} files")


if __name__ == "__main__":
    main()
