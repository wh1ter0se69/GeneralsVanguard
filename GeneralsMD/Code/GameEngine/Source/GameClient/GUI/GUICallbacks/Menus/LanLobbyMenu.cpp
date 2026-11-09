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
// FILE: LanLobbyMenu.cpp
// Author: Chris Huybregts, October 2001
// Description: Lan Lobby Menu
///////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

// GeneralsX @bugfix Claude 27/07/2026 getDefaultPlayerName() has to tell iOS apart from macOS and
// read the device model; TARGET_OS_IPHONE is only defined once TargetConditionals.h is included.
#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <sys/sysctl.h>
#endif

#include "Lib/BaseType.h"
#include "Common/crc.h"
#include "Common/GameEngine.h"
#include "Common/GlobalData.h"
#include "Common/MultiplayerSettings.h"
#include "Common/NameKeyGenerator.h"
#include "Common/Player.h"
#include "Common/PlayerTemplate.h"
#include "Common/QuotedPrintable.h"
#include "Common/OptionPreferences.h"
#include "GameClient/AnimateWindowManager.h"
#include "GameClient/ClientInstance.h"
#include "GameClient/GameText.h"
#include "GameClient/MapUtil.h"
#include "GameClient/Mouse.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/Gadget.h"
#include "GameClient/Shell.h"
#include "GameClient/ShellHooks.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/GameInfoWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GXLobbyCommands.h"
#include "GameClient/GXPlayerIdentity.h"	// hasVerifiedIdentity - the relay sign-in notice below
#include "GameClient/GXLoginFlow.h"		// takeMessage - the /login drain in LanLobbyMenuUpdate
#include "GameClient/GXMatchChat.h"		// gxMatchWideFromUtf8 - the operator-notice drain below
#include "GameClient/GUICallbacks.h"	// GeneralsX @feature Claude 11/08/2026 GXApplyNickname
#include "GameClient/ChatRateLimit.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/MessageBox.h"
#include "GameClient/GameWindowTransitions.h"
#include "GameLogic/GameLogic.h"
#include "GameNetwork/GXRelaySay.h"		// gxSayVoiceOf - who a drained relay line speaks as
#include "GameNetwork/IPEnumeration.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameNetwork/LANGameInfo.h"
#include "GameNetwork/Transport.h"

Bool LANisShuttingDown = false;
Bool LANbuttonPushed = false;
Bool LANSocketErrorDetected = FALSE;
char *LANnextScreen = nullptr;

static Int	initialGadgetDelay = 2;
static Bool justEntered = FALSE;



LANPreferences::LANPreferences()
{
	loadFromIniFile();
}

LANPreferences::~LANPreferences()
{
}

Bool LANPreferences::loadFromIniFile()
{
	if (rts::ClientInstance::getInstanceId() > 1u)
	{
		AsciiString fname;
		fname.format("Network_Instance%.2u.ini", rts::ClientInstance::getInstanceId());
		return load(fname);
	}

	return load("Network.ini");
}

// GeneralsX @bugfix Claude 27/07/2026 A player name carrying ',' ':' or ';' is refused by the host
// with RET_DUPLICATE_NAME, which reaches the joiner as a misleading "name taken", so a name that
// the game picks on the user's behalf must never contain them.
static AsciiString sanitizeDefaultPlayerName(const AsciiString &name)
{
	AsciiString clean;
	for (Int i = 0; i < name.getLength(); ++i)
	{
		const char c = name.getCharAt(i);
		const Bool isRejected = (c == ',' || c == ':' || c == ';' || (UnsignedByte)c < ' ');
		clean.concat(isRejected ? '-' : c);
	}

	return clean;
}

#if defined(__APPLE__)
// GeneralsX @bugfix Claude 27/07/2026 Returns the device family, "iPad" or "Mac" for example.
// UIDevice is not reachable from C++ and since iOS 16 only reports the model name anyway, so the
// sysctl model string is as close to a device name as this platform gets. The key differs per
// platform: hw.machine is the device on iOS ("iPad14,3") but the cpu on macOS ("arm64"), while
// hw.model is the device on macOS ("Mac14,2") but an internal board name on iOS ("J317xAP").
static AsciiString getAppleDeviceFamily()
{
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	const char *modelKey = "hw.machine";
#else
	const char *modelKey = "hw.model";
#endif

	char model[64];
	size_t modelSize = sizeof(model);
	if (sysctlbyname(modelKey, model, &modelSize, nullptr, 0) != 0)
	{
		return AsciiString::TheEmptyString;
	}
	model[sizeof(model) - 1] = '\0';

	// Drop the model and revision numbers: "iPad14,3" becomes "iPad".
	AsciiString family;
	for (const char *c = model; *c != '\0'; ++c)
	{
		if ((*c < 'a' || *c > 'z') && (*c < 'A' || *c > 'Z'))
			break;

		family.concat(*c);
	}

	return family;
}
#endif

// GeneralsX @bugfix Claude 27/07/2026 iOS has no user facing host name - gethostname() answers
// "localhost" on every device - so the machine name cannot serve as the default player name there.
// Fall back to the device family plus a per device number, because a fixed literal would make the
// second iOS device on the network look like a name clash to the host.
static AsciiString getDefaultPlayerName()
{
	IPEnumeration IPs;
	AsciiString machineName = IPs.getMachineName();

	// Keep the leading label only, because gethostname() can answer in the mDNS "<host>.local" form
	// and the g_lanPlayerNameLength limit would otherwise cut the host name off mid word.
	AsciiString hostLabel;
	machineName.nextToken(&hostLabel, ".");
	hostLabel.trim();

	if (hostLabel.isNotEmpty()
		&& hostLabel.compareNoCase("localhost") != 0
		&& hostLabel.compareNoCase("unknown") != 0)
	{
		return sanitizeDefaultPlayerName(hostLabel);
	}

	AsciiString family;
#if defined(__APPLE__)
	family = getAppleDeviceFamily();
#endif
	if (family.isEmpty())
	{
		family = "Player";
	}
	// Leave room for the "-1234" suffix so the whole name survives the g_lanPlayerNameLength limit
	// instead of being cut where the number is.
	family.truncateTo(g_lanPlayerNameLength - 5);

	// Salt with the home directory, which is the per install container path on iOS. That keeps two
	// devices of the same model from defaulting to the same name.
	CRC crc;
	crc.computeCRC(family.str(), family.getLength());
	const char *home = getenv("HOME");
	if (home != nullptr)
	{
		crc.computeCRC(home, (Int)strlen(home));
	}

	AsciiString name;
	name.format("%s-%04u", family.str(), crc.get() % 10000u);
	return sanitizeDefaultPlayerName(name);
}

UnicodeString LANPreferences::getUserName()
{
	UnicodeString ret;

	LANPreferences::const_iterator it = find("UserName");
	if (it != end())
	{
		// Found an user name. Use it if valid.
		ret = QuotedPrintableToUnicodeString(it->second);
		ret.trim();
		if (!ret.isEmpty())
		{
			return ret;
		}
	}

	if (rts::ClientInstance::getInstanceId() > 1u)
	{
		// TheSuperHackers @feature Use the instance id as default user name
		// to avoid duplicate names for multiple client instances.
		ret.format(L"Instance%.2d", rts::ClientInstance::getInstanceId());
		return ret;
	}

	// Use the machine or device name as default user name.
	ret.translate(getDefaultPlayerName());
	return ret;
}

Int LANPreferences::getPreferredColor()
{
	Int ret;
	LANPreferences::const_iterator it = find("Color");
	if (it == end())
	{
		return -1;
	}

	ret = atoi(it->second.str());
	if (ret < -1 || ret >= TheMultiplayerSettings->getNumColors())
		ret = -1;

	return ret;
}

Int LANPreferences::getPreferredFaction()
{
	Int ret;
	LANPreferences::const_iterator it = find("PlayerTemplate");
	if (it == end())
	{
		return PLAYERTEMPLATE_RANDOM;
	}

	ret = atoi(it->second.str());
	if (ret == PLAYERTEMPLATE_OBSERVER || ret < PLAYERTEMPLATE_MIN || ret >= ThePlayerTemplateStore->getPlayerTemplateCount())
		ret = PLAYERTEMPLATE_RANDOM;

	if (ret >= 0)
	{
		const PlayerTemplate *fac = ThePlayerTemplateStore->getNthPlayerTemplate(ret);
		if (!fac)
			ret = PLAYERTEMPLATE_RANDOM;
		else if (fac->getStartingBuilding().isEmpty())
			ret = PLAYERTEMPLATE_RANDOM;
	}

	return ret;
}

Bool LANPreferences::usesSystemMapDir()
{
	OptionPreferences::const_iterator it = find("UseSystemMapDir");
	if (it == end())
		return TRUE;

	if (stricmp(it->second.str(), "yes") == 0) {
		return TRUE;
	}
	return FALSE;
}

AsciiString LANPreferences::getPreferredMap()
{
	AsciiString ret;
	LANPreferences::const_iterator it = find("Map");
	if (it == end())
	{
		ret = getDefaultMap(TRUE);
		return ret;
	}

	ret = QuotedPrintableToAsciiString(it->second);
	ret.trim();
	if (ret.isEmpty() || !isValidMap(ret, TRUE))
	{
		ret = getDefaultMap(TRUE);
		return ret;
	}

	return ret;
}

Int LANPreferences::getNumRemoteIPs()
{
	Int ret;
	LANPreferences::const_iterator it = find("NumRemoteIPs");
	if (it == end())
	{
		ret = 0;
		return ret;
	}

	ret = atoi(it->second.str());
	return ret;
}

UnicodeString LANPreferences::getRemoteIPEntry(Int i)
{
	UnicodeString ret;
	AsciiString key;
	key.format("RemoteIP%d", i);

	AsciiString ipstr;
	AsciiString asciientry;

	LANPreferences::const_iterator it = find(key.str());
	if (it == end())
	{
		asciientry = "";
		return ret;
	}

	asciientry = it->second;

	asciientry.nextToken(&ipstr, ":");
	asciientry.set(asciientry.str() + 1); // skip the ':'

	ret.translate(ipstr);
	if (!asciientry.isEmpty())
	{
		ret.concat(L"(");
		ret.concat(QuotedPrintableToUnicodeString(asciientry));
		ret.concat(L")");
	}

	return ret;
}

static const char superweaponRestrictionKey[] = "SuperweaponRestrict";

Bool LANPreferences::getSuperweaponRestricted() const
{
  LANPreferences::const_iterator it = find(superweaponRestrictionKey);
  if (it == end())
  {
    return false;
  }

  return ( it->second.compareNoCase( "yes" ) == 0 );
}

void LANPreferences::setSuperweaponRestricted( Bool superweaponRestricted )
{
  (*this)[superweaponRestrictionKey] = superweaponRestricted ? "Yes" : "No";
}

static const char startingCashKey[] = "StartingCash";
Money LANPreferences::getStartingCash() const
{
  LANPreferences::const_iterator it = find(startingCashKey);
  if (it == end())
  {
    return TheMultiplayerSettings->getDefaultStartingMoney();
  }

  Money money;
  money.deposit( strtoul( it->second.str(), nullptr, 10 ), FALSE, FALSE );

  return money;
}

void LANPreferences::setStartingCash( const Money & startingCash )
{
  AsciiString option;

  option.format( "%d", startingCash.countMoney() );

  (*this)[startingCashKey] = option;
}

// PRIVATE DATA ///////////////////////////////////////////////////////////////////////////////////


// window ids ------------------------------------------------------------------------------
static NameKeyType parentLanLobbyID = NAMEKEY_INVALID;
static NameKeyType buttonBackID = NAMEKEY_INVALID;
static NameKeyType buttonClearID = NAMEKEY_INVALID;
static NameKeyType buttonHostID = NAMEKEY_INVALID;
static NameKeyType buttonJoinID = NAMEKEY_INVALID;
static NameKeyType buttonDirectConnectID = NAMEKEY_INVALID;
static NameKeyType buttonEmoteID = NAMEKEY_INVALID;
static NameKeyType staticToolTipID = NAMEKEY_INVALID;
static NameKeyType textEntryPlayerNameID = NAMEKEY_INVALID;
static NameKeyType textEntryChatID = NAMEKEY_INVALID;
static NameKeyType listboxPlayersID = NAMEKEY_INVALID;
static NameKeyType staticTextGameInfoID = NAMEKEY_INVALID;


// Window Pointers ------------------------------------------------------------------------
static GameWindow *parentLanLobby = nullptr;
static GameWindow *buttonBack = nullptr;
static GameWindow *buttonClear = nullptr;
static GameWindow *buttonHost = nullptr;
static GameWindow *buttonJoin = nullptr;
static GameWindow *buttonDirectConnect = nullptr;
static GameWindow *buttonEmote = nullptr;
static GameWindow *staticToolTip = nullptr;
static GameWindow *textEntryPlayerName = nullptr;
static GameWindow *textEntryChat = nullptr;
static GameWindow *staticTextGameInfo = nullptr;

//external declarations of the Gadgets the callbacks can use
NameKeyType listboxChatWindowID = NAMEKEY_INVALID;
GameWindow *listboxChatWindow = nullptr;
GameWindow *listboxPlayers = nullptr;
NameKeyType listboxGamesID = NAMEKEY_INVALID;
GameWindow *listboxGames = nullptr;

// hack to disable framerate limiter in LAN games
//static Bool shellmapOn;
static Bool useFpsLimit;
static UnicodeString defaultName;

static void playerTooltip(GameWindow *window,
													WinInstanceData *instData,
													UnsignedInt mouse)
{
	Int x, y, row, col;
	x = LOLONGTOSHORT(mouse);
	y = HILONGTOSHORT(mouse);

	GadgetListBoxGetEntryBasedOnXY(window, x, y, row, col);

	if (row == -1 || col == -1)
	{
		//TheMouse->setCursorTooltip( TheGameText->fetch("TOOLTIP:LobbyPlayers") );
		return;
	}

	// GeneralsX @bugfix BenderAI 12/02/2026 - Cast via uintptr_t for 64-bit compatibility
	UnsignedInt playerIP = static_cast<UnsignedInt>(reinterpret_cast<uintptr_t>(GadgetListBoxGetItemData( window, row, col )));
	// GeneralsX @bugfix Claude 01/10/2026 The Back button deletes TheLAN while the window can still show tooltips; idea from Generals Online 2df73eca6, reimplemented.
	if (!TheLAN)
		return;
	LANPlayer *player = TheLAN->LookupPlayer(playerIP);
	if (!player)
	{
		DEBUG_CRASH(("No player info in listbox!"));
		//TheMouse->setCursorTooltip( TheGameText->fetch("TOOLTIP:LobbyPlayers") );
		return;
	}

	setLANPlayerTooltip(player);
}


//-------------------------------------------------------------------------------------------------
/** Initialize the Lan Lobby Menu */
//-------------------------------------------------------------------------------------------------
void LanLobbyMenuInit( WindowLayout *layout, void *userData )
{
	LANnextScreen = nullptr;
	LANbuttonPushed = false;
	LANisShuttingDown = false;

	// get the ids for our controls
	parentLanLobbyID = TheNameKeyGenerator->nameToKey( "LanLobbyMenu.wnd:LanLobbyMenuParent" );
	buttonBackID = TheNameKeyGenerator->nameToKey( "LanLobbyMenu.wnd:ButtonBack" );
	buttonClearID = TheNameKeyGenerator->nameToKey( "LanLobbyMenu.wnd:ButtonClear" );
	buttonHostID = TheNameKeyGenerator->nameToKey( "LanLobbyMenu.wnd:ButtonHost" );
	buttonJoinID = TheNameKeyGenerator->nameToKey( "LanLobbyMenu.wnd:ButtonJoin" );
	buttonDirectConnectID = TheNameKeyGenerator->nameToKey( "LanLobbyMenu.wnd:ButtonDirectConnect" );
	buttonEmoteID = TheNameKeyGenerator->nameToKey( "LanLobbyMenu.wnd:ButtonEmote" );
	staticToolTipID = TheNameKeyGenerator->nameToKey( "LanLobbyMenu.wnd:StaticToolTip" );
	textEntryPlayerNameID = TheNameKeyGenerator->nameToKey( "LanLobbyMenu.wnd:TextEntryPlayerName" );
	textEntryChatID = TheNameKeyGenerator->nameToKey( "LanLobbyMenu.wnd:TextEntryChat" );
	listboxPlayersID = TheNameKeyGenerator->nameToKey( "LanLobbyMenu.wnd:ListboxPlayers" );
	listboxChatWindowID = TheNameKeyGenerator->nameToKey( "LanLobbyMenu.wnd:ListboxChatWindowLanLobby" );
	listboxGamesID = TheNameKeyGenerator->nameToKey( "LanLobbyMenu.wnd:ListboxGames" );
	staticTextGameInfoID = TheNameKeyGenerator->nameToKey( "LanLobbyMenu.wnd:StaticTextGameInfo" );


	// Get pointers to the window buttons
	parentLanLobby = TheWindowManager->winGetWindowFromId( nullptr, parentLanLobbyID );
	buttonBack = TheWindowManager->winGetWindowFromId( nullptr,  buttonBackID);
	buttonClear = TheWindowManager->winGetWindowFromId( nullptr,  buttonClearID);
	buttonHost = TheWindowManager->winGetWindowFromId( nullptr, buttonHostID );
	buttonJoin = TheWindowManager->winGetWindowFromId( nullptr, buttonJoinID );
	buttonDirectConnect = TheWindowManager->winGetWindowFromId( nullptr, buttonDirectConnectID );
	buttonEmote = TheWindowManager->winGetWindowFromId( nullptr,buttonEmoteID  );
	staticToolTip = TheWindowManager->winGetWindowFromId( nullptr, staticToolTipID );
	textEntryPlayerName = TheWindowManager->winGetWindowFromId( nullptr, textEntryPlayerNameID );
	textEntryChat = TheWindowManager->winGetWindowFromId( nullptr, textEntryChatID );
	listboxPlayers = TheWindowManager->winGetWindowFromId( nullptr, listboxPlayersID );
	listboxChatWindow = TheWindowManager->winGetWindowFromId( nullptr, listboxChatWindowID );
	listboxGames = TheWindowManager->winGetWindowFromId( nullptr, listboxGamesID );
	staticTextGameInfo = TheWindowManager->winGetWindowFromId( nullptr, staticTextGameInfoID );
	listboxPlayers->winSetTooltipFunc(playerTooltip);

	// Show Menu
	layout->hide( FALSE );

	// GeneralsX @bugfix Claude 22/09/2026 ENTERING THIS SCREEN IS SAYING "I AM LOCAL", and the menu is
	// the only place that knows it.
	//
	// Transport::init already retires a pin minted for a different identity, by comparing the pin with
	// the address the socket is handed. That closes every path that re-binds first - but not this one,
	// because the address handed in BELOW is stale too. TheLAN survives the online screen (the only
	// `TheLAN = nullptr` in these menus is this file's own Back button; WOLLobbyMenu has none and
	// SetUpOnlineLobbyLAN rebuilds on the next line), and LANAPI::reset() did not touch m_localIP - so
	// `TheLAN->init()` a few lines down hands Transport::init the address ONLINE pinned, the equality
	// test says "same identity, keep the pin", and Transport::init ends in sendRelayRegistration().
	// That is a GXRLY into the production room, on the wire, BEFORE the SetLocalIP below rebinds
	// locally and retires the pin. The operative socket is local either way; the datagram has gone.
	//
	// Safe to drop unconditionally: every path that legitimately relays re-enters the room itself
	// before it builds a transport - SetUpOnlineLobbyLAN, relayJoinSelectedGame, JoinDirectConnectGame,
	// HeadlessMatch::setUpLan - so nothing here reads a pin it did not just write.
	//
	// GeneralsX @bugfix Claude 23/09/2026 AND DROPPING THE PIN ALONE CHANGED NOTHING ON THE WIRE. This
	// note used to end "This fixes the room", and it did not: the room ONLINE pins is getLocalHostRoom(),
	// which is also this screen's unpinned fallback, under the default LocalVirtualIP (10.42.0.1) - the
	// same (room, id) the pin held in the common case. Nor could it reach TheLAN->reset() below, whose
	// m_transport->update() can send the surviving transport's keepalive from a registration built
	// long before. So this is now enterLocalScreen(): the pin goes AND nothing relays or registers
	// until the next online entry. Transport.cpp says why that latch cannot break an Online screen.
	//
	// With relay off, nothing widened TheLAN->init()'s bind to the wildcard any more, so on Windows it
	// bound ONLINE's 10.42.x.x - on no interface. LANAPI::reset() below now forgets m_localIP, so that
	// bind is the wildcard a fresh TheLAN makes, and SetLocalIP picks the NIC as before.
	Transport::enterLocalScreen();

	// Init LAN API Singleton
	if (!TheLAN)
	{
		TheLAN = GXCreateLobbyLANAPI();	/// @todo clh delete TheLAN and
		useFpsLimit = TheGlobalData->m_useFpsLimit;
	}
	else
	{
		TheWritableGlobalData->m_useFpsLimit = useFpsLimit;
		TheLAN->reset();
	}

	// Choose an IP address, then initialize the LAN singleton
	UnsignedInt IP = TheGlobalData->m_defaultIP;
	IPEnumeration IPs;
	const WideChar* IPSource;
	if (!IP)
	{
		EnumeratedIP *IPlist = IPs.getAddresses();
		/*
		while (IPlist && IPlist->getNext())
		{
			IPlist = IPlist->getNext();
		}
		*/
		DEBUG_ASSERTCRASH(IPlist, ("No IP addresses found!"));
		if (!IPlist)
		{
			// GeneralsX @bugfix The guard body was empty and execution fell straight
			// through to IPlist->getIP(). Any machine with no usable network
			// interface — an iPad with Wi-Fi off is the everyday case — crashed on
			// opening this screen. DEBUG_ASSERTCRASH above compiles away in release,
			// so it caught nothing where it mattered.
			/// @todo: display an error to the user rather than silently backing out
			DEBUG_LOG(("LanLobbyMenuInit: no local IP addresses; leaving IP unset.\n"));
			IPSource = L"No network";
			IP = 0;
		}
		else
		{
			IPSource = L"Local IP chosen";
			IP = IPlist->getIP();
		}
	}
	else
	{
		IPSource = L"Default local IP";
	}
#if defined(RTS_DEBUG)
	UnicodeString str;
	str.format(L"%s: %d.%d.%d.%d", IPSource, PRINTF_IP_AS_4_INTS(IP));
	GadgetListBoxAddEntryText(listboxChatWindow, str, chatSystemColor, -1, 0);
#endif

	// GeneralsX @bugfix Claude 26/09/2026 (backlog #48, Codex) THE ADDRESS FIRST, THEN init(). init() binds the
	// address TheLAN holds, and that is 0 here - a fresh TheLAN has none, and reset() forgets the last one so that
	// ONLINE's relay address cannot leak in - so it bound the wildcard before SetLocalIP chose. A multi-instance client
	// binds the address it is given, and 0.0.0.0:8086 collides with every other instance's 127.x:8086: EADDRINUSE,
	// a 1 s retry, a DEBUG_CRASH. Handing SetLocalIP the address chosen above first makes every bind on this screen
	// the chosen, current one. The SetLocalIP after init() stays the one whose result counts.
	TheLAN->SetLocalIP(IP);
	// TheLAN->init() sets us to be in a LAN menu screen automatically.
	TheLAN->init();
	if (TheLAN->SetLocalIP(IP) == FALSE) {
		LANSocketErrorDetected = TRUE;
	}

	//Initialize the gadgets on the window
	//UnicodeString	txtInput;
	//txtInput.translate(IPs.getMachineName());
	LANPreferences prefs;
	defaultName = prefs.getUserName();
	defaultName.truncateTo(g_lanPlayerNameLength);

	GadgetTextEntrySetText( textEntryPlayerName, defaultName);
	// Clear the text entry line
	GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);

	GadgetListBoxReset(listboxPlayers);
	GadgetListBoxReset(listboxGames);

	defaultName.truncateTo(g_lanPlayerNameLength);
	TheLAN->RequestSetName(defaultName);
	TheLAN->RequestLocations();

	/*
	UnicodeString unicodeChat;

	unicodeChat = L"Local IP list:";
	GadgetListBoxAddEntryText(listboxChatWindow, unicodeChat, chatSystemColor, -1, 0);

	IPlist = IPs.getAddresses();
	while (IPlist)
	{
		unicodeChat.translate(IPlist->getIPstring());
		GadgetListBoxAddEntryText(listboxChatWindow, unicodeChat, chatSystemColor, -1, 0);
		IPlist = IPlist->getNext();
	}
	*/

	// GeneralsX @feature Claude 20/09/2026 SAY WHY, ON THE ONE SCREEN THAT IS "OUR NETWORKING"
	// WITHOUT LOOKING LIKE IT.
	//
	// The owner's rule: "Online needs sign in anything else not needed but anything that uses our
	// networking needs to be signed in." Online already refuses without an account (GXOnlineGate.h:79).
	// This screen is the gap: NETWORK becomes our networking the moment RelayAddress resolves
	// (Transport.cpp:881) - and nothing here ever said so. A relay that requires sign-in drops an
	// unidentified member 15 s after it joins (relay.js:7781), and a player who was never told would
	//
	// report "the game list is empty", not "I should log in".
	//
	// A NOTICE, NOT A REFUSAL, and that distinction is load-bearing in three ways:
	//   1. The relay decides. A client-side admission check is a check a patched client skips
	//      (Transport.cpp:2385), so refusing here would buy no security and cost the honest player.
	//   2. Whether signed-out players are admitted is the relay's setting, not the client's. A refusal
	//      here would lock them out of a relay that admits them, and would not undo itself on rollback.
	//
	//   3. A GENUINELY LOCAL game on this screen needs no account and never will
	//      (GXIdentityAssertion.h:41), and isRelayEnabled() is false for it, so this says nothing.
	//
	//      BUT "on the LAN screen" IS NOT "genuinely local" TODAY, and an earlier draft of this
	//      comment claimed it was. OptionPreferences::getRelayAddress() returns GX_BOOTSTRAP_RELAY
	//      when RelayAddress is absent OR blank (OptionPreferences.cpp:268-277) - only the literal
	//      "none", or GENERALSX_NO_RELAY, yields the empty string that turns initRelay off. So on a
	//      default install this screen IS relay-enabled, which is exactly why the notice belongs
	//      here and not only in the Online lobby. Do not re-derive "the LAN screen has no relay"
	//      from a read of initRelay alone; the default is not "off", it is "bootstrap".
	//
	//      A separate branch (fix/lan-local-default) is making LAN genuinely local by default. When
	//      it lands, this condition simply stops being true on a LAN-only lobby and the notice stops
	//      appearing there - no change is needed here, because the predicate is already
	//      isRelayEnabled() rather than "is this the LAN screen".
	//
	// AT THE TOP OF THE CHAT BOX, in the system colour, because that is where this screen already
	// talks to the player and it needs no new control, no .wnd change and no repack of WindowZH.big.
	// FETCH_OR_SUBSTITUTE so a translation can land later without another code change.
	{
		const Transport *relayT = (TheLAN != nullptr) ? TheLAN->getTransport() : nullptr;
		if (listboxChatWindow != NULL && relayT != nullptr && relayT->isRelayEnabled()
				&& !GXPlayerIdentity::hasVerifiedIdentity())
		{
			// Two short lines rather than one long one: this listbox is narrow and a wrapped
			// sentence reads as two unrelated fragments. The verb matches the relay's own notice
			// ("Type /login") so the player meets one vocabulary and not two.
			GadgetListBoxAddEntryText(listboxChatWindow,
				TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayNeedsAccount",
					L"This server needs an account to play."),
				chatSystemColor, -1, 0);
			GadgetListBoxAddEntryText(listboxChatWindow,
				TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayNeedsAccountHow",
					L"Type /login to sign in with Discord. LAN games need no account."),
				chatSystemColor, -1, 0);
		}
	}

	// Set Keyboard to Main Parent
	//TheWindowManager->winSetFocus( parentLanLobby );
	TheWindowManager->winSetFocus( textEntryChat );
	CreateLANGameInfoWindow(staticTextGameInfo);

	//TheShell->showShellMap(FALSE);
	//shellmapOn = FALSE;
	// coming out of a game, re-load the shell map
	TheShell->showShellMap(TRUE);

	// check for MOTD
	TheLAN->checkMOTD();
	layout->hide(FALSE);
	layout->bringForward();

	justEntered = TRUE;
	initialGadgetDelay = 2;
	GameWindow *win = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("LanLobbyMenu.wnd:GadgetParent"));
	if(win)
		win->winHide(TRUE);


	// animate controls
	//TheShell->registerWithAnimateManager(parentLanLobby, WIN_ANIMATION_SLIDE_TOP, TRUE);
//	TheShell->registerWithAnimateManager(buttonHost, WIN_ANIMATION_SLIDE_LEFT, TRUE, 600);
//	TheShell->registerWithAnimateManager(buttonJoin, WIN_ANIMATION_SLIDE_LEFT, TRUE, 400);
//	TheShell->registerWithAnimateManager(buttonDirectConnect, WIN_ANIMATION_SLIDE_LEFT, TRUE, 200);
//	//TheShell->registerWithAnimateManager(buttonOptions, WIN_ANIMATION_SLIDE_LEFT, TRUE, 1);
//	TheShell->registerWithAnimateManager(buttonBack, WIN_ANIMATION_SLIDE_RIGHT, TRUE, 1);

}

//-------------------------------------------------------------------------------------------------
/** This is called when a shutdown is complete for this menu */
//-------------------------------------------------------------------------------------------------
static void shutdownComplete( WindowLayout *layout )
{

	LANisShuttingDown = false;

	// hide the layout
	layout->hide( TRUE );

	// our shutdown is complete
	TheShell->shutdownComplete( layout, (LANnextScreen != nullptr) );

	if (LANnextScreen != nullptr)
	{
		TheShell->push(LANnextScreen);
	}

	LANnextScreen = nullptr;

}

//-------------------------------------------------------------------------------------------------
/** Lan Lobby menu shutdown method */
//-------------------------------------------------------------------------------------------------
void LanLobbyMenuShutdown( WindowLayout *layout, void *userData )
{
	LANPreferences prefs;
	prefs["UserName"] = UnicodeStringToQuotedPrintable(GadgetTextEntryGetText( textEntryPlayerName ));
	prefs.write();

	DestroyGameInfoWindow();
	// hide menu
	//layout->hide( TRUE );

	TheLAN->RequestLobbyLeave( true );

	// Reset the LAN singleton
	//TheLAN->reset();

	// our shutdown is complete
	//TheShell->shutdownComplete( layout );
	TheWritableGlobalData->m_useFpsLimit = useFpsLimit;

	LANisShuttingDown = true;

	// if we are shutting down for an immediate pop, skip the animations
	Bool popImmediate = *(Bool *)userData;

	LANSocketErrorDetected = FALSE;

	if( popImmediate )
	{

		shutdownComplete( layout );
		return;

	}

	TheShell->reverseAnimatewindow();
	TheTransitionHandler->reverse("LanLobbyFade");
	//if(	shellmapOn)
//		TheShell->showShellMap(TRUE);
}


//-------------------------------------------------------------------------------------------------
/** Lan Lobby menu update method */
//-------------------------------------------------------------------------------------------------
static void lanLobbySay( const UnicodeString &line, Color color );	// defined below, with the chat seam

void LanLobbyMenuUpdate( WindowLayout * layout, void *userData)
{
	if (TheGameLogic->isInShellGame() && TheGameLogic->getFrame() == 1)
	{
		SignalUIInteraction(SHELL_SCRIPT_HOOK_LAN_ENTERED_FROM_GAME);
	}

	if(justEntered)
	{
		if(initialGadgetDelay == 1)
		{
			TheTransitionHandler->setGroup("LanLobbyFade");
			initialGadgetDelay = 2;
			justEntered = FALSE;
		}
		else
			initialGadgetDelay--;
	}

	if(LANisShuttingDown && TheShell->isAnimFinished() && TheTransitionHandler->isFinished())
		shutdownComplete(layout);

	if (TheShell->isAnimFinished() && !LANbuttonPushed && TheLAN)
		TheLAN->update();

	// GeneralsX @bugfix Claude 20/09/2026 THE RELAY'S OWN VOICE HAD NO RENDER SURFACE ON THIS SCREEN.
	//
	// noticeTo and announceToRoom emit an operator line as GXSAY <room> 0.0.0.0 <text> (relay.js:6629)
	// and need no new client capability - but the sender-zero branch that DRAWS it existed in exactly
	// two places: the Online lobby (WOLLobbyMenu.cpp:5557) and, since 09/09/2026, inside a match
	// (GXMatchChat.h gxMatchDrainOperatorLines). This screen had neither, so every relay-side notice
	// aimed at a player sitting in a relay-backed NETWORK lobby was received, queued and discarded:
	// the identity requirement's "Online play needs a linked account. Type /login..." (relay.js:7784),
	// a ban or kick notice, a mute notice. The player saw a lobby that stopped working and no reason.
	//
	// THE DRAIN IS THE ROOT-CAUSE FIX, not a second bespoke message. One sender-zero branch here
	// renders every operator line this screen can ever receive, including ones added later.
	//
	// PLAYER LINES ARE DRAINED AND DISCARDED, exactly as the match drain does it. A player sender
	// on this queue is somebody's Online-lobby chat: it was not addressed to this screen and putting
	// it here would leak the shared lobby into a LAN one. Draining rather than leaving it also stops
	// one talker filling the bounded 16-slot ring (Transport.h:719-724) and starving the operator
	// out of it.
	//
	// GeneralsX @refactor Claude 25/09/2026 The loop moved to GXMatchChat.h gxLobbyDrainRelayNotices, shared
	// with the game-setup room so the two cannot drift (the 23/09 classification bug above was exactly one
	// copy drifting). Through lanLobbySay, this screen's headless-aware sink.
	//
	// GeneralsX @bugfix Claude 25/09/2026 ONLY WHILE THIS SCREEN IS SHOWING. The shell updates EVERY layout on
	// its stack (Shell.cpp, runUpdate over m_screenStack), and joining a game hides this one under the
	// game-setup room rather than removing it - so this drain went on eating the queue there and rendering
	// operator lines into a listbox nobody could see. Hidden, it leaves the queue to the screen on top.
	if (!layout->isHidden())
		gxLobbyDrainRelayNotices(lanLobbySay, chatSystemColor);

	// GeneralsX @bugfix Claude 23/09/2026 THE LOGIN FLOW'S OWN LINE HAD NO RENDER SURFACE HERE EITHER
	// (Codex round 5, P2). The notice in LanLobbyMenuInit says "Type /login", GXLobbyChatCommand
	// dispatches /login on every screen, and GXLoginFlow::start() reports through a one-slot queue
	// whose only drains were the Online lobby (WOLLobbyMenu.cpp) and the main menu's refusal dialog -
	// so on this screen "Your browser is opening", every refusal (bot mode, no pinned key, no
	// listener, no browser), the timeout and the success were queued and never shown: /login read
	// as ignored, and an unidentified player stayed one until the relay dropped them. Through
	// lanLobbySay, which is also this screen's headless [GXLOBBY] sink.
	{
		UnicodeString loginLine;
		Color loginColor = 0;
		while (GXLoginFlow::takeMessage(&loginLine, &loginColor))
			lanLobbySay(loginLine, loginColor);
	}

	if (LANSocketErrorDetected == TRUE) {
		LANSocketErrorDetected = FALSE;
		DEBUG_LOG(("SOCKET ERROR!  BAILING!"));
		MessageBoxOk(TheGameText->fetch("GUI:NetworkError"), TheGameText->fetch("GUI:SocketError"), nullptr);

		// we have a socket problem, back out to the main menu.
		TheWindowManager->winSendSystemMsg(buttonBack->winGetParent(), GBM_SELECTED,
																			 (WindowMsgData)buttonBack, buttonBackID);
	}


}

//-------------------------------------------------------------------------------------------------
/** Lan Lobby menu input callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType LanLobbyMenuInput( GameWindow *window, UnsignedInt msg,
																			 WindowMsgData mData1, WindowMsgData mData2 )
{
	switch( msg )
	{

		// --------------------------------------------------------------------------------------------
		case GWM_CHAR:
		{
			UnsignedByte key = mData1;
			UnsignedByte state = mData2;
			if (LANbuttonPushed)
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
/** Lan Lobby menu window system callback */
//-------------------------------------------------------------------------------------------------

// GeneralsX @feature Claude 11/08/2026 This screen's seam into the shared lobby command table.
//
// Two passes, and the order matters. GXLobbyChatCommand runs FIRST because the verbs it retires
// (/pass, /nopassword, /lobbyhelp) are still recognised DOWNSTREAM by LANAPI::applyLobbyCommand,
// which this branch deliberately does not edit - LANAPI.cpp is inside the SimID source digest, and
// a help string there would force an atomic four-platform rebuild.
//
// GXLobbyGuardUnknownCommand runs LAST, before the send. It knows from the same table that
// /password, /private and /public are live on this screen and lets those through to RequestChat;
// anything else starting with '/' is refused rather than announced to the whole LAN.
static void lanLobbySay( const UnicodeString &line, Color color )
{
	// Same reasoning as the relay lobby's sink: a headless run has no listbox, and silently dropping
	// the line is what made the lobby untestable without a GUI. [GXLOBBY] is what a harness greps for.
	if (listboxChatWindow == NULL)
	{
		AsciiString plain;
		plain.translate(line);
		fprintf(stderr, "[GXLOBBY] %s\n", plain.str());
		fflush(stderr);
		return;
	}
	GadgetListBoxAddEntryText(listboxChatWindow, line, color, -1, 0);
}

/// TRUE when the line was consumed and must NOT be sent.
static Bool lanLobbyHandleChat( UnicodeString &txtInput )
{
	AsciiString ascii;
	ascii.translate(txtInput);

	if (GXLobbyChatCommand(ascii, GX_LOBBY_SCREEN_LAN, listboxChatWindow, lanLobbySay))
		return TRUE;

	// GeneralsX @feature Claude 11/08/2026 /nick on the browser - and it must ALSO move the name
	// box. LanLobbyMenuShutdown writes prefs["UserName"] from that box unconditionally on the way
	// out, so a /nick that leaves the box alone is silently reverted the moment the player leaves
	// this screen. The box is this screen's real state; outApplied is the normalised name the
	// command actually set. The name comes off the WIDE line (GXLobbyTakeNickRest) - a name is
	// characters, not bytes.
	{
		UnicodeString nickRest;
		if (GXLobbyTakeNickRest(txtInput, nickRest))
		{
			UnicodeString note;
			UnicodeString applied;
			if (GXApplyNickname(nickRest, note, &applied) && textEntryPlayerName != NULL)
				GadgetTextEntrySetText(textEntryPlayerName, applied);
			lanLobbySay(note, chatSystemColor);
			return TRUE;
		}
	}

	Bool escaped = FALSE;
	if (GXLobbyGuardUnknownCommand(ascii, GX_LOBBY_SCREEN_LAN, lanLobbySay, escaped))
		return TRUE;

	// Only the // escape may change what is sent, and then only by one leading character.
	//
	// TRIMMED ON THE WIDE STRING, never re-derived from `ascii`. RequestChat puts these wide
	// characters straight on the wire, and AsciiString::translate casts each WideChar to char -
	// its own @todo says "will only work for 7-bit ascii" - so an unconditional round trip here
	// would mangle every accented, Cyrillic, Arabic or CJK line anyone types on this screen.
	if (escaped)
		txtInput = UnicodeString(txtInput.str() + 1);
	return FALSE;
}

WindowMsgHandledType LanLobbyMenuSystem( GameWindow *window, UnsignedInt msg,
														 WindowMsgData mData1, WindowMsgData mData2 )
{
	UnicodeString txtInput;

	switch( msg )
	{


		case GWM_CREATE:
			{
				SignalUIInteraction(SHELL_SCRIPT_HOOK_LAN_OPENED);
				break;
			}

		case GWM_DESTROY:
			{
				SignalUIInteraction(SHELL_SCRIPT_HOOK_LAN_CLOSED);
				break;
			}

		case GWM_INPUT_FOCUS:
			{
				// if we're givin the opportunity to take the keyboard focus we must say we want it
				if( mData1 == TRUE )
					*(Bool *)mData2 = TRUE;

				return MSG_HANDLED;
			}
		case GLM_DOUBLE_CLICKED:
			{
				if (LANbuttonPushed)
					break;
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();
				if( controlID == listboxGamesID )
				{
					int rowSelected = mData2;

					if (rowSelected >= 0)
					{
						LANGameInfo * theGame = TheLAN->LookupGameByListOffset(rowSelected);
						if (theGame)
						{
							TheLAN->RequestGameJoin(theGame);
						}
					}
				}
				break;
			}
		case GLM_SELECTED:
			{
				if (LANbuttonPushed)
					break;
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();
				if( controlID == listboxGamesID )
				{
					int rowSelected = mData2;
					if( rowSelected < 0 )
					{
						HideGameInfoWindow(TRUE);
						break;
					}
					LANGameInfo * theGame = TheLAN->LookupGameByListOffset(rowSelected);
					if (theGame)
						RefreshGameInfoWindow(theGame, theGame->getName());
					else
						HideGameInfoWindow(TRUE);

				}
				break;
			}
		case GBM_SELECTED:
			{
				if (LANbuttonPushed)
					break;
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();

				if ( controlID == buttonBackID )
				{
					//shellmapOn = TRUE;
					LANbuttonPushed = true;
					DEBUG_LOG(("Back was hit - popping to main menu"));
					TheShell->pop();
					delete TheLAN;
					TheLAN = nullptr;
					//TheTransitionHandler->reverse("LanLobbyFade");

				}
				else if ( controlID == buttonHostID )
				{
					TheLAN->RequestGameCreate( L"", FALSE);

				}
				else if ( controlID == buttonClearID )
				{
					GadgetTextEntrySetText(textEntryPlayerName, UnicodeString::TheEmptyString);
					TheWindowManager->winSendSystemMsg( window,
																						GEM_UPDATE_TEXT,
																						(WindowMsgData)textEntryPlayerName,
																						0 );

				}
				else if ( controlID == buttonJoinID )
				{

					//TheShell->push( "Menus/LanGameOptionsMenu.wnd" );

					int rowSelected = -1;
					GadgetListBoxGetSelected( listboxGames, &rowSelected );

					if (rowSelected >= 0)
					{
						LANGameInfo * theGame = TheLAN->LookupGameByListOffset(rowSelected);
						if (theGame)
						{
							TheLAN->RequestGameJoin(theGame);
						}
					}
					else
					{
						GadgetListBoxAddEntryText(listboxChatWindow, TheGameText->fetch("LAN:ErrorNoGameSelected") , chatSystemColor, -1, 0);
					}

				}
				else if ( controlID == buttonEmoteID )
				{
					// read the user's input
					txtInput.set(GadgetTextEntryGetText( textEntryChat ));
					// Clear the text entry line
					GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);
					// Clean up the text (remove leading/trailing chars, etc)
					txtInput.trim();
					// Echo the user's input to the chat window
					if (!txtInput.isEmpty() && !lanLobbyHandleChat(txtInput)) {
				// GeneralsX @feature Claude 21/08/2026 One chat line every ChatRate::MIN_INTERVAL_MS,
				// shared with every other chat box in the game - see GameClient/ChatRateLimit.h.
				// Checked AFTER the slash-command interception, so a command costs no allowance,
				// and the refused line is put BACK in the box rather than dropped: a rate limit
				// that also eats what you typed is a worse experience than the spam it prevents.
						unsigned int gxChatWait = 0;
						if (!ChatRate::consumeShared(timeGetTime(), &gxChatWait)) {
							GadgetTextEntrySetText(textEntryChat, txtInput);
							GadgetListBoxAddEntryText(listboxChatWindow,
								TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("GUI:ChatRateLimited",
									L"Please wait %d more second(s) before sending another message.",
									(Int)gxChatWait),
								chatSystemColor, -1, 0);
						} else {
							TheLAN->RequestPlayerChat(txtInput);
						}
					}
				}
				else if (controlID == buttonDirectConnectID)
				{
					TheLAN->RequestLobbyLeave( false );
					TheShell->push("Menus/NetworkDirectConnect.wnd");
				}

				break;
			}

		case GEM_UPDATE_TEXT:
			{
				if (LANbuttonPushed)
					break;
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();

				if ( controlID == textEntryPlayerNameID )
				{
					// grab the user's name
					txtInput.set(GadgetTextEntryGetText( textEntryPlayerName ));

					// Clean up the text (remove leading/trailing chars, etc)
					const WideChar *c = txtInput.str();
					while (c && (iswspace(*c)))
						c++;

					if (c)
						txtInput = UnicodeString(c);
					else
						txtInput = UnicodeString::TheEmptyString;

					txtInput.truncateTo(g_lanPlayerNameLength);

					// GeneralsX @feature Claude 11/08/2026 '@' is reserved for the role decoration - a
					// moderator renders as <@Name> and an admin as @Name - so it must not survive into any
					// name. STRIPPED here rather than refused, which is the opposite of GXApplyNickname and
					// is deliberate: this runs on EVERY KEYSTROKE, and a refusal here would pop a message
					// per character typed. It is the same treatment ',' ':' and ';' already get below, and
					// unlike theirs it applies anywhere in the string rather than only at the end - a
					// trailing-only strip would let "a@b" through the moment another character followed.
					if (txtInput.find(L'@') != NULL)
					{
						UnicodeString cleaned;
						for (const WideChar *ch = txtInput.str(); ch && *ch != L'\0'; ++ch)
						{
							if (*ch != L'@')
								cleaned.concat(*ch);
						}
						txtInput = cleaned;
					}

					if (!txtInput.isEmpty() && txtInput.getCharAt(txtInput.getLength()-1) == L',')
						txtInput.removeLastChar(); // we use , for strtok's so we can't allow them in names.  :(

					if (!txtInput.isEmpty() && txtInput.getCharAt(txtInput.getLength()-1) == L':')
						txtInput.removeLastChar(); // we use : for strtok's so we can't allow them in names.  :(

					if (!txtInput.isEmpty() && txtInput.getCharAt(txtInput.getLength()-1) == L';')
						txtInput.removeLastChar(); // we use ; for strtok's so we can't allow them in names.  :(

					// send it over the network
					if (!txtInput.isEmpty())
						TheLAN->RequestSetName(txtInput);
					else
						{
							TheLAN->RequestSetName(defaultName);
						}

					// Put the whitespace-free version in the box
					GadgetTextEntrySetText( textEntryPlayerName, txtInput );

				}
				break;
			}
		case GEM_EDIT_DONE:
			{
				if (LANbuttonPushed)
					break;
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();

				// Take the user's input and echo it into the chat window as well as
				// send it to the other clients on the lan
				if ( controlID == textEntryChatID )
				{

					// read the user's input
					txtInput.set(GadgetTextEntryGetText( textEntryChat ));
					// Clear the text entry line
					GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);
					// Clean up the text (remove leading/trailing chars, etc)
					while (!txtInput.isEmpty() && iswspace(txtInput.getCharAt(0)))
						txtInput = UnicodeString(txtInput.str()+1);

					// Echo the user's input to the chat window
					if (!txtInput.isEmpty() && !lanLobbyHandleChat(txtInput)) {
				// GeneralsX @feature Claude 21/08/2026 One chat line every ChatRate::MIN_INTERVAL_MS,
				// shared with every other chat box in the game - see GameClient/ChatRateLimit.h.
				// Checked AFTER the slash-command interception, so a command costs no allowance,
				// and the refused line is put BACK in the box rather than dropped: a rate limit
				// that also eats what you typed is a worse experience than the spam it prevents.
						unsigned int gxChatWait = 0;
						if (!ChatRate::consumeShared(timeGetTime(), &gxChatWait)) {
							GadgetTextEntrySetText(textEntryChat, txtInput);
							GadgetListBoxAddEntryText(listboxChatWindow,
								TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("GUI:ChatRateLimited",
									L"Please wait %d more second(s) before sending another message.",
									(Int)gxChatWait),
								chatSystemColor, -1, 0);
						} else {
							TheLAN->RequestPlayerChat(txtInput);
						}
					}

				}
				/*
				else if ( controlID == textEntryPlayerNameID )
				{
					// grab the user's name
					txtInput.set(GadgetTextEntryGetText( textEntryPlayerName ));

					// Clean up the text (remove leading/trailing chars, etc)
					txtInput.trim();

					// send it over the network
					if (!txtInput.isEmpty())
						TheLAN->RequestSetName(txtInput);

					// Put the whitespace-free version in the box
					GadgetTextEntrySetText( textEntryPlayerName, txtInput );

				}
				*/
				break;
			}
		default:
			return MSG_IGNORED;

	}

	return MSG_HANDLED;
}
