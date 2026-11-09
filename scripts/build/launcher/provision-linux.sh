#!/bin/sh
# GeneralsX launcher packaging - provision the Linux build host.
#
# Measured 2026-08-20: Ubuntu 24.04.4 LTS x86_64, 4 cores, 7.8 GiB RAM, 41 GB free. Only
# build-essential and xvfb were installed; all eleven prerequisites resolve in the package index.
#
# THIS MAY BE A HOST THAT ALSO RUNS A RELAY. So: the caller must name the build host explicitly,
# and if that host runs a relay it must say so a second time. This is not a script that gets to
# make that call.
#
#
#
# THE GLIBC FLOOR IS A CONSEQUENCE, NOT AN OVERSIGHT. An AppImage inherits the glibc of the machine
# that built it. This box is glibc 2.39, so the AppImage requires Ubuntu 24.04 or newer and will
# not start on Ubuntu 22.04 or Debian 12. There is no Docker or podman on this box or on the Mac to
# obtain a 22.04 sysroot, so lowering the floor is a provisioning decision that belongs to whoever
# owns this host - not something a build script can paper over.
#
# Invoked as a FILE, never as a piped block:
#   /root/gxbuild/provision-linux.sh <tauri-cli-version>
set -eu

TAURI_CLI_VERSION="${1:?usage: provision-linux.sh <tauri-cli-version>}"

# Relay pre-flight, run ON the host so it sees the real service.
IS_RELAY=0
if command -v systemctl >/dev/null 2>&1 && systemctl list-unit-files 2>/dev/null | grep -q '^gxrelay'; then
    IS_RELAY=1
    echo "==> gxrelay is present on this host. state before: $(systemctl is-active gxrelay || true)"
    if [ "${GX_ALLOW_RELAY_HOST_BUILD:-0}" != "1" ]; then
        echo "error: this host runs the gxrelay service and GX_ALLOW_RELAY_HOST_BUILD is not 1." >&2
        echo "       Installing eleven apt packages as root here can pull dependency upgrades" >&2
        echo "       that restart services, and the Rust + WebKit build saturates the cores" >&2
        echo "       of the machine answering UDP. Point GX_LINUX_BUILD_HOST at a box" >&2
        echo "       that is not the relay, or set GX_ALLOW_RELAY_HOST_BUILD=1 deliberately." >&2
        exit 1
    fi
    # A relay that has logged nothing for two minutes is very likely idle. It is not proof - an
    # idle lobby holds no traffic - so this is a guard against the obvious case, not a guarantee.
    #
    # CORRECTION TO THE 2026-08-20 PLAN, measured on this host that day. The plan wrote this as
    #     journalctl -u gxrelay --since '-120 seconds' --no-pager | grep -c .
    # and journalctl prints the literal line "-- No entries --" on stdout when the window is
    # empty, EVEN with --no-pager. So RECENT was 1 on a host whose last relay log line was 98
    # minutes old, the guard refused unconditionally, and the only way past it was
    # GX_RELAY_BUSY_OK=1 - which is to say the busy check could never pass and never carried any
    # information about the relay. `-q` suppresses the header lines and `-o cat` drops the
    # timestamp prefix, so this counts real journal entries and nothing else. Proven both ways:
    # over a 24-hour window it counts hundreds, over the empty 120-second window it counts 0.
    RECENT="$(journalctl -u gxrelay --since '-120 seconds' --no-pager -o cat -q 2>/dev/null | grep -c . || true)"
    echo "==> gxrelay journal entries in the last 120s: $RECENT"
    if [ "$RECENT" -gt 0 ] && [ "${GX_RELAY_BUSY_OK:-0}" != "1" ]; then
        echo "error: gxrelay logged $RECENT lines in the last 120 seconds - it is carrying" >&2
        echo "       traffic right now. Wait, or set GX_RELAY_BUSY_OK=1 if you know the" >&2
        echo "       lobbies are empty." >&2
        exit 1
    fi
fi

mkdir -p /root/gxbuild/rust-target

# THE UNDO SNAPSHOT, taken before anything is installed. A hardcoded purge list would remove
# build-essential or libssl-dev from a host that already had them.
apt-mark showmanual | sort > /root/gxbuild/apt-manual-before.txt
echo "==> wrote the pre-install package snapshot to /root/gxbuild/apt-manual-before.txt"

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq \
    libwebkit2gtk-4.1-dev \
    build-essential \
    curl \
    wget \
    file \
    libxdo-dev \
    libssl-dev \
    libayatana-appindicator3-dev \
    librsvg2-dev \
    patchelf \
    xvfb

if ! [ -x "$HOME/.cargo/bin/cargo" ]; then
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
        | sh -s -- -y --default-toolchain stable --profile minimal
fi
. "$HOME/.cargo/env"

if ! [ -x "$HOME/.cargo/bin/cargo-tauri" ]; then
    CARGO_TARGET_DIR=/root/gxbuild/rust-target cargo install --locked "tauri-cli@$TAURI_CLI_VERSION"
fi

# GATE ON THE ARTIFACT. `cargo install` and `apt-get install` both exit 0 when the thing is already
# there, so the evidence is the binary existing and answering.
[ -x "$HOME/.cargo/bin/cargo-tauri" ] ||
    { echo "error: no cargo-tauri after install" >&2; exit 1; }

rustc --version
cargo tauri --version
ldd --version | head -1
if [ "$IS_RELAY" -eq 1 ]; then
    echo "==> gxrelay state after: $(systemctl is-active gxrelay || true)"
fi
