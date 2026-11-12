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

// The world/HUD seam's private device interface
//
// W3DWorldSeam asks the device for it once per device with QueryInterface(IID_GxWorldPass). DXVK and the GXCAP proxy answer
// E_NOINTERFACE, and then the seam does everything with plain d3d8 calls (CopyRects resolve, its own bilinear quad). The native
// device answers it. Upscalers (FSR 1 / FSR 3.1 / MetalFX / DLSS), the linear-light MSAA resolve, depth, motion vectors and jitter
// live behind it, so game code never names a backend.
//
// Front-end contract: every method with a body is an async device command with owned arguments, stream-ordered with
// the draws around it, called only from the thread driving the device; anything the game reads back (Supports) is answered from
// front-side state. Render-only: nothing here is read by the simulation.
#pragma once

#include <d3d8.h>
#include <stdint.h>

// {6C1B2E4A-8D35-4F0B-A1C7-2E9F5D3B8A61}
static const GUID IID_GxWorldPass = { 0x6c1b2e4a, 0x8d35, 0x4f0b, { 0xa1, 0xc7, 0x2e, 0x9f, 0x5d, 0x3b, 0x8a, 0x61 } };

enum GxUpscaleKind : uint32_t
{
	GX_UPSCALE_BILINEAR = 0,	// the seam's own quad; never sent to the device
	GX_UPSCALE_FSR1,
	GX_UPSCALE_FSR3,
	GX_UPSCALE_DLSS,
	GX_UPSCALE_METALFX,
	GX_UPSCALE_METALFX_TEMPORAL,
	GX_UPSCALE_XESS,
	GX_UPSCALE_COUNT,
	// not an upscaler: c.src sharpened 1:1 into c.dst by FSR 1's RCAS - the seam's ONE sharpener after a backend that does not
	// sharpen itself; c.srcRect / c.dstRect = the whole textures, c.scratch unused
	GX_UPSCALE_RCAS = 0x100
};

enum GxWorldPassFlags : uint32_t
{
	GXWP_RESET_HISTORY = 1u << 0,	// seam (re)created, render/output size or sample count changed, first frame after OFF
	GXWP_TEMPORAL      = 1u << 1,	// world targets are single-sample; depth must be sampleable
	GXWP_MSAA_COLOUR   = 1u << 2,	// NextTargetsAreWorld only: the next CreateRenderTarget is the MSAA world colour (ResolveLinear samples it)
};

struct GxWorldPassBegin
{
	IDirect3DSurface8 *colour;	// the bound world colour target (an MSAA surface, or the world texture's level 0)
	IDirect3DSurface8 *depth;	// the bound world depth-stencil
	uint32_t renderW, renderH;	// world target size
	uint32_t outputW, outputH;	// the camera viewport's size in the output target (the view rect the world is upscaled into)
	uint32_t flags;				// GxWorldPassFlags
	// The viewport CameraClass::Apply will set for the world camera, in WORLD-TARGET pixels (its truncating math on renderW/H):
	// a sub-rect when the tactical view is shorter than the screen (ControlBar.cpp m_viewportHeightScale).
	uint32_t viewX, viewY, viewW, viewH;
	float minZ, maxZ;			// its depth range (CameraClass ZBufferMin/Max)
	// The world camera, UNJITTERED, exactly as CameraClass::Apply will send it (D3D layout): the device must not guess it from the
	// SetTransform stream, which also carries identity views for particles and streaks (pointgr.cpp:915-923).
	D3DMATRIX view, proj;
	float nearZ, farZ, fovYRad;
};

struct GxUpscaleCall
{
	IDirect3DSurface8 *src;		// the world colour, single-sample (the world texture's level 0)
	uint32_t srcW, srcH;		// src's size (the render size)
	RECT srcRect;				// the camera's viewport in src: the only texels the world pass drew (CameraClass::Apply's math)
	IDirect3DSurface8 *dst;		// where the upscaled world goes: the seam's output texture (level 0), single-sample
	RECT dstRect;				// in dst pixels; its size is the output size (the camera viewport's size in the replaced target)
	IDirect3DSurface8 *scratch;	// a second output-size texture surface for multi-pass backends (FSR 1: EASU writes it, RCAS reads it)
	uint32_t kind;				// GxUpscaleKind
	float sharpness;			// 0..1 (GXUpscaleSharpness / 100): the backend's own sharpener when SharpensItself(kind), else the seam's RCAS
	uint32_t flags;				// GxWorldPassFlags of this frame
};

struct IGxWorldPass : public IUnknown
{
	// Called right before the seam creates its world targets: the next CreateRenderTarget and CreateDepthStencilSurface on this
	// device are the world colour (MSAA) and depth, so the device may give them the usage the world pass will need (SAMPLED on the
	// MSAA colour for ResolveLinear when flags has GXWP_MSAA_COLOUR; SAMPLED depth when flags has GXWP_TEMPORAL). Front-side; each
	// half clears when its target is created; NextTargetsAreWorld(0) disarms both (the seam calls it on a creation failure and release).
	virtual HRESULT STDMETHODCALLTYPE NextTargetsAreWorld(uint32_t flags) = 0;
	// Between Begin and End every draw is a world draw (velocity, jitter, depth for the temporal backends).
	virtual HRESULT STDMETHODCALLTYPE Begin(const GxWorldPassBegin &b) = 0;
	virtual HRESULT STDMETHODCALLTYPE End() = 0;
	// Resolves an MSAA world colour into a single-sample surface in LINEAR light, the way the native present resolves an MSAA back
	// buffer (gx_present_ms.frag), so a scaled world keeps the 1:1 path's edge look. E_NOTIMPL = the caller uses CopyRects.
	virtual HRESULT STDMETHODCALLTYPE ResolveLinear(IDirect3DSurface8 *msaaSrc, IDirect3DSurface8 *dst) = 0;
	// TRUE when Upscale(kind) can run in this session (front-side answer).
	virtual BOOL STDMETHODCALLTYPE Supports(uint32_t kind) = 0;
	// Writes c.src upscaled into c.dstRect of c.dst, sharpened by the backend. E_NOTIMPL / a failure = the caller draws bilinear.
	virtual HRESULT STDMETHODCALLTYPE Upscale(const GxUpscaleCall &c) = 0;
	// The world is composited; from here to Present only HUD draws (a frame generator may snapshot the HUD-less image here).
	virtual HRESULT STDMETHODCALLTYPE HudBegin() = 0;
	// Between Begin and End: the object-velocity target (RGBA16F, A = written) the motion-units bridge draws into; End's
	// resolve reads it. Front-side registration; null / never called = camera-only vectors.
	virtual HRESULT STDMETHODCALLTYPE SetObjectVelocity(IDirect3DSurface8 *o) = 0;
	// The current world pass's projection jitter in NDC, answered from front-side state ((0,0) when none), so a vertex shader whose
	// matrices the game builds itself can apply the same offset.
	virtual HRESULT STDMETHODCALLTYPE GetJitter(float ndc[2]) = 0;
	// TRUE when Upscale(kind) ends in its own sharpener (FSR 1's and FSR 3.1's RCAS): the seam runs no second one. FALSE (DLSS, whose
	// own sharpening is deprecated; MetalFX; XeSS) = the seam runs Upscale(GX_UPSCALE_RCAS) on the output at c.sharpness. Front-side.
	virtual BOOL STDMETHODCALLTYPE SharpensItself(uint32_t kind) = 0;
};

// A vertex shader whose WVP the game builds itself from _Get_DX8_Transform(PROJECTION)
// (trees W3DTreeBuffer.cpp, water waves W3DWater.cpp) must add the world pass's jitter itself; the device answers GetTransform
// clean. m = a D3D row-vector projection (what _Get_DX8_Transform returns). (0,0) jitter or no interface = unchanged.
inline void GxWorldPassJitterProjection(IGxWorldPass *wp, float m[16])
{
	if (wp == nullptr)
		return;
	float j[2] = { 0.0f, 0.0f };
	wp->GetJitter(j);
	if (j[0] == 0.0f && j[1] == 0.0f)
		return;
	for (int r = 0; r < 4; ++r)
	{
		m[r * 4 + 0] += m[r * 4 + 3] * j[0];
		m[r * 4 + 1] += m[r * 4 + 3] * j[1];
	}
}
