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

// GeneralsX: WS-X2: DrawRun - DXVK draws a claimed run of classic draws itself, at the run's own
// position, through its own d3d9 entry points (the ABI in prose).
//
// GX_L2_DRAWRUN unset (default): nothing runs - no QueryInterface, no Device_Call, no arena. Render-only: nothing here is read by
// the simulation.
#pragma once

#include <d3d8.h>

namespace GxDrawRun
{
	// Byte-identical to the fork's DxvkGxLightSet / DxvkGxTask (d3d9_interfaces.h; D3DLIGHT8 has D3DLIGHT9's layout). The
	// semantics of every field.
	struct LightSet
	{
		UINT enableMask;			// bit i (i < 4): slot i enabled, value lights[i]; a clear bit: LightEnable(i, FALSE)
		UINT reserved;				// 0
		D3DLIGHT8 lights[4];
	};

	struct Task
	{
		D3DMATRIX world;			// D3DTS_WORLD at the draw
		DWORD ambient;				// D3DRS_AMBIENT at the draw
		UINT lightSet;				// index into the run's light sets
		UINT normalizeNormals;		// 1: NORMALIZENORMALS TRUE for this draw only, FALSE right after (the classic object-scale bracket)
		INT baseVertex;				// the d3d8 SetIndices base vertex at the draw (index_base_offset + vba_offset)
		UINT minIndex;
		UINT numVertices;
		UINT startIndex;			// incl. the IB allocation offset
		UINT primCount;
	};

	bool Enabled();					// GX_L2_DRAWRUN=1, read once

	// Main thread, before claiming: true once the fork's DrawRun answered on the current device. The first call queues the
	// check in stream order (DX8Wrapper::Device_Call), so it reads false until that ran; false for the whole session when the
	// fork lacks the extension (an unpatched DXVK, the d3d8 batcher on) or GX_L2_DRAWRUN is unset.
	bool Available();

	// Main thread, at a claimed run's position (after WS-G's PREPARE): copies the run into this frame's arena and records ONE
	// Device_Call. On the device thread the fork's DrawRun draws it; if the fork refuses, the same per-task calls go through the
	// d3d8 device instead - a run is never lost and the D3D stream is the same either way.
	void Draw_Run(D3DPRIMITIVETYPE type, const Task *tasks, unsigned taskCount, const LightSet *sets, unsigned setCount);

	// Main thread, WS-G's per-claim question: replace claimed runs with Draw_Run in THIS frame? Available(), and with the gate's toggle
	// (GX_DRAWRUN_TOGGLE=k) only in its ON periods - the classic arm of the same run draws the OFF periods. Also the
	// toggle's frame clock: call it at least once per render frame while claiming.
	bool S1_On();

	// Main thread, in stream order: opens (begin) or closes a C2b input-digest bracket on the device thread - every draw DXVK
	// commits inside it is hashed and the close prints "[GXDIGEST] tag=<tag> draws= seq=" (fork switch DXVK_GX_INPUT_DIGEST; a
	// no-op without it or without the fork). With GX_DRAWRUN_DIGEST=1, Draw_Run brackets each run itself with tag
	// (path << 32 | run index in the frame), path 1 = the fork's DrawRun, 2 = its fallback; the classic arm of an S1 toggle
	// brackets its run with path 0 and the same index. Digest_Mark replaces bits 40-63 of the tag with the render frame (mod 2^24):
	// the printed tag = frame << 40 | path << 32 | run index.
	void Digest_Mark(bool begin, unsigned long long tag);

	// The carry-out on EXECUTED device state. What DX8Wrapper's caches say the device holds; with
	// GX_DRAWRUN_TRUTH=1 the S1 seat passes it right after its CARRY-OUT's Apply - and in OFF frames at the same point after the
	// classic run's last task (the A/A arm, must be clean) - and the device thread compares it with the d3d8 device's own answers
	// (GetIndices, GetTransform(WORLD), GetRenderState(AMBIENT / NORMALIZENORMALS), GetLightEnable / GetLight for slots 0-3).
	// [GXDRUN] truth_checked_off= truth_mismatch_off= truth_checked_on= truth_mismatch_on= (by the tag's path: 0 = OFF, else ON);
	// the first mismatches print [GXDTRUTH] with the field. A skipped carry-out leaves the
	// wrapper on PREPARE's (first task's) values while the device holds the last task's, so it fires on any run whose tasks differ.
	// Coherence alone cannot see a carry-out that COHERENTLY applies the wrong task, so an ON check also carries the ORACLE: the
	// classic final state taken from the claim itself (the run's last task and its light set, the run's IB), independent of the
	// wrapper's caches - device AND wrapper must equal it. OFF checks (at the seat's next defined point, other draws in between)
	// carry no oracle (hasOracle = 0): coherence only.
	struct Truth
	{
		IDirect3DIndexBuffer8 *ib;	// the wrapper's current index buffer (null = it believes none is bound; compared as such)
		UINT baseVertex;			// and base vertex (index_base_offset + vba_offset)
		D3DMATRIX world;
		DWORD ambient;
		DWORD normalizeNormals;
		UINT lightEnableMask;		// bit i (i < 4): slot i enabled; lights[i] is compared for enabled slots only
		D3DLIGHT8 lights[4];
		unsigned long long tag;		// the run's digest tag (path << 32 | run index in the frame), named in [GXDTRUTH]
		UINT wrapperDirty;			// DX8Wrapper's pending change flags right after the Apply: must be 0
		UINT hasOracle;				// 1: the fields below are the run's classic final state and are compared too
		IDirect3DIndexBuffer8 *runIb;	// the run's index buffer (its tasks' base vertices are against it)
		Task last;					// the run's LAST task, as submitted
		LightSet lastSet;			// its light set
		UINT anyScaled;				// informational: classic leaves NORMALIZENORMALS FALSE after EVERY admitted run (the oracle requires FALSE)
	};
	bool Truth_On();				// GX_DRAWRUN_TRUTH=1, read once
	void Check_Truth(const Truth &t);	// main thread, in stream order (a Device_Call); a no-op unless Truth_On()

	// Once, from RTS3DScene's constructor. GX_HYBRID_DRAWRUN_TEST=1|2|3 installs the synthetic identity test (§13.7): one run of
	// lit cubes per frame, =1 through the d3d8 device, =2 through Draw_Run, =3 alternating per frame (odd frames Draw_Run) - the
	// pictures must be identical, and with the digest armed every frame's bracket must hash the same.
	void Install();
}
