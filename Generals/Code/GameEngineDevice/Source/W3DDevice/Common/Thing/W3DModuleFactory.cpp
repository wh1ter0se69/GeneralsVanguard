/*
**	Command & Conquer Generals(tm)
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

// FILE: W3DModuleFactory.cpp /////////////////////////////////////////////////////////////////////
// Author: Colin Day, April 2001
// Desc:	 W3D specific module
///////////////////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "W3DDevice/Common/W3DModuleFactory.h"
#include "W3DDevice/GameClient/Module/W3DDebrisDraw.h"
#include "W3DDevice/GameClient/Module/W3DDefaultDraw.h"
#include "W3DDevice/GameClient/Module/W3DDependencyModelDraw.h"
#include "W3DDevice/GameClient/Module/W3DModelDraw.h"
#include "W3DDevice/GameClient/Module/W3DLaserDraw.h"
#include "W3DDevice/GameClient/Module/W3DOverlordTankDraw.h"
#include "W3DDevice/GameClient/Module/W3DOverlordTruckDraw.h"
#include "W3DDevice/GameClient/Module/W3DOverlordAircraftDraw.h"
#include "W3DDevice/GameClient/Module/W3DPoliceCarDraw.h"
#include "W3DDevice/GameClient/Module/W3DProjectileStreamDraw.h"
#include "W3DDevice/GameClient/Module/W3DRopeDraw.h"
#include "W3DDevice/GameClient/Module/W3DSupplyDraw.h"
#include "W3DDevice/GameClient/Module/W3DScienceModelDraw.h"
#include "W3DDevice/GameClient/Module/W3DTankDraw.h"
#include "W3DDevice/GameClient/Module/W3DTruckDraw.h"
#include "W3DDevice/GameClient/Module/W3DTankTruckDraw.h"
#include "W3DDevice/GameClient/Module/W3DTracerDraw.h"
#include "W3DDevice/GameClient/Module/W3DTreeDraw.h"
#include "W3DDevice/GameClient/Module/W3DPropDraw.h"

//-------------------------------------------------------------------------------------------------
/** Initialize method */
//-------------------------------------------------------------------------------------------------
void W3DModuleFactory::init()
{

	// extending functionality
	ModuleFactory::init();

	// add the specific module templates we need for the draw methods
	addModule( W3DDefaultDraw );
	addModule( W3DDebrisDraw );
	addModule( W3DModelDraw );
	addModule( W3DLaserDraw );
	addModule( W3DOverlordTankDraw );
	// GeneralsX @feature Claude 14/09/2026 Register the four Zero Hour draw modules in the base
	// Generals module factory, matching TheSuperHackers PR #3073 (upstream d77e26b80) and the
	// GeneralsMD twin, whose init() already registers all four.
	//
	// THIS IS A CONTENT-PARITY BEHAVIOUR CHANGE, NOT A BUGFIX. Nothing in base Generals was
	// broken by their absence. What changes is what the INI parser will accept: a Draw =
	// W3DTreeDraw / W3DPropDraw / W3DOverlordTruckDraw / W3DOverlordAircraftDraw block in
	// Generals data previously missed in ModuleFactory::findModuleTemplate(), which returns
	// nullptr (after a debug-only DEBUG_CRASH), so newModuleDataFromINI()/newModule() built
	// nothing; after this it builds the ZH module. Generals content that never names them is
	// unaffected.
	//
	// Verified in this tree before registering, since a module template must resolve to a class
	// that is actually linked into the Generals binary:
	//  - all four classes live in Core/GameEngineDevice/Include/W3DDevice/GameClient/Module/ and
	//    each declares MAKE_STANDARD_MODULE_MACRO_WITH_MODULE_DATA, which is what addModule() needs;
	//  - their four .cpp files are listed in GAMEENGINEDEVICE_SRC in Core/GameEngineDevice/
	//    CMakeLists.txt, which target_sources() onto the corei_gameenginedevice_private INTERFACE
	//    target that g_gameenginedevice links - so they were already compiled into the Generals
	//    target, merely unreferenced, and the Core Include dir is already on its include path via
	//    corei_gameenginedevice_public;
	//  - their memory pools ("W3DOverlordTruckDraw", "W3DOverlordAircraftDraw", "W3DTreeDraw",
	//    "W3DPropDraw") are already present in
	//    Core/GameEngine/Source/Common/System/GameMemoryInitPools_Generals.inl, so allocating one
	//    at runtime needs no digested change.
	addModule( W3DOverlordTruckDraw );
	addModule( W3DOverlordAircraftDraw );
	addModule( W3DProjectileStreamDraw );
	addModule( W3DPoliceCarDraw );
	addModule( W3DRopeDraw );
	addModule( W3DScienceModelDraw );
	addModule( W3DSupplyDraw );
	addModule( W3DDependencyModelDraw );
	addModule( W3DTankDraw );
	addModule( W3DTruckDraw );
	addModule( W3DTracerDraw );
	addModule( W3DTankTruckDraw );
	// GeneralsX @feature Claude 14/09/2026 Second half of the PR #3073 (d77e26b80) parity
	// registration - see the note above addModule( W3DOverlordTruckDraw ).
	addModule( W3DTreeDraw );
	addModule( W3DPropDraw );

}
