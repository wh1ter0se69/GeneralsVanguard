// GeneralsX: the one-time move into GeneralsX's own user-data folder.
//
// Until this build, Windows GeneralsX kept its user data in
// Documents\<UserDataLeafName, default "Command and Conquer Generals Zero Hour Data">. Retail Zero
// Hour and the Generals Online client use the same folder, so the three programs shared Options.ini
// and MapCache.ini. The engine now uses Documents\GeneralsX\GeneralsZH (GlobalData.cpp). On the first
// start of such a build, this copies across, ONCE:
//
//   Maps\ (minus every MapCache.ini), Replays\, Save\ (folders behind a symlink or junction included), Options.ini, GXLogin.txt, GXPlayer.ini,
//   CommandButtonHotKeys.ini, GXHud.ini, SagePatch.ini, Network.ini, and every Skirmish*.ini at the
//   top level (any case: Skirmish.ini, SkirmishStats.ini, SkirmishStats_InstanceNN.ini)
//
// GXPlayer.ini is the durable identity and the friends list. Without it every Windows player
// becomes a new player. The settings files after it are custom keybinds, HUD and camera settings,
// and the LAN and skirmish setups (owner decision, 23/09/2026).
//
// The rules (owner decision, 23/09/2026), each held by scripts/test/userdata-migrate-test.cpp:
//   * MapCache.ini is NEVER copied. The engine rebuilds its own from the maps.
//   * NOTHING already in the new folder is overwritten. A file a launcher can create in the new
//     folder BEFORE the engine's first start is merged rather than skipped, or "copy only if absent"
//     would silently drop every old setting: Options.ini (gxlaunch.c, both launchers, the harness),
//     SagePatch.ini (both launchers' startup), GXHud.ini and CommandButtonHotKeys.ini (either
//     launcher's HUD or Keys page). The old keys the new file lacks are appended, and no existing
//     line changes. SagePatch.ini's arrive as a GameData block of their own. Network.ini and
//     Skirmish*.ini have no writer but the engine and are plain copies. The C# launcher stays on the
//     OLD folder until GXMigration.txt (below) exists (OwnedPaths.UserDataDir): a camera or HUD save fills
//     the value not touched from what it reads, and the new folder would have given it a default the
//     merge then kept over the player's own (review of the merged folder move, finding 3).
//   * ONE EXCEPTION: a SagePatch.ini that is byte for byte what a launcher or the engine writes
//     with nobody's choice in it counts as absent, and the old one replaces it whole. Those files
//     hold all six fields the engine reads, so a merge could never bring an old camera value
//     across (review of 889749a00). An OLD one like that, today's text or an older writer's, is
//     nothing to bring across, so it never replaces the new folder's defaults (review of 2acfb7031).
//   * NOTHING in the old folder is modified, moved or deleted. Generals Online keeps using it.
//   * Idempotent: GXMigration.txt in the new folder records each plain file once it has SETTLED
//     (copied, merged, found already there, or not in the old folder), and gains a "result:" line
//     when nothing is left. A settled file is never looked at again, so a pass that stays PARTIAL
//     retries only Maps, Replays and Save, and cannot undo a sign-out or an identity reset made between
//     two starts (review of b101653df). An error while looking is a failure, never "absent". So is
//     a file, folder or marker that is there but cannot be opened or read in full (review of
//     6ee23a741): it is not settled, the pass stays PARTIAL, and an unreadable marker stops the
//     pass before anything is touched, as IDENTITY_PENDING.
//   * GXPlayer.ini not yet across returns IDENTITY_PENDING, and WinMain then stops the engine from
//     minting a new identity over the gap (GXPlayerIdentity::holdNewIdentity).
//   * Never fatal and never throws. Failures are reported on stderr, which WinMain has already
//     mirrored to generals-stderr.log by the time this runs.
//
// NOT IN THE SIMULATION DIGEST: GeneralsMD/Code/Main is not a digested root. Pure C++ with
// std::filesystem and no engine headers, so the logic is tested on any host. It also runs on a
// worker thread while WinMain pumps the window (gxMigrateUserDataPumping), so it must stay that
// way: no engine global may be read or written from here.
#pragma once

#include <string>

/// What the pre-GeneralsX-folder engine used when the registry had no UserDataLeafName.
/// scripts/launcher-rs/gxio/src/release.rs guards that it matches USER_DATA_LEAF_DEFAULT.
#define GX_LEGACY_USER_DATA_LEAF_DEFAULT "Command and Conquer Generals Zero Hour Data"

/// Written into the NEW folder once the migration has nothing left to do.
#define GX_MIGRATION_MARKER "GXMigration.txt"

/// The old folder, derived from the new one: <newDir>\..\..\<leaf>. The new folder is
/// <Documents>\GeneralsX\GeneralsZH\, built from the same SHGetKnownFolderPath(FOLDERID_Documents)
/// the old engine used, so going up two levels is the old engine's Documents root, OneDrive
/// redirection included. An EMPTY leaf returns that root itself, because the old engine
/// concatenated an empty registry value as-is. Accepts either separator. Returns "" when newDir
/// has no two levels to go up.
std::string gxLegacyUserDataDir(const char *newDir, const char *leaf);

enum GXMigrateResult
{
	GX_MIGRATE_DONE = 0,	///< everything on the list is in the new folder now; marker written
	GX_MIGRATE_ALREADY,		///< the marker was there; nothing was looked at but Save\, once, on a marker from before it was listed
	GX_MIGRATE_NO_SOURCE,	///< there is no old folder; marker written so it is not asked again
	GX_MIGRATE_PARTIAL,		///< at least one step failed (see stderr); not finished, retried next start
	GX_MIGRATE_IDENTITY_PENDING	///< PARTIAL, and GXPlayer.ini has not come across yet (or was copied, but the marker
								///< could not record it, so it was taken back), or the marker could not be read or an
								///< unexpected error stopped the pass, so whether it has is unknown. The caller must not
								///< let the engine mint a new identity this session (GXPlayerIdentity::holdNewIdentity):
								///< a minted GXPlayer.ini would be "already there" to every retry, and the old
								///< identity and friends list would never arrive.
};

GXMigrateResult gxMigrateUserData(const char *oldDir, const char *newDir);
