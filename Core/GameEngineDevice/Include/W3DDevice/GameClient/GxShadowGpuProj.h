/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 GeneralsX contributors
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

// (R-DL6): the projection the GPU shadow-volume VS gets (gxShadowGpuFlush).
// The volumes are drawn inside the jittered world pass, where the device answers GetTransform(PROJECTION) with the CLEAN matrix, so
// the VS must add the pass's jitter itself, as the trees (W3DTreeBuffer) and water (W3DWater) do. Header-only so gx_native_test can
// check the constants; GX_TEST_NO_SHADOW_JITTER (tests only) removes the call = the X- arm.
#pragma once
#include "GxWorldPass.h"

// proj = a D3D row-vector projection (what GetTransform returns); out[k] = the VS constants c5-c8 (columns of proj).
inline void GxShadowGpuProjConstants(IGxWorldPass *wp, const D3DMATRIX &proj, float out[4][4])
{
	D3DMATRIX p = proj;
#ifndef GX_TEST_NO_SHADOW_JITTER
	GxWorldPassJitterProjection(wp, &p._11);
#else
	(void)wp;
#endif
	const float c[4][4] =
	{
		{ p._11, p._21, p._31, p._41 }, { p._12, p._22, p._32, p._42 },
		{ p._13, p._23, p._33, p._43 }, { p._14, p._24, p._34, p._44 },
	};
	for (int i = 0; i < 4; ++i)
		for (int k = 0; k < 4; ++k)
			out[i][k] = c[i][k];
}
