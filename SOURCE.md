# GeneralsX 0.9.71 — source

This repository is the source code of **GeneralsX 0.9.71**, the release published on 9 November 2026
(game for Windows, Linux and macOS, the GeneralsX Launcher and the Windows installer; the iOS/iPadOS build in this
source is in testing and has no public download).

It is a snapshot of commit `bfeca14d0223e62b936141e1f22a2e37b1bd68c5` of the GeneralsX development
repository, published as one commit, with comments edited for publication. Every statement and string
compiled into a release build is the same as in that commit, but **a game built from this tree cannot play
with the published 0.9.71**, because the comment edits change the source id the engine compares before a
match. Both are explained below, with commands to check them.

GeneralsX is licensed under the GNU General Public License v3.0 or later, with the additional terms
EA attached to its release of the original source — see [LICENSE.md](LICENSE.md). Third-party
components and their licences are listed in [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).

## Game assets are not included

The game's art, audio, maps and data belong to Electronic Arts and are not covered by the GPL. To play
you need your own copy of *Command & Conquer: Generals — Zero Hour* (Steam or the EA app). The installer
and launcher find an existing install; `scripts/get-assets.sh` fetches the files you own through steamcmd.

## What is here

| Path | What it is |
|---|---|
| `Core/`, `Generals/`, `GeneralsMD/`, `Dependencies/` | the engine and game source (Zero Hour is `GeneralsMD/`) |
| `CMakeLists.txt`, `CMakePresets.json`, `cmake/`, `triplets/`, `vcpkg*.json`, `resources/` | build system and build-time resources |
| `Patches/` | patches applied to third-party code at build time (DXVK for Apple) |
| `ios/` | the iOS/iPadOS app wrapper |
| `scripts/build/` | build, bundle and packaging scripts for every platform, the Windows installer (`windows/installer/GeneralsX.iss`) |
| `scripts/launcher-rs/`, `design/launcher/` | the GeneralsX Launcher (Rust + Tauri) and its interface |
| `scripts/tooling/`, `GeneralsZH/Data/`, `scripts/test/maps/` | generators and inputs for the data the packages ship (AI scripts, INI files, the bundled maps) |
| `scripts/legal/` | generates `THIRD_PARTY_NOTICES.txt` |

Internal development notes, test harnesses, the online services' server code and vendored reference
checkouts of other projects are not part of the released programs and are not included.

## Building

* macOS: the quick start in [README.md](README.md#building), and [docs/BUILD/MACOS.md](docs/BUILD/MACOS.md).
* iOS/iPadOS: [docs/BUILD/IOS.md](docs/BUILD/IOS.md).
* Linux: [docs/BUILD/LINUX.md](docs/BUILD/LINUX.md) and `scripts/build/linux/`.
* Windows: the `win64` preset in `CMakePresets.json`; packaging and the installer are
  `scripts/build/windows/package-windows-share.ps1` and `scripts/build/windows/installer/`.
* The launcher: `scripts/build/launcher/package-launcher-{macos,linux,windows}.sh`.

The Apple builds compile DXVK from the one submodule, pinned to the exact commit 0.9.71 used:

```sh
git submodule update --init --recursive references/fbraz3-dxvk
```

(`github.com/wh1ter0se69/dxvk`, commit `954c5c692622ac9bf631c5cee72559cc6a268ceb`, also the head of its
`gx/ff-constants` branch.) Every other dependency is fetched by CMake or vcpkg at the version pinned in
the build files.

Some packaging scripts build on a remote machine and read its address from `scripts/test/local.env`
(not included; copy `scripts/test/local.env.example` and fill it in).

A binary you build embeds this repository's commit id, not `bfeca14d02`, so it reports a different
build id than the published 0.9.71 files.

The launcher's unit tests (`cargo test` in `scripts/launcher-rs`) read development files that are not
included (a CI workflow, test fixtures and test sources under `scripts/test`), so they do not compile from
this snapshot. `cargo build` and the packaging scripts do not use them.

## What differs from the shipped commit

The publication edited 143 files of `bfeca14d02` and added this one. The edits are:

* comments: a personal name replaced by the `wh1ter0se` handle, profanity reworded, a few quoted log
  values paraphrased;
* launcher test code, which is never part of a release build (inside `#[cfg(test)]` or under `tests/`):
  the same name replaced in test fixtures and test names in `scripts/launcher-rs/gxio/src/crashlogs.rs`,
  `gxio/src/options.rs` and `gxio/tests/image_quality_engine_parser.rs`, and an IP address in
  `gxio/src/netdiag.rs` replaced with the documentation address `198.51.100.7` (RFC 5737);
* two `"prior"` entries in `scripts/tooling/rulebank/registry.json` (the same name; no code reads that
  field);
* `.gitmodules`, which lists only the submodule the build needs.

The C and C++ sources are the same as the shipped commit's once comments and layout are set aside. To
check, run

```sh
python3 scripts/tooling/comment-free-sha.py v0.9.71
```

It prints `2cb491977b8ded266a4a2852a48c5d154ce947e2d65a31319b3635ad6558d048  4610 files`, the value the
shipped commit gives for the same 4,610 files.

## A build of this tree does not play with 0.9.71

GeneralsX peers run the simulation in lockstep, so before a match the engine compares a **source id**
(`sourceID`): a hash of the text of every file the simulation is built from, taken at build time by
`resources/gitinfo/simsourcedigest_watcher.cmake`. Peers whose source ids differ refuse each other.
Comments are part of that text, and 49 of the edited files feed the hash, so this tree has a different
source id from the one 0.9.71 shipped with:

| | Zero Hour | Generals |
|---|---|---|
| published 0.9.71 (`bfeca14d02`) | `0x186E518B` | `0x225BD309` |
| this tree (`v0.9.71`) | `0x14DE941E` | `0x8EC22195` |

A game built from this tree is refused by official 0.9.71 players, and refuses them. The build prints the
value (`SimID source digest: ZH=0x... G=0x...`); to read it without building, run from the repository root:

```sh
cmake -DSIMDIGEST_BUILD_TIME=TRUE -DSIMDIGEST_WORKING_DIR="$PWD" -DGIT_EXECUTABLE="$(command -v git)" \
      -DSIMDIGEST_PRE_CONFIGURE_FILE=resources/gitinfo/simsourcedigest.cpp.in \
      -DSIMDIGEST_POST_CONFIGURE_FILE=/tmp/simsourcedigest.cpp \
      -P resources/gitinfo/simsourcedigest_watcher.cmake
```

From the next release on, the release binaries are built from the same tree that is published, so a build
of the release tag carries the shipped source id.
