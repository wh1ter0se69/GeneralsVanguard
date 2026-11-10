/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 GeneralsX contributors
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

// GeneralsX: Headless LAN host/join driver. See HeadlessMatch.h.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/HeadlessMatch.h"

#include "Common/GameEngine.h"
#include "Common/FramePacer.h"
#include "Common/GlobalData.h"
#include "Common/PlayerTemplate.h"
#include "Common/PlayerList.h"
#include "Common/Player.h"
#include "Common/ThingTemplate.h"
#include "Common/ThingFactory.h"
#include "Common/Recorder.h"
#include "Common/MessageStream.h"	// TheMessageStream, for the final flush
#include "GameLogic/Object.h"
#include "GameClient/GameClient.h"
#include "GameClient/MapUtil.h"	// TheMapCache
#include "GameClient/TerrainVisual.h"	// isEveryBridgeModelAvailable - the network refusal below
#include "GameClient/View.h"	// TheTacticalView, for the GX_KILLTMPL camera
#include "GameClient/GXIdentityPush.h"	// gxDeclareIdentityIfChanged - see the lobby pump below
#include "GameLogic/GameLogic.h"
#include "GameNetwork/GameInfo.h"
#include "GameNetwork/IPEnumeration.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameNetwork/LANGameInfo.h"
#include "GameNetwork/NetworkDefs.h"
#include "GameNetwork/NetworkInterface.h"	// TheNetwork, for the live desync flag
#include "GameNetwork/Transport.h"			// the relay game browser
#include "Common/UserPreferences.h"	// LANPreferences
#include "Common/OptionPreferences.h"	// relay settings, read from Options.ini
#include "GameNetwork/GUIUtil.h"			// EnableSlotListUpdates

// ------------------------------------------------------------------------------------------
// THE TRAP THAT DEFINES THIS FILE
//
// LANAPI::update() begins with `if (LANbuttonPushed) return;`, and LANAPI's own callbacks
// SET that flag: OnGameCreate (LANAPICallbacks.cpp:699) and OnGameJoin (:511) both do
// `LANbuttonPushed = true` and then `TheShell->push("Menus/LanGameOptionsMenu.wnd")`. The ONLY
// place that clears it again is LanLobbyMenuInit (LanLobbyMenu.cpp:474).
//
// So a headless driver that reuses stock LANAPI deadlocks the instant its game is created or
// joined: the pump is disabled for the rest of the process and nothing will ever re-enable it.
// The symptom is a host that appears to start fine and then never hears a single peer.
//
// Rather than clear the flag behind LANAPI's back - which would race the real menus if a UI
// build ever reached this path - the headless role gets its own subclass whose callbacks touch
// neither the flag nor TheShell. Everything that is UI-free (notably OnGameStart, which builds
// TheNetwork, parses the user list, posts MSG_NEW_GAME and seeds the RNG) still calls the base
// implementation, so the headless lobby and the real lobby converge on identical simulation
// state. That equality is the whole point: a headless peer must be indistinguishable from a
// human-driven one, or it cannot be used to prove determinism.
// ------------------------------------------------------------------------------------------

namespace
{

/// Set once OnGameStart has run, so the driver knows to leave the lobby loop.
Bool s_gameStarted = FALSE;
/// Starts that only delivered the map and put the lobby back (owner's two-start flow). The
/// driver answers each one with a second RequestGameStart once every slot shows the map.
Int s_transferOnlyStarts = 0;
/// How many of those second starts the driver has already issued.
Int s_restartsIssued = 0;
/// Set when the local create/join succeeded.
Bool s_lobbyReady = FALSE;
/// Set when the create/join failed; the driver aborts rather than spinning to a timeout.
Bool s_lobbyFailed = FALSE;
AsciiString s_lobbyFailureReason;

void headlessLog(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	fprintf(stderr, "[GXLAN] ");
	vfprintf(stderr, fmt, args);
	fprintf(stderr, "\n");
	va_end(args);
	fflush(stderr);
}

class HeadlessLANAPI : public LANAPI
{
public:

	// --- Callbacks that stock LANAPI implements by driving the shell ------------------------

	/// LANAPI hardcodes m_actionTimeout to 5000 ms, which is a LAN figure. A direct-connect join
	/// across the internet needs several exchanges - Request GameInfo, Game Announce, Request
	/// Join, Join Accept - each gated by LANAPI::update()'s own 200 ms throttle, so on a WAN link
	/// the Join Accept routinely arrives AFTER the pending action has already expired into
	/// RET_TIMEOUT. The packets are fine; the deadline is not.
	///
	/// The failure is deeply misleading: the host logs "player joined slot 1" and the joiner logs
	/// receiving Join Accept, yet the joiner still reports "join failed". Nothing looks dropped
	/// because nothing was dropped.
	void setActionTimeout(UnsignedInt ms) { m_actionTimeout = ms; }

	virtual void OnGameCreate( ReturnType ret ) override
	{
		if (ret == RET_OK)
		{
			headlessLog("hosted game created");
			s_lobbyReady = TRUE;
			// Stock OnGameCreate does this after pushing the menu. It is not cosmetic: staying
			// registered in the lobby keeps us answering lobby traffic we no longer belong to.
			RequestLobbyLeave( false );
		}
		else
		{
			s_lobbyFailed = TRUE;
			s_lobbyFailureReason.format("game create failed, code %d", (Int)ret);
			headlessLog("%s", s_lobbyFailureReason.str());
		}
	}

	virtual void OnGameJoin( ReturnType ret, LANGameInfo *theGame ) override
	{
		if (ret == RET_OK)
		{
			headlessLog("joined game");
			s_lobbyReady = TRUE;

			// Mirror the option burst stock OnGameJoin sends. The host's slot list is built
			// from these, so omitting them leaves the joiner without a faction or colour and
			// the two peers disagree about the player set before the match even starts.
			LANPreferences pref;
			AsciiString options;
			// GeneralsX: -lanobserver takes an OBSERVER seat, which the preferred
			// faction never can (LANPreferences refuses it). The host clears an observer's colour, start
			// and team itself (LANAPICallbacks, PlayerTemplate), so an observer sends no colour.
			if (TheGlobalData->m_lanObserver)
			{
				options.format("PlayerTemplate=%d", (Int)PLAYERTEMPLATE_OBSERVER);
				RequestGameOptions(options, true);
				headlessLog("taking an observer seat");
			}
			else
			{
				options.format("PlayerTemplate=%d", pref.getPreferredFaction());
				RequestGameOptions(options, true);
				options.format("Color=%d", pref.getPreferredColor());
				RequestGameOptions(options, true);
			}
			options.format("NAT=%d", FirewallHelperClass::FIREWALL_TYPE_SIMPLE);
			RequestGameOptions(options, true);
		}
		else
		{
			s_lobbyFailed = TRUE;
			// RET_CRC_MISMATCH here is a SimID mismatch, i.e. the two builds are not the same
			// build. That is by far the most common headless join failure and it is worth
			// naming explicitly, because "join failed" sends people hunting for a network fault.
			if (ret == RET_CRC_MISMATCH)
				s_lobbyFailureReason = "join refused: build identity mismatch (SimID) - rebuild both peers from the same commit";
			else
				s_lobbyFailureReason.format("join failed, code %d", (Int)ret);
			headlessLog("%s", s_lobbyFailureReason.str());
		}
	}

	virtual void OnGameStart() override
	{
		// Base implementation is UI-free and does all the work that matters: creates
		// TheNetwork, parses the user list, sets m_pendingFile, posts MSG_NEW_GAME and calls
		// InitRandom with the shared seed. Do NOT reimplement it - the seeding in particular
		// is what keeps the peers in lockstep.
		LANAPI::OnGameStart();

		// The base implementation has a failure path that returns without starting anything:
		// if the map is missing or DoAnyMapTransfers fails it calls OnPlayerLeave, tears down
		// TheNetwork, clears m_currentGame and returns. Reporting "started" there sends the
		// driver into the match loop with no game at all, which segfaults on the first frame.
		// m_currentGame going null is the signal, so check it rather than assuming success.
		if (GetMyGame() == nullptr)
		{
			s_lobbyFailed = TRUE;
			s_lobbyFailureReason = "game did not start - the map is missing or could not be transferred, or the host claimed an admin the relay does not confirm ([GXADMIN])";
			headlessLog("%s", s_lobbyFailureReason.str());
			return;
		}

		// GeneralsX: A start can now legitimately end back in the room:
		// the first start of a lobby with a map-less peer only DELIVERS the map and restores the
		// lobby (see LANAPI::OnGameStart). The game is genuinely started only once startGame ran,
		// which is what isGameInProgress reports. The driver emulates the human's second click
		// from driveLobby once every slot shows the map.
		if (!GetMyGame()->isGameInProgress())
		{
			++s_transferOnlyStarts;
			headlessLog("start round %d only transferred the map; lobby restored", s_transferOnlyStarts);
			return;
		}

		s_gameStarted = TRUE;
		headlessLog("game starting");
	}

	// --- Callbacks that only paint the lobby: reduce to logging ----------------------------

	virtual void OnGameList( LANGameInfo * ) override {}
	virtual void OnPlayerList( LANPlayer * ) override {}
	virtual void OnNameChange( UnsignedInt, UnicodeString ) override {}
	virtual void OnGameStartTimer( Int ) override {}

	// CAUTION: the three callbacks below are NOT UI. LANAPI mixes lobby state mutation into its
	// On* callbacks, so overriding them without delegating silently breaks the host's
	// bookkeeping. That is not a theoretical risk - log-only versions of OnAccept and OnHasMap
	// made the host ignore a peer that had joined, had the map and had accepted, and it then
	// waited out its full timeout. Only override what touches TheShell or LANbuttonPushed.

	virtual void OnPlayerJoin( Int slot, UnicodeString playerName ) override
	{
		headlessLog("player joined slot %d", slot);
		// Base calls resetAccepted() and re-broadcasts the options. Skipping it leaves peers
		// holding a stale slot list.
		LANAPI::OnPlayerJoin(slot, playerName);
	}

	virtual void OnPlayerLeave( UnicodeString player ) override
	{
		headlessLog("a player left");
		// Delegate ONLY when the leaver is not us. The base's "we are leaving" branch sets
		// LANbuttonPushed and pops the shell; its other branch forces the slot-list resend the
		// host needs when a peer drops.
		if (GetMyName().compare(player) != 0)
			LANAPI::OnPlayerLeave(player);
	}

	virtual void OnHostLeave() override
	{
		headlessLog("host left");
		s_lobbyFailed = TRUE;
		s_lobbyFailureReason = "host left the game";
	}

	virtual void OnAccept( UnsignedInt playerIP, Bool status ) override
	{
		headlessLog("accept from %d.%d.%d.%d = %d", PRINTF_IP_AS_4_INTS(playerIP), (Int)status);
		// This is where setAccept()/unAccept() actually happens - handleSetAccept only calls
		// through to here. Without this the host never registers that anyone is ready.
		LANAPI::OnAccept(playerIP, status);
	}

	virtual void OnHasMap( UnsignedInt playerIP, Bool status ) override
	{
		if (!status)
			headlessLog("peer %d.%d.%d.%d does NOT have the map", PRINTF_IP_AS_4_INTS(playerIP));
		// This is where setMapAvailability() happens.
		LANAPI::OnHasMap(playerIP, status);
	}

	virtual void OnChat( UnicodeString player, UnsignedInt ip,
											 UnicodeString message, ChatType format ) override
	{
		// System messages carry the useful diagnostics (map transfer failures, host timeouts).
		AsciiString narrow;
		narrow.translate(message);
		headlessLog("chat: %s", narrow.str());
	}

	virtual void OnGameOptions( UnsignedInt playerIP, Int playerSlot, AsciiString options ) override
	{
		// The base implementation parses the slot list, which the joiner genuinely needs; its
		// only UI calls are lanUpdateSlotList()/updateGameOptions(), and both bail immediately
		// when slot list updates are disabled. setUpLan() disables them for the whole headless
		// run, so this is safe and we keep the real parsing rather than duplicating it.
		LANAPI::OnGameOptions(playerIP, playerSlot, options);
	}
};

// ------------------------------------------------------------------------------------------
// GeneralsX @feature Matchmaking. Which relay room this process belongs in.
//
// A host gets a room of its own (Transport::getLocalHostRoom), because the relay lists exactly one
// game per room and two hosts sharing one would overwrite each other's listing. A joiner has to
// end up in the room of the game it wants, which it does not know from -lanjoin alone: addresses
// are allocated per room, so every host on the relay is 10.42.0.1 and the address identifies
// nothing on its own.
//
// So the joiner browses first, exactly as the UI's game list does, and takes the room off the
// listing whose host address matches. When that is ambiguous or the game is not listed at all -
// a private game never advertises - the room has to be given explicitly:
//
//   GENERALSX_LANROOM=<token>    environment, highest precedence, either role
//   RelayRoom=<token>            Options.ini, the existing explicit setting
//
// The pin is an environment variable rather than a -lanroom flag ONLY because adding a flag means
// editing CommandLine.cpp and GlobalData, which are outside the file set this change is allowed to
// touch. It is read at exactly one place and behaves like a flag in every other respect.
// ------------------------------------------------------------------------------------------

/// An explicitly pinned room, or empty. Environment first, then Options.ini.
AsciiString pinnedRelayRoom()
{
	const char *env = getenv("GENERALSX_LANROOM");
	if (env != nullptr && *env != '\0')
		return AsciiString(env);

	OptionPreferences prefs;
	return prefs.getRelayRoom();
}

/// The room to enter, or empty if a joiner could not work out which game was meant.
AsciiString chooseRelayRoom(const AsciiString &relayHost)
{
	const AsciiString pinned = pinnedRelayRoom();
	if (!pinned.isEmpty())
	{
		headlessLog("relay room pinned to '%s'", pinned.str());
		return pinned;
	}

	if (TheGlobalData->m_lanRole != LANROLE_JOIN)
	{
		// Hosting, or -lanlist, which needs a room only so the relay has somewhere to put it.
		return Transport::getLocalHostRoom();
	}

	// Browsing, over a throwaway socket, before any transport exists - see the note on
	// Transport::requestRelayGameList. Retried because a host publishes its advertisement on its
	// 5 s registration keepalive, so a joiner that starts alongside its host can easily ask before
	// the first one has gone out.
	const UnsignedInt target = TheGlobalData->m_lanJoinIP;
	for (Int attempt = 0; attempt < 4; ++attempt)
	{
		Transport::RelayGameListing games[Transport::MAX_RELAY_LISTINGS];
		const Int count = Transport::requestRelayGameList(relayHost.str(), games,
			Transport::MAX_RELAY_LISTINGS, 2000);

		Int matches = 0;
		Int firstMatch = -1;
		for (Int i = 0; i < count; ++i)
		{
			if (games[i].hostVirtualIP != target)
				continue;
			if (firstMatch < 0)
				firstMatch = i;
			++matches;
		}

		if (matches == 1)
		{
			headlessLog("game at %d.%d.%d.%d is in room '%s' (\"%s\" on %s)",
				PRINTF_IP_AS_4_INTS(target), games[firstMatch].room,
				games[firstMatch].name, games[firstMatch].map);
			return AsciiString(games[firstMatch].room);
		}

		if (matches > 1)
		{
			// Not recoverable by waiting: the address genuinely does not distinguish them.
			headlessLog("%d listed games have host %d.%d.%d.%d - set GENERALSX_LANROOM to pick one:",
				matches, PRINTF_IP_AS_4_INTS(target));
			for (Int i = 0; i < count; ++i)
			{
				if (games[i].hostVirtualIP == target)
					headlessLog("  room=%s name=\"%s\" map=\"%s\"",
						games[i].room, games[i].name, games[i].map);
			}
			return AsciiString();
		}

		headlessLog("no listed game at %d.%d.%d.%d yet (%d listed); retrying",
			PRINTF_IP_AS_4_INTS(target), count);
	}

	headlessLog("nothing on the relay advertises %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(target));
	headlessLog("a private game never advertises - join it with GENERALSX_LANROOM=<room>");
	return AsciiString();
}

/// Throttle for the joiner's re-accept, so the 10 ms pump does not flood the lobby.
UnsignedInt s_lastAcceptSent = 0;

/// Is OUR OWN slot currently marked accepted?
Bool localSlotAccepted()
{
	LANGameInfo *game = TheLAN ? TheLAN->GetMyGame() : nullptr;
	if (!game)
		return TRUE;	// not in a game yet - nothing to assert

	const Int slotNum = game->getLocalSlotNum();
	if (slotNum < 0)
		return TRUE;	// we are not in the slot list yet

	GameSlot *slot = game->getSlot(slotNum);
	return (slot == nullptr) || slot->isAccepted();
}

/// Re-assert our accept if the host has cleared it.
///
/// LANAPI::OnPlayerJoin calls resetAccepted() and re-broadcasts the slot list, on EVERY join. So a
/// peer that accepted once is silently un-accepted the moment the NEXT peer arrives, and the host
/// then waits out its entire timeout for a player that believes it is ready. In a UI lobby a human
/// sees the tick vanish and clicks again; a headless peer accepted once and never looked back.
///
/// This could not show up with a single joiner - nothing ever followed its accept - which is why
/// it survived until a second one joined. Joiners do not even receive OnPlayerJoin, so this is
/// driven off our own slot state rather than off an event: whatever clears the flag, we notice.
void reassertAcceptIfNeeded()
{
	if (localSlotAccepted())
		return;

	const UnsignedInt now = timeGetTime();
	if ((now - s_lastAcceptSent) < 250)
		return;

	s_lastAcceptSent = now;
	TheLAN->RequestAccept();
}

/// How many peers the host waits for, and how many are currently accepted.
Int countAcceptedPeers()
{
	LANGameInfo *game = TheLAN ? TheLAN->GetMyGame() : nullptr;
	if (!game)
		return 0;

	Int accepted = 0;
	for (Int i = 1; i < MAX_SLOTS; ++i)
	{
		GameSlot *slot = game->getSlot(i);
		if (slot && slot->isHuman() && slot->isAccepted())
			++accepted;
	}
	return accepted;
}

/// GeneralsX: How many slots will become real players: occupied and not
/// an observer. This is deliberately the same loop the LAN lobby UI runs before it lets the host
/// press Start (LanGameOptionsMenu.cpp StartPressed) - the headless driver and the GUI are supposed
/// to accept and refuse exactly the same lobbies, and until now only the GUI checked.
///
/// Note isOccupied() is FALSE for SLOT_OPEN, so this counts a peer only once it has actually
/// joined. Calling it before the peers arrive counts intent, not reality; see publishGameOptions.
Int countOccupiedNonObserverSlots(GameInfo *game)
{
	if (!game)
		return 0;

	Int occupied = 0;
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		GameSlot *slot = game->getSlot(i);
		if (slot && slot->isOccupied() && slot->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER)
			++occupied;
	}
	return occupied;
}

// The template index of a PLAYABLE faction by name, or -1 (logged). A known name is not the same as a playable one: the store
// also holds observer and campaign-only templates with no starting building, and a valid index bypasses the random-faction
// validation GameLogic would otherwise apply. The lobby screens on the same property, so an unplayable pin fails here instead
// of producing a broken match that still reports success. (Was inline in -lanai's faction pin; shared with GX_HOST_FACTION.)
static Int playableFaction(const AsciiString &faction, const char *where)
{
	const Int ptIdx = ThePlayerTemplateStore->getTemplateNumByName(faction);
	if (ptIdx < 0)
	{
		headlessLog("unknown faction '%s' in %s", faction.str(), where);
		return -1;
	}
	const PlayerTemplate *pt = ThePlayerTemplateStore->getNthPlayerTemplate(ptIdx);
	if (pt == nullptr || pt->getStartingBuilding().isEmpty())
	{
		headlessLog("faction '%s' is not playable (no starting building)", faction.str());
		return -1;
	}
	return ptIdx;
}

/// GeneralsX: Refuse a lobby holding more players than the map has start
/// positions. This is a GUARD ONLY: it never reassigns or clamps anything, because start positions
/// decide where every player's opening units are placed and are therefore part of the frame-0 world
/// CRC. Moving them would invalidate every .crc baseline and, since this
/// engine defaults RETAIL_COMPATIBLE_CRC to 1, retail 1.04 replay compatibility with it.
///
/// What it prevents: populateRandomStartPosition caps positions at md->m_numPlayers
/// (GameLogic.cpp:882-885) and pre-marks every index past it as taken (:928). A surplus player
/// therefore finds no free position, keeps startPos=-1, gets no Command Center, and its skirmish AI
/// returns early from adjustBuildList and owns nothing for the whole match - while the run still
/// reports the full AI count and passes. Every "8-player" result in this project before this guard
/// was an overfilled 2-player map.
///
/// Fails closed when the map is not in the cache, matching the GUI. Every hosting harness in
/// scripts/ passes -lanmap, which is pre-flighted against the cache earlier in publishGameOptions.
Bool lobbyFitsTheMap(GameInfo *game, Int occupied, const char *when)
{
	if (!game)
		return FALSE;

	const MapMetaData *md = TheMapCache->findMap(game->getMap());
	if (md == nullptr)
	{
		headlessLog("capacity check (%s): map \"%s\" is not in the cache, so the lobby cannot be bounded - refusing",
			when, game->getMap().str());
		return FALSE;
	}

	// GeneralsX: The same side/player capacity the GUI lobbies and the start
	// check use (Common/PlayerCapacity.h): the civilian sides the map keeps plus neutral and the
	// ReplayObserver share MAX_PLAYER_COUNT with the slots. Checked first - it is the one the start
	// refuses on every peer. -lanoverfill bypasses it too, so the start-time refusal can be exercised.
	const Int slotCapacity = TheMapCache->getPlayerSlotCapacity(game->getMap());
	const Int slotsTakingPlayers = game->countSlotsTakingPlayers();
	if (slotCapacity >= 0 && slotsTakingPlayers > slotCapacity)
	{
		if (TheGlobalData->m_lanOverfill)
		{
			headlessLog("WARNING: -lanoverfill: \"%s\" has room for %d slot(s) beside its kept sides, this lobby has %d (%s) - the start will be REFUSED on every peer.",
				game->getMap().str(), slotCapacity, slotsTakingPlayers, when);
		}
		else
		{
			headlessLog("REFUSING over-capacity lobby (%s): \"%s\" has room for %d slot(s) beside the civilian sides it keeps, this lobby has %d.",
				when, game->getMap().str(), slotCapacity, slotsTakingPlayers);
			headlessLog("  Civilian sides are never dropped to make room (owner decision); use fewer slots or another map.");
			return FALSE;
		}
	}

	if (occupied <= md->m_numPlayers)
		return TRUE;

	if (TheGlobalData->m_lanOverfill)
	{
		headlessLog("WARNING: -lanoverfill: \"%s\" holds %d player(s) but this lobby has %d occupied slot(s) (%s).",
			game->getMap().str(), md->m_numPlayers, occupied, when);
		headlessLog("WARNING: %d player(s) will get startPos=-1, no Command Center, and will own nothing all match.",
			occupied - md->m_numPlayers);
		headlessLog("WARNING: any AI count this run reports is a LIE. Do not use it as simulation evidence.");
		return TRUE;
	}

	headlessLog("REFUSING over-capacity lobby (%s): \"%s\" holds %d player(s), this lobby has %d occupied slot(s).",
		when, game->getMap().str(), md->m_numPlayers, occupied);
	headlessLog("  The %d surplus player(s) would get startPos=-1, no Command Center, and their AI would own",
		occupied - md->m_numPlayers);
	headlessLog("  nothing for the whole match while this run still reported the full AI count and passed.");
	headlessLog("  Use a higher-capacity map (death valley, destruction station and twilight flame hold 8),");
	headlessLog("  or lower -lanai/-lanwait, or pass -lanoverfill to reproduce the old behaviour deliberately.");
	return FALSE;
}

} // namespace

// ------------------------------------------------------------------------------------------

Bool HeadlessMatch::isRequested()
{
	// -lanlist has no host/join role but still has to enter this driver rather than the shell.
	// Without it the browse flags parse fine, the engine boots the normal UI, and the process just
	// sits there - which reads as a hang rather than as "that flag did nothing".
	return (TheGlobalData->m_lanRole != LANROLE_NONE) || TheGlobalData->m_lanListGames;
}

template <typename Predicate>
Bool HeadlessMatch::pumpUntil(Predicate predicate, UnsignedInt timeoutMs, const char *what)
{
	const UnsignedInt deadline = timeGetTime() + timeoutMs;
	while (timeGetTime() < deadline)
	{
		if (s_lobbyFailed)
			return FALSE;
		if (predicate())
			return TRUE;

		// Nothing in a headless process pumps TheLAN. In a UI build this call lives in
		// LanLobbyMenuUpdate; GameEngine::update only pumps it while alt-tabbed out.
		TheLAN->update();

		// GeneralsX: AND THE IDENTITY, HERE, BECAUSE THIS LOOP IS THE
		// WHOLE LOBBY PHASE AND NOTHING ELSE RUNS IN IT.
		//
		// The plan for this work put the declare beside TheGXLobbyPresence->update() in the two
		// engine classes - which is right for a GUI peer and USELESS for this one. Measured
		// 20/09/2026 with a headless host against a local relay at RELAY_REQUIRE_IDENTITY=1: the
		// peer was dropped every time, and the reason is two lines above. TheGameEngine->update()
		// is called from EXACTLY ONE place in this file (runMatch's loop), and runMatch does not
		// start until driveLobby has returned. The relay's IDENT_GRACE_MS runs out here, in the
		// lobby, where the engine pump has never once been called - so the peer was swept before
		// the code that would have identified it ever executed.
		//
		// Same function as the engine pumps call, not a second copy: it early-returns unless the
		// transport is relay-enabled and the assertion has actually changed, so this costs a
		// pointer test and a 256-byte hash per 10 ms pass and sends nothing on the LAN path.
		//
		// THIS FILE IS INSIDE THE SIMULATION SOURCE DIGEST and that is a real cost, paid on
		// purpose: there is no non-digested code in this loop to hang it off, and a headless peer
		// that cannot identify is the entire defect. scripts/test/headless-identity-test.sh is the
		// arm that fails without it.
		gxDeclareIdentityIfChanged();

		// LANAPI::update self-throttles to one pass per 200ms, so a tight spin would burn a
		// core to no purpose.
		Sleep(10);
	}
	headlessLog("timed out waiting for %s after %d ms", what, (Int)timeoutMs);
	return FALSE;
}

Bool HeadlessMatch::setUpLan()
{
	// Every UI helper that LANAPI's surviving callbacks reach is gated on this. Turning it off
	// once, for the whole run, is what lets OnGameOptions keep using the real parsing path.
	EnableSlotListUpdates(FALSE);

	if (TheLAN != nullptr)
	{
		delete TheLAN;
		TheLAN = nullptr;
	}
	TheLAN = NEW HeadlessLANAPI();

	// GeneralsX @feature Relay transport. With a relay configured our identity is the VIRTUAL LAN
	// address, not a NIC address: the lobby is a two-machine LAN whose members are 10.42.0.1 and
	// 10.42.0.2 wherever the machines physically are. NetworkDirectConnectInit does exactly this
	// and skips the interface enumeration for the same reason - the virtual address is on no
	// interface, so anything that "corrects" it back to a real one puts a NAT-side address into
	// the slot list, and the peer then spends the whole match trying to reach an IP that was
	// never routable from where it is.
	//
	// Binding is already handled: Transport::init calls initRelay() and forces INADDR_ANY when
	// relay mode is on, precisely because a virtual address can never be bound.
	OptionPreferences prefs;
	const AsciiString relayHost = prefs.getRelayAddress();

	UnsignedInt ip = 0;
	AsciiString room;
	if (!relayHost.isEmpty())
	{
		// Which room, and for a joiner that means browsing first - see chooseRelayRoom.
		room = chooseRelayRoom(relayHost);
		if (room.isEmpty())
		{
			headlessLog("cannot tell which relay room to join - aborting rather than guessing");
			return FALSE;
		}

		// Ask the relay who we are before anything snapshots an identity. A relay-allocated
		// address is what lets strangers play: hand-configured ones have to be unique by human
		// agreement, and everyone who takes the default is 10.42.0.1. The identity is pinned
		// TOGETHER with the room it was allocated in, because it means nothing anywhere else.
		UnsignedInt assignedIP = 0;
		if (Transport::enterRelayRoom(relayHost.str(), room.str(), assignedIP))
		{
			ip = assignedIP;
			headlessLog("relay assigned us %d.%d.%d.%d in room %s",
				PRINTF_IP_AS_4_INTS(ip), room.str());
		}
		else
		{
			// Falling back keeps LAN play and existing hand-configured setups working. It is
			// logged loudly because two peers that both fall back to the default address will
			// collide, and that presents as a lobby that corrupts rather than one that refuses.
			ip = prefs.getLocalVirtualIP();
			if (ip != 0)
			{
				// Still pin the ROOM. Without this initRelay falls back to the room we would host
				// in, which for a joiner is not the room the game is in - so an unanswered GXWHO
				// would quietly turn a join into a game of one.
				Transport::setAssignedIdentity(room.str(), ip);
			}
			headlessLog("relay did not assign an identity; falling back to configured LocalVirtualIP %d.%d.%d.%d",
				PRINTF_IP_AS_4_INTS(ip));
		}
	}

	if (ip == 0)
	{
		// Same interface election the lobby does. LANIPAddress wins if it was set, which is the
		// escape hatch on a multi-homed box; otherwise take the first enumerated address.
		ip = TheGlobalData->m_defaultIP;
		if (!ip)
		{
			IPEnumeration ips;
			EnumeratedIP *list = ips.getAddresses();
			if (!list)
			{
				headlessLog("no local IP addresses - cannot host or join");
				return FALSE;
			}
			ip = list->getIP();
		}
	}

	// Raise the pending-action deadline before init so a WAN join is not abandoned mid-handshake.
	// LAN play is unaffected: a local join completes in well under the stock 5 s, so a larger
	// ceiling only changes how long a genuinely unreachable peer takes to give up - and -lanjoin
	// is bounded by -lantimeout anyway.
	{
		UnsignedInt actionMs = TheGlobalData->m_lanTimeoutMs;
		if (actionMs > 60000) actionMs = 60000;	// no point waiting longer than a minute
		if (actionMs < 5000)  actionMs = 5000;	// never tighter than the stock LAN value
		static_cast<HeadlessLANAPI*>(TheLAN)->setActionTimeout(actionMs);
		headlessLog("join action timeout %u ms", actionMs);
	}

	TheLAN->init();
	if (TheLAN->SetLocalIP(ip) == FALSE)
	{
		headlessLog("failed to bind local IP %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(ip));
		return FALSE;
	}
	// GeneralsX: Say "relay mode" only once the transport has turned it on.
	// This used to print before init whenever an address had been chosen, so a host whose
	// RelayAddress did not resolve (initRelay then leaves relay off) logged relay mode and ran as a
	// plain LAN host; xplat-4platform-play.sh reads this line as the engine's final word. Printed
	// BEFORE "local IP" so anything that waits for that line has already seen this one.
	if (!relayHost.isEmpty())
	{
		Transport *transport = TheLAN->getTransport();
		if (transport != nullptr && transport->isRelayEnabled())
		{
			headlessLog("relay mode: local identity is the virtual address %d.%d.%d.%d (relay %s, room %s)",
				PRINTF_IP_AS_4_INTS(transport->getLocalVirtualIP()), relayHost.str(), transport->getRelayRoom());
		}
		else
		{
			headlessLog("relay mode OFF: RelayAddress '%s' is set but the transport did not enable relay; "
				"this is a plain LAN peer at %d.%d.%d.%d",
				relayHost.str(), PRINTF_IP_AS_4_INTS(ip));
		}
	}
	headlessLog("local IP %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(ip));

	UnicodeString name = TheGlobalData->m_lanPlayerName;
	if (name.isEmpty())
	{
		LANPreferences prefs;
		name = prefs.getUserName();
	}
	name.truncateTo(g_lanPlayerNameLength);
	TheLAN->RequestSetName(name);

	return TRUE;
}

// GeneralsX: THE HOST'S STRESS BATTLEFIELD, from its own environment.
//
// This is the ONLY place any GX_STRESS* variable is read, and it runs only on the peer that
// publishes the game options - the host. Every other peer gets the settings from the host's "ST="
// field and applies them from there (GameLogic/GXStress.h), so a joiner's environment cannot change
// what its simulation does. Template NAMES are resolved to ids here, against data every peer must
// share anyway (dataID), which keeps the announce short.
//
// Anything wrong fails the host LOUDLY instead of publishing a lobby that silently does less than
// the operator asked for - the same rule as the -lanai faction pin. Unset means off.
//
//   GX_STRESS=<n>                 units per playable player (0..200)
//   GX_STRESS_UNIT=<template>     default AmericaVehicleHumvee
//   GX_STRESS_FRAME=<f>           logic frame of the spawn, default 120
//   GX_STRESS_PLACE=ring|base     ring: around the map's centroid, in contact (the old recipe, default)
//                                 base: in front of each player's own base
//   GX_STRESS_RADIUS=<r>          ring radius, default 200
//   GX_STRESS_ORDER=none|enemy|centre   attack-move policy, default none (the old recipe)
//   GX_STRESS_EVERY=<frames>      re-issue orders to idle stress units, default 300
//   GX_STRESS_BUILDINGS=<n>       structures per playable player (0..400), default 0
//   GX_STRESS_BUILDING_TYPES=a,b  default AmericaBarracks,ChinaGattlingCannon,AmericaPowerPlant,ChinaBunker
static Bool stressEnvInt(const char *name, Int def, Int *out)
{
	const char *v = getenv(name);
	if (v == nullptr || v[0] == '\0')
	{
		*out = def;
		return TRUE;
	}
	if (!gxStressParseDec(v, strlen(v), out))
	{
		headlessLog("[GXSTRESS] %s='%s' is not a whole number", name, v);
		return FALSE;
	}
	return TRUE;
}

static Bool stressEnvWord(const char *name, const char *def, const char *const words[], const char letters[], Int n, char *out)
{
	const char *v = getenv(name);
	if (v == nullptr || v[0] == '\0')
		v = def;
	const AsciiString word(v);
	for (Int i = 0; i < n; ++i)
		if (word.compareNoCase(words[i]) == 0)
		{
			*out = letters[i];
			return TRUE;
		}
	headlessLog("[GXSTRESS] %s='%s' is not one of the accepted words", name, v);
	return FALSE;
}

static const ThingTemplate *stressTemplate(const char *what, const AsciiString &name, Bool wantStructure)
{
	const ThingTemplate *tmpl = (TheThingFactory != nullptr) ? TheThingFactory->findTemplate(name, FALSE) : nullptr;
	if (tmpl == nullptr)
	{
		headlessLog("[GXSTRESS] no %s template named '%s'", what, name.str());
		return nullptr;
	}
	if (tmpl->isKindOf(KINDOF_STRUCTURE) != wantStructure)
	{
		headlessLog("[GXSTRESS] '%s' %s a structure, so it cannot be the %s template",
			name.str(), wantStructure ? "is not" : "is", what);
		return nullptr;
	}
	return tmpl;
}

static Bool hostStressFromEnv(GXStressOption *o)
{
	gxStressClear(o);
	if (getenv("GX_STRESS") == nullptr && getenv("GX_STRESS_BUILDINGS") == nullptr)
		return TRUE;

	static const char *const placeWords[] = { "ring", "base" };
	static const char placeLetters[] = { 'R', 'B' };
	static const char *const orderWords[] = { "none", "enemy", "centre", "center" };
	static const char orderLetters[] = { 'N', 'E', 'C', 'C' };
	if (!stressEnvInt("GX_STRESS", 0, &o->units) || !stressEnvInt("GX_STRESS_FRAME", 120, &o->frame)
			|| !stressEnvInt("GX_STRESS_RADIUS", 200, &o->radius) || !stressEnvInt("GX_STRESS_EVERY", 300, &o->every)
			|| !stressEnvInt("GX_STRESS_BUILDINGS", 0, &o->buildings)
			|| !stressEnvWord("GX_STRESS_PLACE", "ring", placeWords, placeLetters, 2, &o->place)
			|| !stressEnvWord("GX_STRESS_ORDER", "none", orderWords, orderLetters, 4, &o->order))
		return FALSE;
	if (o->units == 0 && o->buildings == 0)
	{
		gxStressClear(o);		// GX_STRESS=0 meant "off" before this was a lobby option; it still does
		return TRUE;
	}

	if (o->units > 0)
	{
		const char *u = getenv("GX_STRESS_UNIT");
		const ThingTemplate *tmpl = stressTemplate("unit", AsciiString((u != nullptr && u[0] != '\0') ? u : "AmericaVehicleHumvee"), FALSE);
		if (tmpl == nullptr)
			return FALSE;
		o->unitId = tmpl->getTemplateID();
	}

	if (o->buildings > 0)
	{
		const char *t = getenv("GX_STRESS_BUILDING_TYPES");
		AsciiString list((t != nullptr && t[0] != '\0') ? t : "AmericaBarracks,ChinaGattlingCannon,AmericaPowerPlant,ChinaBunker");
		AsciiString name;
		while (list.nextToken(&name, ","))
		{
			name.trim();
			if (name.isEmpty())
				continue;
			if (o->numBuildingIds == GX_STRESS_MAX_BUILDING_TYPES)
			{
				headlessLog("[GXSTRESS] GX_STRESS_BUILDING_TYPES names more than %d types", GX_STRESS_MAX_BUILDING_TYPES);
				return FALSE;
			}
			const ThingTemplate *tmpl = stressTemplate("building", name, TRUE);
			if (tmpl == nullptr)
				return FALSE;
			o->buildingIds[o->numBuildingIds++] = tmpl->getTemplateID();
		}
	}

	if (!gxStressValid(*o))
	{
		headlessLog("[GXSTRESS] out of range: units 0..%d, buildings 0..%d (not both 0), frame 1..%d, radius 1..%d, every 1..%d",
			GX_STRESS_MAX_UNITS, GX_STRESS_MAX_BUILDINGS, GX_STRESS_MAX_FRAME, GX_STRESS_MAX_RADIUS, GX_STRESS_MAX_EVERY);
		return FALSE;
	}
	return TRUE;
}

Bool HeadlessMatch::publishGameOptions()
{
	LANGameInfo *game = TheLAN->GetMyGame();
	if (!game)
	{
		headlessLog("no game to configure");
		return FALSE;
	}

	// GeneralsX: The lobby's display title, when -lanlobby gave one.
	// Set here rather than beside RequestSetName because the LANGameInfo does not exist until
	// RequestGameCreate has run; this is the first point that has one. setLobbyName sanitises.
	if (!TheGlobalData->m_lanLobbyName.isEmpty())
	{
		AsciiString lobbyName;
		lobbyName.translate(TheGlobalData->m_lanLobbyName);
		game->setLobbyName(lobbyName);
		headlessLog("lobby name '%s'", game->getLobbyName().str());
	}

	if (!TheGlobalData->m_lanMap.isEmpty())
	{
		// Pre-flight the map here rather than letting OnGameStart discover it. Down there the
		// only symptom is the chat line "Unable to transfer the map", which points at the
		// network and not at the actual cause - a name that is not in the map cache. Note the
		// name wanted is the full path INCLUDING the filename
		// ("maps/twilight flame/twilight flame.map"); the M= field on the wire carries only the
		// directory, because GameInfoToAsciiString strips the last component.
		TheMapCache->updateCache();
		const MapMetaData *mapData = TheMapCache->findMap(TheGlobalData->m_lanMap);
		if (mapData == nullptr)
		{
			headlessLog("map \"%s\" is not in the map cache", TheGlobalData->m_lanMap.str());
			headlessLog("expected a full path including the filename, e.g. \"maps/twilight flame/twilight flame.map\"");
			Int shown = 0;
			for (MapCache::const_iterator it = TheMapCache->begin();
					it != TheMapCache->end() && shown < 15; ++it, ++shown)
			{
				headlessLog("  available: %s", it->first.str());
			}
			return FALSE;
		}
		game->setMap(TheGlobalData->m_lanMap);

		// The CRC and size are NOT cosmetic and nothing else fills them in - the lobby UI does
		// it when a map is picked. A joiner decides whether it has the map by comparing the
		// host's advertised CRC against its own cache entry (GameInfo::setMapCRC), so a host
		// that advertises 0 makes every peer conclude it is missing a map it actually has. The
		// peer then sits at "You do not have the map" and never becomes ready, while the host
		// only sees a peer that joined and never accepted.
		game->setMapCRC(mapData->m_CRC);
		game->setMapSize(mapData->m_filesize);

		headlessLog("map set to %s (crc=%08X size=%u)",
			TheGlobalData->m_lanMap.str(), mapData->m_CRC, mapData->m_filesize);
	}

	// GeneralsX: -lanobserver on the host: slot 0 is an observer seat, set
	// the way LANAPICallbacks sets a joiner's (no colour, start or team). The host still hosts and, as
	// the lowest human slot, still routes the match's packets.
	if (TheGlobalData->m_lanObserver)
	{
		LANGameSlot host = *game->getLANSlot(0);
		host.setPlayerTemplate(PLAYERTEMPLATE_OBSERVER);
		host.setColor(-1);
		host.setStartPos(-1);
		host.setTeamNumber(-1);
		game->setSlot(0, host);
		headlessLog("host takes an observer seat (slot 0)");
		if (getenv("GX_HOST_FACTION") != nullptr)
			headlessLog("GX_HOST_FACTION ignored: the host is an observer (-lanobserver)");
	}
	// GeneralsX: GX_HOST_FACTION=<FactionName> pins the host's own seat, as -lanai's :Faction pins an
	// AI's. RequestGameCreate leaves the host's template random and a headless host has no lobby screen to pick one, so a
	// harness seat driven by a bot (scripts/ai/gym, GX_ACT) would otherwise play a random general.
	else if (const char *hf = getenv("GX_HOST_FACTION"))
	{
		const Int ptIdx = playableFaction(AsciiString(hf), "GX_HOST_FACTION");
		if (ptIdx < 0)
			return FALSE;
		LANGameSlot host = *game->getLANSlot(0);
		host.setPlayerTemplate(ptIdx);
		game->setSlot(0, host);
		headlessLog("host plays %s (GX_HOST_FACTION)", hf);
	}

	// Open the slots we are holding for human peers. This is NOT optional and nothing else does
	// it: GameSlot::reset defaults every slot to SLOT_CLOSED, and it is the lobby UI that opens
	// them in a normal game. handleRequestJoin accepts a peer only if some slot isOpen(), so a
	// headless host that skips this denies every join with RET_GAME_FULL while looking, from the
	// host side, like a peer that simply never arrived.
	for (Int i = 1; i <= TheGlobalData->m_lanWaitPeers && i < MAX_SLOTS; ++i)
	{
		LANGameSlot openSlot;
		openSlot.setState(SLOT_OPEN);
		game->setSlot(i, openSlot);
	}

	// Fill slots with AI, starting after the host and after any human slots we are holding
	// open for -lanwait peers.
	Int nextSlot = 1 + TheGlobalData->m_lanWaitPeers;
	for (size_t i = 0; i < TheGlobalData->m_lanAiSpecs.size(); ++i)
	{
		const LanAiSpec &spec = TheGlobalData->m_lanAiSpecs[i];
		SlotState state = SLOT_MED_AI;
		switch (spec.difficulty)
		{
			case 'E': state = SLOT_EASY_AI;   break;
			case 'M': state = SLOT_MED_AI;    break;
			case 'H': state = SLOT_BRUTAL_AI; break;
			default: break;
		}

		for (Int n = 0; n < spec.count; ++n)
		{
			if (nextSlot >= MAX_SLOTS)
			{
				headlessLog("ran out of slots placing AI - wanted more than %d", MAX_SLOTS);
				return FALSE;
			}
			// LANGameInfo::setSlot takes a LANGameSlot, not the base GameSlot.
			LANGameSlot aiSlot;
			aiSlot.setState(state);
			// GeneralsX @feature: optional faction pin from -lanai Hx2:FactionChina. A/B harnesses
			// need a specific faction (e.g. China supply trucks); random factions make runs
			// incomparable. Unknown names fail loudly - a silently random slot would invalidate
			// the measurement while looking identical to success.
			if (spec.faction.isNotEmpty())
			{
				const Int ptIdx = playableFaction(spec.faction, "-lanai spec");
				if (ptIdx < 0)
					return FALSE;
				aiSlot.setPlayerTemplate(ptIdx);
			}
			game->setSlot(nextSlot, aiSlot);
			++nextSlot;
		}
	}

	// GeneralsX: Capacity pre-check on INTENT, before any peer connects.
	// nextSlot is exactly the intended final occupancy: it starts at 1 + m_lanWaitPeers (the host
	// plus the human slots held open) and advances once per AI placed. The slots reserved for peers
	// are still SLOT_OPEN here, so isOccupied() is FALSE for them and counting the slot list would
	// undercount by -lanwait - hence the arithmetic rather than countOccupiedNonObserverSlots().
	//
	// This is the cheap half of the guard: a misconfigured harness dies here in about a second,
	// instead of after a 60 s wait for peers that are then dropped. The authoritative check runs on
	// the real slot list just before RequestGameStart.
	// GeneralsX: ...less the seats that will observe: the host's own under
	// -lanobserver and the -lanwaitobservers ones. Observers take no start position.
	const Int intendedPlayers = nextSlot - (TheGlobalData->m_lanObserver ? 1 : 0) - TheGlobalData->m_lanWaitObservers;
	if (!lobbyFitsTheMap(game, intendedPlayers, "intended lobby"))
		return FALSE;

	// GXDRAWDBG - TEMPORARY. The match loop stops as soon as the LOCAL player is defeated, which on
	// a cramped map is long before the AI have destroyed each other's bases - exactly the events a
	// structure-rendering investigation needs. Allying the host with the first AI keeps the team
	// alive after the host is wiped out, so the run continues to its frame limit. REMOVE when done.
	if (getenv("GX_ALLYHOST") != nullptr)
	{
		LANGameSlot host = *game->getLANSlot(0);
		host.setTeamNumber(0);
		game->setSlot(0, host);
		if (nextSlot > 1)
		{
			LANGameSlot ally = *game->getLANSlot(1);
			ally.setTeamNumber(0);
			game->setSlot(1, ally);
		}
		headlessLog("GX_ALLYHOST: host and slot 1 allied on team 0");
	}

	// -lanseed pins the match seed. RequestGameCreate seeds from GetTickCount(), so two runs
	// never share a starting state and cannot be compared. With a fixed seed, two SOLO headless
	// runs on different platforms produce directly diffable [GXCRC] streams with no network
	// involved at all - which is the only way to separate a simulation divergence from a
	// transport problem.
	if (TheGlobalData->m_lanSeed != 0)
	{
		game->setSeed(TheGlobalData->m_lanSeed);
		headlessLog("seed forced to %d", TheGlobalData->m_lanSeed);
	}

	// GeneralsX: GX_LIMIT_ARMIES=1: the lobby's Limit Armies option ("O=Y"),
	// carried to every peer in the options string like the rest of the host's settings. The -lanai
	// pins above are deliberately NOT sanitised against it: a pinned sub-faction with this set is
	// how the start-time refusal in GameLogic::startNewGame is exercised.
	if (getenv("GX_LIMIT_ARMIES") != nullptr && getenv("GX_LIMIT_ARMIES")[0] == '1')
	{
		game->setOldFactionsOnly(TRUE);
		headlessLog("Limit Armies on (O=Y): USA, China and GLA only");
	}

	{
		GXStressOption stress;
		if (!hostStressFromEnv(&stress))
			return FALSE;
		if (gxStressIsOn(stress))
		{
			// buildGameInfoAsciiString writes ST= only if this succeeds, while this host would still arm
			// from its own GameInfo. Unreachable today - hostStressFromEnv validated the option and the
			// widest valid value (77 chars) fits GX_STRESS_WIRE_MAX, which stress-option-test.cpp checks -
			// so this only guards a future wider field. The reachable failure is the ANNOUNCE overflow:
			// GameInfoToAsciiString logs [GXCAP] and returns empty, this host still arms, and no peer can
			// see the lobby - so nobody joins it, and nothing can desync.
			char value[GX_STRESS_WIRE_MAX];
			if (gxStressFormat(stress, value, sizeof(value)) < 0)
			{
				headlessLog("[GXSTRESS] the stress option does not fit the lobby options (%d bytes) - refusing to host", (Int)GX_STRESS_WIRE_MAX);
				return FALSE;
			}
			headlessLog("[GXSTRESS] lobby option ST=%s: %d unit(s) and %d building(s) per player at frame %d, place %c, orders %c every %d",
				value, stress.units, stress.buildings, stress.frame, stress.place, stress.order, stress.every);
		}
		game->setGxStress(stress);
	}

	// Log the serialised slot list. Without this there is no way to confirm from the logs that
	// -lanai actually placed an AI, and a soak that silently ran human-only would "prove" a fix
	// to the AI-only desync class while never exercising it - skirmish scripts attach ONLY for AI
	// players. AI slots appear as CE/CM/CH in the S= field (CH = Computer Hard = SLOT_BRUTAL_AI).
	const AsciiString opts = GameInfoToAsciiString(game);
	headlessLog("game options: %s", opts.str());

	TheLAN->RequestGameOptions(opts, true);
	return TRUE;
}

// GeneralsX: The host's cue to press start the second time: every
// human slot reports the map. Slot 0 is the host itself, which always has it.
static Bool allHumanSlotsHaveMap( LANGameInfo *game )
{
	if (game == nullptr)
		return FALSE;
	for (Int i = 1; i < MAX_SLOTS; ++i)
	{
		const GameSlot *slot = game->getConstSlot(i);
		if (slot != nullptr && slot->isHuman() && !slot->hasMap())
			return FALSE;
	}
	return TRUE;
}

Bool HeadlessMatch::driveLobby()
{
	const UnsignedInt lobbyTimeout = TheGlobalData->m_lanTimeoutMs;

	if (TheGlobalData->m_lanRole == LANROLE_HOST)
	{
		// isDirectConnect=TRUE matters for more than discovery: RequestGameCreate's
		// "can't create a game while in one" guard is skipped on the direct-connect path,
		// and it is a direct-connect game we want anyway so peers can reach us by IP.
		TheLAN->RequestGameCreate(TheGlobalData->m_lanGameName, TRUE);
		if (!pumpUntil([]{ return s_lobbyReady != FALSE; }, lobbyTimeout, "game create"))
			return FALSE;

		if (!publishGameOptions())
			return FALSE;

		const Int wanted = TheGlobalData->m_lanWaitPeers;
		if (wanted > 0)
		{
			headlessLog("waiting for %d peer(s) to accept", wanted);
			if (!pumpUntil([wanted]{ return countAcceptedPeers() >= wanted; },
					lobbyTimeout, "peers to accept"))
				return FALSE;
		}

		// GeneralsX: GX_TEAMS=<t0>,<t1>,... - a lobby team per slot, '-' to leave
		// one alone (scripts/test/victory-probe.sh). Applied only now, because a join replaces the slot
		// and with it any team set earlier. Then un-accept and re-publish, exactly as a UI team change
		// does, and wait for the joiners to accept the new slot list, so every peer starts from it.
		// GeneralsX: GX_STARTS=<p0>,<p1>,... - a start position per slot the same way, '-' to leave one
		// random (scripts/ai/gym/evaluate.py: a side-swapped pair is the same map, factions and seed with the seats' starts
		// exchanged). Refused unless every pinned position exists on the map and none is pinned twice.
		const char *teams = getenv("GX_TEAMS");
		const char *starts = getenv("GX_STARTS");
		if (teams != nullptr || starts != nullptr)
		{
			LANGameInfo *game = TheLAN->GetMyGame();
			AsciiString list(teams != nullptr ? teams : ""), tok;
			for (Int i = 0; i < MAX_SLOTS && list.nextToken(&tok, ","); ++i)
			{
				LANGameSlot *slot = game->getLANSlot(i);
				if (tok != "-" && slot != nullptr && slot->isOccupied())
					slot->setTeamNumber(atoi(tok.str()));
			}
			if (starts != nullptr)
			{
				// Split by hand: AsciiString::nextToken merges empty tokens, so "0,,1" would pin slots 0 and 1.
				const MapMetaData *md = TheMapCache->findMap(game->getMap());
				const Int positions = (md != nullptr) ? md->m_numPlayers : 0;
				// GeneralsX: scale32: 64 bits - a 34-start map has positions 32 and 33, and `1u << 32` is undefined
				// (on arm64 and x86 it aliases position 0, so a valid GX_STARTS was refused).
				UnsignedInt64 taken = 0;
				const char *p = starts;
				for (Int i = 0; ; ++i)
				{
					const char *comma = strchr(p, ',');
					const size_t len = (comma != nullptr) ? (size_t)(comma - p) : strlen(p);
					char tok[8];
					const Bool keep = (len == 1 && p[0] == '-');
					Bool bad = (i >= MAX_SLOTS || len == 0 || len >= sizeof(tok));
					Int pos = -1;
					LANGameSlot *slot = bad ? nullptr : game->getLANSlot(i);
					if (!bad && !keep)
					{
						memcpy(tok, p, len);
						tok[len] = '\0';
						bad = tok[strspn(tok, "0123456789")] != '\0' || len > 2;
						pos = bad ? -1 : atoi(tok);
						bad = bad || pos >= positions || (taken & ((UnsignedInt64)1 << pos)) != 0 || slot == nullptr || !slot->isOccupied()
							|| slot->getPlayerTemplate() == PLAYERTEMPLATE_OBSERVER;
					}
					if (bad)
					{
						headlessLog("GX_STARTS '%s': entry %d is not '-' or a free start position below %d for an occupied player "
							"seat - refusing", starts, i, positions);
						return FALSE;
					}
					if (!keep)
					{
						taken |= (UnsignedInt64)1 << pos;
						slot->setStartPos(pos);
					}
					if (comma == nullptr)
						break;
					p = comma + 1;
				}
			}
			game->resetAccepted();
			TheLAN->RequestGameOptions(GameInfoToAsciiString(game), true);
			if (teams != nullptr)
				headlessLog("GX_TEAMS applied: %s", GameInfoToAsciiString(game).str());	// net-kill-gates.sh, victory-probe.sh gate on it
			if (starts != nullptr)
				headlessLog("GX_STARTS applied: %s", GameInfoToAsciiString(game).str());
			if (wanted > 0 && !pumpUntil([wanted]{ return countAcceptedPeers() >= wanted; },
					lobbyTimeout, "peers to accept the teams"))
				return FALSE;
		}

		// GeneralsX: The authoritative capacity check, on the slot list as
		// it actually stands. Every accepted peer is SLOT_PLAYER by now, so this counts reality
		// rather than intent and is the exact analogue of the GUI refusing to start
		// (LanGameOptionsMenu.cpp:249-258). It is the last gate before the engine assigns start
		// positions in populateRandomStartPosition.
		if (!lobbyFitsTheMap(TheLAN->GetMyGame(), countOccupiedNonObserverSlots(TheLAN->GetMyGame()),
				"final slot list"))
			return FALSE;

		headlessLog("starting match");
		TheLAN->RequestGameStart();
	}
	else
	{
		headlessLog("joining %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(TheGlobalData->m_lanJoinIP));
		// A joiner's GX_STRESS* is ignored by design (hostStressFromEnv). Say so, rather than let an
		// operator believe the variable they set on this machine did something.
		if (getenv("GX_STRESS") != nullptr || getenv("GX_STRESS_BUILDINGS") != nullptr)
			headlessLog("[GXSTRESS] GX_STRESS* is set on a JOINING peer and is ignored - only the host's lobby option applies");
		TheLAN->RequestGameJoinDirectConnect(TheGlobalData->m_lanJoinIP);
		if (!pumpUntil([]{ return s_lobbyReady != FALSE; }, lobbyTimeout, "game join"))
			return FALSE;

		TheLAN->RequestHasMap();
		TheLAN->RequestAccept();
		s_lastAcceptSent = timeGetTime();
	}

	// Both roles land here. The host reaches OnGameStart through its own RequestGameStart
	// round trip, so it waits too rather than assuming.
	//
	// A joiner keeps re-asserting its accept while it waits - see reassertAcceptIfNeeded. Without
	// it, any lobby with more than one joiner deadlocks: the second join resets the first peer's
	// accept, the host never counts two, and every peer sits there believing it is ready.
	//
	// GeneralsX: A start that only transferred the map ends back in
	// the lobby (owner's two-start flow); the host emulates the human's second click once every
	// human slot reports the map - the has-map re-announcements land within a second or two. A
	// genuinely undeliverable map still fails: the transfer round itself bails and s_lobbyFailed
	// fires, so this cannot loop; the lobbyTimeout is the backstop.
	const Bool isJoiner = (TheGlobalData->m_lanRole != LANROLE_HOST);
	if (!pumpUntil([isJoiner]{
			if (isJoiner)
				reassertAcceptIfNeeded();
			else if (s_transferOnlyStarts > s_restartsIssued
					&& allHumanSlotsHaveMap(TheLAN->GetMyGame()))
			{
				++s_restartsIssued;
				headlessLog("map delivered to every slot - pressing start again");
				TheLAN->RequestGameStart();
			}
			return s_gameStarted != FALSE;
		}, lobbyTimeout, "game start"))
		return FALSE;

	return TRUE;
}

// GeneralsX: The pacer's between-draws pass (GameEngine::updateBetweenDraws), stopped
// at -lanframes N: runMatch's limit check below expects the logic AT N after each iteration - it holds the
// logic for the final flush pass - and a pass inside the pacer's hold would otherwise simulate past it.
static Bool headlessBetweenDraws()
{
	const Int limit = TheGlobalData->m_lanFrameLimit;
	if (limit > 0 && TheGameLogic != nullptr && TheGameLogic->getFrame() >= (UnsignedInt)limit)
		return FALSE;
	return GameEngine::updateBetweenDraws();
}

Bool HeadlessMatch::runMatch()
{
	const Int frameLimit = TheGlobalData->m_lanFrameLimit;
	Bool sawMismatch = FALSE;

	// OnGameStart only POSTS MSG_NEW_GAME; TheGameLogic does not report isInGame() until that
	// message has been propagated and handled. So "started" and "in game" are not the same
	// instant, and !isInGame() cannot be used as the game-over test until the game has actually
	// been entered once.
	//
	// Getting this wrong is a race that hides on fast paths: macOS and Windows both happened to
	// process the message inside the first update() and ran fine for thousands of frames, while
	// Linux did not and exited having simulated 0 frames - reporting success. A third platform
	// is what exposed it.
	Bool everInGame = FALSE;

	// Track the high-water mark as we go. TheGameLogic's frame counter is RESET when the game is
	// torn down, so reading it after the loop reports the post-teardown value: a run that
	// simulated 370 frames logged "simulated 1 frames" while its own [GXCRC] output ran to
	// f=370. The log has to agree with the evidence beside it.
	UnsignedInt maxFrame = 0;

	// Reuse GameEngine::update rather than open-coding the subsystem order. That ordering
	// (client, message stream, network, then logic, all inside VERIFY_CRC) is load-bearing
	// for determinism, and a second copy of it here would drift from the real one.
	// GXDRAWDBG - TEMPORARY deterministic forcing function for the destroyed-structure rendering
	// investigation: capture and then kill every instance of a named template at fixed frames, so
	// the POSTMORTEM audit has something to look at without waiting on the AI. REMOVE when done.
	const char *killTmpl = getenv("GX_KILLTMPL");
	const char *killFrameEnv = getenv("GX_KILLFRAME");
	const UnsignedInt killFrame = (killFrameEnv != nullptr) ? (UnsignedInt)atoi(killFrameEnv) : 400;
	const Bool wantCapture = (getenv("GX_CAPTURE") != nullptr);
	Bool didCapture = FALSE;
	Bool didKill = FALSE;
	Bool cameraSet = FALSE;

	// GeneralsX: GX_HEADLESS_LINGER_MS: keep running this long after the frame limit.
	// Every peer used to exit the instant IT reached -lanframes, and a peer that exits sends no leave and answers
	// no resend. So a peer a few frames behind - one whose commands for frame N were lost in flight from a peer
	// that had already reached the limit and gone - waited forever for data nobody alive could resend. Measured
	// 25/09/2026 with twelve headless peers on one Mac: gx08 stopped at frame 3593 "blocked=7" 7 frames short,
	// 135 resend retries unanswered, after slot 7 and the router had exited at 3600. Lingering keeps every peer
	// pumping and resending until the stragglers are past the limit too. Unset or 0 is the old behaviour.
	const char *lingerEnv = getenv("GX_HEADLESS_LINGER_MS");
	const Int lingerMs = (lingerEnv != nullptr) ? atoi(lingerEnv) : 0;
	UnsignedInt lingerUntil = 0;
	Bool lingering = FALSE;

	while (!TheGameEngine->getQuitting())
	{
		TheGameEngine->update();

		// GeneralsX: Drive the frame pacer from this loop too.
		//
		// This mirrors GameEngine::execute(). A -lanhost/-lanjoin peer never runs execute() -
		// GameMain routes it here instead - so it never called TheFramePacer->update(), and the
		// pacer is BOTH the render cap and the sole source of m_updateTime. With it never
		// running, m_updateTime stayed frozen at the constructor's 1/BaseFps while this loop
		// free-ran, so getActualLogicTimeScaleOverFpsRatio handed a full logic-frame animation
		// step to every rendered frame. That is the measured 853[60]: the HUD prints [60]
		// because the limit still RESOLVES correctly, and resolving and enforcing are different
		// code paths - only the resolving one was running. Not Windows-specific despite where it
		// was seen; any peer started -lanjoin without -headless renders unpaced.
		//
		// Simulation was never affected: canUpdateNetworkGameLogic gates purely on
		// isFrameDataReady() and never reads the render limit, which is why 18 minutes of this
		// produced zero mismatches.
		//
		// Headless peers are deliberately excluded - they have no renderer and want max speed.
		//
		// GeneralsX: GX_PACE_HEADLESS=1 paces a -headless peer too, at its
		// max-FPS cap (-fps N / -noFPSLimit), so a headless lockstep run can measure what the cap does to
		// the logic rate and prove the CRC stream does not depend on it. Read once; off by default.
		static const Bool s_paceHeadless = (getenv("GX_PACE_HEADLESS") != nullptr);
		if ((!TheGlobalData->m_headless || s_paceHeadless) && TheFramePacer != nullptr)
			TheFramePacer->update(&headlessBetweenDraws);

		if (killTmpl != nullptr && TheGameLogic->isInGame())
		{
			const UnsignedInt now = TheGameLogic->getFrame();
			if (wantCapture && !didCapture && now >= killFrame / 2)
			{
				Player *owner = nullptr;
				for (Int p = 0; p < ThePlayerList->getPlayerCount(); ++p)
				{
					Player *cand = ThePlayerList->getNthPlayer(p);
					if (cand != nullptr && cand->isPlayableSide())
					{
						owner = cand;
						break;
					}
				}
				for (Object *o = TheGameLogic->getFirstObject(); o != nullptr; o = o->getNextObject())
				{
					const Bool capMatches = (strcmp(killTmpl, "*STRUCTURE*") == 0)
						? o->isKindOf(KINDOF_STRUCTURE)
						: (strcmp(o->getTemplate()->getName().str(), killTmpl) == 0);
					if (owner != nullptr && capMatches)
					{
						o->setTeam(owner->getDefaultTeam());
						headlessLog("GX_KILLTMPL: captured %s id=%d for player %d", killTmpl, (Int)o->getID(), (Int)owner->getPlayerIndex());

						// Point the camera at the first victim so the result can actually be looked at.
						if (!cameraSet && TheTacticalView != nullptr)
						{
							Coord3D pos = *o->getPosition();
							TheTacticalView->lookAt(&pos);
							TheTacticalView->setZoom(0.35f);
							headlessLog("GX_KILLTMPL: camera on id=%d at %.0f,%.0f", (Int)o->getID(), pos.x, pos.y);
							cameraSet = TRUE;   // only the first
						}
					}
				}
				didCapture = TRUE;
			}
			if (!didKill && now >= killFrame)
			{
				for (Object *o = TheGameLogic->getFirstObject(); o != nullptr; o = o->getNextObject())
				{
					const Bool matches = (strcmp(killTmpl, "*STRUCTURE*") == 0)
						? o->isKindOf(KINDOF_STRUCTURE)
						: (strcmp(o->getTemplate()->getName().str(), killTmpl) == 0);
					if (matches && !o->isEffectivelyDead())
					{
						headlessLog("GX_KILLTMPL: killing %s id=%d at frame %d", o->getTemplate()->getName().str(), (Int)o->getID(), (Int)now);
						o->kill();
					}
				}
				didKill = TRUE;
			}
		}

		// Sample the desync flag while the game is still live and latch it. This must be
		// TheNetwork's flag, NOT TheRecorder's: RecorderClass::sawCRCMismatch dereferences
		// m_crcInfo, which is only ever allocated when loading a replay (Recorder.cpp:1390),
		// so on a live LAN match that call is an unconditional null dereference.
		if (TheNetwork != nullptr && TheNetwork->sawCRCMismatch())
			sawMismatch = TRUE;

		if (TheGameLogic->isInGame())
			everInGame = TRUE;
		if (TheGameLogic->getFrame() > maxFrame)
			maxFrame = TheGameLogic->getFrame();

		// Once the match ends the logic drops out of the game; without this the loop would
		// spin in the post-game shell forever. Gate on everInGame, not s_gameStarted - see the
		// comment above the declaration.
		if (everInGame && !TheGameLogic->isInGame())
		{
			headlessLog("game over at frame %d", (Int)maxFrame);
			break;
		}

		// GeneralsX: Test the limit against THIS iteration's frame, right
		// after update() - never the previous iteration's. It used to run above the maxFrame
		// update, so the loop reached the limit, went round once more and only then stopped: that
		// extra update() simulated frame N when the network pacer was behind (its debt makes the
		// frame ready at once) and nothing when it was on time, so a -lanframes 3000 run's trace
		// ended at f=3000 or f=2999 depending on load. update() advances at most one logic frame,
		// so stopping the moment getFrame() reaches the limit simulates exactly frames 0..N-1.
		//
		// The old extra pass was not all waste: the final tick queued its outgoing messages
		// (MSG_LOGIC_CRC on a CRC frame) in TheMessageStream, and only the NEXT update() propagates
		// them and hands them to TheNetwork - so a peer still playing got this one's last CRC. Keep
		// that pass, with the logic held so it cannot simulate frame N.
		//
		// GeneralsX: (observer seats) ...unless GX_HEADLESS_LINGER_MS asks this peer to
		// stay: then it keeps running past the limit, simulating as lockstep lets it, so a peer still short of
		// the limit can finish instead of stalling on one that left. Its last messages go out with those updates.
		if (frameLimit > 0 && maxFrame >= (UnsignedInt)frameLimit && lingerMs > 0)
		{
			if (!lingering)
			{
				headlessLog("frame limit %d reached", frameLimit);
				lingering = TRUE;
				lingerUntil = timeGetTime() + (UnsignedInt)lingerMs;
				headlessLog("lingering %d ms past the frame limit so a peer still short of it can finish (GX_HEADLESS_LINGER_MS)", lingerMs);
			}
			else if ((Int)(timeGetTime() - lingerUntil) >= 0)
				break;
		}
		else if (frameLimit > 0 && maxFrame >= (UnsignedInt)frameLimit)
		{
			Int queued = 0, left = 0;
			for (GameMessage *m = TheMessageStream->getFirstMessage(); m != nullptr; m = m->next())
				++queued;
			TheGameEngine->holdLogicForNextUpdate();
			TheGameEngine->update();
			for (GameMessage *m = TheMessageStream->getFirstMessage(); m != nullptr; m = m->next())
				++left;
			headlessLog("frame limit %d reached at frame %d; %d queued message(s) flushed, %d left",
				frameLimit, (Int)TheGameLogic->getFrame(), queued, left);
			break;
		}
	}

	headlessLog("simulated %d frames", (Int)maxFrame);

	// A run that simulated nothing is a failure, not a success. Reporting exit 0 here is how the
	// race above stayed invisible: the process looked like a clean run that simply had no work.
	if (maxFrame == 0)
	{
		headlessLog("FAILED: the match never simulated a single frame");
		return FALSE;
	}

	if (sawMismatch)
	{
		headlessLog("CRC MISMATCH seen during this match");
		return FALSE;
	}

	// A clean exit here means only that THIS peer never noticed a mismatch. The authoritative
	// cross-platform check is diffing the two peers' [GXCRC] streams offline, because a
	// divergence can incubate in unhashed state (Object::crc covers nine fields and never walks
	// the behavior modules) long before either peer votes on it.
	return TRUE;
}

int HeadlessMatch::run()
{
	s_gameStarted = FALSE;
	s_lobbyReady = FALSE;
	s_lobbyFailed = FALSE;
	s_lastAcceptSent = 0;
	s_lobbyFailureReason.clear();

	// GeneralsX: REFUSE, LOUDLY, RATHER THAN SIMULATE A DIFFERENT GAME.
	//
	// A peer with no reachable base-Generals archives is missing the bridge models, so before the
	// decouple in W3DBridgeBuffer::addBridge it silently ran a match with two fewer objects than
	// everybody else. The decouple fixes object EXISTENCE; it cannot recover the model's geometry,
	// so such a peer still computes a different bridge footprint and must not be in a lobby.
	// What is asked is exactly that - can every bridge model load (TerrainVisual.h); GameEngine::init
	// already decided it, before any map, and printed it when the answer is no.
	//
	// Exiting non-zero is the point: GameMain.cpp uses this return value as the process exit code,
	// and a harness that reads only "did it reach N frames" would otherwise score the run as a
	// clean experiment. That is precisely what produced a 466-frame three-platform "result" on
	// 2026-09-20.
	//
	// WHAT COUNTS AS A NETWORK GAME HERE, and why it is not simply "-lanhost or -lanjoin":
	// a -lanhost with no -lanwait holds no human slot open and nobody can join it, so it is a solo
	// AI match that happens to be driven through the LAN code path - the headless equivalent of
	// Skirmish, which must keep working without base assets exactly as the GUI's Skirmish does.
	// -lanjoin is always somebody else's game. -lanlist only asks a relay what exists and starts
	// nothing, so it is left alone.
	const Bool bridgeModelsLoad = (TheTerrainVisual == nullptr) || TheTerrainVisual->isEveryBridgeModelAvailable();
	const Bool joiningOthers = (TheGlobalData->m_lanRole == LANROLE_JOIN);
	const Bool hostingForOthers = (TheGlobalData->m_lanRole == LANROLE_HOST && TheGlobalData->m_lanWaitPeers > 0);
	if ((joiningOthers || hostingForOthers) && !TheGlobalData->m_lanListGames && !bridgeModelsLoad)
	{
		headlessLog("aborting: no base Generals assets. Zero Hour needs Textures.big, W3D.big and "
			"Terrain.big from the BASE game; put them in a ZH_Generals folder inside this data "
			"root, or point CNC_GENERALS_PATH at a base Generals install. Network play is refused "
			"without them because this peer would simulate a different game.");
		return 1;
	}

	if (!setUpLan())
		return 1;

	// -lanlist: ask the relay what games exist and print them. This is a browser without a UI, and
	// it is the only way to exercise the whole list path - query, relay reply, client parse -
	// against the real relay on a machine with no display.
	if (TheGlobalData->m_lanListGames)
	{
		Transport *transport = TheLAN->getTransport();
		if (transport == nullptr || !transport->isRelayEnabled())
		{
			headlessLog("-lanlist needs a relay: set RelayAddress and LocalVirtualIP in Options.ini");
			return 1;
		}

		transport->requestGameList();

		// The replies are a burst of datagrams, one per game, so there is nothing to wait FOR -
		// wait a fixed moment and print whatever arrived. Two seconds is far longer than a relay
		// round trip and short enough to stay usable.
		const UnsignedInt deadline = timeGetTime() + 2000;
		while (timeGetTime() < deadline)
		{
			TheLAN->update();
			Sleep(10);
		}

		const Int count = transport->getGameListCount();
		headlessLog("%d game(s) listed", count);
		for (Int i = 0; i < count; ++i)
		{
			const Transport::RelayGameListing *g = transport->getGameListing(i);
			if (g == nullptr)
				continue;
			headlessLog("  room=%s host=%d.%d.%d.%d players=%d/%d name=\"%s\" map=\"%s\"",
				g->room, PRINTF_IP_AS_4_INTS(g->hostVirtualIP), g->players, g->slots,
				g->name, g->map);
		}
		return (count > 0) ? 0 : 1;
	}

	if (!driveLobby())
	{
		if (s_lobbyFailureReason.isNotEmpty())
			headlessLog("aborting: %s", s_lobbyFailureReason.str());
		else
			headlessLog("aborting: lobby did not reach a started game");
		return 1;
	}

	const Bool ok = runMatch();

	if (TheLAN)
	{
		TheLAN->RequestGameLeave();
		TheLAN->update();
	}

	return ok ? 0 : 1;
}
