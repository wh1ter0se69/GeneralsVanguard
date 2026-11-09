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

// FILE: UndeadBody.cpp ////////////////////////////////////////////////////////////////////////
// Author: Graham Smallwood, June 2003
// Desc:	 First death is intercepted and sets flags and setMaxHealth.  Second death is handled normally.
///////////////////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine
#include "Common/Xfer.h"
#include "GameLogic/Module/UndeadBody.h"

#include "GameLogic/Object.h"
#include "GameLogic/Module/SlowDeathBehavior.h"

// PUBLIC FUNCTIONS ///////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------------------------------
void UndeadBodyModuleData::buildFieldParse(MultiIniFieldParse& p)
{
  ActiveBodyModuleData::buildFieldParse(p);
	static const FieldParse dataFieldParse[] =
	{
		{ "SecondLifeMaxHealth",			INI::parseReal,	nullptr,		offsetof( UndeadBodyModuleData, m_secondLifeMaxHealth ) },
		{ nullptr, nullptr, nullptr, 0 }
	};
  p.add(dataFieldParse);
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
UndeadBodyModuleData::UndeadBodyModuleData()
{
	m_secondLifeMaxHealth = 1;
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
UndeadBody::UndeadBody( Thing *thing, const ModuleData* moduleData )
						 : ActiveBody( thing, moduleData )
{
	m_isSecondLife = FALSE;
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
UndeadBody::~UndeadBody()
{

}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void UndeadBody::attemptDamage( DamageInfo *damageInfo )
{
	// If we are on our first life, see if this damage will kill us.  If it will, bind it to one hitpoint
	// remaining, then go ahead and take it.
	Bool shouldStartSecondLife = FALSE;

#if RETAIL_COMPATIBLE_CRC || PRESERVE_PREMATURE_BATTLE_BUS_DEATH
	if( damageInfo->in.m_damageType != DAMAGE_UNRESISTABLE
			&& !m_isSecondLife
			&& damageInfo->in.m_amount >= getHealth()
			&& IsHealthDamagingDamage(damageInfo->in.m_damageType)
			)
	{
		damageInfo->in.m_amount = min( damageInfo->in.m_amount, getHealth() - 1 );
		shouldStartSecondLife = TRUE;
	}
#else
	// TheSuperHackers @bugfix Stubbjax 20/09/2025 Battle Buses now correctly apply damage modifiers when calculating lethal damage
	// GeneralsX @bugfix Claude 06/10/2026 ...all of them: judge the damage ActiveBody::attemptDamage will actually DEAL - the
	// raw amount times armor (estimateDamage) times the body's damage scalar (a battle plan's armor bonus), which it multiplies
	// in after armor. Judging without the scalar burned the first life on a hit a bus under Hold the Line would survive. And
	// the clamp of the RAW amount to health - 1 is kept, but where armor or a scalar above 1 would still multiply it to the
	// whole remaining health the bus died before its second life began, so only then is it scaled down further. Never for a hit that deals no scalable damage to this body: healing
	// (ActiveBody heals and returns), an m_kill hit (it removes all health whatever the amount), an indestructible or an
	// already-dead body (it returns before any damage) (sweep c037-01).
	// What TheSuperHackers' arm (before this fix) leaves in the DamageInfo. A caller may hand it on to further victims
	// (FirestormDynamicGeometryInfoUpdate reuses it), so they get exactly this back after the hit, whatever this bus decides.
	const Real handOn = ( damageInfo->in.m_damageType != DAMAGE_UNRESISTABLE
			&& !m_isSecondLife
			&& estimateDamage(damageInfo->in) >= getHealth()
			&& IsHealthDamagingDamage(damageInfo->in.m_damageType) )
		? min( damageInfo->in.m_amount, getHealth() - 1 ) : damageInfo->in.m_amount;
	if( damageInfo->in.m_damageType != DAMAGE_UNRESISTABLE
			&& !m_isSecondLife
			&& IsHealthDamagingDamage(damageInfo->in.m_damageType)
			&& damageInfo->in.m_damageType != DAMAGE_HEALING
			&& !damageInfo->in.m_kill
			&& !isIndestructible()
			&& !getObject()->isEffectivelyDead()
			)
	{
		const Real dealt = estimateDamage(damageInfo->in) * getDamageScalar();
		if( dealt >= getHealth() )
		{
			damageInfo->in.m_amount = min( damageInfo->in.m_amount, getHealth() - 1 );
			const Real clampedDealt = estimateDamage(damageInfo->in) * getDamageScalar();
			if( clampedDealt >= getHealth() && clampedDealt > 0.0f && damageInfo->in.m_amount > 0.0f && getHealth() > 1.0f )
			{
				// armor and the scalar are plain multipliers (Armor.cpp, ActiveBody.cpp), so this lands what is dealt on
				// health - 1; where float rounding would still deal the whole health (a modded health near 2^24) step it
				// down one float at a time - the same arithmetic ActiveBody does, so the check is exact - but at most a few
				// steps, and otherwise keep the base's clamp: modded data (a zero health, a negative multiplier) must never
				// spin the logic tick
				const Real baseClamp = damageInfo->in.m_amount;
				damageInfo->in.m_amount *= ( getHealth() - 1 ) / clampedDealt;
				for( Int step = 0; step < 8 && estimateDamage(damageInfo->in) * getDamageScalar() >= getHealth(); ++step )
					damageInfo->in.m_amount = nextafterf( damageInfo->in.m_amount, 0.0f );
				if( estimateDamage(damageInfo->in) * getDamageScalar() >= getHealth() )
					damageInfo->in.m_amount = baseClamp;
			}
			shouldStartSecondLife = TRUE;
		}
	}
#endif

	ActiveBody::attemptDamage(damageInfo);

#if !(RETAIL_COMPATIBLE_CRC || PRESERVE_PREMATURE_BATTLE_BUS_DEATH)
	// GeneralsX @bugfix Claude 06/10/2026 Further victims of a reused DamageInfo get what the base left there (sweep c037-01).
	damageInfo->in.m_amount = handOn;
#endif

	// After we take it (which allows for damaging special effects), we will do our modifications to the body module
	if( shouldStartSecondLife )
		startSecondLife(damageInfo);
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void UndeadBody::startSecondLife(DamageInfo *damageInfo)
{
	const UndeadBodyModuleData *data = getUndeadBodyModuleData();

	// Flag module as no longer intercepting damage
	m_isSecondLife = TRUE;

	// Modify ActiveBody's max health and initial health
	setMaxHealth(data->m_secondLifeMaxHealth, FULLY_HEAL);

	// Set Armor set flag to use second life armor
	setArmorSetFlag(ARMORSET_SECOND_LIFE);

	// Fire the Slow Death module.  The fact that this is not the result of an onDie will cause the special behavior
	Int total = 0;
	BehaviorModule** update = getObject()->getBehaviorModules();
	for( ; *update; ++update )
	{
		SlowDeathBehaviorInterface* sdu = (*update)->getSlowDeathBehaviorInterface();
		if (sdu != nullptr  && sdu->isDieApplicable(damageInfo) )
		{
			total += sdu->getProbabilityModifier( damageInfo );
		}
	}
	DEBUG_ASSERTCRASH(total > 0, ("Hmm, this is wrong"));


	// this returns a value from 1...total, inclusive
	Int roll = GameLogicRandomValue(1, total);

	for( update = getObject()->getBehaviorModules(); *update; ++update)
	{
		SlowDeathBehaviorInterface* sdu = (*update)->getSlowDeathBehaviorInterface();
		if (sdu != nullptr && sdu->isDieApplicable(damageInfo))
		{
			roll -= sdu->getProbabilityModifier( damageInfo );
			if (roll <= 0)
			{
				sdu->beginSlowDeath(damageInfo);
				return;
			}
		}
	}

}


// ------------------------------------------------------------------------------------------------
/** CRC */
// ------------------------------------------------------------------------------------------------
void UndeadBody::crc( Xfer *xfer )
{

	// extend base class
	ActiveBody::crc( xfer );

}

// ------------------------------------------------------------------------------------------------
/** Xfer method
	* Version Info:
	* 1: Initial version */
// ------------------------------------------------------------------------------------------------
void UndeadBody::xfer( Xfer *xfer )
{

	// version
	XferVersion currentVersion = 1;
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );

	// extend base class
	ActiveBody::xfer( xfer );

	xfer->xferBool(&m_isSecondLife);

}

// ------------------------------------------------------------------------------------------------
/** Load post process */
// ------------------------------------------------------------------------------------------------
void UndeadBody::loadPostProcess()
{

	// extend base class
	ActiveBody::loadPostProcess();

}
