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

// FILE: GameInfo.cpp //////////////////////////////////////////////////////
// game setup state info
// Author: Matthew D. Campbell, December 2001

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/CRCDebug.h"
#include "Common/file.h"
#include "Common/FileSystem.h"
#include "Common/GameState.h"
#include "GameClient/GameText.h"
#include "GameClient/MapUtil.h"
#include "Common/MultiplayerSettings.h"
#include "Common/PlayerTemplate.h"
#include "Common/Xfer.h"
#include "GameNetwork/FileTransfer.h"
#include "GameNetwork/GameInfo.h"
#include "Common/PlayerCapacity.h"
#include "GameNetwork/GameSpy/ThreadUtils.h"
#include "GameNetwork/GameSpy/StagingRoomGameInfo.h"
#include "GameNetwork/LANAPI.h"						// for testing packet size
#include "GameNetwork/LANAPICallbacks.h"	// for testing packet size
#include "WWLib/strtok_r.h"
#include "WWLib/utf8.h"

#include <ctype.h>	// isxdigit, for the AD= admin mask
#include "Common/GXAdminOrder.h"	// the AD= keys' base64url spelling
#include "Common/OptionPreferences.h"
#include "GameNetwork/GXAiColours.h"



GameInfo *TheGameInfo = nullptr;

static AsciiString percentEncodeMapName(const AsciiString& mapName);
static AsciiString percentDecodeMapName(const AsciiString& encodedMapName);

// GameSlot ----------------------------------------

GameSlot::GameSlot()
{
	reset();
}

void GameSlot::reset()
{
	m_state = SLOT_CLOSED; // decent default
	m_isAccepted = false;
	m_hasMap = true;
	m_color = -1;
	m_startPos = -1;
	m_playerTemplate = -1;
	m_teamNumber = -1;
	m_NATBehavior = FirewallHelperClass::FIREWALL_TYPE_SIMPLE;
	m_lastFrameInGame = 0;
	m_disconnected = FALSE;
	m_IP = 0;
	m_port = 0;
	m_isMuted = FALSE;
	m_hasSavedOriginalSetup = FALSE;
	m_origPlayerTemplate = -1;
	m_origStartPos = -1;
	m_origColor = -1;
}

void GameSlot::saveOriginalSetup()
{
	DEBUG_LOG(("GameSlot::saveOriginalSetup() - orig was color=%d, pos=%d, house=%d",
		m_origColor, m_origStartPos, m_origPlayerTemplate));
	m_origPlayerTemplate = m_playerTemplate;
	m_origStartPos = m_startPos;
	m_origColor = m_color;
	DEBUG_LOG(("GameSlot::saveOriginalSetup() - color=%d, pos=%d, house=%d",
		m_color, m_startPos, m_playerTemplate));

	m_hasSavedOriginalSetup = TRUE;
}

static Int getSlotIndex(const GameSlot *slot)
{
	for (Int i=0; i<MAX_SLOTS; ++i)
	{
		if (TheGameInfo->getConstSlot(i) == slot)
			return i;
	}
	return -1;
}

static Bool isSlotLocalAlly(const GameSlot *slot)
{
	Int slotIndex = getSlotIndex(slot);
	Int localIndex = TheGameInfo->getLocalSlotNum();
	const GameSlot *localSlot = TheGameInfo->getConstSlot(localIndex);

	// if either doesn't exist, not an ally
	if (slotIndex < 0 || localIndex < 0)
		return FALSE;

	// if slot is us, ally
	if (slotIndex == localIndex)
		return TRUE;

	// if slot is same team as us, ally
	if (slot->getTeamNumber() == localSlot->getTeamNumber() && slot->getTeamNumber() >= 0)
		return TRUE;

	// if we're an observer, we see all
	if (localSlot->getOriginalPlayerTemplate() == PLAYERTEMPLATE_OBSERVER)
		return TRUE;

	// nope
	return FALSE;
}

UnicodeString GameSlot::getApparentPlayerTemplateDisplayName() const
{
	if (TheMultiplayerSettings && TheMultiplayerSettings->showRandomPlayerTemplate() &&
		m_origPlayerTemplate == PLAYERTEMPLATE_RANDOM && !isSlotLocalAlly(this))
	{
		return TheGameText->fetch("GUI:Random");
	}
	else if (m_origPlayerTemplate == PLAYERTEMPLATE_OBSERVER)
	{
		return TheGameText->fetch("GUI:Observer");
	}
	DEBUG_LOG(("Fetching player template display name for player template %d (orig is %d)",
		m_playerTemplate, m_origPlayerTemplate));
	if (m_playerTemplate < 0)
	{
		return TheGameText->fetch("GUI:Random");
	}
	return ThePlayerTemplateStore->getNthPlayerTemplate(m_playerTemplate)->getDisplayName();
}

Int GameSlot::getApparentPlayerTemplate() const
{
	if (TheMultiplayerSettings && TheMultiplayerSettings->showRandomPlayerTemplate() &&
		!isSlotLocalAlly(this))
	{
		return m_origPlayerTemplate;
	}
	return m_playerTemplate;
}

Int GameSlot::getApparentColor() const
{
	if (TheMultiplayerSettings && m_origPlayerTemplate == PLAYERTEMPLATE_OBSERVER)
		return TheMultiplayerSettings->getColor(PLAYERTEMPLATE_OBSERVER)->getColor();

	if (TheMultiplayerSettings && TheMultiplayerSettings->showRandomColor() &&
		!isSlotLocalAlly(this))
	{
		return m_origColor;
	}
	return m_color;
}

Int GameSlot::getApparentStartPos() const
{
	if (TheMultiplayerSettings && TheMultiplayerSettings->showRandomStartPos() &&
		!isSlotLocalAlly(this))
	{
		return m_origStartPos;
	}
	return m_startPos;
}


// GeneralsX @feature Claude 24/09/2026 See GameInfo.h. Both halves of the post-start test are needed: an
// original of PLAYERTEMPLATE_OBSERVER alone is also a skirmish seat that has since chosen a faction.
Bool GameSlot::isObserver() const
{
	if (m_playerTemplate == PLAYERTEMPLATE_OBSERVER)
		return TRUE;
	if (!m_hasSavedOriginalSetup || m_origPlayerTemplate != PLAYERTEMPLATE_OBSERVER || m_playerTemplate < 0
			|| ThePlayerTemplateStore == nullptr)
		return FALSE;
	const PlayerTemplate *observerTemplate = ThePlayerTemplateStore->findPlayerTemplate(TheNameKeyGenerator->nameToKey("FactionObserver"));
	return observerTemplate != nullptr && ThePlayerTemplateStore->getNthPlayerTemplate(m_playerTemplate) == observerTemplate;
}

void GameSlot::unAccept()
{
	if (isHuman())
	{
		m_isAccepted = false;
	}
}

void GameSlot::setMapAvailability( Bool hasMap )
{
	if (isHuman())
	{
		m_hasMap = hasMap;
	}
}

void GameSlot::setState( SlotState state, UnicodeString name, UnsignedInt IP )
{
	if (!(isAI() &&  (state == SLOT_EASY_AI || state == SLOT_MED_AI || state == SLOT_BRUTAL_AI)))
	{
		m_color = -1;
		m_startPos = -1;
		m_playerTemplate = -1;
		m_teamNumber = -1;

		if (state == SLOT_OPEN && TheGameSpyGame && TheGameSpyGame->getConstSlot(0) == this)
		{
			DEBUG_CRASH(("Game Is Hosed!"));
		}
	}
	if (state == SLOT_PLAYER)
	{
		reset();
		m_state = state;
		m_name = name;
	}
	else
	{
		m_state = state;
		m_isAccepted = true;
		m_hasMap = true;
		switch(state)
		{
		case SLOT_OPEN:
			m_name = TheGameText->fetch("GUI:Open");
			break;
		case SLOT_EASY_AI:
			m_name = TheGameText->fetch("GUI:EasyAI");
			break;
		case SLOT_MED_AI:
			m_name = TheGameText->fetch("GUI:MediumAI");
			break;
		case SLOT_BRUTAL_AI:
			m_name = TheGameText->fetch("GUI:HardAI");
			break;
		case SLOT_CLOSED:
		default:
			m_name = TheGameText->fetch("GUI:Closed");
			break;
		}
	}

	m_IP = IP;
}

// Various tests
Bool GameSlot::isHuman() const
{
	return m_state == SLOT_PLAYER;
}

Bool GameSlot::isOccupied() const
{
	return m_state == SLOT_PLAYER || m_state == SLOT_EASY_AI || m_state == SLOT_MED_AI || m_state == SLOT_BRUTAL_AI;
}

Bool GameSlot::isAI() const
{
	return m_state == SLOT_EASY_AI || m_state == SLOT_MED_AI || m_state == SLOT_BRUTAL_AI;
}

Bool GameSlot::isPlayer( AsciiString userName ) const
{
	UnicodeString uName;
	uName.translate(userName);
	return (m_state == SLOT_PLAYER && !m_name.compareNoCase(uName));
}

Bool GameSlot::isPlayer( UnicodeString userName ) const
{
	return (m_state == SLOT_PLAYER && !m_name.compareNoCase(userName));
}

Bool GameSlot::isPlayer( UnsignedInt ip ) const
{
	return (m_state == SLOT_PLAYER && m_IP == ip);
}

Bool GameSlot::isOpen() const
{
	return m_state == SLOT_OPEN;
}

// GameInfo ----------------------------------------

GameInfo::GameInfo()
{
	for (int i=0; i<MAX_SLOTS; ++i)
	{
		m_slot[i] = nullptr;
	}
	reset();
}

// GeneralsX @bugfix Claude 01/10/2026 TheGameInfo is a raw pointer to whichever game is current (a LAN game, the
// skirmish info, a replay's); when that object is deleted the global dangled. Idea of Generals Online 2e1578d57
// (reimplemented, tsh batch2 #250).
GameInfo::~GameInfo()
{
	if (TheGameInfo == this)
		TheGameInfo = nullptr;
}

void GameInfo::init()
{
	reset();
}

void GameInfo::reset()
{
	m_crcInterval = NET_CRC_INTERVAL;
	m_inGame = false;
	m_inProgress = false;
	m_gameID = 0;
	m_mapName = "NOMAP";
	m_mapMask = 0;
	m_seed = GetTickCount(); //GameClientRandomValue(0, INT_MAX - 1);
	m_useStats = TRUE;
	m_surrendered = FALSE;
  m_oldFactionsOnly = FALSE;
//	m_localIP = 0; // BGC - actually we don't want this to be reset since the m_localIP is
										// set properly in the constructor of LANGameInfo which uses this as a base class.
	m_mapCRC = 0;
	m_mapSize = 0;
	m_mapDataHash = 0;
  m_superweaponRestriction = 0;
  m_startingCash = TheGlobalData->m_defaultStartingCash;
	gxStressClear( &m_gxStress );
	clearGxAdmin();
	m_legacyObserverLayout = FALSE;
	m_gxAiColours = GXAiColours::MODE_ALL;

	for (Int i=0; i<MAX_SLOTS; ++i)
	{
		if (m_slot[i])
			m_slot[i]->reset();
	}

	m_preorderMask = 0;
}

Bool GameInfo::isPlayerPreorder(Int index)
{
	if (index >= 0 && index < MAX_SLOTS)
		return ((m_preorderMask & (gxSlotBit(index))) != 0);
	return FALSE;
}

void GameInfo::markPlayerAsPreorder(Int index)
{
	if (index >= 0 && index < MAX_SLOTS)
		m_preorderMask |= gxSlotBit(index);
}


void GameInfo::clearSlotList()
{
	for (int i=0; i<MAX_SLOTS; ++i)
	{
		if (m_slot[i])
			m_slot[i]->setState(SLOT_CLOSED);
	}
}

Int GameInfo::getNumPlayers() const
{
	Int numPlayers = 0;
	for (int i=0; i<MAX_SLOTS; ++i)
	{
		if (m_slot[i] && m_slot[i]->isOccupied())
			numPlayers++;
	}
	return numPlayers;
}

// GeneralsX @feature Claude 23/09/2026 The slots the capacity rule charges for (Common/PlayerCapacity.h).
// The lobby and GameLogic::tryStartNewGame both count through here.
Int GameInfo::countSlotsTakingPlayers() const
{
	Int n = 0;
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		const GameSlot *slot = m_slot[i];
		if (slot && slotTakesPlayer(slot->isOccupied(), slotSharesReplayObserver(i)))
			++n;
	}
	return n;
}

Bool GameInfo::slotSharesReplayObserver( Int slotNum ) const
{
	if (m_legacyObserverLayout || slotNum < 0 || slotNum >= MAX_SLOTS)
		return FALSE;
	const GameSlot *slot = m_slot[slotNum];
	return slot != nullptr && slot->isOccupied() && slot->isObserver();
}

Int GameInfo::getNumNonObserverPlayers() const
{
	Int numPlayers = 0;
	for (int i=0; i<MAX_SLOTS; ++i)
	{
		if (m_slot[i] && m_slot[i]->isOccupied() && m_slot[i]->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER)
			numPlayers++;
	}
	return numPlayers;
}

Int GameInfo::getMaxPlayers() const
{
	if (!TheMapCache)
		return -1;

	AsciiString lowerMap = m_mapName;
	lowerMap.toLower();
	MapCache::iterator it = TheMapCache->find(lowerMap);
	if (it == TheMapCache->end())
		return -1;
	MapMetaData data = it->second;
	return data.m_numPlayers;
}

void GameInfo::enterGame()
{
	DEBUG_ASSERTCRASH(!m_inGame && !m_inProgress, ("Entering game at a bad time!"));
	reset();
	m_inGame = true;
	m_inProgress = false;
}

void GameInfo::leaveGame()
{
	DEBUG_ASSERTCRASH(m_inGame && !m_inProgress, ("Leaving game at a bad time!"));
	reset();
}

void GameInfo::startGame( Int gameID )
{
	DEBUG_ASSERTCRASH(m_inGame && !m_inProgress, ("Starting game at a bad time!"));
	m_gameID = gameID;
	closeOpenSlots();
	m_inProgress = true;
}

void GameInfo::endGame()
{
	DEBUG_ASSERTCRASH(m_inGame && m_inProgress, ("Ending game without playing one!"));
	m_inGame = false;
	m_inProgress = false;
}

void GameInfo::setSlot( Int slotNum, GameSlot slotInfo )
{
	DEBUG_ASSERTCRASH( slotNum >= 0 && slotNum < MAX_SLOTS, ("GameInfo::setSlot - Invalid slot number"));
	if (slotNum < 0 || slotNum >= MAX_SLOTS)
		return;

	DEBUG_ASSERTCRASH( m_slot[slotNum], ("null slot pointer"));
	if (!m_slot[slotNum])
		return;

//	Bool isHuman = slotInfo.isHuman();
//	Bool wasHuman = m_slot[slotNum]->isHuman();

	if (slotNum == 0)
	{
		slotInfo.setAccept();
		slotInfo.setMapAvailability(true);
	}
	*m_slot[slotNum] = slotInfo;

#ifdef DEBUG_LOGGING
	UnsignedInt ip = slotInfo.getIP();
#endif

	DEBUG_LOG(("GameInfo::setSlot - setting slot %d to be player %ls with IP %d.%d.%d.%d", slotNum, slotInfo.getName().str(),
							PRINTF_IP_AS_4_INTS(ip)));
}

GameSlot* GameInfo::getSlot( Int slotNum )
{
	DEBUG_ASSERTCRASH( slotNum >= 0 && slotNum < MAX_SLOTS, ("GameInfo::getSlot - Invalid slot number"));
	if (slotNum < 0 || slotNum >= MAX_SLOTS)
		return nullptr;

	DEBUG_ASSERTCRASH( m_slot[slotNum], ("null slot pointer") );
	return m_slot[slotNum];
}

const GameSlot* GameInfo::getConstSlot( Int slotNum ) const
{
	DEBUG_ASSERTCRASH( slotNum >= 0 && slotNum < MAX_SLOTS, ("GameInfo::getSlot - Invalid slot number"));
	if (slotNum < 0 || slotNum >= MAX_SLOTS)
		return nullptr;

	DEBUG_ASSERTCRASH( m_slot[slotNum], ("null slot pointer") );
	return m_slot[slotNum];
}

Int GameInfo::getLocalSlotNum() const
{
	DEBUG_ASSERTCRASH(m_inGame, ("Looking for local game slot while not in game"));
	if (!m_inGame)
		return -1;

	for (Int i=0; i<MAX_SLOTS; ++i)
	{
		const GameSlot *slot = getConstSlot(i);
		if (slot == nullptr) {
			continue;
		}
		if (slot->isPlayer(m_localIP))
			return i;
	}
	return -1;
}

Int GameInfo::getSlotNum( AsciiString userName ) const
{
	DEBUG_ASSERTCRASH(m_inGame, ("Looking for game slot while not in game"));
	if (!m_inGame)
		return -1;

	UnicodeString uName;
	uName.translate(userName);
	for (Int i=0; i<MAX_SLOTS; ++i)
	{
		const GameSlot *slot = getConstSlot(i);
		if (slot->isPlayer( uName ))
			return i;
	}
	return -1;
}

Bool GameInfo::amIHost() const
{
	DEBUG_ASSERTCRASH(m_inGame, ("Looking for game slot while not in game"));
	if (!m_inGame)
		return false;

	return getConstSlot(0)->isPlayer(m_localIP);
}

/// Set the lobby's display name, sanitised for the announce.
///
/// GeneralsX @feature Claude 16/08/2026 SANITISED HERE, ONCE, rather than encoded. The announce is
/// a byte-oriented "KEY=value;" list split with strtok_r on ';' and then on '=', and the slot list
/// nested inside it splits on ',' and ':'. A name carrying any of those would not corrupt one
/// field, it would shift every field after it - and ParseAsciiStringToGameInfo abandons the WHOLE
/// options string the moment it sees an empty value, so the failure mode is the entire lobby
/// vanishing from every browser, not a mangled title.
///
/// Stripping rather than percent-encoding keeps this symmetric with how player names are handled
/// (GXApplyNickname removes the same delimiters) and means no decoder has to exist on the read
/// side - which is one fewer thing that can disagree between two builds.
///
/// Non-ASCII is dropped for the same reason GXApplyNickname refuses it: every consumer below is
/// byte-oriented, and AsciiString::translate would narrow a WideChar to a single char anyway.
void GameInfo::setLobbyName( AsciiString name )
{
	m_lobbyName.clear();

	Int kept = 0;
	for (const char *c = name.str(); c != nullptr && *c != '\0' && kept < g_lobbyDisplayNameLength; ++c)
	{
		const unsigned char ch = (unsigned char)*c;
		// The announce's own delimiters, at both nesting levels.
		if (ch == ';' || ch == '=' || ch == ',' || ch == ':')
			continue;
		// Printable ASCII only; this excludes the NUL, every control character and anything
		// that would have been a narrowed WideChar.
		if (ch < 0x20 || ch > 0x7E)
			continue;
		m_lobbyName.concat((char)ch);
		++kept;
	}

	// Leading and trailing spaces survive the loop but read as an accident on screen, and a name
	// that is nothing BUT spaces must end up empty so the writer omits the field entirely rather
	// than emitting "LN= ;".
	m_lobbyName.trim();
}

// GeneralsX @bugfix Claude 02/10/2026 (Codex m1det3-2b-r6 P2) The contents mask advertises only a file that can be SENT:
// sendFileAnnounce refuses a zero-byte file, so an empty one in the mask failed the whole transfer. That also stranded a
// joiner holding a stale non-empty map.ini/solo.ini against a host's EMPTY one, which GetMapDataHash already counts as
// absent: unadvertised, the joiner's copy is now moved aside after the .map arrives, like any sidecar the host lacks.
static Bool gxSendableFile( const AsciiString& path )
{
	File *fp = TheFileSystem->openFile(path.str());
	if (fp == nullptr)
		return FALSE;
	const Bool sendable = fp->size() > 0;
	fp->close();
	return sendable;
}

void GameInfo::setMap( AsciiString mapName )
{
	m_mapName = mapName;
	if (m_inGame && amIHost())
	{
		const MapMetaData *mapData = TheMapCache->findMap( mapName );
		if (mapData)
		{
			// GeneralsX @note Claude 20/08/2026 DELIBERATELY NOT TRANSLATED HERE, and the reason is
			// worth keeping: translating the identity to its real on-disk path makes these probes
			// succeed on iOS, so the host advertises that it HAS the preview, map.ini and map.str -
			// but DoAnyMapTransfers (FileTransfer.cpp:395-407) still opens game->getMap(), the
			// lowercased identity, and ConnectionManager::sendFile passes that same string to BOTH
			// the local open and the wire. So the host would announce sidecars it then fails to
			// send, which is strictly worse than today's consistent "I do not have them": peers
			// currently do not ask.
			//
			// Fixing it properly means separating the local source path from the portable wire name
			// through the whole transfer layer. That changes what peers compare, which is an
			// all-peers-at-once contract change - the same reason INIMapCache refuses to re-key the
			// cache - and is not a local repair. Tracked rather than half-done.
			m_mapMask = 1;
			AsciiString path = mapName;
			path.truncateBy(3);
			path.concat("tga");
			if (gxSendableFile(path))
				m_mapMask |= 2;

			// GeneralsX @bugfix Claude 10/08/2026 This was a hand-rolled token walk that rebuilt the
			// map's directory - and nextToken skips leading separators, so an absolute POSIX path
			// lost its leading '/' and the probe opened a RELATIVE path against the CWD. Measured
			// live 2026-08-10: a host with map.ini on disk advertised contentsMask without bit 4,
			// so the sidecar was never offered for transfer. GetINIFromMap builds exactly the path
			// this walk was trying to build, with the separator handling fixed in one place.
			if (gxSendableFile(GetINIFromMap(m_mapName)))
				m_mapMask |= 4;
			if (gxSendableFile(GetStrFileFromMap(m_mapName)))
				m_mapMask |= 8;
			if (gxSendableFile(GetSoloINIFromMap(m_mapName)))
				m_mapMask |= 16;
			if (gxSendableFile(GetAssetUsageFromMap(m_mapName)))
				m_mapMask |= 32;
			if (gxSendableFile(GetReadmeFromMap(m_mapName)))
				m_mapMask |= 64;

			// GeneralsX @feature Claude 01/10/2026 M1 step 2b: what every joiner's own map.ini/solo.ini must hash to.
			// KNOWN LIMIT (Codex m1det2-2b-r1 P1): this hashes the RESOLVED sidecars (what loadMapINI loads), while the
			// mask above probes the unresolved name on purpose (the note above). On an iPadOS host whose user map's on-disk
			// case differs, the mask can omit a sidecar this hash includes, so a joiner cannot be sent it: it keeps
			// reporting no map and the start is refused - where before 2b it started and ran other rules. Repairing that
			// needs the local-path/wire-name split described above, an all-peers transfer change of its own.
			m_mapDataHash = GetMapDataHash(m_mapName);
		}
		else
		{
			m_mapMask = 0;
			m_mapDataHash = 0;
		}
	}
}

// GeneralsX @feature Claude 01/10/2026 See GameInfo.h. Resolved the way GameLogic::loadMapINI resolves it (getPathOnDisk,
// then the map's own directory), and loadMapINI loads these same two paths - so the hash is of what the match will load.
//
// FNV-1a, NOT the engine's CRC class (Codex m1det2-2b-r2 P1): CRC is shift-and-add, and "PrimaryDamage = 12.0" and
// "= 20.0" hashed alike, so two peers with different rules were admitted. Each FNV-1a step h = (h ^ b) * prime is a
// bijection of h and, for a fixed h, injective in b, so changing any ONE byte always changes the result. 32 bits because
// the header field is; this guards against stale or edited copies, not against an author forging a collision.
static UnsignedInt gxFnv1a( UnsignedInt h, const void *d, Int n )
{
	const unsigned char *p = (const unsigned char *)d;
	for (Int i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
	return h;
}

UnsignedInt GetMapDataHash( AsciiString mapName )
{
	if (TheMapCache == nullptr || TheFileSystem == nullptr)
		return 0;	// loadMapINI loads nothing without the map cache either
	const AsciiString realMap = TheMapCache->getPathOnDisk(mapName);
	const AsciiString files[2] = { GetINIFromMap(realMap), GetSoloINIFromMap(realMap) };
	UnsignedInt h = 2166136261u;
	Bool any = FALSE;
	for (Int i = 0; i < 2; ++i)
	{
		File *fp = TheFileSystem->openFile(files[i].str(), File::READ | File::BINARY);
		if (fp == nullptr)
			continue;
		const Int len = fp->size();
		if (len <= 0)
		{
			// An empty sidecar defines nothing - the same rules as none - and FileTransfer cannot send a zero-byte
			// file (sendFileAnnounce), so counting it would only strand a joiner (Codex m1det2-2b-r4 P2).
			fp->close();
			continue;
		}
		char *buf = fp->readEntireAndClose();
		// Which file, and its length (little-endian), before its bytes: map.ini "X" plus no solo.ini never equals the reverse.
		const unsigned char head[5] = { (unsigned char)(i + 1), (unsigned char)(len & 0xFF), (unsigned char)((len >> 8) & 0xFF),
			(unsigned char)((len >> 16) & 0xFF), (unsigned char)((len >> 24) & 0xFF) };
		h = gxFnv1a(h, head, sizeof(head));
		h = gxFnv1a(h, buf, len);
		delete[] buf;
		any = TRUE;
	}
	if (!any)
		return 0;
	return (h != 0 && h != MAPDATA_RECORDED_NONE) ? h : 1u;	// 0 = none on the wire; the other is a replay sentinel
}

// GeneralsX @feature Claude 01/10/2026 A joiner whose .map matches still lacks the map when its own sidecars differ from
// the host's: the slot reports no map, so the host's next start re-sends the map with its map.ini/solo.ini (the
// established transfer-only start), and the re-check after the transfer clears it. Without this a stale sidecar was
// never re-sent - FileTransfer only sends to slots without the map - and the match desynced at the first use of a rule.
// GX_TEST_MAPDATA_SKIP_LOBBY=1 (test builds only, -DGX_TEST_INSTRUMENTS=ON) skips this check so the post-load refusal in
// GameLogic can be shown to fire.
static Bool gxLocalMapDataMatches( const GameInfo *game )
{
#if defined(GX_TEST_INSTRUMENTS)
	static const Bool s_skip = getenv("GX_TEST_MAPDATA_SKIP_LOBBY") != nullptr;
#else
	static const Bool s_skip = FALSE;
#endif
	const UnsignedInt local = GetMapDataHash(game->getMap());
	const UnsignedInt host = game->getMapDataHash();
	static UnsignedInt s_lastLocal = 0, s_lastHost = 0;
	if (local != host && (local != s_lastLocal || host != s_lastHost))
	{
		fprintf(stderr, "[GXMAPDATA] lobby: this map's map.ini/solo.ini differ from the host's (local=%08X host=%08X map='%s')%s\n",
			local, host, game->getMap().str(), s_skip ? " - IGNORED (GX_TEST_MAPDATA_SKIP_LOBBY)" : " - reporting no map, so the host re-sends it");
		fflush(stderr);
	}
	s_lastLocal = local;
	s_lastHost = host;
	return s_skip || local == host;
}

void GameInfo::setMapContentsMask( Int mask )
{
	m_mapMask = mask;
}

void GameInfo::setMapCRC( UnsignedInt mapCRC )
{
	m_mapCRC = mapCRC;
	if (!TheMapCache)
		return;

	// check the map cache
	if (m_inGame && getLocalSlotNum() >= 0)
	{
		//TheMapCache->updateCache();
		AsciiString lowerMap = m_mapName;
		lowerMap.toLower();
		//DEBUG_LOG(("GameInfo::setMapCRC - looking for map file \"%s\" in the map cache", lowerMap.str()));
		std::map<AsciiString, MapMetaData>::iterator it = TheMapCache->find(lowerMap);
		if (it == TheMapCache->end())
		{
			/*
			DEBUG_LOG(("GameInfo::setMapCRC - could not find map file."));
			it = TheMapCache->begin();
			while (it != TheMapCache->end())
			{
				DEBUG_LOG(("\t\"%s\"", it->first.str()));
				++it;
			}
			*/
			getSlot(getLocalSlotNum())->setMapAvailability(false);
		}
		else if (m_mapCRC != it->second.m_CRC)
		{
			DEBUG_LOG(("GameInfo::setMapCRC - map CRC's do not match (%X/%X).", m_mapCRC, it->second.m_CRC));
			getSlot(getLocalSlotNum())->setMapAvailability(false);
		}
		else
		{
			//DEBUG_LOG(("GameInfo::setMapCRC - map CRC's match."));
			getSlot(getLocalSlotNum())->setMapAvailability(gxLocalMapDataMatches(this));
		}
	}
}

void GameInfo::setMapSize( UnsignedInt mapSize )
{
	m_mapSize = mapSize;
	if (!TheMapCache)
		return;

	// check the map cache
	if (m_inGame && getLocalSlotNum() >= 0)
	{
		//TheMapCache->updateCache();
		AsciiString lowerMap = m_mapName;
		lowerMap.toLower();
		std::map<AsciiString, MapMetaData>::iterator it = TheMapCache->find(lowerMap);
		if (it == TheMapCache->end())
		{
			DEBUG_LOG(("GameInfo::setMapSize - could not find map file."));
			getSlot(getLocalSlotNum())->setMapAvailability(false);
		}
		else if (m_mapCRC != it->second.m_CRC)
		{
			DEBUG_LOG(("GameInfo::setMapSize - map CRC's do not match."));
			getSlot(getLocalSlotNum())->setMapAvailability(false);
		}
		else
		{
			//DEBUG_LOG(("GameInfo::setMapSize - map CRC's match."));
			getSlot(getLocalSlotNum())->setMapAvailability(gxLocalMapDataMatches(this));
		}
	}
}

void GameInfo::setSeed( Int seed )
{
	m_seed = seed;
}

void GameInfo::setSlotPointer( Int index, GameSlot *slot )
{
	if (index < 0 || index >= MAX_SLOTS)
		return;

	m_slot[index] = slot;
}

void GameInfo::setSuperweaponRestriction( UnsignedShort restriction )
{
  m_superweaponRestriction = restriction;
}

void GameInfo::setStartingCash( const Money & startingCash )
{
  m_startingCash = startingCash;
}

// GeneralsX @feature Claude 07/11/2026 The occupied seats as GXAiColours sees them. Returns how many; seats holds MAX_SLOTS.
static Int gxAiColourSeats( const GameInfo *game, GXAiColours::Seat *seats, Int *slotOf )
{
	Int count = 0;
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		const GameSlot *slot = game->getConstSlot(i);
		if (slot == nullptr || !slot->isOccupied())
			continue;
		const GXAiColours::Seat seat = { i, slot->isHuman() != FALSE, slot->isAI() != FALSE, slot->getState() == SLOT_BRUTAL_AI,
			slot->getTeamNumber(), slot->getColor() };
		slotOf[count] = i;
		seats[count++] = seat;
	}
	return count;
}

// The host at Start. GX_AI_COLOURS over Options.ini's GXAIColours, as GX_WATER_CLEAR over GXWaterClear, for A/B arms.
void GameInfo::stampAiColours()
{
	OptionPreferences prefs;
	const OptionPreferences::const_iterator key = prefs.find("GXAIColours");
	const char *env = getenv("GX_AI_COLOURS");
	const GXAiColours::Mode wanted = GXAiColours::parseMode(env != nullptr ? env : (key != prefs.end() ? key->second.str() : nullptr));

	GXAiColours::Seat seats[MAX_SLOTS];
	Int slotOf[MAX_SLOTS];
	const Int count = gxAiColourSeats(this, seats, slotOf);
	const Bool on = wanted == GXAiColours::MODE_THREE && GXAiColours::applies(seats, count);
	m_gxAiColours = on ? GXAiColours::MODE_THREE : GXAiColours::MODE_ALL;
	if (wanted == GXAiColours::MODE_THREE)
		fprintf(stderr, "[GXAICOLOURS] host asked for three colours: %s\n", on ? "on" : "this game keeps every colour");
}

// The logic, on every peer and in a replay, after populateRandomSideAndColor. The test runs again here so a forged AC= with a second
// human changes nothing, and every peer reads the same slot list, so every peer agrees.
void GameInfo::applyAiColours()
{
	if (m_gxAiColours != GXAiColours::MODE_THREE)
		return;

	GXAiColours::Seat seats[MAX_SLOTS];
	Int slotOf[MAX_SLOTS];
	Int colors[MAX_SLOTS];
	const Int count = gxAiColourSeats(this, seats, slotOf);
	if (!GXAiColours::assign(seats, count, TheMultiplayerSettings->getNumColors(), colors))
	{
		fprintf(stderr, "[GXAICOLOURS] AC=3 ignored: this game keeps every colour\n");
		return;
	}

	AsciiString line;
	for (Int i = 0; i < count; ++i)
	{
		getSlot(slotOf[i])->setColor(colors[i]);
		AsciiString seat;
		seat.format(" %d:%d", slotOf[i], colors[i]);
		line.concat(seat);
	}
	fprintf(stderr, "[GXAICOLOURS] three colours, slot:colour%s\n", line.str());
}

Bool GameInfo::isColorTaken(Int colorIdx, Int slotToIgnore ) const
{
	for (Int i=0; i<MAX_SLOTS; ++i)
	{
		const GameSlot *slot = getConstSlot(i);
		if (slot && slot->getColor() == colorIdx && i != slotToIgnore)
			return true;
	}
	return false;
}

Bool GameInfo::isStartPositionTaken(Int positionIdx, Int slotToIgnore ) const
{
	for (Int i=0; i<MAX_SLOTS; ++i)
	{
		const GameSlot *slot = getConstSlot(i);
		if (slot && slot->getStartPos() == positionIdx && i != slotToIgnore)
			return true;
	}
	return false;
}

void GameInfo::resetAccepted()
{
	GameSlot *slot = getSlot(0);
	if (slot)
		slot->setAccept();
	for(int i = 1; i< MAX_SLOTS; i++)
	{
		slot = getSlot(i);
		if (slot)
			slot->unAccept();
	}
}

void GameInfo::resetStartSpots()
{
	GameSlot *slot = nullptr;
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		slot = getSlot(i);
		if (slot != nullptr)
		{
			slot->setStartPos(-1);
		}
	}
}

// adjust the slots in the game to open or closed
// depending on the players in there now and the number of
// players the map can hold.
void GameInfo::adjustSlotsForMap()
{
	const MapMetaData *md = TheMapCache->findMap(m_mapName);
	if (md != nullptr)
	{
		// get the number of players allowed from the map.
		Int numPlayers = md->m_numPlayers;
		Int numPlayerSlots = 0;

		// first get the number of occupied slots.
		Int i = 0;
		for (; i < MAX_SLOTS; ++i)
		{
			GameSlot *tempSlot = getSlot(i);
			if (tempSlot->isOccupied())
			{
				++numPlayerSlots;
			}
		}

		// now go through and close the appropriate number of slots.
		// note that no players are kicked in this process, we leave
		// that up to the user.
		for (i = 0; i < MAX_SLOTS; ++i)
		{
			// we have room for more players, if this slot is unoccupied, set it to open.
			GameSlot *slot = getSlot(i);
			if (numPlayers > numPlayerSlots)
			{
				if (!(slot->isOccupied()))
				{
					GameSlot newSlot;
					newSlot.setState(SLOT_OPEN);
					setSlot(i, newSlot);
					++numPlayerSlots;
				}
			}
			else
			{
				if (!(slot->isOccupied()))
				{
					// we don't have any more room, set this slot to closed.
					GameSlot newSlot;
					newSlot.setState(SLOT_CLOSED);
					setSlot(i, newSlot);
				}
			}
		}
	}
}

void GameInfo::closeOpenSlots()
{
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		GameSlot *slot = getSlot(i);
		if (!(slot->isOccupied()))
		{
			GameSlot newSlot;
			newSlot.setState(SLOT_CLOSED);
			setSlot(i, newSlot);
		}
	}
}

static Bool isSlotLocalAlly(GameInfo *game, const GameSlot *slot)
{
	const GameSlot *localSlot = game->getConstSlot(game->getLocalSlotNum());
	if (!localSlot)
		return TRUE;

	if (slot == localSlot)
		return TRUE;

	if (slot->getTeamNumber() < 0)
		return FALSE;

	return slot->getTeamNumber() == localSlot->getTeamNumber();
}

Bool GameInfo::isSkirmish()
{
	Bool sawAI = FALSE;

	for (Int i=0; i<MAX_SLOTS; ++i)
	{
		if (i == getLocalSlotNum())
			continue;

		if (getConstSlot(i)->isHuman())
			return FALSE;

		if (getConstSlot(i)->isAI())
		{
			if (isSlotLocalAlly(getConstSlot(i)))
				return FALSE;
			sawAI = TRUE;
		}
	}
	return sawAI;
}

Bool GameInfo::isMultiPlayer()
{
	for (Int i=0; i<MAX_SLOTS; ++i)
	{
		if (i == getLocalSlotNum())
			continue;

		if (getConstSlot(i)->isHuman())
			return TRUE;
	}

	return FALSE;
}

Bool GameInfo::isSandbox()
{
	Int localSlotNum = getLocalSlotNum();
	Int localTeam = getConstSlot(localSlotNum)->getTeamNumber();
	for (Int i=0; i<MAX_SLOTS; ++i)
	{
		if (i == localSlotNum)
			continue;

		const GameSlot *slot = getConstSlot(i);
		if (slot->isOccupied() && (slot->getTeamNumber() < 0 || slot->getTeamNumber() != localTeam))
			return FALSE;
	}
	return TRUE;
}

// GeneralsX @feature Claude 27/09/2026 The start rule (GameInfo.h). Retail's setup screens refused a lobby
// with no human player (GUI:NeedHumanPlayers) and called fewer than two sides "sandbox mode"; the owner's
// rule (27/09/2026) lets an all-AI lobby start as a sandbox too. The slots counted are the lobby menus' own -
// occupied and not observing - and a side is one team, or one player on no team.
GameInfo::StartVerdict GameInfo::getStartVerdict() const
{
	Int players = 0;
	Int humans = 0;
	Int sides = 0;
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		const GameSlot *slot = m_slot[i];
		if (slot == nullptr || !slot->isOccupied() || slot->isObserver())
			continue;
		++players;
		if (slot->isHuman())
			++humans;
		Bool newSide = TRUE;
		for (Int j = 0; j < i && newSide && slot->getTeamNumber() >= 0; ++j)
		{
			const GameSlot *other = m_slot[j];
			if (other != nullptr && other->isOccupied() && !other->isObserver() && other->getTeamNumber() == slot->getTeamNumber())
				newSide = FALSE;
		}
		if (newSide)
			++sides;
	}
	if (players == 0)
		return START_NOBODY_PLAYS;
	return (humans == 0 || sides < 2) ? START_SANDBOX : START_MATCH;
}


// Convenience Functions ----------------------------------------

static const char slotListID		= 'S';

// TheSuperHackers @bugfix bobtista 23/08/2026 Shorten player names without cutting a UTF-8
// character in half.
static void truncatePlayerNameToByteCount(AsciiString& name, Int maxByteCount)
{
	const size_t truncatedLength = Utf8_Truncate_Len(name.str(), name.getLength(), maxByteCount);
	name.truncateTo(static_cast<Int>(truncatedLength));
}

static Int getMinPlayerNameLength(const AsciiString& name)
{
	for (Int maxByteCount = 1; maxByteCount <= name.getLength(); ++maxByteCount)
	{
		const size_t truncatedLength = Utf8_Truncate_Len(name.str(), name.getLength(), maxByteCount);
		if (truncatedLength > 0)
		{
			return static_cast<Int>(truncatedLength);
		}
	}

	return 0;
}

static Bool truncatePlayerNames(const GameInfo& game, AsciiString playerNames[MAX_SLOTS], Int maxPlayerNamesLength)
{
	Int minLengths[MAX_SLOTS] = { 0 };
	Int minTotalLength = 0;
	Int playerCount = 0;
	Int i;

	for (i = 0; i < MAX_SLOTS; ++i)
	{
		const GameSlot *slot = game.getConstSlot(i);
		if (slot && slot->isHuman())
		{
			minLengths[i] = getMinPlayerNameLength(playerNames[i]);
			if (minLengths[i] == 0)
			{
				// Every serialized human must retain at least one complete UTF-8 character.
				return false;
			}
			minTotalLength += minLengths[i];
			++playerCount;
		}
	}

	if (playerCount == 0 || maxPlayerNamesLength < minTotalLength)
	{
		return false;
	}

	Int remainingLength = maxPlayerNamesLength;
	for (i = 0; i < MAX_SLOTS; ++i)
	{
		const GameSlot *slot = game.getConstSlot(i);
		if (slot && slot->isHuman())
		{
			const Int extraLength = (remainingLength - minTotalLength) / playerCount;
			truncatePlayerNameToByteCount(playerNames[i], minLengths[i] + extraLength);
			remainingLength -= playerNames[i].getLength();
			minTotalLength -= minLengths[i];
			--playerCount;
		}
	}

	return true;
}

static AsciiString buildGameInfoAsciiString(const GameInfo& game, const AsciiString playerNames[MAX_SLOTS], Bool withAdmin, Bool forReplayFile)
{
	AsciiString mapName = game.getMap();
	mapName = TheGameState->realMapPathToPortableMapPath(mapName);
	AsciiString newMapName;
	if (!mapName.isEmpty())
	{
		AsciiString token;
		mapName.nextToken(&token, "\\/");
		// add all the tokens except the last one.
		// that way we don't add the filename, just the
		// directory name, we can do this since the filename
		// is just the directory name with the file extention
		// added onto it.
		// GeneralsX @bugfix fbraz 05/05/2026 Handle both forward and backward separators correctly when building map-sidecar lookup paths
		while (!mapName.isEmpty() && (mapName.find('\\') != nullptr || mapName.find('/') != nullptr))
		{
			if (!newMapName.isEmpty())
			{
				newMapName.concat('/');
			}
			newMapName.concat(token);
			mapName.nextToken(&token, "\\/");
		}
		DEBUG_LOG(("Map name is %s", mapName.str()));
	}

	AsciiString optionsString;
#if RTS_GENERALS
	optionsString.format("M=%2.2x%s;MC=%X;MS=%d;SD=%d;C=%d;", game.getMapContentsMask(), percentEncodeMapName(newMapName).str(),
		game.getMapCRC(), game.getMapSize(), game.getSeed(), game.getCRCInterval());
#else
	optionsString.format("US=%d;M=%2.2x%s;MC=%X;MS=%d;SD=%d;C=%d;SR=%u;SC=%u;O=%c;", game.getUseStats(), game.getMapContentsMask(), percentEncodeMapName(newMapName).str(),
		game.getMapCRC(), game.getMapSize(), game.getSeed(), game.getCRCInterval(), game.getSuperweaponRestriction(),
		game.getStartingCash().countMoney(), game.oldFactionsOnly() ? 'Y' : 'N' );
#endif

	// GeneralsX @feature Claude 12/08/2026 The writer's /24 network prefix, written ONCE and
	// amortized across every shorthand slot IP below. The reader reconstructs ".N" against THIS
	// value - never against its own subnet, which it has no reason to share with the writer's
	// (a joiner on 192.168.2.x hearing a host on 192.168.1.x must not invent 192.168.2.7).
	// Absent when the writer has no local IP; the parser refuses shorthand without it.
	// GeneralsX @feature Claude 16/08/2026 The lobby's display name, written ONLY when it has one.
	//
	// Omitted rather than emitted empty, and that is not a style choice: the reader treats an empty
	// value as a malformed options string and abandons the entire announce
	// (ParseAsciiStringToGameInfo, "saw empty value, quitting"), so "LN=;" would delete the lobby
	// from every browser. setLobbyName has already stripped the delimiters and capped the length,
	// so whatever is here is safe to concatenate raw.
	if (!game.getLobbyName().isEmpty())
	{
		AsciiString lnField;
		lnField.format("LN=%s;", game.getLobbyName().str());
		optionsString.concat(lnField);
	}

	// GeneralsX @feature Claude 23/09/2026 The lobby-agreed stress battlefield, written ONLY when the
	// host set one - an ordinary lobby's options string is byte-for-byte what it was. A build that
	// predates the field rejects the whole string on the unknown key (the final `else` in
	// ParseAsciiStringToGameInfo), so such a peer never sees this lobby and cannot join it and desync.
	// Budget: GX_STRESS_ANNOUNCE_BYTES, reserved in LANAPI.h's g_announcePrefixAllowance.
	if (gxStressIsOn(game.getGxStress()))
	{
		char stressValue[GX_STRESS_WIRE_MAX];
		if (gxStressFormat(game.getGxStress(), stressValue, sizeof(stressValue)) > 0)
		{
			AsciiString stField;
			stField.format("ST=%s;", stressValue);
			optionsString.concat(stField);
		}
	}

	// GeneralsX @feature Claude 07/11/2026 AI in three colours, ONLY in the replay header: it only ever applies with one human, so
	// no peer needs it, and an announced lobby's string stays byte-for-byte what it was.
	if (forReplayFile && game.getGxAiColours() != GXAiColours::MODE_ALL)
	{
		AsciiString acField;
		acField.format("AC=%d;", game.getGxAiColours());
		optionsString.concat(acField);
	}

	// GeneralsX @security Claude 25/09/2026 The relay-proven admin slots and their per-match keys - "AD=<mask hex>:
	// <key>,<key>..." with one base64url key per set bit, lowest slot first - written ONLY when there are any: an
	// ordinary lobby's string is byte-for-byte what it was, and a build without the field never sees one that has it
	// (the unknown-key refusal below). ONLY in a string that is never announced - the REPLAY header, the one caller
	// that passes a maxLength (Codex r3 P2: an in-progress game keeps announcing, and up to four keys would have eaten
	// the announce's 365 bytes and stopped it being listed). The replay header holds exactly what the match ran on.
	if (withAdmin && game.getGxAdminMask() != 0)
	{
		AsciiString adField;
		adField.format("AD=%llX:", (unsigned long long)game.getGxAdminMask());
		Bool first = TRUE;
		for (Int i = 0; i < MAX_SLOTS; ++i)
		{
			if ((game.getGxAdminMask() & (gxSlotBit(i))) == 0)
				continue;
			char b64[44];
			gxAdminKeyToB64(game.getGxAdminKey(i), b64);
			if (!first)
				adField.concat(',');
			adField.concat(b64);
			first = FALSE;
		}
		adField.concat(';');
		optionsString.concat(adField);
	}

	// GeneralsX @feature Claude 01/10/2026 M1 step 2b: the host's map.ini/solo.ini hash, written ONLY when the map has
	// one - a stock-map lobby's string is byte-for-byte what it was. A build without the field refuses the whole string
	// on the unknown key, so it never sees such a lobby and cannot join it and run other rules.
	if (game.getMapDataHash() != 0)
	{
		AsciiString mdField;
		mdField.format("MD=%X;", game.getMapDataHash());
		optionsString.concat(mdField);
	}

	// GeneralsX @security Claude 04/11/2026 A replay FILE leaves out the writer's /24 (and its slot IPs below): the reader only
	// needs NP= to expand ".N" shorthand, and a replay writes none.
	const UnsignedInt netPrefix = game.getLocalIP() & 0xFFFFFF00u;
	if (game.getLocalIP() != 0 && !forReplayFile)
	{
		AsciiString npField;
		npField.format("NP=%X;", netPrefix);
		optionsString.concat(npField);
	}

	//add player info for each slot
	optionsString.concat(slotListID);
	optionsString.concat('=');
	for (Int i=0; i<MAX_SLOTS; ++i)
	{
		const GameSlot *slot = game.getConstSlot(i);

		AsciiString str;
		if (slot && slot->isHuman())
		{
			// GeneralsX @feature Claude 12/08/2026 Compact slot encoding for 12 humans. Measured
			// (scripts/test/announce/): the legacy fixed fields overflow the 400-byte budget at
			// NINE humans with names truncated to nothing, so no name policy can close it. The
			// two compressible fields:
			//   IP    - when the slot shares the local peer's /24 (always true through the relay,
			//           which assigns 10.42.0.1-12 in one subnet; true on typical LANs) it is
			//           written as ".<lastOctet>" (2-3 chars) instead of up-to-8 hex chars. The
			//           leading dot disambiguates from bare-hex; cross-subnet LANs keep full hex.
			//   port  - written as "." when it equals NETWORK_BASE_PORT_NUMBER (a bare dot, NOT
			//           an empty field: strtok_r collapses consecutive delimiters, so ",," would
			//           silently shift every later field by one).
			// The parser accepts both compact and legacy forms; legacy replays stay readable.
			//
			// GeneralsX @security Claude 04/11/2026 In a replay FILE the IP is a placeholder: slot index + 1 in plain hex, 0 staying 0
			// (a skirmish human has no address). Playback needs no real value: the local slot is the header's own index
			// (ReplayGameInfo::getLocalSlotNum), GameLogic's by-IP test is commented out, and gxKickShowLine returns early in
			// playback. Measured: a replay with two humans' placeholders forged equal plays the same slot and the same CRC. Nonzero
			// and unique all the same, so the fields keep meaning what they meant to anything that reads a slot's IP (parse-replay.py
			// is_multiplayer). The port is always the bare dot and the NAT type the default, whatever the slot had. Every reader
			// since the compact form parses these.
			AsciiString ipField;
			if (forReplayFile)
				ipField.format("%X", slot->getIP() != 0 ? (UnsignedInt)(i + 1) : 0u);
			else if (((slot->getIP() ^ game.getLocalIP()) & 0xFFFFFF00u) == 0 && game.getLocalIP() != 0)
				ipField.format(".%u", (UnsignedInt)(slot->getIP() & 0xFFu));
			else
				ipField.format("%X", slot->getIP());
			AsciiString portField;
			if (forReplayFile || slot->getPort() == (UnsignedInt)NETWORK_BASE_PORT_NUMBER)
				portField = ".";	// see below - a bare dot, NOT an empty field
			else
				portField.format("%d", slot->getPort());
			// DO NOT "TIDY" THAT DOT INTO AN EMPTY FIELD. The reader splits on ',' with strtok_r,
			// which COLLAPSES consecutive delimiters - so ",," is one delimiter to it, every later
			// field shifts up by one, and the damage surfaces as garbage in an unrelated field
			// (colour, start position, team) rather than as a parse error. The dot is a placeholder
			// that keeps the field count exact. The parser's default-port branch says the same.

			AsciiString tmp;  //all this data goes after name
			tmp.format( ",%s,%s,%c%c,%d,%d,%d,%d,%d:",
				ipField.str(), portField.str(),
				(slot->isAccepted()?'T':'F'),
				(slot->hasMap()?'T':'F'),
				slot->getColor(), slot->getPlayerTemplate(),
				slot->getStartPos(), slot->getTeamNumber(),
				forReplayFile ? FirewallHelperClass::FIREWALL_TYPE_SIMPLE : slot->getNATBehavior() );
			// GeneralsX @refactor Claude 21/09/2026 The per-slot truncate-and-repair block that
			// stood here (the 12/08/2026 negative-lenMax clamp and the trailing continuation-byte
			// strip) is gone. TheSuperHackers #3039 computes every name's length up front instead,
			// so this loop never cuts a name at all and the repair has nothing to repair. The
			// clamp's hang is impossible by construction now, and the strip's own defect - it ran
			// unconditionally, so a name ending in a multi-byte character lost it even when
			// nothing was truncated - goes with it.
			str.format( "H%s%s", playerNames[i].str(), tmp.str() );
		}
		else if (slot && slot->isAI())
		{
			Char c;
			if (slot->getState() == SLOT_EASY_AI)
				c = 'E';
			else if (slot->getState() == SLOT_MED_AI)
				c = 'M';
			else
				c = 'H';
			str.format("C%c,%d,%d,%d,%d:", c,
				slot->getColor(), slot->getPlayerTemplate(),
				slot->getStartPos(), slot->getTeamNumber());
		}
		else if (slot && slot->getState() == SLOT_OPEN)
		{
			str = "O:";
		}
		else if (slot && slot->getState() == SLOT_CLOSED)
		{
			str = "X:";
		}
		else
		{
			DEBUG_CRASH(("Bad slot type"));
			str = "X:";
		}
		optionsString.concat(str);
	}
	optionsString.concat(';');

	return optionsString;
}

AsciiString GameInfoToAsciiString( const GameInfo *game, Int maxLength, Bool forReplayFile )
{
	if (!game)
	{
		return AsciiString::TheEmptyString;
	}

	AsciiString playerNames[MAX_SLOTS];
	Int playerNamesLength = 0;
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		const GameSlot *slot = game->getConstSlot(i);
		if (slot && slot->isHuman())
		{
			playerNames[i] = WideCharStringToMultiByte(slot->getName().str()).c_str();
			playerNamesLength += playerNames[i].getLength();
		}
	}

	// TheSuperHackers @bugfix bobtista 23/08/2026 Prevent an infinite loop when player names exceed
	// the LAN options limit by rebuilding the payload with bounded UTF-8 names.
	// GeneralsX @bugfix Claude 21/09/2026 Size against what an ANNOUNCE actually carries (366),
	// not against the buffer it is copied into (m_lanMaxOptionsLength, 692). The derivation is
	// beside g_lanAnnounceOptionsRoom in LANAPI.h. wireSize() stops sending partway through
	// GameInfo.options for every message type except MSG_GAME_OPTIONS, so a string that passed a
	// 692-byte fit check was cut off mid-field, arrived unterminated, failed
	// ParseGameOptionsString, and the lobby silently never appeared in anyone's game list.
	//
	// The same string also feeds MSG_GAME_OPTIONS, which could carry more - but a lobby nobody can
	// discover is worthless, so the announce is the binding constraint. The rebalancing retry
	// below now targets it, which is what makes this a narrowing rather than a new refusal:
	// strings in 367..692 that used to be accepted-and-silently-truncated now get their player
	// names rebalanced to fit, and only fail loudly if even that cannot.
	// GeneralsX @bugfix Claude 23/09/2026 Size against the longest STRING the room carries (365),
	// not the room itself (366): the terminator has to fit inside the bytes wireSize() sends.
	// A 366-byte string passed here and arrived unterminated. Every string of 365 bytes or
	// fewer takes exactly the path it always did, so its announce is byte-identical.
	// GeneralsX @feature Claude 25/09/2026 Size against the 692-byte buffer (m_lanMaxOptionsLength),
	// because a long announce is now sent at its own length (LANAPI.h LANMessageWireSize) instead of
	// being cut at the legacy extent. The 365-byte room above was the reason a twelve-human lobby
	// returned the empty string here: its fixed per-slot fields alone overflow 365, so no name
	// truncation could rescue it, and the empty string reached MSG_GAME_OPTIONS too. A lobby whose
	// untruncated string is 365 bytes or fewer takes exactly the path it always did and still
	// announces at the legacy extent, byte for byte. One of 366..692 bytes that the old gate rescued
	// by cutting its names now keeps them and goes out at its own length.
	// A positive maxLength is a string that is never announced - the replay header, GX_REPLAY_OPTIONS_MAX,
	// the only one that carries the admin field (GameInfo.h).
	const Int optionsRoom = (maxLength > 0) ? maxLength : m_lanMaxOptionsLength;

	AsciiString optionsString = buildGameInfoAsciiString(*game, playerNames, maxLength > 0, forReplayFile);
	Bool optionsFit = TheLAN == nullptr || optionsString.getLength() <= optionsRoom;
	if (!optionsFit)
	{
		const Int fixedLength = optionsString.getLength() - playerNamesLength;
		const Int maxPlayerNamesLength = optionsRoom - fixedLength;
		if (truncatePlayerNames(*game, playerNames, maxPlayerNamesLength))
		{
			optionsString = buildGameInfoAsciiString(*game, playerNames, maxLength > 0, forReplayFile);
			optionsFit = optionsString.getLength() <= optionsRoom;
		}
	}

	if (!optionsFit)
	{
		// GeneralsX @feature Claude 12/08/2026, kept 21/09/2026 Release-visible overflow trace.
		// Upstream's DEBUG_CRASH here is invisible in a release build, and this is the failure the
		// twelve-human lobby has to be able to see. It now fires only after the rebalancing retry
		// has also failed, so a lobby upstream's truncatePlayerNames rescues no longer prints it.
		// GeneralsX @feature Claude 23/09/2026 Name the stress field when it is part of the overflow,
		// or a stress lobby that stops announcing reads as a plain slot-count problem.
		fprintf(stderr, "[GXCAP] GameInfoToAsciiString overflow: %d of %d bytes - too many human slots for the lobby options string%s; this lobby will not announce correctly\n",
			optionsString.getLength(), optionsRoom,
			gxStressIsOn(game->getGxStress()) ? " (its stress ST= field counts toward it)" : "");
		DEBUG_CRASH(("WARNING: options string cannot fit within the expected length!  Length is %d, but max is %d!",
			optionsString.getLength(), optionsRoom));
		return AsciiString::TheEmptyString;
	}

	return optionsString;
}

// GeneralsX @feature fbraz 05/05/2026 Support special characters in map names via percent encoding.
// This allows map names with brackets, spaces, and other chars commonly used by C&C community mapmakers.
static AsciiString percentEncodeMapName(const AsciiString& mapName)
{
	// Characters that MUST be encoded in replay header field values:
	// % (0x25) - escape indicator; MUST be encoded first to avoid double-encoding
	// [ (0x5B) - INI section marker
	// ] (0x5D) - INI section marker  
	// ; (0x3B) - field separator in replay header
	// = (0x3D) - key-value separator in replay header
	// Space (0x20) - for safety with paths
	AsciiString result;
	const char* src = mapName.str();
	
	for (int i = 0; src[i] != '\0'; ++i) {
		unsigned char c = (unsigned char)src[i];
		switch (c) {
			case '%':  result.concat("%25"); break;  // Escape indicator FIRST
			case '[':  result.concat("%5B"); break;
			case ']':  result.concat("%5D"); break;
			case ';':  result.concat("%3B"); break;
			case '=':  result.concat("%3D"); break;
			case ' ':  result.concat("%20"); break;
			default:   result.concat(src[i]); break;
		}
	}
	return result;
}

// GeneralsX @feature fbraz 05/05/2026 Decode percent-encoded map names (inverse of percentEncodeMapName).
static AsciiString percentDecodeMapName(const AsciiString& encodedMapName)
{
	AsciiString result;
	const char* src = encodedMapName.str();
	int len = encodedMapName.getLength();
	
	for (int i = 0; i < len; ++i) {
		if (src[i] == '%' && i + 2 < len) {
			// Parse two hex digits
			char hex[3] = { src[i+1], src[i+2], '\0' };
			int value = -1;
			if (sscanf(hex, "%x", &value) == 1 && value >= 0 && value <= 255) {
				result.concat((char)value);
				i += 2;  // Skip the two hex digits
				continue;
			}
		}
		// If not a valid escape sequence, just copy the character
		result.concat(src[i]);
	}
	return result;
}

static Int grabHexInt(const char *s)
{
	char tmp[5] = "0xff";
	tmp[2] = s[0];
	tmp[3] = s[1];
	Int b = strtol(tmp, nullptr, 16);
	return b;
}
Bool ParseAsciiStringToGameInfo(GameInfo *game, AsciiString options)
{
	// Parse game options
	char *buf = strdup(options.str());
	char *bufPtr = buf;
	char *strPos, *keyValPair;
	GameSlot newSlot[MAX_SLOTS];
	Bool optionsOk = true;
	AsciiString mapName;
	AsciiString lobbyName;
	Int mapContentsMask;
	UnsignedInt mapCRC, mapSize;
	Int seed = 0;
	Int crc = 100;
	Bool sawCRC = FALSE;
	// GeneralsX @feature Claude 12/08/2026 The WRITER's /24, carried once as "NP=" and used to
	// expand every ".N" shorthand slot IP below. Reconstructing against the READER's own subnet
	// would be silently wrong across subnets - a joiner on 192.168.2.x hearing a host on
	// 192.168.1.x would invent 192.168.2.7 and mesh to an unrelated machine, which presents as
	// "that peer never connects" rather than as a parse error. No NP means no shorthand accepted.
	UnsignedInt netPrefix = 0;
	Bool sawNetPrefix = FALSE;
	GXStressOption stress;	// absent ST means no stress - cleared, never kept from a previous parse
	Int aiColours = GXAiColours::MODE_ALL;	// absent AC means every seat its own colour, likewise
	gxStressClear(&stress);
	RelayMaskType adminMask = 0;	// absent AD means no admin - cleared, never kept from a previous parse
	unsigned char adminKeys[MAX_SLOTS][32];
	memset(adminKeys, 0, sizeof(adminKeys));
  Bool oldFactionsOnly = FALSE;
	Int useStats = TRUE;
  Money startingCash = TheGlobalData->m_defaultStartingCash;
  UnsignedShort restriction = 0; // Always the default

	UnsignedInt mapDataHash = 0;	// absent MD means a map with no map.ini/solo.ini - never kept from a previous parse
	Bool sawMap = FALSE;
	Bool sawMapCRC = FALSE;
	Bool sawMapSize = FALSE;
	Bool sawSeed = FALSE;
	Bool sawSlotlist = FALSE;
	Bool sawUseStats = FALSE;
	Bool sawSuperweaponRestriction = FALSE;
	Bool sawStartingCash = FALSE;
	Bool sawOldFactions = FALSE;

	//DEBUG_LOG(("Saw options of %s", options.str()));
	DEBUG_LOG(("ParseAsciiStringToGameInfo - parsing [%s]", options.str()));


	while ( (keyValPair = strtok_r(bufPtr, ";", &strPos)) != nullptr )
	{
		bufPtr = nullptr; // strtok within the same string

		AsciiString key, val;
		char *pos = nullptr;
		char *keyPtr, *valPtr;
		keyPtr = (strtok_r(keyValPair, "=", &pos));
		valPtr = (strtok_r(nullptr, "\n", &pos));
		if (keyPtr)
			key = keyPtr;
		if (valPtr)
			val = valPtr;

		if (val.isEmpty())
		{
			optionsOk = false;
			DEBUG_LOG(("ParseAsciiStringToGameInfo - saw empty value, quitting"));
			break;
		}

		if (key.compare("NP") == 0)
		{
			// Writer's network prefix for shorthand slot IPs. Must be a /24 base (low octet 0);
			// anything else is malformed and disables shorthand rather than being repaired.
			UnsignedInt np = 0;
			if (sscanf(val.str(), "%X", &np) == 1 && (np & 0xFFu) == 0)
			{
				netPrefix = np;
				sawNetPrefix = TRUE;
			}
			else
			{
				optionsOk = false;
				DEBUG_LOG(("ParseAsciiStringToGameInfo - malformed network prefix, quitting"));
				break;
			}
		}
		else
		if (key.compare("US") == 0)
		{
			useStats = atoi(val.str());
			sawUseStats = true;
		}
		else
		if (key.compare("M") == 0)
		{
			if (val.getLength() < 3)
			{
				optionsOk = FALSE;
				DEBUG_LOG(("ParseAsciiStringToGameInfo - saw bogus map; quitting"));
				break;
			}
			mapContentsMask = grabHexInt(val.str());

			AsciiString portableMapPath = val.str() + 2;
			// GeneralsX @feature fbraz 05/05/2026 Decode percent-encoded map names to support special characters (brackets, spaces, etc).
			portableMapPath = percentDecodeMapName(portableMapPath);
			
			AsciiString legacyPortableMapPath;
			AsciiString token;
			AsciiString tempstr = portableMapPath;
			tempstr.nextToken(&token, "\\/");
			while (!tempstr.isEmpty())
			{
				legacyPortableMapPath.concat(token);
				legacyPortableMapPath.concat('\\');
				tempstr.nextToken(&token, "\\/");
			}
			legacyPortableMapPath.concat(token);
			legacyPortableMapPath.concat('\\');
			legacyPortableMapPath.concat(token);
			legacyPortableMapPath.concat('.');
			legacyPortableMapPath.concat(TheMapCache->getMapExtension());

			AsciiString realMapName = TheGameState->portableMapPathToRealMapPath(legacyPortableMapPath);

			// GeneralsX @bugfix fbraz 05/05/2026 Recover from malformed replay map fields generated from mixed separator custom map paths.
			if (realMapName.isEmpty())
			{
				AsciiString flatPortableMapPath = portableMapPath;
				flatPortableMapPath.concat('.');
				flatPortableMapPath.concat(TheMapCache->getMapExtension());
				realMapName = TheGameState->portableMapPathToRealMapPath(flatPortableMapPath);
			}

			if (realMapName.isEmpty())
			{
				// TheSuperHackers @security slurmlord 18/06/2025 As the map file name/path from the AsciiString failed to normalize,
				// in other words is bogus and points outside of the approved target directory for maps, avoid an arbitrary file overwrite vulnerability
				// if the save or network game embeds a custom map to store at the location, by flagging the options as not OK and rejecting the game.
				optionsOk = FALSE;
				DEBUG_LOG(("ParseAsciiStringToGameInfo - saw bogus map name ('%s'); quitting", legacyPortableMapPath.str()));
				break;
			}
			mapName = realMapName;
			sawMap = true;
			DEBUG_LOG(("ParseAsciiStringToGameInfo - map name is %s", mapName.str()));
		}
		else if (key.compare("MC") == 0)
		{
			mapCRC = 0;
			sscanf(val.str(), "%X", &mapCRC);
			sawMapCRC = true;
		}
		else if (key.compare("MS") == 0)
		{
			mapSize = atoi(val.str());
			sawMapSize = true;
		}
		else if (key.compare("MD") == 0)
		{
			mapDataHash = 0;
			sscanf(val.str(), "%X", &mapDataHash);
		}
		else if (key.compare("SD") == 0)
		{
			seed = atoi(val.str());
			sawSeed = true;
//			DEBUG_LOG(("ParseAsciiStringToGameInfo - random seed is %d", seed));
		}
		else if (key.compare("C") == 0)
		{
			crc = atoi(val.str());
			sawCRC = TRUE;
		}
    else if (key.compare("SR") == 0 )
    {
      restriction = (UnsignedShort)atoi(val.str());
      sawSuperweaponRestriction = TRUE;
    }
    else if (key.compare("SC") == 0 )
    {
      UnsignedInt startingCashAmount = strtoul( val.str(), nullptr, 10 );
      // GeneralsX @bugfix Claude 06/10/2026 Set, not deposit: Money::deposit records income on player 0's AcademyStats
      // whatever its flags, so parsing any peer's lobby announce - even one LANAPI then refuses - wrote game state.
      // init() + deposit(amount, FALSE, FALSE) and setStartingCash(amount) leave the same Money (Codex net-hardening r6).
      startingCash.setStartingCash( startingCashAmount );
      sawStartingCash = TRUE;
    }
    else if (key.compare("O") == 0 )
    {
      oldFactionsOnly = ( val.compareNoCase( "Y" ) == 0 );
      sawOldFactions = TRUE;
    }
		// GeneralsX @feature Claude 16/08/2026 The lobby's display name. Optional by design - a
		// lobby created without one omits the field entirely - so there is no `saw` flag and no
		// validation failure when it is absent. Re-sanitised on the way IN as well as out, because
		// the writer is a remote machine: this build's own writer cannot emit a delimiter, but
		// nothing here gets to assume the sender is this build.
		else if (key.compare("LN") == 0)
		{
			lobbyName = val;
		}
		// GeneralsX @feature Claude 23/09/2026 The lobby-agreed stress battlefield (GameLogic/GXStress.h).
		// Strict: the writer is a remote host, so anything malformed or out of range rejects the WHOLE
		// options string rather than being clamped into a battlefield nobody agreed to.
		else if (key.compare("ST") == 0)
		{
			if (!gxStressParse(val.str(), &stress))
			{
				optionsOk = false;
				DEBUG_LOG(("ParseAsciiStringToGameInfo - bad stress option '%s', quitting", val.str()));
				break;
			}
		}
		// GeneralsX @feature Claude 07/11/2026 AI in three colours. Strict for the same reason ST= is. A value that parses still
		// does nothing in a game with a second human: GameInfo::applyAiColours asks GXAiColours::applies on every peer.
		else if (key.compare("AC") == 0)
		{
			if (val.compare("3") != 0)
			{
				optionsOk = false;
				DEBUG_LOG(("ParseAsciiStringToGameInfo - bad AI colours option '%s', quitting", val.str()));
				break;
			}
			aiColours = GXAiColours::MODE_THREE;
		}
		// GeneralsX @security Claude 25/09/2026 The relay-proven admin slots and their keys. Strict for the same reason
		// ST= is: one hex digit per four slots at most (four for MAX_SLOTS 14 - an admin in slot 12 or 13 writes
		// AD=1000 and up; Codex batch-r6), naming slots that exist, then exactly one canonical base64url key per set
		// bit, or the whole string is refused.
		else if (key.compare("AD") == 0)
		{
			enum { AD_HEX_MAX = (MAX_SLOTS + 3) / 4 };
			const char *v = val.str();
			const char *colon = strchr(v, ':');
			// GeneralsX @feature Claude 06/11/2026 scale32: the AD= mask is up to 34 bits = 9 hex digits, read and written 64-bit (%llX).
			unsigned long long m = 0;
			const Int hexLen = (colon != nullptr) ? (Int)(colon - v) : 0;
			Bool ok = (hexLen >= 1 && hexLen <= AD_HEX_MAX);
			for (Int i = 0; ok && i < hexLen; ++i)
				ok = isxdigit((unsigned char)v[i]) != 0;
			if (ok)
			{
				char hex[AD_HEX_MAX + 1] = { 0 };
				memcpy(hex, v, hexLen);
				ok = sscanf(hex, "%llX", &m) == 1 && m != 0 && ((RelayMaskType)m & ~ALL_SLOTS_MASK) == 0;
			}
			const char *k = ok ? colon + 1 : nullptr;
			for (Int i = 0; ok && i < MAX_SLOTS; ++i)
			{
				if ((m & (gxSlotBit(i))) == 0)
					continue;
				const char *end = strchr(k, ',');
				const Int len = (end != nullptr) ? (Int)(end - k) : (Int)strlen(k);
				ok = gxAdminKeyFromB64(k, len, adminKeys[i]);
				k = (end != nullptr) ? end + 1 : k + len;
				// the separator must follow every key but the last, and nothing may follow the last
				if (ok)
				{
					Bool more = FALSE;
					for (Int j = i + 1; j < MAX_SLOTS; ++j)
						if ((m & (gxSlotBit(j))) != 0)
							more = TRUE;
					ok = more ? (end != nullptr) : (end == nullptr);
				}
			}
			if (ok)
				adminMask = (RelayMaskType)m;
			else
			{
				optionsOk = false;
				DEBUG_LOG(("ParseAsciiStringToGameInfo - bad admin field '%s', quitting", val.str()));
				break;
			}
		}
		else if (key.getLength() == 1 && *key.str() == slotListID)
		{
			sawSlotlist = true;
			/// @TODO: Need to read in all the slot info... big mess right now.
			char *rawSlotBuf = strdup(val.str());
			char *freeMe = nullptr;
			AsciiString rawSlot;
//			Bool slotsOk = true;	//flag that lets us know whether or not the slot list is good.

//			DEBUG_LOG(("ParseAsciiStringToGameInfo - Parsing slot list"));
			for (int i=0; i<MAX_SLOTS; ++i)
				{
					rawSlot = strtok_r(rawSlotBuf,":",&pos);
					if( rawSlotBuf )
						freeMe = rawSlotBuf;
					rawSlotBuf = nullptr;
					// GeneralsX @feature Claude 12/08/2026 Tolerate a slot list shorter than
					// MAX_SLOTS: replays and announces written when MAX_SLOTS was 8 carry exactly
					// 8 entries, and hitting the default arm on the missing 9th used to reject the
					// whole options string - which would have orphaned every pre-raise replay,
					// including the regression corpus. Missing tail entries are closed slots.
					if (rawSlot.isEmpty() && i > 0)
					{
						for (; i < MAX_SLOTS; ++i)
							newSlot[i].setState( SLOT_CLOSED );
						break;
					}
					switch (*rawSlot.str())
					{
						case 'H':
						{
//							DEBUG_LOG(("ParseAsciiStringToGameInfo - Human player"));
							char *slotPos = nullptr;
							//Parse out the Name
							AsciiString slotValue(strtok_r((char *)rawSlot.str(),",",&slotPos));
							if(slotValue.isEmpty())
							{
								optionsOk = false;
								DEBUG_LOG(("ParseAsciiStringToGameInfo - slotValue name is empty, quitting"));
								break;
							}
							UnicodeString name;
              				name.set(MultiByteToWideCharSingleLine(slotValue.str() +1).c_str());
							// GeneralsX @bugfix Claude 12/08/2026 Bound the name HERE, where a
							// received announce turns into a slot. Memory-safe already (UnicodeString
							// allocates), but an announce can carry a ~380-character name that is then
							// rendered in the lobby list.
							//
							// INVARIANT, and read it before adding any new name path: a name reaching a
							// slot or the player list is bounded only if it EITHER comes through this
							// parser (this clamp) OR arrives in a fixed-size wire field read through the
							// field-size-deducing template in LANAPI.h. A display name arriving on its
							// own message type and calling setName directly satisfies NEITHER and this
							// clamp will not see it - a GXIDN-style identity name is exactly that shape.
							// The question for any new path is that one, not a general re-audit.
							name.truncateTo(g_lanPlayerNameLength);

							//DEBUG_LOG(("ParseAsciiStringToGameInfo - name is %s", slotValue.str()+1));

							//Parse out the IP
							slotValue = strtok_r(nullptr,",",&slotPos);
							if(slotValue.isEmpty())
							{
								optionsOk = false;
								DEBUG_LOG(("ParseAsciiStringToGameInfo - slotValue IP address is empty, quitting"));
								break;
							}
							// GeneralsX @feature Claude 12/08/2026 ".<lastOctet>" shorthand, expanded
							// against the WRITER's NP prefix. Validated strictly - this string is
							// untrusted network input, and a plausible-but-wrong address is worse
							// than a refusal: ".", ".999", ".-1", ".7.7" and a shorthand with no NP
							// all reject the whole announce rather than reconstructing something.
							UnsignedInt playerIP = 0;
							if (slotValue.getLength() > 0 && slotValue.str()[0] == '.')
							{
								const char *octetStr = slotValue.str() + 1;
								Bool octetOk = (*octetStr != 0) && sawNetPrefix;
								for (const char *c = octetStr; octetOk && *c != 0; ++c)
								{
									if (*c < '0' || *c > '9')
										octetOk = FALSE;
								}
								UnsignedInt octet = octetOk ? (UnsignedInt)atoi(octetStr) : 0;
								if (!octetOk || octet > 255)
								{
									optionsOk = false;
									DEBUG_LOG(("ParseAsciiStringToGameInfo - bad shorthand IP '%s', quitting", slotValue.str()));
									break;
								}
								playerIP = netPrefix | octet;
							}
							else if (sscanf(slotValue.str(),"%x", &playerIP) != 1)
							{
								optionsOk = false;
								DEBUG_LOG(("ParseAsciiStringToGameInfo - bad IP field '%s', quitting", slotValue.str()));
								break;
							}
							//DEBUG_LOG(("ParseAsciiStringToGameInfo - IP address is %x", playerIP));

							//set the state of the slot
							newSlot[i].setState(SLOT_PLAYER, name, playerIP);

							// parse out the port
							slotValue = strtok_r(nullptr, ",", &slotPos);
							if (slotValue.isEmpty())
							{
								optionsOk = false;
								DEBUG_LOG(("ParseAsciiStringToGameInfo - slotValue port is empty, quitting"));
								break;
							}
							// GeneralsX @feature Claude 12/08/2026 "." means NETWORK_BASE_PORT_NUMBER.
							// A bare dot, never an empty field: strtok_r collapses consecutive
							// delimiters, so ",," would shift every later field by one.
							UnsignedInt playerPort = 0;
							if (slotValue.compare(".") == 0)
							{
								playerPort = (UnsignedInt)NETWORK_BASE_PORT_NUMBER;
							}
							else if (sscanf(slotValue.str(), "%d", &playerPort) != 1)
							{
								optionsOk = false;
								DEBUG_LOG(("ParseAsciiStringToGameInfo - bad port field '%s', quitting", slotValue.str()));
								break;
							}
							newSlot[i].setPort(playerPort);
							DEBUG_LOG(("ParseAsciiStringToGameInfo - port is %d", playerPort));

							//Read if it's accepted or not
							slotValue = strtok_r(nullptr,",",&slotPos);
							if(slotValue.getLength() != 2)
							{
								optionsOk = false;
								DEBUG_LOG(("ParseAsciiStringToGameInfo - slotValue accepted is mis-sized, quitting"));
								break;
							}
							const char *svs = slotValue.str();
							if(*svs == 'T') {
								newSlot[i].setAccept();
								//DEBUG_LOG(("ParseAsciiStringToGameInfo - player has accepted"));
							} else if (*svs == 'F') {
								newSlot[i].unAccept();
								//DEBUG_LOG(("ParseAsciiStringToGameInfo - player has not accepted"));
							}
							++svs;
							if(*svs == 'T') {
								newSlot[i].setMapAvailability(TRUE);
								//DEBUG_LOG(("ParseAsciiStringToGameInfo - player has map"));
							} else {
								newSlot[i].setMapAvailability(FALSE);
								//DEBUG_LOG(("ParseAsciiStringToGameInfo - player does not have map"));
							}

							//Read color index
							slotValue = strtok_r(nullptr,",",&slotPos);
							if(slotValue.isEmpty())
							{
								optionsOk = false;
								DEBUG_LOG(("ParseAsciiStringToGameInfo - slotValue color is empty, quitting"));
								break;
							}
							Int color = atoi(slotValue.str());
							if (color < -1 || color >= TheMultiplayerSettings->getNumColors())
							{
								optionsOk = false;
								DEBUG_LOG(("ParseAsciiStringToGameInfo - player color was invalid, quitting"));
								break;
							}
							newSlot[i].setColor(color);
							//DEBUG_LOG(("ParseAsciiStringToGameInfo - player color set to %d", color));

							//Read playerTemplate index
							slotValue = strtok_r(nullptr,",",&slotPos);
							if(slotValue.isEmpty())
							{
								optionsOk = false;
								DEBUG_LOG(("ParseAsciiStringToGameInfo - slotValue player template is empty, quitting"));
								break;
							}
							Int playerTemplate = atoi(slotValue.str());
							if (playerTemplate < PLAYERTEMPLATE_MIN || playerTemplate >= ThePlayerTemplateStore->getPlayerTemplateCount())
							{
								optionsOk = false;
								DEBUG_LOG(("ParseAsciiStringToGameInfo - player template value is invalid, quitting"));
								break;
							}
							newSlot[i].setPlayerTemplate(playerTemplate);
							//DEBUG_LOG(("ParseAsciiStringToGameInfo - player template is %d", playerTemplate));

							//Read start position index
							slotValue = strtok_r(nullptr,",",&slotPos);
							if(slotValue.isEmpty())
							{
								optionsOk = false;
								DEBUG_LOG(("ParseAsciiStringToGameInfo - slotValue start position is empty, quitting"));
								break;
							}
							Int startPos = atoi(slotValue.str());
							if (startPos < -1 || startPos >= MAX_SLOTS)
							{
								optionsOk = false;
								DEBUG_LOG(("ParseAsciiStringToGameInfo - player start position is invalid, quitting"));
								break;
							}
							newSlot[i].setStartPos(startPos);
							//DEBUG_LOG(("ParseAsciiStringToGameInfo - player start position is %d", startPos));

							//Read team index
							slotValue = strtok_r(nullptr,",",&slotPos);
							if(slotValue.isEmpty())
							{
								optionsOk = false;
								DEBUG_LOG(("ParseAsciiStringToGameInfo - slotValue team number is empty, quitting"));
								break;
							}
							Int team = atoi(slotValue.str());
							if (team < -1 || team >= MAX_SLOTS/2)
							{
								optionsOk = false;
								DEBUG_LOG(("ParseAsciiStringToGameInfo - team number is invalid, quitting"));
								break;
							}
							newSlot[i].setTeamNumber(team);
							//DEBUG_LOG(("ParseAsciiStringToGameInfo - team number is %d", team));

							// Read the NAT behavior
							slotValue = strtok_r(nullptr, ",",&slotPos);
							if (slotValue.isEmpty())
							{
								optionsOk = false;
								DEBUG_LOG(("ParseAsciiStringToGameInfo - NAT behavior is empty, quitting"));
								break;
							}
							FirewallHelperClass::FirewallBehaviorType NATType = (FirewallHelperClass::FirewallBehaviorType)atoi(slotValue.str());
							if ((NATType < FirewallHelperClass::FIREWALL_MIN) ||
									(NATType > FirewallHelperClass::FIREWALL_MAX)) {
								optionsOk = false;
								DEBUG_LOG(("ParseAsciiStringToGameInfo - NAT behavior is invalid, quitting"));
								break;
							}
							newSlot[i].setNATBehavior(NATType);
							DEBUG_LOG(("ParseAsciiStringToGameInfo - NAT behavior is %X", NATType));
						}
						break;
						case 'C':
						{
            	DEBUG_LOG(("ParseAsciiStringToGameInfo - AI player"));
							char *slotPos = nullptr;
							//Parse out the Name
							AsciiString slotValue(strtok_r((char *)rawSlot.str(),",",&slotPos));
							if(slotValue.isEmpty())
							{
								optionsOk = false;
								DEBUG_LOG(("ParseAsciiStringToGameInfo - slotValue AI Type is empty, quitting"));
								break;
							}

							switch(*(slotValue.str() + 1))
							{
								case 'E':
								{
									newSlot[i].setState(SLOT_EASY_AI);
									//DEBUG_LOG(("ParseAsciiStringToGameInfo - Easy AI"));
								}
								break;
								case 'M':
								{
									newSlot[i].setState(SLOT_MED_AI);
									//DEBUG_LOG(("ParseAsciiStringToGameInfo - Medium AI"));
								}
								break;
								case 'H':
								{
									newSlot[i].setState(SLOT_BRUTAL_AI);
									//DEBUG_LOG(("ParseAsciiStringToGameInfo - Brutal AI"));
								}
								break;
								default:
								{
									optionsOk = false;
									DEBUG_LOG(("ParseAsciiStringToGameInfo - Unknown AI, quitting"));
								}
								break;
							}

							//Read color index
							slotValue = strtok_r(nullptr,",",&slotPos);
							if(slotValue.isEmpty())
							{
								optionsOk = false;
								DEBUG_LOG(("ParseAsciiStringToGameInfo - slotValue color is empty, quitting"));
								break;
							}
							Int color = atoi(slotValue.str());
							if (color < -1 || color >= TheMultiplayerSettings->getNumColors())
							{
								optionsOk = false;
								DEBUG_LOG(("ParseAsciiStringToGameInfo - player color was invalid, quitting"));
								break;
							}
							newSlot[i].setColor(color);
							//DEBUG_LOG(("ParseAsciiStringToGameInfo - player color set to %d", color));

							//Read playerTemplate index
							slotValue = strtok_r(nullptr,",",&slotPos);
							if(slotValue.isEmpty())
							{
								optionsOk = false;
								DEBUG_LOG(("ParseAsciiStringToGameInfo - slotValue player template is empty, quitting"));
								break;
							}
							Int playerTemplate = atoi(slotValue.str());
							if (playerTemplate < PLAYERTEMPLATE_MIN || playerTemplate >= ThePlayerTemplateStore->getPlayerTemplateCount())
							{
								optionsOk = false;
								DEBUG_LOG(("ParseAsciiStringToGameInfo - player template value is invalid, quitting"));
								break;
							}
							newSlot[i].setPlayerTemplate(playerTemplate);
							//DEBUG_LOG(("ParseAsciiStringToGameInfo - player template is %d", playerTemplate));

							//Read start pos
							slotValue = strtok_r(nullptr,",",&slotPos);
							if(slotValue.isEmpty())
							{
								optionsOk = false;
								DEBUG_LOG(("ParseAsciiStringToGameInfo - slotValue start pos is empty, quitting"));
								break;
							}
							Int startPos = atoi(slotValue.str());
							Bool isStartPosBad = FALSE;
							if (startPos < -1 || startPos >= MAX_SLOTS)
							{
								isStartPosBad = TRUE;
							}
							for (Int j=0; j<i; ++j)
							{
								if (startPos >= 0 && startPos == newSlot[i].getStartPos())
								{
									isStartPosBad = TRUE; // can't have multiple people using the same start pos
								}
							}
							if (isStartPosBad)
							{
								optionsOk = false;
								DEBUG_LOG(("ParseAsciiStringToGameInfo - start pos is invalid, quitting"));
								break;
							}
							newSlot[i].setStartPos(startPos);
							//DEBUG_LOG(("ParseAsciiStringToGameInfo - start spot is %d", startPos));

							//Read team index
							slotValue = strtok_r(nullptr,",",&slotPos);
							if(slotValue.isEmpty())
							{
								optionsOk = false;
								DEBUG_LOG(("ParseAsciiStringToGameInfo - slotValue team number is empty, quitting"));
								break;
							}
							Int team = atoi(slotValue.str());
							if (team < -1 || team >= MAX_SLOTS/2)
							{
								optionsOk = false;
								DEBUG_LOG(("ParseAsciiStringToGameInfo - team number is invalid, quitting"));
								break;
							}
							newSlot[i].setTeamNumber(team);
							//DEBUG_LOG(("ParseAsciiStringToGameInfo - team number is %d", team));

						}
						break;
						case 'O':
						{
							newSlot[i].setState( SLOT_OPEN );
							//DEBUG_LOG(("ParseAsciiStringToGameInfo - Slot is open"));
						}
						break;
						case 'X':
						{
							newSlot[i].setState( SLOT_CLOSED );
							//DEBUG_LOG(("ParseAsciiStringToGameInfo - Slot is closed"));
						}
						break;
						default:
						{
							optionsOk = false;
							DEBUG_LOG(("ParseAsciiStringToGameInfo - unrecognized slot entry, quitting"));
						}
						break;
					}
				}

			free(freeMe);
		}
		else
		{
			optionsOk = false;
			break;
		}
	}

	free(buf);

	// TheSuperHackers @tweak The following settings are no longer
	// a strict requirement in the Zero Hour Replay file:
	//  * UseStats
	//  * SuperweaponRestriction
	//  * StartingCash
	//  * OldFactionsOnly
	// In Generals they never were.
	if (optionsOk && sawMap && sawMapCRC && sawMapSize && sawSeed && sawSlotlist && sawCRC)
	{
		// GeneralsX @bugfix fbraz 05/05/2026 Recover old malformed custom-map replay headers by selecting map from cache via CRC/size.
		if ((!mapName.isEmpty()) && TheMapCache->findMap(mapName) == nullptr)
		{
			const MapMetaData* crcMatch = nullptr;
			const MapMetaData* exactMatch = nullptr;

			for (std::map<AsciiString, MapMetaData>::const_iterator it = TheMapCache->begin(); it != TheMapCache->end(); ++it)
			{
				if (it->second.m_CRC != mapCRC)
				{
					continue;
				}

				if (crcMatch == nullptr)
				{
					crcMatch = &it->second;
				}

				if (it->second.m_filesize == mapSize)
				{
					exactMatch = &it->second;
					break;
				}
			}

			const MapMetaData* selected = (exactMatch != nullptr) ? exactMatch : crcMatch;
			if (selected != nullptr)
			{
				mapName = selected->m_fileName;
				DEBUG_LOG(("ParseAsciiStringToGameInfo - map path recovered by CRC: %s", mapName.str()));
				// GeneralsX @bugfix fbraz 05/05/2026 Use stderr so the CRC-fallback resolution is visible in headless replay runs where DEBUG_LOG may be suppressed.
				fprintf(stderr, "[GeneralsX] Replay map resolved via CRC fallback: CRC=0x%08X size=%d -> '%s'\n", mapCRC, mapSize, mapName.str());
			}
		}

		// We were setting the Global Data directly here, but Instead, I'm now
		// first setting the data in game.  We'll set the global data when
		// we start a game.
		if (!game)
			return true;

		//DEBUG_LOG(("ParseAsciiStringToGameInfo - game options all good, setting info"));

		for(Int i = 0; i<MAX_SLOTS; i++)
			game->setSlot(i,newSlot[i]);

		game->setMap(mapName);
		// Before setMapCRC/setMapSize: their has-the-map re-check compares this machine's sidecars against it.
		game->setMapDataHash(mapDataHash);
		game->setMapCRC(mapCRC);
		game->setMapSize(mapSize);
		game->setMapContentsMask(mapContentsMask);
		game->setSeed(seed);
		game->setCRCInterval(crc);
		game->setUseStats(useStats);
		game->setSuperweaponRestriction(restriction);
		game->setStartingCash(startingCash);
		game->setOldFactionsOnly(oldFactionsOnly);
		// Absent LN means an unnamed lobby, and setLobbyName('') clears rather than keeps a stale
		// name from a previous announce of the same game.
		game->setLobbyName(lobbyName);
		game->setGxStress(stress);
		game->setGxAiColours(aiColours);
		game->clearGxAdmin();
		game->setGxAdminMask(adminMask);
		for (Int i = 0; i < MAX_SLOTS; ++i)
			if ((adminMask & (gxSlotBit(i))) != 0)
				game->setGxAdminKey(i, adminKeys[i]);

		return true;
	}

	DEBUG_LOG(("ParseAsciiStringToGameInfo - game options messed up"));
	return false;
}


//----------------------------------------------------------------------------------------------------------
//----------------------------------------------------------------------------------------------------------

//------------------------- SkirmishGameInfo ---------------------------------------------------------------

// ------------------------------------------------------------------------------------------------
/** CRC */
// ------------------------------------------------------------------------------------------------
void SkirmishGameInfo::crc( Xfer *xfer )
{
}

// ------------------------------------------------------------------------------------------------
/** Xfer Method */
// ------------------------------------------------------------------------------------------------
void SkirmishGameInfo::xfer( Xfer *xfer )
{
	// GeneralsX @feature Claude 25/09/2026 Version 5 in BOTH games: a save of version 5 records which observer layout
	// its game was started in (the flag at the end of this function), and every older one gave each observer a
	// Player of its own. The start that rebuilds the save's Player table has to build the one the save holds, or
	// PlayerList::xfer meets a different count and refuses the load (Codex r2) - so an older save loads in the
	// legacy layout. Generals was at 2 and jumps to 5 with Zero Hour rather than to 3, because 3 and 4 already
	// mean Zero Hour's superweapon/cash fields below; a Generals save of version 5 carries those fields exactly
	// as ZH's does.
	// GeneralsX @feature Claude 07/11/2026 Version 6 adds the AI colour mode at the end of this function: Player does not save
	// its colour, a load takes it from these slots again, and a Restart has to know the mode the game started in.
	const XferVersion currentVersion = 6;
	const XferVersion observersShareSince = 5;
	const XferVersion aiColoursSince = 6;
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );
	if (xfer->getXferMode() == XFER_LOAD)
		setLegacyObserverLayout(version < observersShareSince);


	// GeneralsX @feature Claude 06/11/2026 scale32: m_preorderMask is 64 bit in memory, the save keeps its 32-bit
	// Int (skirmish saves hold at most 8 slots, so the low word is the whole mask - save format unchanged).
	Int preorderLow = (Int)(UnsignedInt)(m_preorderMask & 0xffffffffu);
	xfer->xferInt(&preorderLow);
	if (xfer->getXferMode() == XFER_LOAD)
		m_preorderMask = (RelayMaskType)(UnsignedInt)preorderLow;
	xfer->xferInt(&m_crcInterval);
	// GeneralsX @fix Claude 23/09/2026 The one writer that bypassed setCRCInterval's floor: a loaded
	// skirmish runs m_frame % getCRCInterval() (isMultiplayer() is TRUE while TheSkirmishGameInfo is
	// set, and nothing re-sets the interval on load), so a save holding 0 divided by zero.
	if (xfer->getXferMode() == XFER_LOAD)
		setCRCInterval(m_crcInterval);
	xfer->xferBool(&m_inGame);
	xfer->xferBool(&m_inProgress);
	xfer->xferBool(&m_surrendered);
	xfer->xferInt(&m_gameID);

	// GeneralsX @bugfix Claude 24/09/2026 A save records how many slots it holds, and a load reads EXACTLY that
	// many. The old loop always read MAX_SLOTS records, so every raise of MAX_SLOTS (8 -> 12, then 12 -> 14 for
	// 12 players + 2 observers) made an older save read the fields that follow its slots as more slots - no slot
	// came out local, and MultiPlayerLoadScreen::init dereferenced getSlot(-1): SIGSEGV. A record past this build's
	// MAX_SLOTS is read into a scratch slot and dropped; a slot the save does not hold stays closed. Saving always
	// writes MAX_SLOTS, so a save written by this build is unchanged.
	// GeneralsX @bugfix Claude 25/09/2026 (merge of the 25/09 save gate) A count no build has written is not a
	// slot table: refused before anything reads it, with no assert ahead of the recovery, so a debug build takes
	// the same path; the caller is inside GameState::loadGame's catch. A save whose local slot this build drops
	// is stopped by MultiPlayerLoadScreen's own local-slot guard.
	Int savedSlots = MAX_SLOTS;
	xfer->xferInt(&savedSlots);
	if (savedSlots < 0 || savedSlots > 64)
	{
		fprintf(stderr, "[SAVELOAD] SkirmishGameInfo: the save claims %d slots, which no build writes; aborting the load\n", (int)savedSlots);
		throw XFER_READ_ERROR;
	}

	GameSlot scratchSlot;
	for (Int slot = 0; slot < savedSlots; slot++)
	{
		GameSlot *const gameSlot = (slot < MAX_SLOTS) ? m_slot[slot] : &scratchSlot;
		Int state = gameSlot->getState();
		xfer->xferInt(&state);

		UnicodeString name=gameSlot->getName();
		if (version >= 2)
		{
			xfer->xferUnicodeString(&name);
		}

		Bool isAccepted=gameSlot->isAccepted();
		xfer->xferBool(&isAccepted);

		Bool isMuted=gameSlot->isMuted();
		xfer->xferBool(&isMuted);
		gameSlot->mute(isMuted);

		Int color=gameSlot->getColor();
		xfer->xferInt(&color);

		Int startPos=gameSlot->getStartPos();
		xfer->xferInt(&startPos);

		Int playerTemplate=gameSlot->getPlayerTemplate();
		xfer->xferInt(&playerTemplate);

		Int teamNumber=gameSlot->getTeamNumber();
		xfer->xferInt(&teamNumber);

		Int origColor=gameSlot->getOriginalColor();
		xfer->xferInt(&origColor);

		Int origStartPos=gameSlot->getOriginalStartPos();
		xfer->xferInt(&origStartPos);

 		Int origPlayerTemplate=gameSlot->getOriginalPlayerTemplate();
		xfer->xferInt(&origPlayerTemplate);

		if( xfer->getXferMode() == XFER_LOAD ) {
			gameSlot->setState((SlotState)state, name);
			if (isAccepted) gameSlot->setAccept();

			gameSlot->setPlayerTemplate(origPlayerTemplate);
			gameSlot->setStartPos(origStartPos);
			gameSlot->setColor(origColor);
			gameSlot->saveOriginalSetup();

			gameSlot->setTeamNumber(teamNumber);
			gameSlot->setColor(color);
			gameSlot->setStartPos(startPos);
			gameSlot->setPlayerTemplate(playerTemplate);
		}
	}

	if (xfer->getXferMode() == XFER_LOAD)
	{
		for (Int slot = savedSlots; slot < MAX_SLOTS; ++slot)
			m_slot[slot]->setState(SLOT_CLOSED);
	}

	xfer->xferUnsignedInt(&m_localIP);

	xfer->xferMapName(&m_mapName);
	xfer->xferUnsignedInt(&m_mapCRC);
	xfer->xferUnsignedInt(&m_mapSize);
	xfer->xferInt(&m_mapMask);
	xfer->xferInt(&m_seed);

  if ( version >= 3 )
  {
    xfer->xferUnsignedShort( &m_superweaponRestriction );

    if ( version == 3 )
    {
      // Version 3 had a bool which is now gone
      Bool obsoleteBool;
      xfer->xferBool( &obsoleteBool );
    }

    xfer->xferSnapshot( &m_startingCash );
  }
  else if ( xfer->getXferMode() == XFER_LOAD )
  {
    m_superweaponRestriction = 0;
    m_startingCash = TheGlobalData->m_defaultStartingCash;
  }

	// GeneralsX @feature Claude 25/09/2026 Version 5 records the observer layout this game was STARTED in, not the
	// one this build starts new games in: a pre-5 save loaded and saved again without a restart still holds the
	// legacy Player table (one Player per observer), and a version 5 that implied the shared layout would rebuild a
	// different table on the next load (Codex r4). An older version implies legacy (set above, before the slots).
	if (version >= observersShareSince)
	{
		Bool legacy = isLegacyObserverLayout();
		xfer->xferBool(&legacy);
		if (xfer->getXferMode() == XFER_LOAD)
			setLegacyObserverLayout(legacy);
	}

	if (version >= aiColoursSince)
		xfer->xferInt(&m_gxAiColours);
	else if (xfer->getXferMode() == XFER_LOAD)
		m_gxAiColours = GXAiColours::MODE_ALL;
}

// ------------------------------------------------------------------------------------------------
/** Load post process */
// ------------------------------------------------------------------------------------------------
void SkirmishGameInfo::loadPostProcess()
{
}

// GeneralsX @bugfix Claude 01/10/2026 Idea of TheSuperHackers PR #3200 (Caball009, draft), hand-ported. A restarted
// skirmish's replay header was written from the slots BEFORE tryStartNewGame put the original setup back, so the
// replay of every restarted skirmish mismatched on playback. Unlike the draft, tryStartNewGame keeps its own loop
// for every game mode: GameSlot::isObserver() needs the saved original setup in LAN and relay games too. And this only
// restores, never saves: tryStartNewGame saves AFTER checkForDuplicateColors, and a save here, before that check,
// would bring a duplicate colour back on the restore.
void SkirmishGameInfo::restoreOriginalSetups()
{
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		GameSlot& slot = m_skirmishSlot[i];

		if (slot.hasSavedOriginalSetup())
		{
			slot.setColor(slot.getOriginalColor());
			slot.setStartPos(slot.getOriginalStartPos());
			slot.setPlayerTemplate(slot.getOriginalPlayerTemplate());
		}
	}
}


