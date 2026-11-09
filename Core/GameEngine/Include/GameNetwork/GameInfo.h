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

// FILE: GameInfo.h //////////////////////////////////////////////////////
// Generals game setup information
// Author: Matthew D. Campbell, December 2001

#pragma once

#include "Common/Snapshot.h"
#include "Common/Money.h"
// GeneralsX @bugfix Claude 06/08/2026 For LOGICFRAMES_PER_SECOND, used by the setCRCInterval cap
// below. Explicit rather than relying on MessageStream.h to drag it in transitively.
#include "Common/GameCommon.h"
#include "GameNetwork/NetworkDefs.h"
#include "GameNetwork/FirewallHelper.h"
#include "GameLogic/GXStress.h"

enum SlotState CPP_11(: Int)
{
	SLOT_OPEN,
	SLOT_CLOSED,
	SLOT_EASY_AI,
	SLOT_MED_AI,
	SLOT_BRUTAL_AI,
	SLOT_PLAYER
};

enum
{
	PLAYERTEMPLATE_RANDOM = -1,
	PLAYERTEMPLATE_OBSERVER = -2,
	PLAYERTEMPLATE_MIN = PLAYERTEMPLATE_OBSERVER
};

/**
  * GameSlot class - maintains information about the contents of a
	* game slot.  This persists throughout the game.
	*/
class GameSlot
{
public:
	GameSlot();
	virtual void reset();

	void setAccept() { m_isAccepted = true; }		///< Accept the current options
	void unAccept();														///< Unaccept (options changed, etc)
	Bool isAccepted() const { return m_isAccepted; }	///< Non-human slots are always accepted

	void setMapAvailability( Bool hasMap );						///< Set whether the slot has the map
	Bool hasMap() const { return m_hasMap; }		///< Non-human slots always have the map

	void setState( SlotState state,
		UnicodeString name = UnicodeString::TheEmptyString,
		UnsignedInt IP = 0);														///< Set the slot's state (human, AI, open, etc)
	SlotState getState() const { return m_state; }		///< Get the slot state

	void setColor( Int color ) { m_color = color; }
	Int getColor() const { return m_color; }

	void setStartPos( Int startPos ) { m_startPos = startPos; }
	Int getStartPos() const { return m_startPos; }

	void setPlayerTemplate( Int playerTemplate )
	{ m_playerTemplate = playerTemplate;
		if (playerTemplate <= PLAYERTEMPLATE_MIN)
			m_startPos = -1;
	 }
	Int getPlayerTemplate() const { return m_playerTemplate; }

	void setTeamNumber( Int teamNumber ) { m_teamNumber = teamNumber; }
	Int getTeamNumber() const { return m_teamNumber; }

	void setName( UnicodeString name ) { m_name = name; }
	UnicodeString getName() const { return m_name; }

	void setIP( UnsignedInt IP ) { m_IP = IP; }
	UnsignedInt getIP() const { return m_IP; }

	void setPort( UnsignedShort port ) { m_port = port; }
	UnsignedShort getPort() const { return m_port; }

	void setNATBehavior( FirewallHelperClass::FirewallBehaviorType NATBehavior) { m_NATBehavior = NATBehavior; }
	FirewallHelperClass::FirewallBehaviorType getNATBehavior() const { return m_NATBehavior; }

	void saveOriginalSetup();
	Bool hasSavedOriginalSetup() const { return m_hasSavedOriginalSetup; }
	Int getOriginalPlayerTemplate() const	{ return m_origPlayerTemplate; }
	Int getOriginalColor() const						{ return m_origColor; }
	Int getOriginalStartPos() const				{ return m_origStartPos; }
	Int getApparentPlayerTemplate() const;
	Int getApparentColor() const;
	Int getApparentStartPos() const;
	UnicodeString getApparentPlayerTemplateDisplayName() const;

	// Various tests
	Bool isHuman() const;															///< Is this slot occupied by a human player?
	Bool isOccupied() const;													///< Is this slot occupied (by a human or an AI)?
	Bool isAI() const;																///< Is this slot occupied by an AI?
	Bool isPlayer( AsciiString userName ) const;						///< Does this slot contain the given user?
	Bool isPlayer( UnicodeString userName ) const;					///< Does this slot contain the given user?
	Bool isPlayer( UnsignedInt ip ) const;									///< Is this slot at this IP?
	Bool isOpen() const;
	/// GeneralsX @feature Claude 24/09/2026 An OBSERVER seat, before the start or after it. The start rewrites
	/// an observer's template from PLAYERTEMPLATE_OBSERVER to the FactionObserver index, and a skirmish save
	/// stores that rewritten value, so after the start only the original template still says "observer".
	/// Every observer seat shares the ReplayObserver, so the start and a load must agree on which they are.
	Bool isObserver() const;

	void setLastFrameInGame( UnsignedInt frame ) { m_lastFrameInGame = frame; }
	void markAsDisconnected() { m_disconnected = TRUE; }
	UnsignedInt lastFrameInGame() const { return m_lastFrameInGame; }
	Bool disconnected() const { return isHuman() && m_disconnected; }

	void mute( Bool isMuted ) { m_isMuted = isMuted; }
	Bool isMuted() const { return m_isMuted; }
protected:
	SlotState m_state;
	Bool m_isAccepted;
	Bool m_hasMap;
	Bool m_isMuted;
	Bool m_hasSavedOriginalSetup;
	Int m_color;																			///< color, or -1 for random
	Int m_startPos;																		///< start position, or -1 for random
	Int m_playerTemplate;															///< PlayerTemplate
	Int m_teamNumber;																	///< alliance, -1 for none
	Int m_origColor;																			///< color, or -1 for random
	Int m_origStartPos;																		///< start position, or -1 for random
	Int m_origPlayerTemplate;															///< PlayerTemplate
	UnicodeString m_name;															///< Only valid for human players
	UnsignedInt m_IP;																	///< Only valid for human players in LAN/WOL
	UnsignedShort m_port;															///< Only valid for human players in LAN/WOL
	FirewallHelperClass::FirewallBehaviorType m_NATBehavior;	///< The NAT behavior for this slot's player.
	UnsignedInt m_lastFrameInGame;	// only valid for human players
	Bool m_disconnected;						// only valid for human players
};

/**
  * GameInfo class - maintains information about the game setup and
	* the contents of its slot list throughout the game.
	*/
/// How much of a typed lobby name survives to the wire.
///
/// GeneralsX @feature Claude 16/08/2026 24 costs 28 announce bytes ("LN=" + 24 + ';'). Measured
/// against the real budget rather than guessed: m_lanMaxOptionsLength is 692, and the twelve-slot
/// worst case is 12*26 + 130 = 442, so this takes 470 of 692. g_announcePrefixAllowance is raised
/// by the same 28 so the existing static_assert keeps covering the whole structure - the budget is
/// the binding constraint on MAX_HUMAN_SLOTS, and a field that quietly ate headroom would surface
/// later as "twelve players stopped announcing" rather than as this change.
/// 23/09/2026: 551 of 692 since the stress battlefield's ST= field (+81, LANAPI.h).
static const Int g_lobbyDisplayNameLength = 24;

class GameInfo
{
public:
	GameInfo();
	virtual ~GameInfo();	///< GeneralsX @bugfix Claude 01/10/2026 clears TheGameInfo if it points here (tsh batch2 #250)

	void init();
	virtual void reset();

	void clearSlotList();

	Int getNumPlayers() const;									///< How many players (human and AI) are in the game?
	Int countSlotsTakingPlayers() const;						///< GeneralsX: occupied slots that each become a Player (Common/PlayerCapacity.h)
	/// GeneralsX @feature Claude 25/09/2026 Is slot `slotNum` an observer that SHARES THE REPLAYOBSERVER - an
	/// occupied observer seat in a game laid out the current way? Every start and mapping decision about an
	/// observer asks this, never GameSlot::isObserver() alone, because a game recorded or saved before observers
	/// shared the ReplayObserver gave each observer a Player of its own, and a replay's commands and a save's
	/// Player table carry the Player INDICES of that layout (Codex r2). setLegacyObserverLayout(TRUE) - a
	/// GENREP/GENR12 replay, a SkirmishGameInfo save older than version 5 - makes this FALSE for every slot, and
	/// the start then builds exactly the table the recording or the save was made with.
	Bool slotSharesReplayObserver( Int slotNum ) const;
	void setLegacyObserverLayout( Bool legacy ) { m_legacyObserverLayout = legacy; }
	Bool isLegacyObserverLayout() const { return m_legacyObserverLayout; }
	Int getNumNonObserverPlayers() const;				///< How many non-observer players (human and AI) are in the game?
	Int getMaxPlayers() const;									///< How many players (human and AI) can be in the game?

	void enterGame();														///< Mark us as having entered the game
	void leaveGame();														///< Mark us as having left the game
	virtual void startGame( Int gameID );											///< Mark our game as started, and record the game ID
	void endGame();															///< Mark us as out of game
	inline Int getGameID() const;								///< Get the game ID of the current game or the last one if we're not in game

	inline void setInGame();										///< set the m_inGame flag
	inline Bool isInGame() const;											///< Are we (in game or in game setup)?  As opposed to chatting, matching, etc
	inline Bool isGameInProgress() const;							///< Is the game in progress?
	inline void setGameInProgress( Bool inProgress ); ///< Set whether the game is in progress or not.
	void setSlot( Int slotNum, GameSlot slotInfo );		///< Set the slot state (human, open, AI, etc)
	GameSlot* getSlot( Int slotNum );									///< Get the slot
	const GameSlot* getConstSlot( Int slotNum ) const;	///< Get the slot
	virtual Bool amIHost() const;															///< Convenience function - is the local player the game host?
	virtual Int getLocalSlotNum() const;				///< Get the local slot number, or -1 if we're not present
	Int getSlotNum( AsciiString userName ) const;			///< Get the slot number corresponding to a specific user, or -1 if he's not present

	// Game options
	/// The lobby's display name. Never the game's identity - see m_lobbyName.
	/// Capped at g_lobbyDisplayNameLength and stripped of the announce's own delimiters.
	void setLobbyName( AsciiString name );
	AsciiString getLobbyName( void ) const { return m_lobbyName; }

	void setMap( AsciiString mapName );								///< Set the map to play on
	void setMapCRC( UnsignedInt mapCRC );							///< Set the map CRC
	void setMapSize( UnsignedInt mapSize );						///< Set the map size
	void setMapContentsMask( Int mask );							///< Set the map contents mask (1=map,2=preview,4=map.ini)
	inline AsciiString getMap() const;								///< Get the game map
	inline UnsignedInt getMapCRC() const;							///< Get the map CRC
	inline UnsignedInt getMapSize() const;						///< Get the map size
	inline Int getMapContentsMask() const;						///< Get the map contents mask
	// GeneralsX @feature Claude 01/10/2026 The host's map-scoped sim data hash (GetMapDataHash, "MD=" on the wire, 0 = the
	// map has no map.ini/solo.ini). A joiner whose own sidecars hash differently reports no map, so the host re-sends
	// them; GameLogic refuses a networked start whose loaded sidecars still differ (M1 Determinism-1 step 2b).
	void setMapDataHash( UnsignedInt hash ) { m_mapDataHash = hash; }
	UnsignedInt getMapDataHash() const { return m_mapDataHash; }
	void setSeed( Int seed );													///< Set the random seed for the game
	inline Int getSeed() const;												///< Get the game seed
	inline Int getUseStats() const;		///< Does this game count towards gamespy stats?
	inline void setUseStats( Int useStats );

  inline UnsignedShort getSuperweaponRestriction() const; ///< Get any optional limits on superweapons
  void setSuperweaponRestriction( UnsignedShort restriction ); ///< Set the optional limits on superweapons
  inline const Money & getStartingCash() const;
  void setStartingCash( const Money & startingCash );

	void setSlotPointer( Int index, GameSlot *slot );	///< Set the slot info pointer

	void setLocalIP( UnsignedInt ip ) { m_localIP =ip; }	///< Set the local IP
	UnsignedInt getLocalIP() const { return m_localIP; }	///< Get the local IP

	Bool isColorTaken(Int colorIdx, Int slotToIgnore = -1 ) const;
	Bool isStartPositionTaken(Int positionIdx, Int slotToIgnore = -1 ) const;

	virtual void resetAccepted();															///< Reset the accepted flag on all players
	virtual void resetStartSpots();						///< reset the start spots for the new map.
	virtual void adjustSlotsForMap();					///< adjusts the slots to open and closed depending on the players in the game and the number of players the map can hold.

	virtual void closeOpenSlots();						///< close all slots that are currently unoccupied.

	// CRC checking hack
	//
	// GeneralsX @bugfix Claude 06/08/2026 THE CAP MUST SCALE WITH THE TICK RATE. At 60 Hz a hard
	// 100 here desynchronises the CRC check itself.
	//
	// The host never comes through this setter: GameInfo::reset() assigns m_crcInterval directly
	// from NET_CRC_INTERVAL (GameInfo.cpp:306, Network.cpp:68), which is
	// (100 * LOGICFRAMES_PER_SECOND) / 30 - so 200 at 60 Hz. Clients DO come through here, applying
	// the host's value as received off the wire (C=%d). With a hard 100 ceiling the host then checks
	// on a 200-frame stride while every client checks on a 100-frame stride, and on each off-phase
	// check only the clients have published - 2 of 3 peers - which GameLogic reports as a shortfall.
	//
	// MEASURED, not reasoned: a 60 Hz three-peer match produced 60 shortfalls, every single one
	// got=2 expected=3, with full checks landing on a 200-frame stride and partials on the same
	// stride offset by 100. Controls at 30 Hz: 1 shortfall in 5211 frames, and 0 in 28031 frames on
	// a longer run. The 60 Hz lobby serialised C=200 while both clients held 100.
	//
	// At 30 Hz the ceiling and the default are both 100, so the clamp is inert and this has been
	// invisible for the entire life of the code. It is a 60 Hz-only defect, and it is exactly the
	// failure Network.cpp:60-67 declares impossible - a mismatched interval "makes peers emit CRCs
	// on different frames ... and reports a FAKE desync indistinguishable from a real one".
	//
	// The clamp is KEPT, because it still bounds a hostile or garbage value arriving from the wire.
	// Only the ceiling changes, and it is now expressed exactly the way the host expresses its own
	// default, so the two cannot drift apart again.
	void setCRCInterval( Int val )
	{
		const Int maxInterval = (100 * LOGICFRAMES_PER_SECOND) / 30;
		// GeneralsX @fix Claude 22/09/2026 The floor was missing, and the ceiling alone does not bound
		// garbage: val arrives as atoi() of the wire/replay `C=` field (GameInfo.cpp:1441/:1932), which
		// is 0 for "0" and for any non-numeric junk. GameLogic then evaluates m_frame % getCRCInterval()
		// unguarded (GameLogic.cpp:4327), so 0 is a divide by zero - #DE on the x86-64 legs, a silent
		// udiv that disables CRC entirely on the arm64 ones - and a negative passes the ceiling test and
		// becomes 0xFFFFFFFF in that unsigned modulo, switching desync detection off with no crash.
		// Anything out of range falls back to maxInterval, which is the host's own default.
		m_crcInterval = ((val >= 1) && (val < maxInterval)) ? val : maxInterval;
	}
	Int getCRCInterval() const { return m_crcInterval; }

	Bool haveWeSurrendered() { return m_surrendered; }
	void markAsSurrendered() { m_surrendered = TRUE; }

	Bool isSkirmish(); // TRUE if 1 human & 1+ AI are present && !isSandbox()
	Bool isMultiPlayer(); // TRUE if 2+ human are present
	Bool isSandbox(); // TRUE if everybody is on the same team

	/// GeneralsX @feature Claude 27/09/2026 THE START RULE, one for every setup screen (LAN and relay lobbies,
	/// GameSpy, Skirmish) and for the match's end report, and inside the SimID digest, so two builds that
	/// disagree on it never share a lobby (owner, 27/09/2026). A lobby may start when at least one occupied
	/// slot PLAYS - an AI or a human player; every human may observe. Such a start is a SANDBOX when no human
	/// plays or every player is on one side: it counts in nobody's stats, ranking or win-loss, and its record
	/// says so.
	enum StartVerdict
	{
		START_NOBODY_PLAYS = 0,	///< refused: no occupied slot plays
		START_MATCH,				///< a match: a human plays, on one of at least two sides
		START_SANDBOX				///< may start, counted for nobody
	};
	StartVerdict getStartVerdict() const;

	Bool isPlayerPreorder(Int index);
	void markPlayerAsPreorder(Int index);

  inline Bool oldFactionsOnly() const;
  inline void setOldFactionsOnly( Bool oldFactionsOnly );

	/// GeneralsX @feature Claude 23/09/2026 The lobby-agreed stress battlefield ("ST=" on the wire,
	/// see GameLogic/GXStress.h). Set only by a HOST, from its own environment (HeadlessMatch);
	/// every other peer gets it from the host's options string and never from its own environment.
	const GXStressOption &getGxStress() const { return m_gxStress; }
	void setGxStress( const GXStressOption &stress ) { m_gxStress = stress; }

	/// GeneralsX @security Claude 25/09/2026 The slots the RELAY proves are admins in this match ("AD=" on the
	/// wire, bit i = slot i). The host stamps it on MSG_GAME_START from its own relay roster, every other peer
	/// adopts it from there and refuses to start unless its OWN roster confirms every bit (LANAPI::gxRelayAdminMask),
	/// and GameLogic snapshots it on a new game - so a cheat order is accepted from exactly these slots, on every
	/// peer and in the replay. Masked to MAX_SLOTS bits.
	// GeneralsX @feature Claude 06/11/2026 scale32: RelayMaskType (64 bit), ALL_SLOTS_MASK.
	RelayMaskType getGxAdminMask() const { return m_gxAdminMask; }
	void setGxAdminMask( RelayMaskType mask ) { m_gxAdminMask = mask & ALL_SLOTS_MASK; }
	/// ...and each admin slot's per-match public key (Common/GXAdminOrder.h), which its cheat orders must be signed
	/// under. 32 zero bytes for a slot that has none. Stamped and verified exactly like the mask; "AD=" carries both.
	const unsigned char *getGxAdminKey( Int slot ) const { return m_gxAdminKeys[( slot >= 0 && slot < MAX_SLOTS ) ? slot : 0]; }
	void setGxAdminKey( Int slot, const unsigned char key[32] ) { if( slot >= 0 && slot < MAX_SLOTS ) memcpy( m_gxAdminKeys[slot], key, 32 ); }
	void clearGxAdmin() { m_gxAdminMask = 0; memset( m_gxAdminKeys, 0, sizeof( m_gxAdminKeys ) ); }

	/// GeneralsX @feature Claude 07/11/2026 AI in three colours (GameNetwork/GXAiColours.h). stampAiColours is the host's choice at
	/// Start, from its own Options.ini; applyAiColours is the logic's, after every colour has been drawn. A replay ("AC=" in its header)
	/// and a save carry the mode, so playing one back follows the recording and never the viewer's own setting.
	Int getGxAiColours() const { return m_gxAiColours; }
	void setGxAiColours( Int mode ) { m_gxAiColours = mode; }
	void stampAiColours();
	void applyAiColours();

protected:
	RelayMaskType m_preorderMask;	// GeneralsX @feature Claude 06/11/2026 scale32: 64 bit, bit per slot
	Int m_crcInterval;
	Bool m_inGame;
	Bool m_inProgress;
	Bool m_surrendered;
	Int m_gameID;
	GameSlot *m_slot[MAX_SLOTS];

	UnsignedInt m_localIP;

	// GeneralsX @feature Claude 16/08/2026 THE LOBBY'S DISPLAY NAME, which is NOT LANGameInfo's
	// name. That one (LANGameInfo::getName) is the LOOKUP KEY - LANAPI.cpp:2239 finds a game with
	// `while (theGame && theGame->getName() != gameName)` - which is why it is built as
	// "%8.8X%8.8X" of IP and seed and why a human name concatenated onto it is truncated away by
	// g_lanGameNameLength. It cannot carry text and stay unique, so this is a separate field.
	//
	// Before this existed there was nowhere to put the name the host types in PopupHostGame, so it
	// was pushed through RequestSetName - i.e. it overwrote the host's PLAYER name, and the host's
	// own slot in GAME OPTIONS showed the lobby name instead of who they are.
	//
	// The relay region is deliberately NOT stored here. Every peer in a lobby is on the same relay
	// and each already knows its own region from the signed manifest, so "[EU] " is applied at
	// display time from Transport::getRelayRegion() - the single runtime source the browser column
	// already uses (WOLLobbyMenu.cpp:1754). Putting it on the wire would cost bytes out of a budget
	// that is already the binding constraint on MAX_HUMAN_SLOTS, to send every peer a value it
	// can derive.
	AsciiString m_lobbyName;

	// Game options
	AsciiString m_mapName;
	UnsignedInt m_mapCRC;
	UnsignedInt m_mapSize;
	UnsignedInt m_mapDataHash;
	Int m_mapMask;
	Int m_seed;
	Int m_useStats;
  Money         m_startingCash;
  UnsignedShort m_superweaponRestriction;
  Bool m_oldFactionsOnly; // Only USA, China, GLA -- not USA Air Force General, GLA Toxic General, et al
	GXStressOption m_gxStress;	///< all zero = off
	RelayMaskType m_gxAdminMask;	///< relay-proven admin slots; 0 = none
	unsigned char m_gxAdminKeys[MAX_SLOTS][32];	///< each admin slot's per-match public key; zero = none
	Bool m_legacyObserverLayout;	///< GeneralsX: every observer takes a Player of its own (see slotSharesReplayObserver)
	Int m_gxAiColours;	///< GeneralsX: GXAiColours::Mode, 0 = every seat its own colour
};

extern GameInfo *TheGameInfo;

// Inline functions
Int					GameInfo::getGameID() const								{ return m_gameID; }
AsciiString	GameInfo::getMap() const									{ return m_mapName; }
UnsignedInt	GameInfo::getMapCRC() const								{ return m_mapCRC; }
UnsignedInt	GameInfo::getMapSize() const							{ return m_mapSize; }
Int					GameInfo::getMapContentsMask() const			{ return m_mapMask; }
Int					GameInfo::getSeed() const									{ return m_seed; }
Bool				GameInfo::isInGame() const								{ return m_inGame; }
void				GameInfo::setInGame()											{ m_inGame = true; }
Bool				GameInfo::isGameInProgress() const				{ return m_inProgress; }
void				GameInfo::setGameInProgress( Bool inProgress )	{ m_inProgress = inProgress; }
Int					GameInfo::getUseStats() const             { return m_useStats; }
void				GameInfo::setUseStats( Int useStats )           { m_useStats = useStats; }
const Money&GameInfo::getStartingCash() const         { return m_startingCash; }
UnsignedShort GameInfo::getSuperweaponRestriction() const { return m_superweaponRestriction; }
Bool        GameInfo::oldFactionsOnly() const           { return m_oldFactionsOnly; }
void        GameInfo::setOldFactionsOnly( Bool oldFactionsOnly ) { m_oldFactionsOnly = oldFactionsOnly; }

// GeneralsX @security Claude 25/09/2026 maxLength 0 is the lobby announce's room (the binding limit for every
// string a lobby sends), and such a string never carries the admin field. A positive maxLength asks for a string that
// is never announced - the replay header passes GX_REPLAY_OPTIONS_MAX - and only that one carries it (Codex r2 P2:
// with the announce limit a crowded lobby with an admin wrote an EMPTY slot list into its replay; Codex r3 P2: with
// the field in announces an in-progress game could stop being listed).
// GeneralsX @feature Claude 06/11/2026 1023 -> 4095 with readAsciiString's buffer (4096) and the GENS34 magic: a 34-slot list
// does not fit 1023 bytes, and an overflow returns the EMPTY string (an unplayable replay). GENS15 files stay <= 1023.
enum { GX_REPLAY_OPTIONS_MAX = 4095 };	///< RecorderClass::readAsciiString's buffer, less its NUL
// GeneralsX @security Claude 04/11/2026 forReplayFile: the string a replay FILE keeps (RecorderClass::startRecording, its only
// caller that passes TRUE). Replays get sent around and posted, so it holds no network address of anyone: no NP=, and each
// human slot's IP and port are placeholders (wh1ter0se 04/11, msg 5a19; names stay). See buildGameInfoAsciiString.
AsciiString GameInfoToAsciiString( const GameInfo *game, Int maxLength = 0, Bool forReplayFile = FALSE );
Bool ParseAsciiStringToGameInfo( GameInfo *game, AsciiString options );

// GeneralsX @feature Claude 01/10/2026 M1 Determinism-1 step 2b. map.ini and solo.ini can redefine Weapon, Object,
// GameData, AudioEvent... rules, and GameLogic::loadMapINI applies them AFTER the SimID/dataID was fixed at startup, so no
// identity covered them: the same .map with one changed Weapon value joined and desynced. This hashes the bytes of the
// two files loadMapINI loads for `mapName` (resolved through TheMapCache exactly as loadMapINI resolves it): 0 when
// neither exists, never 0 otherwise. Bytes, not parsed state: the parse is a function of (build, base data, these
// bytes), and the SimID pins the first two.
UnsignedInt GetMapDataHash( AsciiString mapName );

// GeneralsX @feature Claude 02/10/2026 (Codex m1det3-2b-r8 P2) A replay header's mapDataHash field: 0 is "not recorded" - GENS14
// files from builds before step 2b hold it there - and this value is "recorded: the map had no map.ini/solo.ini".
// GetMapDataHash never returns it, so a hash can always be told from both.
static const UnsignedInt MAPDATA_RECORDED_NONE = 0xFFFFFFFFu;


/**
  * The SkirmishGameInfo class holds information about the skirmish game and
	* the contents of its slot list.
	*/

class SkirmishGameInfo : public GameInfo, public Snapshot
{
private:
	GameSlot m_skirmishSlot[MAX_SLOTS];

protected:
	// snapshot methods
	virtual void crc( Xfer *xfer ) override;
	virtual void xfer( Xfer *xfer ) override;
	virtual void loadPostProcess() override;

public:
	SkirmishGameInfo()
	{
		for (Int i = 0; i< MAX_SLOTS; ++i)
			setSlotPointer(i, &m_skirmishSlot[i]);
	}

	/// GeneralsX @bugfix Claude 01/10/2026 Put back each slot's original colour, start position and faction when a
	/// restart saved them. The recorder calls it before writing a replay's slot list; tryStartNewGame then does the
	/// same restore (a no-op by then) or, on a first start, the save (tsh batch2 #9).
	void restoreOriginalSetups();
};

extern SkirmishGameInfo *TheSkirmishGameInfo;
extern SkirmishGameInfo *TheChallengeGameInfo;
