# Generals Vanguard 0.9.73 — source

This repository is the source code of **Generals Vanguard 0.9.73** (tagged and named GeneralsX 0.9.73 in the commit), the
release published in November 2026
(the game for Windows, Linux and macOS and the Windows installer; the iOS/iPadOS build in this source is in testing
and has no public download). The launcher that ships with the release is a separate program and is not part of this
repository. The Windows updater (`GeneralsXUpdate.exe`) is part of the game's licence and its source is here, in
`scripts/build/windows/launcher/` (`gxlaunch.c`, `gxlaunch.rc`, `build-launcher.cmd`), with the one icon file it
references, `scripts/launcher-rs/gxlauncher/icons/icon.ico`; the 0.9.73 updater is built from this source.

It is a snapshot of commit `046894a397e044d8493ed2e57406d4264b16aa22` of the GeneralsX development
repository, published as one commit, with comments edited for publication. Every statement and string
compiled into a release build is the same as in that commit, but **a game built from this tree cannot play
with the published 0.9.73**, because the comment edits change the source id the engine compares before a
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

The Apple builds compile DXVK from the one submodule, pinned to the exact commit 0.9.73 used:

```sh
git submodule update --init --recursive references/fbraz3-dxvk
```

(`github.com/wh1ter0se69/dxvk`, commit `954c5c692622ac9bf631c5cee72559cc6a268ceb`, also the head of its
`gx/ff-constants` branch.) Every other dependency is fetched by CMake or vcpkg at the version pinned in
the build files.

Some packaging scripts build on a remote machine and read its address from `scripts/test/local.env`
(not included; copy `scripts/test/local.env.example` and fill it in).

A binary you build embeds this repository's commit id, not `046894a397`, so it reports a different
build id than the published 0.9.73 files.

## What differs from the shipped commit

For the `v0.9.73` tag, the publication edited 333 files of `046894a397` and added this one. The edits are:

* comments: personal names replaced by the `wh1ter0se` handle, profanity reworded, notes about how the project was
  worked on removed, headers that named upstream's authors on files they did not write changed to
  "GeneralsX contributors", a few quoted log values paraphrased;
* `scripts/legal/third_party_notices.py`: `check` compares `crates.json` with the launcher's `Cargo.lock` only when
  that file is in the tree;
* `README.md` and `scripts/test/local.env.example`: they say the launcher is a separate program and is not
  published here;
* `.gitmodules`, which lists only the submodule the build needs;
* `GeneralsMD/Code/CompatLib/Includecho`, a stray 160-byte file that nothing reads, and the leftover patch
  files `GameText.cpp.orig` and `GameText.cpp.rej`, removed.

The files of `046894a397` that are not here are the launcher (its source has its own licence and is not published),
internal notes, test harnesses and the online services' server code; see "What is here".

The C and C++ sources are the same as the shipped commit's once comments and layout are set aside. To
check, run

```sh
python3 scripts/tooling/comment-free-sha.py v0.9.73
```

It prints `8c833524c89dcdda12341f03b2847bb47cf3ab63f4598ca0752c8692cce80e59  4619 files`, the value the
shipped commit gives for the same 4,619 files.

## A build of this tree does not play with 0.9.73

GeneralsX peers run the simulation in lockstep, so before a match the engine compares a **source id**
(`sourceID`): a hash of the text of every file the simulation is built from, taken at build time by
`resources/gitinfo/simsourcedigest_watcher.cmake`. Peers whose source ids differ refuse each other.
Comments are part of that text, and edited files feed the hash, so this tree has a different
source id from the one 0.9.73 shipped with:

| | Zero Hour | Generals |
|---|---|---|
| published 0.9.73 (`046894a397`) | `0xEA47633F` | `0x962E248D` |
| this tree (`v0.9.73`) | `0x3A54F097` | `0x8F7FB55E` |

A game built from this tree is refused by official 0.9.73 players, and refuses them. The build prints the
value (`SimID source digest: ZH=0x... G=0x...`); to read it without building, run from the repository root:

```sh
cmake -DSIMDIGEST_BUILD_TIME=TRUE -DSIMDIGEST_WORKING_DIR="$PWD" -DGIT_EXECUTABLE="$(command -v git)" \
      -DSIMDIGEST_PRE_CONFIGURE_FILE=resources/gitinfo/simsourcedigest.cpp.in \
      -DSIMDIGEST_POST_CONFIGURE_FILE=/tmp/simsourcedigest.cpp \
      -P resources/gitinfo/simsourcedigest_watcher.cmake
```

From the next release on, the release binaries are built from the same tree that is published, so a build
of the release tag carries the shipped source id.
