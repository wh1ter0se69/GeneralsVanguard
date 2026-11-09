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

// FILE: Upgrade.h ////////////////////////////////////////////////////////////////////////////////
// Author: Colin Day, March 2002
// Desc:   Upgrade system for players
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

// USER INCLUDES //////////////////////////////////////////////////////////////////////////////////
#include "Common/AudioEventRTS.h"
#include "Common/INI.h"
#include "Common/Snapshot.h"
#include "Common/BitFlags.h"
#include "Common/GXWireId.h"

#include <map>

// FORWARD REFERENCES /////////////////////////////////////////////////////////////////////////////
class Player;
class UpgradeTemplate;
enum NameKeyType CPP_11(: Int);
class Image;
enum AcademyClassificationType CPP_11(: Int);

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
enum UpgradeStatusType CPP_11(: Int)
{
	UPGRADE_STATUS_INVALID = 0,
	UPGRADE_STATUS_IN_PRODUCTION,
	UPGRADE_STATUS_COMPLETE
};

//The maximum number of upgrades.
// TheSuperHackers @tweak Stubbjax 22/01/2026 Increases max upgrade count from Generals:64, Zero Hour:128 to allow for more upgrades.
// A value of 512 was chosen to allow room for plenty of upgrades while also conserving memory.
#define UPGRADE_MAX_COUNT 512

typedef BitFlags<UPGRADE_MAX_COUNT, struct UpgradeMaskTypeTag>	UpgradeMaskType;

#define MAKE_UPGRADE_MASK(k) UpgradeMaskType(UpgradeMaskType::kInit, (k))
#define MAKE_UPGRADE_MASK2(k,a) UpgradeMaskType(UpgradeMaskType::kInit, (k), (a))
#define MAKE_UPGRADE_MASK3(k,a,b) UpgradeMaskType(UpgradeMaskType::kInit, (k), (a), (b))
#define MAKE_UPGRADE_MASK4(k,a,b,c) UpgradeMaskType(UpgradeMaskType::kInit, (k), (a), (b), (c))
#define MAKE_UPGRADE_MASK5(k,a,b,c,d) UpgradeMaskType(UpgradeMaskType::kInit, (k), (a), (b), (c), (d))

inline Bool TEST_UPGRADE_MASK( const UpgradeMaskType& m, Int index )
{
	return m.test( index );
}

inline Bool TEST_UPGRADE_MASK_ANY( const UpgradeMaskType& m, const UpgradeMaskType& mask )
{
	return m.anyIntersectionWith( mask );
}

inline Bool TEST_UPGRADE_MASK_MULTI( const UpgradeMaskType& m, const UpgradeMaskType& mustBeSet, const UpgradeMaskType& mustBeClear )
{
	return m.testSetAndClear( mustBeSet, mustBeClear );
}

inline Bool UPGRADE_MASK_ANY_SET( const UpgradeMaskType& m)
{
	return m.any();
}

inline void CLEAR_UPGRADE_MASK( UpgradeMaskType& m )
{
	m.clear();
}

inline void SET_ALL_UPGRADE_MASK_BITS( UpgradeMaskType& m )
{
	m.clear();
	m.flip();
}

inline void FLIP_UPGRADE_MASK( UpgradeMaskType& m )
{
	m.flip();
}


//-------------------------------------------------------------------------------------------------
/** A single upgrade *INSTANCE* */
//-------------------------------------------------------------------------------------------------
class Upgrade : public MemoryPoolObject,
								public Snapshot
{

	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE( Upgrade, "Upgrade" )

public:

	Upgrade( const UpgradeTemplate *upgradeTemplate );
	// virtual destructor prototypes provided by memory pool object

	/// get the upgrade template for this instance
	const UpgradeTemplate *getTemplate() const { return m_template; }

	// status access
	UpgradeStatusType getStatus() const { return m_status; }						///< get status
	void setStatus( UpgradeStatusType status ) { m_status = status; }		///< set the status

	// friend access methods
	void friend_setNext( Upgrade *next ) { m_next = next; }
	void friend_setPrev( Upgrade *prev ) { m_prev = prev; }
	Upgrade *friend_getNext() { return m_next; }
	Upgrade *friend_getPrev() { return m_prev; }

protected:

	// snapshot methods
	virtual void crc( Xfer *xfer ) override;
	virtual void xfer( Xfer *xfer ) override;
	virtual void loadPostProcess() override;

	const UpgradeTemplate *m_template;	///< template this upgrade instance is based on
	UpgradeStatusType m_status;							///< status of upgrade
	Upgrade *m_next;				///< next
	Upgrade *m_prev;				///< prev

};

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
enum UpgradeType CPP_11(: Int)
{
	UPGRADE_TYPE_PLAYER = 0,						// upgrade applies to a player as a whole
	UPGRADE_TYPE_OBJECT,								// upgrade applies to an object instance only

	NUM_UPGRADE_TYPES
};
extern const char *const TheUpgradeTypeNames[]; //Change above, change this!

//-------------------------------------------------------------------------------------------------
/** A single upgrade template definition */
//-------------------------------------------------------------------------------------------------
class UpgradeTemplate : public MemoryPoolObject
{

	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE( UpgradeTemplate, "UpgradeTemplate" )

public:

	UpgradeTemplate();
	// virtual destructor defined by memory pool object

	Int calcTimeToBuild( Player *player ) const;			///< time in logic frames it will take this player to "build" this UpgradeTemplate
	Int calcCostToBuild( Player *player ) const;			///< calc the cost to build this upgrade

	// field access
	void setUpgradeName( const AsciiString& name ) { m_name = name; }
	const AsciiString& getUpgradeName() const { return m_name; }
	/// GeneralsX @fix Claude 05/10/2026 what MSG_QUEUE_UPGRADE / MSG_CANCEL_UPGRADE carry: the name's id, never the NameKey (GXWireId.h)
	UnsignedInt getWireId() const { return gxWireIdForName( m_name.str() ); }
	void setUpgradeNameKey( NameKeyType key ) { m_nameKey = key; }
	NameKeyType getUpgradeNameKey() const { return m_nameKey; }
	const AsciiString& getDisplayNameLabel() const { return m_displayNameLabel; }
	const UpgradeMaskType& getUpgradeMask() const { return m_upgradeMask; }
	UpgradeType getUpgradeType() const { return m_type; }
	const AudioEventRTS* getResearchCompleteSound() const { return &m_researchSound; }
	const AudioEventRTS* getUnitSpecificSound() const { return &m_unitSpecificSound; }
	AcademyClassificationType getAcademyClassificationType() const { return m_academyClassificationType; }

	/// inventory pictures
	void cacheButtonImage();
	const Image* getButtonImage() const { return m_buttonImage; }

	/// INI parsing
	const FieldParse *getFieldParse() const { return m_upgradeFieldParseTable; }

	// friend access methods for the UpgradeCenter ONLY
	void friend_setNext( UpgradeTemplate *next ) { m_next = next; }
	void friend_setPrev( UpgradeTemplate *prev ) { m_prev = prev; }
	UpgradeTemplate *friend_getNext() { return m_next; }
	UpgradeTemplate *friend_getPrev() { return m_prev; }
	const UpgradeTemplate *friend_getNext() const { return m_next; }
	const UpgradeTemplate *friend_getPrev() const { return m_prev; }
	void friend_setUpgradeMask( UpgradeMaskType mask ) { m_upgradeMask = mask; }
	void friend_makeVeterancyUpgrade(VeterancyLevel v);

protected:

	UpgradeType m_type;									///< upgrade type (PLAYER or OBJECT)
	AsciiString m_name;									///< upgrade name
	NameKeyType m_nameKey;							///< name key
	AsciiString m_displayNameLabel;			///< String manager label for UI display name
	Real m_buildTime;										///< database # for how long it takes to "build" this
	Int m_cost;													///< cost for production
	UpgradeMaskType m_upgradeMask;			///< Unique bitmask for this upgrade template
	AudioEventRTS	m_researchSound;			///< Sound played when upgrade researched.
	AudioEventRTS	m_unitSpecificSound;	///< Secondary sound played when upgrade researched.
	AcademyClassificationType m_academyClassificationType; ///< A value used by the academy to evaluate advice based on what players do.

	UpgradeTemplate *m_next;						///< next
	UpgradeTemplate *m_prev;						///< prev

	AsciiString m_buttonImageName;			///< "Queue" images to show in the build queue
	const Image *m_buttonImage;

	/// INI field table
	static const FieldParse m_upgradeFieldParseTable[];		///< the parse table

};

//-------------------------------------------------------------------------------------------------
/** The upgrade center keeps some basic information about the possible upgrades */
//-------------------------------------------------------------------------------------------------
class UpgradeCenter : public SubsystemInterface
{

public:

	UpgradeCenter();
	virtual ~UpgradeCenter() override;

	virtual void init() override;												///< subsystem interface
	virtual void reset() override;												///< subsystem interface
	virtual void update() override { }										///< subsystem interface

	UpgradeTemplate *firstUpgradeTemplate(); ///< return the first upgrade template
	const UpgradeTemplate *findUpgradeByKey( NameKeyType key ) const; ///< find upgrade by name key
	const UpgradeTemplate *findUpgrade( const AsciiString& name ) const; ///< find and return upgrade by name
	const UpgradeTemplate *findUpgrade( const char* name ) const; ///< find and return upgrade by name
	const UpgradeTemplate *findVeterancyUpgrade(VeterancyLevel level) const; ///< find and return upgrade by veterancy level
	/// GeneralsX @fix Claude 05/10/2026 The template a network message names (UpgradeTemplate::getWireId, GXWireId.h), from
	/// the table rebuildWireIds() made at map load: nullptr for an id no template has, or one two templates share.
	const UpgradeTemplate *findUpgradeByWireId( UnsignedInt wireId ) const;
	/// Rebuild that table from every template linked now, a map.ini's inventions included. FALSE when two names share an id.
	Bool rebuildWireIds();

	UpgradeTemplate *newUpgrade( const AsciiString& name );				///< allocate, link, and return new upgrade

	/// GeneralsX @bugfix Claude 25/09/2026 Moves whenever the SET of templates does - newUpgrade(), or
	/// reset() retiring a map's inventions. A pointer cached across calls must be re-resolved when it
	/// moves; UpgradeTemplateRef below does exactly that.
	UnsignedInt getTemplateGeneration() const { return m_templateGeneration; }

	/// does this player have all the necessary things to make this upgrade
	Bool canAffordUpgrade( Player *player, const UpgradeTemplate *upgradeTemplate, Bool displayReason = FALSE ) const;
	std::vector<AsciiString> getUpgradeNames() const;	// For WorldBuilder only!!!

	static void parseUpgradeDefinition( INI *ini );

protected:

	UpgradeTemplate *findNonConstUpgradeByKey( NameKeyType key );		///< find upgrade by name key

	void linkUpgrade( UpgradeTemplate *upgrade );			///< link upgrade to list
	void unlinkUpgrade( UpgradeTemplate *upgrade );		///< remove upgrade from list

	UpgradeTemplate *m_upgradeList;										///< list of all upgrades we can have
	Int m_nextTemplateMaskBit;												///< Each instantiated UpgradeTemplate will be given a Int64 bit as an identifier
	std::map<UnsignedInt, const UpgradeTemplate *> m_wireIds;	///< GeneralsX: wire id -> template, nullptr where two share one (rebuildWireIds)
	Bool buttonImagesCached;

	// GeneralsX @bugfix Claude 25/09/2026 TheSuperHackers/GeneralsGameCode#1749, adapted
	//
	// parseUpgradeDefinition parses a map.ini's Upgrade block INTO the existing template, and reset()
	// restored nothing, so a custom map's upgrade edits - and the upgrades it invented, with their
	// mask bits - outlived the map and reached every later game in the process. A peer that had
	// played that map then mismatched against one that had not.
	//
	// Upstream's PR deletes every template in reset() and re-reads the INI. That frees objects that
	// live raw pointers still name - every startup CommandButton::m_upgradeTemplate, and what were
	// function-local static pointers in AIUpdate / ChinookAIUpdate / WorkerAIUpdate / CommandXlat (now
	// UpgradeTemplateRef) - and stays correct only while the memory pool happens to hand the same
	// addresses back. Snapshot-and-restore instead, the same
	// shape as the PlayerTemplate and OCL stores (TheSuperHackers #1972/#1974): the template OBJECTS
	// never move, a map.ini's edits keep applying for the match exactly as before, and reset() copies
	// the pristine values back. These lists stay empty unless a map actually overrides something.
	std::vector< std::pair<UpgradeTemplate*, UpgradeTemplate*> > m_overrideBackups;	///< (live template, pristine copy) for each template an override load overwrote
	std::vector<UpgradeTemplate*> m_overrideAdded;		///< templates an override load created this match (still linked)
	std::vector<UpgradeTemplate*> m_overrideRetired;	///< created templates reset() unlinked; freed only in the destructor
	UnsignedInt m_templateGeneration;										///< see getTemplateGeneration()

};

// EXTERNALS //////////////////////////////////////////////////////////////////////////////////////
extern UpgradeCenter *TheUpgradeCenter;

//-------------------------------------------------------------------------------------------------
/** GeneralsX @bugfix Claude 25/09/2026 A hard-coded upgrade name, looked up again whenever the set of
	* upgrade templates changes.
	*
	* Replaces `static const UpgradeTemplate *t = TheUpgradeCenter->findUpgrade( "..." );`, which pinned
	* whatever the FIRST call in the process saw. Once reset() retires the upgrades a map.ini invented
	* and hands their mask bits back, such a pin could name a retired template whose bit now belongs to
	* a different upgrade - and which template it named depended on the maps THIS peer had played, so
	* two peers could disagree about it (Codex P2 on the #1749 adaptation). Same for a pin that saw
	* nullptr before a map defined the name. Costs one compare per call once resolved. */
//-------------------------------------------------------------------------------------------------
class UpgradeTemplateRef
{
public:
	explicit UpgradeTemplateRef( const char *name ) : m_name( name ), m_template( nullptr ), m_generation( 0 ) { }

	const UpgradeTemplate *get()
	{
		if( m_generation != TheUpgradeCenter->getTemplateGeneration() )
		{
			m_template = TheUpgradeCenter->findUpgrade( m_name );
			m_generation = TheUpgradeCenter->getTemplateGeneration();
		}
		return m_template;
	}

private:
	const char *m_name;
	const UpgradeTemplate *m_template;
	UnsignedInt m_generation;	///< 0 = never resolved; the center's generation starts at 1
};
