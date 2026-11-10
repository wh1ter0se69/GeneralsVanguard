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
// GeneralsX: THE MATH OF THE SUPERWEAPON MISSILES' VISUAL FLIGHT, with no engine in it so
// scripts/test/missile-visual-test.cpp can drive it standalone against a replica of the logic flight.
//
// WHAT IT IS FOR. The nuke (NeutronMissileUpdate) and the SCUD Storm missiles (MissileAIUpdate) fly in GAME LOGIC along
// a path that is simple to simulate and wrong to look at: straight up, an instant corner, a flat line at ~500 height,
// another corner and a straight drop. The simulation must not change (it is in the SimID digest and every peer runs it),
// so the client DRAWS the missile along a different path: a real ballistic flight (vertical rise, pitch program, boost,
// a coasting arc under gravity, a steep dive), time-compressed onto the logic's own launch-to-death timeline.
//   V = lerp( C(u), P, b )
//   C(u)  the canonical flight below, mapped from the launch point L to the logic death point E;
//   u     the fraction of the flight flown, advanced by mvStepU from the logic's own remaining-time estimate;
//   P     the logic position; b the terminal blend, 1 by the time the logic dies, so the missile is drawn exactly where
//         the simulation detonates it, on the frame it does.
//
// THE PHYSICS (sources). A 2D point mass, gravity 1: thrust along a
// pitch program (vertical, then a linear pitch-over to `pitchEnd` at burnout) with the thrust-to-weight ratio growing as
// propellant burns (rocket equation mass loss), then a coast under gravity, with drag in an exponential atmosphere. It is
// integrated once per profile and normalised to range 1, time 1. The profiles were tuned (proto in the missiles folder)
// to a lofted ICBM (apogee 0.6 range, ~67 deg dive, 16% boost) that coasts in vacuum - drag is zero because an ICBM
// spends almost its whole flight above the atmosphere, and a drag term there measurably changed nothing - and a lower
// Scud-like flight (apogee 0.4 range, ~61 deg, 21% boost) that flies in the air, where drag steepens the dive from ~56
// to ~61 deg as it does on a real re-entry. Real minimum-energy flights top out near range/4 and come in shallower; the
// lofted shape is what reads as "a ballistic missile" from a game camera. The vertical rise is ~5% of the flight, longer
// than a real one (~1-2%) so it can be seen.
#pragma once

#include <math.h>

struct MVVec
{
	float x, y, z;
};

inline MVVec mvMake( float x, float y, float z ) { MVVec v; v.x = x; v.y = y; v.z = z; return v; }
inline MVVec mvSub( const MVVec &a, const MVVec &b ) { return mvMake( a.x - b.x, a.y - b.y, a.z - b.z ); }
inline MVVec mvLerp( const MVVec &a, const MVVec &b, float t ) { return mvMake( a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t ); }
inline float mvLen( const MVVec &a ) { return sqrtf( a.x * a.x + a.y * a.y + a.z * a.z ); }
inline float mvClamp01( float t ) { return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t); }
inline float mvSmooth( float t ) { t = mvClamp01( t ); return t * t * (3.0f - 2.0f * t); }

struct MVProfile
{
	float burnTime;				///< thrust duration (gravity-normalised time)
	float verticalTime;		///< straight-up part of the burn
	float pitchEnd;				///< thrust angle above horizontal at burnout, radians
	float thrust0;				///< thrust / weight at liftoff
	float massFraction;		///< propellant fraction burnt by burnout (thrust/mass grows by 1/(1-f))
	float drag;						///< drag coefficient at sea level
	float scaleHeight;		///< atmosphere scale height
	float smoothing;			///< 1: thrust spools up and tails off and the pitch eases in and out; 0: the step thrust and constant pitch rate the flight had at first (the probe's RED arm)
};

// GeneralsX: the thrust spools up over the
// first 20% of the burn from 60% of full and tails off over the last 30% (real motors do; a step to full thrust at
// liftoff and a step to none at burnout were visible as a jump in the puff spacing), and the pitch program eases in and
// out (smoothstep) instead of starting at a constant rate. The thrust and burnout angle were re-tuned so the flight
// keeps its shape (nuke apogee 0.61 of range, ~67 deg dive; SCUD 0.41, ~61 deg): the acceleration now changes ~9x more
// gently per step (nuke 6.8 -> 0.75) with no discontinuity.
inline MVProfile mvProfileNuke() { MVProfile p = { 1.6f, 0.6f, 24.0f * 3.14159265f / 180.0f, 2.2f, 0.7f, 0.0f, 0.8f, 1.0f }; return p; }
inline MVProfile mvProfileScud() { MVProfile p = { 2.5f, 0.5f, 18.0f * 3.14159265f / 180.0f, 3.0f, 0.7f, 0.15f, 3.0f, 1.0f }; return p; }

static const int MV_TABLE_STEPS = 256;

/// The canonical flight sampled at MV_TABLE_STEPS+1 uniform times: x from 0 to 1, z from 0 back to 0 (range = 1).
struct MVTable
{
	float x[MV_TABLE_STEPS + 1];
	float z[MV_TABLE_STEPS + 1];
	bool valid;
};

/// One integration step of the canonical flight (semi-implicit Euler, 2 ms). Returns false once it is back on the
/// ground after burnout, with *x, *z, *tm set to the interpolated touchdown.
inline bool mvStep( const MVProfile &p, double *x, double *z, double *vx, double *vz, double *tm )
{
	const double dt = 0.002;
	double ax = 0.0, az = 0.0;
	if( *tm < p.burnTime )
	{
		double th = 1.5707963267948966;
		if( *tm >= p.verticalTime )
		{
			const double q = (*tm - p.verticalTime) / (p.burnTime - p.verticalTime);
			th += (p.pitchEnd - th) * (q + p.smoothing * (q * q * (3.0 - 2.0 * q) - q));
		}
		const double s = *tm / p.burnTime;
		const double spool = s < 0.2 ? s / 0.2 : 1.0, taper = s > 0.7 ? (s - 0.7) / 0.3 : 0.0;
		const double env = 1.0 + p.smoothing * ((0.6 + 0.4 * spool * spool * (3.0 - 2.0 * spool)) * (1.0 - taper * taper * (3.0 - 2.0 * taper)) - 1.0);
		const double a = env * p.thrust0 / (1.0 - p.massFraction * *tm / p.burnTime);
		ax = a * cos( th );
		az = a * sin( th );
	}
	const double v = sqrt( *vx * *vx + *vz * *vz );
	const double rho = exp( -(*z > 0.0 ? *z : 0.0) / p.scaleHeight );
	ax -= p.drag * rho * v * *vx;
	az -= p.drag * rho * v * *vz + 1.0;
	*vx += ax * dt;
	*vz += az * dt;
	const double nx = *x + *vx * dt, nz = *z + *vz * dt;
	if( nz < 0.0 && *tm > p.burnTime )
	{
		const double f = *z / (*z - nz);
		*x += f * (nx - *x);
		*z = 0.0;
		*tm += f * dt;
		return false;
	}
	*x = nx; *z = nz; *tm += dt;
	return true;
}

inline void mvBuildTable( const MVProfile &p, MVTable *t )
{
	// Two passes over the same deterministic integration, so no buffer is needed: the first finds the flight time and
	// range, the second samples it at uniform times. ~5000 steps each, once per profile.
	const int MAX_STEPS = 200000;
	double x = 0.0, z = 0.0, vx = 0.0, vz = 0.0, tm = 0.0;
	int n = 0;
	t->valid = false;
	while( mvStep( p, &x, &z, &vx, &vz, &tm ) )
		if( ++n >= MAX_STEPS )
			return;
	const double T = tm, X = x;
	if( !(T > 0.0) || !(X > 0.0) )
		return;
	double px = 0.0, pz = 0.0, pt = 0.0;
	x = z = vx = vz = tm = 0.0;
	bool flying = true;
	for( int i = 0; i <= MV_TABLE_STEPS; ++i )
	{
		const double tt = T * i / MV_TABLE_STEPS;
		while( flying && tm < tt )
		{
			px = x; pz = z; pt = tm;
			flying = mvStep( p, &x, &z, &vx, &vz, &tm );
		}
		const double f = (tm > pt && tt < tm) ? (tt - pt) / (tm - pt) : 1.0;
		t->x[i] = (float)((px + f * (x - px)) / X);
		t->z[i] = (float)((pz + f * (z - pz)) / X);
	}
	t->x[0] = 0.0f; t->z[0] = 0.0f;
	t->x[MV_TABLE_STEPS] = 1.0f; t->z[MV_TABLE_STEPS] = 0.0f;
	t->valid = true;
}

/// Catmull-Rom through the samples, so the drawn velocity (and the nose that follows it) has no kinks at sample edges.
inline void mvEval( const MVTable &t, float u, float *cx, float *cz )
{
	const float s = mvClamp01( u ) * MV_TABLE_STEPS;
	int i = (int)s;
	if( i > MV_TABLE_STEPS - 1 )
		i = MV_TABLE_STEPS - 1;
	const float f = s - (float)i;
	const int i0 = i > 0 ? i - 1 : 0, i2 = i + 1, i3 = (i + 2 <= MV_TABLE_STEPS) ? i + 2 : MV_TABLE_STEPS;
	const float f2 = f * f, f3 = f2 * f;
	const float w0 = -0.5f * f3 + f2 - 0.5f * f, w1 = 1.5f * f3 - 2.5f * f2 + 1.0f;
	const float w2 = -1.5f * f3 + 2.0f * f2 + 0.5f * f, w3 = 0.5f * f3 - 0.5f * f2;
	*cx = w0 * t.x[i0] + w1 * t.x[i] + w2 * t.x[i2] + w3 * t.x[i3];
	*cz = w0 * t.z[i0] + w1 * t.z[i] + w2 * t.z[i2] + w3 * t.z[i3];
}

/// The canonical flight carried from L to E. Its vertical scale is the ground range clamped to [hmin, hmax]: a short
/// shot is lofted (real short-range shots are), a very long one is flattened so its apogee stays drawable.
inline MVVec mvWorld( const MVTable &t, const MVVec &L, const MVVec &E, float hmin, float hmax, float u )
{
	float cx, cz;
	mvEval( t, u, &cx, &cz );
	const float dx = E.x - L.x, dy = E.y - L.y;
	const float D = sqrtf( dx * dx + dy * dy );
	const float hx = D > 1.0f ? dx / D : 0.0f, hy = D > 1.0f ? dy / D : 0.0f;
	float H = D < hmin ? hmin : D;
	if( H > hmax )
		H = hmax;
	return mvMake( L.x + hx * D * cx, L.y + hy * D * cx, L.z + H * cz + (E.z - L.z) * cx );
}

/// One logic frame of flight time: u moves the remaining (1 - u) at the rate the logic says is left, so an estimate
/// that is off only changes the pace, never the landing, and u is exactly 1 once the logic has a frame or less to go.
inline float mvStepU( float u, float tauFrames )
{
	return u + (1.0f - u) / (tauFrames > 1.0f ? tauFrames : 1.0f);
}

/// Where a missile first seen mid-flight (a loaded save) starts: the elapsed share of the estimated whole.
inline float mvInitU( float elapsedFrames, float tauFrames )
{
	const float whole = elapsedFrames + (tauFrames > 0.0f ? tauFrames : 0.0f);
	return whole > 0.0f ? mvClamp01( elapsedFrames / whole ) : 0.0f;
}

/// One logic frame: advance u, then draw at the canonical flight pulled onto the logic position by `blend`.
inline MVVec mvAdvance( const MVTable &t, float *u, const MVVec &L, const MVVec &E, const MVVec &P, float tauFrames,
	float blend, float hmin, float hmax )
{
	*u = mvStepU( *u, tauFrames );
	return mvLerp( mvWorld( t, L, E, hmin, hmax, *u ), P, mvClamp01( blend ) );
}

/// Blend from the logic missile's own height above the height it would die at HERE (HeightDieUpdate's threshold,
/// structures included when its data says so): 1 at or below `margin` above it - a missile that low can detonate this
/// frame - easing to 0 at `band` above it. Continuous and direction-free on purpose: a SCUD climbing
/// off its rail, or out of the band after skimming a structure, eases off the logic path instead of popping.
inline float mvHeightBlend( float heightAboveDie, float band, float margin )
{
	if( heightAboveDie <= margin )
		return 1.0f;
	if( !(band > margin) )
		return 0.0f;
	return mvSmooth( (band - heightAboveDie) / (band - margin) );
}

/// Proximity to something that can detonate the missile, closing at `closing` units per logic frame (>= 0). A contact
/// cannot be predicted, only approached, and it can land between two frames, so the blend is 1 while the gap is within
/// two frames of that approach (mvProxLead): on the frame a collision detonates it, the missile is already drawn on the
/// logic. It starts `band` or `rampFrames` frames of the approach before that, whichever is longer,
/// so a fast approach is a short glide onto the logic path, never a snap.
inline float mvProxLead( float closing ) { return 2.0f * (closing > 0.0f ? closing : 0.0f) + 1.0f; }
inline float mvProxReach( float closing, float band, float rampFrames )
{
	const float c = closing > 0.0f ? closing : 0.0f;
	return mvProxLead( c ) + (rampFrames * c > band ? rampFrames * c : band);
}
inline float mvProxBlend( float gap, float closing, float band, float rampFrames )
{
	return mvHeightBlend( gap, mvProxReach( closing, band, rampFrames ), mvProxLead( closing ) );
}

// ------------------------------------------------------------------------------------------------------------------
// The nuke. A plain copy of the NeutronMissileUpdate state the estimate needs (MissileVisual.cpp reads it through the
// module's own save path) and the constants of its flight, all in logic units per logic frame.
struct MVNukeState
{
	MVVec pos;
	MVVec intermed;					///< target + TargetFromDirectlyAbove: the logic flies here, then drops straight down
	float noTurnDistLeft;		///< > 0 while still rising
	bool reachedIntermed;		///< dropping
	float framesSinceLaunch;
};

struct MVNukeFlight
{
	float cruiseSpeed;			///< RelativeSpeed / ForwardDamping (terminal speed), units per logic frame
	float specialFrames;		///< SpecialSpeedTime in logic frames: the forced vertical rise
	float specialHeight;		///< SpecialSpeedHeight
	float launchZ;					///< height at launch
	float dieZ;							///< the logic dies moving down below this (HeightDieUpdate), at the target
};

/// Logic frames the nuke has left to fly: rise, straight line to the drop point, drop at half speed to dieZ.
inline float mvNukeTau( const MVNukeState &s, const MVNukeFlight &f )
{
	const float drop = f.cruiseSpeed * 0.5f;
	if( s.reachedIntermed )
		return (s.pos.z > f.dieZ ? s.pos.z - f.dieZ : 0.0f) / drop;
	const float dropFrames = (s.intermed.z > f.dieZ ? s.intermed.z - f.dieZ : 0.0f) / drop;
	if( s.noTurnDistLeft > 0.0f )
	{
		// Still going up: the forced rise runs its time, the rest of the no-turn distance climbs at ~0.6 cruise.
		const float specialLeft = f.specialFrames > s.framesSinceLaunch ? f.specialFrames - s.framesSinceLaunch : 0.0f;
		const float forcedTop = f.launchZ + f.specialHeight;
		const float forcedLeft = forcedTop > s.pos.z ? forcedTop - s.pos.z : 0.0f;
		const float climbLeft = s.noTurnDistLeft > forcedLeft ? s.noTurnDistLeft - forcedLeft : 0.0f;
		const MVVec corner = mvMake( s.pos.x, s.pos.y, s.pos.z + s.noTurnDistLeft );
		return specialLeft + climbLeft / (0.6f * f.cruiseSpeed) + mvLen( mvSub( s.intermed, corner ) ) / f.cruiseSpeed + dropFrames;
	}
	return mvLen( mvSub( s.intermed, s.pos ) ) / f.cruiseSpeed + dropFrames;
}

/// The terminal blend of the nuke: none. (It used to blend over the whole ~375-unit drop, dragging the drawn missile off
/// its steep arc onto the logic's slow vertical drop - - and any shorter blend still had to
/// brake it from the dive speed to the logic's drop speed in a few frames.) The drawn flight ends AT the target, at the
/// height the logic dies at (`E`), on the frame the logic dies: it lands at an angle, at speed, and matches the logic to
/// within one drop step (a few units, less than the frame's own travel). Only the proximity blend can still pull it onto
/// the logic. The function stays so the callers and the probe read the same way for the SCUD and the nuke.
inline float mvNukeBlend( const MVNukeState &, const MVNukeFlight & )
{
	return 0.0f;
}
