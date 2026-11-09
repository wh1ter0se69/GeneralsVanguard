/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
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
// GeneralsX @feature Claude 28/09/2026 The superweapon missiles' visual flight: the engine half. See MissileVisual.h
// for the contract and MissileVisualMath.h for the flight itself.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/MessageStream.h"
#include "Common/NameKeyGenerator.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/GameUtility.h"
#include "Common/SpecialPower.h"
#include "Common/ThingTemplate.h"
#include "Common/XferSave.h"
#include "GameClient/ControlBar.h"
#include "GameClient/Display.h"
#include "GameClient/Drawable.h"
#include "GameClient/GameClient.h"
#include "GameClient/GXNoShroud.h"
#include "GameClient/ParticleSys.h"
#include "GameClient/Shadow.h"
#include "Common/AudioEventRTS.h"
#include "Common/AudioHandleSpecialValues.h"
#include "Common/GameAudio.h"
#include "GameClient/MissileVisual.h"
#include "GameClient/MissileVisualMath.h"
#include "GameClient/View.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Locomotor.h"
#include "GameLogic/Object.h"
#include "GameLogic/PartitionManager.h"
#include "GameLogic/TerrainLogic.h"
#include "GameLogic/Weapon.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/BodyModule.h"
#include "GameLogic/Module/HeightDieUpdate.h"
#include "GameLogic/Module/MissileAIUpdate.h"
#include "GameLogic/Module/NeutronMissileSlowDeathUpdate.h"
#include "GameLogic/Module/NeutronMissileUpdate.h"
#include "GameLogic/Module/PhysicsUpdate.h"
#include "GameLogic/Module/SpecialPowerCompletionDie.h"
#include "GameLogic/Module/SpecialPowerModule.h"

#include <algorithm>
#include <vector>

namespace
{
	// Vertical scale of the flight: the ground range, clamped. Shorter shots are lofted to HMIN, longer ones flattened
	// to HMAX so the apogee (0.6 of it for the nuke, 0.4 for the SCUD) stays inside what the camera draws.
	const float MV_HMIN = 1200.0f;
	const float MV_HMAX = 5000.0f;
	// The terminal blend reaches 1 this far above the height the logic dies at, so a few units of terrain the estimate
	// did not see cannot leave the drawn missile off the logic one on the frame it detonates.
	const float MV_DIE_MARGIN = 20.0f;
	// SCUD: blend onto the logic over this last distance to its end point, complete MV_SCUD_BLEND_NEAR from it. The
	// margin matters: inside LockDistance/2 of a ground goal MissileAIUpdate brakes straight onto the goal itself
	// (doKillState), 15 below E, so a blend that reached 1 only AT E would leave it 0.7% short on the detonation frame.
	const float MV_SCUD_BLEND_DIST = 300.0f;
	const float MV_SCUD_BLEND_NEAR = 60.0f;
	// SCUD: it blends in over this height above its local die height (it cruises ~225 above the ground, so level flight
	// over open ground stays clear of the band).
	const float MV_SCUD_BLEND_BAND = 200.0f;
	// Both: blend onto the logic as another object closes in on the logic missile's bounding sphere (a collision detonates
	// it where the logic is): complete two frames of the approach before contact, started at least this far and this many
	// frames of the approach before that (mvProxBlend). Small on purpose - a near miss pulls the drawn missile, briefly.
	const float MV_PROX_BAND = 80.0f;
	const float MV_PROX_RAMP_FRAMES = 12.0f;
	// The fastest other object the query looks out for, in units per logic frame (1200/s at 60 Hz: past any aircraft).
	const float MV_PROX_OTHER_SPEED = 20.0f;
	// A SCUD whose goal moves further than this (jammed: MissileAIUpdate::projectileNowJammed scatters it) stops being
	// drawn away from the logic; it is pulled back over MV_ABANDON_FRAMES and then drawn at the logic transform.
	const float MV_GOAL_MOVED = 60.0f;
	const Int MV_ABANDON_FRAMES = LOGICFRAMES_PER_SECOND / 2;
}

enum MissileVisualKind { MV_KIND_NUKE, MV_KIND_SCUD };

struct MissileVisual
{
	MissileVisualKind kind;
	const MVTable *table;
	const WeaponTemplate *weapon;			///< SCUD: the weapon that fired it (its collision filter), once found
	MVVec L, E, V, prevV;
	MVVec lastP;							///< logic position at the last evaluation (its velocity, for the proximity lead)
	Bool haveLastP;
	Bool descended;						///< the logic missile has moved down at least once (HeightDieUpdate can kill it)
	Vector3 dir;
	float u;
	float teOffset;						///< SCUD: logic frame of launch (the nuke reads its own)
	float maxClimb;						///< SCUD: highest it has climbed above L (the climb is done once, never again)
	UnsignedInt evalFrame;		///< logic frame of the cached evaluation
	float lastTau;				///< the last remaining-flight estimate (logic frames): bounds the pull back onto the logic path
	Bool shocked;				///< the detonation shockwave has been started for this missile
	Int abandonLeft;					///< > 0: being pulled back onto the logic
	Bool started, havePrev, active, retired;
	Matrix3D xfrm;
};

// ------------------------------------------------------------------------------------------------------------------
// GX_MISSILEVIS unset: drawn along the visual flight, silent. "1": the same, with the [GXMVIS] trace. "0": the flight
// is still computed and traced but NOT drawn - the missile keeps the logic look, as before this feature. The "0" arm is
// the before/after control: same binary, same capture camera, one variable.
static Int missileVisualMode()
{
	static Int s_mode = -1;
	if( s_mode < 0 )
	{
		const char *env = getenv( "GX_MISSILEVIS" );
		s_mode = env == nullptr ? 2 : (env[0] == '1' ? 1 : (env[0] == '0' ? 0 : 2));
	}
	return s_mode;
}

Bool missileVisualTraceOn()
{
	return missileVisualMode() != 2;
}

static Bool missileVisualDrawOn()
{
	return missileVisualMode() != 0;
}

static const MVTable *profileTable( MissileVisualKind kind )
{
	static MVTable s_nuke, s_scud;
	static Bool s_built = FALSE;
	if( !s_built )
	{
		mvBuildTable( mvProfileNuke(), &s_nuke );
		mvBuildTable( mvProfileScud(), &s_scud );
		s_built = TRUE;
	}
	const MVTable *t = kind == MV_KIND_NUKE ? &s_nuke : &s_scud;
	return t->valid ? t : nullptr;
}

static const ModuleData *templateModule( const ThingTemplate *tmpl, const char *name )
{
	const ModuleInfo &mi = tmpl->getBehaviorModuleInfo();
	for( Int i = 0; i < mi.getCount(); ++i )
		if( mi.getNthName( i ).compareNoCase( name ) == 0 )
			return mi.getNthData( i );
	return nullptr;
}

/// The height `obj` dies at where it is NOW - HeightDieUpdate::update's own threshold, mirrored: terrain (or the highest
/// bridge layer) plus TargetHeight, or the tallest structure in its bounding circle when that is higher and the data says
/// structures count. Read-only partition query, the kind the in-game UI makes every frame.
static Real dieHeightHere( const Object *obj, const HeightDieUpdateModuleData *hd )
{
	const Coord3D *pos = obj->getPosition();
	Real ground = TheTerrainLogic->getGroundHeight( pos->x, pos->y );
	const Real above = hd != nullptr ? hd->m_targetHeightAboveTerrain : 0.0f;
	if( hd == nullptr || !hd->m_targetHeightIncludesStructures )
		return ground + above;
	const PathfindLayerEnum layer = TheTerrainLogic->getHighestLayerForDestination( pos );
	if( layer != LAYER_GROUND )
	{
		const Real layerHeight = TheTerrainLogic->getLayerHeight( pos->x, pos->y, layer );
		if( layerHeight > ground )
			ground = layerHeight;
	}
	PartitionFilterAcceptByKindOf filter1( MAKE_KINDOF_MASK( KINDOF_STRUCTURE ), KINDOFMASK_NONE );
	PartitionFilter *filters[] = { &filter1, nullptr };
	ObjectIterator *iter = ThePartitionManager->iterateObjectsInRange( obj, obj->getGeometryInfo().getBoundingCircleRadius(),
		FROM_BOUNDINGSPHERE_3D, filters );
	MemoryPoolObjectHolder hold( iter );
	Real tallest = 0.0f;
	for( Object *o = iter->first(); o != nullptr; o = iter->next() )
	{
		if( o == obj )
			continue;
		const Real h = o->getGeometryInfo().getMaxHeightAbovePosition();
		if( h > tallest )
			tallest = h;
	}
	return ground + (tallest > above ? tallest : above);
}

/// The weapon that fired `obj`: its launcher's weapon whose projectile is this missile. It is the detonation weapon
/// MissileAIUpdate keeps privately (WeaponTemplate::fireWeaponTemplate passes itself), so its collision filter is the
/// SCUD's. Null when the launcher is gone or no longer carries it; read once, at first sight, and kept.
static const WeaponTemplate *firingWeapon( const Object *obj, const Object *launcher )
{
	if( launcher == nullptr )
		return nullptr;
	for( Int s = PRIMARY_WEAPON; s < WEAPONSLOT_COUNT; ++s )
	{
		const Weapon *w = launcher->getWeaponInWeaponSlot( (WeaponSlotType)s );
		const WeaponTemplate *wt = w != nullptr ? w->getTemplate() : nullptr;
		if( wt != nullptr && wt->getProjectileTemplate() != nullptr && wt->getProjectileTemplate()->isEquivalentTo( obj->getTemplate() ) )
			return wt;
	}
	return nullptr;
}

/// Blend onto the logic as something that can detonate the missile closes in on the LOGIC missile's bounding sphere:
/// mvProxBlend of the gap at the rate it is closing, 1 two frames of that approach before contact (Codex missile-r14:
/// a contact that lands between two frames still finds the drawn missile on the logic one, where its explosion is).
/// A collision detonates it where the logic is (NeutronMissileUpdate and MissileAIUpdate::projectileHandleCollision,
/// Codex missile-r3), and a collision cannot be predicted, only approached. Neither missile dies of damage in practice
/// (NeutronMissile MaxHealth 99999999, ScudStormMissile 10000 behind ProjectileArmor). Only what the logic would let through (Codex missile-r11): nothing while the warhead is unarmed or either
/// side is OBJECT_STATUS_NO_COLLISIONS (PartitionContactList::processContactList), never its launcher, and for a SCUD only
/// what its weapon collides with (WeaponTemplate::shouldProjectileCollideWith) - so the other missiles of its salvo do
/// not pull it off its arc. The SCUD Storm fires at a location, so there is no intended victim (INVALID_ID). A SCUD whose
/// weapon was never found (launcher gone before first sight) falls back to every object. Read-only partition query and
/// template reads.
static float proximityBlend( const Object *obj, MissileVisual *mv, float ignoreTopsBelow = -1.0e9f )
{
	const ProjectileUpdateInterface *pui = obj->getProjectileUpdateInterface();
	if( pui == nullptr || !pui->projectileIsArmed() || obj->getStatusBits().test( OBJECT_STATUS_NO_COLLISIONS ) )
		return 0.0f;
	const ObjectID launcherID = pui->projectileGetLauncherID();
	const Object *launcher = TheGameLogic->findObjectByID( launcherID );
	if( mv->kind == MV_KIND_SCUD && mv->weapon == nullptr )
		mv->weapon = firingWeapon( obj, launcher );
	const WeaponTemplate *wt = mv->weapon;
	// The missile's own logic velocity per frame, from its last two positions (the nuke has no PhysicsBehavior).
	const Coord3D *P = obj->getPosition();
	const MVVec vm = mv->haveLastP ? mvMake( P->x - mv->lastP.x, P->y - mv->lastP.y, P->z - mv->lastP.z ) : mvMake( 0.0f, 0.0f, 0.0f );
	const Real reach = mvProxReach( mvLen( vm ) + MV_PROX_OTHER_SPEED, MV_PROX_BAND, MV_PROX_RAMP_FRAMES );
	ObjectIterator *iter = ThePartitionManager->iterateObjectsInRange( obj, reach, FROM_BOUNDINGSPHERE_3D, nullptr );
	MemoryPoolObjectHolder hold( iter );
	float blend = 0.0f;
	for( Object *o = iter->first(); o != nullptr; o = iter->next() )
	{
		if( o == obj || o->getID() == launcherID || o->isEffectivelyDead() || o->getStatusBits().test( OBJECT_STATUS_NO_COLLISIONS ) )
			continue;
		if( wt != nullptr && !wt->shouldProjectileCollideWith( launcher, obj, o, INVALID_ID ) )
			continue;
		// Something whose top is below the height the missile dies at cannot be touched before it dies (the nuke, dying by
		// height over its target, passes the target's own command center and the buildings around it without touching them):
		// no pull onto the logic path for those (wh1ter0se's look #1, the landing).
		if( o->getPosition()->z + o->getGeometryInfo().getMaxHeightAbovePosition() < ignoreTopsBelow )
			continue;
		const Real gap = sqrtf( ThePartitionManager->getDistanceSquared( obj, o, FROM_BOUNDINGSPHERE_3D ) );
		// The gap closes at the relative velocity along missile -> other (0 for anything moving away).
		const Coord3D *Q = o->getPosition();
		const MVVec u = mvMake( Q->x - P->x, Q->y - P->y, Q->z - P->z );
		const float ul = mvLen( u );
		MVVec vo = mvMake( 0.0f, 0.0f, 0.0f );
		if( const PhysicsBehavior *ph = o->getPhysics() )
			vo = mvMake( ph->getVelocity()->x, ph->getVelocity()->y, ph->getVelocity()->z );
		const float closing = ul > 0.001f ? ((vm.x - vo.x) * u.x + (vm.y - vo.y) * u.y + (vm.z - vo.z) * u.z) / ul : 0.0f;
		blend = std::max( blend, mvProxBlend( gap, closing, MV_PROX_BAND, MV_PROX_RAMP_FRAMES ) );
	}
	return blend;
}

/// Only the superweapon missiles: the projectile's SpecialPowerCompletionDie names the power that fired it. The same
/// modules fly the (flat) cruise missile and several ordinary missiles, which must keep their logic look.
static Bool isSuperweaponMissile( const ThingTemplate *tmpl, MissileVisualKind kind )
{
	const SpecialPowerCompletionDieModuleData *d =
		static_cast<const SpecialPowerCompletionDieModuleData *>( templateModule( tmpl, "SpecialPowerCompletionDie" ) );
	if( d == nullptr || d->m_specialPowerTemplate == nullptr )
		return FALSE;
	const SpecialPowerType t = d->m_specialPowerTemplate->getSpecialPowerType();
	if( kind == MV_KIND_NUKE )
		return t == SPECIAL_NEUTRON_MISSILE || t == NUKE_SPECIAL_NEUTRON_MISSILE || t == SUPW_SPECIAL_NEUTRON_MISSILE;
	return t == SPECIAL_SCUD_STORM;
}

// ------------------------------------------------------------------------------------------------------------------
// The nuke keeps its target private and publishes no accessor, and adding one would move the SimID digest. Its own
// save path writes every field we need, so run it: XferSave is the Snapshot friend that calls xfer() in XFER_SAVE mode,
// which only READS the module (NeutronMissileUpdate::xfer, UpdateModule/BehaviorModule/ObjectModule/Module::xfer and
// RadiusDecal::xferRadiusDecal change state only under XFER_LOAD). Nothing is written anywhere: every sink is a no-op
// and the typed values are recorded in call order. NeutronMissileUpdate::xfer (version 1) writes, in order:
//   Coord3D target, intermed, accel, vel | Bool isLaunched, isArmed, reachedIntermediatePos | Real noTurnDistLeft,
//   heightAtLaunch | UnsignedInt nextCallFrameAndPhase (base), stateTimestamp, frameAtLaunch | ObjectID launcher.
// The counts are checked exactly and intermed == target + TargetFromDirectlyAbove is checked against the INI, so a
// change to that function is caught here (the missile then keeps its logic look) instead of being misread.
class NukeStateReader : public XferSave
{
public:
	enum { MAX_ITEMS = 8 };
	Coord3D coords[MAX_ITEMS];
	Bool bools[MAX_ITEMS];
	Real reals[MAX_ITEMS];
	UnsignedInt uints[MAX_ITEMS];
	ObjectID ids[MAX_ITEMS];
	Int nCoords, nBools, nReals, nUints, nIds;

	NukeStateReader() : nCoords( 0 ), nBools( 0 ), nReals( 0 ), nUints( 0 ), nIds( 0 ) {}

	void read( Snapshot *s ) { xferSnapshot( s ); }

	virtual void open( AsciiString ) override {}
	virtual void close() override {}
	virtual Int beginBlock() override { return 0; }
	virtual void endBlock() override {}
	virtual void skip( Int ) override {}
	virtual void xferAsciiString( AsciiString * ) override {}
	virtual void xferUnicodeString( UnicodeString * ) override {}
	virtual void xferCoord3D( Coord3D *c ) override { if( nCoords < MAX_ITEMS ) coords[nCoords] = *c; ++nCoords; }
	virtual void xferBool( Bool *b ) override { if( nBools < MAX_ITEMS ) bools[nBools] = *b; ++nBools; }
	virtual void xferReal( Real *r ) override { if( nReals < MAX_ITEMS ) reals[nReals] = *r; ++nReals; }
	virtual void xferUnsignedInt( UnsignedInt *u ) override { if( nUints < MAX_ITEMS ) uints[nUints] = *u; ++nUints; }
	virtual void xferObjectID( ObjectID *id ) override { if( nIds < MAX_ITEMS ) ids[nIds] = *id; ++nIds; }

protected:
	virtual void xferImplementation( void *, Int ) override {}
};

struct NukeLogicState
{
	Coord3D target, intermed;
	Bool launched, reachedIntermed;
	Real noTurnDistLeft, heightAtLaunch;
	UnsignedInt frameAtLaunch;
	ObjectID launcher;
};

static Bool readNukeState( Object *obj, const NeutronMissileUpdateModuleData *md, NukeLogicState *out )
{
	static const NameKeyType s_key = NAMEKEY( "NeutronMissileUpdate" );
	UpdateModule *m = obj->findUpdateModule( s_key );
	if( m == nullptr )
		return FALSE;
	NukeStateReader r;
	r.read( m );
	if( r.nCoords != 4 || r.nBools != 3 || r.nReals != 2 || r.nUints != 3 || r.nIds != 1 )
		return FALSE;
	out->target = r.coords[0];
	out->intermed = r.coords[1];
	out->launched = r.bools[0];
	out->reachedIntermed = r.bools[2];
	out->noTurnDistLeft = r.reals[0];
	out->heightAtLaunch = r.reals[1];
	out->frameAtLaunch = r.uints[2];
	out->launcher = r.ids[0];
	return out->intermed.x == out->target.x && out->intermed.y == out->target.y
		&& fabsf( out->intermed.z - (out->target.z + md->m_targetFromDirectlyAbove) ) < 0.01f;
}

// ------------------------------------------------------------------------------------------------------------------
static void trace( const Drawable *draw, const MissileVisual *mv, const char *what, UnsignedInt now, float te, float tau,
	float blend, const Coord3D *P )
{
	if( !missileVisualTraceOn() )
		return;
	fprintf( stderr, "[GXMVIS] f=%u id=%u %s %s te=%.0f tau=%.1f u=%.4f b=%.3f P=(%.1f,%.1f,%.1f) V=(%.1f,%.1f,%.1f) L=(%.1f,%.1f,%.1f) E=(%.1f,%.1f,%.1f)\n",
		(unsigned)now, (unsigned)draw->getID(), mv->kind == MV_KIND_NUKE ? "nuke" : "scud", what, te, tau, mv->u, blend,
		P->x, P->y, P->z, mv->V.x, mv->V.y, mv->V.z, mv->L.x, mv->L.y, mv->L.z, mv->E.x, mv->E.y, mv->E.z );
	fflush( stderr );
}

/// Whether HeightDieUpdate could kill the missile by height now: never without one (mod data, Codex visuals-3 r8); always,
/// unless its data says OnlyWhenMovingDown - then
/// from the first logic frame it moved down (latched: the kill needs only one down frame, and a per-frame flag would
/// flicker the blend on a level flight). Both missiles launch climbing, so neither is pulled onto the logic on the pad.
static Bool canDieLow( const MissileVisual *mv, const HeightDieUpdateModuleData *hd )
{
	return hd != nullptr && (!hd->m_onlyWhenMovingDown || mv->descended);
}

/// One logic frame of the nuke. Returns FALSE when the missile must be drawn at its logic transform from now on.
static Bool stepNuke( MissileVisual *mv, Drawable *draw, Object *obj, UnsignedInt now )
{
	const NeutronMissileUpdateModuleData *md =
		static_cast<const NeutronMissileUpdateModuleData *>( templateModule( obj->getTemplate(), "NeutronMissileUpdate" ) );
	NukeLogicState s;
	if( md == nullptr || md->m_forwardDamping <= 0.0f || !readNukeState( obj, md, &s ) )
		return FALSE;
	if( !s.launched )
		return TRUE;			// still in the silo, hidden; nothing to draw yet
	const HeightDieUpdateModuleData *hd =
		static_cast<const HeightDieUpdateModuleData *>( templateModule( obj->getTemplate(), "HeightDieUpdate" ) );
	const Coord3D *P = obj->getPosition();
	// The client runs after the logic frame counter moved on, so the frame the logic launched on is now - 1.
	const float te = now > s.frameAtLaunch ? (float)(now - 1 - s.frameAtLaunch) : 0.0f;

	MVNukeFlight fl;
	// RelativeSpeed and ForwardDamping are authored per 30 Hz frame, and GX_LOGIC_60HZ scales them (x1/4, x1/2) to keep
	// the terminal speed s/d the same per SECOND; per logic frame that is s/d * 30 / LOGICFRAMES_PER_SECOND.
	fl.cruiseSpeed = md->m_relativeSpeed / md->m_forwardDamping * 30.0f / (float)LOGICFRAMES_PER_SECOND;
	fl.specialFrames = (float)md->m_specialSpeedTime;
	fl.specialHeight = md->m_specialSpeedHeight;
	fl.launchZ = s.heightAtLaunch;
	// Dropping, the logic is over the target: its own threshold there, structures included if the data says so (Codex
	// missile-r1). Before that, the terrain at the target is the estimate. No margin on it (Codex visuals-3 r18): a margin
	// above the die height ended the flight clock a few frames before the logic dies, and the missile sat at its end point.
	fl.dieZ = (s.reachedIntermed ? dieHeightHere( obj, hd ) : TheTerrainLogic->getGroundHeight( s.target.x, s.target.y )
		+ (hd != nullptr ? hd->m_targetHeightAboveTerrain : 0.0f));

	MVNukeState ns;
	ns.pos = mvMake( P->x, P->y, P->z );
	ns.intermed = mvMake( s.intermed.x, s.intermed.y, s.intermed.z );
	ns.noTurnDistLeft = s.noTurnDistLeft;
	ns.reachedIntermed = s.reachedIntermed;
	ns.framesSinceLaunch = te;
	const float tau = mvNukeTau( ns, fl );
	mv->lastTau = tau;
	// Nothing pulls the nuke's drawn flight onto the logic path (wh1ter0se's look #1, the landing): the drawn arc ends at the target
	// on the frame the logic dies. HeightDieUpdate cannot kill it early either - it kills only moving down (its data says
	// OnlyWhenMovingDown), and the nuke moves down only in its drop over the target, at the die height it was aimed at - so
	// the height blends the SCUD needs are not needed here. Only a contact with something that can detonate it counts.
	// HeightDieUpdate can still kill it early over terrain higher than the target's (a launch from high ground toward a lower
	// target, Codex visuals-3 r19): only then does it get pulled onto the logic near the local die height. Over the target
	// itself the local die height IS the target's, so the landing keeps its angle.
	float low = 0.0f;
	if( canDieLow( mv, hd ) )
	{
		const float dieHere = dieHeightHere( obj, hd );
		if( dieHere > fl.dieZ + 5.0f )
			low = mvHeightBlend( P->z - dieHere, MV_SCUD_BLEND_BAND, MV_DIE_MARGIN );
	}
	float blend = proximityBlend( obj, mv, fl.dieZ );
	if( low > blend )
		blend = low;

	if( !mv->started )
	{
		if( te <= 0.0f )
			mv->L = ns.pos;
		else
		{
			// First seen mid-flight (a loaded save): the launch point is the silo's, at the height the logic recorded.
			const Object *launcher = TheGameLogic->findObjectByID( s.launcher );
			if( launcher == nullptr )
				return FALSE;
			mv->L = mvMake( launcher->getPosition()->x, launcher->getPosition()->y, s.heightAtLaunch );
		}
		mv->E = mvMake( s.target.x, s.target.y, fl.dieZ );
		mv->u = mvInitU( te, tau );
		mv->V = mvLerp( mvWorld( *mv->table, mv->L, mv->E, MV_HMIN, MV_HMAX, mv->u ), ns.pos, blend );
		mv->started = TRUE;
		trace( draw, mv, "start", now, te, tau, blend, P );
		return TRUE;
	}
	// Dropping, the die height is the logic's own there (structures included when its data says so - a bridge or a tall
	// building at the target, Codex visuals-3 r17): the arc ends at it. Terrain only until then.
	if( s.reachedIntermed )
		mv->E.z = fl.dieZ;
	mv->V = mvAdvance( *mv->table, &mv->u, mv->L, mv->E, ns.pos, tau, blend, MV_HMIN, MV_HMAX );
	trace( draw, mv, "fly", now, te, tau, blend, P );
	return TRUE;
}

/// One logic frame of a SCUD Storm missile.
static Bool stepScud( MissileVisual *mv, Drawable *draw, Object *obj, UnsignedInt now )
{
	AIUpdateInterface *ai = obj->getAI();
	const MissileAIUpdateModuleData *md =
		static_cast<const MissileAIUpdateModuleData *>( templateModule( obj->getTemplate(), "MissileAIUpdate" ) );
	const HeightDieUpdateModuleData *hd =
		static_cast<const HeightDieUpdateModuleData *>( templateModule( obj->getTemplate(), "HeightDieUpdate" ) );
	if( ai == nullptr || md == nullptr || ai->getGoalPosition() == nullptr || ai->getCurLocomotor() == nullptr )
		return FALSE;
	const Coord3D *P = obj->getPosition();
	const Coord3D *goal = ai->getGoalPosition();
	// The speed the locomotor moves it at NOW: a damaged body state can have a lower one (Codex visuals-3 r30).
	const BodyModuleInterface *body = obj->getBodyModule();
	const float speed = ai->getCurLocomotor()->getMaxSpeedForCondition( body != nullptr ? body->getDamageState() : BODY_PRISTINE );
	if( speed <= 0.0f )
		return FALSE;
	// The goal's own height when it is above the ground there - a bridge or other raised target (Codex missile-r6).
	const Real groundAtGoal = TheTerrainLogic->getGroundHeight( goal->x, goal->y );
	const MVVec E = mvMake( goal->x, goal->y, (goal->z > groundAtGoal ? goal->z : groundAtGoal)
		+ (hd != nullptr ? hd->m_targetHeightAboveTerrain : 0.0f) );
	const MVVec pos = mvMake( P->x, P->y, P->z );

	if( !mv->started )
	{
		// Only a missile still on its straight climb above its launcher gets a flight; one first seen elsewhere (a loaded
		// save) keeps the logic look - unlike the nuke, a SCUD does not record where and when it left. Position, not
		// speed (Codex missile-r2): a network tick can move a fresh SCUD before its first client update.
		const ProjectileUpdateInterface *pui = obj->getProjectileUpdateInterface();
		const Object *launcher = pui != nullptr ? TheGameLogic->findObjectByID( pui->projectileGetLauncherID() ) : nullptr;
		if( launcher == nullptr )
			return FALSE;
		const Real lx = P->x - launcher->getPosition()->x, ly = P->y - launcher->getPosition()->y;
		const Real reach = launcher->getGeometryInfo().getBoundingCircleRadius() + 50.0f;
		if( lx * lx + ly * ly > reach * reach || P->z - launcher->getPosition()->z > md->m_initialDist )
			return FALSE;
		// A finite FuelLifetime with DetonateOnNoFuel (mod data; the shipped SCUD has none) detonates it wherever it is when
		// the fuel runs out (MissileAIUpdate::doAttackState). The ignition frame is private to the logic module and a client
		// update can first see the missile several ticks after it (Codex missile-r1 of visuals-3), so the countdown cannot be
		// anchored: such a missile keeps the logic look.
		if( md->m_fuelLifetime > 0 && md->m_detonateOnNoFuel )
			return FALSE;
		mv->L = pos;
		mv->teOffset = (float)now;
	}
	else if( mvLen( mvSub( E, mv->E ) ) > MV_GOAL_MOVED )
		return FALSE;
	mv->E = E;

	// Remaining path: what is left of the straight climb, then a straight line from its top to the end point.
	// Latched: the logic SCUD comes back down below its climb height long before the end, and counting that as climb
	// still to do made tau 1.5-4x too long over the last 30% of the flight (measured on the Mini recording, rec4).
	if( pos.z - mv->L.z > mv->maxClimb )
		mv->maxClimb = pos.z - mv->L.z;
	const float climbed = mv->maxClimb;
	const float climbLeft = md->m_initialDist > climbed ? md->m_initialDist - climbed : 0.0f;
	const float topZ = pos.z + climbLeft;
	const float dx = E.x - pos.x, dy = E.y - pos.y;
	float tau = (climbLeft + sqrtf( dx * dx + dy * dy + (topZ - E.z) * (topZ - E.z) )) / speed;
	mv->lastTau = tau;
	// Onto the logic near its goal, and also wherever it could detonate early (Codex missile-r1): a SCUD collides with
	// structures (ProjectileCollidesWith) and HeightDieUpdate counts them, so nearing its local die height pulls it onto
	// the logic too. KNOWN LIMIT: cruising low over a structure taller than ~40 above the ground inside its bounding
	// circle pulls the drawn SCUD part-way toward the logic path for those frames.
	const float nearDeath = canDieLow( mv, hd ) ? mvHeightBlend( pos.z - dieHeightHere( obj, hd ), MV_SCUD_BLEND_BAND, MV_DIE_MARGIN ) : 0.0f;
	const float nearGoal = mvSmooth( 1.0f - (mvLen( mvSub( pos, E ) ) - MV_SCUD_BLEND_NEAR) / (MV_SCUD_BLEND_DIST - MV_SCUD_BLEND_NEAR) );
	const float prox = proximityBlend( obj, mv );
	float blend = nearDeath > nearGoal ? nearDeath : nearGoal;
	if( prox > blend )
		blend = prox;
	const float te = (float)now - mv->teOffset;

	if( !mv->started )
	{
		mv->u = 0.0f;
		mv->V = pos;
		mv->started = TRUE;
		trace( draw, mv, "start", now, te, tau, blend, P );
		return TRUE;
	}
	mv->V = mvAdvance( *mv->table, &mv->u, mv->L, mv->E, pos, tau, blend, MV_HMIN, MV_HMAX );
	trace( draw, mv, "fly", now, te, tau, blend, P );
	return TRUE;
}

// ------------------------------------------------------------------------------------------------------------------
// GeneralsX @test Claude 28/09/2026 GX_TEST_FIRESW=1: the LOCAL player fires each of its nuke / SCUD Storm special powers
// once, as soon as it is ready, at the nearest enemy command center - through MSG_DO_SPECIAL_POWER_AT_LOCATION with the
// silo as the source, which is exactly what a click on the superweapon button sends. It is player INPUT: it goes through
// the command stream, is executed on the same logic frame by every peer and is recorded in the replay, so the replay can
// be played back by any build of the same digest - the before/after captures and the CRC A/B are built on that. The
// skirmish AI does not fire superweapons it did not build itself (measured: 30000 frames, none), hence this. Off unless
// set; it only reads logic state to choose.
void missileVisualTestFire()
{
	static Int s_on = -1;
	static Int s_buildOnly = 0;	// GX_TEST_FIRESW=2: finish the sites and say when a source is ready, fire nothing (a human presses the key)
	if( s_on < 0 )
	{
		const char *env = getenv( "GX_TEST_FIRESW" );
		s_on = (env != nullptr && (env[0] == '1' || env[0] == '2')) ? 1 : 0;
		s_buildOnly = (env != nullptr && env[0] == '2') ? 1 : 0;
		if( s_on )
		{
			fprintf( stderr, "[GXMVIS] test fire ON%s: the local player fires its ready nuke/SCUD at the nearest enemy command center\n", s_buildOnly ? " (GX_TEST_FIRESW=2: builds the sites, logs 'test ready', never fires)" : "" );
			fflush( stderr );
		}
	}
	// GX_TEST_AICASH=<amount>: once per match, at frame 30, the local player gives every computer player <amount>
	// through MSG_GX_CHEAT_GIVE_MONEY (a single-human match allows it), so the skirmish AI builds and fires real
	// superweapons. The lobby stress option cannot supply them: BuildAssistant::buildObjectNow leaves its buildings
	// OBJECT_STATUS_UNDER_CONSTRUCTION forever (measured: the SCUD Storm stays idle with 9 rounds after the order).
	static Int s_cash = -1;
	if( s_cash < 0 )
	{
		const char *env = getenv( "GX_TEST_AICASH" );
		s_cash = env != nullptr ? atoi( env ) : 0;
		if( s_cash < 0 )
			s_cash = 0;
	}
	if( (!s_on && s_cash == 0) || TheGameLogic == nullptr || !TheGameLogic->isInGame() || TheGameLogic->isInShellGame() || ThePlayerList == nullptr )
		return;
	static std::vector<ObjectID> s_fired;
	static UnsignedInt s_lastFrame = 0;
	static Bool s_paid = FALSE;
	static ObjectID s_site = INVALID_ID;
	static UnsignedInt s_sentAt = 0;
	const UnsignedInt now = TheGameLogic->getFrame();
	if( now < s_lastFrame )
	{
		s_fired.clear();	// a new match: everything per-match starts over (Codex missile-r5)
		s_paid = FALSE;
		s_site = INVALID_ID;
		s_sentAt = 0;
	}
	s_lastFrame = now;
	if( s_cash > 0 && !s_paid && now >= 30 )
	{
		s_paid = TRUE;
		for( Int i = 0; i < ThePlayerList->getPlayerCount(); ++i )
		{
			Player *p = ThePlayerList->getNthPlayer( i );
			if( p == nullptr || p->getPlayerType() != PLAYER_COMPUTER || !p->isPlayableSide() )
				continue;
			GameMessage *msg = TheMessageStream->appendMessage( GameMessage::MSG_GX_CHEAT_GIVE_MONEY );
			msg->appendIntegerArgument( p->getPlayerIndex() );
			msg->appendIntegerArgument( s_cash );
			fprintf( stderr, "[GXMVIS] f=%u test cash: $%d to computer player %d\n", (unsigned)now, s_cash, (int)p->getPlayerIndex() );
			fflush( stderr );
		}
	}
	if( !s_on )
		return;
	const Player *me = ThePlayerList->getLocalPlayer();
	// Finish our own stuck buildings first, through the real construction path. The lobby stress option places them
	// with BuildAssistant::buildObjectNow, which leaves OBJECT_STATUS_UNDER_CONSTRUCTION set forever, so a superweapon
	// there never fires. Once a second, our first dozer/worker is ordered to resume the first of our buildings still under
	// construction: MSG_CREATE_SELECTED_GROUP + MSG_RESUME_CONSTRUCTION, exactly what clicking it on the building sends.
	if( now % LOGICFRAMES_PER_SECOND == 0 )
	{
		const Object *dozer = nullptr, *site = nullptr;
		for( Object *o = TheGameLogic->getFirstObject(); o != nullptr; o = o->getNextObject() )
		{
			if( o->getControllingPlayer() != me || o->isEffectivelyDead() )
				continue;
			if( dozer == nullptr && o->isKindOf( KINDOF_DOZER ) )
				dozer = o;
			else if( site == nullptr && o->isKindOf( KINDOF_STRUCTURE ) && o->testStatus( OBJECT_STATUS_UNDER_CONSTRUCTION ) )
				site = o;
		}
		if( dozer != nullptr && site != nullptr && (site->getID() != s_site || now >= s_sentAt + 20 * LOGICFRAMES_PER_SECOND) )
		{
			GameMessage *sel = TheMessageStream->appendMessage( GameMessage::MSG_CREATE_SELECTED_GROUP );
			sel->appendBooleanArgument( TRUE );
			sel->appendObjectIDArgument( dozer->getID() );
			GameMessage *msg = TheMessageStream->appendMessage( GameMessage::MSG_RESUME_CONSTRUCTION );
			msg->appendObjectIDArgument( site->getID() );
			s_site = site->getID();
			s_sentAt = now;
			fprintf( stderr, "[GXMVIS] f=%u test build: '%s' id=%u resumes '%s' id=%u\n", (unsigned)now, dozer->getTemplate()->getName().str(),
				(unsigned)dozer->getID(), site->getTemplate()->getName().str(), (unsigned)site->getID() );
			fflush( stderr );
		}
	}
	for( Object *src = TheGameLogic->getFirstObject(); src != nullptr; src = src->getNextObject() )
	{
		if( src->getControllingPlayer() != me || src->isEffectivelyDead() )
			continue;
		const SpecialPowerType types[4] = { SPECIAL_NEUTRON_MISSILE, NUKE_SPECIAL_NEUTRON_MISSILE, SUPW_SPECIAL_NEUTRON_MISSILE, SPECIAL_SCUD_STORM };
		for( Int k = 0; k < 4; ++k )
		{
			SpecialPowerModuleInterface *sp = src->findSpecialPowerModuleInterface( types[k] );
			if( sp == nullptr || !sp->isReady() || std::find( s_fired.begin(), s_fired.end(), src->getID() ) != s_fired.end() )
				continue;
			if( s_buildOnly )
			{
				s_fired.push_back( src->getID() );
				fprintf( stderr, "[GXMVIS] f=%u test ready: '%s' id=%u\n", (unsigned)now, src->getTemplate()->getName().str(), (unsigned)src->getID() );
				fflush( stderr );
				continue;
			}
			const Object *best = nullptr;
			Real bestD = 0.0f;
			for( Object *o = TheGameLogic->getFirstObject(); o != nullptr; o = o->getNextObject() )
			{
				if( !o->isKindOf( KINDOF_COMMANDCENTER ) || o->isEffectivelyDead() || src->getRelationship( o ) != ENEMIES )
					continue;
				// ActionManager::canDoSpecialPowerAtLocation refuses a shrouded target: pick only one it accepts, so the silo is
				// not marked fired for an order that cannot execute (Codex visuals-3 r8).
				if( ThePartitionManager->getShroudStatusForPlayer( me->getPlayerIndex(), o->getPosition() ) == CELLSHROUD_SHROUDED )
					continue;
				const Real d = ThePartitionManager->getDistanceSquared( src, o, FROM_CENTER_2D );
				if( best == nullptr || d < bestD )
				{
					best = o;
					bestD = d;
				}
			}
			if( best == nullptr )
				continue;
			GameMessage *msg = TheMessageStream->appendMessage( GameMessage::MSG_DO_SPECIAL_POWER_AT_LOCATION );
			msg->appendIntegerArgument( sp->getSpecialPowerTemplate()->getID() );
			msg->appendLocationArgument( *best->getPosition() );
			msg->appendRealArgument( INVALID_ANGLE );
			msg->appendObjectIDArgument( INVALID_ID );
			msg->appendIntegerArgument( NEED_TARGET_POS );
			msg->appendObjectIDArgument( src->getID() );
			s_fired.push_back( src->getID() );
			fprintf( stderr, "[GXMVIS] f=%u test fire %s from '%s' id=%u at '%s' (%.0f,%.0f,%.0f) range=%.0f\n", (unsigned)now,
				types[k] == SPECIAL_SCUD_STORM ? "scud" : "nuke", src->getTemplate()->getName().str(), (unsigned)src->getID(),
				best->getTemplate()->getName().str(), best->getPosition()->x, best->getPosition()->y, best->getPosition()->z, sqrtf( bestD ) );
			fflush( stderr );
		}
	}
}

// ------------------------------------------------------------------------------------------------------------------
// GeneralsX @feature Claude 29/09/2026 (wh1ter0se's look #1: "like in CS2, a shockwave"): a VISUAL expanding dust ring from the
// nuke's detonation, client-only and digest-neutral - nothing here reads back into the simulation. It is timed to the
// blasts the simulation itself does: the nuke's NeutronMissileSlowDeathBehavior data lists each blast's delay after the
// missile dies and its radius (60, 90, 120, 150, 180, then the damage blast at 210 with the 3500 damage), so the ring's
// radius is that schedule, read from the data, and its front reaches 210 on the very frame the damage lands. The
// existing NukeShockwave particle template (EXCloud01 dust) is used as is, re-shaped per instance to a hollow ring.
namespace
{
	struct Shock
	{
		Coord3D pos;
		UnsignedInt start, end;
		int n;
		float t[MAX_NEUTRON_BLASTS + 1], r[MAX_NEUTRON_BLASTS + 1];
		ParticleSystemID ids[6];
		Bool stopped;	///< the emission has been stopped (the dust is fading out)
		AudioHandle whoosh;	///< test trace only: is the blast sound playing?
		Shadow *decal;		///< the front: an alpha ground decal (DM_Ring) re-sized every rendered frame (a particle only moves per particle update)
		DrawableID drawable;	///< the nuke's drawable: the ring is shown while it is (its shroud flag, with the classic grace)
	};
	std::vector<Shock> s_shocks;
	const float MV_SHOCK_START_RADIUS = 20.0f;
	const int MV_SHOCK_RINGS = 0;		// the front is a ground decal now (below); the particle systems are only the dust kicked up behind it
	const int MV_SHOCK_SYSTEMS = 3;
	const float MV_SHOCK_OVERSHOOT_FRAMES = 45.0f;	// it keeps expanding past the last blast radius for this long, fading out
	const float MV_SHOCK_OVERSHOOT_SPEED = 3.0f;	// units per logic frame
	const UnsignedInt MV_SHOCK_LINGER = 3000;	// frames the visibility check goes on after the emission stops: longer than the dust lives (600 frames)
	const UnsignedInt MV_SHOCK_TAIL = 8;	// frames the dust keeps coming after the damage blast
}

static float shockRadius( const Shock &s, float t )
{
	if( t <= s.t[0] )
		return s.r[0];
	for( int i = 1; i < s.n; ++i )
		if( t <= s.t[i] )
			return s.r[i-1] + (s.r[i] - s.r[i-1]) * (t - s.t[i-1]) / (s.t[i] - s.t[i-1]);
	// Past the last blast the front keeps going (visual only) at the pace it had, and the caller fades it out.
	return s.r[s.n - 1] + ( t - s.t[s.n - 1] ) * MV_SHOCK_OVERSHOOT_SPEED;
}

static void shockStart( Object *obj, Drawable *draw )
{
	if( TheParticleSystemManager == nullptr || TheGlobalData == nullptr || !TheGlobalData->m_useFX )
		return;
	const NeutronMissileSlowDeathBehaviorModuleData *md = static_cast<const NeutronMissileSlowDeathBehaviorModuleData *>(
		templateModule( obj->getTemplate(), "NeutronMissileSlowDeathBehavior" ) );
	const ParticleSystemTemplate *tmpl = TheParticleSystemManager->findTemplate( AsciiString( "NukeShockwave" ) );
	if( md == nullptr || tmpl == nullptr )
		return;
	// Not through the fog: unattached particle systems are not shroud-checked, so a ring at an unseen detonation would give
	// its place away (Codex visuals-3 r17). The classic FX have the same gate in the look of the nuke; here it is explicit.
	if( gxShroudDrawn() && draw->getFullyObscuredByShroud() )
		return;
	Shock s;
	s.n = 0;
	s.t[s.n] = 0.0f;
	s.r[s.n++] = MV_SHOCK_START_RADIUS;
	// The blasts in delay order, skipping the ones parked at 999999 ("don't do the damage wave").
	Int order[MAX_NEUTRON_BLASTS];
	Int m = 0;
	for( Int i = 0; i < MAX_NEUTRON_BLASTS; ++i )
		if( md->m_blastInfo[i].enabled && md->m_blastInfo[i].delay >= 0.0f && md->m_blastInfo[i].delay < 20.0f * LOGICFRAMES_PER_SECOND
			&& md->m_blastInfo[i].outerRadius > 0.0f )
			order[m++] = i;
	std::sort( order, order + m, [md]( Int a, Int b ) { return md->m_blastInfo[a].delay < md->m_blastInfo[b].delay; } );
	for( Int k = 0; k < m && s.n < MAX_NEUTRON_BLASTS + 1; ++k )
	{
		// The logic fires a blast once `frame - activation > delay`, i.e. on frame delay + 1.
		const float t = md->m_blastInfo[order[k]].delay + 1.0f;
		const float r = md->m_blastInfo[order[k]].outerRadius;
		if( t > s.t[s.n - 1] && r >= s.r[s.n - 1] )
		{
			s.t[s.n] = t;
			s.r[s.n++] = r;
		}
	}
	if( s.n < 2 )
		return;
	s.stopped = FALSE;
	s.drawable = draw->getID();
	s.start = TheGameLogic->getFrame();
	s.end = s.start + (UnsignedInt)( s.t[s.n - 1] + MV_SHOCK_OVERSHOOT_FRAMES ) + MV_SHOCK_TAIL;
	s.pos = *obj->getPosition();
	s.pos.z = TheTerrainLogic->getGroundHeight( s.pos.x, s.pos.y );
	// wh1ter0se's look #2/#3: the front is a ground decal (shockDecalUpdate, re-sized every rendered frame); the particle systems here
	// are the short-lived dust emitted at the front that hangs behind it.
	const ParticleSystemTemplate *ringTmpl = TheParticleSystemManager->findTemplate( AsciiString( "ArtilleryBarrageShockwave" ) );
	for( int i = 0; i < MV_SHOCK_SYSTEMS; ++i )
	{
		s.ids[i] = INVALID_PARTICLE_SYSTEM_ID;
		const Bool ring = i < MV_SHOCK_RINGS;
		if( ring && ringTmpl == nullptr )
			continue;
		ParticleSystem *ps = TheParticleSystemManager->createParticleSystem( ring ? ringTmpl : tmpl, FALSE );
		if( ps == nullptr )
			continue;
		ps->setSaveable( FALSE );	// not part of a save: the ring is a visual of the moment and its radius is driven from here (Codex visuals-3 r20)
		Coord3D at = s.pos;
		at.z += 2.0f;	// ground-hugging
		ps->setPosition( &at );
		if( ring )
		{
			ps->m_particleAlignment = ParticleSystem::PARTICLE_ALIGNMENT_XYPLANAR;	// flat on the ground
			ps->m_emissionVolumeType = ParticleSystem::POINT;
			ps->m_isEmissionVolumeHollow = FALSE;
			ps->m_emissionVelocity.outward.speed.setRange( 0.0f, 0.0f );
			ps->m_lifetime.setRange( 8.0f, 8.0f );	// each ring particle keeps the size of the moment it was emitted: a fading wake of rings behind the front
			ps->m_sizeRate.setRange( 0.0f, 0.0f );
			ps->m_startSizeRate.setRange( 0.0f, 0.0f );
			ps->m_sizeRateDamping.setRange( 1.0f, 1.0f );
			ps->m_startSize.setRange( 2.0f * MV_SHOCK_START_RADIUS, 2.0f * MV_SHOCK_START_RADIUS );
			static const float ringRGB[3][3] = { { 1.0f, 1.0f, 0.9f }, { 0.85f, 0.65f, 0.35f }, { 0.55f, 0.4f, 0.25f } };	// front: hot white; then orange; then dim
			ps->m_colorKey[0].color.red = ringRGB[i][0];
			ps->m_colorKey[0].color.green = ringRGB[i][1];
			ps->m_colorKey[0].color.blue = ringRGB[i][2];
			ps->m_colorKey[0].frame = 0;
			ps->m_colorKey[1].color.red = 0.0f;
			ps->m_colorKey[1].color.green = 0.0f;
			ps->m_colorKey[1].color.blue = 0.0f;
			ps->m_colorKey[1].frame = 8;
			ps->setBurstCountMultiplier( i == 0 ? 4.0f : ( i == 1 ? 3.0f : 2.0f ) );	// additive: several ring particles per update add up to a bright, thick front
		}
		else
		{
			ps->m_isEmissionVolumeHollow = TRUE;
			ps->m_emissionVolume.cylinder.radius = MV_SHOCK_START_RADIUS;
			ps->m_emissionVolume.cylinder.length = 3.0f;
			ps->m_lifetime.setRange( 22.0f, 32.0f );
			ps->m_startSize.setRange( 6.0f, 14.0f );
			ps->m_sizeRate.setRange( 0.3f, 0.8f );
			ps->m_colorKey[0].color.red = 0.85f;	// light dust, not the template's dark grey smoke
			ps->m_colorKey[0].color.green = 0.75f;
			ps->m_colorKey[0].color.blue = 0.6f;
			ps->m_colorKey[1].color.red = 0.7f;
			ps->m_colorKey[1].color.green = 0.6f;
			ps->m_colorKey[1].color.blue = 0.45f;
			ps->setBurstCountMultiplier( 0.10f );
		}
		ps->setSystemLifetime( (UnsignedInt)( s.t[s.n - 1] + MV_SHOCK_OVERSHOOT_FRAMES + MV_SHOCK_TAIL ) );
		s.ids[i] = ps->getSystemID();
	}
	s.decal = nullptr;
	s.whoosh = AHSV_Error;
	// (the decal front is made on the first tick, shockDecalUpdate)
	// A shipped sound (SoundEffects.ini: the nuke cinematic's whoosh), positional at the blast; the ring's own visibility gate
	// above already kept this out of the fog.
	if( TheAudio != nullptr )
	{
		AudioEventRTS whoosh( "Cin_NukeWhoosh" );
		whoosh.setPosition( &s.pos );
		const AudioHandle h = TheAudio->addAudioEvent( &whoosh );
		s.whoosh = h;
		if( missileVisualTraceOn() )
			fprintf( stderr, "[GXMVSHOCK] whoosh handle=%u at (%.0f,%.0f,%.0f)\n", (unsigned)h, s.pos.x, s.pos.y, s.pos.z );
	}
	s_shocks.push_back( s );
	if( missileVisualTraceOn() )
	{
		fprintf( stderr, "[GXMVIS] f=%u shockwave start at (%.0f,%.0f,%.0f), %d steps to r=%.0f at +%.0f frames\n", (unsigned)s.start,
			s.pos.x, s.pos.y, s.pos.z, s.n, s.r[s.n - 1], s.t[s.n - 1] );
		fflush( stderr );
	}
}

// The texture is the shipped DM_Ring (white, the ring is its alpha): the particle texture EXShockWav.tga is in no shipped archive, so a
// decal named after it has nothing to draw (measured: the decal existed and moved every frame, no ring in any still).
static void shockDecalRelease( Shock &s )
{
	if( s.decal != nullptr )
		s.decal->release();
	s.decal = nullptr;
}

/// The front at radius r, opacity 0..1: made once, then only re-sized (Shadow::setSize) - called every rendered frame.
static void shockDecalUpdate( Shock &s, float r, float opacity )
{
	if( TheProjectedShadowManager == nullptr || r <= 0.0f || opacity <= 0.0f )
	{
		shockDecalRelease( s );
		return;
	}
	if( s.decal == nullptr )
	{
		Shadow::ShadowTypeInfo info;
		info.allowUpdates = FALSE;
		info.allowWorldAlign = TRUE;
		info.m_type = SHADOW_ALPHA_DECAL;	// the ring is DM_Ring's alpha channel, its colour is plain white: additive would draw a white square
		strlcpy( info.m_ShadowName, "DM_Ring", ARRAY_SIZE( info.m_ShadowName ) );
		info.m_sizeX = info.m_sizeY = 2.0f * r;
		s.decal = TheProjectedShadowManager->addDecal( &info );
		if( s.decal == nullptr )
			return;
		s.decal->setAngle( 0.0f );
		s.decal->setColor( 0xFFFFEBC8 );
		s.decal->setPosition( s.pos.x, s.pos.y, s.pos.z );
	}
	// Cost: a decal is tessellated over the terrain it covers each frame (about 4,900 vertices at the ~345 peak radius). That is the
	// price of the shipped radius-cursor decals (RadiusDecal, 2r squares re-projected every frame while a superweapon is aimed) and
	// this one lives ~3 s per detonation (visual only), so it is accepted, not bounded (Codex v4 r2).
	s.decal->setSize( 2.0f * r, 2.0f * r );
	s.decal->setOpacity( (Int)( 255.0f * ( opacity > 1.0f ? 1.0f : opacity ) ) );
}

void missileVisualShockReset()
{
	for( size_t i = 0; i < s_shocks.size(); ++i )
		shockDecalRelease( s_shocks[i] );
	s_shocks.clear();
}

void missileVisualShockTick()
{
	if( s_shocks.empty() || TheGameLogic == nullptr || TheParticleSystemManager == nullptr )
		return;
	const UnsignedInt now = TheGameLogic->getFrame();
	for( size_t i = 0; i < s_shocks.size(); )
	{
		Shock &s = s_shocks[i];
		if( now < s.start )
		{
			// The frame counter went backwards: a new match. Its systems went with the old one's particle manager, and their IDs
			// may be somebody else's now - forget the ring without touching them (Codex visuals-3 r17).
			s_shocks[i].decal = nullptr;	// the manager that owned it went with the old match
			s_shocks.erase( s_shocks.begin() + i );
			continue;
		}
		// Recheck the fog every frame (Codex visuals-3 r18): losing sight of the place, or switching to a player who cannot see
		// it, removes the ring at once - unattached particle systems are not shroud-checked by the particle code. The check
		// goes on while the dust fades out after the last blast (MV_SHOCK_LINGER), until the particles are gone (r27).
		Drawable *nuke = TheGameClient->findDrawableByID( s.drawable );
		// The nuke's drawable gone (its slow death is over): so is the ring, with nothing left to check its visibility by (r29).
		const Bool hidden = nuke == nullptr || (gxShroudDrawn() && nuke->getFullyObscuredByShroud());
		const Bool ending = now >= s.end;
		if( missileVisualTraceOn() && TheAudio != nullptr && s.whoosh >= AHSV_FirstHandle && now == s.start + 3 )
			fprintf( stderr, "[GXMVSHOCK] f=%u whoosh handle=%u playing=%d\n", (unsigned)now, (unsigned)s.whoosh, (int)TheAudio->isCurrentlyPlaying( s.whoosh ) );
		const float r = shockRadius( s, (float)( now - s.start ) );
		for( int k = 0; k < MV_SHOCK_SYSTEMS; ++k )
		{
			if( s.ids[k] == INVALID_PARTICLE_SYSTEM_ID )
				continue;
			ParticleSystem *ps = TheParticleSystemManager->findParticleSystem( s.ids[k] );
			// Ours are never saveable (shockStart): a saveable system under this ID is a loaded or new game's, not ours
			// (Codex visuals-3 r21) - the ring is forgotten, not touched.
			if( ps != nullptr && ps->isSaveable() )
				ps = nullptr;
			if( ps == nullptr )
			{
				s.ids[k] = INVALID_PARTICLE_SYSTEM_ID;
				continue;
			}
			if( hidden )
			{
				// Sight lost: gone at once, particles and all (the manager itself deletes systems this way).
				deleteInstance( ps );
				s.ids[k] = INVALID_PARTICLE_SYSTEM_ID;
			}
			else if( ending )
			{
				if( !s.stopped )
					ps->destroy();	// no more dust; what is in the air fades out
			}
			else if( k < MV_SHOCK_RINGS )
				ps->m_startSize.setRange( 2.0f * r * ( k == 0 ? 1.0f : ( k == 1 ? 1.1f : 1.2f ) ), 2.0f * r * ( k == 0 ? 1.0f : ( k == 1 ? 1.1f : 1.2f ) ) );	// the ring particle IS the wave front
			else
				ps->setEmissionVolumeCylinderRadius( r );
		}
		if( ending )
			s.stopped = TRUE;
		Bool anyLeft = FALSE;
		for( int k = 0; k < MV_SHOCK_SYSTEMS; ++k )
			anyLeft = anyLeft || s.ids[k] != INVALID_PARTICLE_SYSTEM_ID;
		// The front: re-sized every RENDERED frame from the logic phase (wh1ter0se: the particle ring moved once per particle update -
		// "1 fps"), still on the blast schedule; past the last blast it keeps going and fades.
		{
			const float phase = TheDisplay != nullptr ? TheDisplay->getLogicPhase() : 1.0f;
			float tf = (float)( now - s.start ) - 1.0f + phase;
			if( tf < 0.0f )
				tf = 0.0f;
			const float tLast = s.t[s.n - 1];
			const float op = tf <= tLast ? 1.0f : 1.0f - ( tf - tLast ) / MV_SHOCK_OVERSHOOT_FRAMES;
			if( hidden || ending )
				shockDecalRelease( s );
			else
				shockDecalUpdate( s, shockRadius( s, tf ), op );
			if( missileVisualTraceOn() && ( now % 10 ) == 0 && !hidden && !ending )
				fprintf( stderr, "[GXMVSHOCK] f=%u tf=%.2f r=%.1f op=%.2f decal=%p\n", (unsigned)now, tf, shockRadius( s, tf ), op, (void*)s.decal );
		}
		if( !anyLeft || now >= s.end + MV_SHOCK_LINGER )
			shockDecalRelease( s );
		if( !anyLeft || now >= s.end + MV_SHOCK_LINGER )	// the dust lives ~600 frames (NukeShockwave): tracked until it is gone (r31)
			s_shocks.erase( s_shocks.begin() + i );
		else
			++i;
	}
}

// ------------------------------------------------------------------------------------------------------------------
const Matrix3D *missileVisualUpdate( Drawable *draw, MissileVisual **state, Bool *checked )
{
	Object *obj = draw->getObject();
	if( *state == nullptr )
	{
		if( *checked || obj == nullptr )
			return nullptr;
		*checked = TRUE;
		static const NameKeyType s_nuke = NAMEKEY( "NeutronMissileUpdate" );
		static const NameKeyType s_scud = NAMEKEY( "MissileAIUpdate" );
		MissileVisualKind kind;
		if( obj->findUpdateModule( s_nuke ) != nullptr )
			kind = MV_KIND_NUKE;
		else if( obj->getAI() != nullptr && obj->getAI()->getModuleNameKey() == s_scud )
			kind = MV_KIND_SCUD;
		else
			return nullptr;
		if( !isSuperweaponMissile( obj->getTemplate(), kind ) || profileTable( kind ) == nullptr )
			return nullptr;
		if( missileVisualTraceOn() )
		{
			fprintf( stderr, "[GXMVIS] f=%u id=%u track %s '%s'\n", (unsigned)TheGameLogic->getFrame(), (unsigned)draw->getID(),
				kind == MV_KIND_NUKE ? "nuke" : "scud", obj->getTemplate()->getName().str() );
			fflush( stderr );
		}
		MissileVisual *mv = NEW MissileVisual;
		memset( (void *)mv, 0, sizeof( *mv ) );
		mv->kind = kind;
		// First seen already dead (a loaded save during the slow death): the blast began long ago, no ring now (Codex visuals-3
		// r23). KNOWN LIMIT: a nuke that dies before its first client update (its launch frame) gets no ring either - it cannot: its
		// HeightDieUpdate has InitialDelay 1000 ms, and nothing else kills it on the pad.
		mv->shocked = obj->isEffectivelyDead();
		mv->table = profileTable( kind );
		mv->evalFrame = 0xFFFFFFFF;
		mv->xfrm.Make_Identity();
		*state = mv;
	}
	MissileVisual *mv = *state;
	// The nuke's death starts the shockwave (once), whether or not its flight was still being drawn.
	if( mv->kind == MV_KIND_NUKE && !mv->shocked && obj != nullptr && obj->isEffectivelyDead() )
	{
		mv->shocked = TRUE;
		shockStart( obj, draw );
	}
	if( mv->retired )
		return nullptr;
	const UnsignedInt now = TheGameLogic->getFrame();
	if( now == mv->evalFrame )
	{
		return missileVisualTransform( mv );
	}
	mv->evalFrame = now;

	if( obj == nullptr || obj->isEffectivelyDead() )
	{
		mv->active = FALSE;
		mv->retired = TRUE;
		return nullptr;
	}

	const Coord3D *P = obj->getPosition();
	// HeightDieUpdate compares this frame's height with the last one's; a missile first seen already falling counts too.
	if( mv->haveLastP && P->z < mv->lastP.z )
		mv->descended = TRUE;
	Bool ok;
	if( mv->abandonLeft > 0 )
		ok = TRUE;
	else
		ok = mv->kind == MV_KIND_NUKE ? stepNuke( mv, draw, obj, now ) : stepScud( mv, draw, obj, now );
	mv->lastP = mvMake( P->x, P->y, P->z );
	mv->haveLastP = TRUE;

	if( !ok && mv->started && mv->abandonLeft == 0 )
	{
		// It was being drawn away from the logic: pull it back rather than jump.
		// Back on the logic path before it can die: no longer than the flight has left (a SCUD jammed late in flight, Codex
		// visuals-3 r29), so the explosion is where the drawn missile is.
		mv->abandonLeft = std::max( 1, std::min( (int)MV_ABANDON_FRAMES, (int)ceilf( mv->lastTau ) ) );
		trace( draw, mv, "abandon", now, 0.0f, 0.0f, 0.0f, P );
	}
	if( mv->abandonLeft > 0 )
	{
		const MVVec pos = mvMake( P->x, P->y, P->z );
		mv->V = mvLerp( mv->V, pos, 1.0f / (float)mv->abandonLeft );
		if( --mv->abandonLeft == 0 )
		{
			mv->active = FALSE;
			mv->retired = TRUE;
			return nullptr;
		}
	}
	else if( !ok )
	{
		// Never drawn away from the logic (unreadable state, or first seen already in flight): logic look throughout.
		if( missileVisualTraceOn() )
		{
			fprintf( stderr, "[GXMVIS] f=%u id=%u %s declined: drawn at the logic transform\n", (unsigned)now,
				(unsigned)draw->getID(), mv->kind == MV_KIND_NUKE ? "nuke" : "scud" );
			fflush( stderr );
		}
		mv->active = FALSE;
		mv->retired = TRUE;
		return nullptr;
	}
	if( !mv->started )
	{
		mv->active = FALSE;
		return nullptr;
	}

	// The nose follows the drawn velocity. While that is still vertical its heading is taken toward the target, so the
	// frame has a defined yaw (buildTransformMatrix points +X along the direction).
	const MVVec step = mvSub( mv->V, mv->prevV );
	if( mv->havePrev && mvLen( step ) > 0.01f )
	{
		mv->dir.Set( step.x, step.y, step.z );
		mv->dir.Normalize();
	}
	else if( !mv->havePrev )
		mv->dir = obj->getTransformMatrix()->Get_X_Vector();
	Vector3 d = mv->dir;
	const float hx = mv->E.x - mv->L.x, hy = mv->E.y - mv->L.y, hl = sqrtf( hx * hx + hy * hy );
	if( d.X * d.X + d.Y * d.Y < 1.0e-6f && hl > 1.0f )
	{
		d.X += 1.0e-3f * hx / hl;
		d.Y += 1.0e-3f * hy / hl;
		d.Normalize();
	}
	mv->xfrm.buildTransformMatrix( Vector3( mv->V.x, mv->V.y, mv->V.z ), d );
	// Visibility is the missile's own, as in the classic look: GameClient::update sets the drawable's shroud flag from the
	// logic object (with its grace). It is NOT decided at the drawn position (wh1ter0se's look #1, "it disappears"): the nuke's
	// vision clears the fog around its logic position only, and the drawn arc runs hundreds of units from it.
	mv->prevV = mv->V;
	mv->havePrev = TRUE;
	mv->active = TRUE;
	return missileVisualTransform( mv );
}

const Matrix3D *missileVisualTransform( const MissileVisual *state )
{
	return (state != nullptr && state->active && !state->retired && missileVisualDrawOn()) ? &state->xfrm : nullptr;
}

void missileVisualFree( MissileVisual **state )
{
	delete *state;
	*state = nullptr;
}
