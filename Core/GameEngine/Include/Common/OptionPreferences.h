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
// FILE: OptionPreferences.h
// Author: Matthew D. Campbell, April 2002
// Description: Saving/Loading of option preferences
///////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "WW3D2/ww3d.h"
#include "WW3D2/texturefilter.h"

#include "Common/UserPreferences.h"

typedef UnsignedInt CursorCaptureMode;
typedef UnsignedInt ScreenEdgeScrollMode;

//-----------------------------------------------------------------------------
// OptionsPreferences options menu class
//-----------------------------------------------------------------------------
class OptionPreferences : public UserPreferences
{
public:
	OptionPreferences();
	virtual ~OptionPreferences() override;

	enum AntiAliasingMode CPP_11(: Int)
	{
		AntiAliasingMode_OFF = 0,
		AntiAliasingMode_MSAA_2X,
		AntiAliasingMode_MSAA_4X,
		AntiAliasingMode_MSAA_8X,
		AntiAliasingMode_Count
	};

	Bool loadFromIniFile();

	WW3D::MultiSampleModeEnum getAntiAliasing() const;
	TextureFilterClass::TextureFilterMode getTextureFilterMode() const;
	TextureFilterClass::AnisotropicFilterMode getTextureAnisotropyLevel() const;
	UnsignedInt getLANIPAddress();
	Bool getUseVPNForLAN() const;
	UnsignedInt getDirectConnectIPAddress();	///< GeneralsX: GameSpyIPAddress under "Use my VPN for LAN"	///< GeneralsX: "UseVPNForLAN", default off (IPEnumeration)
	UnsignedInt getOnlineIPAddress();
	UnsignedInt getOnlineIPSelection();	///< GeneralsX: the Options menu's online pick, never the VPN-first head
	void setLANIPAddress(AsciiString IP);
	void setOnlineIPAddress(AsciiString IP);
	void setLANIPAddress(UnsignedInt IP);
	void setOnlineIPAddress(UnsignedInt IP);
	// GeneralsX @feature Relay transport. RelayAddress being set, together with LocalVirtualIP,
	// is what turns relay mode on. LocalVirtualIP is this machine's identity - what the lobby,
	// the slot list and the in-game connections see - so no machine ever has to know another's
	// public address.
	//
	// PeerVirtualIP is NOT part of routing and is not needed by a host. It is a convenience for
	// the Direct Connect screen only: the address a joiner dials, i.e. the host's virtual
	// identity, offered at the top of the remote-IP list before any history exists. Every other
	// destination is learned from the slot list, exactly as it would be on a LAN, which is what
	// lets a room hold eight peers instead of two.
	AsciiString getRelayAddress() const;
	// GeneralsX @feature Claude 29/10/2026 S1-3 relay-list-v2. The relay's LOBBY port, from the SAME decision as
	// getRelayAddress() (its game port is this + 2): the signed relay list's home port, or the port of a
	// "RelayAddress = host:port" pin, else 8086 - the only port there was before.
	UnsignedShort getRelayPort() const;
	// Standby i of the signed list (S1-4 fails over to it), ONLY when the address in use is the list's own home -
	// no pin, or a pin naming exactly that host and port. A pinned private relay never fails over onto ours.
	Bool getRelayStandby(Int i, AsciiString &host, UnsignedShort &lobbyPort) const;
	// May the Online lobby ask the compiled-in bootstrap for a newer list? No when relay play is off or the pin
	// names somebody else's relay (a lab or private relay must not make a client talk to ours).
	Bool getRelayListMayFetch() const;
	// The compiled-in bootstrap host, for the list's refresh.
	static const char *getRelayBootstrapHost();
	AsciiString getRelayRoom() const;
	AsciiString getRelayRegion() const;
	// GeneralsX @feature Claude 08/08/2026 Direct peer-to-peer, opt-OUT.
	//
	// A direct connection publishes this machine's IP address to everyone else in the match -
	// today only the relay learns it. That is normal for a peer-to-peer game and it is also a
	// change that cannot be undone after the fact, so the control ships in the same change as the
	// feature rather than after it. "RelayOnly = yes" in Options.ini keeps every datagram on the
	// relay: no candidate is ever published, no probe is ever sent, no direct route is ever
	// accepted, and the player pays the relay's latency in exchange for not being seen.
	Bool getRelayOnly() const;
	UnsignedInt getLocalVirtualIP() const;
	UnsignedInt getPeerVirtualIP() const;
	Bool getArchiveReplaysEnabled() const;
	Bool getAlternateMouseModeEnabled();
	Bool getRightMouseScrollWithAlternateMouseEnabled() const;
	Bool getRetaliationModeEnabled();
	Bool getDoubleClickAttackMoveEnabled();
	Int getJpegQuality() const;
	Real getScrollFactor();
	Bool getDrawScrollAnchor();
	Bool getMoveScrollAnchor();
	Bool getCursorCaptureEnabledInWindowedGame() const;
	Bool getCursorCaptureEnabledInWindowedMenu() const;
	Bool getCursorCaptureEnabledInFullscreenGame() const;
	Bool getCursorCaptureEnabledInFullscreenMenu() const;
	CursorCaptureMode getCursorCaptureMode() const;
	Bool getScreenEdgeScrollEnabledInWindowedApp() const;
	Bool getScreenEdgeScrollEnabledInFullscreenApp() const;
	ScreenEdgeScrollMode getScreenEdgeScrollMode() const;
	Int getFirewallBehavior();
	Short getFirewallPortAllocationDelta();
	UnsignedShort getFirewallPortOverride();
	Bool getFirewallNeedToRefresh();
	Bool usesSystemMapDir();
	AsciiString getPreferred3DProvider();
	AsciiString getSpeakerType();
	Real getSoundVolume();
	Real get3DSoundVolume();
	Real getSpeechVolume();
	Real getMusicVolume();
	Real getMoneyTransactionVolume() const;
	Bool saveCameraInReplays();
	Bool useCameraInReplays();
	Bool getPlayerObserverEnabled() const;
	Int getStaticGameDetail();
	Int getIdealStaticGameDetail();
	Real getGammaValue();
	Int getTextureReduction();
	void getResolution(Int *xres, Int *yres);
	Bool get3DShadowsEnabled();
	Bool get2DShadowsEnabled();
	Bool getCloudShadowsEnabled();
	Bool getLightmapEnabled();
	Bool getSmoothWaterEnabled();
	Bool getTreesEnabled();
	Bool getExtraAnimationsDisabled();
	Bool getUseHeatEffects();
	Bool getDynamicLODEnabled();
	Bool getLegacyFPSLimitOff(); ///< GeneralsX: "FPSLimit" present and not "yes" - the old launcher's "uncapped"
	Bool getBuildingOcclusionEnabled();
	Int getParticleCap();

	Int getCampaignDifficulty();
	void setCampaignDifficulty(Int diff);

	Int getNetworkLatencyFontSize();
	Int getRenderFpsFontSize();
	Int getSystemTimeFontSize();
	Int getGameTimeFontSize();
	Int getInGameMessageFontSize();	///< GeneralsX: 0 = leave the INI/language value alone
	Int getPlayerInfoListFontSize();

	Real getResolutionFontAdjustment();

	Bool getShowMoneyPerMinute() const;

	Real getGameWindowTransitionSpeedMultiplier() const;

	Real getMaxCameraHeight() const;
	Real getMinCameraHeight() const;
	Real getCameraPitch() const;
};
