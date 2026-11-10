/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////////////
// FILE: FileTransfer.cpp
// Author: Matthew D. Campbell, December 2002
// Description: File Transfer wrapper using TheNetwork
///////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/LocalFileSystem.h"
#include "GameClient/LoadScreen.h"
#include "GameClient/MapUtil.h"
#include "GameClient/Shell.h"
#include "GameNetwork/FileTransfer.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameNetwork/networkutil.h"

//-------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------

static Bool doFileTransfer( AsciiString filename, MapTransferLoadScreen *ls, RelayMaskType mask )
{
	Bool fileTransferDone = FALSE;
	Int fileTransferPercent = 0;
	Int i;

	if (mask)
	{
		// GeneralsX @debug /08/2026 GX_MAPDBG: A1 measurement probes for the post-transfer
		// lobby bounce. fprintf(stderr) so they survive release builds; env-gated so they cost one
		// branch when off.
		static const Bool s_mapDbg = (getenv("GX_MAPDBG") != NULL);
		if (s_mapDbg)
		{
			fprintf(stderr, "[GXMAPDBG] doFileTransfer begin: file='%s' mask=%llX host=%d\n",
				filename.str(), (unsigned long long)mask, TheGameInfo->amIHost() ? 1 : 0);
			fflush(stderr);
		}
		Int dbgLastProgress[MAX_SLOTS];
		for (i=0; i<MAX_SLOTS; ++i)
			dbgLastProgress[i] = -1000;

		ls->setCurrentFilename(filename);
		UnsignedInt startTime = timeGetTime();
		// GeneralsX: One minute, was two. Owner's call, 2026-08-10: a stalled
		// transfer is the failure mode people actually hit, and two minutes of "Preparing..." before
		// anything is said is long enough that it reads as a hang rather than as a timeout. Halving it
		// costs nothing on a working transfer - a real map is tens to a couple of hundred KB over a
		// unicast path and completes in seconds - and returns everyone to the lobby twice as fast when
		// it is not going to work.
		//
		// This is the SAME constant on every platform because it lives in shared Core/, so host and
		// joiner agree on the deadline by construction. It is also what the "Abort in M:SS" countdown
		// on the transfer screen renders (ls->processTimeout below), so the number a player sees and
		// the number that fires are the same one.
		const Int timeoutPeriod = 1*60*1000;
		ls->processTimeout(timeoutPeriod/1000);

		ls->update(0);
		fileTransferDone = FALSE;
		fileTransferPercent = 0;

		UnsignedShort fileCommandID = 0;
		Bool sentFile = FALSE;
		if (TheGameInfo->amIHost())
		{
			Sleep(500);
			fileCommandID = TheNetwork->sendFileAnnounce(filename, mask);

			// GeneralsX: 0 means the announce was never sent - the file
			// could not be opened, or its portable name failed conversion and every receiver
			// would have refused it silently. Waiting out the full timeout on a transfer that
			// cannot start is the same 60 s dishonesty the receiver-side refusals used to cause;
			// fail at once. (The joiners still wait out their own per-file timeout for this leg -
			// they cannot know - but the host stops burning one timeout per remaining file and
			// the room is told immediately.)
			if (fileCommandID == 0)
			{
				fprintf(stderr, "[GXFILE] transfer of '%s' aborted at the send side - announce not sent\n",
					filename.str());
				fflush(stderr);
				ls->processProgress(0, 0, "MapTransfer:Timeout");
				return FALSE;
			}
		}
		else
		{
			sentFile = TRUE;
		}

		DEBUG_LOG(("Starting file transfer loop"));

		while (!fileTransferDone)
		{
			// GeneralsX: The delivery window used to be the one place a lost
			// MSG_GAME_START had no further chance to arrive: this loop blocks inside OnGameStart,
			// LANAPI::update never runs, and a joiner that missed both immediate copies has no
			// TheNetwork - so the host spent the whole timeout sending unackable chunks into a
			// void (round-2 run 6). Re-emit the stored start from here; it is nonce-idempotent,
			// self-throttled to one datagram per 2 s, and a no-op on non-hosts.
			if (TheLAN)
				TheLAN->resendPendingGameStart();

			if (!sentFile && TheNetwork->areAllQueuesEmpty())
			{
				TheNetwork->sendFile(filename, mask, fileCommandID);
				sentFile = TRUE;
			}

			// get the progress for each player, and take the min for our overall progress
			fileTransferDone = TRUE;
			fileTransferPercent = 100;

			// GeneralsX: A peer that could not write the file now says so
			// (GX_FILE_PROGRESS_FAILED, networkutil.h) instead of reporting a flat 100. Give up on it
			// at once rather than spinning out the remaining timeout: the peer is not going to
			// succeed, and two more minutes of "Recieving" in front of a failure that has already
			// happened is the same dishonesty this fix exists to remove, just slower.
			Bool transferFailed = FALSE;

			for (i=1; i<MAX_SLOTS; ++i)
			{
				if (TheGameInfo->getConstSlot(i)->isHuman() && !TheGameInfo->getConstSlot(i)->hasMap())
				{
					Int slotTransferPercent = TheNetwork->getFileTransferProgress(i, filename);
					if (s_mapDbg && slotTransferPercent != dbgLastProgress[i])
					{
						fprintf(stderr, "[GXMAPDBG] doFileTransfer: slot %d progress %d (was %d) file='%s'\n",
							i, slotTransferPercent, dbgLastProgress[i], filename.str());
						fflush(stderr);
						dbgLastProgress[i] = slotTransferPercent;
					}
					fileTransferPercent = min(fileTransferPercent, slotTransferPercent);

					if (slotTransferPercent < 0)
					{
						// "MapTransfer:Timeout" rather than a new string on purpose - it already ships
						// in every user's data and it is true (the transfer did not complete). A new
						// key would mean a four-platform DATA rollout to display one line.
						transferFailed = TRUE;
						ls->processProgress(i, 0, "MapTransfer:Timeout");
					}
					else if (slotTransferPercent == 0)
						ls->processProgress(i, slotTransferPercent, "MapTransfer:Preparing");
					else if (slotTransferPercent < 100)
						ls->processProgress(i, slotTransferPercent, "MapTransfer:Recieving");
					else
						ls->processProgress(i, slotTransferPercent, "MapTransfer:Done");
				}
			}

			if (transferFailed)
			{
				DEBUG_LOG(("doFileTransfer - a peer reported it could not write '%s'; aborting", filename.str()));
				fprintf(stderr, "[GXFILE] transfer of '%s' aborted - a peer could not write it\n", filename.str());
				fflush(stderr);
				fileTransferDone = FALSE;
				break;			// leaves fileTransferDone FALSE, so doFileTransfer returns FALSE below
			}
			if (fileTransferPercent < 100)
			{
				fileTransferDone = FALSE;
				if (fileTransferPercent == 0)
					ls->processProgress(0, fileTransferPercent, "MapTransfer:Preparing");
				else
					ls->processProgress(0, fileTransferPercent, "MapTransfer:Sending");
			}
			else
			{
				DEBUG_LOG(("File transfer is 100%%!"));
				ls->processProgress(0, fileTransferPercent, "MapTransfer:Done");
			}

			Int now = timeGetTime();
			if (now > startTime + timeoutPeriod) // bail if we don't finish in a reasonable amount of time
			{
				DEBUG_LOG(("Timing out file transfer"));
				if (s_mapDbg)
				{
					fprintf(stderr, "[GXMAPDBG] doFileTransfer TIMEOUT after %d ms: file='%s' minProgress=%d\n",
						now - (Int)startTime, filename.str(), fileTransferPercent);
					fflush(stderr);
				}
				break;
			}
			else
			{
				ls->processTimeout((startTime + timeoutPeriod - now)/1000);
			}

			ls->update(fileTransferPercent);
		}

		if (s_mapDbg)
		{
			fprintf(stderr, "[GXMAPDBG] doFileTransfer end: file='%s' done=%d elapsedMs=%u\n",
				filename.str(), fileTransferDone ? 1 : 0, (UnsignedInt)(timeGetTime() - startTime));
			fflush(stderr);
		}

		if (!fileTransferDone)
		{
			return FALSE;
		}
	}

	return TRUE;
}

//-------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------

// GeneralsX: These helpers were backslash-only, so on POSIX - where a
// map path is '/home/.../maps/foo/foo.map' - GetBasePathFromPath found no separator and returned
// empty, and every joined sidecar path came out as garbage like '\map.ini' or
// '\/full/path/foo.tga'. Measured live 2026-08-10 (Linux relay harness, GX_MAPDBG): the joiner
// polled that garbage against the announced transfer forever, timed out after 60 s, and bounced
// back to the lobby - while the bytes had landed. The host-side variant even shipped the garbage
// on the wire as a bogus "portable" name that the receiver's normalization then refused.
// Recognise both separators, and join with the separator the input path actually uses.
static const char* findLastSeparatorIn( const AsciiString& path )
{
	const char *bs = path.reverseFind('\\');
	const char *fs = path.reverseFind('/');
	if (bs != nullptr && fs != nullptr)
		return (bs > fs) ? bs : fs;
	return (bs != nullptr) ? bs : fs;
}

static char pathSeparatorFor( const AsciiString& path )
{
	const char *s = findLastSeparatorIn(path);
	return (s != nullptr) ? *s : '\\';
}

AsciiString GetBasePathFromPath( AsciiString path )
{
	const char *s = findLastSeparatorIn(path);
	if (s)
	{
		Int len = s - path.str();

		AsciiString base;
		char *buf = base.getBufferForRead(len + 1);
		memcpy(buf, path.str(), len);
		buf[len] = 0;
		return buf;
	}
	return AsciiString::TheEmptyString;
}

AsciiString GetFileFromPath( AsciiString path )
{
	const char *s = findLastSeparatorIn(path);
	if (s)
		return s+1;
	return path;
}

AsciiString GetExtensionFromFile( AsciiString fname )
{
	const char *s = fname.reverseFind('.');
	if (s)
		return s+1;
	return fname;
}

AsciiString GetBaseFileFromFile( AsciiString fname )
{
	const char *s = fname.reverseFind('.');
	if (s)
	{
		Int len = s - fname.str();

		AsciiString base;
		char *buf = base.getBufferForRead(len + 1);
		memcpy(buf, fname.str(), len);
		buf[len] = 0;
		return buf;
	}
	return AsciiString::TheEmptyString;
}

AsciiString GetPreviewFromMap( AsciiString path )
{
	AsciiString fname = GetBaseFileFromFile(GetFileFromPath(path));
	AsciiString base = GetBasePathFromPath(path);

	AsciiString out;
	out.format("%s%c%s.tga", base.str(), pathSeparatorFor(path), fname.str());
	return out;
}

AsciiString GetINIFromMap( AsciiString path )
{
	AsciiString base = GetBasePathFromPath(path);

	AsciiString out;
	out.format("%s%cmap.ini", base.str(), pathSeparatorFor(path));
	return out;
}

AsciiString GetStrFileFromMap( AsciiString path )
{
	AsciiString base = GetBasePathFromPath(path);

	AsciiString out;
	out.format("%s%cmap.str", base.str(), pathSeparatorFor(path));
	return out;
}

AsciiString GetSoloINIFromMap( AsciiString path )
{
	AsciiString base = GetBasePathFromPath(path);

	AsciiString out;
	out.format("%s%csolo.ini", base.str(), pathSeparatorFor(path));
	return out;
}

AsciiString GetAssetUsageFromMap( AsciiString path )
{
	AsciiString base = GetBasePathFromPath(path);

	AsciiString out;
	out.format("%s%cassetusage.txt", base.str(), pathSeparatorFor(path));
	return out;
}

AsciiString GetReadmeFromMap( AsciiString path )
{
	AsciiString base = GetBasePathFromPath(path);

	AsciiString out;
	out.format("%s%creadme.txt", base.str(), pathSeparatorFor(path));
	return out;
}

//-------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------

// The on-disk spelling of a loose file, empty when there is none. On a case-sensitive filesystem -
// Linux, iPadOS - the local file system finds "Map.ini" for "map.ini" when it OPENS a file, and the opened File carries the
// resolved path (File::open's setName); a raw fopen/rename of the spelling built from the map name would miss it.
static AsciiString gxLocalPathOnDisk( const AsciiString& path )
{
	File *fp = TheLocalFileSystem != nullptr ? TheLocalFileSystem->openFile(path.str(), File::READ | File::BINARY) : nullptr;
	if (fp == nullptr)
		return AsciiString::TheEmptyString;
	const AsciiString onDisk = fp->getName();
	fp->close();
	return onDisk;
}

// GeneralsX: M1 step 2b. The name a retired sidecar is renamed to (see
// retireSidecar): the next free .gxstale, .gxstale1, ... - an earlier one is never overwritten, it may be the player's own
// edit. Only loose files are ever retired; a sidecar inside an archive is the same on every install.
static AsciiString gxStaleName( const AsciiString& path )
{
	AsciiString aside;
	for (Int n = 0; n < 100; ++n)
	{
		aside = path;
		aside.concat(".gxstale");
		if (n > 0)
		{
			AsciiString num;
			num.format("%d", n);
			aside.concat(num);
		}
		if (!TheLocalFileSystem->doesFileExist(aside.str()))
			return aside;
	}
	return AsciiString::TheEmptyString;
}

// GeneralsX: M1 step 2b. A joiner about to RECEIVE this map
// retires every loose copy of a sidecar first - renamed to the next free .gxstale name, never deleted, every case variant
// ("Map.ini" and "map.ini" are two files on Linux/iPadOS and the local file system finds either) - so after reception
// exactly the host's files are active. Reception used to overwrite the player's own edit in place, and a file the host
// lacks, or a case variant beside the received one, stayed active and kept this peer's map data different forever.
// FALSE when one could not be renamed: nothing is then transferred, since this peer would still hash the old file. Every
// rename is recorded in `retired`, so a transfer that does not complete can put the player's files back.
struct GXRetiredSidecar { AsciiString name, path, aside; };
static Bool retireSidecar( const AsciiString& name, std::vector<GXRetiredSidecar>& retired )
{
	for (Int tries = 0; tries < 16; ++tries)
	{
		const AsciiString path = gxLocalPathOnDisk(name);
		if (path.isEmpty())
			return TRUE;
		const AsciiString aside = gxStaleName(path);
		const Bool moved = !aside.isEmpty() && rename(path.str(), aside.str()) == 0;
		fprintf(stderr, "[GXMAPDATA] transfer: this peer's '%s' %s%s\n", path.str(),
			moved ? "kept as " : "could NOT be moved aside, so nothing is transferred", moved ? aside.str() : "");
		fflush(stderr);
		if (!moved)
			return FALSE;
		GXRetiredSidecar r;
		r.name = name;
		r.path = path;
		r.aside = aside;
		retired.push_back(r);
	}
	return FALSE;
}

// The transfer did not complete: every retired sidecar goes back to its own spelling, newest first, so the map loads what
// it loaded before. First every copy active under one of those names NOW - a received one, in any
// spelling - is retired the same way, so the restored files are the only ones the file system can find (r12 P2: a received
// "map.ini" shadowed a restored "Map.ini" on Linux). Nothing is deleted.
static void restoreRetiredSidecars( const std::vector<GXRetiredSidecar>& retired )
{
	std::vector<GXRetiredSidecar> received;
	for (size_t i = 0; i < retired.size(); ++i)
		retireSidecar(retired[i].name, received);
	for (size_t i = retired.size(); i-- > 0;)
	{
		const GXRetiredSidecar& r = retired[i];
		FILE *fp = fopen(r.path.str(), "rb");	// still taken (a retire above failed): never rename over it
		const Bool occupied = fp != nullptr;
		if (fp != nullptr)
			fclose(fp);
		const Bool restored = !occupied && rename(r.aside.str(), r.path.str()) == 0;
		fprintf(stderr, "[GXMAPDATA] transfer failed: '%s' %s%s\n", r.path.str(),
			restored ? "restored from " : "could NOT be restored - it stays at ", r.aside.str());
	}
	fflush(stderr);
}

Bool DoAnyMapTransfers(GameInfo *game, Bool keepShellUp)
{
	TheGameInfo = game;
	// GeneralsX: scale32: the transfer recipient mask is 64 bit (slots 0-33).
	RelayMaskType mask = 0;
	Int i=0;
	for (i=1; i<MAX_SLOTS; ++i)
	{
		if (TheGameInfo->getConstSlot(i)->isHuman() && !TheGameInfo->getConstSlot(i)->hasMap())
		{
			DEBUG_LOG(("Adding player %d to transfer mask", i));
			mask |= gxSlotBit(i);
		}
	}
	static const Bool s_mapDbg = (getenv("GX_MAPDBG") != NULL);
	if (s_mapDbg)
	{
		fprintf(stderr, "[GXMAPDBG] DoAnyMapTransfers: map='%s' contentsMask=%X transferMask=%llX\n",
			game->getMap().str(), TheGameInfo->getMapContentsMask(), (unsigned long long)mask);
		fflush(stderr);
	}

	if (!mask)
		return TRUE;

	// GeneralsX: M1 step 2b. AN OFFICIAL MAP IS NEVER SENT: its files live
	// in the game's own archives and Maps folder, sendFileAnnounce opens loose local files only, and a host must not write
	// into another install's game folder. The setup screens refuse to start a lobby where a human lacks one
	// (GUI:PlayerNoMap) - and since 2b a slot also lacks it when its map.ini/solo.ini differ from the host's although the
	// .map matches (a loose override of a stock map's file, say). A start that got here anyway (an accepted slot, a
	// headless host, a slot that changed under a countdown) announced nothing on the host while every joiner waited out a
	// whole transfer timeout, and then each peer left the room. Refuse here instead, at once and on every peer (each one
	// reads the same slot list): those peers' rules differ, so the match would desync, and no transfer can repair it.
	// The same test the setup screens use: the official flag when this machine knows the map, else the user-map folder.
	const MapMetaData *mapMeta = TheMapCache != nullptr ? TheMapCache->findMap(game->getMap()) : nullptr;
	if (mapMeta != nullptr ? mapMeta->m_isOfficial : !WouldMapTransfer(game->getMap()))
	{
		fprintf(stderr, "[GXFILE] '%s' is an official map and is never transferred; slots %llX lack it or hold different "
			"map.ini/solo.ini - not starting\n", game->getMap().str(), (unsigned long long)mask);
		fflush(stderr);
		return FALSE;
	}

	const Int localSlot = TheGameInfo->getLocalSlotNum();
	const Bool receiving = !TheGameInfo->amIHost() && localSlot >= 0 && (mask & (gxSlotBit(localSlot))) != 0;
	const AsciiString realMap = TheMapCache != nullptr ? TheMapCache->getPathOnDisk(game->getMap()) : game->getMap();
	std::vector<GXRetiredSidecar> retired;
	if (receiving && (!retireSidecar(GetINIFromMap(realMap), retired) || !retireSidecar(GetSoloINIFromMap(realMap), retired)))
	{
		restoreRetiredSidecars(retired);
		return FALSE;
	}

	// See keepShellUp in FileTransfer.h: a delivery-run transfer overlays the live room screen;
	// shutting the shell down here is what forced a room re-init on return.
	if (!keepShellUp)
		TheShell->hideShell();
	MapTransferLoadScreen *ls = NEW MapTransferLoadScreen;
	ls->init(TheGameInfo);
	Bool ok = TRUE;
	if (TheGameInfo->getMapContentsMask() & 2)
		ok = doFileTransfer(GetPreviewFromMap(game->getMap()), ls, mask);
	if (ok && TheGameInfo->getMapContentsMask() & 4)
		ok = doFileTransfer(GetINIFromMap(game->getMap()), ls, mask);
	if (ok && TheGameInfo->getMapContentsMask() & 8)
		ok = doFileTransfer(GetStrFileFromMap(game->getMap()), ls, mask);
	if (ok && TheGameInfo->getMapContentsMask() & 16)
		ok = doFileTransfer(GetSoloINIFromMap(game->getMap()), ls, mask);
	if (ok && TheGameInfo->getMapContentsMask() & 32)
		ok = doFileTransfer(GetAssetUsageFromMap(game->getMap()), ls, mask);
	if (ok && TheGameInfo->getMapContentsMask() & 64)
		ok = doFileTransfer(GetReadmeFromMap(game->getMap()), ls, mask);
	if (ok)
		ok = doFileTransfer(game->getMap(), ls, mask);
	delete ls;
	ls = nullptr;
	if (s_mapDbg)
	{
		fprintf(stderr, "[GXMAPDBG] DoAnyMapTransfers: result=%d\n", ok ? 1 : 0);
		fflush(stderr);
	}
	if (!ok)
	{
		restoreRetiredSidecars(retired);
		// The failure path bails out of the game entirely, and its established UX is the full
		// shutdown+init cycle. A keepShellUp run skipped the hide, so replicate the cycle here.
		if (keepShellUp)
			TheShell->hideShell();
		TheShell->showShell();
	}
	return ok;
}
