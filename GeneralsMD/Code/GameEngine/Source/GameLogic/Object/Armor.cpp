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

// FILE: ArmorTemplate.cpp ///////////////////////////////////////////////////////////////////////////////
// Author: Steven Johnson, November 2001
// Desc:   ArmorTemplate descriptions
///////////////////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine


#include "Common/INI.h"
#include "Common/ThingFactory.h"
#include "GameLogic/Armor.h"
#include "GameLogic/Damage.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
// PUBLIC DATA ////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////
ArmorStore* TheArmorStore = nullptr;					///< the ArmorTemplate store definition

///////////////////////////////////////////////////////////////////////////////////////////////////
// PRIVATE DATA ///////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////
// PUBLIC FUNCTIONS ///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------------------------------
ArmorTemplate::ArmorTemplate()
{
	clear();
}

//-------------------------------------------------------------------------------------------------
void ArmorTemplate::clear()
{
	for (int i = 0; i < DAMAGE_NUM_TYPES; i++)
	{
		m_damageCoefficient[i] = 1.0f;
	}
}

//-------------------------------------------------------------------------------------------------
Real ArmorTemplate::adjustDamage(DamageType t, Real damage) const
{
	if (t == DAMAGE_UNRESISTABLE)
		return damage;
	if (t == DAMAGE_SUBDUAL_UNRESISTABLE)
		return damage;

	damage *= m_damageCoefficient[t];

	if (damage < 0.0f)
		damage = 0.0f;

	return damage;
}

//-------------------------------------------------------------------------------------------Static
/*static*/ void ArmorTemplate::parseArmorCoefficients( INI* ini, void *instance, void* /* store */, const void* userData )
{
	ArmorTemplate* self = (ArmorTemplate*) instance;

	const char* damageName = ini->getNextToken();
	Real pct = INI::scanPercentToReal(ini->getNextToken());

	if (stricmp(damageName, "Default") == 0)
	{
		for (Int i = 0; i < DAMAGE_NUM_TYPES; i++)
		{
			self->m_damageCoefficient[i] = pct;
		}
		return;
	}

	// GeneralsX @bugfix Claude 24/09/2026 An unknown DamageType name now fails the INI parse (INI_INVALID_DATA) instead of indexing with -1.
	// Upstream reference: xezon, PR #3344
	// https://github.com/TheSuperHackers/GeneralsGameCode/pull/3344
	DamageType dt = (DamageType)INI::scanIndexList(damageName, DamageTypeFlags::getBitNames());
	self->m_damageCoefficient[dt] = pct;
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
ArmorStore::ArmorStore()
{
	m_armorTemplates.clear();
}

//-------------------------------------------------------------------------------------------------
ArmorStore::~ArmorStore()
{
	m_armorTemplates.clear();
}

//-------------------------------------------------------------------------------------------------
const ArmorTemplate* ArmorStore::findArmorTemplate(NameKeyType namekey) const
{
	ArmorTemplateMap::const_iterator it = m_armorTemplates.find(namekey);
	if (it == m_armorTemplates.end())
	{
		return nullptr;
	}
	else
	{
		return &(*it).second;
	}
}

//-------------------------------------------------------------------------------------------------
const ArmorTemplate* ArmorStore::findArmorTemplate(const AsciiString& name) const
{
	return findArmorTemplate(TheNameKeyGenerator->nameToKey(name));
}

//-------------------------------------------------------------------------------------------------
const ArmorTemplate* ArmorStore::findArmorTemplate(const char* name) const
{
	return findArmorTemplate(TheNameKeyGenerator->nameToKey(name));
}

//-------------------------------------------------------------------------------------------------
/*static */ void ArmorStore::parseArmorDefinition(INI *ini)
{
	static const FieldParse myFieldParse[] =
	{
		{ "Armor", ArmorTemplate::parseArmorCoefficients, nullptr, 0 },
		{ nullptr, nullptr, nullptr, 0 }
	};

	const char *c = ini->getNextToken();
	NameKeyType key = TheNameKeyGenerator->nameToKey(c);

	// GeneralsX @bugfix Claude 25/09/2026 TheSuperHackers/GeneralsGameCode#2060, adapted
	// Record what a map.ini is about to overwrite, so reset() can put it back. find() first:
	// operator[] below default-constructs an entry for a new name, which is how a NEW armor is told
	// apart from an OVERRIDDEN one. At most one backup per name per match - a map that overrides the
	// same armor twice must not have its first override recorded as the pristine state.
	if (ini->getLoadType() == INI_LOAD_CREATE_OVERRIDES)
	{
		ArmorTemplateMap::const_iterator existing = TheArmorStore->m_armorTemplates.find(key);
		if (existing == TheArmorStore->m_armorTemplates.end())
			TheArmorStore->m_overrideAdded.push_back(key);
		else if (TheArmorStore->m_overrideBackups.find(key) == TheArmorStore->m_overrideBackups.end())
			TheArmorStore->m_overrideBackups[key] = existing->second;
	}

	ArmorTemplate& armorTmpl = TheArmorStore->m_armorTemplates[key];
	armorTmpl.clear();
	ini->initFromINI(&armorTmpl, myFieldParse);
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @bugfix Claude 25/09/2026 TheSuperHackers/GeneralsGameCode#2060, adapted
// Undo exactly what a map.ini did. Assigning into the existing node keeps every
// ArmorTemplateSet's pointer to it valid.
//
// Erasing the armors a map INVENTED is safe only because the sole holders of an ArmorTemplate
// pointer are ArmorTemplateSets and live objects' Armor: objects are gone (GameEngine::resetSubsystems
// resets TheGameLogic first), and a startup ThingTemplate cannot name an armor that did not exist at
// startup, so only a map.ini's own ThingTemplate overrides can - and SubsystemInterfaceList::resetAll()
// runs in REVERSE registration order, so TheThingFactory (registered after TheArmorStore in
// GameEngine.cpp) has already deleted them. Reordering that registration would dangle them.
void ArmorStore::reset()
{
	for (ArmorTemplateMap::const_iterator b = m_overrideBackups.begin(); b != m_overrideBackups.end(); ++b)
		m_armorTemplates[b->first] = b->second;
	m_overrideBackups.clear();

	for (std::vector<NameKeyType>::const_iterator k = m_overrideAdded.begin(); k != m_overrideAdded.end(); ++k)
		m_armorTemplates.erase(*k);
	m_overrideAdded.clear();
}

//-------------------------------------------------------------------------------------------------
/*static*/ void INI::parseArmorDefinition(INI *ini)
{
	ArmorStore::parseArmorDefinition(ini);
}

