// GeneralsX @feature How far back the camera must be for the whole map to be on screen. Pure and header-only so
// scripts/test/whole-map-view-test.cpp can check it without the engine. Client only: the view is not simulated.
#pragma once

#include <math.h>

// Is every corner of a 2*halfX by 2*halfY rectangle on the ground, centred on the point the camera looks at, inside
// the view of a camera dist away from that point at pitch (radians below the horizon) and yaw (radians about the
// vertical, 0 = looking along +Y)? tanX / tanY are the tangents of the half fields of view, as in
// CameraClass::Get_View_Plane's max corner.
inline bool gxWholeMapInView( float halfX, float halfY, float pitch, float yaw, float dist, float tanX, float tanY )
{
	const float cp = cosf( pitch ), sp = sinf( pitch ), cy = cosf( yaw ), sy = sinf( yaw );
	const float f[3] = { -sy * cp, cy * cp, -sp };	// forward
	const float r[3] = { cy, sy, 0.0f };				// right
	const float u[3] = { -sy * sp, cy * sp, cp };		// up
	for( int i = 0; i < 4; ++i )
	{
		// corner minus camera; the camera sits at -f * dist
		const float v[3] = { ( ( i & 1 ) ? halfX : -halfX ) + f[0] * dist, ( ( i & 2 ) ? halfY : -halfY ) + f[1] * dist, f[2] * dist };
		const float z = v[0] * f[0] + v[1] * f[1] + v[2] * f[2];
		if( z <= 0.0f )
			return false;
		if( fabsf( v[0] * r[0] + v[1] * r[1] ) > tanX * z )
			return false;
		if( fabsf( v[0] * u[0] + v[1] * u[1] + v[2] * u[2] ) > tanY * z )
			return false;
	}
	return true;
}

// How far from the point the camera looks at the bottom edge of the screen meets the ground, for a camera dist away at
// pitch: what W3DView::calcCameraAreaOffset measures, from the centre ray's ground point to the bottom edge's.
inline float gxBottomEdgeGroundDistance( float pitch, float dist, float tanY )
{
	const float cp = cosf( pitch ), sp = sinf( pitch );
	const float down = sp + tanY * cp;	// how steeply the bottom edge's ray falls
	if( down <= 0.0f )
		return 1.0e6f;					// at or above the horizon, as calcCameraAreaOffset answers
	const float t = sp * dist / down;
	return fabsf( cp * dist - t * ( cp - tanY * sp ) );
}

// Is the map whole on screen from EVERY pivot the area constraints allow at that distance? They keep the pivot
// offsetScale x that ground distance plus offsetBias inside each edge, capped at the shorter half side
// (calcCameraAreaConstraints), so it can sit up to (half side - offset) off the middle on each axis; the map is whole
// from all of them when the rectangle grown by that slack is whole from the middle.
inline bool gxWholeMapInViewFromAnyPivot( float halfX, float halfY, float pitch, float yaw, float dist, float tanX, float tanY,
	float offsetScale, float offsetBias )
{
	float offset = offsetScale * gxBottomEdgeGroundDistance( pitch, dist, tanY ) + offsetBias;
	if( offset > halfX )
		offset = halfX;
	if( offset > halfY )
		offset = halfY;
	if( offset < 0.0f )
		offset = 0.0f;
	return gxWholeMapInView( 2.0f * halfX - offset, 2.0f * halfY - offset, pitch, yaw, dist, tanX, tanY );
}

// The least such distance, to a part in 10^4. 0 for an empty map or view; -1 when even maxDist does not show it all.
inline float gxWholeMapDistance( float halfX, float halfY, float pitch, float yaw, float tanX, float tanY, float maxDist,
	float offsetScale, float offsetBias )
{
	if( !( halfX > 0.0f ) || !( halfY > 0.0f ) || !( tanX > 0.0f ) || !( tanY > 0.0f ) )
		return 0.0f;
	if( !gxWholeMapInViewFromAnyPivot( halfX, halfY, pitch, yaw, maxDist, tanX, tanY, offsetScale, offsetBias ) )
		return -1.0f;
	float lo = 0.0f, hi = maxDist;
	for( int i = 0; i < 64 && hi - lo > hi * 1.0e-4f; ++i )
	{
		const float mid = 0.5f * ( lo + hi );
		if( gxWholeMapInViewFromAnyPivot( halfX, halfY, pitch, yaw, mid, tanX, tanY, offsetScale, offsetBias ) )
			hi = mid;
		else
			lo = mid;
	}
	return hi;
}
