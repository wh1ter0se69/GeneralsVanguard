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
// FILE: OptionPreferences.cpp
// Author: Matthew D. Campbell, April 2002
// Description: Saving/Loading of option preferences
///////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/AudioSettings.h"
#include "Common/GameAudio.h"
#include "Common/GameLOD.h"
#include "Common/GlobalData.h"
#include "Common/OptionPreferences.h"

#include "GameClient/ClientInstance.h"
#include "GameClient/GXRelayList.h"		// S1-3: the signed relay list's home (getRelayAddress)
#include "GameClient/Display.h"
#include "GameClient/LookAtXlat.h"
#include "GameClient/Mouse.h"

#include "GameLogic/ScriptEngine.h"

#include "GameNetwork/IPEnumeration.h"

OptionPreferences::OptionPreferences()
{
	loadFromIniFile();
}

OptionPreferences::~OptionPreferences()
{
}

Bool OptionPreferences::loadFromIniFile()
{
	if (rts::ClientInstance::getInstanceId() > 1u)
	{
		AsciiString fname;
		fname.format("Options_Instance%.2u.ini", rts::ClientInstance::getInstanceId());
		return load(fname);
	}

	return load("Options.ini");
}

WW3D::MultiSampleModeEnum OptionPreferences::getAntiAliasing() const
{
	OptionPreferences::const_iterator it = find("AntiAliasing");
	if (it == end())
		return WW3D::MULTISAMPLE_MODE_NONE;

	WW3D::MultiSampleModeEnum level = (WW3D::MultiSampleModeEnum)atoi(it->second.str());
	level = clamp(WW3D::MULTISAMPLE_MODE_NONE, level, WW3D::MULTISAMPLE_MODE_8X);
	level = highestBit(level);

	return level;
}

TextureFilterClass::TextureFilterMode OptionPreferences::getTextureFilterMode() const
{
	OptionPreferences::const_iterator it = find("TextureFilter");
	if (it == end())
		return TextureFilterClass::TEXTURE_FILTER_BILINEAR;

	return TextureFilterClass::getTextureFilterMode(it->second.str());
}

TextureFilterClass::AnisotropicFilterMode OptionPreferences::getTextureAnisotropyLevel() const
{
	OptionPreferences::const_iterator it = find("AnisotropyLevel");
	if (it == end())
		return TextureFilterClass::TEXTURE_FILTER_ANISOTROPIC_2X;

	TextureFilterClass::AnisotropicFilterMode level = (TextureFilterClass::AnisotropicFilterMode)atoi(it->second.str());
	level = clamp(TextureFilterClass::TEXTURE_FILTER_ANISOTROPIC_2X, level, TextureFilterClass::TEXTURE_FILTER_ANISOTROPIC_16X);
	level = highestBit(level);

	return level;
}

Int OptionPreferences::getCampaignDifficulty()
{
	OptionPreferences::const_iterator it = find("CampaignDifficulty");
	if (it == end())
		return TheScriptEngine->getGlobalDifficulty();

	Int factor = atoi(it->second.str());
	if (factor < DIFFICULTY_EASY)
		factor = DIFFICULTY_EASY;
	if (factor > DIFFICULTY_HARD)
		factor = DIFFICULTY_HARD;

	return factor;
}

void OptionPreferences::setCampaignDifficulty(Int diff)
{
	AsciiString prefString;
	prefString.format("%d", diff);
	(*this)["CampaignDifficulty"] = prefString;
}

// GeneralsX: IPAddress (LAN) and GameSpyIPAddress as Direct Connect reads it are both
// just a remembered pick from the enumeration - the in-game Options menu writes the list head into
// both on any save - so both follow "Use my VPN for LAN" the same way
// (IPEnumeration::isSavedAddressUsable): a saved address on the side the switch does not prefer
// counts as unset while the preferred side exists. Unset falls back as it always did.
static UnsignedInt gxSavedAddress( const AsciiString &selectedIP )
{
	IPEnumeration IPs;
	EnumeratedIP *head = IPs.getAddresses();
	for (EnumeratedIP *ip = head; ip != nullptr; ip = ip->getNext())
	{
		if (selectedIP.compareNoCase(ip->getIPstring()) == 0)
		{
			if (!IPEnumeration::isSavedAddressUsable(ip->isTunnel(), ip->isCellular(), head->isTunnel()))
			{
				break;
			}
			return ip->getIP();
		}
	}
	return TheGlobalData->m_defaultIP;
}

UnsignedInt OptionPreferences::getLANIPAddress()
{
	return gxSavedAddress((*this)["IPAddress"]);
}

// A saved pick as retail reads it: the address if an interface holds it now, else 0.
static UnsignedInt gxLiveAddress( const AsciiString &selectedIP )
{
	IPEnumeration IPs;
	for (EnumeratedIP *ip = IPs.getAddresses(); ip != nullptr; ip = ip->getNext())
	{
		if (selectedIP.compareNoCase(ip->getIPstring()) == 0)
		{
			return ip->getIP();
		}
	}
	return 0;
}

// GeneralsX: "Use my VPN for LAN", written by the launcher's Options page.
// Absent or anything but "yes" is off, so retail and every existing Options.ini keep the default.
Bool OptionPreferences::getUseVPNForLAN() const
{
	OptionPreferences::const_iterator it = find("UseVPNForLAN");
	return it != end() && stricmp(it->second.str(), "yes") == 0;
}

void OptionPreferences::setLANIPAddress(AsciiString IP)
{
	(*this)["IPAddress"] = IP;
}

void OptionPreferences::setLANIPAddress(UnsignedInt IP)
{
	AsciiString tmp;
	tmp.format("%d.%d.%d.%d", PRINTF_IP_AS_4_INTS(IP));
	(*this)["IPAddress"] = tmp;
}

// GeneralsX: GameSpy chat binds to this (PeerThread), and its
// fallback was m_defaultIP - which is now the LAN election and follows "Use my VPN for LAN", so the LAN-only
// switch could bind chat to a tailnet with no internet route. The fallback is 0.9.47's m_defaultIP instead:
// the saved IPAddress when an interface holds it (retail getLANIPAddress), else m_defaultIP, which in that
// case equals 0.9.47's (0 at startup; an Options-menu Accept writes it and IPAddress alike).
UnsignedInt OptionPreferences::getOnlineIPAddress()
{
	UnsignedInt ip = gxLiveAddress((*this)["GameSpyIPAddress"]);
	if (ip == 0)
	{
		ip = gxLiveAddress((*this)["IPAddress"]);
	}
	return ip != 0 ? ip : TheGlobalData->m_defaultIP;
}

// GeneralsX: What the Options menu's online selector shows, and
// saves on Accept: getOnlineIPAddress when an interface holds it, else the head the list would have with "Use
// my VPN for LAN" off - never the VPN-first head, which would persist a tailnet as GameSpyIPAddress. With the
// switch off that head IS the list head, as before. The menu asks this before its LAN selector rewrites
// IPAddress, so a pick the player saved is read as saved.
UnsignedInt OptionPreferences::getOnlineIPSelection()
{
	const UnsignedInt want = getOnlineIPAddress();
	IPEnumeration IPs;
	EnumeratedIP *best = nullptr;
	for (EnumeratedIP *ip = IPs.getAddresses(); ip != nullptr; ip = ip->getNext())
	{
		if (ip->getIP() == want)
		{
			return want;
		}
		if (best == nullptr || IPEnumeration::sortsBeforeWithoutVPN(ip->getIP(), ip->isTunnel(), ip->isCellular(),
				best->getIP(), best->isTunnel(), best->isCellular()))
		{
			best = ip;
		}
	}
	return best != nullptr ? best->getIP() : 0;
}

// GeneralsX: The same saved key, read for Direct Connect only: Direct Connect
// is where "Use my VPN for LAN" matters (a game joined by IP over a tailnet), while the GameSpy chat
// binding (PeerThread) keeps its retail reading through getOnlineIPAddress above.
UnsignedInt OptionPreferences::getDirectConnectIPAddress()
{
	return gxSavedAddress((*this)["GameSpyIPAddress"]);
}

void OptionPreferences::setOnlineIPAddress(AsciiString IP)
{
	(*this)["GameSpyIPAddress"] = IP;
}

void OptionPreferences::setOnlineIPAddress(UnsignedInt IP)
{
	AsciiString tmp;
	tmp.format("%d.%d.%d.%d", PRINTF_IP_AS_4_INTS(IP));
	(*this)["GameSpyIPAddress"] = tmp;
}

// GeneralsX @feature Relay transport. Defaults let the machine that hosts get away with only
// naming the relay and the room; its opponent swaps the two addresses in its own Options.ini.
static const UnsignedInt DEFAULT_LOCAL_VIRTUAL_IP = (10u << 24) | (42u << 16) | (0u << 8) | 1u;
static const UnsignedInt DEFAULT_PEER_VIRTUAL_IP = (10u << 24) | (42u << 16) | (0u << 8) | 2u;

// GeneralsX @feature A virtual address stands in for a real one everywhere above the transport
// layer, so it has to survive every place the engine handles an IP. 10.0.0.0/8 is mandatory, not
// stylistic: JoinDirectConnectGame builds the address it dials with "(ip1 << 24) + ..." into a
// signed Int, which is undefined behaviour for any first octet >= 128.
static UnsignedInt parseVirtualIP(AsciiString value, UnsignedInt defaultIP)
{
	if (value.isEmpty())
		return defaultIP;

	UnsignedInt octet[4];
	if (sscanf(value.str(), "%u.%u.%u.%u", &octet[0], &octet[1], &octet[2], &octet[3]) != 4)
	{
		DEBUG_LOG(("parseVirtualIP - '%s' is not a dotted quad; ignoring", value.str()));
		return 0;
	}

	for (Int i = 0; i < 4; ++i)
	{
		if (octet[i] > 255)
		{
			DEBUG_LOG(("parseVirtualIP - '%s' has an octet out of range; ignoring", value.str()));
			return 0;
		}
	}

	if (octet[0] != 10)
	{
		DEBUG_LOG(("parseVirtualIP - '%s' is outside 10.0.0.0/8; virtual addresses must be 10.x so the direct connect join path's signed shift stays in range", value.str()));
		return 0;
	}

	return (octet[0] << 24) | (octet[1] << 16) | (octet[2] << 8) | octet[3];
}

// GeneralsX: A fresh install had NO relay and therefore no online game.
//
// getRelayAddress() returned empty when the key was absent, and nothing on any platform ever wrote
// it: the macOS bundle ships no Options.ini at all, the iOS config carries only LOD settings, and
// only the Windows installer's bootstrap publishes one. So a Mac player who unzipped the release
// got "No server configured", could not chat ("not connected to a relay room"), never populated the
// roster, and - if they hosted - was never advertised, because Transport::setGameAdvertisement
// bails when m_relayRoom is empty. Observed on a new player's machine 15/08/2026; the design note
// had already predicted exactly this client: "the only hard
// failure is a client that has never had a manifest and has no RelayAddress".
//
// WHY A COMPILED-IN DEFAULT IS NOT A COMPILED-IN RELAY LIST. 's rule - "adding a relay
// must not require updating clients" - is about the LIST, and it still holds: the fetched, signed
// relay list overrides this, and an explicit RelayAddress in Options.ini overrides it too. This is
// only the bootstrap, which is the role the design doc already assigns it ("RelayAddress stays as
// the bootstrap and the manual override"). A client with no list and no key had nothing to bootstrap
// FROM, which is the whole defect.
//
// "none" IS A DELIBERATE OPT-OUT AND IS LOAD-BEARING. Absence used to mean "no relay", and five
// harnesses rely on that to stay off production - drive-lobby-win.sh, lanbridge-npeer.sh,
// probe-dataid.sh and xplat-machost-play.sh write no RelayAddress at all. Giving every client a
// default would silently point those at the real relay, which is the failure already recorded in
// this project's notes: a local-relay test that hits production and still reports success. So
// "absence" now means "use the bootstrap" and there has to be a way to say "genuinely none" - it is
// spelled `RelayAddress = none`, and those harnesses are updated in the same commit to write it.
// GENERALSX_NO_RELAY IS THE HARNESS-SAFE OPT-OUT, and it exists because there is no shared shell
// library to hook: only one script under scripts/test/ sources a common lib, so "edit every harness"
// would be a list that silently goes stale the next time someone writes one. An env var is a single
// export in whatever wrapper already builds the sandbox HOME, needs no file on disk, and cannot be
// forgotten by a harness that never writes an Options.ini at all - which is the exact shape of the
// four that would otherwise have started talking to the production relay.
static const char GX_BOOTSTRAP_RELAY[] = "relay.wh1ter0se.dev";

static Bool gxRelayDisabledByEnv( void )
{
	const char *v = getenv("GENERALSX_NO_RELAY");
	return (v != NULL && *v != '\0' && *v != '0') ? TRUE : FALSE;
}

// GeneralsX: S1-3 relay-list-v2: THE SIGNED LIST IS NOW CONSULTED, between the pin and the
// bootstrap. The order is unchanged at both ends - GENERALSX_NO_RELAY, then a non-blank RelayAddress (with
// "none" meaning no relay), then the compiled-in name - and the list's HOME slots in only where the bootstrap used to
// be: an absent or blank key. So every harness that writes "none", every pinned lab relay and every Windows install
// (whose launcher writes the list's home into RelayAddress) behaves exactly as it did; Mac, Linux and the iPad now
// follow the list instead of the compiled-in name. The decision itself is gxRelayResolve (GameClient/GXRelayList.cpp,
// tested by scripts/test/relay-list-test.cpp row 6), so it is written once for the host and the port.
// A pin may now carry a lobby port, "host:port" (the Windows launcher writes one for a relay off 8086); a pin that is
// not a plain host[:port] is used verbatim on 8086, so it fails or works exactly as before ports were data.
static GXRelaySource gxResolveRelayPrefs(const OptionPreferences &prefs, char *host, size_t cap,
	UnsignedShort *port, GXRelayEndpoint *home, Bool *haveHome)
{
	*haveHome = FALSE;
	// Checked BEFORE the key, so a sandbox can force "no relay" even over an Options.ini it inherited or one a
	// previous run left behind - and before the list, so such a sandbox never reads or writes the list's files.
	if (gxRelayDisabledByEnv())
	{
		host[0] = '\0';
		*port = GX_RELAYLIST_DEFAULT_PORT;
		return GX_RELAY_SRC_NONE;
	}
	OptionPreferences::const_iterator it = prefs.find("RelayAddress");
	const char *key = (it == prefs.end()) ? NULL : it->second.str();
	// The Windows launcher writes the value it chose here too (gxlaunch.c publish_relay); see gxRelayResolve.
	OptionPreferences::const_iterator lt = prefs.find("RelayAddressLauncher");
	const char *launcherKey = (lt == prefs.end()) ? NULL : lt->second.str();
	*haveHome = GXRelayList::home(home->host, sizeof(home->host), &home->port) ? TRUE : FALSE;
	unsigned short p = GX_RELAYLIST_DEFAULT_PORT;
	const GXRelaySource src = gxRelayResolve(0, key, launcherKey, *haveHome ? home : NULL, GXRelayList::bootstrap(GX_BOOTSTRAP_RELAY), host, cap, &p);
	*port = (UnsignedShort)p;
	return src;
}

AsciiString OptionPreferences::getRelayAddress() const
{
	char host[256];
	UnsignedShort port;
	GXRelayEndpoint home;
	Bool haveHome;
	gxResolveRelayPrefs(*this, host, sizeof(host), &port, &home, &haveHome);
	return AsciiString(host);
}

UnsignedShort OptionPreferences::getRelayPort() const
{
	char host[256];
	UnsignedShort port;
	GXRelayEndpoint home;
	Bool haveHome;
	gxResolveRelayPrefs(*this, host, sizeof(host), &port, &home, &haveHome);
	return port;
}

Bool OptionPreferences::getRelayStandby(Int i, AsciiString &outHost, UnsignedShort &outPort) const
{
	char host[256];
	UnsignedShort port;
	GXRelayEndpoint home;
	Bool haveHome;
	const GXRelaySource src = gxResolveRelayPrefs(*this, host, sizeof(host), &port, &home, &haveHome);
	if (!gxRelayStandbyAllowed(src, host, port, haveHome ? &home : NULL))
		return FALSE;
	char alt[GX_RELAYLIST_HOST_MAX + 1];
	unsigned short altPort = 0;
	if (!GXRelayList::standby(i, alt, sizeof(alt), &altPort))
		return FALSE;
	outHost = alt;
	outPort = (UnsignedShort)altPort;
	return TRUE;
}

Bool OptionPreferences::getRelayListMayFetch() const
{
	char host[256];
	UnsignedShort port;
	GXRelayEndpoint home;
	Bool haveHome;
	const GXRelaySource src = gxResolveRelayPrefs(*this, host, sizeof(host), &port, &home, &haveHome);
	return gxRelayMayFetch(src, host, port, haveHome ? &home : NULL, GXRelayList::bootstrap(GX_BOOTSTRAP_RELAY)) ? TRUE : FALSE;
}

const char *OptionPreferences::getRelayBootstrapHost()
{
	return GXRelayList::bootstrap(GX_BOOTSTRAP_RELAY);
}

AsciiString OptionPreferences::getRelayRoom() const
{
	OptionPreferences::const_iterator it = find("RelayRoom");
	if (it == end())
		return AsciiString::TheEmptyString;

	return it->second;
}

// GeneralsX: The FALLBACK region for the browser's row prefix, used only
// when the relay does not declare one itself (Transport's GXREGION). It exists so that labelling a
// relay correctly is a data change and never a client release - see the note by RELAY_REGION_TAG in
// Transport.cpp. Empty means "no opinion", which leaves Transport on its own default.
AsciiString OptionPreferences::getRelayRegion() const
{
	OptionPreferences::const_iterator it = find("RelayRegion");
	if (it == end())
		return AsciiString::TheEmptyString;

	return it->second;
}

// GeneralsX: The player's own veto on direct connections.
//
// Absent means "no objection", which is the only default that lets the feature exist at all -
// but note it is NOT on its own enough to turn direct connections on. Transport also requires the
// relay to declare the capability, so an unset preference on a relay that says nothing leaves
// every datagram exactly where it is today. See Transport::p2pActive.
//
// Spelled as an opt-OUT rather than an opt-in because the honest description of the setting is
// "hide my address, and pay for it", and a player who wants that wants it for every game rather
// than per match. Matched against "yes" like every other Bool here, so an unrecognised value is
// "no" and cannot half-enable anything.
Bool OptionPreferences::getRelayOnly() const
{
	OptionPreferences::const_iterator it = find("RelayOnly");
	if (it == end())
		return FALSE;

	if (stricmp(it->second.str(), "yes") == 0) {
		return TRUE;
	}
	return FALSE;
}

UnsignedInt OptionPreferences::getLocalVirtualIP() const
{
	OptionPreferences::const_iterator it = find("LocalVirtualIP");
	if (it == end())
		return DEFAULT_LOCAL_VIRTUAL_IP;

	return parseVirtualIP(it->second, DEFAULT_LOCAL_VIRTUAL_IP);
}

UnsignedInt OptionPreferences::getPeerVirtualIP() const
{
	OptionPreferences::const_iterator it = find("PeerVirtualIP");
	if (it == end())
		return DEFAULT_PEER_VIRTUAL_IP;

	return parseVirtualIP(it->second, DEFAULT_PEER_VIRTUAL_IP);
}

Bool OptionPreferences::getArchiveReplaysEnabled() const
{
	// GeneralsX: DEFAULTS ON, because off meant every match was destroyed.
	// RecorderClass writes one replay per match to a FIXED name - lastReplayFileName is the literal
	// "00000000" (Recorder.cpp:96) - so each match overwrites the previous one. archiveReplay()
	// (Recorder.cpp:758, TheSuperHackers @feature Stubbjax 17/10/2025) already copies it to
	// ArchivedReplays/YYYYMMDD_HHMMSS.rep, and it is already portable: GetLocalTime comes from
	// CompatLib/Source/time_compat.cpp, CopyFile is the inline POSIX one in file_compat.h, and
	// getReplayArchiveDir() picks the POSIX separator. The whole feature worked; it was just
	// gated behind a preference nobody had set, so the default was silently throwing the record
	// of every game away.
	//
	// ONLY THE ABSENT CASE CHANGES. An explicit "ArchiveReplays = no" is still honoured below, so
	// anyone who turned it off keeps it off; this only decides for the user who never chose.
	OptionPreferences::const_iterator it = find("ArchiveReplays");
	if (it == end())
		return TRUE;

	if (stricmp(it->second.str(), "yes") == 0) {
		return TRUE;
	}
	return FALSE;
}

Bool OptionPreferences::getAlternateMouseModeEnabled()
{
	OptionPreferences::const_iterator it = find("UseAlternateMouse");
	if (it == end())
		return TheGlobalData->m_useAlternateMouse;

	if (stricmp(it->second.str(), "yes") == 0) {
		return TRUE;
	}
	return FALSE;
}

Bool OptionPreferences::getRightMouseScrollWithAlternateMouseEnabled() const
{
	OptionPreferences::const_iterator it = find("UseRightMouseScrollWithAlternateMouse");
	if (it == end())
		return TheGlobalData->m_useRightMouseScrollWithAlternateMouse;

	if (stricmp(it->second.str(), "yes") == 0) {
		return TRUE;
	}
	return FALSE;
}

Bool OptionPreferences::getRetaliationModeEnabled()
{
	OptionPreferences::const_iterator it = find("Retaliation");
	if (it == end())
		return TheGlobalData->m_clientRetaliationModeEnabled;

	if (stricmp(it->second.str(), "yes") == 0) {
		return TRUE;
	}
	return FALSE;
}

Bool OptionPreferences::getDoubleClickAttackMoveEnabled()
{
	OptionPreferences::const_iterator it = find("UseDoubleClickAttackMove");
	if( it == end() )
		return TheGlobalData->m_doubleClickAttackMove;

	if( stricmp( it->second.str(), "yes" ) == 0 )
		return TRUE;

	return FALSE;
}

Int OptionPreferences::getJpegQuality() const
{
	OptionPreferences::const_iterator it = find("JpegQuality");
	if (it == end())
		return DEFAULT_JPEG_QUALITY;

	// TheSuperHackers @info bobtista 14/07/2026 Clamp the quality to 50-95: above 95 the file
	// size increases significantly with no visible benefit, below 50 the image degrades visibly.
	const Int quality = atoi(it->second.str());
	return clamp(50, quality, 95);
}

Real OptionPreferences::getScrollFactor()
{
	OptionPreferences::const_iterator it = find("ScrollFactor");
	if (it == end())
		return TheGlobalData->m_keyboardDefaultScrollFactor;

	Int factor = atoi(it->second.str());

	// TheSuperHackers @tweak xezon 11/07/2025
	// No longer caps the upper limit to 100, because the options setting can go beyond that.
	// No longer caps the lower limit to 0, because that would mean standstill.
	if (factor < 1)
		factor = 1;

	return factor/100.0f;
}

Bool OptionPreferences::getDrawScrollAnchor()
{
	OptionPreferences::const_iterator it = find("DrawScrollAnchor");
	// TheSuperHackers @info this default is based on the same variable within InGameUi.ini
	if (it == end())
		return FALSE;

	if (stricmp(it->second.str(), "yes") == 0) {
		return TRUE;
	}
	return FALSE;
}

Bool OptionPreferences::getMoveScrollAnchor()
{
	OptionPreferences::const_iterator it = find("MoveScrollAnchor");
	// TheSuperHackers @info this default is based on the same variable within InGameUi.ini
	if (it == end())
		return TRUE;

	if (stricmp(it->second.str(), "yes") == 0) {
		return TRUE;
	}
	return FALSE;
}

Bool OptionPreferences::getCursorCaptureEnabledInWindowedGame() const
{
	OptionPreferences::const_iterator it = find("CursorCaptureEnabledInWindowedGame");
	if (it == end())
		return (CursorCaptureMode_Default & CursorCaptureMode_EnabledInWindowedGame) != 0;

	if (stricmp(it->second.str(), "yes") == 0)
		return TRUE;

	return FALSE;
}

Bool OptionPreferences::getCursorCaptureEnabledInWindowedMenu() const
{
	OptionPreferences::const_iterator it = find("CursorCaptureEnabledInWindowedMenu");
	if (it == end())
		return (CursorCaptureMode_Default & CursorCaptureMode_EnabledInWindowedMenu) != 0;

	if (stricmp(it->second.str(), "yes") == 0)
		return TRUE;

	return FALSE;
}

Bool OptionPreferences::getCursorCaptureEnabledInFullscreenGame() const
{
	OptionPreferences::const_iterator it = find("CursorCaptureEnabledInFullscreenGame");
	if (it == end())
		return (CursorCaptureMode_Default & CursorCaptureMode_EnabledInFullscreenGame) != 0;

	if (stricmp(it->second.str(), "yes") == 0)
		return TRUE;

	return FALSE;
}

Bool OptionPreferences::getCursorCaptureEnabledInFullscreenMenu() const
{
	OptionPreferences::const_iterator it = find("CursorCaptureEnabledInFullscreenMenu");
	if (it == end())
		return (CursorCaptureMode_Default & CursorCaptureMode_EnabledInFullscreenMenu) != 0;

	if (stricmp(it->second.str(), "yes") == 0)
		return TRUE;

	return FALSE;
}

CursorCaptureMode OptionPreferences::getCursorCaptureMode() const
{
	CursorCaptureMode mode = 0;
	mode |= getCursorCaptureEnabledInWindowedGame() ? CursorCaptureMode_EnabledInWindowedGame : 0;
	mode |= getCursorCaptureEnabledInWindowedMenu() ? CursorCaptureMode_EnabledInWindowedMenu : 0;
	mode |= getCursorCaptureEnabledInFullscreenGame() ? CursorCaptureMode_EnabledInFullscreenGame : 0;
	mode |= getCursorCaptureEnabledInFullscreenMenu() ? CursorCaptureMode_EnabledInFullscreenMenu : 0;
	return mode;
}

Bool OptionPreferences::getScreenEdgeScrollEnabledInWindowedApp() const
{
	OptionPreferences::const_iterator it = find("ScreenEdgeScrollEnabledInWindowedApp");
	if (it == end())
		return (ScreenEdgeScrollMode_Default & ScreenEdgeScrollMode_EnabledInWindowedApp) != 0;

	if (stricmp(it->second.str(), "yes") == 0)
		return TRUE;

	return FALSE;
}

Bool OptionPreferences::getScreenEdgeScrollEnabledInFullscreenApp() const
{
	OptionPreferences::const_iterator it = find("ScreenEdgeScrollEnabledInFullscreenApp");
	if (it == end())
		return (ScreenEdgeScrollMode_Default & ScreenEdgeScrollMode_EnabledInFullscreenApp) != 0;

	if (stricmp(it->second.str(), "yes") == 0)
		return TRUE;

	return FALSE;
}

ScreenEdgeScrollMode OptionPreferences::getScreenEdgeScrollMode() const
{
	ScreenEdgeScrollMode mode = 0;
	mode |= getScreenEdgeScrollEnabledInWindowedApp() ? ScreenEdgeScrollMode_EnabledInWindowedApp : 0;
	mode |= getScreenEdgeScrollEnabledInFullscreenApp() ? ScreenEdgeScrollMode_EnabledInFullscreenApp : 0;
	return mode;
}

Bool OptionPreferences::usesSystemMapDir()
{
	OptionPreferences::const_iterator it = find("UseSystemMapDir");
	if (it == end())
		return TRUE;

	if (stricmp(it->second.str(), "yes") == 0) {
		return TRUE;
	}
	return FALSE;
}

Bool OptionPreferences::saveCameraInReplays()
{
	OptionPreferences::const_iterator it = find("SaveCameraInReplays");
	if (it == end())
		return TRUE;

	if (stricmp(it->second.str(), "yes") == 0) {
		return TRUE;
	}
	return FALSE;
}

Bool OptionPreferences::useCameraInReplays()
{
	OptionPreferences::const_iterator it = find("UseCameraInReplays");
	if (it == end())
		return TRUE;

	if (stricmp(it->second.str(), "yes") == 0) {
		return TRUE;
	}
	return FALSE;
}

Bool OptionPreferences::getPlayerObserverEnabled() const
{
	OptionPreferences::const_iterator it = find("PlayerObserverEnabled");
	if (it == end())
		return TRUE;

	if (stricmp(it->second.str(), "yes") == 0)
		return TRUE;

	return FALSE;
}

Int OptionPreferences::getIdealStaticGameDetail()
{
	OptionPreferences::const_iterator it = find("IdealStaticGameLOD");
	if (it == end())
		return STATIC_GAME_LOD_UNKNOWN;

	return TheGameLODManager->getStaticGameLODIndex(it->second);
}

Int OptionPreferences::getStaticGameDetail()
{
	OptionPreferences::const_iterator it = find("StaticGameLOD");
	if (it == end())
		return TheGameLODManager->getStaticLODLevel();

	return TheGameLODManager->getStaticGameLODIndex(it->second);
}

Int OptionPreferences::getFirewallBehavior()
{
	OptionPreferences::const_iterator it = find("FirewallBehavior");
	if (it == end())
		return TheGlobalData->m_firewallBehavior;

	Int behavior = atoi(it->second.str());
	if (behavior < 0)
	{
		behavior = 0;
	}
	return behavior;
}

Short OptionPreferences::getFirewallPortAllocationDelta()
{
	OptionPreferences::const_iterator it = find("FirewallPortAllocationDelta");
	if (it == end()) {
		return TheGlobalData->m_firewallPortAllocationDelta;
	}

	Short delta = atoi(it->second.str());
	return delta;
}

UnsignedShort OptionPreferences::getFirewallPortOverride()
{
	OptionPreferences::const_iterator it = find("FirewallPortOverride");
	if (it == end()) {
		return TheGlobalData->m_firewallPortOverride;
	}

	Int portOverride = atoi(it->second.str());
	if (portOverride < 0 || portOverride > 65535)
		portOverride = 0;
	return portOverride;
}

Bool OptionPreferences::getFirewallNeedToRefresh()
{
	OptionPreferences::const_iterator it = find("FirewallNeedToRefresh");
	if (it == end()) {
		return FALSE;
	}

	Bool retval = FALSE;
	AsciiString str = it->second;
	if (str.compareNoCase("TRUE") == 0) {
		retval = TRUE;
	}
	return retval;
}

AsciiString OptionPreferences::getPreferred3DProvider()
{
	OptionPreferences::const_iterator it = find("3DAudioProvider");
	if (it == end())
		return TheAudio->getAudioSettings()->m_preferred3DProvider[MAX_HW_PROVIDERS];
	return it->second;
}

AsciiString OptionPreferences::getSpeakerType()
{
	OptionPreferences::const_iterator it = find("SpeakerType");
	if (it == end())
		return TheAudio->translateUnsignedIntToSpeakerType(TheAudio->getAudioSettings()->m_defaultSpeakerType2D);
	return it->second;
}

Real OptionPreferences::getSoundVolume()
{
	OptionPreferences::const_iterator it = find("SFXVolume");
	if (it == end())
	{
		Real relative = TheAudio->getAudioSettings()->m_relative2DVolume;
		if( relative < 0 )
		{
			Real scale = 1.0f + relative;
			return TheAudio->getAudioSettings()->m_defaultSoundVolume * 100.0f * scale;
		}
		return TheAudio->getAudioSettings()->m_defaultSoundVolume * 100.0f;
	}

	Real volume = (Real) atof(it->second.str());
	if (volume < 0.0f)
	{
		volume = 0.0f;
	}
	return volume;
}

Real OptionPreferences::get3DSoundVolume()
{
	OptionPreferences::const_iterator it = find("SFX3DVolume");
	if (it == end())
	{
		Real relative = TheAudio->getAudioSettings()->m_relative2DVolume;
		if( relative > 0 )
		{
			Real scale = 1.0f - relative;
			return TheAudio->getAudioSettings()->m_default3DSoundVolume * 100.0f * scale;
		}
		return TheAudio->getAudioSettings()->m_default3DSoundVolume * 100.0f;
	}

	Real volume = (Real) atof(it->second.str());
	if (volume < 0.0f)
	{
		volume = 0.0f;
	}
	return volume;
}

Real OptionPreferences::getSpeechVolume()
{
	OptionPreferences::const_iterator it = find("VoiceVolume");
	if (it == end())
		return TheAudio->getAudioSettings()->m_defaultSpeechVolume * 100.0f;

	Real volume = (Real) atof(it->second.str());
	if (volume < 0.0f)
	{
		volume = 0.0f;
	}
	return volume;
}

Bool OptionPreferences::getCloudShadowsEnabled()
{
	OptionPreferences::const_iterator it = find("UseCloudMap");
	if (it == end())
		return TheGlobalData->m_useCloudMap;

	if (stricmp(it->second.str(), "yes") == 0) {
		return TRUE;
	}
	return FALSE;
}

Bool OptionPreferences::getLightmapEnabled()
{
	OptionPreferences::const_iterator it = find("UseLightMap");
	if (it == end())
		return TheGlobalData->m_useLightMap;

	if (stricmp(it->second.str(), "yes") == 0) {
		return TRUE;
	}
	return FALSE;
}

Bool OptionPreferences::getSmoothWaterEnabled()
{
	OptionPreferences::const_iterator it = find("ShowSoftWaterEdge");
	if (it == end())
		return TheGlobalData->m_showSoftWaterEdge;

	if (stricmp(it->second.str(), "yes") == 0) {
		return TRUE;
	}
	return FALSE;
}

Bool OptionPreferences::getTreesEnabled()
{
	OptionPreferences::const_iterator it = find("ShowTrees");
	if (it == end())
		return TheGlobalData->m_useTrees;

	if (stricmp(it->second.str(), "yes") == 0) {
		return TRUE;
	}
	return FALSE;
}

Bool OptionPreferences::getExtraAnimationsDisabled()
{
	OptionPreferences::const_iterator it = find("ExtraAnimations");
	if (it == end())
		return TheGlobalData->m_useDrawModuleLOD;

	if (stricmp(it->second.str(), "yes") == 0) {
		return FALSE;	//we are enabling extra animations, so disabled LOD
	}
	return TRUE;
}

Bool OptionPreferences::getUseHeatEffects()
{
	OptionPreferences::const_iterator it = find("HeatEffects");
	if (it == end())
		return TheGlobalData->m_useHeatEffects;

	if (stricmp(it->second.str(), "yes") == 0) {
		return TRUE;
	}
	return FALSE;
}

Bool OptionPreferences::getDynamicLODEnabled()
{
	OptionPreferences::const_iterator it = find("DynamicLOD");
	if (it == end())
		return TheGlobalData->m_enableDynamicLOD;

	if (stricmp(it->second.str(), "yes") == 0) {
		return TRUE;
	}
	return FALSE;
}

// GeneralsX: LEGACY, read once at startup and only when no -fps / -noFPSLimit was
// given (GameEngine::init). The launcher before the one max-FPS setting spelled "uncapped" as FPSLimit = no;
// the launcher now reads that key as 0 = unlimited and removes it on its next frame-rate write
// (gxio::options::fps_cap), and a direct launch (run-macos-zh.sh, run-linux-zh.sh) must agree with it.
// Same test the old getter made: present and not stricmp "yes" is off. Nothing in the engine writes the key.
Bool OptionPreferences::getLegacyFPSLimitOff()
{
	OptionPreferences::const_iterator it = find("FPSLimit");
	return it != end() && stricmp(it->second.str(), "yes") != 0;
}

Bool OptionPreferences::get3DShadowsEnabled()
{
	OptionPreferences::const_iterator it = find("UseShadowVolumes");
	if (it == end())
		return TheGlobalData->m_useShadowVolumes;

	if (stricmp(it->second.str(), "yes") == 0) {
		return TRUE;
	}
	return FALSE;
}

Bool OptionPreferences::get2DShadowsEnabled()
{
	OptionPreferences::const_iterator it = find("UseShadowDecals");
	if (it == end())
		return TheGlobalData->m_useShadowDecals;

	if (stricmp(it->second.str(), "yes") == 0) {
		return TRUE;
	}
	return FALSE;
}

Bool OptionPreferences::getBuildingOcclusionEnabled()
{
	OptionPreferences::const_iterator it = find("BuildingOcclusion");
	if (it == end())
		return TheGlobalData->m_enableBehindBuildingMarkers;

	if (stricmp(it->second.str(), "yes") == 0) {
		return TRUE;
	}
	return FALSE;
}

Int OptionPreferences::getParticleCap()
{
	OptionPreferences::const_iterator it = find("MaxParticleCount");
	if (it == end())
		return TheGlobalData->m_maxParticleCount;

	Int factor = (Int) atoi(it->second.str());
	if (factor < 100)	//clamp to at least 100 particles.
		factor = 100;

	return factor;
}

Int OptionPreferences::getTextureReduction()
{
	OptionPreferences::const_iterator it = find("TextureReduction");
	if (it == end())
		return -1;	//unknown texture reduction

	Int factor = (Int) atoi(it->second.str());
	if (factor > 2)	//clamp it.
		factor=2;
	return factor;
}

Real OptionPreferences::getGammaValue()
{
	OptionPreferences::const_iterator it = find("Gamma");
	if (it == end())
		return 50.0f;

	Real gamma = (Real) atoi(it->second.str());
	return gamma;
}

void OptionPreferences::getResolution(Int *xres, Int *yres)
{
	*xres = TheGlobalData->m_xResolution;
	*yres = TheGlobalData->m_yResolution;

	OptionPreferences::const_iterator it = find("Resolution");
	if (it == end())
		return;

	Int selectedXRes,selectedYRes;
	if (sscanf(it->second.str(),"%d%d", &selectedXRes, &selectedYRes) != 2)
		return;

	*xres=selectedXRes;
	*yres=selectedYRes;
}

Real OptionPreferences::getMusicVolume()
{
	OptionPreferences::const_iterator it = find("MusicVolume");
	if (it == end())
		return TheAudio->getAudioSettings()->m_defaultMusicVolume * 100.0f;

	Real volume = (Real) atof(it->second.str());
	if (volume < 0.0f)
	{
		volume = 0.0f;
	}
	return volume;
}

Real OptionPreferences::getMoneyTransactionVolume() const
{
	OptionPreferences::const_iterator it = find("MoneyTransactionVolume");
	if (it == end())
		return TheAudio->getAudioSettings()->m_defaultMoneyTransactionVolume * 100.0f;

	Real volume = (Real) atof(it->second.str());
	if (volume < 0.0f)
		volume = 0.0f;

	return volume;
}

Int OptionPreferences::getNetworkLatencyFontSize()
{
	OptionPreferences::const_iterator it = find("NetworkLatencyFontSize");
	if (it == end())
		return 8;

	Int fontSize = atoi(it->second.str());
	if (fontSize < 0)
	{
		fontSize = 0;
	}
	return fontSize;
}

Int OptionPreferences::getRenderFpsFontSize()
{
	OptionPreferences::const_iterator it = find("RenderFpsFontSize");
	if (it == end())
		return 8;

	Int fontSize = atoi(it->second.str());
	if (fontSize < 0)
	{
		fontSize = 0;
	}
	return fontSize;
}

Int OptionPreferences::getSystemTimeFontSize()
{
	OptionPreferences::const_iterator it = find("SystemTimeFontSize");
	if (it == end())
		return 8;

	Int fontSize = atoi(it->second.str());
	if (fontSize < 0)
	{
		fontSize = 0;
	}
	return fontSize;
}

Int OptionPreferences::getGameTimeFontSize()
{
	OptionPreferences::const_iterator it = find("GameTimeFontSize");
	if (it == end())
		return 8;

	Int fontSize = atoi(it->second.str());
	if (fontSize < 0)
	{
		fontSize = 0;
	}
	return fontSize;
}

// GeneralsX: The in-game message list's size, as a USER preference.
//
// Those messages ("Unit under attack", "Upgrade Complete", "... has been Defeated") are sized by
// MessagePointSize in the InGameUI INI block, overridden by the language data. Neither is reachable
// by a player, and the INI one must NOT become reachable: it goes through the INI parser into
// iniCRC -> dataID, which is a REFUSAL field, so a player who enlarged their own message text would
// be unable to join anyone. That is why this is an Options.ini preference like every other HUD size
// rather than an INI edit.
//
// 0 means "leave the INI/language value alone", which is the default and is why absence returns 0
// rather than a guess at a point size - the shipped value differs per language and inventing one
// here would silently override localisation for everybody who never touched the setting.
Int OptionPreferences::getInGameMessageFontSize()
{
	OptionPreferences::const_iterator it = find("InGameMessageFontSize");
	if (it == end())
		return 0;

	Int fontSize = atoi(it->second.str());
	if (fontSize < 0)
	{
		fontSize = 0;
	}
	// A point size the font library refuses is a SILENT no-op (FontLibrary::getFont returns nullptr
	// above 100), so clamp here where it can be reasoned about rather than letting a typo in
	// Options.ini quietly do nothing.
	if (fontSize > 72)
	{
		fontSize = 72;
	}
	return fontSize;
}

Int OptionPreferences::getPlayerInfoListFontSize()
{
	OptionPreferences::const_iterator it = find("PlayerInfoListFontSize");
	if (it == end())
		return 8;

	Int fontSize = atoi(it->second.str());
	if (fontSize < 0)
	{
		fontSize = 0;
	}
	return fontSize;
}

Real OptionPreferences::getResolutionFontAdjustment()
{
	OptionPreferences::const_iterator it = find("ResolutionFontAdjustment");
	if (it == end())
		return -1.0f;

	Real fontScale = (Real)atof(it->second.str()) / 100.0f;
	if (fontScale < 0.0f)
	{
		fontScale = -1.0f;
	}
	return fontScale;
}

Bool OptionPreferences::getShowMoneyPerMinute() const
{
	OptionPreferences::const_iterator it = find("ShowMoneyPerMinute");
	if (it == end())
		return TheGlobalData->m_showMoneyPerMinute;

	if (stricmp(it->second.str(), "yes") == 0)
	{
		return TRUE;
	}
	return FALSE;
}

Real OptionPreferences::getMaxCameraHeight() const
{
	OptionPreferences::const_iterator it = find("MaxCameraHeight");
	if (it == end())
		return TheGlobalData->m_maxCameraHeight;

	Real val = (Real)atof(it->second.str());
	if (val < 100.0f)
		val = 100.0f;
	if (val > 1000.0f)
		val = 1000.0f;
	return val;
}

Real OptionPreferences::getMinCameraHeight() const
{
	OptionPreferences::const_iterator it = find("MinCameraHeight");
	if (it == end())
		return TheGlobalData->m_minCameraHeight;

	Real val = (Real)atof(it->second.str());
	if (val < 50.0f)
		val = 50.0f;
	if (val > 300.0f)
		val = 300.0f;
	return val;
}

Real OptionPreferences::getCameraPitch() const
{
	OptionPreferences::const_iterator it = find("CameraPitch");
	if (it == end())
		return TheGlobalData->m_cameraPitch;

	Real val = (Real)atof(it->second.str());
	if (val < 20.0f)
		val = 20.0f;
	if (val > 60.0f)
		val = 60.0f;
	return val;
}

Real OptionPreferences::getGameWindowTransitionSpeedMultiplier() const
{
	OptionPreferences::const_iterator it = find("GameWindowTransitionSpeedMultiplier");
	if (it == end())
		return 1.0f;

	Real speed = (Real) atof(it->second.str());
	return clamp(1.0f, speed, 1000.0f);
}
