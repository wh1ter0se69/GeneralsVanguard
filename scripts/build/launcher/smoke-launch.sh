#!/bin/sh
# GeneralsX launcher packaging - does the packaged app actually open a window?
#
# A packaged app that opens no window is the failure that ships silently: the bundler exits 0, the
# installer runs, the file has a plausible size and a correct hash, and the first person to find
# out is a player. So no artifact is named in gxartifacts.txt until it has been launched on its own
# platform, FROM THE FORM IT SHIPS IN, and has said so itself.
#
# It also carries three checks that are not about "did it start":
#   commit    - the binary reports the commit compiled INTO it. If that is not the commit the
#               packager passed as GX_COMMIT, the build did not relink and the artifact is a
#               previous build wearing a new name.
#   platform  - guards against fetching the wrong file back from the wrong host.
#   state_dir - must not be under a path that does not survive the run. On macOS App Translocation
#               relocates a quarantined .app to a read-only random path under /private/var/folders
#               on first launch; on Linux --appimage-extract-and-run unpacks under
#               /tmp/appimage_extracted_* and deletes it on exit; on Windows the per-user install
#               directory is replaced wholesale by the next update. A launcher keeping config
#               beside itself finds none on exactly the download every new player performs.
#
# Usage:
#   smoke-launch.sh --expect-commit <9hex> --expect-platform <token> \
#                   [--forbid-prefix <path>]... -- <command> [args...]
set -u

EXPECT_COMMIT=""; EXPECT_PLATFORM=""
FORBID_FILE="$(mktemp)"; : > "$FORBID_FILE"
OUTFILE="$(mktemp)"; ERRFILE="$(mktemp)"
trap 'rm -f "$FORBID_FILE" "$OUTFILE" "$ERRFILE"' EXIT

while [ $# -gt 0 ]; do
    case "$1" in
        --expect-commit)   EXPECT_COMMIT="$2"; shift 2 ;;
        --expect-platform) EXPECT_PLATFORM="$2"; shift 2 ;;
        --forbid-prefix)   printf '%s\n' "$2" >> "$FORBID_FILE"; shift 2 ;;
        --) shift; break ;;
        *) echo "SMOKE FAIL: unknown argument $1" >&2; exit 1 ;;
    esac
done
[ $# -ge 1 ] || { echo "SMOKE FAIL: no command after --" >&2; exit 1; }
[ -n "$EXPECT_COMMIT" ]   || { echo "SMOKE FAIL: --expect-commit is required" >&2; exit 1; }
[ -n "$EXPECT_PLATFORM" ] || { echo "SMOKE FAIL: --expect-platform is required" >&2; exit 1; }
[ -s "$FORBID_FILE" ]     || { echo "SMOKE FAIL: at least one --forbid-prefix is required" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 ||
    { echo "SMOKE FAIL: python3 is required to parse the selftest line" >&2; exit 1; }

# GeneralsX @bugfix Claude 28/09/2026 NEVER IN THE CALLER'S PROFILE. The selftest writes the launcher's state
# (state_dir()/update), and run from a packager it wrote the OWNER's - the 0.9.49 macOS staging bumped
# ~/Library/Application Support/GeneralsX/update, and its startup sweeps also reach ~/Applications (the
# engine install) and SagePatch.ini, all under HOME. So the child gets a scratch HOME of its own, and
# CFFIXED_USER_HOME with it: NSHomeDirectory(), which WKWebView's data store uses, ignores HOME (measured).
# No inherited XDG_*_HOME either: each would win over HOME on Linux. A caller's GX_USERDATA_DIR is KEPT: it is
# that caller's own test profile, and the launcher's guard on it (no install, no startup sweep of the app
# being smoked) must hold (Codex iso-4-r1). /tmp rather than mktemp's default, which on macOS is under
# /private/var/folders - a prefix the macOS callers forbid for state_dir.
SCRATCH="$(mktemp -d /tmp/gxsmoke-home.XXXXXX)" && [ -d "$SCRATCH" ] ||
    { echo "SMOKE FAIL: could not make a scratch HOME under /tmp" >&2; exit 1; }
trap 'rm -f "$FORBID_FILE" "$OUTFILE" "$ERRFILE"; rm -rf "$SCRATCH"' EXIT

# 90 seconds, because a cold webview on a first run is genuinely slow and a timeout that fires on a
# healthy app teaches everyone to ignore this check.
env -u XDG_DATA_HOME -u XDG_CONFIG_HOME -u XDG_CACHE_HOME -u XDG_STATE_HOME \
    HOME="$SCRATCH" CFFIXED_USER_HOME="$SCRATCH" "$@" --gx-selftest >"$OUTFILE" 2>"$ERRFILE" &
CHILD=$!
WAITED=0
while kill -0 "$CHILD" 2>/dev/null; do
    if [ "$WAITED" -ge 90 ]; then
        kill -9 "$CHILD" 2>/dev/null
        echo "SMOKE FAIL: --gx-selftest did not exit within 90s; no window was reported" >&2
        sed -n '1,20p' "$ERRFILE" >&2
        exit 1
    fi
    sleep 1
    WAITED=$((WAITED + 1))
done
wait "$CHILD"; RC=$?

if [ "$RC" -ne 0 ]; then
    echo "SMOKE FAIL: --gx-selftest exited $RC" >&2
    sed -n '1,20p' "$ERRFILE" >&2
    exit 1
fi

# One line of JSON. python3 rather than a grep, because a grep over JSON is how a field ends up
# matching a substring of a different field.
PARSED="$(EXPECT_COMMIT="$EXPECT_COMMIT" EXPECT_PLATFORM="$EXPECT_PLATFORM" \
          FORBID_FILE="$FORBID_FILE" python3 - "$OUTFILE" <<'PY'
import json, os, sys
raw = open(sys.argv[1], "r", encoding="utf-8", errors="replace").read().strip()
lines = [l for l in raw.splitlines() if l.strip()]
if len(lines) != 1:
    print("BAD not exactly one line of output (%d lines)" % len(lines)); raise SystemExit(0)
try:
    d = json.loads(lines[0])
except Exception as e:
    print("BAD output is not JSON: %s" % e); raise SystemExit(0)
for k in ("gxselftest", "commit", "version", "platform", "window", "webview", "state_dir"):
    if k not in d:
        print("BAD missing key %r" % k); raise SystemExit(0)
if d["gxselftest"] != 1:
    print("BAD gxselftest is %r, expected 1" % (d["gxselftest"],)); raise SystemExit(0)
if d["window"] != "shown":
    print("BAD window is %r, expected 'shown'" % (d["window"],)); raise SystemExit(0)
if d["webview"] != "painted":
    print("BAD webview is %r, expected 'painted'" % (d["webview"],)); raise SystemExit(0)
if d["commit"] != os.environ["EXPECT_COMMIT"]:
    print("BAD binary reports commit %s, packager asked for %s - the build did not relink"
          % (d["commit"], os.environ["EXPECT_COMMIT"])); raise SystemExit(0)
if d["platform"] != os.environ["EXPECT_PLATFORM"]:
    print("BAD binary reports platform %s, expected %s"
          % (d["platform"], os.environ["EXPECT_PLATFORM"])); raise SystemExit(0)
for fp in open(os.environ["FORBID_FILE"], encoding="utf-8").read().splitlines():
    if fp and d["state_dir"].startswith(fp):
        print("BAD state_dir %s is under the forbidden prefix %s; that path is read-only or "
              "is deleted when the process exits, so the launcher would find no config on "
              "exactly the download every new player performs" % (d["state_dir"], fp))
        raise SystemExit(0)
print("OK %s %s %s" % (d["platform"], d["commit"], d["state_dir"]))
PY
)"

case "$PARSED" in
    "OK "*) echo "SMOKE ${PARSED}"; exit 0 ;;
    *)      echo "SMOKE FAIL: ${PARSED#BAD }" >&2; exit 1 ;;
esac
