// GeneralsX @perf Water cull: may a water patch be skipped for this camera?
//
// WaterRenderObjClass::renderWater walks every water area of the map each frame and builds every patch's vertex grid on the
// CPU, twice with RA3 water, visible or not. On the crater maps that is ~half of every frame.
// This header is the one decision, in plain floats so scripts/test/water-cull-test.cpp checks it without the engine.
//
// A plane is (nx, ny, nz, d); a point p is OUTSIDE it when n.p - d > 0 (the WW3D convention: CollisionMath reports that side as
// OUTSIDE for a FrustumClass plane). A patch may be skipped only when ALL its corners are outside ONE plane by more than the
// padding, so a patch that is even partly in view is drawn whole and nothing pops at the edge of the screen. The padding grows the
// corners' convex hull by mxy in x/y and mz in z, which covers every vertex the patch draws (bilinear blends of its corners, raised
// or lowered in z only by the water's wave and feather layers).

#pragma once

inline bool gxWaterPatchOutside(const float planes[6][4], const float (*pts)[3], int n, float mxy, float mz)
{
	if (n <= 0)
		return false;
	for (int i = 0; i < 6; ++i)
	{
		const float *pl = planes[i];
		const float pad = mxy * ((pl[0] < 0 ? -pl[0] : pl[0]) + (pl[1] < 0 ? -pl[1] : pl[1])) + mz * (pl[2] < 0 ? -pl[2] : pl[2]);
		bool allOut = true;
		for (int k = 0; k < n && allOut; ++k)
			allOut = pl[0] * pts[k][0] + pl[1] * pts[k][1] + pl[2] * pts[k][2] - pl[3] > pad;
		if (allOut)
			return true;
	}
	return false;
}
