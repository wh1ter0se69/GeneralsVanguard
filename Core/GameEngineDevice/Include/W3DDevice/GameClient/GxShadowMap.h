// GeneralsX: S7 GPU sun shadow map: the native device's private shadow-map interface
// (modernize/look-director/d2/PREREG_look-shadowmaps.md; PLAN_look-shadowmaps.md Task 4).
//
// Game code asks the device for it once per device with QueryInterface(IID_GxShadowMap). DXVK, the GXCAP proxy, and the native device
// with the key OFF (GXShadowMap = off, read once at device init: PREREG A18) answer E_NOINTERFACE - and then the game draws today's
// stencil volumes and blobs exactly as before. With the key ON the device answers it, and the game:
//   * once per tactical frame, before any world draw: BeginShadowFrame (the map's sun, fit, colour and step);
//   * around draws: SetShadowTags (only on change) - CAST on a caster's draws, CAST_ONLY around the early off-screen caster flush
//     (PREREG A23/A31), RECEIVE from the scene start to the 2003 stencil-shadow boundary (A29: never on the team-colour overlays).
//
// Front-end contract (rw3-X, A23-B): every method is an async device command with owned arguments, stream-ordered with the draws
// around it, executed in the device worker's exec half - S7 never records from the caller, so the A23-B worker stays on (PREREG 1.4).
// Render-only: nothing here is read by the simulation, and no value comes back to the game.
#pragma once

#include <d3d8.h>
#include <stdint.h>

// {3F9A6C21-7B4E-4D58-9E13-5A0C8B2D47F6}
static const GUID IID_GxShadowMap = { 0x3f9a6c21, 0x7b4e, 0x4d58, { 0x9e, 0x13, 0x5a, 0x0c, 0x8b, 0x2d, 0x47, 0xf6 } };

// The tags (the device's Rec::kShadowTag* carry the same values; a static_assert in the device keeps them equal).
enum GxShadowTag : uint32_t
{
	GX_SHADOW_TAG_NONE = 0,
	GX_SHADOW_TAG_RECEIVE = 1,		// the draws that follow sample the map (P2)
	GX_SHADOW_TAG_CAST = 2,			// ... and are captured as casters
	GX_SHADOW_TAG_CAST_ONLY = 4,	// ... are never drawn; with CAST (and z test + z write, the drawn path's rule) captured, else dropped (A44)
	GX_SHADOW_TAG_SOLID = 8,		// A61: with CAST_ONLY + CAST, captured without the camera's z write (a sorted CAST_SHADOW caster - a rotor
									// disc - that 2003's volume casts solid). Read only under CAST_ONLY; the drawn path ignores it.
};

// One frame's map. Matrices are D3D row-vector (_11 _12 ... ; v' = v * M) with Vulkan clip space (z 0..1, the atlas rect's y down).
struct GxShadowFrame
{
	float lightViewProj[2][16];		// per cascade: world -> the cascade's clip space
	uint32_t rect[2][4];			// per cascade: the atlas rectangle (texels) x, y, w, h - the step's sub-rectangle (A14 guard inside)
	float depthBias[3];				// constant, clamp, slope (the step's registered bias, A14)
	uint32_t shadowColor;			// ARGB, the map's (WorldHeightMap's lighting chunk; W3DShadowManager::getShadowColor)
	uint32_t step;					// 0..3 (3 = off: no capture, no pass; the game draws the stencil volumes)
	uint32_t cascades;				// 0, 1 or 2
	// P2 (D · look-shadowmaps-2): what the receivers need beyond the replay (the device packs them into set 2's frame UBO)
	float depthRangeWU[2];			// per cascade: the light depth range in world units (the fit's zFar - zNear)
	float texelWU[2];				// per cascade: one texel in world units (extent / res; at the axis centre since A39)
	uint32_t guard;					// the step's guard band, texels per side
	// A39 (D · look-shadowmaps-3): the map is a PERSPECTIVE projection from 2003's point light L (lightViewProj carries it)
	float tanTexel[2];				// per cascade: one texel in tangent units (x the axis distance from L = world units)
	float nearFar[2][2];			// per cascade: near / far axis distances from L (the receivers linearise depth with them)
	// A79 (E · look-shadowmaps-7): the own-light pull (GXRender Record/GxOwnPull.h) - L.xyz and eps (0 = no pull); muMax, noRecv (TEST-ONLY:
	// gx_native_test's frame only - the game writes 0 always), 0, 0
	float ownPull[4];
	float ownPullAux[4];
};

struct IGxShadowMap : public IUnknown
{
	virtual HRESULT STDMETHODCALLTYPE BeginShadowFrame(const GxShadowFrame &f) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetShadowTags(uint32_t tags) = 0;
	// A41: a clamped instance's own light for the draws that follow, ONLY on change (0 = none). packed = mu's float bits (low 32) |
	// z0's float bits (high 32): its casters are captured with World x S (z' = z0 + mu (z - z0)) and its receivers look the map up
	// at that scaled position (GXShadowMapFit.h ownLight). Stream-ordered with the draws, like the tags.
	virtual HRESULT STDMETHODCALLTYPE SetOwnLight(uint64_t packed) = 0;
};
