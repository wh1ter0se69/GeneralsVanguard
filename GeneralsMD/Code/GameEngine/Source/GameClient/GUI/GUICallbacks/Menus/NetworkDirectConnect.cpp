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
// FILE: NetworkDirectConnect.cpp
// Author: Bryan Cleveland, November 2001
// Description: Lan Lobby Menu
///////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "gamespy/peer/peer.h"

#include "Common/QuotedPrintable.h"
#include "Common/OptionPreferences.h"
#include "GameClient/AnimateWindowManager.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/Gadget.h"
#include "GameClient/GameText.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GadgetComboBox.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/Shell.h"
#include "GameClient/GameWindowTransitions.h"
#include "GameClient/GUICallbacks.h"		// GXCreateLobbyLANAPI

#include "GameNetwork/IPEnumeration.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/LANAPICallbacks.h"


// window ids ------------------------------------------------------------------------------

// Window Pointers ------------------------------------------------------------------------

// PRIVATE DATA ///////////////////////////////////////////////////////////////////////////////////

extern Bool LANbuttonPushed;
extern Bool LANisShuttingDown;

static Bool isShuttingDown = false;
static Bool buttonPushed = false;

static NameKeyType buttonBackID = NAMEKEY_INVALID;
static NameKeyType buttonHostID = NAMEKEY_INVALID;
static NameKeyType buttonJoinID = NAMEKEY_INVALID;
static NameKeyType editPlayerNameID = NAMEKEY_INVALID;
static NameKeyType comboboxRemoteIPID = NAMEKEY_INVALID;
static NameKeyType staticLocalIPID = NAMEKEY_INVALID;

static GameWindow *buttonBack = nullptr;
static GameWindow *buttonHost = nullptr;
static GameWindow *buttonJoin = nullptr;
static GameWindow *editPlayerName = nullptr;
static GameWindow *comboboxRemoteIP = nullptr;
static GameWindow *staticLocalIP = nullptr;

// Matchmaking: every host advertises in its own room, so the host address alone no longer identifies a game.
// The room travels with the selection: snapshotted per list row (read back by index on Join) and appended to the
// entry text as "#<room>", which survives into the saved history. JoinDirectConnectGame splits on '(' first.
static char s_listedRoom[Transport::MAX_RELAY_LISTINGS][32];
static UnsignedInt s_listedHostIP[Transport::MAX_RELAY_LISTINGS];
static Int s_listedCount = 0;

/// The room a combo box entry refers to, or empty if it names none.
static AsciiString relayRoomForEntry( Int selectedPos, UnsignedInt hostIP, const AsciiString &entryText )
{
	// The snapshot wins when it agrees about the host address. The cross-check matters: the list is
	// rebuilt whenever the relay's reply count changes, so a stale index would otherwise silently
	// send the player into a different game's room.
	if (selectedPos >= 0 && selectedPos < s_listedCount
		&& s_listedHostIP[selectedPos] == hostIP && s_listedRoom[selectedPos][0] != '\0')
	{
		return AsciiString(s_listedRoom[selectedPos]);
	}

	// A typed address, or one picked out of the saved history, lands here - neither is covered by
	// a snapshot that only describes rows this visit built.
	const char *hash = strrchr(entryText.str(), '#');
	if (hash != nullptr)
	{
		char room[32];
		Int n = 0;
		for (const char *c = hash + 1; *c != '\0' && n < (Int)sizeof(room) - 1; ++c)
		{
			if (!isalnum((unsigned char)*c) && *c != '-' && *c != '_' && *c != '.')
				break;
			room[n++] = *c;
		}
		room[n] = '\0';
		if (n > 0)
			return AsciiString(room);
	}

	return AsciiString();
}

void PopulateRemoteIPComboBox()
{
	LANPreferences userprefs;
	GadgetComboBoxReset(comboboxRemoteIP);

	Int numRemoteIPs = userprefs.getNumRemoteIPs();
	Color white = GameMakeColor(255,255,255,255);

	// GeneralsX: Relay transport. Relay mode is decided by OUR identity, the same test
	// Transport::initRelay makes - a room can now hold up to eight peers, so there is no single
	// "other machine" to key it on.
	//
	// PeerVirtualIP survives here as a pure convenience and no longer means "the other player":
	// it is the address a JOINER dials, i.e. the host's virtual identity, and it is not in the
	// saved history until the first join. A host does not need it set at all. Routing no longer
	// reads it - every destination now comes from the slot list, like it would on a LAN.
	OptionPreferences prefs;

	// Ask the TRANSPORT whether relay mode is on, not the preferences file. Since the relay
	// assigns identities, LocalVirtualIP is usually absent now, and keying off it here would
	// silently hide the game list on exactly the setups the assignment was built for.
	Transport *relayTransport = (TheLAN != nullptr) ? TheLAN->getTransport() : nullptr;
	const Bool relayMode = (relayTransport != nullptr) && relayTransport->isRelayEnabled();
	const UnsignedInt peerVirtualIP = prefs.getPeerVirtualIP();
	UnicodeString peerEntry;

	// GeneralsX: Server list. Live games the relay knows about go first - this is the
	// browser. The entry has to stay dialable, because JoinDirectConnectGame reads the combo box
	// text back and parses an address out of it; it already splits on '(' before doing so, so an
	// annotation in parentheses is carried for free and needs no change there.
	Int listedGames = 0;
	s_listedCount = 0;
	if (relayMode && TheLAN != nullptr && TheLAN->getTransport() != nullptr)
	{
		Transport *transport = TheLAN->getTransport();
		const Int count = transport->getGameListCount();
		for (Int g = 0; g < count; ++g)
		{
			const Transport::RelayGameListing *game = transport->getGameListing(g);
			if (game == nullptr || game->hostVirtualIP == 0)
				continue;

			// Do not offer our own game - a host browsing its own lobby would be dialling itself.
			// The ROOM is half of that test and not an optimisation: addresses are allocated per
			// room, so every host in the browser is 10.42.0.1 and so are we. Matching on the
			// address alone used to be right when there was one room; now it hides every game in
			// the list, including all the ones we could actually join.
			if (game->hostVirtualIP == TheLAN->GetLocalIP()
				&& strcmp(game->room, relayTransport->getRelayRoom()) == 0)
				continue;

			UnicodeString wideName, wideMap;
			wideName.translate(AsciiString(game->name));
			wideMap.translate(AsciiString(game->map));

			UnicodeString wideRoom;
			wideRoom.translate(AsciiString(game->room));

			// The room rides in the text as well as the snapshot (see s_listedRoom), inside the parentheses after the
			// address so JoinDirectConnectGame's split on '(' is unaffected. The tail carries what a human picks by
			// (map, players, host), as a narrow combo box shows the end of the row.
			UnicodeString listing;
			listing.format(L"%d.%d.%d.%d (#%ls  %ls  %d/%d  %ls)",
				PRINTF_IP_AS_4_INTS(game->hostVirtualIP),
				wideRoom.str(), wideMap.str(), game->players, game->slots, wideName.str());
			GadgetComboBoxAddEntry(comboboxRemoteIP, listing, white);

			if (s_listedCount < Transport::MAX_RELAY_LISTINGS)
			{
				strlcpy(s_listedRoom[s_listedCount], game->room, sizeof(s_listedRoom[0]));
				s_listedHostIP[s_listedCount] = game->hostVirtualIP;
				++s_listedCount;
			}
			++listedGames;
		}
	}

	if (relayMode && peerVirtualIP != 0 && listedGames == 0)
	{
		// Only when the relay offered nothing: with a live list this is redundant, and a bare
		// address next to described games reads like a broken entry.
		peerEntry.format(L"%d.%d.%d.%d", PRINTF_IP_AS_4_INTS(peerVirtualIP));
		GadgetComboBoxAddEntry(comboboxRemoteIP, peerEntry, white);
	}

	for (Int i = 0; i < numRemoteIPs; ++i)
	{
		UnicodeString entry;
		entry = userprefs.getRemoteIPEntry(i);
		// Keyed on peerEntry rather than relayMode: relay mode no longer implies a peer address
		// was offered above (a host does not set one), and an empty peerEntry would make
		// startsWith match EVERY entry - dropping any history entry that is not an IP.
		if (!peerEntry.isEmpty() && entry.startsWith(peerEntry))
		{
			// The history keeps a copy of the peer after the first join, and it is already offered
			// above. Check the character that follows too, so that 10.42.0.1 does not swallow a
			// saved 10.42.0.10.
			const Int tail = peerEntry.getLength();
			const WideChar next = (entry.getLength() > tail) ? entry.getCharAt(tail) : L'\0';
			if (next < L'0' || next > L'9')
				continue;
		}

		GadgetComboBoxAddEntry(comboboxRemoteIP, entry, white);
	}

	if (listedGames > 0 || !peerEntry.isEmpty() || numRemoteIPs > 0)
	{
		GadgetComboBoxSetSelectedPos(comboboxRemoteIP, 0, TRUE);
	}
	userprefs.write();
}

void UpdateRemoteIPList()
{
	Int n1[4], n2[4];
	LANPreferences prefs;
	Int numEntries = GadgetComboBoxGetLength(comboboxRemoteIP);
	Int currentSelection = -1;
	GadgetComboBoxGetSelectedPos(comboboxRemoteIP, &currentSelection);
	UnicodeString unisel = GadgetComboBoxGetText(comboboxRemoteIP);
	AsciiString sel;
	sel.translate(unisel);

//	UnicodeString newEntry = prefs.getRemoteIPEntry(0);
	UnicodeString newEntry = unisel;
	UnicodeString newIP;
	newEntry.nextToken(&newIP, L":");
	Int numFields = swscanf(newIP.str(), L"%d.%d.%d.%d", &(n1[0]), &(n1[1]), &(n1[2]), &(n1[3]));

	if (numFields != 4) {
		// this is not a properly formatted IP, don't change a thing.
		return;
	}

	prefs["RemoteIP0"] = sel;

	Int currentINIEntry = 1;

	for (Int i = 0; i < numEntries; ++i)
	{
		if (i != currentSelection)
		{
			GadgetComboBoxSetSelectedPos(comboboxRemoteIP, i, FALSE);
			UnicodeString uni;
			uni = GadgetComboBoxGetText(comboboxRemoteIP);
			AsciiString ascii;
			ascii.translate(uni);

			// prevent more than one copy of an IP address from being put in the list.
			if (currentSelection == -1)
			{
				UnicodeString oldEntry = uni;
				UnicodeString oldIP;
				oldEntry.nextToken(&oldIP, L":");

				swscanf(oldIP.str(), L"%d.%d.%d.%d", &(n2[0]), &(n2[1]), &(n2[2]), &(n2[3]));

				Bool isEqual = TRUE;
				for (Int i = 0; (i < 4) && (isEqual == TRUE); ++i) {
					if (n1[i] != n2[i]) {
						isEqual = FALSE;
					}
				}
				// check to see if this is a duplicate or if this is not a properly formatted IP address.
				if (isEqual == TRUE)
				{
					--numEntries;
					continue;
				}
			}
			AsciiString temp;
			temp.format("RemoteIP%d", currentINIEntry);
			++currentINIEntry;
			prefs[temp.str()] = ascii;
		}
	}

	if (currentSelection == -1)
	{
		++numEntries;
	}

	AsciiString numRemoteIPs;
	numRemoteIPs.format("%d", numEntries);

	prefs["NumRemoteIPs"] = numRemoteIPs;

	prefs.write();
}

void HostDirectConnectGame()
{
	// Init LAN API Singleton
	DEBUG_ASSERTCRASH(TheLAN != nullptr, ("TheLAN is null!"));
	if (!TheLAN)
	{
		TheLAN = GXCreateLobbyLANAPI();
	}

	UnsignedInt localIP = TheLAN->GetLocalIP();
	UnicodeString localIPString;
	localIPString.format(L"%d.%d.%d.%d", PRINTF_IP_AS_4_INTS(localIP));

	UnicodeString name;
	name = GadgetTextEntryGetText(editPlayerName);

	LANPreferences prefs;
	prefs["UserName"] = UnicodeStringToQuotedPrintable(name);
	prefs.write();

	name.truncateTo(g_lanPlayerNameLength);
	TheLAN->RequestSetName(name);
	TheLAN->RequestGameCreate(localIPString, TRUE);
}

void JoinDirectConnectGame()
{
	// Init LAN API Singleton

	if (!TheLAN)
	{
		TheLAN = GXCreateLobbyLANAPI();
	}

	// Read the selection BEFORE anything rebuilds the combo box. UpdateRemoteIPList walks the list
	// by MOVING the selection, and PopulateRemoteIPComboBox then resets it to 0 and rebuilds the
	// room snapshot underneath us - so by the time either has run, neither the index nor the text
	// still describes what the player clicked.
	Int selectedPos = -1;
	GadgetComboBoxGetSelectedPos(comboboxRemoteIP, &selectedPos);

	UnsignedInt ipaddress = 0;
	UnicodeString ipunistring = GadgetComboBoxGetText(comboboxRemoteIP);
	AsciiString asciientry;
	asciientry.translate(ipunistring);
	const AsciiString selectedEntry = asciientry;	// nextToken below consumes asciientry

	AsciiString ipstring;
	asciientry.nextToken(&ipstring, "(");

	Int ip1, ip2, ip3, ip4;
	Int numFields = sscanf(ipstring.str(), "%d.%d.%d.%d", &ip1, &ip2, &ip3, &ip4);
	(void)numFields; DEBUG_ASSERTCRASH(numFields == 4, ("JoinDirectConnectGame - invalid IP address format: %s", ipstring.str()));

	DEBUG_LOG(("JoinDirectConnectGame - joining at %d.%d.%d.%d", ip1, ip2, ip3, ip4));

	ipaddress = (ip1 << 24) + (ip2 << 16) + (ip3 << 8) + ip4;
//	ipaddress = htonl(ipaddress);

	const AsciiString targetRoom = relayRoomForEntry(selectedPos, ipaddress, selectedEntry);

	UnicodeString name;
	name = GadgetTextEntryGetText(editPlayerName);

	LANPreferences prefs;
	prefs["UserName"] = UnicodeStringToQuotedPrintable(name);
	prefs.write();

	UpdateRemoteIPList();
	PopulateRemoteIPComboBox();

	// Matchmaking: move into the picked game's room before dialling. Order matters: take the room from the LISTING;
	// get an identity IN THAT ROOM (addresses are per room); rebuild TheLAN (LANGameInfo snapshots the local IP);
	// then SetLocalIP (rebinds, re-runs initRelay); then dial the host. Do nothing if already in that room.
	Transport *transport = TheLAN->getTransport();
	if (transport != nullptr && transport->isRelayEnabled() && !targetRoom.isEmpty()
		&& strcmp(targetRoom.str(), transport->getRelayRoom()) != 0)
	{
		OptionPreferences optprefs;
		const AsciiString relayHost = optprefs.getRelayAddress();

		UnsignedInt joinIP = 0;
		if (Transport::enterRelayRoom(relayHost.str(), targetRoom.str(), joinIP))
		{
			DEBUG_LOG(("JoinDirectConnectGame - moving from room '%s' to '%s'; we are %d.%d.%d.%d there",
				transport->getRelayRoom(), targetRoom.str(), PRINTF_IP_AS_4_INTS(joinIP)));

			delete TheLAN;
			TheLAN = GXCreateLobbyLANAPI();
			DEBUG_ASSERTCRASH(TheLAN->GetMyGame() == nullptr,
				("JoinDirectConnectGame - a LANGameInfo already exists and has snapshotted the old local IP; SetLocalIP below will not reach it"));
			TheLAN->init();
			TheLAN->SetLocalIP(joinIP);

			// The browser was built from the old transport's replies and the new one has heard
			// nothing yet. Re-ask, so that a failed join leaves a screen that refills itself
			// instead of an empty list.
			if (TheLAN->getTransport() != nullptr && TheLAN->getTransport()->isRelayEnabled())
				TheLAN->getTransport()->requestGameList();
		}
		else
		{
			// Dialling anyway would reach the relay but never the host: we are not a member of the
			// room the packet has to be forwarded in. Say so rather than letting it time out as a
			// generic unreachable peer.
			DEBUG_LOG(("JoinDirectConnectGame - the relay would not give us an identity in room '%s'; the join will not reach the host",
				targetRoom.str()));
		}
	}

	name.truncateTo(g_lanPlayerNameLength);
	TheLAN->RequestSetName(name);

	TheLAN->RequestGameJoinDirectConnect(ipaddress);
}

//-------------------------------------------------------------------------------------------------
/** Build TheLAN for an online screen. See the note on the declaration in GUICallbacks.h. */
//-------------------------------------------------------------------------------------------------
Bool SetUpOnlineLobbyLAN( void )
{
	delete TheLAN;
	TheLAN = nullptr;

	TheLAN = GXCreateLobbyLANAPI();

	OptionPreferences prefs;
	UnsignedInt IP = prefs.getDirectConnectIPAddress();	// GeneralsX: follows "Use my VPN for LAN"

	// Relay transport: our identity is the virtual LAN address, which is on no interface, so skip the interface
	// enumeration below or its fallback would replace it with a real one.
	// Ask the relay for our address before SetLocalIP (LANGameInfo snapshots it); fall back to LocalVirtualIP.
	const AsciiString relayHost = prefs.getRelayAddress();
	UnsignedInt virtualIP = 0;
	if (!relayHost.isEmpty())
	{
		// GeneralsX: Matchmaking. Our OWN room, not a shared one. Entering an online
		// screen is entering the room we would host in, so that a game created from here is the
		// only game in it; picking somebody else's game out of the browser moves us into theirs
		// instead - see JoinDirectConnectGame.
		AsciiString room = Transport::getLocalHostRoom();

		UnsignedInt assignedIP = 0;
		if (Transport::enterRelayRoom(relayHost.str(), room.str(), assignedIP))
		{
			virtualIP = assignedIP;
			DEBUG_LOG(("SetUpOnlineLobbyLAN - relay assigned %d.%d.%d.%d in room '%s'",
				PRINTF_IP_AS_4_INTS(virtualIP), room.str()));
		}
		else
		{
			virtualIP = prefs.getLocalVirtualIP();
			DEBUG_LOG(("SetUpOnlineLobbyLAN - relay did not assign an identity; falling back to LocalVirtualIP %d.%d.%d.%d",
				PRINTF_IP_AS_4_INTS(virtualIP)));
		}
	}

	const Bool relayMode = !relayHost.isEmpty() && (virtualIP != 0);

	if (relayMode)
	{
		IP = virtualIP;
		DEBUG_LOG(("SetUpOnlineLobbyLAN - relay mode, local identity is %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(IP)));
	}
	else
	{
		IPEnumeration IPs;

		EnumeratedIP *IPlist = IPs.getAddresses();
		DEBUG_ASSERTCRASH(IPlist, ("No IP addresses found!"));
		if (!IPlist)
		{
			/// @todo: display an error to the user rather than continuing silently
			DEBUG_LOG(("SetUpOnlineLobbyLAN: no local IP addresses found.\n"));
		}

		Bool foundIP = FALSE;
		EnumeratedIP *tempIP = IPlist;
		while ((tempIP != nullptr) && (foundIP == FALSE)) {
			if (IP == tempIP->getIP()) {
				foundIP = TRUE;
			}
			tempIP = tempIP->getNext();
		}

		if (foundIP == FALSE && IPlist != nullptr) {
			// The IP that we had no longer exists, we need to pick a new one.
			IP = IPlist->getIP();
		}
		// GeneralsX: IPlist can be null when the machine has no usable
		// network interface (an iPad with Wi-Fi off). The empty `if (!IPlist)`
		// guard above let execution reach here, and the while loop's own null
		// test meant foundIP stayed FALSE — so this branch dereferenced the
		// null every time. DEBUG_ASSERTCRASH compiles away in release.
	}

	// GeneralsX: The order here is load-bearing, and only accidentally correct: there
	// is a third copy of the local identity, because LANGameInfo's constructor snapshots
	// TheLAN->GetLocalIP() and GameInfo::getLocalSlotNum and amIHost read that snapshot rather
	// than TheLAN. SetLocalIP therefore has to run before any LANGameInfo exists - which the
	// delete and re-new of TheLAN above guarantees. Assert it so that a future reshuffle of
	// this function fails loudly here instead of producing a lobby that accepts a joiner and
	// then silently ejects them.
	DEBUG_ASSERTCRASH(TheLAN->GetMyGame() == nullptr,
		("SetUpOnlineLobbyLAN - a LANGameInfo already exists and has snapshotted the old local IP; SetLocalIP below will not reach it"));
	TheLAN->init();
	TheLAN->SetLocalIP(IP);

	return relayMode;
}

//-------------------------------------------------------------------------------------------------
/** Initialize the WOL Welcome Menu */
//-------------------------------------------------------------------------------------------------
void NetworkDirectConnectInit( WindowLayout *layout, void *userData )
{
	LANbuttonPushed = false;
	LANisShuttingDown = false;

	if (TheLAN == nullptr)
	{
		TheLAN = GXCreateLobbyLANAPI();
		TheLAN->init();
	}
	TheLAN->reset();

	buttonPushed = false;
	isShuttingDown = false;
	TheShell->showShellMap(TRUE);
	buttonBackID = TheNameKeyGenerator->nameToKey( "NetworkDirectConnect.wnd:ButtonBack" );
	buttonHostID = TheNameKeyGenerator->nameToKey( "NetworkDirectConnect.wnd:ButtonHost" );
	buttonJoinID = TheNameKeyGenerator->nameToKey( "NetworkDirectConnect.wnd:ButtonJoin" );
	editPlayerNameID = TheNameKeyGenerator->nameToKey( "NetworkDirectConnect.wnd:EditPlayerName" );
	comboboxRemoteIPID = TheNameKeyGenerator->nameToKey( "NetworkDirectConnect.wnd:ComboboxRemoteIP" );
	staticLocalIPID = TheNameKeyGenerator->nameToKey( "NetworkDirectConnect.wnd:StaticLocalIP" );

	buttonBack = TheWindowManager->winGetWindowFromId( nullptr,  buttonBackID);
	buttonHost = TheWindowManager->winGetWindowFromId( nullptr,	buttonHostID);
	buttonJoin = TheWindowManager->winGetWindowFromId( nullptr,	buttonJoinID);
	editPlayerName = TheWindowManager->winGetWindowFromId( nullptr,	editPlayerNameID);
	comboboxRemoteIP = TheWindowManager->winGetWindowFromId( nullptr,	comboboxRemoteIPID);
	staticLocalIP = TheWindowManager->winGetWindowFromId( nullptr, staticLocalIPID);

	// Matchmaking: relabel the screen when it is a game browser (the .wnd says "Remote IP" over a game list).
	// Done at runtime rather than editing the .wnd inside a .big, and only when a relay is configured:
	// on a plain LAN this is direct connect by IP and the original wording is right.
	{
		Transport *labelTransport = (TheLAN != nullptr) ? TheLAN->getTransport() : nullptr;
		if (labelTransport != nullptr && labelTransport->isRelayEnabled())
		{
			static const struct { const char *control; const char *key; const WideChar *fallback; }
			relabels[] =
			{
				{ "NetworkDirectConnect.wnd:StaticWindowLabel",          "GUI:OnlineGames",     L"ONLINE" },
				{ "NetworkDirectConnect.wnd:StaticRemoteIPDescription",  "GUI:AvailableGames",  L"Games" },
				{ "NetworkDirectConnect.wnd:StaticLocalIPDescription",   "GUI:YourIdentity",    L"You" },
			};

			for (Int i = 0; i < (Int)(sizeof(relabels) / sizeof(relabels[0])); ++i)
			{
				GameWindow *label = TheWindowManager->winGetWindowFromId(
					nullptr, TheNameKeyGenerator->nameToKey(relabels[i].control));
				if (label == nullptr)
					continue;	// a layout without this control is not an error, just older

				// FETCH_OR_SUBSTITUTE so a translation can exist later without another code change;
				// no .csf carries these keys today, so the literal is what shows.
				GadgetStaticTextSetText(label,
					TheGameText->FETCH_OR_SUBSTITUTE(relabels[i].key, relabels[i].fallback));
			}
		}
	}

//	// animate controls
//	TheShell->registerWithAnimateManager(buttonBack, WIN_ANIMATION_SLIDE_LEFT, TRUE, 800);
//	TheShell->registerWithAnimateManager(buttonHost, WIN_ANIMATION_SLIDE_LEFT, TRUE, 600);
//	TheShell->registerWithAnimateManager(buttonJoin, WIN_ANIMATION_SLIDE_LEFT, TRUE, 200);
//
	LANPreferences userprefs;
	UnicodeString name;
	name = userprefs.getUserName();

	if (name.isEmpty())
	{
		name = TheGameText->fetch("GUI:Player");
	}

	GadgetTextEntrySetText(editPlayerName, name);

	PopulateRemoteIPComboBox();

	UnicodeString ipstr;

	// GeneralsX: Matchmaking. Was inline here; extracted so the WOL lobby screen runs the
	// identical sequence rather than a second copy of it that can drift. See SetUpOnlineLobbyLAN.
	SetUpOnlineLobbyLAN();

	// GeneralsX: Server list. Ask the relay what games exist as soon as the transport is
	// up. The replies arrive asynchronously on the same socket, so nothing is waited on here -
	// NetworkDirectConnectUpdate notices them and refills the list.
	if (TheLAN->getTransport() != nullptr && TheLAN->getTransport()->isRelayEnabled())
	{
		TheLAN->getTransport()->requestGameList();
	}

	UnsignedInt ip = TheLAN->GetLocalIP();
	ipstr.format(L"%d.%d.%d.%d", PRINTF_IP_AS_4_INTS(ip));
	GadgetStaticTextSetText(staticLocalIP, ipstr);

	TheLAN->RequestLobbyLeave(true);
	layout->hide(FALSE);
	layout->bringForward();
	TheTransitionHandler->setGroup("NetworkDirectConnectFade");


}

//-------------------------------------------------------------------------------------------------
/** This is called when a shutdown is complete for this menu */
//-------------------------------------------------------------------------------------------------
static void shutdownComplete( WindowLayout *layout )
{

	isShuttingDown = false;

	// hide the layout
	layout->hide( TRUE );

	// our shutdown is complete
	TheShell->shutdownComplete( layout );

}

//-------------------------------------------------------------------------------------------------
/** WOL Welcome Menu shutdown method */
//-------------------------------------------------------------------------------------------------
void NetworkDirectConnectShutdown( WindowLayout *layout, void *userData )
{
	isShuttingDown = true;

	// if we are shutting down for an immediate pop, skip the animations
	Bool popImmediate = *(Bool *)userData;
	if( popImmediate )
	{

		shutdownComplete( layout );
		return;

	}

	TheShell->reverseAnimatewindow();

	TheTransitionHandler->reverse("NetworkDirectConnectFade");
}


//-------------------------------------------------------------------------------------------------
/** WOL Welcome Menu update method */
//-------------------------------------------------------------------------------------------------
void NetworkDirectConnectUpdate( WindowLayout * layout, void *userData)
{
	// GeneralsX: Server list. The relay's replies arrive asynchronously, one datagram per
	// game, so the list fills in over a few frames rather than all at once. Refill the combo box
	// when the count changes rather than every frame, so a player part-way through opening the
	// dropdown does not have it rebuilt under them.
	//
	// The transport is pumped here too: on this screen nothing else does it, and without a pump
	// the replies sit in the socket and the browser stays empty - the same trap that made the
	// headless driver have to pump LANAPI itself.
	static Int lastListedCount = -1;
	if (TheLAN != nullptr && TheLAN->getTransport() != nullptr
		&& TheLAN->getTransport()->isRelayEnabled())
	{
		TheLAN->update();

		const Int count = TheLAN->getTransport()->getGameListCount();
		if (count != lastListedCount)
		{
			lastListedCount = count;
			PopulateRemoteIPComboBox();
		}
	}
	else
	{
		lastListedCount = -1;
	}

	// We'll only be successful if we've requested to
	if(isShuttingDown && TheShell->isAnimFinished() && TheTransitionHandler->isFinished())
		shutdownComplete(layout);
}

//-------------------------------------------------------------------------------------------------
/** WOL Welcome Menu input callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType NetworkDirectConnectInput( GameWindow *window, UnsignedInt msg,
																			 WindowMsgData mData1, WindowMsgData mData2 )
{
	switch( msg )
	{

		// --------------------------------------------------------------------------------------------
		case GWM_CHAR:
		{
			UnsignedByte key = mData1;
			UnsignedByte state = mData2;
			if (buttonPushed)
				break;

			switch( key )
			{

				// ----------------------------------------------------------------------------------------
				case KEY_ESC:
				{

					//
					// send a simulated selected event to the parent window of the
					// back/exit button
					//
					if( BitIsSet( state, KEY_STATE_UP ) )
					{
						TheWindowManager->winSendSystemMsg( window, GBM_SELECTED,
																							(WindowMsgData)buttonBack, buttonBackID );

					}

					// don't let key fall through anywhere else
					return MSG_HANDLED;

				}

			}

		}

	}

	return MSG_IGNORED;
}

//-------------------------------------------------------------------------------------------------
/** WOL Welcome Menu window system callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType NetworkDirectConnectSystem( GameWindow *window, UnsignedInt msg,
														 WindowMsgData mData1, WindowMsgData mData2 )
{
	UnicodeString txtInput;

	switch( msg )
	{


		case GWM_CREATE:
			{

				break;
			}

		case GWM_DESTROY:
			{
				break;
			}

		case GWM_INPUT_FOCUS:
			{
				// if we're givin the opportunity to take the keyboard focus we must say we want it
				if( mData1 == TRUE )
					*(Bool *)mData2 = TRUE;

				return MSG_HANDLED;
			}

		case GBM_SELECTED:
			{
				if (buttonPushed)
					break;

				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();

				if ( controlID == buttonBackID )
				{
					UnicodeString name;
					name = GadgetTextEntryGetText(editPlayerName);

					LANPreferences prefs;
					prefs["UserName"] = UnicodeStringToQuotedPrintable(name);
					prefs.write();

					name.truncateTo(g_lanPlayerNameLength);
					TheLAN->RequestSetName(name);

					buttonPushed = true;
					LANbuttonPushed = true;
					TheShell->pop();
				}
				else if (controlID == buttonHostID)
				{
					HostDirectConnectGame();
				}
				else if (controlID == buttonJoinID)
				{
					JoinDirectConnectGame();
				}
				break;
			}

		case GEM_EDIT_DONE:
			{
				break;
			}
		default:
			return MSG_IGNORED;

	}

	return MSG_HANDLED;
}
