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

// FILE: StagingRoomGameInfo.h //////////////////////////////////////////////////////
// Generals GameSpy GameInfo
// Author: Matthew D. Campbell, Sept 2002

#pragma once

#include "GameNetwork/GameInfo.h"
#include "GameNetwork/Transport.h"

class GameSpyGameSlot : public GameSlot
{
public:
	GameSpyGameSlot();
	Int getProfileID() const { return m_profileID; }
	void setProfileID( Int id ) { m_profileID = id; }
	AsciiString getLoginName() const { return m_gameSpyLogin; }
	void setLoginName( AsciiString name ) { m_gameSpyLogin = name; }
	AsciiString getLocale() const { return m_gameSpyLocale; }
	void setLocale( AsciiString name ) { m_gameSpyLocale = name; }
	Int getWins() const { return m_wins; }
	Int getLosses() const { return m_losses; }
	void setWins( Int wins ) { m_wins = wins; }
	void setLosses( Int losses ) { m_losses = losses; }

	Int getSlotRankPoints() const { return m_rankPoints; }
	Int getFavoriteSide() const { return m_favoriteSide; }
	void setSlotRankPoints( Int val ) { m_rankPoints = val; }
	void setFavoriteSide( Int val ) { m_favoriteSide = val; }

	void setPingString( AsciiString pingStr );
	AsciiString getPingString() const { return m_pingStr; }
	Int getPingAsInt() const { return m_pingInt; }

protected:
	Int m_profileID;
	AsciiString m_gameSpyLogin;
	AsciiString m_gameSpyLocale;

	AsciiString m_pingStr;
	Int m_pingInt;
	Int m_wins, m_losses;
	Int m_rankPoints, m_favoriteSide;
};

/**
  * GameSpyStagingRoom class - maintains information about the GameSpy game and
	* the contents of its slot list throughout the game.
	*/
class GameSpyStagingRoom : public GameInfo
{
private:
	GameSpyGameSlot m_GameSpySlot[MAX_SLOTS];											///< The GameSpy Games Slot List
	UnicodeString m_gameName;
	Int m_id;
	Transport *m_transport;
	AsciiString m_localName;
	Bool m_requiresPassword;
	Bool m_allowObservers;
	UnsignedInt m_version;
	UnsignedInt m_exeCRC;
	UnsignedInt m_iniCRC;
	Bool m_isQM;

	AsciiString m_ladderIP;
	AsciiString m_pingStr;
	Int m_pingInt;
	UnsignedShort m_ladderPort;

	Int m_reportedNumPlayers;
	Int m_reportedMaxPlayers;
	Int m_reportedNumObservers;
	Int m_hostPingMs;			///< GeneralsX: host's own leg to the relay, 0 = unknown. See setPingMs.

public:
	GameSpyStagingRoom();
	virtual void reset() override;

	void cleanUpSlotPointers();
	void setID(Int id) { m_id = id; }
	Int getID() const { return m_id; }

	void setHasPassword(Bool val) { m_requiresPassword = val; }
	Bool getHasPassword() const { return m_requiresPassword; }
	void setAllowObservers(Bool val) { m_allowObservers = val; }
	Bool getAllowObservers() const { return m_allowObservers; }

	void setVersion(UnsignedInt val) { m_version = val; }
	UnsignedInt getVersion() const { return m_version; }
	void setExeCRC(UnsignedInt val) { m_exeCRC = val; }
	UnsignedInt getExeCRC() const { return m_exeCRC; }
	void setIniCRC(UnsignedInt val) { m_iniCRC = val; }
	UnsignedInt getIniCRC() const { return m_iniCRC; }

	void setReportedNumPlayers(Int val) { m_reportedNumPlayers = val; }
	Int getReportedNumPlayers() const { return m_reportedNumPlayers; }

	void setReportedMaxPlayers(Int val) { m_reportedMaxPlayers = val; }
	Int getReportedMaxPlayers() const { return m_reportedMaxPlayers; }

	void setReportedNumObservers(Int val) { m_reportedNumObservers = val; }
	Int getReportedNumObservers() const { return m_reportedNumObservers; }

	void setLadderIP( AsciiString ladderIP ) { m_ladderIP = ladderIP; }
	AsciiString getLadderIP() const { return m_ladderIP; }
	void setLadderPort( UnsignedShort ladderPort ) { m_ladderPort = ladderPort; }
	UnsignedShort getLadderPort() const { return m_ladderPort; }
	void setPingString( AsciiString pingStr );
	AsciiString getPingString() const { return m_pingStr; }
	Int getPingAsInt() const { return m_pingInt; }

	// GeneralsX: Set a ping that is ALREADY in milliseconds.
	//
	// setPingString() cannot be used for this. It hands the string to GameSpyInfo::getPingValue(),
	// which is a GameSpy-specific scheme: it expects a string of hex byte-pairs, one per GameSpy
	// ping server, compares it position-by-position against OUR equivalent string, and scales the
	// best pair by TheGameSpyConfig->getPingTimeoutInMs(). Given a plain number it length-mismatches
	// and returns the full timeout - i.e. "worst possible" - and on the way there it dereferences
	// TheGameSpyConfig unconditionally.
	//
	// The relay path measures a real round trip in milliseconds and has no GameSpy backend at all,
	// so it sets m_pingInt directly. m_pingStr is still filled in, because the browser row treats
	// an EMPTY ping string as "no measurement exists" and leaves the cell blank.
	//
	// Both halves are stored, not just the total. The tooltip wants to show the breakdown, and
	// deriving the host's leg later as (total - ourCurrentLeg) would subtract a LIVE reading from
	// a total frozen when the row was built: the remainder is then a residue of two different
	// instants wearing the label of a measurement. Keeping the half we were given makes both
	// printed numbers the ones that were actually measured.
	void setPingMs( Int totalMs, Int hostLegMs );
	Int getHostPingMs() const { return m_hostPingMs; }	///< host's own leg, 0 = unknown

	virtual Bool amIHost() const override;															///< Convenience function - is the local player the game host?

	GameSpyGameSlot *getGameSpySlot( Int index );

	AsciiString generateGameSpyGameResultsPacket();
	AsciiString generateLadderGameResultsPacket();
	void markGameAsQM() { m_isQM = TRUE; }
	Bool isQMGame() { return m_isQM; }

	virtual void init();
	virtual void resetAccepted() override;															///< Reset the accepted flag on all players

	virtual void startGame(Int gameID) override;														///< Mark our game as started and record the game ID.
	void launchGame();																			///< NAT negotiation has finished - really start
	virtual Int getLocalSlotNum() const override;										///< Get the local slot number, or -1 if we're not present

	void setGameName( UnicodeString name ) { m_gameName = name; }
	UnicodeString getGameName() const { return m_gameName; }

	void setLocalName( AsciiString name ) { m_localName = name; }
};

extern GameSpyStagingRoom *TheGameSpyGame;
