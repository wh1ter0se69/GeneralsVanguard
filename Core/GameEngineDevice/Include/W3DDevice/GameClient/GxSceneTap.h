// GeneralsX: F-depth+: the scene copies' private device interface
//
//
// The game asks the device for it once per device with QueryInterface(IID_GxSceneTap); DXVK and the GXCAP proxy answer
// E_NOINTERFACE, and then nothing is taken (classic has no reader). The native device answers it. A reader that draws with the
// world's depth or its opaque colour (soft particles, native water) registers by name; at OpaqueDone the device takes them once per
// frame: single-sample and sampleable, the state after the opaque world (terrain, objects, trees, shadows) and before water and every
// transparent. With no reader on the game makes no OpaqueDone call and the device records nothing - today's frame, exactly.
//
// Independent of the world/HUD seam (GxWorldPass.h): with GXUpscaler off (the default) the world is the back buffer and its depth,
// with it on the seam's targets; the device takes whatever pair is bound at OpaqueDone.
// Front-end contract (rw3-X, A23-B): front-side state; OpaqueDone is an async device command, stream-ordered with the draws around
// it. Render-only: nothing here is read by the simulation.
#pragma once

#include <d3d8.h>
#include <stdint.h>

// {CB20AB0E-0482-4F56-89A4-CA0DFE1E1FE3}
static const GUID IID_GxSceneTap = { 0xcb20ab0e, 0x0482, 0x4f56, { 0x89, 0xa4, 0xca, 0x0d, 0xfe, 0x1e, 0x1f, 0xe3 } };

enum GxSceneNeed : uint32_t
{
	GX_SCENE_DEPTH  = 1u << 0,	// the world depth (DEPTH aspect, sample 0 of a multisample depth)
	GX_SCENE_COLOUR = 1u << 1,	// the opaque world colour (a multisample colour resolved by averaging)
};

struct IGxSceneTap : public IUnknown
{
	// A reader's needs (GxSceneNeed bits; 0 = off). name = the reader's key, a string the caller keeps alive (a literal). Any time;
	// a change takes effect at the next OpaqueDone.
	virtual HRESULT STDMETHODCALLTYPE SetReader(const char *name, uint32_t needs) = 0;
	// TRUE when a reader is on: the caller makes the OpaqueDone call only then. Front-side.
	virtual BOOL STDMETHODCALLTYPE Wanted() = 0;
	// The opaque world is complete in the bound colour target and depth-stencil. Once per frame (a second call before Present is
	// ignored); the copies stay valid and sampleable until Present.
	virtual HRESULT STDMETHODCALLTYPE OpaqueDone() = 0;
};
