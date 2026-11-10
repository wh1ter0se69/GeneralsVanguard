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
// FILE: UserPreferences.cpp
// Author: Matthew D. Campbell, April 2002
// Description: Saving/Loading of user preferences
///////////////////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------------
// SYSTEM INCLUDES ////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include <errno.h>		// load(): ENOENT is the only "no file"
#include <string.h>

//-----------------------------------------------------------------------------
// USER INCLUDES //////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------
#include "Common/GameSpyMiscPreferences.h"
#include "Common/UserPreferences.h"
#include "GameClient/GXReplaceFile.h"
#include "Common/LadderPreferences.h"
#include "Common/Player.h"
#include "Common/PlayerTemplate.h"
#include "Common/Registry.h"
#include "Common/QuickmatchPreferences.h"
#include "Common/CustomMatchPreferences.h"
#include "Common/IgnorePreferences.h"
#include "Common/QuotedPrintable.h"
#include "Common/MultiplayerSettings.h"
#include "GameClient/MapUtil.h"
#include "GameClient/ChallengeGenerals.h"
#include "GameNetwork/GameSpy/PeerDefs.h"


//-----------------------------------------------------------------------------
// DEFINES ////////////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// PRIVATE TYPES //////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// PRIVATE DATA ///////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// PUBLIC DATA ////////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// PRIVATE PROTOTYPES /////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// PRIVATE FUNCTIONS //////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

static AsciiString intAsStr(Int val)
{
	AsciiString ret;
	ret.format("%d", val);
	return ret;
}

static AsciiString boolAsStr(Bool val)
{
	AsciiString ret;
	ret.format("%d", val);
	return ret;
}

static AsciiString realAsStr(Real val)
{
	AsciiString ret;
	ret.format("%g", val);
	return ret;
}

//-----------------------------------------------------------------------------
// PUBLIC FUNCTIONS ///////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------


//-----------------------------------------------------------------------------
// UserPreferences Class
//-----------------------------------------------------------------------------

UserPreferences::UserPreferences() : m_unreadable(FALSE)
{
}

// GeneralsX: A file that exists and cannot be read is never replaced (the
// file-wipe class). load() used to keep the name and return false for ANY fopen failure, and most callers ignore that
// result (OptionPreferences' constructor, SkirmishBattleHonors, LANPreferences...), so the next write() moved the few
// keys set since over the whole file: the in-game Options menu's Accept, GameLOD's automatic write at startup, and
// SkirmishStats.ini - campaign and challenge completion - after every game. Only ENOENT is "no file" now.
// Capped: some screens build an OptionPreferences per refresh, and a file that stays unreadable must not flood the log.
static void noteUnreadable(const AsciiString &path, int err)
{
	static Int s_notes = 0;
	if (s_notes >= 8)
		return;
	++s_notes;
	fprintf(stderr, "[GXPREFS] %s exists but could not be read (%s, errno %d): it is left exactly as it is, and "
		"nothing is saved over it from this read.%s\n", path.str(), strerror(err), err,
		s_notes == 8 ? " (further unreadable preference reads are not logged)" : "");
}

UserPreferences::~UserPreferences()
{
}

#define LINE_LEN 2048
Bool UserPreferences::load(AsciiString fname)
{
//	if (strstr(fname.str(), "\\"))
//		throw INI_INVALID_DATA;	// must be a leaf name

	m_filename = TheGlobalData->getPath_UserData();
	m_filename.concat(fname);
	m_unreadable = FALSE;

	errno = 0;
	FILE *fp = fopen(m_filename.str(), "r");
	const int openErr = errno;
	// A dangling symlink opens as ENOENT too, and writing over its name would cut the player's file off when its volume
	// comes back (gxNameExists, GXReplaceFile.h).
	if (fp == nullptr && (openErr != ENOENT || gxNameExists(m_filename.str())))
	{
		m_unreadable = TRUE;
		noteUnreadable(m_filename, openErr);
	}
	if (fp)
	{
		char buf[LINE_LEN];
		Bool firstLine = TRUE;
		while( fgets( buf, LINE_LEN, fp ) != nullptr )
		{
			// GeneralsX: A UTF-8 BOM (a hand edit in an older Notepad) is not part of the first key.
			// Kept, it hid that key from the game while the launcher (gxio strip_bom) read it, so a first line
			// "GXRenderer = native" showed the new renderer on in the launcher and ran classic.
			const char *start = buf;
			if (firstLine && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF)
				start = buf + 3;
			firstLine = FALSE;

			AsciiString line = start;
			line.trim();

			AsciiString key, val;
			line.nextToken(&key, "=");

			// GeneralsX @bugfix: nextToken() leaves the remainder starting at the '='
			// separator ONLY when it actually found one. On a line with no '=' (a blank
			// line, a hand-added comment, a file truncated mid-write, or the tail chunk
			// of a >LINE_LEN line that fgets split), it consumes the whole string and
			// leaves 'line' empty. AsciiString::str() then returns the address of a
			// one-byte static NUL, so the old unconditional "line.str() + 1" handed
			// operator= a pointer one byte PAST that static and strlen() ran off it -
			// an out-of-bounds read whose length, and therefore whether it faults or
			// silently injects a garbage preference value, depends on whatever the
			// linker placed next. Skip the line instead; with no '=' there is no
			// key/value pair to store, which is what the key/val emptiness test below
			// was already doing for the malformed lines it happened to catch.
			if (line.isEmpty())
				continue;

			val = line.str() + 1;

			key.trim();
			val.trim();

			if (key.isEmpty() || val.isEmpty())
				continue;

			(*this)[key] = val;
		}
		// fgets returns NULL for an error as well as the end: a read that failed part way (EISDIR for a directory,
		// EIO) holds part of the file, and writing that part back would drop the rest.
		if (ferror(fp))
		{
			m_unreadable = TRUE;
			noteUnreadable(m_filename, errno);
		}
		fclose(fp);
		return true;
	}
	return false;
}

Bool UserPreferences::write()
{
	if (m_filename.isEmpty())
		return false;
	if (m_unreadable)
		return false;	// load() said why; the file stays exactly as it is

	// GeneralsX: Written beside the file and then swapped into place, never
	// truncated in place. This writes Options.ini, and on Windows that file lives under Documents,
	// which OneDrive keeps under sync. See GameClient/GXReplaceFile.h.
	AsciiString tmp = m_filename;
	tmp.concat(".tmp");
	FILE *fp = fopen(tmp.str(), "w");
	if (fp)
	{
		PreferenceMap::const_iterator it = begin();
		while (it != end())
		{
			fprintf(fp, "%s = %s\n", it->first.str(), it->second.str());
			++it;
		}
	}
	return gxCommitAndReplace(fp, tmp.str(), m_filename.str());
}

Bool UserPreferences::getBool(AsciiString key, Bool defaultValue) const
{
	AsciiString val = getAsciiString(key, AsciiString::TheEmptyString);
	if (val.isEmpty())
	{
		return defaultValue;
	}

	val.toLower();
	return (val == "1" || val == "t" || val == "true" || val == "y" || val == "yes" || val == "ok");
}

Real UserPreferences::getReal(AsciiString key, Real defaultValue) const
{
	AsciiString val = getAsciiString(key, AsciiString::TheEmptyString);
	if (val.isEmpty())
	{
		return defaultValue;
	}

	return (Real)atof(val.str());
}

Int UserPreferences::getInt(AsciiString key, Int defaultValue) const
{
	AsciiString val = getAsciiString(key, AsciiString::TheEmptyString);
	if (val.isEmpty())
	{
		return defaultValue;
	}

	return atoi(val.str());
}

AsciiString UserPreferences::getAsciiString(AsciiString key, AsciiString defaultValue) const
{
	UserPreferences::const_iterator it = find(key);
	if (it == end())
	{
		return defaultValue;
	}

	return it->second;
}

void UserPreferences::setBool(AsciiString key, Bool val)
{
	(*this)[key] = boolAsStr(val);
}

void UserPreferences::setReal(AsciiString key, Real val)
{
	(*this)[key] = realAsStr(val);
}

void UserPreferences::setInt(AsciiString key, Int val)
{
	(*this)[key] = intAsStr(val);
}

void UserPreferences::setAsciiString(AsciiString key, AsciiString val)
{
	(*this)[key] = val;
}

//-----------------------------------------------------------------------------
// QuickMatchPreferences base class
//-----------------------------------------------------------------------------

QuickMatchPreferences::QuickMatchPreferences()
{
	AsciiString userPrefFilename;
	Int localProfile = TheGameSpyInfo->getLocalProfileID();
	userPrefFilename.format("GeneralsOnline\\QMPref%d.ini", localProfile);
	load(userPrefFilename);
}

QuickMatchPreferences::~QuickMatchPreferences()
{
}

void QuickMatchPreferences::setMapSelected(const AsciiString& mapName, Bool selected)
{
	(*this)[AsciiStringToQuotedPrintable(mapName)] = (selected)?"1":"0";
}

Bool QuickMatchPreferences::isMapSelected(const AsciiString& mapName)
{
	Int ret;
	QuickMatchPreferences::const_iterator it = find(AsciiStringToQuotedPrintable(mapName));
	if (it == end())
	{
		return TRUE;
	}

	ret = atoi(it->second.str());

	return (ret != 0);
}

void QuickMatchPreferences::setLastLadder(const AsciiString& addr, UnsignedShort port)
{
	AsciiString strVal;
	strVal.format("%d", port);
	(*this)["LastLadderAddr"] = addr;
	(*this)["LastLadderPort"] = strVal;
}

AsciiString QuickMatchPreferences::getLastLadderAddr()
{
	QuickMatchPreferences::const_iterator it = find("LastLadderAddr");
	if (it == end())
	{
		return AsciiString::TheEmptyString;
	}
	return it->second;
}

UnsignedShort QuickMatchPreferences::getLastLadderPort()
{
	QuickMatchPreferences::const_iterator it = find("LastLadderPort");
	if (it == end())
	{
		return 0;
	}
	return atoi(it->second.str());
}

void QuickMatchPreferences::setMaxDisconnects(Int val)
{
	AsciiString strVal;
	strVal.format("%d", val);
	(*this)["MaxDisconnects"] = strVal;
}

Int QuickMatchPreferences::getMaxDisconnects()
{
	QuickMatchPreferences::const_iterator it = find("MaxDisconnects");
	if (it == end())
	{
		return 0;
	}
	return atoi(it->second.str());
}

void QuickMatchPreferences::setMaxPoints(Int val)
{
	AsciiString strVal;
	strVal.format("%d", val);
	(*this)["MaxPoints"] = strVal;
}

Int QuickMatchPreferences::getMaxPoints()
{
	QuickMatchPreferences::const_iterator it = find("MaxPoints");
	if (it == end())
	{
		return 1000;
	}
	return atoi(it->second.str());
}

void QuickMatchPreferences::setMinPoints(Int val)
{
	AsciiString strVal;
	strVal.format("%d", val);
	(*this)["MinPoints"] = strVal;
}

Int QuickMatchPreferences::getMinPoints()
{
	QuickMatchPreferences::const_iterator it = find("MinPoints");
	if (it == end())
	{
		return 0;
	}
	return atoi(it->second.str());
}

void QuickMatchPreferences::setWaitTime(Int val)
{
	AsciiString strVal;
	strVal.format("%d", val);
	(*this)["WaitTime"] = strVal;
}

Int QuickMatchPreferences::getWaitTime()
{
	QuickMatchPreferences::const_iterator it = find("WaitTime");
	if (it == end())
	{
		return 0;
	}
	return atoi(it->second.str());
}

void QuickMatchPreferences::setNumPlayers(Int val)
{
	AsciiString strVal;
	strVal.format("%d", val);
	(*this)["NumPlayers"] = strVal;
}

Int QuickMatchPreferences::getNumPlayers()
{
	QuickMatchPreferences::const_iterator it = find("NumPlayers");
	if (it == end())
	{
		return 0;	// first in list, 1v1
	}
	return atoi(it->second.str());
}

void QuickMatchPreferences::setMaxPing(Int val)
{
	AsciiString strVal;
	strVal.format("%d", val);
	(*this)["MaxPing"] = strVal;
}

Int QuickMatchPreferences::getMaxPing()
{
	QuickMatchPreferences::const_iterator it = find("MaxPing");
	if (it == end())
	{
		return 5;
	}
	return atoi(it->second.str());
}

void QuickMatchPreferences::setColor( Int val )
{
	setInt("Color", val);
}

Int QuickMatchPreferences::getColor()
{
	return getInt("Color", 0);
}

void QuickMatchPreferences::setSide( Int val )
{
	setInt("Side", val);
}

Int QuickMatchPreferences::getSide()
{
	return getInt("Side", 0);
}

//-----------------------------------------------------------------------------
// CustomMatchPreferences base class
//-----------------------------------------------------------------------------

CustomMatchPreferences::CustomMatchPreferences()
{
	// GeneralsX: The file name is keyed on the GameSpy profile, and in relay
	// mode TheGameSpyInfo only exists while the Online lobby is up. A construction outside that
	// window (the lobby's second shutdown at process exit was one) read through a null pointer and
	// killed the process in ~GameEngine. With no profile there is no file to name: load nothing, and
	// leave m_filename empty so write() refuses rather than inventing a CustomPref0.ini.
	if (TheGameSpyInfo == nullptr)
		return;

	AsciiString userPrefFilename;
	Int localProfile = TheGameSpyInfo->getLocalProfileID();
	userPrefFilename.format("GeneralsOnline\\CustomPref%d.ini", localProfile);
	load(userPrefFilename);
}

CustomMatchPreferences::~CustomMatchPreferences()
{
}

void CustomMatchPreferences::setLastLadder(const AsciiString& addr, UnsignedShort port)
{
	AsciiString strVal;
	strVal.format("%d", port);
	(*this)["LastLadderAddr"] = addr;
	(*this)["LastLadderPort"] = strVal;
}

AsciiString CustomMatchPreferences::getLastLadderAddr()
{
	QuickMatchPreferences::const_iterator it = find("LastLadderAddr");
	if (it == end())
	{
		return AsciiString::TheEmptyString;
	}
	return it->second;
}

UnsignedShort CustomMatchPreferences::getLastLadderPort()
{
	QuickMatchPreferences::const_iterator it = find("LastLadderPort");
	if (it == end())
	{
		return 0;
	}
	return atoi(it->second.str());
}

Int CustomMatchPreferences::getPreferredColor()
{
	Int ret;
	CustomMatchPreferences::const_iterator it = find("Color");
	if (it == end())
	{
		return -1;
	}

	ret = atoi(it->second.str());
	if (ret < -1 || ret >= TheMultiplayerSettings->getNumColors())
		ret = -1;

	return ret;
}

void CustomMatchPreferences::setPreferredColor(Int val)
{
	AsciiString s;
	s.format("%d", val);
	(*this)["Color"] = s;
}

Int CustomMatchPreferences::getChatSizeSlider()
{
	Int ret;
	CustomMatchPreferences::const_iterator it = find("ChatSlider");
	if (it == end())
	{
		return 45;
	}

	ret = atoi(it->second.str());
	if (ret < 0 || ret > 100)
		ret = 45;

	return ret;
}

void CustomMatchPreferences::setChatSizeSlider(Int val)
{
	AsciiString s;
	s.format("%d", val);
	(*this)["ChatSlider"] = s;
}

Int CustomMatchPreferences::getPreferredFaction()
{
	Int ret;
	CustomMatchPreferences::const_iterator it = find("PlayerTemplate");
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
		else if (TheGameInfo && TheGameInfo->oldFactionsOnly() && !fac->isOldFaction())
			ret = PLAYERTEMPLATE_RANDOM;
		else {
			// Prevent from loading the disabled Generals, in case you had previously selected one as your preferred faction.
			// This is also enforced at GUI setup (GUIUtil.cpp and GameLogic.cpp).
			// @todo: unlock these when something rad happens
			Bool disallowLockedGenerals = TRUE;
			const GeneralPersona *general = TheChallengeGenerals->getGeneralByTemplateName(fac->getName());
			Bool startsLocked = general ? !general->isStartingEnabled() : FALSE;
			if (disallowLockedGenerals && startsLocked)
				ret = PLAYERTEMPLATE_RANDOM;
		}
	}

	return ret;
}

void CustomMatchPreferences::setPreferredFaction(Int val)
{
	AsciiString s;
	s.format("%d", val);
	(*this)["PlayerTemplate"] = s;
}

Bool CustomMatchPreferences::usesSystemMapDir()
{
	CustomMatchPreferences::const_iterator it = find("UseSystemMapDir");
	if (it == end())
		return TRUE;

	if (stricmp(it->second.str(), "1") == 0) {
		return TRUE;
	}
	return FALSE;
}

void CustomMatchPreferences::setUsesSystemMapDir(Bool val)
{
	AsciiString s;
	s.format("%d", val);
	(*this)["UseSystemMapDir"] = s;
}

Bool CustomMatchPreferences::usesLongGameList()
{
	return TRUE;
	CustomMatchPreferences::const_iterator it = find("UseLongGameList");
	if (it == end())
		return FALSE;

	if (stricmp(it->second.str(), "1") == 0) {
		return TRUE;
	}
	return FALSE;
}

void CustomMatchPreferences::setUsesLongGameList(Bool val)
{
	AsciiString s;
	s.format("%d", val);
	(*this)["UseLongGameList"] = s;
}

Bool CustomMatchPreferences::allowsObservers()
{
	CustomMatchPreferences::const_iterator it = find("AllowObservers");
	if (it == end())
		return TRUE;

	if (stricmp(it->second.str(), "1") == 0) {
		return TRUE;
	}
	return FALSE;
}

void CustomMatchPreferences::setAllowsObserver(Bool val)
{
	AsciiString s;
	s.format("%d", val);
	(*this)["AllowObservers"] = s;
}

Bool CustomMatchPreferences::getDisallowAsianText()
{
	CustomMatchPreferences::const_iterator it = find("DisallowAsianText");
	if (it == end())
	{
		// since English Win98 machines don't have a Unicode font installed by default,
		// we're forced to disable asian chat by default for English builds.
		if (GetRegistryLanguage().compareNoCase("chinese") == 0 || GetRegistryLanguage().compareNoCase("korean") == 0 )
			return FALSE;
		else
			return TRUE;
	}

	if (stricmp(it->second.str(), "1") == 0) {
		return TRUE;
	}
	return FALSE;
}

void CustomMatchPreferences::setDisallowAsianText(Bool val)
{
	AsciiString s;
	s.format("%d", val);
	(*this)["DisallowAsianText"] = s;

}

Bool CustomMatchPreferences::getDisallowNonAsianText()
{
	CustomMatchPreferences::const_iterator it = find("DisallowNonAsianText");
	if (it == end())
		return FALSE;

	if (stricmp(it->second.str(), "1") == 0) {
		return TRUE;
	}
	return FALSE;
}

void CustomMatchPreferences::setDisallowNonAsianText( Bool val )
{
	AsciiString s;
	s.format("%d", val);
	(*this)["DisallowNonAsianText"] = s;
}

AsciiString CustomMatchPreferences::getPreferredMap()
{
	AsciiString ret;
	CustomMatchPreferences::const_iterator it = find("Map");
	if (it == end())
	{	//map not found, use default instead
		ret = getDefaultOfficialMap();
		return ret;
	}

	ret = QuotedPrintableToAsciiString(it->second);
	ret.trim();
	if (ret.isEmpty() || !isValidMap(ret, TRUE))
	{	//map is invalid, use default instead
		ret = getDefaultOfficialMap();
		return ret;
	}

	//can only use official maps if recording stats
	if( getUseStats() && !isOfficialMap(ret) )
		ret = getDefaultOfficialMap();
	return ret;
}

void CustomMatchPreferences::setPreferredMap(AsciiString val)
{
	(*this)["Map"] = AsciiStringToQuotedPrintable(val);
}


static const char superweaponRestrictionKey[] = "SuperweaponRestrict";

Bool CustomMatchPreferences::getSuperweaponRestricted() const
{
  const_iterator it = find(superweaponRestrictionKey);
  if (it == end())
  {
    return false;
  }

  return ( it->second.compareNoCase( "yes" ) == 0 );
}

void CustomMatchPreferences::setSuperweaponRestricted( Bool superweaponRestricted )
{
  (*this)[superweaponRestrictionKey] = superweaponRestricted ? "Yes" : "No";
}

static const char startingCashKey[] = "StartingCash";
Money CustomMatchPreferences::getStartingCash() const
{
  const_iterator it = find(startingCashKey);
  if (it == end())
  {
    return TheMultiplayerSettings->getDefaultStartingMoney();
  }

  Money money;
  money.deposit( strtoul( it->second.str(), nullptr, 10 ), FALSE, FALSE );

  return money;
}

void CustomMatchPreferences::setStartingCash( const Money & startingCash )
{
  AsciiString option;

  option.format( "%d", startingCash.countMoney() );

  (*this)[startingCashKey] = option;
}


static const char limitFactionsKey[] = "LimitArmies";

// Prefers to only use the original 3 sides, not USA Air Force General, GLA Toxin General, et al
Bool CustomMatchPreferences::getFactionsLimited() const
{
  const_iterator it = find(limitFactionsKey);
  if (it == end())
  {
    return false; // The default
  }

  return ( it->second.compareNoCase( "yes" ) == 0 );
}

void CustomMatchPreferences::setFactionsLimited( Bool factionsLimited )
{
  (*this)[limitFactionsKey] = factionsLimited ? "Yes" : "No";
}


static const char useStatsKey[] = "UseStats";

Bool CustomMatchPreferences::getUseStats() const
{
  const_iterator it = find(useStatsKey);
  if (it == end())
  {
    return true; // The default
  }

  return ( it->second.compareNoCase( "yes" ) == 0 );
}

void CustomMatchPreferences::setUseStats( Bool useStats )
{
  (*this)[useStatsKey] = useStats ? "Yes" : "No";
}

//-----------------------------------------------------------------------------
// GameSpyMiscPreferences base class
//-----------------------------------------------------------------------------

GameSpyMiscPreferences::GameSpyMiscPreferences()
{
	AsciiString userPrefFilename;
	Int localProfile = TheGameSpyInfo->getLocalProfileID();
	userPrefFilename.format("GeneralsOnline\\GSMiscPref%d.ini", localProfile);
	load(userPrefFilename);
}

GameSpyMiscPreferences::~GameSpyMiscPreferences()
{
}

Int GameSpyMiscPreferences::getLocale()
{
	return getInt("Locale", 0);
}

void GameSpyMiscPreferences::setLocale( Int val )
{
	setInt("Locale", val);
}

AsciiString GameSpyMiscPreferences::getCachedStats()
{
	return getAsciiString("CachedStats", AsciiString::TheEmptyString);
}

void GameSpyMiscPreferences::setCachedStats( AsciiString val )
{
	setAsciiString("CachedStats", val);
}

Bool GameSpyMiscPreferences::getQuickMatchResLocked()
{
	return getBool("QMResLock", FALSE);
}

Int GameSpyMiscPreferences::getMaxMessagesPerUpdate()
{
	return getInt("MaxMessagesPerUpdate", 100);
}

//-----------------------------------------------------------------------------
// IgnorePreferences base class
//-----------------------------------------------------------------------------

IgnorePreferences::IgnorePreferences()
{
	AsciiString userPrefFilename;
//	if(!TheGameSpyInfo)
	Int localProfile = TheGameSpyInfo->getLocalProfileID();
	userPrefFilename.format("GeneralsOnline\\IgnorePref%d.ini", localProfile);
	load(userPrefFilename);
}

IgnorePreferences::~IgnorePreferences()
{
}

void IgnorePreferences::setIgnore(const AsciiString& userName, Int profileID, Bool ignore)
{
	AsciiString strVal;
	strVal.format("%d", profileID);
	if (ignore)
	{
		(*this)[strVal] = userName;
	}
	else
	{
		erase(strVal);
	}
}

IgnorePrefMap IgnorePreferences::getIgnores()
{
	IgnorePrefMap ignores;

	IgnorePreferences::iterator it;
	for (it = begin(); it != end(); ++it)
	{
		AsciiString profileStr = it->first;
		AsciiString lastLoginStr = it->second;
		Int profileID = atoi(profileStr.str());

		ignores[profileID] = lastLoginStr;
	}

	return ignores;
}

//-----------------------------------------------------------------------------
// LadderPreferences base class
//-----------------------------------------------------------------------------

LadderPreferences::LadderPreferences()
{
}

LadderPreferences::~LadderPreferences()
{
}

Bool LadderPreferences::loadProfile( Int profileID )
{
	clear();
	m_ladders.clear();
	AsciiString userPrefFilename;
	userPrefFilename.format("GeneralsOnline\\Ladders%d.ini", profileID);
	Bool success = load(userPrefFilename);
	if (!success)
		return success;

	// parse out our ladders
	for (LadderPreferences::iterator it = begin(); it != end(); ++it)
	{
		LadderPref p;
		AsciiString ladName = it->first;
		AsciiString ladData = it->second;

		DEBUG_LOG(("Looking at [%s] = [%s]", ladName.str(), ladData.str()));

		const char *ptr = ladName.reverseFind(':');
		DEBUG_ASSERTCRASH(ptr, ("Did not find ':' in ladder name - skipping"));
		if (!ptr)
			continue;

		p.port = atoi( ptr + 1 );
		ladName.truncateBy(strlen(ptr));
		p.address = QuotedPrintableToAsciiString(ladName);

		ptr = ladData.reverseFind(':');
		DEBUG_ASSERTCRASH(ptr, ("Did not find ':' in ladder data - skipping"));
		if (!ptr)
			continue;

		p.lastPlayDate = atoi( ptr + 1 );
		ladData.truncateBy(strlen(ptr));
		p.name = QuotedPrintableToUnicodeString(ladData);

		m_ladders[p.lastPlayDate] = p;
	}

	return true;
}

bool LadderPreferences::write()
{
	clear();
	LadderPrefMap::iterator lpIt;

	static const Int MAX_LADDERS = 5;
	Int count;
	for (lpIt = m_ladders.begin(), count=0; lpIt != m_ladders.end() && count<MAX_LADDERS; ++lpIt, ++count)
	{
		LadderPref p = lpIt->second;
		AsciiString ladName;
		AsciiString ladData;
		ladName.format("%s:%d", AsciiStringToQuotedPrintable(p.address).str(), p.port);
		ladData.format("%s:%d", UnicodeStringToQuotedPrintable(p.name).str(), p.lastPlayDate);
		(*this)[ladName] = ladData;
	}

	return UserPreferences::write();
}

const LadderPrefMap& LadderPreferences::getRecentLadders()
{
	return m_ladders;
}

void LadderPreferences::addRecentLadder( LadderPref ladder )
{
	for (LadderPrefMap::iterator it = m_ladders.begin(); it != m_ladders.end(); ++it)
	{
		if (it->second == ladder)
		{
			m_ladders.erase(it);
			break;
		}
	}

	m_ladders[ladder.lastPlayDate] = ladder;
}
