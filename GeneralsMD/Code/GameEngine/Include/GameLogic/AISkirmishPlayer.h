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

// AISkirmishPlayer.h
// Computerized opponent
// Author: Michael S. Booth, January 2002

#pragma once

#include "Common/GameMemory.h"
#include "GameLogic/AIPlayer.h"

class BuildListInfo;
class SpecialPowerTemplate;
class GXRuleBank;


/**
 * The computer-controlled opponent.
 */
class AISkirmishPlayer : public AIPlayer
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE( AISkirmishPlayer, "AISkirmishPlayer"  )

public:	 // AISkirmish specific methods.

	AISkirmishPlayer( Player *p );							///< constructor
	virtual Bool computeSuperweaponTarget(const SpecialPowerTemplate *power, Coord3D *pos, Int playerNdx, Real weaponRadius) override; ///< Calculates best pos for weapon given radius.

public:	// AIPlayer interface methods.

	virtual void update() override;											///< simulates the behavior of a player

	virtual void newMap() override;											///< New map loaded call.

	/// Invoked when a unit I am training comes into existence
	virtual void onUnitProduced( Object *factory, Object *unit ) override;

	virtual void buildSpecificAITeam(TeamPrototype *teamProto, Bool priorityBuild) override; ///< Builds this team immediately.

	virtual void buildSpecificAIBuilding(const AsciiString &thingName) override; ///< Builds this building as soon as possible.

	virtual void buildAIBaseDefense(Bool flank) override; ///< Builds base defense on front or flank of base.

	virtual void buildAIBaseDefenseStructure(const AsciiString &thingName, Bool flank) override; ///< Builds base defense on front or flank of base.

	virtual void recruitSpecificAITeam(TeamPrototype *teamProto, Real recruitRadius) override; ///< Builds this team immediately.

	virtual Bool isSkirmishAI() override {return true;}
	virtual Bool gxFrontPoint(Coord3D *out) override;	///< GeneralsX 22/10/2026 (ai-wh1ter0se-econ R): the base front front defences use
	void gxTunnelAirWatch();	///< GeneralsX 22/10/2026 (ai-wh1ter0se-econ T; ai-pro-rules-2 T2): incoming aircraft -> assigned tunnel guards
	void gxAirRows(Int from, UnsignedInt now, Int me);	///< GeneralsX 28/10/2026 T2 r15: the watch's row loop, from row <from> on
	void gxAirTestNoAir(UnsignedInt now);	///< GeneralsX 27/10/2026 (T2, test-instruments builds): GX_TAA_TEST_NOAIR
	void gxAirLifetime(TeamPrototype *proto, UnsignedInt now);	///< GeneralsX 27/10/2026 (ai-pro-rules-2 T2): condition C + the team release
	virtual Bool gxFrontPointFixed() override;	///< GeneralsX 28/10/2026: gxFrontPoint's answer cannot change (AIPlayer.cpp gxBuildAngles)
	virtual Bool gxFrontDoors() override;	///< GeneralsX 04/11/2026 base-layout: this side's list carries GXFrontDoors
	virtual const Coord3D &gxSupplyRef() const override { return m_gxStockCenterSet ? m_gxStockCenter : m_baseCenter; }	///< GeneralsX 06/11/2026 base-layout (ADDENDUM_9)
	virtual GXRuleBank *gxRuleBank() override { return m_gxRules; }	///< GeneralsX 30/10/2026 rb-core: the AI rule bank

	virtual Bool checkBridges(Object *unit, Waypoint *way) override;

	virtual Player *getAiEnemy() override;	///< Solo AI attacks based on scripting.  Only skirmish auto-acquires an enemy at this point.  jba.

protected:

	// snapshot methods
	virtual void crc( Xfer *xfer ) override;
	virtual void xfer( Xfer *xfer ) override;
	virtual void loadPostProcess() override;

	virtual void doBaseBuilding() override;
	virtual void checkReadyTeams() override;
	virtual void checkQueuedTeams() override;
	virtual void doTeamBuilding() override;
	virtual Object *findDozer(const Coord3D *pos) override;
	virtual void queueDozer() override;

protected:

	virtual Bool selectTeamToBuild() override;			///< determine the next team to build
	virtual Bool selectTeamToReinforce( Int minPriority ) override;			///< determine the next team to reinforce
	virtual Bool startTraining( WorkOrder *order, Bool busyOK, AsciiString teamName) override;	///< find a production building that can handle the order, and start building

	virtual Bool isAGoodIdeaToBuildTeam( TeamPrototype *proto ) override;		///< return true if team should be built
	virtual void processBaseBuilding() override;		///< do base-building behaviors
	virtual void processTeamBuilding() override;		///< do team-building behaviors

protected:
	void adjustBuildList(BuildListInfo *list);
	Waypoint *gxFrontWaypoint();	///< GeneralsX 28/10/2026: gxFrontPoint's Center-path waypoint (nullptr: none)
	const class AISideBuildList *gxSideList() const;	///< GeneralsX 04/11/2026 base-layout: this side's list in the AI data (nullptr: none)
	Bool gxBaseFrontFrom(const Coord3D &from, Coord3D *out);	///< GeneralsX 04/11/2026 base-layout: the keyed base front seen from a point; TRUE when it is a Center waypoint
	Bool gxStartPoint(Coord3D *out);	///< GeneralsX 04/11/2026 base-layout: this player's start waypoint, which nothing moves (FALSE: none)
	void gxComputeStockCenter();	///< GeneralsX 06/11/2026 base-layout (ADDENDUM_9): m_gxStockCenter from the placed stock spots (newMap)
	Int getMyEnemyPlayerIndex();
	void acquireEnemy();

protected:
	Int m_curFrontBaseDefense; // First is 0.
	Int m_curFlankBaseDefense; // First is 0.
	Real m_curFrontLeftDefenseAngle;
	Real m_curFrontRightDefenseAngle;
	Real m_curLeftFlankLeftDefenseAngle;
	Real m_curLeftFlankRightDefenseAngle;
	Real m_curRightFlankLeftDefenseAngle;
	Real m_curRightFlankRightDefenseAngle;

	UnsignedInt m_frameToCheckEnemy;
	std::map<const TeamPrototype*, Bool> m_hoardAttackTeams;	///< GeneralsX: aiTeamAttacks per team, filled on first use; pure, so not saved
	UnsignedInt m_frameEnemyReviewed;	///< GeneralsX: when the enemy was last chosen or re-scored; like m_frameToCheckEnemy, not saved
	Player			*m_currentEnemy;
	GXRuleBank		*m_gxRules;	///< GeneralsX 30/10/2026 rb-core: the AI rule bank (GXRules/GXRuleBank.h); saved in xfer's own block
	// GeneralsX 06/11/2026 base-layout: a keyed list's STOCK base centre, the centroid of the stock spots its
	// entries fall back to, placed as retail places the stock list: exactly the m_baseCenter the stock list gives. Set only when every entry carries
	// a layout fallback (GXTurnLayout lists from make-aidata-layouts.py); FALSE for every other list, which then ranks from m_baseCenter as before.
	Coord3D			m_gxStockCenter;
	Bool				m_gxStockCenterSet;

};
