# Generals Vanguard

**Command & Conquer: Generals - Zero Hour on Windows, Linux, macOS and iPad. One online match, up to 32 players, a 60 Hz
simulation.**

**[Download](https://wh1ter0se.dev/generals)** · [What changed](https://wh1ter0se.dev/generals/changes) · [Source of each release](SOURCE.md)

Generals Vanguard is a fork of the Zero Hour engine, built from EA's 2003 source (released under the GPL in 2025).
It runs natively on Windows (x64), Linux (x86_64) and macOS (Apple silicon), and players on all three join the same
online match through a relay server; the iPhone/iPad build plays in the same matches and is in testing, with no public
download. It keeps the original game and its content and changes what is underneath: a bigger lobby, a faster and
fairer simulation, better graphics and better computer opponents.

Generals Vanguard was called GeneralsX until November 2026. Names a program or an updater looks for did not change:
the game binary is still `GeneralsXZH`, and files and install folders keep the `GeneralsX` name.

## Why try it

1. **It is fast.** Over 450 frames a second in a 6v6 at 2560x1440 on a Windows test PC, and a 60 Hz simulation that
   holds 60 (EA's ran at 30). Each number below says which machine it came from.
2. **Play with anyone, on any computer.** Windows, Mac and Linux players in the same online match, and the iPad too.
3. **32 players in one game.** Up to 32 plus 2 observers, with maps built for it, and the computer can fill empty seats.
4. **It looks better and still looks like Generals.** Sharper textures and 3D models, moving water, remastered ground:
   the original art, not restyled.
5. **A real caster/spectator mode, and fair play.** Scoreboard, see through a player's eyes, peek into bunkers, scrub a
   replay; a join is refused with a reason instead of a silent desync.

## Features

**Play online**
- One match across Windows, Linux and macOS; iPad joins the same games (testing).
- Up to 32 players plus 2 observers in one game, with three 32-player maps (Crown 32, Crown Flat 32, Last Stand 32).
- Online lobby with chat, friends and private messages. Sign in with Discord or a username and password.
- Vote kick (`/kick name`), and a join is refused with the reason if the builds or data differ.
- The game reads the relay address from a signed list, so the relay can move without a game update.

**Platforms**
- Native Windows (x64), Linux (x86_64) and macOS (Apple silicon); iPhone/iPad in testing.
- A launcher that installs and updates the game and holds the options (look, HUD, keybinds, spectator).

**Game**
- Spectator screen for casters and observers: scoreboard with cash, power, K/D and XP; follow a player and see the map
  through their fog; click a bunker or transport to see who is inside; replay seek strip with speed buttons.
- Better-looking, still the original art: remastered ground, moving water, HD textures and HD 3D models, a smooth fog
  edge, and an optional new native renderer. A look governor trims extra effects before it lets the game slow down.
- Stronger computer players: Hard AI aims superweapons at your economy, lays out bases facing the enemy, and fills all
  32 seats.
- 20 new player colours, $500,000 and $1,000,000 start cash, camera tilt, sell on Z.

**Performance** (measured on test machines, not typical hardware)
- 60 simulation ticks a second, and it holds 60: in a late-game 6v6 replay on a Mac Mini the game went from about 53 to
  60 steps a second.
- Over 450 frames a second in a 6v6 on Twin Ridges at 2560x1440 on a Windows test PC (453 fps with the classic
  renderer, 448 with the new one); the shadow hitches of up to about 95 ms that 0.9.68 had are gone.
- Faster drawing from a separate render thread: about 9 to 11% more frames a second on a Windows test PC, about 3% on a
  Mac Mini. GPU-built shadow shapes on Windows: about 12 to 16% more fps in a late 6v6 replay.
- Frame time: unchanged lights and materials are no longer re-sent, lighting is rebuilt only when changed and decals are
  batched, about 0.4 to 0.7 ms off each frame for each change on a Windows test PC; animation poses run on several
  cores, about 0.8 ms per frame on a Mac Mini.
- Smoother on Mac and iPad at 60 fps: frames over 20 ms fell from 8.3% to 0.3% in Mac Mini tests, and the first-shadow
  freeze of about 50 ms is gone.
- Lighter big battles (game logic, same game): in a 6v6 with 12 computer players the average tick is several percent
  cheaper from faster pathfinding and cheaper production checks, and late-game logic is a few percent cheaper, up to 9%
  in the heaviest game. These are logic costs on a test machine, not measured frame rates.

**Under the hood**
- The same simulation result on x64 and arm64: strict floating-point flags and one math kernel.
- Replays record graphics settings and a once-a-second log of frame rate and game speed, and store no network address.
- Every change builds on four platforms from the same commit, is reviewed by a second independent model, and
  simulation changes must replay to identical per-frame CRCs; speed changes are measured A/B.

This repository holds the source of each published release as one snapshot commit, tagged with its version. The
current tag, `v0.9.72`, is the source of the 0.9.72 release and was cut from development commit
`aaf44a2a421352c89877868ed55a5d803af477d2`; [SOURCE.md](SOURCE.md) lists exactly what differs from that commit.
Downloads and release notes: [wh1ter0se.dev/generals](https://wh1ter0se.dev/generals) and
[wh1ter0se.dev/generals/changes](https://wh1ter0se.dev/generals/changes).

**No game assets are included.** You need your own copy of Zero Hour
([Steam](https://store.steampowered.com/app/2732960/) or the EA app).

## What it does

| Feature | Where to look |
|---|---|
| One match across Windows, Linux and macOS (iPad in testing). Shipping presets: `win64`, `linux64-deploy`, `macos-vulkan`, `ios-vulkan`. `win64` is the only 64-bit Windows preset, and cross-play needs it: a 32-bit peer hashes different sizes into the network CRC. | [`CMakePresets.json`](CMakePresets.json) |
| Online play through a relay server. The client reads the relay's address from a list signed with Ed25519 against pinned keys, so a relay can move without a game update. The relay server itself is not in this repository. | [`GXRelayList.cpp`](Core/GameEngine/Source/GameClient/GXRelayList.cpp), [`Transport.cpp`](Core/GameEngine/Source/GameNetwork/Transport.cpp) |
| Up to 32 players and 2 observers: 34 lobby slots (`MAX_PLAYER = 33`), all of which may be human; the simulation's `MAX_PLAYER_COUNT` is 40, with 64-bit player masks. | [`NetworkDefs.h`](Core/GameEngine/Include/GameNetwork/NetworkDefs.h), [`GameCommon.h`](Core/GameEngine/Include/Common/GameCommon.h) |
| The simulation runs at 60 ticks a second (EA's ran at 30). It is a build option, on in all four shipping presets; 30 Hz and 60 Hz builds refuse to join each other. | [`WWCommon.h`](Core/Libraries/Source/WWVegas/WWLib/WWCommon.h), [`cmake/config-build.cmake`](cmake/config-build.cmake) |
| Build identity on join: engine, simulation source, data and ordinal ids must match or the join is refused with the reason; parse, asset and platform differences only warn. The source id is a digest of the simulation code taken at build time. | [`SimulationId.h`](Core/GameEngine/Include/Common/Diagnostic/SimulationId.h), [`simsourcedigest_watcher.cmake`](resources/gitinfo/simsourcedigest_watcher.cmake) |
| The same simulation results on x64 and arm64: strict floating-point compiler flags (`-ffp-contract=off`, `/fp:precise` on MSVC) and one math kernel for the simulation. | [`cmake/compilers.cmake`](cmake/compilers.cmake), [`gamemath.h`](Core/Libraries/Source/WWVegas/WWMath/gamemath.h) |
| Replays record graphics settings and a once-a-second log of frame rate and game speed, and store no network address. `GXReplayTelemetry = full \| perf \| off` in `Options.ini`. | [`GXReplayDiag.h`](GeneralsMD/Code/GameEngine/Include/GameClient/GXReplayDiag.h) |
| Online lobby with chat, friends and private messages; sign-in with Discord or a Generals Vanguard account. Only the game client's side is here; the account and lobby services are not. | [`GXLobbyPresence.cpp`](Core/GameEngine/Source/GameClient/GXLobbyPresence.cpp), [`GXRelayAuth.h`](Core/GameEngine/Include/GameClient/GXRelayAuth.h) |
| The 2003 Direct3D 8 renderer runs through DXVK to Vulkan, and MoltenVK to Metal on Apple. An optional new Vulkan renderer (`GXRenderer = native` in `Options.ini`; classic is the default) is in development. | [`GXRendererKey.h`](Core/GameEngine/Include/GameClient/GXRendererKey.h), [`GXRender/`](Core/GameEngineDevice/Source/W3DDevice/GameClient/GXRender) |
| Skirmish AI additions: a rule bank in the engine and generated AI scripts. | [`GXRules/`](GeneralsMD/Code/GameEngine/Source/GameLogic/AI/GXRules), [`scripts/tooling/`](scripts/tooling) |

## Where it comes from

**Generals Vanguard** builds on **[GeneralsX](https://github.com/fbraz3/GeneralsX)** by fbraz3, the macOS and Linux port
of Zero Hour that this project started from and still draws on directly. GeneralsX itself stands on
[TheSuperHackers/GeneralsGameCode](https://github.com/TheSuperHackers/GeneralsGameCode), the community mainline, and on
[Fighter19's original Unix port](https://github.com/Fighter19/CnC_Generals_Zero_Hour), all of it descending from the GPL v3
source EA released for Command & Conquer: Generals and Zero Hour, with DXVK, MoltenVK, SDL, OpenAL Soft and FFmpeg
underneath. Generals Vanguard is an independent project, is not affiliated with or endorsed by fbraz3, TheSuperHackers, EA
or the Generals Online team, and ships no game assets.

Most of this code is not ours, and the projects it comes from did the foundational work:

| Project | What it contributed |
|---|---|
| Westwood Pacific / EA Pacific, and [EA](https://github.com/electronicarts/CnC_Generals_Zero_Hour) | The game, and the GPL v3 source release of February 2025. |
| [TheSuperHackers/GeneralsGameCode](https://github.com/TheSuperHackers/GeneralsGameCode) | The community mainline: build modernization from VC6 to modern toolchains, the shared `Core/` tree, a long list of fixes, and much of the cross-platform groundwork, including the FFmpeg video backend and the OpenAL audio device work by [feliwir](https://github.com/feliwir). GeneralsX's development history is a fork of theirs, and their later fixes are ported in. |
| [Fighter19/CnC_Generals_Zero_Hour](https://github.com/Fighter19/CnC_Generals_Zero_Hour) | The original Unix / 64-bit port: SDL3 platform management, C++17 filesystem and threading, FreeType/Fontconfig text rendering, and the DXVK approach to rendering. |
| [fbraz3/GeneralsX](https://github.com/fbraz3/GeneralsX) | The macOS and Linux port this line descends from. The name GeneralsX is theirs. |
| [ammaarreshi/Generals-Mac-iOS-iPad](https://github.com/ammaarreshi/Generals-Mac-iOS-iPad) | The iOS/iPadOS port (arm64 cross-build, DXVK on iOS, touch controls, app lifecycle, packaging), and the project this fork was made from. The port's own account of that work: [docs/IOS_PORT.md](docs/IOS_PORT.md). |
| [DXVK](https://github.com/doitsujin/dxvk), [MoltenVK](https://github.com/KhronosGroup/MoltenVK), [SDL](https://github.com/libsdl-org/SDL), [OpenAL Soft](https://github.com/kcat/openal-soft), [FFmpeg](https://ffmpeg.org), and the rest | Every third-party component and its licence: [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt). |

### Lines of code by origin

Engine C/C++ in `v0.9.72` (`Core/`, `Generals/`, `GeneralsMD/`, `Dependencies/`), each line counted toward the
earliest project that already has it:

| Engine C/C++ | Lines | Share |
|---|---:|---:|
| EA's 2003 release | 1,408,502 | 71.0% |
| TheSuperHackers | 216,136 | 10.9% |
| Fighter19 / fbraz3 / ammaarreshi, not in upstream | 27,209 | 1.4% |
| This fork | 332,937 | 16.8% |
| **Total** | **1,984,784** | |

Of this fork's lines, 45,529 are vendored third-party code, 29,676 are generated shader bytecode and 34,839 are
tests; 222,893 are hand-written engine code. Method, the commits compared and the command to reproduce it:
[docs/LINES_BY_ORIGIN.md](docs/LINES_BY_ORIGIN.md).

## How it is built
<!-- how-built -->

Generals Vanguard is directed, reviewed and funded by its author, wh1ter0se, and tested on the author's own hardware. Much of
the code is written with AI assistance; AI is a tool here, and a change ships on what it measures, not on who or what
typed it. The gates:

* **Four platforms.** A change merges only after it builds for Windows, Linux, macOS and iOS from the same commit.
* **Cross-model review.** Every change is reviewed by a second, independent model before that build, and its
  findings are fixed or ruled on before the change merges.
* **Determinism.** A change that touches the simulation must play recorded replays and two-build matches to identical
  per-frame CRCs; otherwise it is a declared simulation change, its source id moves, and older builds refuse to join
  it.
* **Performance.** A change aimed at speed is measured in A/B runs against the build before it, with the tick rate
  reported over the whole run and every drop explained.

The test harnesses and CI that run these gates are development tooling and are not part of this snapshot
([SOURCE.md](SOURCE.md)).

## Building

The commands below are the short path; the platform guides have prerequisites and troubleshooting. Each build
produces `GeneralsXZH` (`generalszh.exe` on Windows).

**macOS (Apple silicon)** — prerequisites in [docs/BUILD/MACOS.md](docs/BUILD/MACOS.md) (Xcode command line tools,
Homebrew `cmake ninja meson pkgconf`, the LunarG Vulkan SDK, vcpkg with `VCPKG_ROOT` set):

```sh
git submodule update --init --recursive references/fbraz3-dxvk
./scripts/build/macos/build-macos-zh.sh            # configures the macos-vulkan preset and builds
./scripts/build/macos/deploy-macos-zh.sh           # installs to ~/GeneralsX/GeneralsZH with a run.sh
./scripts/get-assets.sh <your_steam_username>      # fetches the Zero Hour data you own (steamcmd)
cd ~/GeneralsX/GeneralsZH && ./run.sh -win
```

**iPhone / iPad** — [docs/BUILD/IOS.md](docs/BUILD/IOS.md).

**Linux (x86_64)** — prerequisites in [docs/BUILD/LINUX.md](docs/BUILD/LINUX.md):

```sh
cmake --preset linux64-deploy
cmake --build build/linux64-deploy --target z_generals
./scripts/build/linux/deploy-linux-zh.sh            # installs to ~/GeneralsX/GeneralsZH
./scripts/get-assets.sh <your_steam_username>
./scripts/build/linux/run-linux-zh.sh
```

**Windows (x64)** — from an x64 Visual Studio developer prompt, with vcpkg:

```sh
cmake --preset win64
cmake --build --preset win64 --target z_generals   # RelWithDebInfo, the configuration that ships
```

`scripts/build/windows/setup-run-win64.ps1` stages the build into its own folder and leaves your Steam install
untouched; packaging and the installer are in `scripts/build/windows/`. The Generals Vanguard launcher and its updater are
separate programs and are not part of this repository; the game builds and runs without them.

A game built from the `v0.9.72` tag cannot play with the published 0.9.72: publishing edited comments in files the
simulation's source id is computed from, so the two refuse each other. [SOURCE.md](SOURCE.md) has the numbers and
how to check that only comments differ; from the next release on, the tag is the tree the binaries were built from.

## Licence

GPL-3.0-or-later, with the additional terms EA attached to its release of the original source: see
[LICENSE.md](LICENSE.md). Third-party components and their licences: [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).
Each release tag here is the corresponding source of the game and installer binaries published with that version.
The game's art, audio, maps and data belong to Electronic Arts and are not covered by this licence.

## Reporting a problem

Open an issue at [github.com/wh1ter0se69/GeneralsX-source/issues](https://github.com/wh1ter0se69/GeneralsX-source/issues).
A report that can be acted on includes:

* the Generals Vanguard version and the version line shown on the main menu;
* your platform, OS version and graphics card;
* what you did, what you expected, and what happened instead;
* for a problem in a match, the replay (replays store no network address) and whether it was LAN or online.

This repository is a snapshot per release, not where development happens, so a pull request cannot be merged here as
it is. Changes that are taken are applied in development and appear in a later release and its source tag.
