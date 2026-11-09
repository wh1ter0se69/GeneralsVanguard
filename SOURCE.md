# GeneralsX 0.9.71 — source

This repository is the source code of **GeneralsX 0.9.71**, the release published on 9 November 2026
(game for Windows, Linux, macOS and iOS/iPadOS, the GeneralsX Launcher and the Windows installer).

It is a snapshot of commit `bfeca14d0223e62b936141e1f22a2e37b1bd68c5` of the GeneralsX development
repository, published as one commit. The compiled code is identical to that commit; the differences are
this file, `.gitmodules` (only the submodule the build needs is listed), comments edited for publication
and one unit-test fixture (both noted below).

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

* macOS and iOS/iPadOS: the quick starts in [README.md](README.md), and [docs/BUILD/MACOS.md](docs/BUILD/MACOS.md).
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

Comments in this snapshot were edited for publication (a personal name replaced by the `wh1ter0se`
handle, profanity reworded, a few quoted log values paraphrased). Code and every string compiled into a
release build are unchanged.

## One difference from the development tree

`scripts/launcher-rs/gxio/src/netdiag.rs`: an IP address in one comment and one `#[test]` was replaced
with the documentation address `198.51.100.7` (RFC 5737). Neither is compiled into a release build.
