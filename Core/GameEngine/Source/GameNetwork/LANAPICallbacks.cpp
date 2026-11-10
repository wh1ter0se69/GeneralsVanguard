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
// FILE: LANAPICallbacks.cpp
// Author: Chris Huybregts, October 2001
// Description: LAN API Callbacks
///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include <cstdint>
#include <time.h>   // gmtime/strftime for simIdBuiltWhen
#include "WWLib/strtok_r.h"
// GeneralsX: SimID join-time compatibility diagnostics.
#include "Common/Diagnostic/SimulationId.h"
#include "Common/GameEngine.h"
#include "Common/GameState.h"
#include "Common/GlobalData.h"
#include "Common/MessageStream.h"
#include "Common/MultiplayerSettings.h"
#include "Common/PlayerTemplate.h"
#include "Common/QuotedPrintable.h"
#include "Common/RandomValue.h"
#include "Common/UserPreferences.h"
#include "GameClient/GameText.h"
#include "GameClient/LanguageFilter.h"
#include "GameClient/MapUtil.h"
#include "GameClient/MessageBox.h"
#include "GameLogic/GameLogic.h"
#include "GameNetwork/FileTransfer.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameClient/GXAdminKey.h"	// GeneralsX @security the match signing key, promoted at a real start
#include "GameNetwork/networkutil.h"

LANAPI *TheLAN = nullptr;
extern Bool LANbuttonPushed;


//Colors used for the chat dialogs
const Color playerColor =  GameMakeColor(255,255,255,255);
const Color gameColor =  GameMakeColor(255,255,255,255);
const Color gameInProgressColor =  GameMakeColor(128,128,128,255);
const Color chatNormalColor =  GameMakeColor(50,215,230,255);
const Color chatActionColor =  GameMakeColor(255,0,255,255);
const Color chatLocalNormalColor =  GameMakeColor(255,128,0,255);
const Color chatLocalActionColor =  GameMakeColor(128,255,255,255);
const Color chatSystemColor =  GameMakeColor(255,255,255,255);
const Color chatLogColor =  GameMakeColor(255,255,0,255);	// [LOG] lines - owner asked for yellow
const Color acceptTrueColor =  GameMakeColor(0,255,0,255);
const Color acceptFalseColor =  GameMakeColor(255,0,0,255);

// GeneralsX: One narration line: yellow [LOG] chat for the humans,
// [GXLOG] stderr for the harnesses. Chat entries append even while the shell is hidden by the
// transfer screen, so lines logged mid-transfer are visible the moment the room comes back.
static void gxLobbyLog( LANAPI *lan, const UnicodeString &msg )
{
	fprintf(stderr, "[GXLOG] %ls\n", msg.str());
	fflush(stderr);
	if (lan != nullptr)
		lan->OnChat(UnicodeString(L"LOG"), 0, msg, LANAPIInterface::LANCHAT_LOG);
}

UnicodeString LANAPIInterface::getErrorStringFromReturnType( ReturnType ret )
{
	switch (ret)
	{
		case RET_OK:
			return TheGameText->fetch("LAN:OK");
		case RET_TIMEOUT:
			return TheGameText->fetch("LAN:ErrorTimeout");
		case RET_GAME_FULL:
			return TheGameText->fetch("LAN:ErrorGameFull");
		case RET_DUPLICATE_NAME:
			return TheGameText->fetch("LAN:ErrorDuplicateName");
		case RET_CRC_MISMATCH:
			return TheGameText->fetch("LAN:ErrorCRCMismatch");
		case RET_GAME_STARTED:
			return TheGameText->fetch("LAN:ErrorGameStarted");
		case RET_GAME_EXISTS:
			return TheGameText->fetch("LAN:ErrorGameExists");
		case RET_GAME_GONE:
			return TheGameText->fetch("LAN:ErrorGameGone");
		case RET_BUSY:
			return TheGameText->fetch("LAN:ErrorBusy");
		case RET_SERIAL_DUPE:
			return TheGameText->fetch("WOL:ChatErrorSerialDup");
		// GeneralsX: Lobby passwords. FETCH_OR_SUBSTITUTE rather than
		// fetch, so the feature needs no modified CSF - editing one would move dataID and every
		// peer would then refuse every join with DATA_DIFFERS, which is a spectacular price to
		// pay for two strings.
		case RET_NEEDS_PASSWORD:
			return TheGameText->FETCH_OR_SUBSTITUTE("LAN:ErrorNeedsPassword",
				L"This game is password protected. Type /password <password> in the chat box, then Join again.");
		case RET_BAD_PASSWORD:
			return TheGameText->FETCH_OR_SUBSTITUTE("LAN:ErrorBadPassword",
				L"Wrong password. Type /password <password> in the chat box to correct it, then Join again.");
		// GeneralsX: Build-kind mismatch. FETCH_OR_SUBSTITUTE for the same
		// reason as the two above: a modified CSF moves dataID, and every peer would then refuse
		// every join with DATA_DIFFERS - a spectacular price for one string.
		//
		// It says WHY rather than just "refused", because the whole value of this refusal is that
		// somebody finds out they are on the wrong build now rather than after twenty minutes of a
		// match that is inexplicably slow. The specific flags on both sides are in the log.
		case RET_DEBUG_PEER:
			// It does NOT point at a log file. The first version of this string sent the reader to
			// gxlaunch.log, which is the Windows launcher's log and not the engine's - and the
			// engine-side detail was a DEBUG_LOG, which every release preset compiles to
			// ((void)0). So it directed somebody to a file that does not contain it, on a machine
			// that may not have that file at all. The numbers now go to stderr unconditionally in
			// handleJoinDeny; this string says the thing the player can act on.
			return TheGameText->FETCH_OR_SUBSTITUTE("LAN:ErrorDebugPeer",
				L"This game only admits peers running the same kind of build. One of you is on a debug, profiled or sanitizer build and the other is not - they do not simulate identically, so the match would drift apart.");
		default:
			return TheGameText->fetch("LAN:ErrorUnknown");
	}
}

// TheSuperHackers @feature arcticdolphin 08/08/2026 Adds /me emotes to LAN player chat, consistent with WOL
//
// GeneralsX: The emote body must NOT be re-parsed as a lobby command.
//
// Upstream calls RequestChat with the peeled text, and in THIS fork RequestChat is also the
// interception point for lobby commands - it hands its argument to applyLobbyCommand before
// anything is built or sent (LANAPI.cpp, RequestChat). So "/me /password hunter2" would peel to
// "/password hunter2", be recognised by applyLobbyCommand, and BIND THE HOST PASSWORD instead of
// saying the words - a side effect the player never asked for and a disclosure of the password to
// whoever is watching the screen.
//
// Refused rather than escaped, and refused on the leading slash alone rather than against a copy
// of applyLobbyCommand's verb list. Two copies of that list already exist (LANAPI.cpp, and the
// slash-escape guard in GXLobbyCommands.cpp whose own comment warns that a third would drift); a
// body beginning with a slash is the entire shape that can be re-read as a command, so the one
// condition can never fall out of sync.
void LANAPI::RequestPlayerChat( UnicodeString message )
{
	if (message.startsWithNoCase(L"/me "))
	{
		UnicodeString emote(message.str() + 4);
		if (emote.isEmpty())
			return;					// nothing after the verb - there is no emote to send

		if (emote.getCharAt(0) == L'/')
		{
			OnChat(UnicodeString::TheEmptyString, m_localIP,
				TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXEmoteSlashBody",
					L"An emote cannot begin with a slash - it would be read as a command instead "
					L"of said. Nothing was sent."),
				LANCHAT_SYSTEM);
			return;
		}

		RequestChat(emote, LANCHAT_EMOTE);
		return;
	}

	RequestChat(message, LANCHAT_NORMAL);
}

// On functions are (generally) the result of network traffic

void LANAPI::OnAccept( UnsignedInt playerIP, Bool status )
{
	if( AmIHost() )
	{
		Int i = 0;
		for (; i < MAX_SLOTS; i++)
		{
			if (m_currentGame->getIP(i) == playerIP)
			{
				if(status)
					m_currentGame->getLANSlot(i)->setAccept();
				else
					m_currentGame->getLANSlot(i)->unAccept();
				break;
			}
		}
		if (i != MAX_SLOTS )
		{
			RequestGameOptions( GenerateGameOptionsString(), false );
			lanUpdateSlotList();
		}
	}
	else
	{
		//i'm not the host but if the accept came from the host...
		if( m_currentGame->getIP(0) == playerIP )
		{
			UnicodeString text;
			text = TheGameText->fetch("GUI:HostWantsToStart");
			OnChat(L"SYSTEM", m_localIP, text, LANCHAT_SYSTEM);
		}
	}
}

void LANAPI::OnHasMap( UnsignedInt playerIP, Bool status )
{
	// GeneralsX: (cross-model review) The whole body was gated on
	// AmIHost(), so a non-host peer never learned OTHER peers' availability. That kept every
	// joiner's map icon frozen on other players' rows, and after a transfer-only first start it
	// split lobbies of three or more humans: a peer whose slot list still said another recipient
	// lacked the map recomputed needTransfer=TRUE on the second start and sat waiting for a
	// transfer nobody was running, while the host - whose list was current - started the match.
	// Every peer keeps its slot list current now; only the "player has no map" chat announcement
	// stays host-side, exactly as before.
	// GeneralsX: (cross-model review) Availability now arrives repeatedly -
	// a reassert burst after every announce plus a 10 s keepalive piggyback (LANAPI::update) - so
	// the chat announcement below must fire on the TRANSITION, not on every datagram, or the host
	// chat fills with one "player has no map" line per reassert.
	Bool wasAvailable = TRUE;
	Int i = 0;
	for (; i < MAX_SLOTS; i++)
	{
		if (m_currentGame->getIP(i) == playerIP)
		{
			wasAvailable = m_currentGame->getLANSlot(i)->hasMap();
			m_currentGame->getLANSlot(i)->setMapAvailability( status );
			break;
		}
	}
	if (i != MAX_SLOTS && wasAvailable != status)
		lanUpdateSlotList();

	if( AmIHost() )
	{
		if (i != MAX_SLOTS )
		{
			UnicodeString mapDisplayName;
			const MapMetaData *mapData = TheMapCache->findMap( m_currentGame->getMap() );
			Bool willTransfer = TRUE;
			if (mapData)
			{
				mapDisplayName.format(L"%ls", mapData->m_displayName.str());
				if (mapData->m_isOfficial)
					willTransfer = FALSE;
			}
			else
			{
				// GeneralsX: The map's NAME, never its path. This fallback
				// used to print the full local path into every peer's chat (owner report).
				mapDisplayName.format(L"%hs", TheGameState->getMapLeafName(m_currentGame->getMap()).str());
				willTransfer = WouldMapTransfer(m_currentGame->getMap());
			}
			if (!status && wasAvailable)
			{
				UnicodeString text;
				if (willTransfer)
					text.format(TheGameText->fetchGameOnly("GUI:PlayerNoMapWillTransfer"), m_currentGame->getLANSlot(i)->getName().str(), mapDisplayName.str());
				else
					text.format(TheGameText->fetchGameOnly("GUI:PlayerNoMap"), m_currentGame->getLANSlot(i)->getName().str(), mapDisplayName.str());
				OnChat(L"SYSTEM", m_localIP, text, LANCHAT_SYSTEM);
			}
		}
	}
}

void LANAPI::OnGameStartTimer( Int seconds )
{
	UnicodeString text;
	if (seconds == 1)
		text.format(TheGameText->fetchGameOnly("LAN:GameStartTimerSingular"), seconds);
	else
		text.format(TheGameText->fetchGameOnly("LAN:GameStartTimerPlural"), seconds);
	OnChat(L"SYSTEM", m_localIP, text, LANCHAT_SYSTEM);
}

void LANAPI::OnGameStart()
{
	//DEBUG_LOG(("Map is '%s', preview is '%s'", m_currentGame->getMap().str(), GetPreviewFromMap(m_currentGame->getMap()).str()));
	//DEBUG_LOG(("Map is '%s', INI is '%s'", m_currentGame->getMap().str(), GetINIFromMap(m_currentGame->getMap()).str()));

	// GeneralsX: A HOST MAY NOT MAKE ANYBODY AN ADMIN, NOR UNMAKE ONE, NOR SWAP A KEY. The
	// slots and keys adopted from MSG_GAME_START (handleGameStart) say who may cheat in a match with more than one
	// human and what their orders must be signed under, and every simulation must run on that one answer, so a single
	// peer cannot correct it - it can only refuse to play on it. It refuses unless the stamp is EXACTLY what a fresh
	// answer from its own relay says, asked now and tied to the question by a nonce:
	//   * a slot or key the relay does not back - a host granting admin, or keeping a revoked one - is refused, and so
	//     is ANY stamp this peer cannot confirm with a fresh answer: a privilege fails closed;
	//   * an admin the relay names but the host left out - a host stripping an admin - is refused too. With no fresh
	//     answer and nothing stamped the match starts: nothing is granted, and a relay that did not answer must not
	//     cost an ordinary match its start.
	// Refusing takes this peer out before any network exists, exactly as a peer without the map does below.
	//
	// GeneralsX: ...EXCEPT a start the host stamped "unconfirmed": the host's
	// own relay query failed, so it grants no admin and every peer runs on exactly that - no admin, no keys. Nothing to
	// confirm, so nothing is asked; refusing it would end an honest match over the host's timeout.
	if (m_currentGame && !m_currentGame->amIHost() && m_gxStartAdminUnconfirmed)
	{
		fprintf(stderr, "[GXADMIN] start: admin mask 0x%llX unconfirmed by the host - the match runs with no admin\n",
			(unsigned long long)m_currentGame->getGxAdminMask());
		fflush(stderr);
	}
	else if (m_currentGame && !m_currentGame->amIHost())
	{
		const RelayMaskType stamped = m_currentGame->getGxAdminMask();
		const Bool relay = (m_transport != NULL && m_transport->isRelayEnabled());
		const Bool fresh = relay && m_transport->refreshRelayAdminKeysNow(2000);
		RelayMaskType proven = 0;
		unsigned char provenKeys[MAX_SLOTS][32];
		memset(provenKeys, 0, sizeof(provenKeys));
		if (fresh)
			gxRelayAdmins(m_currentGame, &proven, provenKeys);
		Bool keysMatch = TRUE;
		for (Int i = 0; i < MAX_SLOTS; ++i)
			if ((stamped & (gxSlotBit(i))) != 0 && memcmp(provenKeys[i], m_currentGame->getGxAdminKey(i), 32) != 0)
				keysMatch = FALSE;
		const Bool refuse = fresh ? (proven != stamped || !keysMatch) : (stamped != 0);
		if (refuse)
		{
			fprintf(stderr, "[GXADMIN] refused to start: the host stamped admin mask 0x%llX, the relay proves %s0x%llX%s\n",
				(unsigned long long)stamped, fresh ? "" : "nothing (no fresh answer) ", (unsigned long long)proven,
				(fresh && proven == stamped && !keysMatch) ? " with other keys" : "");
			fflush(stderr);
			OnPlayerLeave(m_name);
			removeGame(m_currentGame);
			delete m_currentGame;
			m_currentGame = nullptr;
			m_inLobby = TRUE;
			OnChat(UnicodeString::TheEmptyString, 0,
				UnicodeString(L"Left the game: the host's list of admins does not match the server's."),
				LANCHAT_SYSTEM);
			gxLobbyLog(this, UnicodeString(L"refused to start - the admin list does not match the server's"));
			return;
		}
		if (relay)
		{
			fprintf(stderr, "[GXADMIN] start: admin mask 0x%llX %s\n", (unsigned long long)stamped,
				fresh ? "confirmed by the relay" : "unchecked - no fresh answer, and nothing granted");
			fflush(stderr);
		}
	}

	if (m_currentGame)
	{
		LANPreferences pref;
		AsciiString option;
		option.format("%d", m_currentGame->getLANSlot( m_currentGame->getLocalSlotNum() )->getPlayerTemplate());
		pref["PlayerTemplate"] = option;
		option.format("%d", m_currentGame->getLANSlot( m_currentGame->getLocalSlotNum() )->getColor());
		pref["Color"] = option;
		if (m_currentGame->amIHost())
    {
    	pref["Map"] = AsciiStringToQuotedPrintable(m_currentGame->getMap());
      pref.setSuperweaponRestricted( m_currentGame->getSuperweaponRestriction() > 0 );
      pref.setBool( "LimitArmies", m_currentGame->oldFactionsOnly() );	// GeneralsX: read back by LanGameOptionsMenuInit
      pref.setStartingCash( m_currentGame->getStartingCash() );
    }
		pref.write();

		m_isInLANMenu = FALSE;

		//m_currentGame->startGame(0);

		// Set up the game network
		DEBUG_ASSERTCRASH(TheNetwork == nullptr, ("For some reason TheNetwork isn't null at the start of this game.  Better look into that."));

		delete TheNetwork;
		TheNetwork = nullptr;

		// Time to initialize TheNetwork for this game.
		TheNetwork = NetworkInterface::createNetwork();
		TheNetwork->init();
		TheNetwork->setLocalAddress(m_localIP, 8088);
		TheNetwork->initTransport();

		TheNetwork->parseUserList(m_currentGame);

		// GeneralsX: Owner's flow: the FIRST start only delivers the
		// map, everyone returns to the room with the icons refreshed, and the SECOND start plays.
		// Tying the transfer to the start action is what keeps this simple: TheNetwork exists,
		// the blocking transfer screen is already this path's UX, and every start re-evaluates
		// who lacks the map - so a map change or a new joiner between starts just makes the next
		// start another transfer round instead of needing invalidation machinery.
		Bool needTransfer = FALSE;
		for (Int slotIdx = 1; slotIdx < MAX_SLOTS; ++slotIdx)
		{
			if (m_currentGame->getConstSlot(slotIdx)->isHuman() &&
					!m_currentGame->getConstSlot(slotIdx)->hasMap())
				needTransfer = TRUE;
		}

		if (needTransfer)
		{
			UnicodeString mapLeaf;
			mapLeaf.translate(GetFileFromPath(m_currentGame->getMap()));
			UnicodeString logLine;
			if (m_currentGame->amIHost())
				logLine.format(L"map transfer: sending '%ls' to players without it", mapLeaf.str());
			else if (!m_currentGame->getConstSlot(m_currentGame->getLocalSlotNum())->hasMap())
				logLine.format(L"map transfer: receiving '%ls' from the host", mapLeaf.str());
			else
				logLine.format(L"map transfer: waiting while '%ls' is delivered", mapLeaf.str());
			gxLobbyLog(this, logLine);
		}

		Bool filesOk = DoAnyMapTransfers(m_currentGame, needTransfer);

		// see if we really have the map.  if not, back out.
		// GeneralsX @debug /08/2026 GX_MAPDBG: A1 measurement probe on the ACTUAL bounce
		// gate - filesOk and findMap() are the only two things that can send a peer back to the
		// lobby here. findMap is a pure lowercase+find, so the extra call under the gate is safe.
		static const Bool s_mapDbg = (getenv("GX_MAPDBG") != NULL);
		Int dbgCacheBefore = 0;
		if (s_mapDbg)
			dbgCacheBefore = (Int)TheMapCache->size();
		TheMapCache->updateCache();
		if (s_mapDbg)
		{
			fprintf(stderr, "[GXMAPDBG] OnGameStart gate: filesOk=%d findMap('%s') %s; cache %d -> %d entries\n",
				filesOk ? 1 : 0, m_currentGame->getMap().str(),
				(TheMapCache->findMap(m_currentGame->getMap()) != nullptr) ? "HIT" : "MISS",
				dbgCacheBefore, (Int)TheMapCache->size());
			fflush(stderr);
		}
		if (!filesOk || TheMapCache->findMap(m_currentGame->getMap()) == nullptr)
		{
			DEBUG_LOG(("After transfer, we didn't really have the map.  Bailing..."));
			if (s_mapDbg)
			{
				fprintf(stderr, "[GXMAPDBG] OnGameStart: BAILING back to lobby\n");
				fflush(stderr);
			}
			OnPlayerLeave(m_name);
			removeGame(m_currentGame);
			delete m_currentGame;
			m_currentGame = nullptr;
			m_inLobby = TRUE;

			delete TheNetwork;
			TheNetwork = nullptr;

			OnChat(UnicodeString::TheEmptyString, 0, TheGameText->fetch("GUI:CouldNotTransferMap"), LANCHAT_SYSTEM);
			gxLobbyLog(this, UnicodeString(L"map transfer FAILED - returning to the lobby"));
			return;
		}

		if (needTransfer)
		{
			// GeneralsX: DRAIN BEFORE TEARDOWN. Measured live on the
			// Windows-host + macOS-joiner topology: the joiner finished its legs in 777 ms,
			// deleted TheNetwork and restored its lobby while the HOST was still collecting the
			// joiner's reliable progress reports - reports only the joiner's retransmit
			// machinery could guarantee delivery of. With the sender gone, the host's view
			// crawled and timed out its whole transfer. areAllQueuesEmpty() is the receipt:
			// every reliable command this peer sent (including its 100% progress reports) has
			// been ACKNOWLEDGED by its recipients, so tearing down after it cannot strand
			// anybody mid-loop. Bounded so a vanished peer cannot wedge us here - on timeout we
			// tear down anyway, which is exactly the pre-drain behaviour.
			{
				const UnsignedInt drainStart = timeGetTime();
				while (!TheNetwork->areAllQueuesEmpty()
						&& (UnsignedInt)(timeGetTime() - drainStart) < 10000)
				{
					TheNetwork->liteupdate();
					Sleep(10);
				}
				if (!TheNetwork->areAllQueuesEmpty())
				{
					fprintf(stderr, "[GXLOG] transfer drain timed out with unacknowledged traffic\n");
					fflush(stderr);
				}
			}

			// The transfer succeeded and the map resolves locally - but this start was a
			// delivery run, not a launch. The room screen was never shut down (keepShellUp),
			// so there is NOTHING to re-show and no init to re-run - the failure that emptied
			// the host's slot list, doubled the password row and swapped the background was
			// precisely a showShell() re-init here. Put the room back startable: TheNetwork is
			// recreated by the next start (which asserts it is null), the availability flags
			// re-announce so every peer's map icon flips, and the menu resets its start
			// machinery via lanNotifyTransferOnlyStart.
			delete TheNetwork;
			TheNetwork = nullptr;
			m_isInLANMenu = TRUE;

			// GeneralsX: Back in the lobby, so no admin mask: the next start stamps a fresh
			// one, and a lobby never announces one (LANAPI.h beside g_announcePrefixAllowance).
			m_currentGame->clearGxAdmin();

			// The exact lobby re-entry sequence (LanGameOptionsMenu.cpp:1456): force the local
			// has-map recheck against the just-refreshed cache, then tell the room.
			m_currentGame->setMapCRC( m_currentGame->getMapCRC() );
			m_currentGame->setMapSize( m_currentGame->getMapSize() );
			RequestHasMap();

			// Reconverge every peer from the LIVE room state rather than trusting whatever
			// snapshot each held through the transfer: the host re-broadcasts its game options
			// - which is the one authoritative slot list - with accepts reset, so the room
			// comes back exactly as a normal lobby: everyone present, everyone un-readied,
			// ready up and start. (Measured live 2026-08-10: without this, three peers came
			// back holding three different room snapshots.)
			//
			// The options string serializes each slot's hasMap (GameInfo.cpp:937), and the
			// recipients' own RequestHasMap updates have NOT arrived yet - the host would
			// serialize the pre-transfer FALSE and stamp it back onto everyone (cross-model
			// review catch). The host holds better evidence than any message: its transfer
			// loop just saw 100% from every recipient, so mark the delivered slots before
			// serializing. Their own has-map announcements confirm moments later.
			if (m_currentGame->amIHost())
			{
				for (Int slotIdx = 1; slotIdx < MAX_SLOTS; ++slotIdx)
				{
					GameSlot *slot = m_currentGame->getSlot(slotIdx);
					if (slot != nullptr && slot->isHuman() && !slot->hasMap())
						slot->setMapAvailability(TRUE);
				}
				m_currentGame->resetAccepted();
				RequestGameOptions(GameInfoToAsciiString(m_currentGame), TRUE);
			}

			lanNotifyTransferOnlyStart();

			if (m_currentGame->amIHost())
				gxLobbyLog(this, UnicodeString(L"map transfer complete - start when everyone is ready"));
			else
				gxLobbyLog(this, UnicodeString(L"map transfer complete - the map is installed, ready up"));
			return;
		}

		// GeneralsX: Clear the game data only once the start is known to
		// be a real launch. This used to run BEFORE the transfer - and the animated shell-map
		// background IS a game (GAME_SHELL), so a delivery-run start unloaded it and the restored
		// room sat on a dead backdrop (owner report). During the transfer the shell game is merely
		// frozen behind the transfer screen (the blocking loop pumps no logic), and it resumes
		// untouched when the room comes back.
		if (TheGameLogic->isInGame())
			TheGameLogic->clearGameData();

		m_currentGame->startGame(0);

		// GeneralsX: THE MATCH REALLY BEGINS: if our slot is an admin slot stamped with the key
		// we offer, that key becomes this match's signing key and a fresh one is offered for the next match
		// (GameClient/GXAdminKey.h). Here and not at the top: a start that only delivers the map must not spend the key.
		{
			const Int local = m_currentGame->getLocalSlotNum();
			const Bool mine = local >= 0 && (m_currentGame->getGxAdminMask() & (gxSlotBit(local))) != 0;
			gxAdminKeyBeginMatch(mine ? m_currentGame->getGxAdminKey(local) : NULL, (unsigned int)m_currentGame->getSeed());
			if (mine)
			{
				fprintf(stderr, "[GXADMIN] start: this peer is an admin of the match (slot %d)%s\n", (int)local,
					gxAdminKeyState().matchOk ? "" : " - but the stamped key is neither its fresh key nor its last match key "
					"under a new seed, so it cannot sign");
				fflush(stderr);
			}
		}

		// shutdown the top, but do not pop it off the stack
		//TheShell->hideShell();
		// setup the Global Data with the Map and Seed
		TheWritableGlobalData->m_pendingFile = m_currentGame->getMap();

		// send a message to the logic for a new game
		GameMessage *msg = TheMessageStream->appendMessage( GameMessage::MSG_NEW_GAME );
		msg->appendIntegerArgument(GAME_LAN);

		// GeneralsX: Do NOT force the render limiter off for the match.
		//
		// This used to set m_useFpsLimit = false. Nothing turns it back on until clearGameData(),
		// so FramePacer::isActualFramesPerSecondLimitEnabled() was false for the WHOLE match and
		// getActualFramesPerSecondLimit() returned UncappedFpsValue - the render loop ran unbounded
		// from the first frame to the last. Measured at ~540 iterations/second against a 30 Hz
		// logic rate, 18x, on a sample where the logic frame was still ADVANCING at 29/s, so this
		// was never limited to the blocked case.
		//
		// The stated reason was that peers agree the logic rate between themselves and a local cap
		// must not fight it. That conflates the render rate with the logic rate. The negotiated
		// rate is hard-capped at 30 before it is broadcast (ConnectionManager.cpp, newMinFps > 30),
		// and canUpdateNetworkGameLogic() gates purely on isFrameDataReady() and never reads the
		// render limit at all - so the render cap cannot fight it.
		//
		// Removing the assignment restores the player's OWN preference rather than imposing one.
		// That matters because rendering above the logic rate is not waste here: W3DDisplay hands
		// getLogicTimeStepMilliseconds() to WW3D::Update_Logic_Frame_Time per rendered frame, and
		// sway, water, trees, camera shake and drawable animation all scale off it. Someone who
		// selected Uncapped keeps uncapped; someone on the default keeps 30.
		//
		// Safe against dragging the lobby down: FramePacer::getActualFramesPerSecondLimit never
		// runs the loop below the tick rate, whatever the cap says (27/09/2026: the cap is the
		// player's one max-FPS setting, 0 = unlimited, and the old 8x network floor is gone), so the
		// cap can never pull TheDisplay->getAverageFPS() - which FrameMetrics feeds to the packet
		// router - below the logic rate.

		// Set the seeds
		InitRandom( m_currentGame->getSeed() );
		DEBUG_LOG(("InitRandom( %d )", m_currentGame->getSeed()));
	}
}

void LANAPI::OnGameOptions( UnsignedInt playerIP, Int playerSlot, AsciiString options )
{
	if (!m_currentGame)
		return;

	if (m_currentGame->getIP(playerSlot) != playerIP)
		return; // He's not in our game?!?


	if (m_currentGame->isGameInProgress())
		return; // we don't want to process any game options while in game.

	if (playerSlot == 0 && !m_currentGame->amIHost())
	{
		m_currentGame->setLastHeard(timeGetTime());
		AsciiString oldOptions = GameInfoToAsciiString(m_currentGame); // save these off for if we get booted
		if(ParseGameOptionsString(m_currentGame,options))
		{
			lanUpdateSlotList();
			updateGameOptions();
		}
		Bool booted = true;
		for(Int player = 1; player< MAX_SLOTS; player++)
		{
			if(m_currentGame->getIP(player) == m_localIP)
			{
				booted = false;
				break;
			}
		}
		if(booted)
		{
			// restore the options with us in so we can save prefs
			ParseGameOptionsString(m_currentGame, oldOptions);
			OnPlayerLeave(m_name);
		}

	}
	else
	{
		// Check for user/host updates
		{
			AsciiString key;
			AsciiString munkee = options;
			munkee.nextToken(&key, "=");
			//DEBUG_LOG(("GameOpt request: key=%s, val=%s from player %d", key.str(), munkee.str(), playerSlot));

			LANGameSlot *slot = m_currentGame->getLANSlot(playerSlot);
			if (!slot)
				return;

			// GeneralsX: The value is the rest after "=", and str()+1 skips it. With no
			// "=" the rest is empty and str() is a one-byte static, so +1 read past it (sweep v-net-06). Every honest
			// option carries "=value"; one without is ignored.
			if (key == "User")
			{
				if (!munkee.isEmpty())
					slot->setLogin(munkee.str()+1);
				return;
			}
			else if (key == "Host")
			{
				if (!munkee.isEmpty())
					slot->setHost(munkee.str()+1);
				return;
			}
		}

		// Parse player requests (side, color, etc)
		if( AmIHost() && m_localIP != playerIP)
		{
			if (options.compare("HELLO") == 0)
			{
				m_currentGame->setPlayerLastHeard(playerSlot, timeGetTime());
			}
			else
			{
				m_currentGame->setPlayerLastHeard(playerSlot, timeGetTime());
				Bool change = false;
				Bool shouldUnaccept = false;
				AsciiString key;
				options.nextToken(&key, "=");
				if (options.isEmpty())
					return;	// GeneralsX: no "=value" - see the User/Host note above
				Int val = atoi(options.str()+1);
				DEBUG_LOG(("GameOpt request: key=%s, val=%s from player %d", key.str(), options.str(), playerSlot));

				LANGameSlot *slot = m_currentGame->getLANSlot(playerSlot);
				if (!slot)
					return;

				if (key == "Color")
				{
					if (val >= -1 && val < TheMultiplayerSettings->getNumColors() && val != slot->getColor() && slot->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER)
					{
						Bool colorAvailable = TRUE;
						if(val != -1 )
						{
							for(Int i=0; i <MAX_SLOTS; i++)
							{
								LANGameSlot *checkSlot = m_currentGame->getLANSlot(i);
								if(val == checkSlot->getColor() && slot != checkSlot)
								{
									colorAvailable = FALSE;
									break;
								}
							}
						}
						if(colorAvailable)
							slot->setColor(val);
						change = true;
					}
					else
					{
						DEBUG_LOG(("Rejecting invalid color %d", val));
					}
				}
				else if (key == "PlayerTemplate")
				{
					// GeneralsX: Limit Armies, host side: a joiner asking for a
					// general's sub-faction while it is on gets Random instead - the retail rule from the
					// GameSpy staging room (WOLGameSetupMenu.cpp), which the LAN/relay path never had.
					Bool coerced = false;
					if (m_currentGame->oldFactionsOnly() && val >= 0 && val < ThePlayerTemplateStore->getPlayerTemplateCount())
					{
						const PlayerTemplate *fac = ThePlayerTemplateStore->getNthPlayerTemplate(val);
						if (fac == nullptr || !fac->isOldFaction() || fac->getStartingBuilding().isEmpty())	// the lobby's predicate
						{
							val = PLAYERTEMPLATE_RANDOM;
							coerced = true;
						}
					}
					// The joiner has already put its pick on its own slot (LanGameOptionsMenu
					// handlePlayerTemplateSelection) and unaccepted. When the slot here is already Random, the
					// coercion changes nothing on this side - but the slot lists now differ, and a start before
					// some later broadcast would begin two different games. Send the host's slot list back and
					// unaccept, as a real change does.
					if (coerced && val == slot->getPlayerTemplate())
					{
						change = true;
						shouldUnaccept = true;
					}
					if (val >= PLAYERTEMPLATE_MIN && val < ThePlayerTemplateStore->getPlayerTemplateCount() && val != slot->getPlayerTemplate())
					{
						slot->setPlayerTemplate(val);
						if (val == PLAYERTEMPLATE_OBSERVER)
						{
							slot->setColor(-1);
							slot->setStartPos(-1);
							slot->setTeamNumber(-1);
						}
						change = true;
						shouldUnaccept = true;
					}
					else
					{
						DEBUG_LOG(("Rejecting invalid PlayerTemplate %d", val));
					}
				}
				else if (key == "StartPos" && slot->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER)
				{

					if (val >= -1 && val < MAX_SLOTS && val != slot->getStartPos())
					{
						Bool startPosAvailable = TRUE;
						if(val != -1)
							for(Int i=0; i <MAX_SLOTS; i++)
							{
								LANGameSlot *checkSlot = m_currentGame->getLANSlot(i);
								if(val == checkSlot->getStartPos() && slot != checkSlot)
								{
									startPosAvailable = FALSE;
									break;
								}
							}
						if(startPosAvailable)
							slot->setStartPos(val);
						change = true;
						shouldUnaccept = true;
					}
					else
					{
						DEBUG_LOG(("Rejecting invalid startPos %d", val));
					}
				}
				else if (key == "Team")
				{
					if (val >= -1 && val < MAX_SLOTS/2 && val != slot->getTeamNumber() && slot->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER)
					{
						slot->setTeamNumber(val);
						change = true;
						shouldUnaccept = true;
					}
					else
					{
						DEBUG_LOG(("Rejecting invalid team %d", val));
					}
				}
				else if (key == "NAT")
				{
					if ((val >= FirewallHelperClass::FIREWALL_TYPE_SIMPLE) &&
							(val <= FirewallHelperClass::FIREWALL_TYPE_DESTINATION_PORT_DELTA))
					{
						slot->setNATBehavior((FirewallHelperClass::FirewallBehaviorType)val);
						DEBUG_LOG(("NAT behavior set to %d for player %d", val, playerSlot));
						change = true;
					}
					else
					{
						DEBUG_LOG(("Rejecting invalid NAT behavior %d", (Int)val));
					}
				}

				if (change)
				{
					if (shouldUnaccept)
						m_currentGame->resetAccepted();
					RequestGameOptions(GenerateGameOptionsString(), true);
					lanUpdateSlotList();
					DEBUG_LOG(("Slot value is color=%d, PlayerTemplate=%d, startPos=%d, team=%d",
						slot->getColor(), slot->getPlayerTemplate(), slot->getStartPos(), slot->getTeamNumber()));
					DEBUG_LOG(("Slot list updated to %s", GenerateGameOptionsString().str()));
				}
			}
		}
	}
}


/*
void LANAPI::OnSlotList( ReturnType ret, LANGameInfo *theGame )
{
	if (!theGame || theGame != m_currentGame)
		return;

	Bool foundMe = false;
	for (int player = 0; player < MAX_SLOTS; ++player)
	{
		if (m_currentGame->getIP(player) == m_localIP)
		{
			foundMe = true;
			break;
		}
	}
	if (!foundMe)
	{
		// I've been kicked - back to the lobby for me!
		// We're counting on the fact that OnPlayerLeave winds up calling reset on TheLAN.
		OnPlayerLeave(m_name);
		return;
	}

	lanUpdateSlotList();
}
*/
void LANAPI::OnPlayerJoin( Int slot, UnicodeString playerName )
{
	if (m_currentGame && m_currentGame->getIP(0) == m_localIP)
	{
		// Someone New Joined.. lets reset the accepts
		m_currentGame->resetAccepted();

		// Send out the game options
		RequestGameOptions(GenerateGameOptionsString(), true);
	}

	lanUpdateSlotList();
}

// GeneralsX: Render a SimID buildTime for a human.
//
// The wire slot carries GitCommitTimeStamp now, and "1785540224" tells a player nothing at all -
// it is a worse answer than the commit count it replaced, unless it is rendered as a date.
//
// strftime into a NARROW buffer and splice with %hs, rather than wcsftime. wchar_t is 16-bit on
// the Windows target and 32-bit on the Apple and Linux ones, so a wide-character time format is
// the sort of thing that compiles everywhere and produces mojibake on one platform. %hs is this
// codebase's established way of putting a char* into a UnicodeString (GameText.cpp:1408).
//
// One side per call, because gmtime() returns a pointer to a single static tm: formatting both
// machines in one call would render the same instant twice.
static UnicodeString simIdBuiltWhen( UnsignedInt unixTime )
{
	UnicodeString out;
	const time_t t = (time_t)unixTime;
	char buf[32];
	const struct tm *g = gmtime( &t );
	if( g != NULL && strftime( buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", g ) != 0 )
		out.format( L"%hs", buf );
	else
		out.format( L"%u", unixTime );   // unrenderable - the raw number beats an empty string
	return out;
}

// GeneralsX: One identity field, marked agreeing or differing.
//
// The dialog used to print the verdict as prose and leave the reader to diff two 8-digit hex
// strings by eye. In the case that actually happens most - engine, data and ordinal all match
// and only `source` moved - that eyeballing IS the diagnosis, and it was being asked of the
// player rather than done for them.
//
// Marked with BOTH a glyph column and a word, never colour. Measured, not assumed: a static
// text gadget renders through one call to DisplayString::draw with a single `textColor`
// (W3DStaticText.cpp:139), so this engine cannot colour part of one control - per-field colour
// would need one gadget per field, and there is no shipped layout with them. A glyph plus a
// word also survives the two ways this text is actually consumed: a screenshot read by somebody
// who cannot separate red from green, and a paste into a chat window as plain text.
//
// The raw hex stays on every row, agreeing rows included. It is what people paste at each
// other, and a row that shows only a verdict cannot be cross-checked against the [SIMID] trace.
enum SimIdRowMark
{
	SIMID_ROW_AGREES = 0,
	SIMID_ROW_DIFFERS,
	SIMID_ROW_SKIPPED    // 0 provenance sentinel on one side: never compared, so never "agrees"
};

// `paddedName` is padded by the caller rather than with a %-8ls width flag: the wide-string
// width flags are the sort of thing that behaves differently under MSVC's swprintf and the
// Apple/Linux one, and this file already ships to both.
static void simIdAppendIdRow( UnicodeString &out, const WideChar *paddedName,
                              SimIdRowMark mark, UnsignedInt mine, UnsignedInt theirs )
{
	// One line per field, both values on it, because the box has a hard line budget: at the
	// smallest supported resolution (800x600, retail MessageBox.wnd) the message control can
	// grow to about 460px, which is roughly 28 lines of Arial 10. A three-line-per-field form
	// spent twelve of those on the four-fields-differ case alone. The header states the order
	// once - "yours vs theirs" - so the row does not have to.
	UnicodeString row;
	switch( mark )
	{
		case SIMID_ROW_AGREES:
			row.format( L"%ls =  agrees   %08X\n", paddedName, mine );
			break;
		case SIMID_ROW_SKIPPED:
			row.format( L"%ls ?  not compared  %08X vs %08X\n", paddedName, mine, theirs );
			break;
		default:
			row.format( L"%ls X  DIFFERS  %08X vs %08X\n", paddedName, mine, theirs );
			break;
	}
	out.concat( row );
}

void LANAPI::OnGameJoin( ReturnType ret, LANGameInfo *theGame )
{
	if (ret == RET_OK)
	{
		LANbuttonPushed = true;
		TheShell->push( "Menus/LanGameOptionsMenu.wnd" );
		//lanUpdateSlotList();

		LANPreferences pref;
		AsciiString options;
		options.format("PlayerTemplate=%d", pref.getPreferredFaction());
		RequestGameOptions(options, true);
		options.format("Color=%d", pref.getPreferredColor());
		RequestGameOptions(options, true);
		options.format("User=%s", m_userName.str());
		RequestGameOptions( options, true );
		options.format("Host=%s", m_hostName.str());
		RequestGameOptions( options, true );
		options.format("NAT=%d", FirewallHelperClass::FIREWALL_TYPE_SIMPLE); // BGC: This is a LAN game, so there is no firewall.
		RequestGameOptions( options, true );
	}
	else if (ret != RET_BUSY)
	{
		/// @todo: re-enable lobby controls?  Error msgs?
		UnicodeString title, body;
		title = TheGameText->FETCH_OR_SUBSTITUTE("LAN:JoinFailed", L"Could not join the game");

		// GeneralsX: Name the peer, do not just say "the other machine".
		//
		// Every string below said "the other machine", which is fine with two boxes on a desk and
		// useless in a four-platform lobby - the reader is told a build is wrong and not which one
		// to go and rebuild. On a join failure the machine in question is unambiguous: it is the
		// host of the game being dialled, which is slot 0 of theGame. Prefixed rather than
		// substituted into each string so the existing CSF labels keep working untouched and a
		// missing name simply degrades to the old wording.
		UnicodeString whoPrefix;
		if( theGame != nullptr )
		{
			const GameSlot *hostSlot = theGame->getConstSlot( 0 );
			if( hostSlot != nullptr && !hostSlot->getName().isEmpty() )
				whoPrefix.format( L"Host \"%s\": ", hostSlot->getName().str() );
		}

		// GeneralsX: SimID join-failure diagnostics.
		// getErrorStringFromReturnType is deliberately NOT changed. Its RET_CRC_MISMATCH
		// branch fetches LAN:ErrorCRCMismatch, whose retail text is a mid-game desync notice
		// that tells the reader their opponent may have cheated and points them at the Ignore
		// List - the worst possible copy for "these two builds are not the same build".
		// Every string below ships an English substitute via FETCH_OR_SUBSTITUTE, so the
		// feature needs no modified CSF and nothing can render as "MISSING: 'label'".
		if (ret == RET_CRC_MISMATCH)
		{
			const SimulationId &me = SimulationId::get();
			switch (m_remoteSimVerdict)
			{
			case SIMID_LOCAL_INVALID:
				body = TheGameText->FETCH_OR_SUBSTITUTE("LAN:SimIdLocalInvalid",
					L"This copy of the game did not finish loading its data and cannot join or host a network game. Restart the game.");
				break;

			case SIMID_PEER_SILENT:
				// No build times here: on a pre-SimID peer that field is whatever
				// happened to be in the sender's uninitialised stack, so quoting it would
				// be worse than saying nothing.
				body = TheGameText->FETCH_OR_SUBSTITUTE("LAN:SimIdPeerSilent",
					L"Version mismatch, not a network problem. The other machine is running an older build of GeneralsX that cannot report its build identity. Update both machines to the same build.");
				break;

			case SIMID_ENGINE_DIFFERS:
			case SIMID_SOURCE_DIFFERS:
			case SIMID_SOURCE_UNKNOWN:
			case SIMID_DATA_DIFFERS:
			case SIMID_ORDINAL_DIFFERS:
			{
				// PRESENTATION ONLY. Nothing in this block decides anything: the refusal was
				// taken upstream by SimIdCompare and is already final by the time OnGameJoin is
				// called with RET_CRC_MISMATCH. Every value read here is read to be *printed*.
				//
				// GeneralsX: The build time used to be GitRevision, a
				// commit count, printed as "build %d". Both halves were wrong: the value could
				// name the WRONG machine (see SimulationId.h for the three measured ways a commit
				// count fails to order two builds), and a bare number was never useful to a
				// player. Every CSF label in this block is NEW rather than reworded in place -
				// fetchOrSubstituteFormatVA formats the FETCHED string when a label exists, so a
				// translation written against an older format string would read the new arguments
				// through the old specifiers. No CSF in this tree defines any LAN:SimId* label
				// today, so new names cost nothing and close that off permanently.
				//
				// GeneralsX: 0 is the "built without git" sentinel, not a
				// time. Feeding it to simIdBuiltWhen renders "1970-01-01 00:00:00 UTC", which reads
				// as a real - and absurd - build date rather than as missing information, and the
				// row directly above already says the age is unknown.
				const UnicodeString mineWhen   = ( me.buildTime != 0 )
					? simIdBuiltWhen( me.buildTime )
					: TheGameText->FETCH_OR_SUBSTITUTE("LAN:SimIdBuiltNoGit", L"unknown (built without git)");
				const UnicodeString theirsWhen = ( m_remoteSim.buildTime != 0 )
					? simIdBuiltWhen( m_remoteSim.buildTime )
					: TheGameText->FETCH_OR_SUBSTITUTE("LAN:SimIdBuiltNoGit", L"unknown (built without git)");

				// GeneralsX: Report every identity field, not just the
				// first one that tripped.
				//
				// The old text quoted the verdict's own field and nothing else, so the reader was
				// handed one pair of hex strings and no way to tell whether the rest of the build
				// agreed. The screenshot that prompted this had engine, data AND ordinal all
				// matching and only `source` moved - which is the "just rebuild, the data is
				// fine" case - and none of that was on screen.
				//
				// This reads the SAME fields SimIdCompare reads and in the SAME precedence order,
				// but it does not decide anything: the refusal already happened, upstream, on the
				// verdict. Comparing all four here rather than trusting the verdict alone is
				// strictly more information - SimIdCompare returns the FIRST field that differs
				// and stops, so later fields may differ too and the verdict cannot say.
				const Bool engineDiffers  = (me.engineID  != m_remoteSim.engineID);
				const Bool dataDiffers    = (me.dataID    != m_remoteSim.dataID);
				const Bool ordinalDiffers = (me.ordinalID != m_remoteSim.ordinalID);
				// Mirrors SimIdCompare's carve-out exactly: 0 is the reserved "built without
				// git" sentinel and is never comparable, so this field is reported as skipped
				// rather than as agreeing. Calling it "agrees" would be a false all-clear on the
				// one field a degraded detector cannot speak for.
				const Bool sourceComparable = (me.sourceID != 0u && m_remoteSim.sourceID != 0u);
				const Bool sourceDiffers    = sourceComparable && (me.sourceID != m_remoteSim.sourceID);

				// GeneralsX: An unknown provenance is not an established mismatch:
				// say what it is.
				body = (m_remoteSimVerdict == SIMID_SOURCE_UNKNOWN)
					? TheGameText->FETCH_OR_SUBSTITUTE("LAN:SimIdNoGitHead",
						L"Unknown build provenance, not a network problem: one side was built without git. Field by field, yours vs theirs:\n\n")
					: TheGameText->FETCH_OR_SUBSTITUTE("LAN:SimIdMismatchHead",
						L"Build mismatch, not a network problem. Field by field, yours vs theirs:\n\n");

				simIdAppendIdRow( body, L"engine ",
					engineDiffers ? SIMID_ROW_DIFFERS : SIMID_ROW_AGREES,
					me.engineID, m_remoteSim.engineID );
				simIdAppendIdRow( body, L"source ",
					!sourceComparable ? SIMID_ROW_SKIPPED
					                  : (sourceDiffers ? SIMID_ROW_DIFFERS : SIMID_ROW_AGREES),
					me.sourceID, m_remoteSim.sourceID );
				simIdAppendIdRow( body, L"data   ",
					dataDiffers ? SIMID_ROW_DIFFERS : SIMID_ROW_AGREES,
					me.dataID, m_remoteSim.dataID );
				simIdAppendIdRow( body, L"ordinal",
					ordinalDiffers ? SIMID_ROW_DIFFERS : SIMID_ROW_AGREES,
					me.ordinalID, m_remoteSim.ordinalID );

				// The build time gets a NEUTRAL marker, never the DIFFERS glyph. It is not a
				// compatibility field - SimulationId.h is explicit that it is never compared for
				// accept/deny - and marking it like one would tell a reader that two builds made
				// at different moments cannot play, which is false.
				//
				// A COMMIT DATE IS NOT A RELEASE ORDINAL, and the wording stays weaker than
				// "must update" because of it. A newer commit on an older branch, a rollback or a
				// backdated commit all make the later timestamp the wrong answer - this
				// repository has three coexisting lineages, so that is a live case, not a
				// theoretical one. State the fact and show both dates so the reader can judge.
				//
				// 0 is the real "no git" sentinel on both sides - git_watcher.cmake emits 0 for
				// GIT_COMMIT_TSTAMP exactly as it did for GIT_REV_LIST_COUNT.
				const Bool ageUnknown = (m_remoteSim.buildTime == 0 || me.buildTime == 0);
				const Bool theyAreOlder = (!ageUnknown && me.buildTime > m_remoteSim.buildTime);
				const Bool weAreOlder   = (!ageUnknown && me.buildTime < m_remoteSim.buildTime);

				UnicodeString builtRow;
				if (ageUnknown)
					builtRow = TheGameText->FETCH_OR_SUBSTITUTE("LAN:SimIdAgeUnknown",
						L"built   -  age unknown, one build has no git info\n");
				else if (theyAreOlder)
					builtRow = TheGameText->FETCH_OR_SUBSTITUTE("LAN:SimIdAgeTheirs",
						L"built   -  THEIRS is older\n");
				else if (weAreOlder)
					builtRow = TheGameText->FETCH_OR_SUBSTITUTE("LAN:SimIdAgeMine",
						L"built   -  THIS device is older\n");
				else
					builtRow = TheGameText->FETCH_OR_SUBSTITUTE("LAN:SimIdAgeEqual",
						L"built   -  same commit date, so one side has local changes\n");
				body.concat(builtRow);

				UnicodeString whenRows;
				whenRows.format(L"     yours  %ls\n     theirs %ls\n\n",
					mineWhen.str(), theirsWhen.str());
				body.concat(whenRows);

				// The one-line answer, so nobody has to count the rows. Named separately from
				// the rows above because this is the sentence a player will repeat to a friend.
				//
				// The denominator is how many fields were actually COMPARED, not how many exist.
				// When one side has no git provenance, `source` is skipped and may well differ -
				// so "Only engine differs" would contradict the "source ? not compared" row three
				// lines above it and claim more than was established.
				const Int tier1Differs = (engineDiffers  ? 1 : 0) + (sourceDiffers  ? 1 : 0)
				                       + (dataDiffers    ? 1 : 0) + (ordinalDiffers ? 1 : 0);
				const Int tier1Compared = sourceComparable ? 4 : 3;
				UnicodeString summary;
				if (tier1Differs == 1)
				{
					const WideChar *only = engineDiffers ? L"engine"
					                     : sourceDiffers ? L"source"
					                     : dataDiffers   ? L"data" : L"ordinal";
					if (sourceComparable)
						summary = TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("LAN:SimIdOnlyOne",
							L"Only %ls differs. ", only);
					else
						summary = TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("LAN:SimIdOnlyOneOfSome",
							L"Of the %d fields that could be compared, only %ls differs. ",
							tier1Compared, only);
				}
				else if (tier1Differs > 1)
				{
					summary = TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("LAN:SimIdSeveral",
						L"%d of the %d identity fields compared differ. ",
						tier1Differs, tier1Compared);
				}
				else
				{
					// Unreachable as the code stands, and deliberately not asserted. Reaching this
					// case means the four comparisons above disagree with the verdict that got us
					// into this branch - and they cannot, because m_remoteSimVerdict is
					// SimIdCompare(m_remoteSim) over the very same snapshot (LANAPIhandlers.cpp:823
					// and :996), using the same rules including the sourceID != 0 guard. It exists
					// because the comparison is now written in two places, and the day someone
					// changes one of them this dialog must degrade to a sentence rather than to
					// "0 of the 4 identity fields differ".
					summary = (m_remoteSimVerdict == SIMID_SOURCE_UNKNOWN)
						? TheGameText->FETCH_OR_SUBSTITUTE("LAN:SimIdNoneDifferNoGit",
							L"Every field that can be compared matches; the source cannot be compared. ")
						: TheGameText->FETCH_OR_SUBSTITUTE("LAN:SimIdNoneVisible",
							L"The refused field cannot be compared from here. ");
				}
				body.concat(summary);

				// What differs, and what to do about THAT.
				//
				// GeneralsX: Two things were wrong here, and both sent
				// the player to work on the wrong thing.
				//
				// The FIELD: this used to switch on m_remoteSimVerdict, which is only the FIRST
				// mismatch - SimIdCompare stops at it. A source-plus-data mismatch arrives as
				// SIMID_SOURCE_DIFFERS, so the reader was told to rebuild and nothing more; they
				// rebuild, the data mismatch survives, and the next join fails identically. The
				// remediation is now composed from the booleans, so every differing field gets
				// its own instruction.
				//
				// The CLOCK: "rebuild the older machine" used to be appended to every verdict. It
				// answers the question for exactly one field:
				//
				//   * sourceID IS the source tree, so "the older one moved" is the diagnosis.
				//   * dataID mixes m_iniCRC with the loose simulation-file digest. A mod or an
				//     edited INI on the NEWER machine trips it just as readily, and rebuilding
				//     the older one changes neither side's data.
				//   * engineID is the build CONFIGURATION. Two builds off the same commit with
				//     different cmake options differ here, so "the older one" may not exist at
				//     all - and rebuilding it with the options it already had is a no-op.
				//   * ordinalID is fnv1a over TheNameKeyGenerator's counter either side of the
				//     science-store init (SimulationId.cpp:638). It counts the NAMES registered
				//     at startup, so a difference here while engine, source and data all agree
				//     means one machine loaded content the data digest does not cover - a
				//     different set of archives. Age says nothing about that.
				UnicodeString ageAdvice;
				if (theyAreOlder)
					ageAdvice = TheGameText->FETCH_OR_SUBSTITUTE("LAN:SimIdFixTheirs",
						L"Rebuild the older machine, which is theirs.");
				else if (weAreOlder)
					ageAdvice = TheGameText->FETCH_OR_SUBSTITUTE("LAN:SimIdFixMine",
						L"Rebuild the older machine, which is this one.");
				else
					ageAdvice = TheGameText->FETCH_OR_SUBSTITUTE("LAN:SimIdFixBoth",
						L"Rebuild both machines from the same commit.");

				// GeneralsX: M1 Determinism-1: an unknown source is refused on its own
				// (SimIdCompare, SIMID_SOURCE_UNKNOWN), so say so whenever it holds - even when another field was the first
				// mismatch, or aligning that field would only meet this refusal next.
				if (!sourceComparable)
					body.concat(TheGameText->FETCH_OR_SUBSTITUTE("LAN:SimIdWhySourceUnknown",
						L"One of the two machines runs a build made without its source history (no git), so "
						L"nothing can tell whether it simulates the same game. Such a build cannot play online: "
						L"use an official build. "));
				if (tier1Differs >= 3)
				{
					// Three or four differing is not four separate problems, it is one: these are
					// not the same install. Saying it once is both shorter and truer than reciting
					// per-field advice - and the line budget is real, see simIdAppendIdRow.
					//
					// "with the same build options" is load-bearing and not padding: engineID can
					// differ on cmake options alone, so a reader who rebuilds both machines from
					// one commit and changes nothing else is refused again, identically.
					body.concat(TheGameText->FETCH_OR_SUBSTITUTE("LAN:SimIdWhyEverything",
						L"Nearly nothing matches, so these are not the same install. Rebuild both "
						L"machines from the same commit with the same build options, and give them "
						L"the same game folder."));
				}
				else
				{
					if (engineDiffers)
						body.concat(TheGameText->FETCH_OR_SUBSTITUTE("LAN:SimIdWhyEngine",
							L"The build configuration is not the same: both machines must be built "
							L"with the same options, whichever is newer. "));
					if (sourceDiffers)
					{
						body.concat(TheGameText->FETCH_OR_SUBSTITUTE("LAN:SimIdWhySource",
							L"The engine source is not the same. "));
						body.concat(ageAdvice);
						body.concat(L" ");
					}
					if (dataDiffers)
						body.concat(TheGameText->FETCH_OR_SUBSTITUTE("LAN:SimIdWhyData",
							L"The game data is not the same - a different INI or a mod on one side. "
							L"Rebuilding will not align it: both machines need the same mods and "
							L"the same files under Data. "));
					if (ordinalDiffers)
					{
						// GeneralsX: Do not blame the archives unless the
						// ARCHIVE FINGERPRINT says so. ordinalID counts registered names; assetID
						// is the mounted-archive fingerprint (SimulationId.cpp:640). A different
						// archive set is only ONE way to move the name count - so when sourceID
						// could not be compared, two different codebases move it just as easily
						// with byte-identical archives, and "your .big files differ" would send
						// the reader to check something that is already correct.
						if (me.assetID != m_remoteSim.assetID)
							body.concat(TheGameText->FETCH_OR_SUBSTITUTE("LAN:SimIdWhyOrdinalAssets",
								L"The internal name IDs do not match, and the two machines have "
								L"different game archives. Make the set of .big files in the game "
								L"folder the same on both. "));
						else if (!sourceComparable)
							body.concat(TheGameText->FETCH_OR_SUBSTITUTE("LAN:SimIdWhyOrdinalNoSource",
								L"The internal name IDs do not match. The archives are the same and "
								L"the engine source could not be compared, so start by making both "
								L"machines the same build. "));
						else
							body.concat(TheGameText->FETCH_OR_SUBSTITUTE("LAN:SimIdWhyOrdinalUnknown",
								L"The internal name IDs do not match even though the code, the data "
								L"and the archives all agree. Send both machines' startup [SIMID] "
								L"log lines to whoever maintains this build. "));
					}

					// Belt and braces for the unreachable case described at the summary above:
					// never leave the dialog with a diagnosis and no instruction. GeneralsX: Not
					// for an unknown source alone: that refusal now reaches here with nothing
					// else differing, its instruction is already above ("use an official build"), and build age would
					// point at the wrong machine when the no-git build is the newer one.
					if (tier1Differs == 0 && sourceComparable)
						body.concat(ageAdvice);
				}
				break;
			}

			default:
				// Belt and braces: no verdict value, present or future, may ever produce an
				// empty dialog. SIMID_OK lands here too - it means the mismatch came from
				// the retail CRC path rather than from SimID.
				body = getErrorStringFromReturnType(ret);
				break;
			}
		}
		else if (ret == RET_TIMEOUT)
		{
			// handleRequestJoin returns silently on any request whose GameToJoin.gameIP does
			// not match, so a genuinely incompatible peer very often presents as a timeout
			// rather than as an explicit refusal.
			body = getErrorStringFromReturnType(ret);
			body.concat(TheGameText->FETCH_OR_SUBSTITUTE("LAN:SimIdTimeoutHint",
				L" If the host is running a different build of GeneralsX it may be ignoring this request entirely."));
		}
		else
		{
			body = getErrorStringFromReturnType(ret);
		}

		// Only the identity verdicts name a machine; a room-full or timeout is not somebody's
		// fault and prefixing those with a name would read as an accusation.
		if( !whoPrefix.isEmpty() && ret == RET_CRC_MISMATCH )
		{
			UnicodeString named = whoPrefix;
			named.concat( body );
			body = named;
		}

		MessageBoxOk(title, body, nullptr);
	}
}

void LANAPI::OnHostLeave()
{
	DEBUG_ASSERTCRASH(!m_inLobby && m_currentGame, ("Game info is gone!"));
	if (m_inLobby || !m_currentGame)
		return;
	LANbuttonPushed = true;
	DEBUG_LOG(("Host left - popping to lobby"));
	TheShell->pop();
}

void LANAPI::OnPlayerLeave( UnicodeString player )
{
	DEBUG_ASSERTCRASH(!m_inLobby && m_currentGame, ("Game info is gone!"));
	if (m_inLobby || !m_currentGame || m_currentGame->isGameInProgress())
		return;

	if (m_name.compare(player) == 0)
	{
		// We're leaving.  Save options and Pop the shell up a screen.
		//DEBUG_CRASH(("Slot is %d", m_currentGame->getLocalSlotNum()));
		if (m_currentGame && m_currentGame->isInGame() && m_currentGame->getLocalSlotNum() >= 0)
		{
			LANPreferences pref;
			AsciiString option;
			option.format("%d", m_currentGame->getLANSlot( m_currentGame->getLocalSlotNum() )->getPlayerTemplate());
			pref["PlayerTemplate"] = option;
			option.format("%d", m_currentGame->getLANSlot( m_currentGame->getLocalSlotNum() )->getColor());
			pref["Color"] = option;
			if (m_currentGame->amIHost())
				pref["Map"] = AsciiStringToQuotedPrintable(m_currentGame->getMap());
			pref.write();
		}
		LANbuttonPushed = true;
		DEBUG_LOG(("OnPlayerLeave says we're leaving!  pop away!"));
		TheShell->pop();
	}
	else
	{
		if (m_currentGame && m_currentGame->getIP(0) == m_localIP)
		{
			// Force a new slotlist send
			m_lastResendTime = 0;

			lanUpdateSlotList();
			RequestGameOptions( GenerateGameOptionsString(), true );

		}
	}
}

void LANAPI::OnGameList( LANGameInfo *gameList )
{

	if (m_inLobby)
	{
		LANDisplayGameList(listboxGames, gameList);
	}
}

void LANAPI::OnGameCreate( ReturnType ret )
{
	if (ret == RET_OK)
	{

		LANbuttonPushed = true;
		TheShell->push( "Menus/LanGameOptionsMenu.wnd" );

		RequestLobbyLeave( false );
		//RequestGameAnnounce(); // can't do this here, since we don't have a map set
	}
	else
	{
		if(m_inLobby)
		{
			switch( ret )
			{
			case RET_GAME_EXISTS:
				GadgetListBoxAddEntryText(listboxChatWindow, TheGameText->fetch("LAN:ErrorGameExists"), chatSystemColor, -1, -1);
				break;
			case RET_BUSY:
				GadgetListBoxAddEntryText(listboxChatWindow, TheGameText->fetch("LAN:ErrorBusy"), chatSystemColor, -1, -1);
				break;
			// GeneralsX: RequestGameCreate's SimID pre-flight guard
			// reports RET_CRC_MISMATCH. Without this case it falls through to default: and
			// prints LAN:ErrorUnknown, which tells the user nothing actionable. Hosting is
			// always initiated from the LAN lobby, so listboxChatWindow is live here - the
			// enclosing if(m_inLobby) has already established that.
			case RET_CRC_MISMATCH:
				// GeneralsX: A box, not only a lobby line, for the no-git refusal:
				// Direct Connect and the Online host popup have no listboxChatWindow, and a line into a
				// null listbox is dropped - the Host click would look like it did nothing.
				if (m_remoteSimVerdict == SIMID_SOURCE_UNKNOWN)
					MessageBoxOk(TheGameText->FETCH_OR_SUBSTITUTE("LAN:SimIdNoGitHostTitle", L"Cannot host"),
						TheGameText->FETCH_OR_SUBSTITUTE("LAN:SimIdNoGitHost",
							L"This build was made without its source history (no git), so no other copy can tell whether it simulates the same game. It cannot host or join a network game: use an official build."),
						nullptr);
				else
					GadgetListBoxAddEntryText(listboxChatWindow,
						TheGameText->FETCH_OR_SUBSTITUTE("LAN:SimIdLocalInvalid",
							L"This copy of the game did not finish loading its data and cannot join or host a network game. Restart the game."),
						chatSystemColor, -1, -1);
				break;
			default:
				GadgetListBoxAddEntryText(listboxChatWindow, TheGameText->fetch("LAN:ErrorUnknown"), chatSystemColor, -1, -1);
			}
		}
	}

}

void LANAPI::OnPlayerList( LANPlayer *playerList )
{
	if (m_inLobby)
	{

		UnsignedInt selectedIP = 0;
		Int selectedIndex = -1;
		Int indexToSelect = -1;
		GadgetListBoxGetSelected(listboxPlayers, &selectedIndex);

		if (selectedIndex != -1 )
			// GeneralsX: Use uintptr_t for 64-bit safe pointer cast
			selectedIP = (uintptr_t) GadgetListBoxGetItemData(listboxPlayers, selectedIndex, 0);

		GadgetListBoxReset(listboxPlayers);

		LANPlayer *player = m_lobbyPlayers;
		while (player)
		{
			Int addedIndex = GadgetListBoxAddEntryText(listboxPlayers, player->getName(), playerColor, -1, -1);
			GadgetListBoxSetItemData(listboxPlayers, reinterpret_cast<void*>(std::uintptr_t(player->getIP())),addedIndex, 0 );

			if (selectedIP == player->getIP())
				indexToSelect = addedIndex;

			player = player->getNext();
		}

		if (indexToSelect >= 0)
			GadgetListBoxSetSelected(listboxPlayers, indexToSelect);
	}
}

void LANAPI::OnNameChange( UnsignedInt IP, UnicodeString newName )
{
	OnPlayerList(m_lobbyPlayers);
}

void LANAPI::OnInActive(UnsignedInt IP) {

}

// GeneralsX @feature Chat text drawn in a player's own lobby colour is unreadable for the
// darker half of the multiplayer palette (red, blue, purple) on the near-black chat listbox.
// Lift anything below a luma floor by blending it toward white: luma is linear in the
// components, so the exact blend factor can be solved for instead of iterating, and blending
// toward white keeps the hue recognisable instead of washing the colour out to grey.
static Color MakeChatColorReadable( Color color )
{
	const Int minLuma = 128;

	UnsignedByte red, green, blue, alpha;
	GameGetColorComponents(color, &red, &green, &blue, &alpha);

	// Rec.601 weights scaled to 256 so this stays integer math.
	const Int luma = (77 * red + 150 * green + 29 * blue) >> 8;
	if (luma >= minLuma)
		return color;

	const Real blend = (Real)(minLuma - luma) / (Real)(255 - luma);
	red   = (UnsignedByte)(red   + (255 - red)   * blend);
	green = (UnsignedByte)(green + (255 - green) * blend);
	blue  = (UnsignedByte)(blue  + (255 - blue)  * blend);

	return GameMakeColor(red, green, blue, alpha);
}

// GeneralsX @feature Resolve the lobby colour a chat line should be drawn in. Anyone who holds
// no slot (system lines, players still in the LAN lobby), anyone still set to a random colour
// and observers keep the caller's classic chat colour, matching the "???" / "None" swatch the
// slot list shows for them.
static Color GetPlayerChatColor( LANGameInfo *game, UnicodeString playerName, Color defaultColor )
{
	if (game == nullptr || !game->isInGame() || TheMultiplayerSettings == nullptr)
		return defaultColor;

	const Int slotNum = game->getSlotNum(playerName);
	if (slotNum < 0)
		return defaultColor;

	const GameSlot *slot = game->getConstSlot(slotNum);
	if (slot == nullptr || slot->getColor() < 0 || slot->getPlayerTemplate() == PLAYERTEMPLATE_OBSERVER)
		return defaultColor;

	const MultiplayerColorDefinition *def = TheMultiplayerSettings->getColor(slot->getColor());
	if (def == nullptr)
		return defaultColor;

	return MakeChatColorReadable(def->getColor());
}

void LANAPI::OnChat( UnicodeString player, UnsignedInt ip, UnicodeString message, ChatType format )
{
	GameWindow *chatWindow = nullptr;

	if (m_inLobby)
	{
		chatWindow = listboxChatWindow;
	}
	else if( m_currentGame && m_currentGame->isGameInProgress() && TheShell->isShellActive())
	{
		chatWindow = listboxChatWindowScoreScreen;
	}
	else if( m_currentGame && !m_currentGame->isGameInProgress())
	{
		chatWindow = listboxChatWindowLanGame;
	}
	if (chatWindow == nullptr)
		return;

	// GeneralsX @feature Slot colours only exist once we are in a game setup; chat in the LAN
	// lobby itself keeps the classic local/remote colouring.
	LANGameInfo *slotColorGame = (m_inLobby) ? nullptr : m_currentGame;

	Int index = -1;
	UnicodeString unicodeChat;
	switch (format)
	{
		case LANAPIInterface::LANCHAT_SYSTEM:
			unicodeChat = message;
			index =GadgetListBoxAddEntryText(chatWindow, unicodeChat, chatSystemColor, -1, -1);
			break;
		case LANAPIInterface::LANCHAT_LOG:
			unicodeChat = L"[LOG] ";
			unicodeChat.concat(message);
			index = GadgetListBoxAddEntryText(chatWindow, unicodeChat, chatLogColor, -1, -1);
			break;
		case LANAPIInterface::LANCHAT_EMOTE:
		{
			unicodeChat = player;
			unicodeChat.concat(L' ');
			unicodeChat.concat(message);
			const Color defaultColor = (ip == m_localIP) ? chatLocalActionColor : chatActionColor;
			index =GadgetListBoxAddEntryText(chatWindow, unicodeChat, GetPlayerChatColor(slotColorGame, player, defaultColor), -1, -1);
			break;
		}
		case LANAPIInterface::LANCHAT_NORMAL:
		default:
		{
			// Do the language filtering.
			TheLanguageFilter->filterLine(message);

			const Color defaultColor = (ip == m_localIP) ? chatLocalNormalColor : chatNormalColor;

			unicodeChat = L"[";
			unicodeChat.concat(player);
			unicodeChat.concat(L"] ");
			unicodeChat.concat(message);
			index =GadgetListBoxAddEntryText(chatWindow, unicodeChat, GetPlayerChatColor(slotColorGame, player, defaultColor), -1, -1);
			break;
		}
	}
	GadgetListBoxSetItemData(chatWindow, (void *)-1, index);
}

// GeneralsX: The deferred tier2 SimID warnings that OnChat above
// cannot print at discovery time (listboxChatWindowLanGame does not exist until Shell::update()
// instantiates the layout) are emitted by LANAPI::flushPendingSimWarnings, which lives in
// LANAPIhandlers.cpp next to PostSimWarningsToChat. Keeping it there is deliberate: the host
// prints the same four warnings inline from handleRequestJoin, and one shared emitter is the
// only way the two wordings cannot drift apart.
