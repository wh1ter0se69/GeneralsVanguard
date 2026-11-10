// GeneralsX: The object-velocity bridge (A35, modernize/render-dlss/
// ): per-object motion vectors for moved rigid meshes and CPU-skinned meshes, until the MRT path replaces it
// after A22 + A25 (then this file is DELETED, not kept as a second path). Native renderer only.
//
// The world pass's one ordered sequence (ruling 01:08Z), all in W3DWorldSeam::push/pop:
//   IGxWorldPass::Begin -> GxMotionUnits::Begin -> the world draws -> GxMotionUnits::End -> IGxWorldPass::End (velocity resolve)
// Between Begin and End the bridge records the draws of meshes that moved since the previous world pass (GxMotionHistory.h) and of
// every skinned mesh; End re-draws only those into the object-velocity image O (GXRA, .xy = current -> previous in render pixels,
// .a = written) with the gx:vel-* programs, depth-tested against this frame's world depth, and registers O with the device.
//
// Key: GX_MOTION_UNITS=1 (the device creates the programs from the same variable) and a temporal (1-sample) world pass. Key off:
// both calls return at the first test; no hook is installed, nothing is created. Render-only: nothing here is read by the simulation.
#pragma once

#include <cstdint>

struct IGxWorldPass;
struct GxWorldPassBegin;

namespace GxMotionUnits
{
	// GX_MOTION_UNITS=1, read once at start-up. The seam calls nothing below while it is 0 (key off = no call at all).
	extern const int Key;
	void Begin(IGxWorldPass *wp, const GxWorldPassBegin &b);
	void End();
	// W3DWorldSeam::release (DX8Wrapper::Default_Pool_Release_Hook, before a device reset): releases O and the skin buffers
	// (D3DPOOL_DEFAULT) and forgets every history; an open pass is dropped, never replayed.
	void Release();
}
