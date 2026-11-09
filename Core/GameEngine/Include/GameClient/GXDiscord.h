/* Discord Rich Presence for GeneralsX.
 *
 * GeneralsX @feature Claude 09/08/2026
 *
 * WHY IT LIVES UNDER GameClient. `resources/gitinfo/simsourcedigest_watcher.cmake` digests only
 * `{Include,Source}/{Common,GameLogic,GameNetwork}` plus WWMath and the build config. `GameClient`
 * appears in none of those lists, so everything here is OUTSIDE the SimID source digest. That is
 * not tidiness - it is the difference between "change the wording of a presence string" costing an
 * atomic four-platform rebuild and redeploy, and costing one platform's build. A cosmetic feature
 * must never be able to force a lockstep-atomic release.
 *
 * The same rule constrains what this file may DO: it reads the engine's state, and it writes
 * nothing that any simulation can observe. Reading a digested header is free; editing one is not,
 * which is why the preference below is read through OptionPreferences' existing map interface
 * rather than by adding a typed accessor to it.
 *
 * NO LIBRARY, NO NETWORK. Rich Presence is a local IPC protocol: length-prefixed JSON over a named
 * pipe on Windows (\\.\pipe\discord-ipc-N) and a unix socket in the runtime directory elsewhere.
 * That is a few hundred lines with no dependency, which matters in a project whose vcpkg manifest
 * gates curl off Windows entirely. Discord's own SDK would be a third-party binary on four
 * platforms for a feature nobody needs in order to play.
 *
 * IT IS DESKTOP-ONLY, and iPadOS is the reason rather than an oversight: iOS has no local Discord
 * client to talk to, so there is no socket to open. Every entry point below compiles everywhere and
 * does nothing where there is no Discord.
 *
 * IT FAILS SILENT. Discord not installed, not running, closed mid-match, a refused connection, a
 * half-written frame - all of them end in "no presence", never in a log line per frame and never in
 * anything a player notices. A cosmetic feature that can degrade a match is a bug.
 */

#pragma once

#include "Common/GameCommon.h"

/// Called once from GameClient::init(). Safe to call when Discord is absent.
void GXDiscordCreate(void);

/// Called every client frame from GameClient::update(). Cheap: it early-outs unless the presence
/// it would publish has actually changed, and it rate-limits the wire regardless - Discord asks
/// for at most one update every 15 seconds and drops the rest.
void GXDiscordUpdate(void);

/// Called from ~GameClient(). Clears the presence rather than leaving a stale "In a match" on
/// somebody's profile after the game has exited.
void GXDiscordDestroy(void);
