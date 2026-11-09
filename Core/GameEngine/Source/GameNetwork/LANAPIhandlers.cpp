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
// FILE: LANAPIHandlers.cpp
// Author: Matthew D. Campbell, October 2001
// Description: LAN callback handlers
///////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/crc.h"
#include "Common/GameState.h"
#include "Common/Registry.h"
#include "Common/GlobalData.h"
#include "Common/QuotedPrintable.h"
#include "Common/UserPreferences.h"
#include "GameNetwork/LANAPI.h"
#include "Common/Diagnostic/SimulationId.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameClient/MapUtil.h"
// GeneralsX @feature Claude 24/08/2026 TheGameText, for the host-camera lobby notice below.
// FETCH_OR_SUBSTITUTE, so the line ships as CODE ONLY - a new .csf would mean a new .big and a
// new .big moves assetID, which turns a one-line chat message into a data rollout.
#include "GameClient/GameText.h"
// GeneralsX @feature Claude 24/08/2026 Host-authoritative camera height limits. This header
// resolves to GameClient/GXHostCamera.h when feat/hostcam-apply is present and to a set of
// inert no-ops when it is not, so the wire half builds and links on its own. See GXHostCamSeam.h.
#include "GameNetwork/GXHostCamSeam.h"

// GeneralsX @build BenderAI 13/02/2026 WideCharWindows conversion helpers (fighter19 pattern)
// These functions handle conversion between WideChar (wchar_t) and WideCharWindows (uint16_t)
// Required because wchar_t size varies (2 bytes Windows, 4 bytes Linux) but network protocol needs fixed size

void CopyWcharToWindowsWideChar( WideCharWindows *dest, const WideChar *src, UnsignedInt len )
{
	// GeneralsX @bugfix The copy loop used to run exactly `len` iterations no matter how long `src`
	// actually was. Every caller passes ARRAY_SIZE(dest)-1, i.e. the *destination* capacity, so a
	// short source was over-read by up to len-1 elements: UnicodeString::str() hands back a pointer
	// to the single static &TheNullChr when the string is empty, and one caller passes a bare L""
	// literal, so the 100-element chat field read ~400 bytes off the end of a 4-byte object. The low
	// 16 bits of each stray word then went out on a UDP broadcast to the whole subnet. Stop at the
	// source terminator, and zero the remainder of the field so no residue is put on the wire
	// either. This writes dest[0..len] - exactly the range the old code wrote - so it stays in
	// bounds at every call site.
	UnsignedInt i = 0;
	while (i < len && src[i] != 0)
	{
		dest[i] = src[i];
		++i;
	}
	while (i <= len)
	{
		dest[i] = 0;
		++i;
	}
}

// GeneralsX @bugfix Claude 12/08/2026 WindowsWideCharToWchar and the field-size-deduced
// GetWindowsWideCharFieldAsWchar now live in LANAPI.h, so the SAFE reader is the exported one.
// The unbounded pointer form that used to be declared there is deleted; it had no call sites.

void LANAPI::handleRequestLocations( LANMessage *msg, UnsignedInt senderIP )
{
	if (m_inLobby)
	{
		LANMessage reply;
		fillInLANMessage( &reply );
		reply.messageType = LANMessage::MSG_LOBBY_ANNOUNCE;

		sendMessage(&reply);
		m_lastResendTime = timeGetTime();
	}
	else
	{
		// In game - are we a game host?
		if (m_currentGame)
		{
			if (m_currentGame->getIP(0) == m_localIP)
			{
				// TheSuperHackers @bugfix bobtista 23/08/2026 Bail on an empty options string rather
				// than announce a payload no receiver can parse.
				AsciiString gameOpts = GenerateGameOptionsString();
				if (!gameOpts.isEmpty())
				{
					LANMessage reply;
					fillInLANMessage( &reply );
					reply.messageType = LANMessage::MSG_GAME_ANNOUNCE;
					strlcpy(reply.GameInfo.options, gameOpts.str(), ARRAY_SIZE(reply.GameInfo.options));
					// GeneralsX @bugfix BenderAI 13/02/2026 Use CopyWcharToWindowsWideChar (fighter19 pattern)
					CopyWcharToWindowsWideChar(reply.GameInfo.gameName, m_currentGame->getName().str(), ARRAY_SIZE(reply.GameInfo.gameName) - 1);
					reply.GameInfo.inProgress = m_currentGame->isGameInProgress();
					reply.GameInfo.isDirectConnect = m_currentGame->getIsDirectConnect();

					sendMessage(&reply);
				}
			}
			else
			{
				// We're a joiner
			}
		}
	}
	// Add the player to the lobby player list
	LANPlayer *player = LookupPlayer(senderIP);
	if (!player)
	{
		player = NEW LANPlayer;
		player->setIP(senderIP);
	}
	else
	{
		removePlayer(player);
	}
	// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
	player->setName(UnicodeString(GetWindowsWideCharFieldAsWchar(msg->name)));
	player->setHost(msg->hostName);
	player->setLogin(msg->userName);
	player->setLastHeard(timeGetTime());

	addPlayer(player);

	OnNameChange(player->getIP(), player->getName());
}

void LANAPI::handleGameAnnounce( LANMessage *msg, UnsignedInt senderIP )
{
	if (senderIP == m_localIP)
	{
		return; // Don't try to update own info
	}
	else if (m_currentGame && m_currentGame->isGameInProgress())
	{
		return; // Don't care about games if we're playing
	}
	else if (senderIP == m_directConnectRemoteIP)
	{

		if (m_currentGame == nullptr)
		{
			// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
			LANGameInfo *game = LookupGame(UnicodeString(GetWindowsWideCharFieldAsWchar(msg->GameInfo.gameName)));
			if (!game)
			{
				game = NEW LANGameInfo;
				// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
				game->setName(UnicodeString(GetWindowsWideCharFieldAsWchar(msg->GameInfo.gameName)));
				addGame(game);
			}
			Bool success = ParseGameOptionsString(game,AsciiString(msg->GameInfo.options));
			// GeneralsX @bugfix Claude 06/10/2026 Flags only from a parsed announce (as the other branch). The address we
			// dialled is otherwise trusted for its game as before - through NAT its slot 0 is a private address.
			if (success)
			{
				game->setGameInProgress(msg->GameInfo.inProgress);
				game->setIsDirectConnect(msg->GameInfo.isDirectConnect);
				game->setLastHeard(timeGetTime());
			}
			if (!success)
			{
				// remove from list
				removeGame(game);
				delete game;
				return;
			}
			// GeneralsX @bugfix Claude 11/08/2026 Only the FIRST announce advances the handshake.
			// m_currentGame stays null until the accept arrives, so this branch is re-entered by
			// every further announce from the host - including its periodic 10 s re-announce and
			// the replies to the reasserted game-info request above. Each used to call
			// RequestGameJoin again, whose ACT_JOIN guard answers OnGameJoin(RET_BUSY) - aborting
			// a join that was proceeding normally. Bookkeeping above still runs; the join is
			// kicked off exactly once.
			if (m_pendingAction == ACT_JOINDIRECTCONNECT)
				RequestGameJoin(game, m_directConnectRemoteIP);
		}
	}
	else
	{
		// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
		LANGameInfo *game = LookupGame(UnicodeString(GetWindowsWideCharFieldAsWchar(msg->GameInfo.gameName)));
		// GeneralsX @security Claude 06/10/2026 An announce speaks only for its sender's own game (sweep v-net-03).
		// The lookup is by NAME, and the game we are in - as host or joiner - is in m_games too, so any peer could
		// rewrite it (slots, IPs, map, seed) by announcing the same name, or free it with one that failed to parse.
		// The game we are in, and the row of a game we are joining, answer only to their slot-0 address - who the lobby
		// already takes as the host (handleGameStart, OnGameOptions require getIP(0) == senderIP). Any other row is just
		// a name, and names are not owned (base behaviour: the next announce wins), but an announce is taken only if it
		// is self-consistent: its slot 0 is its sender, or it says in progress and its sender plays in it (the packet
		// router re-announcing a running match whose host left). That keeps a peer from writing rows that point at a
		// third party, and keeps a squatter from locking a host out of its own name (Codex net-hardening r2-r5). It is
		// checked on a scratch copy, so a refused announce changes nothing. A host whose packets leave from another
		// address than its slot 0 is not listed: it could not start a game for anyone anyway.
		const Bool joiningThis = (game != nullptr) && (m_pendingAction == ACT_JOIN)
			&& (game->getIP(0) == m_lastJoinRequest.GameToJoin.gameIP);
		Bool accepted;
		if ((game != nullptr) && ((game == m_currentGame) || joiningThis))
		{
			accepted = (game->getIP(0) == senderIP);
		}
		else
		{
			LANGameInfo probe;
			accepted = ParseGameOptionsString(&probe, AsciiString(msg->GameInfo.options));
			if (accepted && (probe.getIP(0) != senderIP))
			{
				accepted = FALSE;
				for (Int i = 1; msg->GameInfo.inProgress && (i < MAX_SLOTS); ++i)
				{
					if (probe.getIP(i) == senderIP)
					{
						accepted = TRUE;
						break;
					}
				}
			}
		}
		if (!accepted)
		{
			static Int s_refusals = 0;	// ponytail: capped so a flood cannot become a flood of fflush
			if (s_refusals < 32)
			{
				++s_refusals;
				fprintf(stderr, "[LAN] game announce from %d.%d.%d.%d ignored: it does not speak for that game\n",
					PRINTF_IP_AS_4_INTS(senderIP));
				fflush(stderr);
			}
			return;
		}
		if (!game)
		{
			game = NEW LANGameInfo;
			// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
			game->setName(UnicodeString(GetWindowsWideCharFieldAsWchar(msg->GameInfo.gameName)));
			addGame(game);
		}
		Bool success = ParseGameOptionsString(game,AsciiString(msg->GameInfo.options));
		// GeneralsX @bugfix Claude 06/10/2026 Only a parsed announce updates the flags: the game we are in is kept on
		// a failed parse, and must not become "in progress" (which stops every later announce, option and start)
		// on the word of an announce whose options were refused (Codex net-hardening r1).
		if (success)
		{
			game->setGameInProgress(msg->GameInfo.inProgress);
			game->setIsDirectConnect(msg->GameInfo.isDirectConnect);
			game->setLastHeard(timeGetTime());
		}
		else if (game != m_currentGame)
		{
			// remove from list
			removeGame(game);
			delete game;
			game = nullptr;
		}

		OnGameList( m_games );
	//	if (game == m_currentGame && !m_inLobby)
	//		OnSlotList(RET_OK, game);
	}
}

void LANAPI::handleLobbyAnnounce( LANMessage *msg, UnsignedInt senderIP )
{
	LANPlayer *player = LookupPlayer(senderIP);
	if (!player)
	{
		player = NEW LANPlayer;
		player->setIP(senderIP);
	}
	else
	{
		removePlayer(player);
	}
	// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
	player->setName(UnicodeString(GetWindowsWideCharFieldAsWchar(msg->name)));
	player->setHost(msg->hostName);
	player->setLogin(msg->userName);
	player->setLastHeard(timeGetTime());

	addPlayer(player);

	OnNameChange(player->getIP(), player->getName());
}

void LANAPI::handleRequestGameInfo( LANMessage *msg, UnsignedInt senderIP )
{
	// In game - are we a game host?
	if (m_currentGame)
	{
		if (m_currentGame->getIP(0) == m_localIP || (m_currentGame->isGameInProgress() && TheNetwork && TheNetwork->isPacketRouter())) // if we're in game we should reply if we're the packet router
		{
			// TheSuperHackers @bugfix bobtista 23/08/2026 Bail on an empty options string rather than
			// answer a directed request with a payload the asker cannot parse.
			AsciiString gameOpts = GameInfoToAsciiString(m_currentGame);
			if (gameOpts.isEmpty())
			{
				return;
			}

			LANMessage reply;
			fillInLANMessage( &reply );
			reply.messageType = LANMessage::MSG_GAME_ANNOUNCE;

			strlcpy(reply.GameInfo.options,gameOpts.str(), ARRAY_SIZE(reply.GameInfo.options));
			// GeneralsX @bugfix BenderAI 13/02/2026 Use CopyWcharToWindowsWideChar (fighter19 pattern)
			CopyWcharToWindowsWideChar(reply.GameInfo.gameName, m_currentGame->getName().str(), ARRAY_SIZE(reply.GameInfo.gameName) - 1);
			reply.GameInfo.inProgress = m_currentGame->isGameInProgress();
			reply.GameInfo.isDirectConnect = m_currentGame->getIsDirectConnect();

			sendMessage(&reply, senderIP);
		}
	}
}

static Bool IsInvalidCharForPlayerName(const WideChar c)
{
	return c < L' ' // C0 control chars
		|| c == L',' || c == L':' || c == L';' // chars used for strtok in ParseAsciiStringToGameInfo
		|| (c >= L'\x007f' && c <= L'\x009f') // DEL + C1 control chars
		|| c == L'\x2028' || c == L'\x2029' // line and paragraph separators
		|| (c >= L'\xdc00' && c <= L'\xdfff') // low surrogate, for chars beyond the Unicode Basic Multilingual Plane
		|| (c >= L'\xd800' && c <= L'\xdbff'); // high surrogate, for chars beyond the BMP
}

static Bool IsSpaceCharacter(const WideChar c)
{
	return c == L' ' // space
		|| c == L'\xA0' // no-break space
		|| c == L'\x1680' // ogham space mark
		|| (c >= L'\x2000' && c <= L'\x200A') // en/em spaces, figure, punctuation, thin, hair
		|| c == L'\x202F' // narrow no-break space
		|| c == L'\x205F' // medium mathematical space
		|| c == L'\x3000'; // ideographic space
}

static Bool ContainsInvalidChars(const WideChar* playerName)
{
	DEBUG_ASSERTCRASH(playerName != nullptr, ("playerName is null"));
	while (*playerName)
	{
		if (IsInvalidCharForPlayerName(*playerName++))
			return true;
	}

	return false;
}

static Bool ContainsAnyReadableChars(const WideChar* playerName)
{
	DEBUG_ASSERTCRASH(playerName != nullptr, ("playerName is null"));
	while (*playerName)
	{
		if (!IsSpaceCharacter(*playerName++))
			return true;
	}

	return false;
}

// GeneralsX @feature Claude 27/07/2026 Tier2 SimID warnings. Both peers see the same wording
// from one place: the host emits them inline from handleRequestJoin, the joiner has to defer
// its copy until its chat listbox exists (see LANAPI::flushPendingSimWarnings below), and the
// two must not be allowed to drift apart. These are warnings, not denials - the pair is still
// allowed to play, so the text says what to suspect if the game does desync.
//
// GeneralsX @bugfix Claude 22/09/2026 ONCE PER LOBBY, and `said` is what makes it once. Every
// caller passes the SAME LANAPI member, so the anti-drift rule above now covers suppression too:
// there is no arrangement of joins in which one path says a line the other path would have
// swallowed. Passing nullptr restores the old per-join behaviour and is used only by the self-test.
// See GXSimWarnUnsaidFlags in LANAPI.h for the scope choice and the reset.
//
// GeneralsX @bugfix Claude 22/09/2026 WORDING, for the two conditions that fire on every ordinary
// mixed-platform game. Both were written as faults and neither is one:
//
//   PLATFORM  "cross-platform play has not been validated" was true when it was written and reads
//             as a defect report. Four-platform play IS exercised now, so the honest sentence is
//             that the libraries differ, that this is expected, and that it is worth remembering
//             only if the game actually desyncs. It is NOT softened into a promise of safety -
//             a real cross-platform desync was found in this repo on 22/09 (WIDECHAR command
//             arguments go on the lockstep wire at sizeof(wchar_t), 4 bytes on Apple and Linux
//             against 2 on Windows), so the condition keeps firing and keeps saying something.
//   ASSET     same shape: different archives usually play fine, and phrasing it as a warning makes
//             every modded lobby look broken. Say it once, say what to do if it does go wrong.
//
// PARSE and SOURCE_UNKNOWN are deliberately untouched. Neither fires in an ordinary game, both are
// genuinely alarming when they do, and an edit to either costs a four-platform ship for no gain.
//
// The sentences say "another machine here" rather than "the other machine" because one line now
// stands for the whole lobby: with four peers, the machine that raised it is no longer the only
// other machine in the room.
static void PostSimWarningsToChat( LANAPI *lan, UnsignedInt warn, UnsignedInt *said, UnsignedInt localIP )
{
	warn = GXSimWarnUnsaidFlags( warn, said );

	if (warn & SIMID_WARN_ASSET)
		lan->OnChat(UnicodeString::TheEmptyString, localIP,
			UnicodeString(L"Note: not every machine here has the same game archives loaded (mods?). That normally plays fine - if this game desyncs, it is the first thing to check."),
			LANAPIInterface::LANCHAT_SYSTEM);
	if (warn & SIMID_WARN_PARSE)
		lan->OnChat(UnicodeString::TheEmptyString, localIP,
			UnicodeString(L"Warning: the other machine parses INI decimal numbers differently. If the game desyncs, this is the likely cause."),
			LANAPIInterface::LANCHAT_SYSTEM);
	if (warn & SIMID_WARN_PLATFORM)
		lan->OnChat(UnicodeString::TheEmptyString, localIP,
			UnicodeString(L"Note: another machine here uses a different system math library. That is expected in a mixed-platform game and has not caused a known desync - worth mentioning only if this game does desync."),
			LANAPIInterface::LANCHAT_SYSTEM);
	// Release-visible on purpose: a degraded detector must never be mistaken for a passing check.
	if (warn & SIMID_WARN_SOURCE_UNKNOWN)
		lan->OnChat(UnicodeString::TheEmptyString, localIP,
			UnicodeString(L"Warning: one of these builds has no git information, so its source code could not be compared."),
			LANAPIInterface::LANCHAT_SYSTEM);
}

// GeneralsX @feature Claude 24/08/2026 THE ONE PLACE the host-camera notice is worded - BOTH
// sentences, in one function, on purpose.
//
// Same split as PostSimWarningsToChat directly above: GXHostCamDiffFlags decides (pure, testable,
// no UI), this renders. And the same anti-drift rule: the host and the joiner are told about the
// same fact from opposite ends of it, so their two sentences have to sit where nobody can edit one
// without seeing the other. A second emitter in a second place is exactly how "the host's limits
// apply" and "your limits apply" end up disagreeing about which machine is authoritative.
//
// THE TWO SENTENCES ARE DIFFERENT, and the difference is not cosmetic:
//
//   TO_JOINER  your camera is about to change, and here is what it changes to
//   TO_HOST    somebody else's camera is about to change, here is who and by how much
//
// NOTHING ABOUT THE HOST'S OWN CAMERA MOVES, ever - the host is the authority. The host-side line
// must not read as a warning aimed at the host's own view, which is why it names the player and
// ends by saying whose view is being brought into line with whose.
//
// THE NUMBERS ARE THE MESSAGE. "The host's camera settings apply here" answers nothing: a player
// who notices their zoom changed wants to know what it changed TO, and a warning that does not say
// generates the support question it was written to prevent. Both pairs, always, in one line.
//
// Heights are rendered as whole world units. That is not sloppiness, it is the contract
// GXHOSTCAM_NOTICE_EPSILON is set to: a difference this cannot show is a difference that does not
// fire the notice, so neither sentence can ever name an unchanged range.
enum GXHostCamNoticeRole
{
	GXHOSTCAM_NOTICE_TO_JOINER,	///< printed where the overridden pair is the reader's own
	GXHOSTCAM_NOTICE_TO_HOST,	///< printed where the authoritative pair is the reader's own
};

static Int GXHostCamRound( Real v )
{
	// Camera heights are positive by construction (the seam's floors are 50 and 100), so a plain
	// half-up round is enough and does not need the negative case.
	return (Int)(v + 0.5f);
}

/// `authoritative*` is the pair that will govern the match; `overridden*` is the pair belonging to
/// the machine whose own settings lose. `subjectName` names the owner of the overridden pair and is
/// used only by the host-side sentence - the joiner already knows the answer is "you".
static void PostHostCameraNoticeToChat( LANAPI *lan, GXHostCamNoticeRole role, UnsignedInt diffFlags,
	const WideChar *subjectName,
	Real authoritativeMin, Real authoritativeMax,
	Real overriddenMin, Real overriddenMax, UnsignedInt localIP )
{
	if (diffFlags == 0)
		return;		// nothing differs; a notice that always fires is not a notice

	UnicodeString msg;

	if (role == GXHOSTCAM_NOTICE_TO_HOST)
	{
		// A missing name cannot happen on the live path (handleRequestJoin has already refused a
		// blank or unreadable name by here) but a nameless sentence is still better than a crash.
		const WideChar *who = (subjectName != nullptr && subjectName[0] != 0)
			? subjectName : L"A player";

		msg = TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("LAN:CameraMismatchHost",
			L"%ls prefers camera limits of %d to %d. Yours, %d to %d, govern this game, so their view will match yours.",
			who,
			GXHostCamRound(overriddenMin), GXHostCamRound(overriddenMax),
			GXHostCamRound(authoritativeMin), GXHostCamRound(authoritativeMax));
	}
	else
	{
		msg = TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("LAN:CameraMismatch",
			L"The host's camera limits apply to everyone in this game: %d to %d, instead of your %d to %d.",
			GXHostCamRound(authoritativeMin), GXHostCamRound(authoritativeMax),
			GXHostCamRound(overriddenMin), GXHostCamRound(overriddenMax));
	}

	lan->OnChat(UnicodeString::TheEmptyString, localIP, msg, LANAPIInterface::LANCHAT_SYSTEM);
}

void LANAPI::handleRequestJoin( LANMessage *msg, UnsignedInt senderIP )
{
	if (msg->GameToJoin.gameIP != m_localIP)
	{
		return; // Not us.  Ignore it.
	}
	LANMessage reply;
	// GeneralsX @feature Claude 27/07/2026 `reply` carries every deny AND the accept in this
	// function, and only the RET_GAME_FULL branch fills GameNotJoined.gameName. The
	// RET_GAME_STARTED branch, both RET_DUPLICATE_NAME branches and the RET_GAME_GONE branch
	// all shipped ~34 bytes of uninitialised stack that handleJoinDeny then feeds straight into
	// LookupGame(). Zeroing here is a prerequisite for the appended sim field being trustworthy
	// and independently fixes an existing stack leak onto the LAN broadcast.
	memset(&reply, 0, sizeof(reply));
	fillInLANMessage( &reply );
	if (!m_inLobby && m_currentGame && m_currentGame->getIP(0) == m_localIP)
	{
		if (m_currentGame->isGameInProgress())
		{
			reply.messageType = LANMessage::MSG_JOIN_DENY;
			reply.GameNotJoined.reason = LANAPIInterface::RET_GAME_STARTED;
			reply.GameNotJoined.gameIP = m_localIP;
			reply.GameNotJoined.playerIP = senderIP;
			DEBUG_LOG(("LANAPI::handleRequestJoin - join denied because game already started."));
		}
		else
		{
			// GeneralsX @feature Claude 10/08/2026 Idempotent re-accept. The joiner re-sends an
			// unanswered MSG_REQUEST_JOIN once a second (LANAPI::update), so a request from a
			// peer already seated in this game means our MSG_JOIN_ACCEPT was lost or still in
			// flight. Without this branch the duplicate walks into the duplicate-name scan below
			// and is answered RET_DUPLICATE_NAME - the retry converts a lost accept into "name in
			// use" - and in a password game it dies even earlier, because the first accept
			// consumed the challenge and the replay reads as RET_BAD_PASSWORD.
			//
			// So: BEFORE the SimID and password gates, re-send the accept for the slot the peer
			// already holds. No setSlot (would wipe accept/hasMap state), no OnPlayerJoin (would
			// reset every accept and rebroadcast), no warning re-post. The build-kind stamp is
			// duplicated from the shared tail on purpose - this branch returns before reaching
			// it. Also heals a leave-then-rejoin inside the 80 s reap window, which used to be
			// denied RET_DUPLICATE_NAME until the ghost slot expired.
			for (int seated = 1; seated < MAX_SLOTS; ++seated)
			{
				LANGameSlot *seatedSlot = m_currentGame->getLANSlot(seated);
				if (seatedSlot && seatedSlot->isHuman()
					&& m_currentGame->getIP(seated) == senderIP
					&& seatedSlot->getName().compare(GetWindowsWideCharFieldAsWchar(msg->name)) == 0)
				{
					// GeneralsX @fix Claude 10/08/2026 (cross-model review) Clear the slot's accept.
					// For a true retransmission this is a no-op - the peer has not seen our accept,
					// so it cannot have readied up. For a crash-and-rejoin inside the 80 s reap
					// window it is the fix: the old session's accept must not let the host start a
					// match the returning player never re-consented to. Team, colour and start
					// position deliberately survive - a quick reconnect keeping its seat is a
					// feature; a stale consent is not. The options rebroadcast below propagates it.
					seatedSlot->unAccept();

					reply.messageType = LANMessage::MSG_JOIN_ACCEPT;
					CopyWcharToWindowsWideChar(reply.GameJoined.gameName, m_currentGame->getName().str(), ARRAY_SIZE(reply.GameJoined.gameName) - 1);
					reply.GameJoined.slotPosition = seated;
					reply.GameJoined.gameIP = m_localIP;
					reply.GameJoined.playerIP = senderIP;
					SimulationId::get().toWire( reply.GameJoined.sim );
					reply.GameJoined.buildTag   = BUILDKIND_WIRE_TAG;
					reply.GameJoined.buildFlags = gxLocalBuildKind();

					fprintf(stderr, "[LAN] duplicate join request from %d.%d.%d.%d - re-sending accept for slot %d\n",
						PRINTF_IP_AS_4_INTS(senderIP), seated);
					fflush(stderr);

					sendMessage(&reply, senderIP);
					RequestGameOptions(GenerateGameOptionsString(), true);
					return;
				}
			}

			int player;
			Bool canJoin = true;

			// GeneralsX @feature Claude 27/07/2026 Join-time build/data compatibility check.
			// Unconditional on purpose: both retail guards had to go, not just the inner one.
			// RTS_ZEROHOUR=1 compiled the inner CRC check away entirely in Zero Hour, and the
			// outer RTS_DEBUG plus m_netMinPlayers guard left it skipped in every release build,
			// so in practice nothing was ever compared. TheGlobalData->m_iniCRC/m_exeCRC are no
			// longer consulted here - the immutable snapshot in SimulationId is what is compared,
			// because the RTS_DEBUG backdoor in WOLLobbyMenu can overwrite the GlobalData copies
			// with a remote peer's advertised values. The joiner still sends the legacy
			// exeCRC/iniCRC fields untouched, for old peers that only understand those.
			// This runs before the player-name validation and the open-slot scan below, so the
			// build check is the first gate.
			const SimIdVerdict simVerdict = SimIdCompare( msg->GameToJoin.sim );

			// GeneralsX @feature Claude 09/08/2026 Does the build KIND explain this mismatch?
			//
			// computeEngineID() puts RTS_DEBUG in engine bit 7, and engineID is tier1 - so a debug
			// peer against a release host is already refused here, as SIMID_ENGINE_DIFFERS, before
			// the build-kind gate further down can ever see it. Left alone, the person is told
			// "build mismatch, engine 00000080 vs 00000000" and has to know what bit 7 means.
			//
			// So the refusal stays exactly where it was and only its REASON changes. That keeps
			// one gate, one order and one set of side effects, and avoids the alternative of
			// moving the build-kind check above the SimID check - which would have quietly
			// changed which diagnosis every other kind of mismatch gets.
			const Bool kindKnown = (msg->GameToJoin.buildTag == BUILDKIND_WIRE_TAG);
			const Bool kindDiffers = kindKnown && (msg->GameToJoin.buildFlags != gxLocalBuildKind());

			if (simVerdict != SIMID_OK)
			{
				reply.messageType            = LANMessage::MSG_JOIN_DENY;
				reply.GameNotJoined.reason   = kindDiffers ? LANAPIInterface::RET_DEBUG_PEER
														   : LANAPIInterface::RET_CRC_MISMATCH;
				reply.GameNotJoined.gameIP   = m_localIP;
				reply.GameNotJoined.playerIP = senderIP;
				CopyWcharToWindowsWideChar(reply.GameNotJoined.gameName, m_currentGame->getName().str(),
					ARRAY_SIZE(reply.GameNotJoined.gameName) - 1);
				// Give the joiner our identity so it can name the specific field too.
				SimulationId::get().toWire( reply.GameNotJoined.sim );
				canJoin = false;

				// Release-visible, unlike the DEBUG_LOG this replaces. The host is already inside
				// LanGameOptionsMenu with the listbox live, so this reaches the screen.
				const SimulationId &me = SimulationId::get();

				// GeneralsX @feature Claude 27/07/2026 The chat line below is capped at what fits
				// on screen and drops parseID/assetID/platformID entirely. Log the full field-by-field
				// comparison to stderr as well, so a refusal is diagnosable from the log alone - this
				// is the host half of the pair, the joiner half is in the JOIN_ACCEPT handler.
				{
					char peerAddr[24];
					snprintf(peerAddr, sizeof(peerAddr), "%u.%u.%u.%u",
						(senderIP >> 24) & 0xFF, (senderIP >> 16) & 0xFF, (senderIP >> 8) & 0xFF, senderIP & 0xFF);
					SimIdLogMismatch("HOST", peerAddr, simVerdict, msg->GameToJoin.sim);
				}

				UnicodeString sysMsg;
				if (kindDiffers)
				{
					// Named FIRST, because when it applies it is the whole explanation and the
					// digests below are a consequence of it rather than an independent fact.
					sysMsg.format(L"%ls could not join: they are running a different kind of build (theirs 0x%02x, yours 0x%02x). Debug, profiled and sanitizer builds do not simulate identically to a release build.",
						GetWindowsWideCharFieldAsWchar(msg->name),
						msg->GameToJoin.buildFlags, gxLocalBuildKind());
				}
				else if (simVerdict == SIMID_LOCAL_INVALID)
				{
					// Blaming the joiner here would be the wrong diagnosis - we are the broken
					// machine. RequestGameCreate refuses to host in this state, so reaching this
					// means a future caller found a way to host without going through it.
					sysMsg.format(L"%ls could not join: THIS machine did not finish loading its data and cannot host a network game. Restart the game.",
						GetWindowsWideCharFieldAsWchar(msg->name));
				}
				else if (simVerdict == SIMID_PEER_SILENT)
				{
					sysMsg.format(L"%ls could not join: their build is too old to report a build identity.",
						GetWindowsWideCharFieldAsWchar(msg->name));
				}
				else
				{
					// GeneralsX @bugfix Claude 01/08/2026 "build %u" was a commit count and is now
					// a unix build timestamp - see SimulationId.h. Left as a raw number rather
					// than a date: this is the HOST's diagnostic chat line, sitting beside four
					// %08X digests, and a reader comparing it against the [SIMID] stderr line
					// wants the same number they will find there.
					sysMsg.format(L"%ls could not join: build mismatch. Theirs built %u / engine %08X / source %08X / data %08X / ordinal %08X; yours built %u / %08X / %08X / %08X / %08X.",
						GetWindowsWideCharFieldAsWchar(msg->name),
						msg->GameToJoin.sim.buildTime, msg->GameToJoin.sim.engineID,
						msg->GameToJoin.sim.sourceID,  msg->GameToJoin.sim.dataID,
						msg->GameToJoin.sim.ordinalID,
						me.buildTime, me.engineID, me.sourceID, me.dataID, me.ordinalID);
				}
				OnChat(UnicodeString::TheEmptyString, m_localIP, sysMsg, LANAPIInterface::LANCHAT_SYSTEM);
			}
			// GeneralsX @feature Claude 08/08/2026 LOBBY PASSWORD GATE. This is the enforcement.
			//
			// It lives here, in the host's inbound handler, and nowhere else. Hiding a game behind
			// a prompt on the joining machine protects nothing - the joiner is the machine you do
			// not control, and a peer that simply does not run the prompt is in. The only place a
			// password can actually be enforced is the point where the host decides whether to put
			// somebody in a slot, and that point is the loop below.
			//
			// Runs AFTER the SimID check (an incompatible build is refused for being incompatible,
			// which is the more useful diagnosis) and BEFORE the name and slot scans, so a refused
			// joiner never reaches m_currentGame->setSlot and can never appear in the lobby.
			//
			// The two refusal reasons are distinct on purpose. NEEDS_PASSWORD is what an honest
			// first knock gets and is not a failure - the joiner answers it automatically. Only
			// BAD_PASSWORD reaches the player as an error.
			if (canJoin && m_hostPwRequired)
			{
				const Bool offered = (msg->GameToJoin.pwTag == LOBBYPW_WIRE_TAG);
				if (!offered || !checkPasswordResponse(msg->GameToJoin.pwChallenge, msg->GameToJoin.pwResponse))
				{
					reply.messageType            = LANMessage::MSG_JOIN_DENY;
					reply.GameNotJoined.reason   = offered ? LANAPIInterface::RET_BAD_PASSWORD
														  : LANAPIInterface::RET_NEEDS_PASSWORD;
					reply.GameNotJoined.gameIP   = m_localIP;
					reply.GameNotJoined.playerIP = senderIP;
					CopyWcharToWindowsWideChar(reply.GameNotJoined.gameName, m_currentGame->getName().str(),
						ARRAY_SIZE(reply.GameNotJoined.gameName) - 1);

					// Hand back the salt and a FRESH single-use challenge, so the joiner can
					// compute an answer. Sending these on a wrong-password refusal too is
					// deliberate: without it a mistyped password would leave the joiner holding a
					// consumed challenge and every retry would fail for the wrong reason.
					reply.GameNotJoined.pwTag       = LOBBYPW_WIRE_TAG;
					reply.GameNotJoined.pwSalt      = m_hostPwSalt;
					reply.GameNotJoined.pwChallenge = issuePasswordChallenge();

					canJoin = false;

					DEBUG_LOG(("LANAPI::handleRequestJoin - join refused from %d.%d.%d.%d: %s",
						PRINTF_IP_AS_4_INTS(senderIP),
						offered ? "wrong password" : "no password supplied"));
				}
			}

			// GeneralsX @feature Claude 09/08/2026 BUILD-KIND GATE.
			//
			// sourceID does not catch this and cannot: it digests the simulation SOURCE, so a debug
			// build and a release build of the same commit have the same sourceID and join each
			// other happily. They do not simulate identically - a debug build takes different code
			// paths and runs asserts - and under lockstep the slowest peer sets the pace for
			// everybody, so one forgotten debug build degrades the whole lobby.
			//
			// THE RULE IS "MATCH THE HOST", not "no debug builds". A lobby where everybody is on a
			// debug build is a debugging session and works fine; the case worth refusing is the
			// MISMATCH, which is almost always somebody who forgot which build they were on. That
			// also means the gate needs no setting, no UI and no default to argue about, and it can
			// never block a deliberate all-debug test.
			//
			// A peer that sends no tag is ADMITTED. It is a build from before this feature, and
			// refusing what you cannot measure would turn "I do not know" into "no" - which is the
			// same answer this gate gives to a peer it has actually judged.
			//
			// GX_ALLOW_DEBUG_PEERS=1 on the HOST turns it off, for the session where somebody
			// genuinely wants a mixed lobby. It is an environment variable rather than a preference
			// because it is a developer's escape hatch, and putting it in Options.ini would make it
			// a thing players find and turn on.
			if (canJoin && msg->GameToJoin.buildTag == BUILDKIND_WIRE_TAG)
			{
				const UnsignedInt theirs = msg->GameToJoin.buildFlags;
				const UnsignedInt ours   = gxLocalBuildKind();
				const char *allow = getenv("GX_ALLOW_DEBUG_PEERS");
				if (theirs != ours && !(allow && allow[0] == '1'))
				{
					reply.messageType            = LANMessage::MSG_JOIN_DENY;
					reply.GameNotJoined.reason   = LANAPIInterface::RET_DEBUG_PEER;
					reply.GameNotJoined.gameIP   = m_localIP;
					reply.GameNotJoined.playerIP = senderIP;
					CopyWcharToWindowsWideChar(reply.GameNotJoined.gameName, m_currentGame->getName().str(),
						ARRAY_SIZE(reply.GameNotJoined.gameName) - 1);

					canJoin = false;

					fprintf(stderr, "[LAN] join refused from %d.%d.%d.%d: build kind 0x%02x, "
						"this host is 0x%02x\n", PRINTF_IP_AS_4_INTS(senderIP), theirs, ours);
					fflush(stderr);
				}
			}

			// The tier2 warnings for a peer that passes are NOT emitted here: this function is
			// reached by every MSG_REQUEST_JOIN datagram, including duplicates and requests that
			// are about to be refused for a duplicate name or a full game. They are emitted from
			// the JOIN_ACCEPT branch below instead, so they appear exactly once, and only about
			// a peer that actually got into a slot.

// TheSuperHackers @tweak Disables the duplicate serial check
#if 0
			// check for a duplicate serial
			AsciiString s;
			for (player = 0; canJoin && player<MAX_SLOTS; ++player)
			{
				LANGameSlot *slot = m_currentGame->getLANSlot(player);
				s.clear();
				if (player == 0)
				{
					GetStringFromRegistry("\\ergc", "", s);
				}
				else if (slot->isHuman())
				{
					s = slot->getSerial();
					if (s.isEmpty())
						s = "<Munkee>";
				}

				if (s.isNotEmpty())
				{
					DEBUG_LOG(("Checking serial '%s' in slot %d", s.str(), player));

					if (!strncmp(s.str(), msg->GameToJoin.serial, g_maxSerialLength))
					{
						// serials match!  kick the punk!
						reply.messageType = LANMessage::MSG_JOIN_DENY;
						reply.GameNotJoined.reason = LANAPIInterface::RET_SERIAL_DUPE;
						reply.GameNotJoined.gameIP = m_localIP;
						reply.GameNotJoined.playerIP = senderIP;
						canJoin = false;

						DEBUG_LOG(("LANAPI::handleRequestJoin - join denied because of duplicate serial # (%s).", s.str()));
						break;
					}
				}
			}
#endif

			// TheSuperHackers @bugfix slurmlord 18/09/2025 need to validate the name of the connecting player before
			// allowing them to join to prevent messing up the format of game state string. Commas, colons, semicolons etc.
			// should not be in a player name. It should also not consist of only space characters.
			if (canJoin)
			{
				// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
				if (ContainsInvalidChars(GetWindowsWideCharFieldAsWchar(msg->name)) || !ContainsAnyReadableChars(GetWindowsWideCharFieldAsWchar(msg->name)))
				{
					// Just deny with a duplicate name reason, for backwards compatibility with retail
					reply.messageType = LANMessage::MSG_JOIN_DENY;
					reply.GameNotJoined.reason = LANAPIInterface::RET_DUPLICATE_NAME;
					reply.GameNotJoined.gameIP = m_localIP;
					reply.GameNotJoined.playerIP = senderIP;
					canJoin = false;

					DEBUG_LOG(("LANAPI::handleRequestJoin - join denied because of illegal characters in the player name."));
				}
			}	

			// Then see if the player has a duplicate name
			for (player = 0; canJoin && player<MAX_SLOTS; ++player)
			{
				LANGameSlot *slot = m_currentGame->getLANSlot(player);
				// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
				if (slot->isHuman() && slot->getName().compare(GetWindowsWideCharFieldAsWchar(msg->name)) == 0)
				{
					// just deny duplicates
					reply.messageType = LANMessage::MSG_JOIN_DENY;
					reply.GameNotJoined.reason = LANAPIInterface::RET_DUPLICATE_NAME;
					reply.GameNotJoined.gameIP = m_localIP;
					reply.GameNotJoined.playerIP = senderIP;
					canJoin = false;

					DEBUG_LOG(("LANAPI::handleRequestJoin - join denied because of duplicate names."));
					break;
				}
			}

			// TheSuperHackers @bugfix Stubbjax 26/09/2025 Players can now join open slots regardless of starting spots on the map.
			// GeneralsX @feature Claude 12/08/2026 Humans may only occupy slots 0..MAX_HUMAN_SLOTS-1:
			// the byte-wide relay destination mask cannot address a machine in slot 8+, so a human
			// placed there would form the lobby and then never receive a single in-match command.
			// Slots MAX_HUMAN_SLOTS..MAX_SLOTS-1 are for AI only. See NetworkDefs.h.
			for (player = 0; canJoin && player<MAX_HUMAN_SLOTS; ++player)
			{
				if (m_currentGame->getLANSlot(player)->isOpen())
				{
					// OK, add him in.
					reply.messageType = LANMessage::MSG_JOIN_ACCEPT;
					// GeneralsX @bugfix BenderAI 13/02/2026 Use CopyWcharToWindowsWideChar (fighter19 pattern)
					CopyWcharToWindowsWideChar(reply.GameJoined.gameName, m_currentGame->getName().str(), ARRAY_SIZE(reply.GameJoined.gameName) - 1);
					reply.GameJoined.slotPosition = player;
					reply.GameJoined.gameIP = m_localIP;
					reply.GameJoined.playerIP = senderIP;
					// GeneralsX @feature Claude 27/07/2026 The accept carries the host's identity so
					// the joiner can verify it too - the host is the machine that may be stale, and
					// its own check above only proves the joiner matches whatever the host is.
					SimulationId::get().toWire( reply.GameJoined.sim );

					LANGameSlot newSlot;
					// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
					newSlot.setState(SLOT_PLAYER, UnicodeString(GetWindowsWideCharFieldAsWchar(msg->name)));
					newSlot.setIP(senderIP);
					newSlot.setPort(NETWORK_BASE_PORT_NUMBER);
					newSlot.setLastHeard(timeGetTime());
					newSlot.setSerial(msg->GameToJoin.serial);
					m_currentGame->setSlot(player,newSlot);
					DEBUG_LOG(("LANAPI::handleRequestJoin - added player %ls at ip 0x%08x to the game", msg->name, senderIP));

					// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
					OnPlayerJoin(player, UnicodeString(GetWindowsWideCharFieldAsWchar(msg->name)));

					// GeneralsX @feature Claude 27/07/2026 Host side tier2 warnings, posted only
					// now that the peer is genuinely in a slot. The LanGameOptionsMenu listbox is
					// already live on this side, so unlike the joiner (see flushPendingSimWarnings)
					// these can go straight out.
					// GeneralsX @bugfix Claude 22/09/2026 The mask argument is what stops this
					// repeating per joiner: this runs once for EVERY peer that takes a slot, so
					// before it a 4-player lobby printed the same sentence three times.
					// (Deliberately not naming the member in prose - a comment that spells it
					// makes `grep -c` over this file count two call sites where there is one.)
					PostSimWarningsToChat( this, SimIdWarnFlags( msg->GameToJoin.sim ),
						&m_saidSimWarnFlags, m_localIP );

					// GeneralsX @feature Claude 24/08/2026 Host side of the camera notice, item 3 of
					// the same shape. Inline rather than deferred for the same reason the tier2
					// warnings above are: the LanGameOptionsMenu chat listbox is ALREADY live on this
					// side, so there is nothing to wait for. Only the joiner has to stash and flush.
					//
					// THE HOST'S OWN CAMERA DOES NOT MOVE. This is the host being told which of its
					// players is having their settings replaced, and by how much - the host is the
					// one machine that can do anything about it, because the lobby's limits come from
					// the host's own Options.ini.
					//
					// Both queries must succeed or we say nothing. An untagged field is a peer that
					// predates the feature - the ordinary case for an older build, not an error, and
					// MSG_REQUEST_JOIN is the message where uninitialised dead bytes are most likely
					// to arrive. A pair validate() refuses is a pair we cannot honestly quote.
					{
						Real joinerCamMin = 0.0f;
						Real joinerCamMax = 0.0f;
						GXHostCamera::Limits theirs;
						GXHostCamera::Limits mine;
						if (GXHostCamWireRead(msg->GameToJoin.cam, &joinerCamMin, &joinerCamMax)
							&& GXHostCamera::validate(joinerCamMin, joinerCamMax, &theirs)
							&& GXHostCamera::getLocalLimits(&mine))
						{
							// Same predicate as the joiner side, same epsilon, same silence when the
							// two agree. Passed straight to the emitter, which is a no-op on 0 -
							// PostSimWarningsToChat above is called the same way for the same reason.
							PostHostCameraNoticeToChat( this, GXHOSTCAM_NOTICE_TO_HOST,
								GXHostCamDiffFlags( mine.minHeight, mine.maxHeight,
									theirs.minHeight, theirs.maxHeight ),
								GetWindowsWideCharFieldAsWchar( msg->name ),
								mine.minHeight, mine.maxHeight,
								theirs.minHeight, theirs.maxHeight, m_localIP );
						}
					}

					break;
				}
			}

			if (canJoin && player == MAX_HUMAN_SLOTS)
			{
				reply.messageType = LANMessage::MSG_JOIN_DENY;
				// GeneralsX @bugfix BenderAI 13/02/2026 Use CopyWcharToWindowsWideChar (fighter19 pattern)
				CopyWcharToWindowsWideChar(reply.GameNotJoined.gameName, m_currentGame->getName().str(), ARRAY_SIZE(reply.GameNotJoined.gameName) - 1);
				reply.GameNotJoined.reason = LANAPIInterface::RET_GAME_FULL;
				reply.GameNotJoined.gameIP = m_localIP;
				reply.GameNotJoined.playerIP = senderIP;
				DEBUG_LOG(("LANAPI::handleRequestJoin - join denied because game is full."));
			}
		}
	}
	else
	{
		reply.messageType = LANMessage::MSG_JOIN_DENY;
		reply.GameNotJoined.reason = LANAPIInterface::RET_GAME_GONE;
		reply.GameNotJoined.gameIP = m_localIP;
		reply.GameNotJoined.playerIP = senderIP;
	}
	// GeneralsX @feature Claude 09/08/2026 The host's own build kind rides EVERY reply, accept or
	// deny, and it is stamped HERE - one place, immediately before the send, after messageType has
	// stopped changing.
	//
	// It has to be keyed on messageType because accept and deny are different arms of a union and
	// put this field at different offsets. An earlier version stamped GameNotJoined unconditionally,
	// which on the accept path writes those eight bytes into the middle of GameJoined - somewhere
	// around sim - and corrupts the identity the joiner is about to verify. The union is why this
	// looks like it needs a switch and does.
	if (reply.messageType == LANMessage::MSG_JOIN_ACCEPT)
	{
		reply.GameJoined.buildTag   = BUILDKIND_WIRE_TAG;
		reply.GameJoined.buildFlags = gxLocalBuildKind();

		// GeneralsX @feature Claude 24/08/2026 Our camera height limits ride the accept, stamped
		// in the same block and for the same union reason: accept and deny put their appended
		// fields at different offsets, so this may only be written once messageType has stopped
		// changing. The DENY path deliberately carries nothing - a peer that was refused has no
		// use for our camera settings, and every byte of a deny is read by code that predates this.
		//
		// The explicit clear is not redundant with the memset at the top of this function. It is
		// the statement that matters: an unstamped field means 'we are not telling you', and the
		// only way to say that is to make sure the tag is not there. GAME_START, whose message is
		// NOT memset, depends on exactly this discipline.
		GXHostCamera::Limits mine;
		if (GXHostCamera::getLocalLimits(&mine))
			GXHostCamWireWrite(reply.GameJoined.cam, mine.minHeight, mine.maxHeight);
		else
			GXHostCamWireClear(reply.GameJoined.cam);
	}
	else if (reply.messageType == LANMessage::MSG_JOIN_DENY)
	{
		reply.GameNotJoined.buildTag   = BUILDKIND_WIRE_TAG;
		reply.GameNotJoined.buildFlags = gxLocalBuildKind();
	}

	// GeneralsX @bugfix The accept path used to zero the destination before this send, which drops the
	// reply into sendMessage's broadcast branch. Darwin refuses a send to 255.255.255.255 with
	// EHOSTUNREACH, so a game hosted from the LAN lobby - as opposed to Direct Connect, whose branch
	// unicasts to each occupied slot and therefore happened to work - never put the accept on the wire
	// at all: the host showed the joiner in a slot while the joiner sat until m_actionTimeout and
	// reported RET_TIMEOUT. The reply belongs to exactly one peer in any case. It carries that peer's
	// slot position, every other recipient discards it on the "playerIP == m_localIP" test in
	// handleJoinAccept, and the rest of the lobby learns about the new player from the MSG_GAME_OPTIONS
	// slot list pushed immediately below, not from this message. So address it to the requester, the
	// same destination the deny replies above already use.
	sendMessage(&reply, senderIP);
	RequestGameOptions(GenerateGameOptionsString(), true);
}

void LANAPI::handleJoinAccept( LANMessage *msg, UnsignedInt senderIP )
{
	// GeneralsX @bugfix Claude 28/07/2026 NAT: the joiner and the host do not agree on the
	// joiner's address.
	//
	// The stock test is `msg->GameJoined.playerIP == m_localIP`, which assumes the address the
	// host saw is the address we know ourselves by. That holds on a LAN and fails through NAT:
	// the host fills playerIP with the packet's source address (our PUBLIC IP), while m_localIP
	// is the private one. The accept is then dropped in silence - the host logs
	// "player joined slot 1" while the joiner sits until its deadline and reports a timeout, so
	// both machines' logs look like the other end is at fault.
	//
	// Accept the second form only for a direct-connect join we ourselves initiated to this exact
	// host. m_directConnectRemoteIP is zero for ordinary LAN joins, so LAN behaviour is
	// unchanged, and an unsolicited accept from any other address is still ignored.
	const Bool addressedToUs = (msg->GameJoined.playerIP == m_localIP);
	// GeneralsX @security Claude 06/10/2026 Addressed to us is not enough: it must come from the host our pending
	// MSG_REQUEST_JOIN went to. Any peer could send an accept naming our address and seat us in whatever listing, at
	// whatever slot, it named (Codex net-hardening r1). That host is the address we dialled for a direct connect
	// (sendJoinRequest keeps it as m_lastJoinRequestDest) - the only authority there: the slot-0 address it announced
	// can be a private one some peer on our own LAN holds (Codex r2) - and otherwise the listing's host, the gameIP the
	// request named, which answers from the address handleGameStart already requires. Through NAT a direct-connect
	// host sees our public address, so there the accept need not name m_localIP.
	const Bool directJoin = (m_lastJoinRequestDest != 0);
	const UnsignedInt askedHost = directJoin ? m_lastJoinRequestDest : m_lastJoinRequest.GameToJoin.gameIP;
	const Bool fromHostWeAsked = (askedHost != 0) && (senderIP == askedHost);

	if (fromHostWeAsked && (addressedToUs || directJoin)) // Is it for us?
	{
		// Adopt the address the host assigned us. Every peer in this game will refer to us by it,
		// and the slot list we are about to parse is keyed on it - keeping the private address
		// here would leave us unable to find our own slot.
		if (!addressedToUs && msg->GameJoined.playerIP != 0)
		{
			DEBUG_LOG(("handleJoinAccept - NAT: host sees us as %d.%d.%d.%d, we call ourselves %d.%d.%d.%d",
				PRINTF_IP_AS_4_INTS(msg->GameJoined.playerIP), PRINTF_IP_AS_4_INTS(m_localIP)));
			fprintf(stderr, "[LAN] NAT: host sees us as %d.%d.%d.%d, local is %d.%d.%d.%d\n",
				PRINTF_IP_AS_4_INTS(msg->GameJoined.playerIP), PRINTF_IP_AS_4_INTS(m_localIP));
			fflush(stderr);
		}

		if (m_pendingAction == ACT_JOIN) // Are we trying to join?
		{
			// GeneralsX @feature Claude 27/07/2026 The host may be the un-upgraded machine:
			// handleRequestJoin's only pre-SimID check was wrapped in #if defined(RTS_DEBUG) and
			// #if !RTS_ZEROHOUR, so a stale host accepts happily and the desync surfaces around
			// frame 100. Verify the host's identity here, independently. Runs before LookupGame
			// below, so no game state is mutated on a rejected accept.
			// GeneralsX @feature Claude 09/08/2026 The host let us in, and this says what it is
			// running. Under lockstep a debug HOST degrades every peer in the match, and the host
			// is the one machine whose build kind nobody else can refuse - so if it is going to be
			// slow, it should at least be explicable from the log rather than a mystery.
			if (msg->GameJoined.buildTag == BUILDKIND_WIRE_TAG && msg->GameJoined.buildFlags != 0)
			{
				// stderr for the same reason as the deny trace: DEBUG_LOG is ((void)0) in every
				// release preset, and "the host is a debug build" is precisely the fact somebody
				// wants when they later ask why the match was slow.
				fprintf(stderr, "[LAN] the HOST of this game is running a debug/instrumented build "
					"(kind 0x%02x); expect it to set the pace for everyone\n",
					msg->GameJoined.buildFlags);
				fflush(stderr);
			}

			m_remoteSim        = msg->GameJoined.sim;
			m_remoteSimValid   = (m_remoteSim.tag == SIMID_WIRE_TAG);
			m_remoteSimVerdict = SimIdCompare( m_remoteSim );
			if (m_remoteSimVerdict != SIMID_OK)
			{
				// GeneralsX @feature Claude 27/07/2026 Joiner half of the refusal trace. Without
				// this the joiner only ever shows RET_CRC_MISMATCH in a MessageBox, which names no
				// field, and the host's stderr is on a different machine.
				{
					char peerAddr[24];
					snprintf(peerAddr, sizeof(peerAddr), "%u.%u.%u.%u",
						(senderIP >> 24) & 0xFF, (senderIP >> 16) & 0xFF, (senderIP >> 8) & 0xFF, senderIP & 0xFF);
					SimIdLogMismatch("JOINER", peerAddr, m_remoteSimVerdict, m_remoteSim);
				}

				// Free the slot the host already reserved rather than leaving a ghost player.
				LANMessage leaveMsg;
				memset(&leaveMsg, 0, sizeof(leaveMsg));
				leaveMsg.messageType = LANMessage::MSG_REQUEST_GAME_LEAVE;
				fillInLANMessage( &leaveMsg );
				CopyWcharToWindowsWideChar(leaveMsg.PlayerInfo.playerName, m_name.str(),
					ARRAY_SIZE(leaveMsg.PlayerInfo.playerName) - 1);
				sendMessage(&leaveMsg, senderIP);
				m_pendingAction = ACT_NONE;
				m_expiration = 0;
				OnGameJoin(RET_CRC_MISMATCH, nullptr);
				return;
			}
			m_pendingSimWarnFlags = SimIdWarnFlags( m_remoteSim );

			// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
			m_currentGame = LookupGame(UnicodeString(GetWindowsWideCharFieldAsWchar(msg->GameJoined.gameName)));

			if (!m_currentGame)
			{
				DEBUG_CRASH(("Could not find game to join!"));
				OnGameJoin(RET_UNKNOWN, nullptr);
			}
			else
			{
				m_inLobby = false;
				// GeneralsX @bugfix Claude 11/08/2026 Fresh room, fresh start-nonce history. A new host
				// counts nonces from 1, so a stale value from a previous room's host could swallow
				// this room's first genuine start.
				m_lastStartNonceProcessed = 0;

				// GeneralsX @feature Claude 24/08/2026 The host's camera height limits, learned in the
				// lobby. Adopted here rather than only at GAME_START so the pair is known while the
				// player can still back out; the GAME_START copy is the authoritative one and REPLACES
				// whatever this left behind, including replacing it with nothing (see handleGameStart).
				//
				// Nothing is validated here: the seam's setHostLimits clamps, refuses NaN and refuses an
				// inverted pair, and a second range check in this file would be a second thing to keep
				// in step with OptionPreferences. An untagged field is not an error and not a zero - it
				// is a host that never wrote those bytes, and it leaves us on our own limits.
				{
					Real hostCamMin = 0.0f;
					Real hostCamMax = 0.0f;
					if (GXHostCamWireRead(msg->GameJoined.cam, &hostCamMin, &hostCamMax))
					{
						GXHostCamera::setHostLimits(hostCamMin, hostCamMax);

						// GeneralsX @feature Claude 24/08/2026 Say so, if it actually changes anything for us.
						//
						// COMPARED AFTER validate(), NOT AS RECEIVED. validate() applies exactly the clamp
						// setHostLimits will apply, so this compares the pair that will really be in force
						// against ours. Comparing the raw wire pair would announce a difference that the clamp
						// is about to erase - a warning about something that does not happen.
						//
						// EITHER QUERY FAILING MEANS SILENCE, and that is the correct default rather than a
						// missed case: a pair validate() refuses is a pair the peer will not adopt, so nothing
						// changes and there is nothing to announce; and if we cannot read our OWN limits we
						// have nothing to compare against and cannot honestly name a number.
						//
						// Stashed, not printed. OnChat routes to listboxChatWindowLanGame, which
						// LanGameOptionsMenuInit has not assigned yet at this point in the join - anything
						// emitted here is dropped on the floor by OnChat's null check. See
						// flushPendingHostCameraNotice.
						GXHostCamera::Limits inForce;
						GXHostCamera::Limits mine;
						if (GXHostCamera::validate(hostCamMin, hostCamMax, &inForce)
							&& GXHostCamera::getLocalLimits(&mine))
						{
							const UnsignedInt diff = GXHostCamDiffFlags(inForce.minHeight, inForce.maxHeight,
								mine.minHeight, mine.maxHeight);
							if (diff != 0)
							{
								m_pendingHostCamFlags    = diff;
								m_pendingHostCamHostMin  = inForce.minHeight;
								m_pendingHostCamHostMax  = inForce.maxHeight;
								m_pendingHostCamLocalMin = mine.minHeight;
								m_pendingHostCamLocalMax = mine.maxHeight;
							}
						}
					}
				}
				AsciiString options = GameInfoToAsciiString(m_currentGame);
				m_currentGame->enterGame();
				ParseAsciiStringToGameInfo(m_currentGame, options);

				Int pos = msg->GameJoined.slotPosition;

				LANGameSlot slot;
				slot.setState(SLOT_PLAYER, m_name);
				slot.setIP(m_localIP);
				slot.setPort(NETWORK_BASE_PORT_NUMBER);
				slot.setLastHeard(0);
				slot.setLogin(m_userName);
				slot.setHost(m_hostName);
				m_currentGame->setSlot(pos, slot);

				m_currentGame->getLANSlot(0)->setHost(msg->hostName);
				m_currentGame->getLANSlot(0)->setLogin(msg->userName);

				LANPreferences prefs;
				AsciiString entry;
				entry.format("%d.%d.%d.%d:%s", PRINTF_IP_AS_4_INTS(senderIP), UnicodeStringToQuotedPrintable(m_currentGame->getSlot(0)->getName()).str());
				prefs["RemoteIP0"] = entry;
				prefs.write();

				OnGameJoin(RET_OK, m_currentGame);
				//DEBUG_CRASH(("setting host to %ls@%ls", m_currentGame->getLANSlot(0)->getUser()->getLogin().str(),
				//	m_currentGame->getLANSlot(0)->getUser()->getHost().str()));
			}
			m_pendingAction = ACT_NONE;
			m_expiration = 0;
		}
	}
}

// GeneralsX @feature Claude 27/07/2026 Joining side only. handleJoinAccept discovers the tier2
// warnings, but cannot print them: OnChat routes to listboxChatWindowLanGame once we are in a
// game setup, and LanGameOptionsMenuInit does not assign that window until Shell::update()
// instantiates the layout - Shell::push only records m_pendingPush - so anything emitted from
// handleJoinAccept is dropped by OnChat's `if (chatWindow == nullptr) return;`. LANAPI::update()
// calls this every tick while flags are pending. Lives in this file rather than LANAPI.cpp
// because LANAPICallbacks.h, which declares the listbox, is already included here.
void LANAPI::flushPendingSimWarnings()
{
	if (m_pendingSimWarnFlags == 0)
		return;

	// Nothing left to attach the message to: the join was abandoned before the menu appeared,
	// we are back in the lobby, or the match has already started. Drop the warnings rather than
	// let them surface later in an unrelated game.
	if (m_inLobby || m_currentGame == nullptr || m_currentGame->isGameInProgress())
	{
		m_pendingSimWarnFlags = 0;
		return;
	}

	// Keep waiting rather than lose the message.
	if (listboxChatWindowLanGame == nullptr)
		return;

	const UnsignedInt warn = m_pendingSimWarnFlags;
	m_pendingSimWarnFlags = 0;   // cleared first, so nothing OnChat does can print these twice
	// GeneralsX @bugfix Claude 22/09/2026 The SAME mask the host side passes. A joiner reaches this
	// once per join and so does not repeat on its own, but it must share the mask anyway: the two
	// paths are not exclusive - a peer that hosts after joining, or joins after hosting, runs both
	// against one LANAPI, and a second mask is exactly how the two sentences drift apart.
	PostSimWarningsToChat( this, warn, &m_saidSimWarnFlags, m_localIP );
}

// GeneralsX @feature Claude 08/08/2026 Tell the host what kind of lobby they just opened.
//
// This is not decoration. /password and /private both survive from one game to the next (they
// have to - the relay join path destroys and rebuilds LANAPI, see the note in LANAPI.cpp), so a
// player who set a password to get into somebody else's game would otherwise host a locked lobby
// without knowing it and would read their friends' "cannot join" as a network fault.
//
// Deferred exactly like flushPendingSimWarnings: OnGameCreate pushes the game options screen, and
// the listbox it writes to does not exist until Shell::update has instantiated that layout.
void LANAPI::flushPendingLobbyNotice()
{
	if (!m_pendingLobbyNotice)
		return;

	// Left the game before the menu came up, or the match already started. Drop it rather than
	// let it surface in an unrelated lobby later.
	if (m_inLobby || m_currentGame == nullptr || m_currentGame->isGameInProgress())
	{
		m_pendingLobbyNotice = FALSE;
		return;
	}

	if (listboxChatWindowLanGame == nullptr)
		return;		// keep waiting rather than lose the message

	m_pendingLobbyNotice = FALSE;   // cleared first, so nothing OnChat does can print it twice

	if (m_hostPwRequired)
		OnChat(UnicodeString::TheEmptyString, m_localIP,
			UnicodeString(L"This game requires a password to join. Type /nopassword here to open it to everyone."),
			LANAPIInterface::LANCHAT_SYSTEM);

	if (m_hostPrivate)
		OnChat(UnicodeString::TheEmptyString, m_localIP,
			UnicodeString(L"This game is private: it is not listed in the browser and not announced on the LAN. Type /public to list it."),
			LANAPIInterface::LANCHAT_SYSTEM);
}

// GeneralsX @feature Claude 24/08/2026 Joining side only, and deferred for the third time in this
// file for the third identical reason: handleJoinAccept discovers the difference, but OnChat routes
// to listboxChatWindowLanGame and LanGameOptionsMenuInit has not assigned it yet - Shell::push only
// records m_pendingPush, and the layout is not instantiated until Shell::update runs. A line emitted
// at discovery time is dropped by OnChat's null check and nobody ever sees it.
//
// The HOST needs none of this. Its counterpart goes out inline from handleRequestJoin, through the
// same PostHostCameraNoticeToChat, because its chat listbox already exists when a join arrives.
// That is the whole asymmetry: one deferred caller, one immediate one, one emitter.
void LANAPI::flushPendingHostCameraNotice()
{
	if (m_pendingHostCamFlags == 0)
		return;

	// The join was abandoned before the menu appeared, we are back in the lobby, or the match has
	// already started. Drop it rather than let it surface later in an unrelated game.
	if (m_inLobby || m_currentGame == nullptr || m_currentGame->isGameInProgress())
	{
		m_pendingHostCamFlags = 0;
		return;
	}

	if (listboxChatWindowLanGame == nullptr)
		return;		// keep waiting rather than lose the message

	const UnsignedInt diff = m_pendingHostCamFlags;
	m_pendingHostCamFlags = 0;   // cleared first, so nothing OnChat does can print it twice

	// Authoritative = the host's pair, overridden = ours. The mirror image of the host-side call.
	PostHostCameraNoticeToChat( this, GXHOSTCAM_NOTICE_TO_JOINER, diff,
		nullptr,	// the joiner-side sentence says "your", so it needs no name
		m_pendingHostCamHostMin, m_pendingHostCamHostMax,
		m_pendingHostCamLocalMin, m_pendingHostCamLocalMax,
		m_localIP );
}

void LANAPI::handleJoinDeny( LANMessage *msg, UnsignedInt senderIP )
{
	// GeneralsX @bugfix Claude 09/08/2026 NAT: a DENY was only ever accepted when the host's idea
	// of our address matched our own, and through NAT it does not - the host fills playerIP with
	// the packet's source, our PUBLIC address, while m_localIP is private.
	//
	// handleJoinAccept has had the fallback for this since 28/07/2026; the deny path never got it.
	// So over direct-connect every refusal was silently discarded and reported as RET_TIMEOUT -
	// build mismatch, wrong password, game full, all of them - and the joiner was told the host had
	// not answered when the host had answered and said no. Found by cross-model review of the
	// build-kind refusal, which is simply the newest reason to be dropped this way.
	//
	// Same shape as the accept: addressed to us, OR from the host we are direct-connecting to.
	const Bool denyAddressedToUs = (msg->GameNotJoined.playerIP == m_localIP);
	// GeneralsX @security Claude 06/10/2026 As handleJoinAccept: only the host our pending join went to may refuse us.
	// Any peer could abort our join with a denial addressed to us, or answer NEEDS_PASSWORD naming itself as gameIP,
	// which re-aimed the password retry (and so m_lastJoinRequest) at itself and let its own accept through (Codex
	// net-hardening r1).
	const Bool denyDirectJoin = (m_lastJoinRequestDest != 0);
	const UnsignedInt denyAskedHost = denyDirectJoin ? m_lastJoinRequestDest : m_lastJoinRequest.GameToJoin.gameIP;
	const Bool denyFromHostWeAsked = (denyAskedHost != 0) && (senderIP == denyAskedHost);

	if (denyFromHostWeAsked && (denyAddressedToUs || denyDirectJoin)) // Is it for us?
	{
		if (m_pendingAction == ACT_JOIN) // Are we trying to join?
		{
			// GeneralsX @feature Claude 27/07/2026 OnGameJoin cannot take extra parameters without
			// changing the LANAPIInterface virtual contract, so stash the host's identity here for
			// the failure dialog to format. Note this function reads msg->GameJoined.playerIP out
			// of a JOIN_DENY, which works because both union arms keep playerIP at the same
			// offset - the appended sim field is last in each arm and does not disturb that.
			m_remoteSim        = msg->GameNotJoined.sim;
			m_remoteSimValid   = (m_remoteSim.tag == SIMID_WIRE_TAG);
			m_remoteSimVerdict = SimIdCompare( m_remoteSim );

			// GeneralsX @feature Claude 09/08/2026 Both sides of the build-kind comparison, in the
			// log the joiner actually has. The dialog can only say what the rule is; this says
			// which side broke it, and it is the one piece of information that turns "refused"
			// into "turn off your debug build".
			if (msg->GameNotJoined.buildTag == BUILDKIND_WIRE_TAG)
			{
				// stderr, NOT DEBUG_LOG. Every active release preset compiles DEBUG_LOG to
				// ((void)0), and this is the one line that turns "refused" into "turn off your
				// debug build" - so putting it behind a macro that is off in exactly the builds
				// most people run would leave the dialog promising a diagnosis nobody can find.
				// The SimID mismatch trace beside it made the same choice for the same reason.
				fprintf(stderr, "[LAN] build kind: host 0x%02x, ours 0x%02x%s\n",
					msg->GameNotJoined.buildFlags, gxLocalBuildKind(),
					msg->GameNotJoined.reason == LANAPIInterface::RET_DEBUG_PEER
						? " - and that is why we were refused" : "");
				fflush(stderr);
			}

			// GeneralsX @feature Claude 27/07/2026 We were denied by the host. Log why, but only
			// for the build-mismatch reason - this branch also carries "game full", "duplicate
			// name" and friends, and dumping a SimID table for those would be noise.
			if (msg->GameNotJoined.reason == LANAPIInterface::RET_CRC_MISMATCH)
			{
				char peerAddr[24];
				snprintf(peerAddr, sizeof(peerAddr), "%u.%u.%u.%u",
					(senderIP >> 24) & 0xFF, (senderIP >> 16) & 0xFF, (senderIP >> 8) & 0xFF, senderIP & 0xFF);
				SimIdLogMismatch("JOINER-DENIED", peerAddr, m_remoteSimVerdict, m_remoteSim);
			}

			// GeneralsX @feature Claude 08/08/2026 Lobby password: take the host's salt and
			// challenge off the refusal, then answer it.
			//
			// This is what makes the two-phase handshake invisible to the player. They type
			// /password once and press Join once; the "you need a password" refusal is consumed
			// here and turned straight into a second, answered request. Only a genuinely wrong
			// password ever reaches the dialog.
			//
			// The retry budget is 1 and is reset only by a user-initiated RequestGameJoin. A host
			// that answers every attempt with NEEDS_PASSWORD - broken, or hostile - therefore
			// costs us exactly one extra datagram, not a loop.
			if ((msg->GameNotJoined.reason == LANAPIInterface::RET_NEEDS_PASSWORD
			  || msg->GameNotJoined.reason == LANAPIInterface::RET_BAD_PASSWORD)
			 && msg->GameNotJoined.pwTag == LOBBYPW_WIRE_TAG)
			{
				m_joinPwHost      = msg->GameNotJoined.gameIP;
				m_joinPwSalt      = msg->GameNotJoined.pwSalt;
				m_joinPwChallenge = msg->GameNotJoined.pwChallenge;
				m_joinPwOffered   = (m_joinPwChallenge != 0);

				// Only NEEDS_PASSWORD is retried. BAD_PASSWORD means the password we hold is
				// wrong, and re-sending the same wrong password would just burn the new challenge
				// and produce the same refusal with a worse error message.
				//
				// GeneralsX @fix Claude 10/08/2026 (cross-model review) Budget raised 1 -> 3. The
				// 1 Hz join-request reassert means the host can see MORE THAN ONE password-free
				// knock and answer each with NEEDS_PASSWORD + a fresh challenge. With a budget of
				// 1, the deny for the SECOND knock arrived after the budget was spent, was treated
				// as terminal, and aborted a join whose answered request was still in flight - the
				// host then seated a joiner that had already given up. Three answered refusals
				// absorb the reassert overlap and a lost answer; a hostile always-NEEDS_PASSWORD
				// host now costs three datagrams instead of one, and is then terminal as before.
				if (msg->GameNotJoined.reason == LANAPIInterface::RET_NEEDS_PASSWORD
					&& m_joinPwOffered && !m_joinPassword.isEmpty() && m_joinPwAutoRetries < 3)
				{
					++m_joinPwAutoRetries;
					DEBUG_LOG(("LANAPI::handleJoinDeny - host wants a password; answering the challenge"));

					// Unicast to the host that refused us rather than re-broadcasting: it is the
					// only peer that can act on it, and the challenge is worthless to anyone else.
					sendJoinRequest( msg->GameNotJoined.gameIP, senderIP );

					// Stay in ACT_JOIN and give the second round trip a fresh deadline, otherwise
					// a join that had already spent most of its timeout on the first knock would
					// report RET_TIMEOUT while the answer was still in flight.
					m_expiration = timeGetTime() + m_actionTimeout;
					return;
				}
			}

			// GeneralsX @feature Claude 10/08/2026 The host answered no. Stop re-knocking - each
			// further knock would be re-denied, and on a password game each re-deny mints a fresh
			// challenge into the 8-deep ring, evicting other joiners' live challenges.
			m_lastJoinRequestValid = FALSE;

			// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
			OnGameJoin(msg->GameNotJoined.reason, LookupGame(UnicodeString(GetWindowsWideCharFieldAsWchar(msg->GameNotJoined.gameName))));
			m_pendingAction = ACT_NONE;
			m_expiration = 0;
		}
	}
}

void LANAPI::handleRequestGameLeave( LANMessage *msg, UnsignedInt senderIP )
{
	if (!m_inLobby && m_currentGame && !m_currentGame->isGameInProgress())
	{
		int player;
		for (player = 0; player < MAX_SLOTS; ++player)
		{
			if (m_currentGame->getIP(player) == senderIP)
			{
				if (player == 0)
				{
					// GeneralsX @bugfix Claude 12/08/2026 The host said goodbye - stop showing its game.
					//
					// This is the EXPLICIT half of "the host is gone". The other half, the host that
					// crashed or lost its link, is caught by the staleness sweep in LANAPI::update();
					// both have to suppress or the browser is only clean on whichever path was taken.
					// This one usually needs no help - a host that leaves deliberately sends its own
					// GXUNAD and the relay delists at once - but that is the host's datagram, sent on
					// the host's socket, and it is not guaranteed to arrive. When it is lost, this is
					// the only evidence THIS client has that the game is dead, and it is direct.
					if (m_transport != NULL && m_transport->isRelayEnabled())
						Transport::suppressRoomListing(m_transport->getRelayRoom());

					OnHostLeave();
					removeGame(m_currentGame);
					delete m_currentGame;
					m_currentGame = nullptr;

					/// @todo re-add myself to lobby?  Or just keep me there all the time?  If we send a LOBBY_ANNOUNCE things'll work out...
					LANPlayer *lanPlayer = LookupPlayer(m_localIP);
					if (!lanPlayer)
					{
						lanPlayer = NEW LANPlayer;
						lanPlayer->setIP(m_localIP);
					}
					else
					{
						removePlayer(lanPlayer);
					}
					lanPlayer->setName(UnicodeString(m_name));
					lanPlayer->setHost(m_hostName);
					lanPlayer->setLogin(m_userName);
					lanPlayer->setLastHeard(timeGetTime());
					addPlayer(lanPlayer);

				}
				else
				{
					if (AmIHost())
					{
						// remove the deadbeat
						LANGameSlot slot;
						slot.setState(SLOT_OPEN);
						m_currentGame->setSlot( player, slot );
					}
					// GeneralsX @security Claude 06/10/2026 A remote leave is never ours (update() drops our own
					// datagrams), but OnPlayerLeave reads our own name as "we are leaving" - and the name is the
					// packet's, so a seated peer naming the host made the host walk out of its own game (Codex
					// net-hardening r1). Its other branch does not use the name; a leave that names us is simply not
					// passed on (the options sent below still tell the room). The packet's name stays otherwise: a
					// guest's /nick does not reach the host's copy of its slot (Codex r1 on the slot-name version).
					// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
					const UnicodeString leaverName(GetWindowsWideCharFieldAsWchar(msg->name));
					if (m_name.compare(leaverName) != 0)
					{
						OnPlayerLeave(leaverName);
					}
					else if (AmIHost())
					{
						// What OnPlayerLeave does for anybody else's leave (LANAPICallbacks.cpp): the room gets a
						// fresh slot list now (Codex r2 - an honest guest renamed to our name must still be seen to go).
						m_lastResendTime = 0;
						lanUpdateSlotList();
						RequestGameOptions( GenerateGameOptionsString(), true );
					}
					m_currentGame->getLANSlot(player)->setState(SLOT_OPEN);
					m_currentGame->resetAccepted();
					RequestGameOptions(GenerateGameOptionsString(), false, senderIP);
					//m_currentGame->endGame();
				}
				break;
			}
			DEBUG_ASSERTCRASH(player < MAX_SLOTS, ("Didn't find player!"));
		}
	}
	else if (m_inLobby)
	{
		// Look for dissappearing games
		LANGameInfo *game = m_games;
		while (game)
		{
			// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
			// GeneralsX @security Claude 06/10/2026 Only the game's own host takes its listing down; any peer could
			// delete any row by naming it, then announce that name itself (Codex net-hardening r1).
			if (game->getName().compare(GetWindowsWideCharFieldAsWchar(msg->GameToLeave.gameName)) == 0
				&& game->getIP(0) == senderIP)
			{
				removeGame(game);
				delete game;
				OnGameList(m_games);
				break;
			}
			game = game->getNext();
		}
	}
}

void LANAPI::handleRequestLobbyLeave( LANMessage *msg, UnsignedInt senderIP )
{
	if (m_inLobby)
	{
		LANPlayer *player = m_lobbyPlayers;
		while (player)
		{
			if (player->getIP() == senderIP)
			{
				removePlayer(player);
				delete player;
				OnPlayerList(m_lobbyPlayers);
				break;
			}
			player = player->getNext();
		}
	}
}

void LANAPI::handleSetAccept( LANMessage *msg, UnsignedInt senderIP )
{
	if (!m_inLobby && m_currentGame && !m_currentGame->isGameInProgress())
	{
		int player;
		for (player = 0; player < MAX_SLOTS; ++player)
		{
			if (m_currentGame->getIP(player) == senderIP)
			{
				OnAccept(senderIP, msg->Accept.isAccepted);
				break;
			}
		}
	}
}

void LANAPI::handleHasMap( LANMessage *msg, UnsignedInt senderIP )
{
	if (!m_inLobby && m_currentGame)
	{
		CRC mapNameCRC;
//	mapNameCRC.computeCRC(m_currentGame->getMap().str(), m_currentGame->getMap().getLength());
		AsciiString portableMapName = TheGameState->realMapPathToPortableMapPath(m_currentGame->getMap());
		mapNameCRC.computeCRC(portableMapName.str(), portableMapName.getLength());
		if (mapNameCRC.get() != msg->MapStatus.mapCRC)
		{
			// GeneralsX @feature Claude 11/08/2026 This drop used to be SILENT, and it is the
			// leading suspect for "the announce arrived and the host still started alone": the
			// sender computes its CRC over a portable name for a file it may not HAVE, and the
			// conversion's documented fallback ships the raw path. A refused hasMap=false leaves
			// the slot at its default TRUE. Release-visible because the whole failure class is
			// invisible without it.
			fprintf(stderr, "[LAN] DROPPING map availability from %d.%d.%d.%d: their mapCRC=%08X"
				" ours=%08X (our portable='%s') hasMap=%d\n",
				PRINTF_IP_AS_4_INTS(senderIP), msg->MapStatus.mapCRC, mapNameCRC.get(),
				portableMapName.str(), (int)msg->MapStatus.hasMap);
			fflush(stderr);
			return;
		}

		int player;
		for (player = 0; player < MAX_SLOTS; ++player)
		{
			if (m_currentGame->getIP(player) == senderIP)
			{
				OnHasMap(senderIP, msg->MapStatus.hasMap);
				break;
			}
		}
	}
}

void LANAPI::handleChat( LANMessage *msg, UnsignedInt senderIP )
{
	if (m_inLobby)
	{
		LANPlayer *player;
		if((player=LookupPlayer(senderIP)) != nullptr)
		{
			// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
			OnChat(UnicodeString(player->getName()), player->getIP(), UnicodeString(GetWindowsWideCharFieldAsWchar(msg->Chat.message)), msg->Chat.chatType);
			player->setLastHeard(timeGetTime());
		}
	}
	else
	{
		// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
		if (LookupGame(UnicodeString(GetWindowsWideCharFieldAsWchar(msg->Chat.gameName))) != m_currentGame)
		{
			DEBUG_LOG(("Game '%ls' is not my game", msg->Chat.gameName));
			if (m_currentGame)
			{
				DEBUG_LOG(("Current game is '%ls'", m_currentGame->getName().str()));
			}
			return;
		}

		int player;
		for (player = 0; player < MAX_SLOTS; ++player)
		{
			if (m_currentGame && m_currentGame->getIP(player) == senderIP)
			{
				// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
				OnChat(UnicodeString(GetWindowsWideCharFieldAsWchar(msg->name)), m_currentGame->getIP(player), UnicodeString(GetWindowsWideCharFieldAsWchar(msg->Chat.message)), msg->Chat.chatType);
				break;
			}
		}
	}
}

void LANAPI::handleGameStart( LANMessage *msg, UnsignedInt senderIP )
{
	// GeneralsX @tweak Claude 11/08/2026 Deterministic loss forcing for the netem harness. The
	// natural rate of "both immediate start copies lost" is p-squared per start - too rare to
	// force economically with netem (15/15 baseline runs passed while the round-2 class sat at
	// ~1 in 8) - so the harness drops the first N starts HERE and the recovery machinery has to
	// prove itself against a certainty instead of a lottery ticket. Env-gated, zero cost off.
	static Int s_dropStarts = getenv("GX_TEST_DROP_STARTS") ? atoi(getenv("GX_TEST_DROP_STARTS")) : 0;
	if (s_dropStarts > 0)
	{
		--s_dropStarts;
		fprintf(stderr, "[LAN] TEST dropped Game Start (%d more to drop)\n", s_dropStarts);
		fflush(stderr);
		return;
	}

	// GeneralsX @bugfix Claude 11/08/2026 Nonce dedupe FIRST, unconditionally. The legacy
	// duplicate gate below (!isGameInProgress) is disarmed exactly where duplicates are most
	// dangerous: after a transfer-only round restores the lobby, and during the
	// drain-before-teardown epilogue. A re-sent start we already ran must be inert at ANY time -
	// that is what makes the host's delivery-window resends and the longer launch burst safe to
	// send at all.
	//
	// SERIAL ordering, not equality (cross-model review, P2): with consecutive transfer-only
	// rounds a delayed copy of start N can land after start N+1's round restored the lobby, and
	// an equality test would call N "new" and start the peers without the host. Anything at or
	// before the newest processed nonce is stale. Serial arithmetic so a (practically
	// unreachable) counter wrap still compares correctly; m_lastStartNonceProcessed resets to 0
	// on room entry, and hosts never issue nonce 0.
	const Bool nonced = (msg->GameStart.nonceTag == STARTNONCE_WIRE_TAG && msg->GameStart.nonce != 0);
	if (nonced && m_lastStartNonceProcessed != 0
		&& (Int)(msg->GameStart.nonce - m_lastStartNonceProcessed) <= 0)
		return;

	if (!m_inLobby && m_currentGame && m_currentGame->getIP(0) == senderIP && !m_currentGame->isGameInProgress())
	{
		if (nonced)
			m_lastStartNonceProcessed = msg->GameStart.nonce;

		// GeneralsX @feature Claude 24/08/2026 Adopt the host's camera height limits, and do it
		// HERE - after the nonce gate, so a re-sent start cannot re-latch, and before OnGameStart,
		// which is where the match begins and after which the seam refuses to change the pair.
		//
		// THE else CLEAR IS THE POINT, not tidiness. This message is authoritative in BOTH
		// directions: a host that says nothing must leave us on our own limits, even if we
		// adopted a pair from an earlier lobby's JOIN_ACCEPT. Without it, 'joined a host that
		// sets limits, left, joined one that does not' would silently keep the first host's
		// ceiling - a stale-state bug that would only ever show up on somebody else's machine.
		{
			Real hostCamMin = 0.0f;
			Real hostCamMax = 0.0f;
			if (GXHostCamWireRead(msg->GameStart.cam, &hostCamMin, &hostCamMax))
				GXHostCamera::setHostLimits(hostCamMin, hostCamMax);
			else
				GXHostCamera::clearHostLimits();
		}

		// GeneralsX @security Claude 25/09/2026 Adopt the host's relay-proven admin slots and keys from THIS message, for the
		// reason the camera pair is adopted here: every peer starts on the one mask the host stamped, never on a
		// lobby options string that may be stale. Adopting is not trusting - OnGameStart refuses to start unless a
		// fresh answer from our own relay says exactly this. An unstamped arm reads as no admin at all.
		m_gxStartAdminUnconfirmed = GXAdminWireRead(msg->GameStart.admin, *m_currentGame);

		OnGameStart();
	}
}

void LANAPI::handleGameStartTimer( LANMessage *msg, UnsignedInt senderIP )
{
	if (!m_inLobby && m_currentGame && m_currentGame->getIP(0) == senderIP && !m_currentGame->isGameInProgress())
	{
		OnGameStartTimer(msg->StartTimer.seconds);
	}
}

void LANAPI::handleGameOptions( LANMessage *msg, UnsignedInt senderIP )
{
	if (!m_inLobby && m_currentGame && !m_currentGame->isGameInProgress())
	{
		int player;
		for (player = 0; player < MAX_SLOTS; ++player)
		{
			if (m_currentGame->getIP(player) == senderIP)
			{
				OnGameOptions(senderIP, player, AsciiString(msg->GameOptions.options));
				break;
			}
		}
	}
}

void LANAPI::handleInActive(LANMessage *msg, UnsignedInt senderIP) {
	if (m_inLobby || (m_currentGame == nullptr) || (m_currentGame->isGameInProgress())) {
		return;
	}

	// check to see if we are the host of this game.
	if (m_currentGame->amIHost() == FALSE) {
		return;
	}

	UnicodeString playerName;
	// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
	playerName = UnicodeString(GetWindowsWideCharFieldAsWchar(msg->name));

	Int slotNum = m_currentGame->getSlotNum(playerName);
	if (slotNum < 0)
		return;
	GameSlot *slot = m_currentGame->getSlot(slotNum);
	if (slot == nullptr) {
		return;
	}

	if (senderIP != slot->getIP()) {
		return;
	}

	// don't want to unaccept the host, that's silly.  They can't hit start alt-tabbed anyways.
	if (senderIP == GetLocalIP()) {
		return;
	}

	// only unaccept if the timer hasn't started yet.
	if (m_gameStartTime != 0) {
		return;
	}

	slot->unAccept();
	AsciiString options = GenerateGameOptionsString();
	RequestGameOptions(options, FALSE);
	lanUpdateSlotList();
}
