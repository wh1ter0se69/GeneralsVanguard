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

// NetworkInterface.h ///////////////////////////////////////////////////////////////
// Network singleton class - defines interface to Network methods
// Author: Matthew D. Campbell, July 2001

#pragma once

#include <map>
#include "Common/MessageStream.h"
#include "GameNetwork/ConnectionManager.h"
#include "GameNetwork/User.h"
#include "GameNetwork/NetworkDefs.h"

// forward declarations

struct CommandPacket;
class GameMessage;
class GameInfo;

void ClearCommandPacket(UnsignedInt frame);										///< ClearCommandPacket clears the command packet at the start of the frame.
CommandPacket *GetCommandPacket();											///< TheNetwork calls GetCommandPacket to get commands to send.



/**
 * Interface definition for the Network.
 */
class NetworkInterface : public SubsystemInterface
{
protected:

public:
	virtual ~NetworkInterface() override { };

	static NetworkInterface * createNetwork();

	virtual void liteupdate() = 0;															///< does a lightweight update for passing messages around.
	virtual void sendLocalCommands() = 0;											///< GeneralsX: put this peer's translated orders on the wire now, without relaying a frame (H02).

	virtual void setLocalAddress(UnsignedInt ip, UnsignedInt port) = 0;	///< Tell the network what local ip and port to bind to.
	virtual Bool isFrameDataReady() = 0;												///< Are the commands for the next frame available?
	virtual Bool isStalling() = 0;
	virtual void parseUserList( const GameInfo *game ) = 0;						///< Parse a userlist, creating connections
	virtual void startGame() = 0;																	///< Sets the network game frame counter to -1
	virtual UnsignedInt getRunAhead() = 0;												///< Get the current RunAhead value
	virtual UnsignedInt getFrameRate() = 0;												///< Get the current allowed frame rate.
	virtual Int getPacerFramesDue() = 0;													///< GeneralsX: frame periods the pacer has due now (Common/CatchUpPolicy.h); 0 outside a running match
	virtual UnsignedInt getPacketArrivalCushion() = 0;						///< Get the smallest packet arrival cushion since this was last called.

	// Chat functions
	virtual void sendChat(UnicodeString text, RelayMaskType playerMask) = 0;		///< Send a chat line using the normal system.
	virtual void sendDisconnectChat(UnicodeString text) = 0;					///< Send a chat line using the disconnect manager.

	virtual void sendFile(AsciiString path, RelayMaskType playerMask, UnsignedShort commandID) = 0;
	virtual UnsignedShort sendFileAnnounce(AsciiString path, RelayMaskType playerMask) = 0;
	virtual Int getFileTransferProgress(Int playerID, AsciiString path) = 0;
	virtual Bool areAllQueuesEmpty() = 0;

	virtual void quitGame() = 0;																			///< Quit the game right now.

	virtual void selfDestructPlayer(Int index) = 0;

	virtual void voteForPlayerDisconnect(Int slot) = 0;								///< register a vote towards this player's disconnect.
	virtual Bool isPacketRouter() = 0;

	/// Which slot is the packet router, or -1 when that is not known yet.
	///
	/// GeneralsX @feature Claude 19/08/2026 isPacketRouter() answers "am I it", which is enough to
	/// render your OWN row and nothing else. A reader that needs to say anything about the router's
	/// row - and the latency column does - has to be able to name it, because the one figure the
	/// router never has is its own: publishLatencyReport leaves the router's slot at 0, and 0 is the
	/// "no figure" sentinel throughout that path, so every client rendered the host as "--ms" by
	/// construction.
	///
	/// ConnectionManager has known this since the fallback plan was written
	/// (ConnectionManager.h:102); it was simply never on the interface. Returns Int rather than the
	/// UnsignedInt underneath so "not known" is expressible, matching getPeerPingMs and
	/// getSlotAverageFPS which already use -1 for the same purpose.
	virtual Int getPacketRouterSlot() = 0;

	// Bandwidth metrics
	virtual Real getIncomingBytesPerSecond() = 0;
	virtual Real getIncomingPacketsPerSecond() = 0;
	virtual Real getOutgoingBytesPerSecond() = 0;
	virtual Real getOutgoingPacketsPerSecond() = 0;
	virtual Real getUnknownBytesPerSecond() = 0;
	virtual Real getUnknownPacketsPerSecond() = 0;

	virtual void updateLoadProgress( Int percent ) = 0;
	virtual void loadProgressComplete() = 0;
	virtual void sendTimeOutGameStart() = 0;
	virtual UnsignedInt getLocalPlayerID()= 0;
	virtual UnicodeString getPlayerName(Int playerNum)= 0;
	virtual Int getNumPlayers() = 0;

	virtual Int getAverageFPS() = 0;
	virtual Int getSlotAverageFPS(Int slot) = 0;

	/// GeneralsX @feature Claude 04/08/2026 The latency to put in front of a human, in MILLISECONDS.
	///
	/// Converted to milliseconds here rather than at the call site so every consumer agrees on the
	/// unit - the seconds/milliseconds confusion is the obvious way to get a readout that is wrong
	/// by 1000x and still looks plausible.
	///
	/// Negative means "no measurement", which a caller MUST render differently from a low ping.
	///
	/// GeneralsX @fix Claude 10/08/2026 Repointed from the mean at the MINIMUM reduction: every
	/// sample is true round trip plus a slice of one poll interval, so the mean shows each peer its
	/// own frame time as ping (scope 2e - a 25 fps box inflates by ~20 ms). The run-ahead arithmetic
	/// keeps the mean untouched; this accessor exists for humans.
	///
	/// GeneralsX @bugfix Claude 13/08/2026 Now reports the WORST peer and NAMES it, because under
	/// lockstep the worst peer is the only one that can be hurting the match - the previous reduction
	/// ran over a ring that mixes all peers together on the packet router and so returned the
	/// FASTEST. @param fromSlot receives the slot of the peer being reported, or -1 when the return
	/// is negative. It is an out-param rather than a second accessor on purpose: two calls could
	/// straddle a sample and print one peer's name against another peer's number.
	///
	/// HONESTY NOTE FOR CALLERS. On the packet router this is the lobby's worst leg. On any other
	/// peer only one leg exists to measure, so it is that peer's round trip to the router and
	/// nothing at all is known about the other peers. Render the name from @p fromSlot beside the
	/// number so the reading is never mistaken for a lobby-wide claim it cannot support.
	virtual Real getDisplayLatencyMs(Int &fromSlot) = 0;

	/// GeneralsX @feature Claude 13/08/2026 Round trip to @p slot in MILLISECONDS for the in-game
	/// player list, or -1 for "no live figure".
	///
	/// -1 is ORDINARY, not exceptional, and a caller that renders it as 0 has a bug: the slot may
	/// be empty or the local player; the packet router may not have published yet (the first row
	/// is up to m_networkRunAheadMetricsTime into the match); the report may have been lost; or
	/// the figure may have aged out because the peer went quiet. Render a dash.
	///
	/// HONESTY NOTE. In the star topology every one of these is a leg to the PACKET ROUTER, which
	/// is the only latency anybody measures. It is "this player's ping to the host", not a ping
	/// between two arbitrary players - nothing in the engine measures that.
	virtual Int getPeerPingMs(Int slot) = 0;

	virtual void attachTransport(Transport *transport) = 0;
	virtual void initTransport() = 0;
	virtual Bool sawCRCMismatch() = 0;

	/// Report a confirmed desync, and name the peers it happened to.
	///
	/// GeneralsX @feature Claude 17/08/2026 TAKES THE CRC TABLE, rather than being a bare
	/// "something went wrong" flag. Retail pops Menus/CRCMismatch.wnd, which names nobody, and
	/// then ends the game - so the one question every player asks ("who desynced?") had no answer
	/// anywhere the player could see it. The evidence exists at the call site and was being
	/// thrown away one frame later.
	///
	/// The table is passed rather than a pre-formatted string so the wording lives in ONE place
	/// for both games: Generals and Zero Hour each have their own GameLogic.cpp and would
	/// otherwise carry two copies of the same report builder, which is how they drift.
	///
	/// \param playerCRCs  network SLOT -> the CRC that slot reported this interval (GeneralsX 24/09/2026:
	///                    slots, not Player indices, since observer slots share one Player). A slot
	///                    absent from this map reported nothing, which is a DIFFERENT state from
	///                    disagreeing and is rendered differently.
	/// \param numPlayers  how many connected slots were expected to report.
	virtual void setSawCRCMismatch(const std::map<Int, UnsignedInt> &playerCRCs, Int numPlayers) = 0;

	virtual Bool isPlayerConnected(Int playerID) = 0;

	virtual void notifyOthersOfCurrentFrame() = 0;					///< Tells all the other players what frame we are on.
	virtual void notifyOthersOfNewFrame(UnsignedInt frame) = 0;							///< Tells all the other players that we are on a new frame.

	virtual Int  getExecutionFrame() = 0;																			///< Returns the next valid frame for simultaneous command execution.

#if defined(RTS_DEBUG)
	virtual void toggleNetworkOn() = 0;													///< toggle whether or not to send network traffic.
#endif

	// For disconnect blame assignment
	virtual UnsignedInt getPingFrame() = 0;
	virtual Int getPingsSent() = 0;
	virtual Int getPingsReceived() = 0;
};


/**
 * ResolveIP turns a string ("games2.westwood.com", or "192.168.0.1") into
 * a 32-bit unsigned integer.
 */
UnsignedInt ResolveIP(AsciiString host);
