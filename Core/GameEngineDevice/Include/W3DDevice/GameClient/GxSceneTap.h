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

// The scene copies' private device interface, taken with QueryInterface(IID_GxSceneTap); DXVK and
// the GXCAP proxy answer E_NOINTERFACE. A reader of the world's depth or opaque colour (soft particles,
// native water) registers by name; at OpaqueDone, an async stream-ordered command, the device takes them.
// Render-only: nothing here is read by the simulation.
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
