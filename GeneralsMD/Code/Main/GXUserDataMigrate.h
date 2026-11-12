/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 GeneralsX contributors
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// the one-time move into GeneralsX's user-data folder (Documents\GeneralsX\GeneralsZH) from the old shared one
// (Documents\<UserDataLeafName>, shared with retail and Generals Online). Copies Maps\ (never MapCache.ini),
// Replays\, Save\, Options.ini, GXLogin.txt, GXPlayer.ini, CommandButtonHotKeys.ini, GXHud.ini, SagePatch.ini,
// Network.ini and Skirmish*.ini. Never overwrites; launcher-written files are merged; old folder untouched.
// GXPlayer.ini not yet across returns IDENTITY_PENDING: mint no new identity. GXMigration.txt records progress.
// Not in the simulation digest; runs on a worker thread, so it must not touch any engine global.
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
