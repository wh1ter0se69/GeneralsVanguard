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

// The state one classic draw uses, as the device truth holds it (dx8wrapper.h GxTruth*), and the comparison of two such
// states under the contract's relevance mask - the classic draw's against L2's prediction for it. The mask generator is
// checked against a hand-written table.

#pragma once

#include <d3d8.h>
#include <cstdint>

class LightEnvironmentClass;

struct GxDrawState
{
	unsigned rs[256]; bool rsKnown[256];			// the device truth, not the wrapper's cache
	// All 8 stages (MAX_TEXTURE_STAGES) are kept; the comparison still covers stages 0-3 (two
	// per material (MAX_TEX_STAGES) + ShaderClass::Apply's raw stage 2)
	unsigned tss[8][32]; bool tssKnown[8][32];
	const void *tex[8];
	D3DMATERIAL8 mat; bool matKnown;
	D3DLIGHT8 light[4]; bool lightOn[4], lightValid[4];	// lightValid: DX8Wrapper::CurrentDX8LightValid (T3)
	float world[16], view[16], proj[16]; bool worldKnown, viewKnown, projKnown;	// world/view: DX8Wrapper::GxXformValid
	float texm[8][16]; bool texmKnown[8];
	unsigned fvf; const void *vb, *ib; unsigned baseVertex, indexBase, vbaOffset, ibaOffset;	// baseVertex = indexBase + vbaOffset
	unsigned prim, minIndex, numVerts, startIndex, primCount;
};

void GxL2Check_Capture(GxDrawState &s, unsigned prim, unsigned minIndex, unsigned numVerts, unsigned startIndex, unsigned primCount);

// C2: every relevant field of `classic` vs `l2`. Returns the number of differing fields; the first `printMax` are written as
// "[GXL2CHK] ... kind=STATE field=<f> classic=<v> l2=<v>" with the given prefix. An unknown classic slot that the pipeline reads is
// a difference (C2).
void GxL2Check_PrintFieldHist();	// "[GXL2CHK] state_fields=<field>:<n>,..." - every C2 mismatch so far, by field
int GxL2Check_CompareState(const GxDrawState &classic, const GxDrawState &l2, const char *prefix, int printMax);
int GxL2Check_CompareStateM(const GxDrawState &classic, const GxDrawState &l2, const char *prefix, int printMax, int mutant);

// The render / texture-stage states DXVK's fixed-function pipeline reads for a triangle draw (d3d9_fixed_function.cpp, d3d9_state.h):
// what E2 requires KNOWN and what C2 compares (subject to the mask).
extern const unsigned kGxFfRs[];
extern const int kGxFfRsN;
extern const unsigned kGxFfTss[];
extern const int kGxFfTssN;

// C8: the relevance-mask generator vs a hand-written expected table; returns the number of disagreeing slots (0 = PASS).
int GxL2Check_MaskOracle(int mutant);

// L2's own copy of Set_Light_Environment's per-light construction (dx8wrapper.cpp, verbatim): kept separate from the classic code
// on purpose, so C2's lights field compares two implementations.
void GxL2Check_Light(const LightEnvironmentClass &env, int l, D3DLIGHT8 &light);
