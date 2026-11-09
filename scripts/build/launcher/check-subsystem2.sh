#!/bin/sh
# GeneralsX launcher packaging - assert everything this plan takes on trust from subsystem 2.
#
# Called by every packager before it tars the source. All of these fail late and confusingly:
#   - a missing workspace member fails as "current package believes it's in a workspace when it's
#     not", on three hosts at once, after the tarball shipped;
#   - a productName that is not "GeneralsX Launcher" makes every artifact gate report
#     "GATE FAIL: no unpacked exe at ...", which reads as a build failure, not a naming mismatch;
#   - a build.rs with no cargo:rerun-if-env-changed=GX_COMMIT keeps the previous commit baked into
#     a binary whose sources did not change, so smoke-launch.sh says "the build did not relink"
#     about a build that relinked perfectly.
set -eu

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
. "$REPO/scripts/build/launcher/toolchain.env"

WS="$REPO/scripts/launcher-rs/Cargo.toml"
CRATE="$REPO/scripts/launcher-rs/gxlauncher"
CONF="$CRATE/tauri.conf.json"
BUILDRS="$CRATE/build.rs"

fail() { echo "CONTRACT FAIL: $1" >&2; exit 1; }

[ -f "$WS" ]    || fail "no cargo workspace at $WS"
[ -f "$CONF" ]  || fail "no tauri.conf.json at $CONF - subsystem 2 has not landed"
[ -f "$BUILDRS" ] || fail "no build.rs at $BUILDRS"

grep -q '"gxlauncher"' "$WS" ||
    fail "$WS does not list gxlauncher as a workspace member.
               It reads:
$(grep -n 'members' "$WS")
               It must read:  members = [\"gxcore\", \"gxlauncher\"]"
echo "CONTRACT OK  workspace member gxlauncher"

grep -q 'cargo:rerun-if-env-changed=GX_COMMIT' "$BUILDRS" ||
    fail "$BUILDRS does not emit cargo:rerun-if-env-changed=GX_COMMIT.
               Cargo does not re-run a build script when an undeclared env var changes, so the
               previous commit stays compiled in and the staleness gate fires on a healthy build."
echo "CONTRACT OK  build.rs declares GX_COMMIT"

GX_PRODUCT="$GX_PRODUCT" GX_APP_VERSION="$GX_APP_VERSION" GX_BUNDLE_ID="$GX_BUNDLE_ID" \
python3 - "$CONF" <<'PY'
import json, os, sys
c = json.load(open(sys.argv[1], encoding="utf-8"))
def die(msg):
    print("CONTRACT FAIL: %s" % msg, file=sys.stderr); raise SystemExit(1)
def get(d, path):
    cur = d
    for k in path.split("."):
        if not isinstance(cur, dict) or k not in cur:
            return None
        cur = cur[k]
    return cur

want_product = os.environ["GX_PRODUCT"]
for key in ("productName", "mainBinaryName"):
    got = c.get(key)
    if got != want_product:
        die("tauri.conf.json %s is %r, must be %r - every packager derives its paths from "
            "GX_PRODUCT in toolchain.env" % (key, got, want_product))
if c.get("version") != os.environ["GX_APP_VERSION"]:
    die("tauri.conf.json version is %r, must be %r" % (c.get("version"), os.environ["GX_APP_VERSION"]))
if c.get("identifier") != os.environ["GX_BUNDLE_ID"]:
    die("tauri.conf.json identifier is %r, must be %r"
        % (c.get("identifier"), os.environ["GX_BUNDLE_ID"]))

bbc = get(c, "build.beforeBuildCommand")
if bbc != "":
    die("build.beforeBuildCommand is %r, must be the empty string - no packaging machine has node"
        % (bbc,))
dist = get(c, "build.frontendDist")
if not isinstance(dist, str) or not dist:
    die("build.frontendDist is %r, must be a path to a committed static directory" % (dist,))
distdir = os.path.normpath(os.path.join(os.path.dirname(sys.argv[1]), dist))
if not os.path.isdir(distdir):
    die("build.frontendDist points at %s, which does not exist" % distdir)

if get(c, "plugins.updater") is not None:
    die("plugins.updater is present. This project has one Ed25519 chain in gxcore; the Tauri "
        "updater plugin brings a second private key and a second manifest format.")
if get(c, "bundle.createUpdaterArtifacts"):
    die("bundle.createUpdaterArtifacts is true; it must be false for the same reason.")

for line in ("productName and mainBinaryName are %r" % want_product,
             "version %s, identifier %s" % (c["version"], c["identifier"]),
             "frontendDist %s exists, beforeBuildCommand empty" % dist,
             "no tauri updater plugin, no updater artifacts"):
    print("CONTRACT OK  " + line)
PY
echo "CONTRACT OK  all subsystem-2 preconditions hold"
