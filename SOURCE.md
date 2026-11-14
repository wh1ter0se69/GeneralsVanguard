# Generals Vanguard 0.9.74 — source

This repository is the source code of **Generals Vanguard 0.9.74** (tagged and named GeneralsX 0.9.74 in the commit), the
release published on 13 November 2026
(the game for Windows, Linux and macOS and the Windows installer; the iOS/iPadOS build in this source is in testing
and has no public download). The launcher that ships with the release is a separate program and is not part of this
repository. The Windows updater (`GeneralsXUpdate.exe`) is part of the game's licence and its source is here, in
`scripts/build/windows/launcher/` (`gxlaunch.c`, `gxlaunch.rc`, `build-launcher.cmd`), with the one icon file it
references, `scripts/launcher-rs/gxlauncher/icons/icon.ico`; the 0.9.74 updater is built from this source.

It is a snapshot of commit `7c6a2ad43a0f00b7cf9511a340ef06c3f721d38f` of the GeneralsX development
repository, published as one commit, with comments edited for publication. Every statement and string
compiled into a release build is the same as in that commit, but **a game built from this tree cannot play
with the published 0.9.74**, because the comment edits change the source id the engine compares before a
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
* The game builds and runs without the launcher: `scripts/build/windows/setup-run-win64.ps1` stages a Windows build
  into a runnable folder without one. `scripts/build/windows/package-windows-share.ps1`, which assembles the players'
  Windows download with the launcher and its updater, checks both against the development repository and does not
  run from this tree.

The Apple builds compile DXVK from the one submodule, pinned to the exact commit 0.9.74 used:

```sh
git submodule update --init --recursive references/fbraz3-dxvk
```

(`github.com/wh1ter0se69/dxvk`, commit `954c5c692622ac9bf631c5cee72559cc6a268ceb`, also the head of its
`gx/ff-constants` branch.) Every other dependency is fetched by CMake or vcpkg at the version pinned in
the build files.

Some packaging scripts build on a remote machine and read its address from `scripts/test/local.env`
(not included; copy `scripts/test/local.env.example` and fill it in).

A binary you build embeds this repository's commit id, not `7c6a2ad43a`, so it reports a different
build id than the published 0.9.74 files.

## What differs from the shipped commit

For the `v0.9.74` tag, the publication edited 335 files of `7c6a2ad43a` and added this one. The edits are:

* comments: personal names replaced by the `wh1ter0se` handle, profanity reworded, notes about how the project was
  worked on removed, headers that named upstream's authors on files they did not write changed to
  "GeneralsX contributors", a few quoted log values paraphrased;
* `scripts/legal/third_party_notices.py`: `check` compares `crates.json` with the launcher's `Cargo.lock` only when
  that file is in the tree;
* `README.md` and `scripts/test/local.env.example`: they say the launcher is a separate program and is not
  published here;
* `.gitmodules`, which lists only the submodule the build needs;
* `GeneralsMD/Code/CompatLib/Includecho`, a stray 160-byte file that nothing reads, and the leftover patch
  files `GameText.cpp.orig` and `GameText.cpp.rej`, removed; three build-script test and
  check files that nothing in the game or the packages reads (`scripts/build/test-runsh-linux-root.sh`,
  `scripts/build/winmacro-check.py`, `scripts/build/windows/launcher/test-product-dir.sh`).

The files of `7c6a2ad43a` that are not here are the launcher (its source has its own licence and is not published),
internal notes, test harnesses and the online services' server code; see "What is here".

The C and C++ sources are the same as the shipped commit's once comments and layout are set aside. To
check, run

```sh
python3 scripts/tooling/comment-free-sha.py v0.9.74
```

It prints `6a6247ab134d52594b4a6c446d3a02a8db049504e74c1f795e79d2acaeadd7ad  4646 files`, the value the
shipped commit gives for the same 4,646 files.

## A build of this tree does not play with 0.9.74

GeneralsX peers run the simulation in lockstep, so before a match the engine compares a **source id**
(`sourceID`): a hash of the text of every file the simulation is built from, taken at build time by
`resources/gitinfo/simsourcedigest_watcher.cmake`. Peers whose source ids differ refuse each other.
Comments are part of that text, and edited files feed the hash, so this tree has a different
source id from the one 0.9.74 shipped with:

| | Zero Hour | Generals |
|---|---|---|
| published 0.9.74 (`7c6a2ad43a`) | `0x0FE832A3` | `0x5C9FAAD5` |
| this tree (`v0.9.74`) | `0xA907CE32` | `0x47D58129` |

A game built from this tree is refused by official 0.9.74 players, and refuses them. The build prints the
value (`SimID source digest: ZH=0x... G=0x...`); to read it without building, run from the repository root:

```sh
cmake -DSIMDIGEST_BUILD_TIME=TRUE -DSIMDIGEST_WORKING_DIR="$PWD" -DGIT_EXECUTABLE="$(command -v git)" \
      -DSIMDIGEST_PRE_CONFIGURE_FILE=resources/gitinfo/simsourcedigest.cpp.in \
      -DSIMDIGEST_POST_CONFIGURE_FILE=/tmp/simsourcedigest.cpp \
      -P resources/gitinfo/simsourcedigest_watcher.cmake
```

From the next release on, the release binaries are built from the same tree that is published, so a build
of the release tag carries the shipped source id.
