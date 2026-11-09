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

// FILE: FXListDie.cpp ///////////////////////////////////////////////////////////////////////////
// Author: Steven Johnson, Jan 2002
// Desc:   Simple Die module
///////////////////////////////////////////////////////////////////////////////////////////////////


// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/INI.h"
#include "Common/Player.h"
#include "Common/ThingTemplate.h"
#include "Common/Xfer.h"
#include "GameClient/FXList.h"
#include "GameLogic/Damage.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/Module/FXListDie.h"
#include "GameLogic/Module/AIUpdate.h"

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
FXListDie::FXListDie( Thing *thing, const ModuleData* moduleData ) : DieModule( thing, moduleData )
{
	if( getFXListDieModuleData()->m_initiallyActive )
	{
		giveSelfUpgrade();
	}
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
FXListDie::~FXListDie()
{

}

//-------------------------------------------------------------------------------------------------
/** The die callback. */
//-------------------------------------------------------------------------------------------------
void FXListDie::onDie( const DamageInfo *damageInfo )
{
	if (!isUpgradeActive())
		return;
	if (!isDieApplicable(damageInfo))
		return;
	const FXListDieModuleData* d = getFXListDieModuleData();

	UpgradeMaskType activation, conflicting;
	getUpgradeActivationMasks( activation, conflicting );
	Object *obj = getObject();
	if( obj->getObjectCompletedUpgradeMask().testForAny( conflicting ) )
	{
		return;
	}
	if( obj->getControllingPlayer() && obj->getControllingPlayer()->getCompletedUpgradeMask().testForAny( conflicting ) )
	{
		return;
	}

	if (d->m_defaultDeathFX)
	{
		if (d->m_orientToObject)
		{
			Object *damageDealer = TheGameLogic->findObjectByID( damageInfo->in.m_sourceID );
			FXList::doFXObj(getFXListDieModuleData()->m_defaultDeathFX, getObject(), damageDealer);
		}
		else
		{
			FXList::doFXPos(getFXListDieModuleData()->m_defaultDeathFX, getObject()->getPosition());
		}
	}
}

// ------------------------------------------------------------------------------------------------
/** CRC */
// ------------------------------------------------------------------------------------------------
void FXListDie::crc( Xfer *xfer )
{

	// extend base class
	DieModule::crc( xfer );

	//
	// GeneralsX @bugfix Claude 25/07/2026 This module mixes in UpgradeMux but never folded the mux's
	// m_upgradeExecuted flag into the logic CRC, so a peer whose death-FX activation state had
	// diverged looked identical to everyone else until the object actually died - the desync was
	// only observable as one player seeing an explosion the others did not. Every other UpgradeMux
	// host (FireWeaponWhenDeadBehavior, AutoHealBehavior, UpgradeModule, ...) chains here.
	// Gated: the frame CRC stream is byte-compatible with retail Zero Hour 1.04 and nearly every
	// object template carries an FXListDie, so adding bytes here would invalidate every retail
	// replay. See the policy note at the top of Common/GameDefines.h.
	//
#if !RETAIL_COMPATIBLE_CRC
	// extend upgrade mux
	UpgradeMux::upgradeMuxCRC( xfer );
#endif

}

// ------------------------------------------------------------------------------------------------
/** Xfer method
	* Version Info:
	* 1: Initial version
	* 2: GeneralsX @bugfix Claude 25/07/2026 Serialize the UpgradeMux state */
// ------------------------------------------------------------------------------------------------
void FXListDie::xfer( Xfer *xfer )
{

	//
	// GeneralsX @bugfix Claude 25/07/2026 UpgradeMux::m_upgradeExecuted was never written to the save,
	// and nothing re-derives it on load (Object::updateUpgradeModules only runs when an upgrade
	// completes). A module declared "StartsActive = No" plus "TriggeredBy" that had since been
	// switched on therefore came back from a save with the flag clear, so the isUpgradeActive() test
	// at the top of onDie() silently swallowed the death FX for the rest of the game even though the
	// player still owned the triggering upgrade. Version 2 stores the flag.
	//
	// The bump is gated on RETAIL_COMPATIBLE_XFER_SAVE because Object::xfer reads a module's block
	// size but never seeks past it, so appending bytes under version 1 would make the module over-run
	// its block and corrupt every following module in a retail-format save. Reading stays conditional
	// on the stored version so version 1 saves keep loading in either configuration.
	//
#if RETAIL_COMPATIBLE_XFER_SAVE
	// version
	XferVersion currentVersion = 1;
#else
	// version
	XferVersion currentVersion = 2;
#endif
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );

	// extend base class
	DieModule::xfer( xfer );

	// extend upgrade mux
	if( version >= 2 )
	{
		UpgradeMux::upgradeMuxXfer( xfer );
	}

}

// ------------------------------------------------------------------------------------------------
/** Load post process */
// ------------------------------------------------------------------------------------------------
void FXListDie::loadPostProcess()
{

	// extend base class
	DieModule::loadPostProcess();

	//
	// GeneralsX @bugfix Claude 25/07/2026 The UpgradeMux half of this module was never post-processed,
	// so any fixup added to the shared mux would silently skip FXListDie the way the xfer above did.
	// The base is empty today, so this costs nothing and only restores consistency with the other
	// UpgradeMux hosts.
	//
	UpgradeMux::upgradeMuxLoadPostProcess();

}
