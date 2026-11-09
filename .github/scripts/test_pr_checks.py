#!/usr/bin/env python3
"""Tests for pr_checks.py: every rule is seen to fire on a bad change and to stay quiet on a clean one.

    python3 .github/scripts/test_pr_checks.py
"""
import os
import subprocess
import sys
import tempfile
import unittest
import zlib

SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pr_checks.py")
ELF = b"\x7fELF\x02\x01\x01" + b"\0" * 64
XZ = b"\xfd7zXZ\x00\x00\x04\xe6\xd6\xb4\x46" + b"\0" * 32


def png_1x1():
    def chunk(kind, body):
        return len(body).to_bytes(4, "big") + kind + body + zlib.crc32(kind + body).to_bytes(4, "big")
    ihdr = (1).to_bytes(4, "big") * 2 + bytes([8, 2, 0, 0, 0])
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(b"\0\0\0\0")) + chunk(b"IEND", b"")


class Repo:
    def __init__(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = self.tmp.name
        self.git("init", "-q", "-b", "main")
        self.write("src/game.cpp", "int main() { return 0; }\n")
        self.write("README.md", "hello\n")
        self.base = self.commit("base")

    def git(self, *args):
        env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.com", GIT_COMMITTER_NAME="t",
                   GIT_COMMITTER_EMAIL="t@example.com")
        return subprocess.run(["git", *args], cwd=self.path, check=True, capture_output=True, env=env).stdout

    def write(self, rel, data, mode=None):
        full = os.path.join(self.path, rel)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as fh:
            fh.write(data.encode() if isinstance(data, str) else data)
        if mode:
            os.chmod(full, mode)

    def commit(self, msg):
        self.git("add", "-A")
        self.git("commit", "-q", "--allow-empty", "-m", msg)
        return self.git("rev-parse", "HEAD").decode().strip()

    def run(self, kind, head, messages=None):
        args = [sys.executable, SCRIPT, kind, self.base, head]
        if messages is not None:
            mfile = os.path.join(self.path, ".git", "messages")
            with open(mfile, "w", encoding="utf-8") as fh:
                fh.write("\0".join(messages))
            args.append(mfile)
        env = dict(os.environ)
        env.pop("GITHUB_STEP_SUMMARY", None)
        p = subprocess.run(args, cwd=self.path, capture_output=True, text=True, env=env)
        return p.returncode, p.stdout


class FilesCheck(unittest.TestCase):
    def check(self, setup, want_rc, want_text=None):
        r = Repo()
        self.addCleanup(r.tmp.cleanup)
        setup(r)
        rc, out = r.run("files", r.commit("change"))
        self.assertEqual(rc, want_rc, out)
        if want_text:
            self.assertIn(want_text, out)
        return out

    def test_clean_text_change_passes(self):
        out = self.check(lambda r: r.write("src/game.cpp", "int main() { return 1; }\n"), 0)
        self.assertNotIn("::error", out)

    def test_elf_binary_fails(self):
        self.check(lambda r: r.write("tools/helper", ELF), 1, "binary, executable or archive")

    def test_dll_extension_fails(self):
        self.check(lambda r: r.write("lib/x.dll", "not really a dll\n"), 1, "binary, executable or archive")

    def test_big_archive_fails(self):
        self.check(lambda r: r.write("Data/INIZH.big", b"BIGF" + b"\0" * 32), 1, "binary, executable or archive")

    def test_unknown_binary_fails(self):
        self.check(lambda r: r.write("data/blob.dat", b"\x01\x02\0\x03"), 1, "binary file")

    def test_large_text_fails(self):
        self.check(lambda r: r.write("src/huge.txt", "x" * (1024 * 1024 + 1)), 1, "larger than 1 MiB")

    def test_symlink_fails(self):
        def setup(r):
            os.symlink("/etc/passwd", os.path.join(r.path, "link"))
        self.check(setup, 1, "symbolic link")

    def test_workflow_change_fails(self):
        self.check(lambda r: r.write(".github/workflows/x.yml", "on: push\n"), 1, "only the maintainer")

    def test_gitmodules_change_fails(self):
        self.check(lambda r: r.write(".gitmodules", "[submodule \"x\"]\n"), 1, "only the maintainer")

    def test_png_is_for_the_maintainer(self):
        self.check(lambda r: r.write("docs/shot.png", png_1x1()), 1, "binary file (images too)")

    def test_svg_warns_only(self):
        out = self.check(lambda r: r.write("docs/logo.svg", '<svg xmlns="http://www.w3.org/2000/svg"/>\n'), 0, "SVG")
        self.assertIn("::warning", out)

    def test_archive_named_as_image_fails(self):
        self.check(lambda r: r.write("docs/payload.png", XZ), 1, "binary")

    def test_archive_after_a_long_text_prefix_fails(self):
        self.check(lambda r: r.write("docs/notes.txt", b"a" * 9000 + b"PK\x03\x04\x14\x00\x00\x00"), 1, "binary file")

    def test_one_control_byte_fails(self):
        self.check(lambda r: r.write("src/x.cpp", "int x;\x01\n"), 1, "binary file")

    def test_tabs_and_line_breaks_pass(self):
        self.check(lambda r: r.write("src/x.cpp", "int\tx;\r\n\x0c\x0b\n"), 0)

    def test_network_code_warns_only(self):
        out = self.check(lambda r: r.write("Core/GameEngine/Source/GameNetwork/Transport.cpp", "int x;\n"), 0,
                         "network code")
        self.assertIn("::warning", out)

    def test_signature_code_warns_only(self):
        out = self.check(lambda r: r.write("Core/GameEngine/Source/GameClient/GXSha512.cpp", "int x;\n"), 0,
                         "network code")
        self.assertIn("::warning", out)

    def test_pure_rename_of_large_file_passes(self):
        r = Repo()
        self.addCleanup(r.tmp.cleanup)
        r.write("maps/big.map.txt", "y" * (2 * 1024 * 1024))
        r.base = r.commit("add big")
        r.git("mv", "maps/big.map.txt", "maps/moved.map.txt")
        rc, out = r.run("files", r.commit("move"))
        self.assertEqual(rc, 0, out)

    def test_gitattributes_change_fails_even_nested(self):
        self.check(lambda r: r.write("src/.gitattributes", "*.cpp -diff\n"), 1, "only the maintainer")

    def test_rename_out_of_github_fails(self):
        r = Repo()
        self.addCleanup(r.tmp.cleanup)
        r.write(".github/scripts/pr_checks.py", "print(1)\n")
        r.base = r.commit("add checker")
        os.makedirs(os.path.join(r.path, "tools"))
        r.git("mv", ".github/scripts/pr_checks.py", "tools/pr_checks.py")
        rc, out = r.run("files", r.commit("move it"))
        self.assertEqual(rc, 1, out)
        self.assertIn("file=.github/scripts/pr_checks.py", out)

    def test_path_cannot_inject_workflow_commands(self):
        out = self.check(lambda r: r.write("lib/a\n::warning::x.dll", "x\n"), 1)
        for line in out.splitlines():
            self.assertFalse(line.startswith("::warning::x"), out)


class NeedlesCheck(unittest.TestCase):
    def check(self, text, want_rc, want_text=None, messages=None):
        r = Repo()
        self.addCleanup(r.tmp.cleanup)
        r.write("src/game.cpp", "int main() { return 0; }\n" + text + "\n")
        rc, out = r.run("needles", r.commit("change"), messages)
        self.assertEqual(rc, want_rc, out)
        if want_text:
            self.assertIn(want_text, out)
        return out

    def test_clean_comment_passes(self):
        out = self.check("// Clamp so a slow peer cannot stall the frame.", 0)
        self.assertNotIn("::", out)

    def test_assistant_name_fails(self):
        self.check("// Claude: fixed the stall", 1, "assistant or vendor name")

    def test_codex_round_fails(self):
        self.check("// found by Codex review round 3", 1, "assistant name")

    def test_generated_with_fails(self):
        self.check("// Generated with an AI tool", 1, "'generated with/by' line")

    def test_coauthor_in_commit_message_fails(self):
        self.check("// fine", 1, "commit 1 of the pull request",
                   messages=["Fix stall\n\nCo-Authored-By: Claude <noreply@anthropic.com>\n"])

    def test_clean_commit_message_passes(self):
        self.check("// fine", 0, messages=["Fix stall\n\nCo-authored-by: A Person <a@users.noreply.github.com>\n"])

    def test_home_path_warns(self):
        self.check('const char* p = "/Users/alice/game";', 0, "home directory path")

    def test_placeholder_home_path_quiet(self):
        out = self.check('// e.g. /home/user/.local/share', 0)
        self.assertNotIn("home directory", out)

    def test_home_path_starting_like_a_placeholder_warns(self):
        self.check('const char* p = "/Users/megan/private";', 0, "home directory path")
        self.check('const char* p = "C:\\\\Users\\\\Melanie\\\\Documents";', 0, "home directory path")

    def test_private_ip_warns(self):
        self.check('const char* host = "192.168.1.20";', 0, "private network address")

    def test_tailnet_name_warns(self):
        # Assembled here so release scans of this file do not report the suffix itself.
        self.check('const char* host = "box.tail1234' + '.ts' + '.net";', 0, "private network host name")

    def test_email_warns_noreply_quiet(self):
        self.check("// mail someone@provider.test", 0, "e-mail address")
        out = self.check("// 1+x@users.noreply.github.com", 0)
        self.assertNotIn("e-mail", out)

    def test_attributes_cannot_hide_added_lines(self):
        r = Repo()
        self.addCleanup(r.tmp.cleanup)
        r.write(".gitattributes", "*.cpp -diff\n")
        r.base = r.commit("attributes on main")
        r.write("src/game.cpp", "int main() { return 0; }\n// Claude wrote this\n")
        rc, out = r.run("needles", r.commit("change"))
        self.assertEqual(rc, 1, out)
        self.assertIn("assistant or vendor name", out)

    def test_added_line_starting_with_plus_plus_does_not_end_the_file(self):
        out = self.check("++ marker inside a block comment\n// Claude was here", 1, "assistant or vendor name")
        self.assertIn("line=3", out)

    def test_line_text_is_never_echoed(self):
        out = self.check("// Claude ::error::injected", 1)
        self.assertNotIn("injected", out)


if __name__ == "__main__":
    unittest.main(verbosity=1)
