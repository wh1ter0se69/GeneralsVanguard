/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
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

/***********************************************************************************************
 ***              C O N F I D E N T I A L  ---  W E S T W O O D  S T U D I O S               ***
 ***********************************************************************************************
 *                                                                                             *
 *                 Project Name : WW3D                                                         *
 *                                                                                             *
 *                     $Archive:: /Commando/Code/ww3d2/dx8wrapper.cpp                         $*
 *                                                                                             *
 *              Original Author:: Jani Penttinen                                               *
 *                                                                                             *
 *                      $Author:: Kenny Mitchell                                               *
 *                                                                                             *
 *                     $Modtime:: 08/05/02 1:27p                                              $*
 *                                                                                             *
 *                    $Revision:: 170                                                         $*
 *                                                                                             *
 * 06/26/02 KM Matrix name change to avoid MAX conflicts                                       *
 * 06/27/02 KM Render to shadow buffer texture support														*
 * 06/27/02 KM Shader system updates																				*
 * 08/05/02 KM Texture class redesign
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 *   DX8Wrapper::_Update_Texture -- Copies a texture from system memory to video memory        *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

//#define CREATE_DX8_MULTI_THREADED
//#define CREATE_DX8_FPU_PRESERVE
#define WW3D_DEVTYPE D3DDEVTYPE_HAL

#if !defined(WINVER) || WINVER < 0x0500
#undef WINVER
#define WINVER 0x0500 // Required to access GetMonitorInfo in VC6.
#endif

#include "dx8wrapper.h"
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
// GeneralsX: Need LoadLibrary/GetProcAddress/FreeLibrary for dynamic loading
// GeneralsX: module_compat.h maps these onto dlopen for POSIX. On
// Windows they are the real Win32 API from <windows.h>, and the shim header is no longer on
// the include path there (it lived beside the windows.h that was shadowing the SDK).
#ifndef _WIN32
#include "module_compat.h"
#endif
// GeneralsX @build felipebraz 16/02/2026 - Need dlerror() for dlopen() error reporting on Linux
#ifndef _WIN32
#include <dlfcn.h>
#endif
// GeneralsX: Embedded browser Windows-only (requires COM LPDISPATCH)
#ifdef _WIN32
#include "dx8webbrowser.h"
#endif
#include "dx8fvf.h"
#include "dx8vertexbuffer.h"
#include "dx8indexbuffer.h"
#include "dx8renderer.h"
#include "ww3d.h"
#include "camera.h"
#include "WWLib/wwstring.h"
#include "WWMath/matrix4.h"
#include "vertmaterial.h"
#include "rddesc.h"
#include "lightenvironment.h"
#include "statistics.h"
#include "WWLib/registry.h"
#include "boxrobj.h"
#include "pointgr.h"
#include "render2d.h"
#include "sortingrenderer.h"
#include "shattersystem.h"
#include "light.h"
#include "assetmgr.h"
#include "textureloader.h"
#include "missingtexture.h"
#include "WWLib/thread.h"
#include <d3dx8core.h>
#include "WWMath/pot.h"
// d3d8 ring spike (DX8Ring)
#include <atomic>
#include <cfenv>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#if defined(__APPLE__)
#include <pthread.h>
#endif
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#include "WWDebug/wwprofile.h"
#include "WWLib/ffactory.h"
#include "dx8caps.h"
#include "formconv.h"
#include "dx8texman.h"
#include "WWLib/bound.h"
#include "WWLib/DbgHelpGuard.h"
#include <Utility/bit_adapter.h>

#include "shdlib.h"
#include <chrono>

const int DEFAULT_RESOLUTION_WIDTH = 640;
const int DEFAULT_RESOLUTION_HEIGHT = 480;
const int DEFAULT_BIT_DEPTH = 32;
const int DEFAULT_TEXTURE_BIT_DEPTH = 16;
const D3DMULTISAMPLE_TYPE DEFAULT_MSAA = D3DMULTISAMPLE_NONE;

static D3DPRESENT_PARAMETERS _PresentParameters;

// --- Pillarbox: render game to offscreen RT, blit centered onto backbuffer ---
// GeneralsX @feature xxorza 15/04/2026 Unified pillarbox for fullscreen and windowed
DX8Wrapper::DisplaySizeFunc DX8Wrapper::s_getNativeDisplaySize = nullptr;
DX8Wrapper::DisplaySizeFunc DX8Wrapper::s_getWindowSize = nullptr;
bool DX8Wrapper::s_pillarboxEnabled = false;
bool DX8Wrapper::s_pillarboxActive = false;
int DX8Wrapper::s_bbW = 0;
int DX8Wrapper::s_bbH = 0;
int DX8Wrapper::s_dstX = 0;
int DX8Wrapper::s_dstY = 0;
int DX8Wrapper::s_dstW = 0;
int DX8Wrapper::s_dstH = 0;
float DX8Wrapper::s_pixelDensity = 1.0f;
IDirect3DTexture8* DX8Wrapper::s_offscreenTex = nullptr;
IDirect3DSurface8* DX8Wrapper::s_offscreenSurf = nullptr;
IDirect3DSurface8* DX8Wrapper::s_depthSurf = nullptr;
IDirect3DSurface8* DX8Wrapper::s_savedBackbuffer = nullptr;
IDirect3DSurface8* DX8Wrapper::s_savedDepth = nullptr;
// GeneralsX: The multisampled colour target the pillarboxed scene renders into while MSAA
// is on; Pillarbox_End resolves it into s_offscreenSurf. Null when MSAA is off or the GPU refused it.
static IDirect3DSurface8* s_msaaSurf = nullptr;

void DX8Wrapper::Set_Display_Size_Provider(DisplaySizeFunc nativeSize, DisplaySizeFunc windowSize)
{
	s_getNativeDisplaySize = nativeSize;
	s_getWindowSize = windowSize;
}

bool DX8Wrapper::GetNativeDisplaySize(int& outW, int& outH, float& outDensity)
{
	return s_getNativeDisplaySize && s_getNativeDisplaySize(outW, outH, outDensity);
}

bool DX8Wrapper::GetWindowSize(int& outW, int& outH, float& outDensity)
{
	return s_getWindowSize && s_getWindowSize(outW, outH, outDensity);
}

void DX8Wrapper::Pillarbox_Cleanup()
{
	if (s_savedBackbuffer) { s_savedBackbuffer->Release(); s_savedBackbuffer = nullptr; }
	if (s_savedDepth) { s_savedDepth->Release(); s_savedDepth = nullptr; }
	if (s_depthSurf) { s_depthSurf->Release(); s_depthSurf = nullptr; }
	if (s_msaaSurf) { s_msaaSurf->Release(); s_msaaSurf = nullptr; }
	if (s_offscreenSurf) { s_offscreenSurf->Release(); s_offscreenSurf = nullptr; }
	if (s_offscreenTex) { s_offscreenTex->Release(); s_offscreenTex = nullptr; }
	s_pillarboxEnabled = false;
	s_pillarboxActive = false;
}

// GeneralsX: CONTRACT, because the bool returned here is a trap.
//
// The return means "is the pillarbox now enabled", NOT "did this succeed". It is false on four
// different paths and only three of them are failures:
//
//   true   - offscreen target built, s_pillarboxEnabled set, INFO line carrying the fit rect
//   false  - backbuffer already equals the game resolution, nothing to do, INFO line "1:1"
//   false  - CreateTexture failed, ERROR line carrying the HRESULT
//   false  - GetSurfaceLevel failed, ERROR line carrying the HRESULT
//   false  - CreateDepthStencilSurface failed, ERROR line carrying the HRESULT
//
// So `if (!Pillarbox_Setup(...)) <treat as error>` is WRONG, and wrong in the worst possible place.
// The 1:1 exit is the NORMAL configuration whenever the chosen resolution matches the display -
// which is exactly what the owner's own machine does once a stored 1680x1050 migrates to the panel
// size. Such a check DOES fire on the three failures; the defect is that it fires identically on
// the healthy 1:1 launch and cannot tell the two apart. Since 1:1 is the common case and an
// allocation failure is the rare one, in practice it would be almost entirely false alarms, on
// exactly the machines that have nothing wrong with them.
//
// Distinguishing the two falses is therefore a log-level distinction today (INFO vs ERROR), not a
// return-value one. All four call sites deliberately discard the return and each says why at the
// site. If a caller ever needs to ACT on failure, this must first become a tri-state - do not
// "fix" it by testing the bool.
//
// GeneralsX: DECISION on forcing the renderer back to the window size when
// a REQUESTED pillarbox fails (s_pillarboxEnabled false while backbuffer != game): NOT done, and
// deliberately so.
//
// It is the right instinct. Render2DClass and the viewport really do end up disagreeing with the
// backbuffer, and that disagreement is the visible defect. But both cheap ways to resolve it are
// worse than the wrong picture they would fix:
//
//   - Rewrite ResolutionWidth/ResolutionHeight to the backbuffer size. Those are read all over the
//     engine - Get_Device_Resolution, mouse mapping, UI layout - so a render-target allocation
//     failure would silently change the resolution the game reports to the player and to every
//     caller that asks. That is a larger lie than the one being corrected.
//   - Reset the backbuffer down to the game resolution instead. That oscillates once per frame;
//     the reasoning is written out at the Pillarbox_Process_Resize call site.
//
// The honest fix is for the layer that CHOSE the resolution to learn the choice did not take and
// pick again - above DX8Wrapper, not inside it - which needs the tri-state above. None of it can be
// verified today either: no failure here is reachable DETERMINISTICALLY without a fault-injection
// hook - real video-memory exhaustion does return D3DERR_OUTOFVIDEOMEMORY, just not on demand - so behaviour
// added on this path would ship untested. Making the path observable is the part that CAN be done
// correctly right now, so that is what this change limits itself to.
bool DX8Wrapper::Pillarbox_Setup(int gameW, int gameH)
{
	DX8Ring::Drain();	// d3d8 ring spike: the device must be idle before a direct D3DDevice call
	Pillarbox_Cleanup();

	// Determine backbuffer dimensions from the active present parameters first.
	// GeneralsX: Using native display size here can diverge from
	// the actual DXVK backbuffer during SDL fullscreen transitions and cause zoom/crop artifacts.
	int bbW = (int)_PresentParameters.BackBufferWidth;
	int bbH = (int)_PresentParameters.BackBufferHeight;
	float density = 1.0f;

	if (bbW <= 0 || bbH <= 0) {
		if (!GetWindowSize(bbW, bbH, density)) {
			if (!GetNativeDisplaySize(bbW, bbH, density)) {
				bbW = gameW;
				bbH = gameH;
				density = 1.0f;
			}
		}
	}
	else {
		// GeneralsX: The density was only ever fetched on the FALLBACK path.
		//
		// On the live path _PresentParameters.BackBufferWidth is already valid, so the branch above
		// is skipped and `density` kept its 1.0f initialiser - which meant s_pixelDensity was 1.0
		// on every real frame. Pillarbox_Get_Rect divides by s_pixelDensity precisely so it can hand
		// back POINTS, so with the divisor stuck at 1 it returned raw BACKBUFFER PIXELS while every
		// caller read it as points.
		//
		// Harmless while points == pixels, which is every 1x display - which is why it survived. It
		// becomes wrong the moment a 2x display renders at native pixels: SDL3Mouse's pillarbox
		// branch subtracts this rect from rawX/rawY, and those are SDL window coordinates, i.e.
		// POINTS. Mixing the two puts clicks at 1/density of where the player aimed, and pillarbox
		// is active in the default first-launch configuration on a Retina Mac, not some corner case.
		//
		// Fetch the density without disturbing bbW/bbH: the backbuffer size from the present
		// parameters is the authoritative one here (see the note above about SDL fullscreen
		// transitions) and must not be replaced by the window size.
		int densityProbeW = 0, densityProbeH = 0;
		if (!GetWindowSize(densityProbeW, densityProbeH, density)) {
			if (!GetNativeDisplaySize(densityProbeW, densityProbeH, density)) {
				density = 1.0f;
			}
		}
		if (density <= 0.0f) {
			density = 1.0f;
		}
	}

	// No pillarbox needed if backbuffer matches game resolution
	// GeneralsX: SAY SO OUT LOUD. This early return used to be the only
	// silent exit in the function, and that made the healthy case and the worst case produce
	// byte-identical logs.
	//
	// Every macOS fullscreen launch creates the device against SDL3Main's 1024x768 PLACEHOLDER
	// window, because W3DDisplay::init calls SDL3_ApplyWindowModeForRenderConfig only AFTER the
	// Set_Render_Device loop. So every run logs "backbuffer=1024x768" first, whatever resolution
	// was asked for, and the picture is only rescued a frame later by Pillarbox_Process_Resize
	// resetting the swapchain to the real window size. Measured across 40+ runs: with game
	// 1280x720 / 1600x900 / 2560x1440 / 3840x2160 the rescue is visible as a second line at
	// backbuffer=1920x1080.
	//
	// It is NOT visible when the game resolution already equals the display - which on this
	// 1920x1080 panel is precisely what the stored 1680x1050 migrates to, i.e. the configuration
	// the owner actually plays. There, a rescued run returns here (silent) and an unrescued run
	// never calls this at all (also silent). "No second line" was both the success message and
	// the symptom, so the question "is the Mac presenting 1024x768 upscaled?" could not be
	// answered from a log at all. One line per launch buys a falsifiable answer: present means
	// 1:1 at native, absent means the swapchain never left the placeholder.
	if (bbW == gameW && bbH == gameH) {
		// GeneralsX @diag /08/2026 density is in this line because its absence hid a bug.
		// s_pixelDensity was silently 1.0 on every live frame, and Pillarbox_Get_Rect divides by it
		// to hand back points - so the rect was pixels wearing a points label and nothing in any log
		// could show it. On a 1x machine the two are equal and the line looks identical either way,
		// which is exactly why this needs printing rather than deriving.
		fprintf(stderr, "INFO: Pillarbox: game=%dx%d, backbuffer=%dx%d, present=%ux%u, windowed=%d, density=%.3f - 1:1, no pillarbox\n",
			gameW,
			gameH,
			bbW,
			bbH,
			_PresentParameters.BackBufferWidth,
			_PresentParameters.BackBufferHeight,
			IsWindowed ? 1 : 0,
			density);
		return false;
	}

	// GeneralsX @diag /09/2026 The allocation failures below used to `return false` without
	// writing anything anywhere, and that silence is the reason this block was rewritten.
	//
	// s_pillarboxEnabled stays false on this path, so Pillarbox_Begin early-returns and the scene is
	// drawn straight onto a window-sized backbuffer - while Render2DClass and the viewport are still
	// carrying the GAME resolution that Set_Render_Device installed. The player gets a wrong picture,
	// the log gets nothing, and afterwards there is no way to tell it happened. The success path and
	// the 1:1 exit both already print, so a failure that printed nothing was indistinguishable from
	// this function never having been called at all - which is the one thing a reader would conclude.
	//
	// These lines carry the same fields in the same order as the two INFO lines so all three grep
	// alike, plus the HRESULT, which is the only thing separating "out of VRAM" from "format
	// rejected", and a restatement of the resulting mismatch so the consequence needs no lookup.

	// Create offscreen render target at game resolution
	HRESULT hr = D3DDevice->CreateTexture(gameW, gameH, 1, D3DUSAGE_RENDERTARGET,
		D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &s_offscreenTex);
	if (FAILED(hr)) {
		fprintf(stderr, "ERROR: Pillarbox: game=%dx%d, backbuffer=%dx%d, present=%ux%u, windowed=%d, density=%.3f - CreateTexture(RENDERTARGET, A8R8G8B8) FAILED hresult=0x%08X - pillarbox DISABLED, scene renders straight to the %dx%d backbuffer while Render2D and the viewport stay at %dx%d\n",
			gameW, gameH,
			bbW, bbH,
			_PresentParameters.BackBufferWidth, _PresentParameters.BackBufferHeight,
			IsWindowed ? 1 : 0,
			density,
			(unsigned)hr,
			bbW, bbH,
			gameW, gameH);
		return false;
	}

	// GeneralsX: GetSurfaceLevel's HRESULT was dropped on the floor, and both
	// consequences land inside the very failure this pass exists to expose.
	//
	// A null s_offscreenSurf reaching the enabled state would make Pillarbox_Begin early-return on its
	// own !s_offscreenSurf test - the identical silent wrong picture, arrived at by a different route.
	// Worse, the depth-stencil handler immediately below dereferences s_offscreenSurf without checking
	// it, so a failure here turned that error path into a null dereference.
	//
	// Level 0 of a texture created with 1 level is not expected to fail. "Not expected" is why it was
	// never checked; it is not a reason to leave a crash sitting in an error handler.
	hr = s_offscreenTex->GetSurfaceLevel(0, &s_offscreenSurf);
	if (FAILED(hr) || !s_offscreenSurf) {
		fprintf(stderr, "ERROR: Pillarbox: game=%dx%d, backbuffer=%dx%d, present=%ux%u, windowed=%d, density=%.3f - GetSurfaceLevel(0) FAILED hresult=0x%08X - pillarbox DISABLED, scene renders straight to the %dx%d backbuffer while Render2D and the viewport stay at %dx%d\n",
			gameW, gameH,
			bbW, bbH,
			_PresentParameters.BackBufferWidth, _PresentParameters.BackBufferHeight,
			IsWindowed ? 1 : 0,
			density,
			(unsigned)hr,
			bbW, bbH,
			gameW, gameH);
		s_offscreenSurf = nullptr;
		s_offscreenTex->Release(); s_offscreenTex = nullptr;
		return false;
	}

	// GeneralsX: MSAA SURVIVES THE PILLARBOX. Every frame on this path is drawn into the
	// offscreen target, and a texture render target cannot be multisampled - so whenever the backbuffer was not
	// the game resolution (a scaled desktop, a Retina panel below its native mode) the chosen MSAA was paid for
	// on the backbuffer and never seen. With MSAA on, the scene now renders into a multisampled surface with a
	// matching depth, resolved into the texture at Pillarbox_End (CopyRects -> DXVK StretchRect resolve).
	// Refused by the GPU: say so and keep the old non-multisampled target.
	s_msaaSurf = nullptr;
	s_depthSurf = nullptr;
	if (MultiSampleAntiAliasing != D3DMULTISAMPLE_NONE) {
		hr = D3DDevice->CreateRenderTarget(gameW, gameH, D3DFMT_A8R8G8B8, MultiSampleAntiAliasing, FALSE, &s_msaaSurf);
		if (SUCCEEDED(hr)) {
			hr = D3DDevice->CreateDepthStencilSurface(gameW, gameH,
				_PresentParameters.AutoDepthStencilFormat, MultiSampleAntiAliasing, &s_depthSurf);
		}
		if (FAILED(hr)) {
			fprintf(stderr, "WARNING: Pillarbox: MSAA %d offscreen target FAILED hresult=0x%08X - the scene renders without MSAA\n",
				(int)MultiSampleAntiAliasing, (unsigned)hr);
			if (s_msaaSurf) { s_msaaSurf->Release(); s_msaaSurf = nullptr; }
			s_depthSurf = nullptr;
		}
	}

	// Create dedicated depth stencil at game resolution
	hr = s_depthSurf ? D3D_OK : D3DDevice->CreateDepthStencilSurface(gameW, gameH,
		_PresentParameters.AutoDepthStencilFormat, D3DMULTISAMPLE_NONE, &s_depthSurf);
	if (FAILED(hr)) {
		fprintf(stderr, "ERROR: Pillarbox: game=%dx%d, backbuffer=%dx%d, present=%ux%u, windowed=%d, density=%.3f - CreateDepthStencilSurface(fmt=%d) FAILED hresult=0x%08X - pillarbox DISABLED, scene renders straight to the %dx%d backbuffer while Render2D and the viewport stay at %dx%d\n",
			gameW, gameH,
			bbW, bbH,
			_PresentParameters.BackBufferWidth, _PresentParameters.BackBufferHeight,
			IsWindowed ? 1 : 0,
			density,
			(int)_PresentParameters.AutoDepthStencilFormat,
			(unsigned)hr,
			bbW, bbH,
			gameW, gameH);
		s_offscreenSurf->Release(); s_offscreenSurf = nullptr;
		s_offscreenTex->Release(); s_offscreenTex = nullptr;
		return false;
	}

	// Cache backbuffer size and compute aspect-correct fit rect once
	s_bbW = bbW; s_bbH = bbH;
	s_pixelDensity = density;
	float gameAspect = (float)gameW / (float)gameH;
	float bbAspect = (float)bbW / (float)bbH;
	if (bbAspect > gameAspect) {
		s_dstW = (int)(bbH * gameAspect); s_dstH = bbH;
		s_dstX = (bbW - s_dstW) / 2; s_dstY = 0;
	} else {
		s_dstW = bbW; s_dstH = (int)(bbW / gameAspect);
		s_dstX = 0; s_dstY = (bbH - s_dstH) / 2;
	}
	s_pillarboxEnabled = true;
	// GeneralsX @diag /08/2026 density printed here for the reason given at the 1:1 exit
	// above, and it matters more on this path: this is the branch where the rect actually gets used,
	// so a wrong density lands on the player's cursor rather than merely on a log line.
	fprintf(stderr, "INFO: Pillarbox: game=%dx%d, backbuffer=%dx%d, present=%ux%u, windowed=%d, density=%.3f, fit=%dx%d@%d,%d, msaa=%d\n",
		gameW,
		gameH,
		bbW,
		bbH,
		_PresentParameters.BackBufferWidth,
		_PresentParameters.BackBufferHeight,
		IsWindowed ? 1 : 0,
		density,
		s_dstW,
		s_dstH,
		s_dstX,
		s_dstY,
		s_msaaSurf ? (int)MultiSampleAntiAliasing : 0);
	return true;
}

void DX8Wrapper::Pillarbox_Begin()
{
	DX8Ring::Drain();	// d3d8 ring spike: the device must be idle before a direct D3DDevice call
	if (!s_pillarboxEnabled || !s_offscreenSurf || !s_depthSurf) return;
	// GeneralsX: Route pillarbox target switches through the DX8Wrapper cache so render-target size queries stay correct during the frame.
	Set_Render_Target(s_msaaSurf ? s_msaaSurf : s_offscreenSurf, s_depthSurf);
	// GeneralsX: Pillarbox uses an offscreen target for presentation only and must not advertise the whole frame as an effect RTT pass.
	IsRenderToTexture = false;
	// GeneralsX: Ensure scene render uses game-resolution viewport on the offscreen RT.
	// Without this, a stale fullscreen-sized viewport can survive the target switch and crop/zoom into the top-left area.
	D3DVIEWPORT8 sceneViewport = {
		0,
		0,
		(DWORD)ResolutionWidth,
		(DWORD)ResolutionHeight,
		0.0f,
		1.0f
	};
	D3DDevice->SetViewport(&sceneViewport);
	// Clear the offscreen RT and its depth buffer. The Begin_Render clear targeted
	// the backbuffer before we switched, so without this the offscreen would carry
	// stale depth from the previous frame.
	D3DDevice->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
		0x00000000, 1.0f, 0);
	D3DDevice->SetRenderState(D3DRS_ZENABLE, TRUE);
	D3DDevice->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
	D3DDevice->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	Gx_Truth_Raw_RS(D3DRS_ZENABLE, TRUE);	// WS-G device truth: these three bypass the wrapper's cache
	Gx_Truth_Raw_RS(D3DRS_ZWRITEENABLE, TRUE);
	Gx_Truth_Raw_RS(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	s_pillarboxActive = true;
}

void DX8Wrapper::Pillarbox_End()
{
	DX8Ring::Drain();	// d3d8 ring spike: the device must be idle before a direct D3DDevice call
	if (!s_pillarboxActive || !s_offscreenTex) return;
	s_pillarboxActive = false;

	// GeneralsX: Resolve the multisampled scene into the texture the blit below samples.
	if (s_msaaSurf) {
		const HRESULT hr = D3DDevice->CopyRects(s_msaaSurf, nullptr, 0, s_offscreenSurf, nullptr);
		static bool s_resolveFailLogged = false;
		if (FAILED(hr) && !s_resolveFailLogged) {
			s_resolveFailLogged = true;
			fprintf(stderr, "ERROR: Pillarbox: MSAA resolve (CopyRects) FAILED hresult=0x%08X - the frame shows a stale image\n", (unsigned)hr);
		}
	}

	// Restore backbuffer (still inside active scene)
	Set_Render_Target((IDirect3DSurface8 *)nullptr);
	IsRenderToTexture = false;

	// Blit offscreen texture centered onto backbuffer (fit rect cached in Pillarbox_Setup)
	D3DVIEWPORT8 vp = {0, 0, (DWORD)s_bbW, (DWORD)s_bbH, 0.0f, 1.0f};
	D3DDevice->SetViewport(&vp);
	D3DDevice->Clear(0, nullptr, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0);

	// Use DX8Wrapper's cached state setters so the cache stays in sync — no invalidation needed.
	D3DDevice->SetTexture(0, s_offscreenTex);
	Set_DX8_Render_State(D3DRS_ZENABLE, FALSE);
	Set_DX8_Render_State(D3DRS_ZWRITEENABLE, FALSE);
	Set_DX8_Render_State(D3DRS_LIGHTING, FALSE);
	Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE, FALSE);
	Set_DX8_Render_State(D3DRS_ALPHATESTENABLE, FALSE);
	Set_DX8_Render_State(D3DRS_CULLMODE, D3DCULL_NONE);
	Set_DX8_Render_State(D3DRS_FOGENABLE, FALSE);
	Set_DX8_Render_State(D3DRS_STENCILENABLE, FALSE);
	Set_DX8_Render_State(D3DRS_COLORWRITEENABLE, 0xF);
	Set_DX8_Render_State(D3DRS_CLIPPING, FALSE);
	Set_DX8_Texture_Stage_State(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
	Set_DX8_Texture_Stage_State(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
	Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	DWORD filterMode = (s_dstW == ResolutionWidth && s_dstH == ResolutionHeight)
		? D3DTEXF_POINT : D3DTEXF_LINEAR;
	Set_DX8_Texture_Stage_State(0, D3DTSS_MINFILTER, filterMode);
	Set_DX8_Texture_Stage_State(0, D3DTSS_MAGFILTER, filterMode);

	// D3D8 half-pixel offset: XYZRHW vertices need -0.5 for exact texel-to-pixel alignment
	float x0 = (float)s_dstX - 0.5f, y0 = (float)s_dstY - 0.5f;
	float x1 = (float)(s_dstX + s_dstW) - 0.5f, y1 = (float)(s_dstY + s_dstH) - 0.5f;
	struct BV { float x, y, z, rhw; float u, v; };
	BV quad[4] = {
		{x0, y0, 0, 1, 0, 0}, {x1, y0, 0, 1, 1, 0},
		{x0, y1, 0, 1, 0, 1}, {x1, y1, 0, 1, 1, 1},
	};
	D3DDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_TEX1);
	D3DDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(BV));
	DX8Wrapper::Record_Direct_Draw();	// [GXGFX] dc= (graphics P2 i-02)
	D3DDevice->SetTexture(0, nullptr);
}

bool DX8Wrapper::Pillarbox_Get_Rect(int& x, int& y, int& w, int& h)
{
	if (!s_pillarboxEnabled) return false;
	x = (int)(s_dstX / s_pixelDensity);
	y = (int)(s_dstY / s_pixelDensity);
	w = (int)(s_dstW / s_pixelDensity);
	h = (int)(s_dstH / s_pixelDensity);
	return true;
}

// GeneralsX: Does a NON-windowed device present as a window at the desktop
// mode rather than mode-setting? Always on SDL3 platforms (see _PresentParameters.Windowed below);
// on Windows only for borderless fullscreen (DX8Wrapper::Set_Borderless). The !_WIN32 answer is a
// constant so no SDL3 platform can change behaviour through this.
static inline bool Fullscreen_Presents_As_Window()
{
#ifdef _WIN32
	return DX8Wrapper::Is_Borderless();
#else
	return true;
#endif
}

// The Windows half alone: the sites that differ between Windows windowed and exclusive, and that SDL3
// platforms deliberately keep on the exclusive branch.
static inline bool Win32_Presents_Borderless()
{
#ifdef _WIN32
	return DX8Wrapper::Is_Borderless();
#else
	return false;
#endif
}

#ifdef _WIN32
// GeneralsX: The borderless popup covers the monitor it is on. `setup` is device
// setup (Resize_And_Position_Window): always placed, and dropped from the TOPMOST band WinMain gives every
// non-windowed window. Otherwise it is the per-frame check (Pillarbox_Process_Resize), which moves the
// window only when that monitor's rectangle no longer matches it - a resolution or layout change
// (WM_DISPLAYCHANGE), or Win+Shift+Arrow onto another monitor - and never steals focus or z-order doing
// so. Before it, the popup kept the rectangle captured at setup and the backbuffer followed that stale
// client size, leaving desktop exposed or the picture off-screen. A minimised window is
// left alone; the first frame after it is restored fits it.
static void Borderless_Cover_Monitor(HWND hwnd, bool setup)
{
	if (hwnd == nullptr || (!setup && IsIconic(hwnd)))
		return;
	MONITORINFO mi = {sizeof(MONITORINFO)};
	if (!GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi))
		return;
	const RECT& m = mi.rcMonitor;
	if (!setup) {
		RECT w;
		if (!GetWindowRect(hwnd, &w) || EqualRect(&w, &m))
			return;
	}
	::SetWindowPos(hwnd, HWND_NOTOPMOST, m.left, m.top, m.right - m.left, m.bottom - m.top,
		setup ? SWP_FRAMECHANGED : (SWP_NOZORDER | SWP_NOACTIVATE));
	fprintf(stderr, "[GXDISPLAY] borderless window %s monitor at %ld,%ld size %ldx%ld primary=%d\n",
		setup ? "covers" : "refit to", (long)m.left, (long)m.top, (long)(m.right - m.left), (long)(m.bottom - m.top),
		(mi.dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0);
}
#endif

// GeneralsX: Keep SDL-managed fullscreen swapchains aligned to the current window size until the native fullscreen transition finishes.
static void Resolve_Present_BackBuffer_Size(int gameW, int gameH, bool isWindowed, UINT& outW, UINT& outH)
{
	outW = (UINT)gameW;
	outH = (UINT)gameH;

	// GeneralsX: Was `#ifndef _WIN32`; Windows borderless takes it too.
	if (!isWindowed && Fullscreen_Presents_As_Window()) {
		int windowW = gameW;
		int windowH = gameH;
		float density = 1.0f;
		if (DX8Wrapper::GetWindowSize(windowW, windowH, density)) {
			outW = (UINT)windowW;
			outH = (UINT)windowH;
			return;
		}
	}

	if (!isWindowed) {
		int nativeW = 0;
		int nativeH = 0;
		float density = 1.0f;
		if (DX8Wrapper::GetNativeDisplaySize(nativeW, nativeH, density)) {
			outW = (UINT)nativeW;
			outH = (UINT)nativeH;
		}
	}
}

DX8FrameStatistics DX8Wrapper::FrameStatistics;
void (*DX8Wrapper::Present_Probe)(bool after) = nullptr;
void (*DX8Wrapper::GxPrePresentHook)() = nullptr;	// GeneralsX: WS-G S1 seat (dx8wrapper.h)
void (*GxDrawHook)(unsigned, unsigned, unsigned, unsigned, unsigned, bool) = nullptr;	// WS-G (dx8wrapper.h)
void (*GxPostDrawHook)() = nullptr;	// WS-G seat: after the same two draws are recorded (dx8wrapper.h)
bool DX8Wrapper::GxCaptureOn = false;
D3DVIEWPORT8 DX8Wrapper::GxViewportSent;
bool DX8Wrapper::GxViewportSentValid = false;
bool DX8Wrapper::GxViewportCaptureOn = false;
D3DMATERIAL8 DX8Wrapper::GxMaterialSent;
bool DX8Wrapper::GxMaterialSentValid = false;
Matrix4x4 DX8Wrapper::GxProjSent;
bool DX8Wrapper::GxProjSentValid = false;
Matrix4x4 DX8Wrapper::GxTexmSent[8];
unsigned DX8Wrapper::GxTexmValid = 0;
unsigned DX8Wrapper::GxXformValid = 0;
Matrix4x4 DX8Wrapper::GxViewSent;
bool DX8Wrapper::GxViewSentValid = false;
bool DX8Wrapper::GxTruthOn = false;
unsigned DX8Wrapper::GxTruthRS[256];
bool DX8Wrapper::GxTruthRSKnown[256];
unsigned DX8Wrapper::GxTruthTSS[MAX_TEXTURE_STAGES][32];
bool DX8Wrapper::GxTruthTSSKnown[MAX_TEXTURE_STAGES][32];
unsigned long long DX8Wrapper::GxTruthRawCalls = 0;
// WS-G device truth (dx8wrapper.h): read every slot from the device. Called when the check turns tracking on, and after a Reset.
void DX8Wrapper::Gx_Truth_Reseed()
{
	if (!GxTruthOn || D3DDevice == nullptr)
		return;
	IDirect3DDevice8 *dev = _Get_D3D_Device8();
	for (unsigned i = 0; i < 256; ++i) {
		DWORD v = 0;
		GxTruthRSKnown[i] = dev->GetRenderState((D3DRENDERSTATETYPE)i, &v) == D3D_OK;
		GxTruthRS[i] = v;
	}
	for (unsigned st = 0; st < MAX_TEXTURE_STAGES; ++st)
		for (unsigned t = 0; t < 32; ++t) {
			DWORD v = 0;
			GxTruthTSSKnown[st][t] = dev->GetTextureStageState(st, (D3DTEXTURESTAGESTATETYPE)t, &v) == D3D_OK;
			GxTruthTSS[st][t] = v;
		}
}
void DX8Wrapper::Gx_Truth_Enable()
{
	GxTruthOn = true;
	Gx_Truth_Reseed();
}
void (*DX8Wrapper::Pre_Reset_Hook)() = nullptr;
void (*DX8Wrapper::Pre_Release_Hook)() = nullptr;
void (*DX8Wrapper::Default_Pool_Release_Hook)() = nullptr;
unsigned long long DX8Wrapper::PresentCount = 0;
// GeneralsX: [GXCENSUS] (dx8wrapper.h enum DX8CensusCounter; the order must match it).
#if defined(GX_TEST_INSTRUMENTS) || defined(GX_RENDER_CENSUS)
bool DX8Wrapper::CensusOn = false;
#endif
unsigned long long DX8Wrapper::Census[DX8C_N] = {};
bool DX8Wrapper::CensusTransformValid[2] = {};
bool DX8Wrapper::XformShadowValid[2] = {};
unsigned DX8Wrapper::WrapSkipMask = 0;
unsigned DX8Wrapper::LightStateSerial = 0;
D3DMATERIAL8 DX8Wrapper::MaterialShadow;
bool DX8Wrapper::MaterialShadowValid = false;
unsigned DX8Wrapper::Read_Wrap_Skip()
{
	const char *e = getenv("GX_WRAPSKIP");
	const unsigned mask = (e != nullptr && e[0] >= '0' && e[0] <= '9') ? (unsigned)atoi(e) & 15u : 15u;
	fprintf(stderr, "[GXWRAPSKIP] mask=%u (1 lights, 2 material, 4 world/view, 8 light-environment inputs)\n", mask);
	return mask;
}
unsigned DX8Wrapper::CensusLastFVF = ~0u;
const char *const DX8Wrapper::CensusNames[DX8C_N] = {
	"draw", "world_dev", "world_same", "view_dev", "view_same",
	"lenv", "lenv_same", "light_dev", "mat", "mat_dev", "mat_same", "tex", "tex_dev",
	"tss_dev", "rs_dev", "shader", "shader_apply", "vb_dev", "vb_same", "fvf_dev", "fvf_same",
	"ib_dev", "ib_same", "lk_disc", "lk_noow", "lk_plain", "dynvb_wrap", "dynvb_grow", "dynib_wrap", "dynib_grow",
	"sort_order_ns", "sort_fill_ns", "sort_tri_ns", "sort_tris",
	"obj", "obj_shroud", "obj_multilod", "obj_lt4", "obj_lt8", "obj_lt16", "mesh", "mesh_lt4", "mesh_lt8", "mesh_lt16",
	"sstat_draw", "sstat_vbsw", "obj_fog", "skip_xform", "skip_mat", "skip_light", "skip_lenv",
	"l2_flush", "l2_task", "l2_rigid", "l2_r_sort", "l2_r_blend", "l2_r_ovr", "l2_r_cam", "l2_r_scale", "l2_r_lenv0",
	"l2_r_pass1", "l2_r_elig", "l2_r_bin", "l2_skin", "l2_skin_sort", "l2_skin_b1", "l2_s_task", "l2_matpass", "l2_dmatpass", "l2_decal" };
// The buffers the wrapper last sent (a null binding is recorded as the address of the variable itself, so it never
// matches a real buffer and a bind-unbind-rebind is not "same"). Other code binds buffers and FVFs on the device
// directly (shadows, water, terrain overlays); most of it calls Invalidate_Cached_Render_States afterwards, which
// forgets these, but not all of it, so vb_same/ib_same/fvf_same are UPPER bounds of what a wrapper-side compare could
// skip. A buffer freed and re-created at the same address also reads as "same".
static const void *s_cenLastVB = nullptr;
static const void *s_cenLastIB = nullptr;
static unsigned s_cenLastBase = ~0u;
static_assert(sizeof(DX8Wrapper::CensusNames) / sizeof(DX8Wrapper::CensusNames[0]) == DX8C_N, "[GXCENSUS] names");
long long DX8Wrapper::Census_Now_Ns()
{
	return (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}
static DX8FrameStatistics LastFrameStatistics;

bool DX8Wrapper_IsWindowed = true;

// FPU_PRESERVE
int DX8Wrapper_PreserveFPU = 0;

/***********************************************************************************
**
** DX8Wrapper Static Variables
**
***********************************************************************************/

static HWND						_Hwnd															= nullptr;
bool								DX8Wrapper::IsInitted									= false;
bool								DX8Wrapper::_EnableTriangleDraw						= true;

int								DX8Wrapper::CurRenderDevice							= -1;
int								DX8Wrapper::ResolutionWidth							= DEFAULT_RESOLUTION_WIDTH;
int								DX8Wrapper::ResolutionHeight							= DEFAULT_RESOLUTION_HEIGHT;
int								DX8Wrapper::BitDepth										= DEFAULT_BIT_DEPTH;
int								DX8Wrapper::TextureBitDepth							= DEFAULT_TEXTURE_BIT_DEPTH;
bool								DX8Wrapper::IsWindowed									= false;
bool								DX8Wrapper::Borderless									= false;
bool								DX8Wrapper::WindowedVSync								= false;
D3DFORMAT					DX8Wrapper::DisplayFormat	= D3DFMT_UNKNOWN;
D3DMULTISAMPLE_TYPE DX8Wrapper::MultiSampleAntiAliasing	= DEFAULT_MSAA;

// GeneralsX @diag /10/2026 What Set_Render_Device's format search did, for the [GXCAPS] line printed after
// Compute_Caps. -1 = the windowed branch ran (no mode search), 0 = the exclusive branch's exact-mode search failed,
// 1 = it found a mode, 2 = it failed and the iOS fallback took the current display mode's format. A failed search leaves DisplayFormat UNKNOWN, and Compute_Caps(UNKNOWN) then reports every
// texture, render-target and depth format unsupported - which is what the iPad did (ios-caps-fix-1).
static int GxModeSearch = -1;
static int GxModeAskedW = 0;
static int GxModeAskedH = 0;

// shader system additions KJM v
DWORD								DX8Wrapper::Vertex_Shader								= 0;
DWORD								DX8Wrapper::Pixel_Shader								= 0;

Vector4							DX8Wrapper::Vertex_Shader_Constants[MAX_VERTEX_SHADER_CONSTANTS];
Vector4							DX8Wrapper::Pixel_Shader_Constants[MAX_PIXEL_SHADER_CONSTANTS];

LightEnvironmentClass*		DX8Wrapper::Light_Environment							= nullptr;

DWORD								DX8Wrapper::Vertex_Processing_Behavior				= 0;
ZTextureClass*					DX8Wrapper::Shadow_Map[MAX_SHADOW_MAPS];

Vector3							DX8Wrapper::Ambient_Color;
// shader system additions KJM ^

bool								DX8Wrapper::world_identity;
unsigned							DX8Wrapper::RenderStates[256];
unsigned							DX8Wrapper::TextureStageStates[MAX_TEXTURE_STAGES][32];
IDirect3DBaseTexture8 *		DX8Wrapper::Textures[MAX_TEXTURE_STAGES];
RenderStateStruct				DX8Wrapper::render_state;
unsigned							DX8Wrapper::render_state_changed;

bool								DX8Wrapper::FogEnable									= false;
D3DCOLOR							DX8Wrapper::FogColor										= 0;

IDirect3D8 *					DX8Wrapper::D3DInterface								= nullptr;
IDirect3DDevice8 *			DX8Wrapper::D3DDevice									= nullptr;
DX8Wrapper::DeviceWrapHook	DX8Wrapper::Device_Wrap_Hook							= nullptr;
DX8Wrapper::InterfaceWrapHook	DX8Wrapper::Interface_Wrap_Hook						= nullptr;
IDirect3DSurface8 *			DX8Wrapper::CurrentRenderTarget						= nullptr;
IDirect3DSurface8 *			DX8Wrapper::CurrentDepthBuffer						= nullptr;
IDirect3DSurface8 *			DX8Wrapper::DefaultRenderTarget						= nullptr;
IDirect3DSurface8 *			DX8Wrapper::DefaultDepthBuffer						= nullptr;
bool								DX8Wrapper::IsRenderToTexture							= false;

unsigned							DX8Wrapper::_MainThreadID								= 0;
bool								DX8Wrapper::CurrentDX8LightEnables[4];
D3DLIGHT8							DX8Wrapper::CurrentDX8Lights[4];
bool								DX8Wrapper::CurrentDX8LightValid[4];
bool								DX8Wrapper::IsDeviceLost;
int								DX8Wrapper::ZBias;
float								DX8Wrapper::ZNear;
float								DX8Wrapper::ZFar;
// GeneralsX: Use Matrix4x4 not D3DMATRIX (fighter19 pattern)
Matrix4x4						DX8Wrapper::ProjectionMatrix;
Matrix4x4						DX8Wrapper::DX8Transforms[D3DTS_WORLD+1];

DX8Caps*							DX8Wrapper::CurrentCaps = nullptr;

// Hack test... this disables rendering of batches of too few polygons.
unsigned							DX8Wrapper::DrawPolygonLowBoundLimit=0;

D3DADAPTER_IDENTIFIER8		DX8Wrapper::CurrentAdapterIdentifier;

unsigned long DX8Wrapper::FrameCount = 0;

bool								_DX8SingleThreaded										= false;

static DynamicVectorClass<StringClass>					_RenderDeviceNameTable;
static DynamicVectorClass<StringClass>					_RenderDeviceShortNameTable;
static DynamicVectorClass<RenderDeviceDescClass>	_RenderDeviceDescriptionTable;


typedef IDirect3D8* (WINAPI *Direct3DCreate8Type) (UINT SDKVersion);
Direct3DCreate8Type	Direct3DCreate8Ptr = nullptr;
HINSTANCE D3D8Lib = nullptr;

DX8_CleanupHook	 *DX8Wrapper::m_pCleanupHook=nullptr;
#ifdef EXTENDED_STATS
DX8_Stats	 DX8Wrapper::stats;
#endif
// Called once per frame from W3DDisplay::draw(). Checks if the window size
// changed since last frame and reconfigures pillarbox accordingly.
void DX8Wrapper::Pillarbox_Process_Resize()
{
	if (!D3DDevice) return;

#ifdef _WIN32
	// Borderless: first put the window back over its monitor if a display change moved the monitor
	// under it, so the size read below is the one the backbuffer should follow.
	if (!IsWindowed && Borderless)
		Borderless_Cover_Monitor(_Hwnd, false);
#endif

	int physW = 0, physH = 0;
	float density = 1.0f;
	if (!GetWindowSize(physW, physH, density)) {
		if (!IsWindowed) {
			// GeneralsX: SDL fullscreen transitions can change window pixel size
			// after device creation. Fall back to native display size so we can re-sync the backbuffer.
			if (!GetNativeDisplaySize(physW, physH, density)) {
				return;
			}
		}
		else {
			return;
		}
	}

	// Nothing to do if backbuffer already matches window
	if ((int)_PresentParameters.BackBufferWidth == physW &&
		(int)_PresentParameters.BackBufferHeight == physH) return;

	Pillarbox_Cleanup();
	_PresentParameters.BackBufferWidth = physW;
	_PresentParameters.BackBufferHeight = physH;

	if (!Reset_Device()) {
		_PresentParameters.BackBufferWidth = ResolutionWidth;
		_PresentParameters.BackBufferHeight = ResolutionHeight;
		Reset_Device();
		return;
	}

	// GeneralsX: DECISION: return deliberately discarded, and no recovery is
	// attempted here. Written down because this is the site that most looks like it wants one - there
	// is already a fallback for the Reset_Device failure directly above - and that fallback must NOT
	// be reused for a Pillarbox_Setup failure.
	//
	// This function runs once per frame as the first statement of W3DDisplay::draw(), and it re-derives
	// its own trigger from the live window size at the top. Restoring the backbuffer to
	// ResolutionWidth/Height here would make the picture self-consistent for exactly one frame: next
	// frame GetWindowSize reports the physical window again, the equality test above fails again, and
	// we would cleanup / resize / Reset_Device / fail / restore / Reset_Device once more, for as long
	// as the failure persists. A transient failure would recover; a persistent one - which is what an
	// exhausted video-memory budget is - would not, and costs two device resets per frame until it
	// clears. That is considerably worse than a wrong picture.
	//
	// The Reset_Device fallback directly above is NOT exempt from this either: if the physical-size
	// reset keeps failing while the game-size one succeeds, the backbuffer is left at the game size,
	// next frame's equality test above sees it differ from the window again, and that fallback also
	// repeats. It is not "terminal so it runs once" - do not lean on that when reasoning here.
	//
	// What keeps the CURRENT code stable is that a Pillarbox_Setup failure leaves the present
	// parameters at the physical size, so the equality test above returns early on the next frame and
	// nothing retries. Adding a restore is precisely what would break that property.
	//
	// A correct recovery has to clear the trigger rather than the symptom - remember that setup failed
	// for this backbuffer size and stop retrying it - which is a design change, not an error check. See
	// the contract note above Pillarbox_Setup. Until then the ERROR line inside it is the deliverable:
	// it makes this state provable from a field log, which it previously was not.
	Pillarbox_Setup(ResolutionWidth, ResolutionHeight);
}

// GeneralsX: See dx8wrapper.h. A change on a live device resets it. No change, no
// reset: W3DDisplay::draw calls this on its first frame and whenever the cap or the refresh moves. Unlike
// Pillarbox_Process_Resize, a failed reset does NOT put the old value back: a swap effect is never why a
// reset fails (DXVK's d3d8 front end refuses only plain COPY with more than one back buffer, d3d8_device.cpp),
// a lost device is - and the lost-device recovery in Begin_Render resets with these parameters, so it lands
// in the requested mode rather than silently in the old one.
unsigned int DX8Wrapper::Get_FullScreen_Present_Interval()
{
	return _PresentParameters.FullScreen_PresentationInterval;
}

void DX8Wrapper::Set_Windowed_VSync(bool on)
{
	WindowedVSync = on;
	if (!D3DDevice || !_PresentParameters.Windowed) return;

	const D3DSWAPEFFECT want = on ? D3DSWAPEFFECT_COPY_VSYNC : D3DSWAPEFFECT_DISCARD;
	if (_PresentParameters.SwapEffect == want) return;

	_PresentParameters.SwapEffect = want;
	Reset_Device();	// which re-creates the pillarbox surfaces itself once the reset has worked
}

/***********************************************************************************
**
** DX8Wrapper Implementation
**
***********************************************************************************/

void Log_DX8_ErrorCode(unsigned res)
{
	char tmp[256]="";

	HRESULT new_res=D3DXGetErrorStringA(
		res,
		tmp,
		sizeof(tmp));

	if (new_res==D3D_OK) {
		WWDEBUG_SAY((tmp));
	}

	WWASSERT(0);
}

void Non_Fatal_Log_DX8_ErrorCode(unsigned res,const char * file,int line)
{
	char tmp[256]="";

	HRESULT new_res=D3DXGetErrorStringA(
		res,
		tmp,
		sizeof(tmp));

	if (new_res==D3D_OK) {
		WWDEBUG_SAY(("DX8 Error: %s, File: %s, Line: %d",tmp,file,line));
	}
}

// TheSuperHackers @info helmutbuhler 14/04/2025
// Helper function that moves x and y such that the inner rect fits into the outer rect.
// If the inner rect already is in the outer rect, then this does nothing.
// If the inner rect is larger than the outer rect, then the inner rect will be aligned to the top left of the outer rect.
void MoveRectIntoOtherRect(const RECT& inner, const RECT& outer, int* x, int* y)
{
	int dx = 0;
	if (inner.right > outer.right)
		dx = outer.right-inner.right;
	if (inner.left < outer.left)
		dx = outer.left-inner.left;

	int dy = 0;
	if (inner.bottom > outer.bottom)
		dy = outer.bottom-inner.bottom;
	if (inner.top < outer.top)
		dy = outer.top-inner.top;

	*x += dx;
	*y += dy;
}


// ----------------------------------------------------------------------------
// GeneralsX: d3d8 call ring spike (see DX8Ring in dx8wrapper.h).
// A single-producer single-consumer byte ring: monotonic 64-bit positions, power-of-two capacity, every command
// contiguous (a PAD command fills the end of the buffer when one would wrap). The main thread writes ahead of
// s_pub and publishes in batches; the render thread executes up to s_pub and reports progress in s_done. Only the
// main thread reads Recording/Pending, and every device access outside the recorder waits on s_done first.
// ----------------------------------------------------------------------------
namespace {
enum RingOp : uint32_t {
	RING_PAD, RING_RS, RING_TSS, RING_TEX, RING_REL, RING_XF, RING_MAT, RING_LIGHT, RING_LENA, RING_VS, RING_PS,
	RING_VSC, RING_PSC, RING_CLIP, RING_STREAM, RING_IND, RING_DIP, RING_VBCOPY, RING_IBCOPY,
	RING_CALL	// GeneralsX: DX8Wrapper::Device_Call - a function run on the device's thread, in stream order
};
struct RingHdr { uint32_t op, size; };	// size includes this header, a multiple of 8
struct RingStage { RingHdr h; void *buf; uint32_t offset, bytes, flags, pad; };	// + bytes of data
const size_t RING_CAP = size_t(64) << 20;
const size_t RING_MASK = RING_CAP - 1;
const size_t RING_PUBLISH_BYTES = 4096;
const size_t RING_STAGE_SLACK = size_t(8) << 20;	// room kept free for whatever is recorded while a stage is open
uint8_t *s_ring = nullptr;
size_t s_wr = 0;					// main: next write position
size_t s_lastPub = 0;				// main: the last position published
size_t s_doneInline = 0;			// mode 3: what the main thread has executed itself
size_t s_stageStart = 0;			// main: where the outermost open stage begins
std::atomic<size_t> s_pub(0);		// main -> render: written and complete up to here
std::atomic<size_t> s_done(0);		// render -> main: executed up to here
std::atomic<bool> s_sleeping(false);
std::atomic<bool> s_quit(false);
// never destroyed: the render thread may still be asleep on them while static destructors run at exit
std::mutex &s_wakeMutex = *new std::mutex;
std::condition_variable &s_wakeCv = *new std::condition_variable;
std::thread *s_thread = nullptr;
bool s_suspended = false;			// main: a raw device pointer was handed out inside the bracket (DX8Ring::Fallback)
// the render thread's idle spin before it sleeps (GX_D3D8RING_SPIN_US). 08/10, G0B replay, n=3 interleaved, object-flush span:
// wh1ter0se's PC (Windows): no spin beat the 100 us spin in every run and window (-0.07..-0.14 ms). The Mini (macOS): the 100 us
// spin beat no spin in every run and window (-0.1 ms; waking the thread ~106 times a frame costs more there). Pinning the
// render thread to its own core (Windows) did not help (main confined, trees span +0.2 ms) and was dropped.
#if defined(_WIN32)
int64_t s_spinNs = 0;
#else
int64_t s_spinNs = 100000;
#endif
bool s_stats = false;				// [GXRING] window lines only when GX_D3D8RING is set explicitly (a measurement)
int s_openStages = 0;
// [GXRING] window: main-thread sums (ns, counts) and the render thread's busy time
int64_t s_recStart = 0, s_midNs = 0;
uint64_t s_winRecNs = 0, s_winDrainNs = 0, s_winMidNs = 0, s_winCmds = 0, s_winBytes = 0, s_winStageBytes = 0;
uint64_t s_winFallbacks = 0, s_winFull = 0, s_winSpans = 0, s_winDraws = 0, s_winResumes = 0;
uint32_t s_winFrames = 0;
std::atomic<uint64_t> s_replayNs(0), s_wakes(0);
uint64_t s_replayNsPrev = 0, s_wakesPrev = 0;

int64_t Ring_Now_Ns()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline void Ring_Pause()
{
#if defined(_MSC_VER) && defined(_M_ARM64)
	__yield();
#elif defined(_MSC_VER)
	_mm_pause();
#elif defined(__aarch64__)
	__asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
	__builtin_ia32_pause();
#endif
}

// Publish everything written, or, while a staged lock is still being written, everything before it.
void Ring_Publish()
{
	const size_t upto = s_openStages > 0 ? s_stageStart : s_wr;
	if (upto == s_lastPub) return;
	s_lastPub = upto;
	if (DX8Ring::Mode != 1) return;
	s_pub.store(upto, std::memory_order_seq_cst);
	if (s_sleeping.load(std::memory_order_seq_cst)) {
		std::lock_guard<std::mutex> g(s_wakeMutex);
		s_wakeCv.notify_one();
	}
}

// Execute one command on the real device. Runs on the render thread (mode 1) or the main thread (mode 3).
size_t Ring_Execute(IDirect3DDevice8 *dev, const uint8_t *p)
{
	const RingHdr *h = (const RingHdr *)p;
	const uint32_t *a = (const uint32_t *)(p + sizeof(RingHdr));
	switch (h->op) {
	case RING_PAD: break;
	case RING_RS: dev->SetRenderState((D3DRENDERSTATETYPE)a[0], a[1]); break;
	case RING_TSS: dev->SetTextureStageState(a[0], (D3DTEXTURESTAGESTATETYPE)a[1], a[2]); break;
	case RING_TEX: { IDirect3DBaseTexture8 *t; memcpy(&t, a + 2, sizeof(t)); dev->SetTexture(a[0], t); break; }
	case RING_REL: { IUnknown *o; memcpy(&o, a, sizeof(o)); o->Release(); break; }
	case RING_XF: dev->SetTransform((D3DTRANSFORMSTATETYPE)a[0], (const D3DMATRIX *)(a + 2)); break;
	case RING_MAT: dev->SetMaterial((const D3DMATERIAL8 *)a); break;
	case RING_LIGHT: dev->SetLight(a[0], (const D3DLIGHT8 *)(a + 2)); break;
	case RING_LENA: dev->LightEnable(a[0], (BOOL)a[1]); break;
	case RING_VS: dev->SetVertexShader(a[0]); break;
	case RING_PS: dev->SetPixelShader(a[0]); break;
	case RING_VSC: dev->SetVertexShaderConstant(a[0], a + 2, a[1]); break;
	case RING_PSC: dev->SetPixelShaderConstant(a[0], a + 2, a[1]); break;
	case RING_CLIP: dev->SetClipPlane(a[0], (const float *)(a + 2)); break;
	case RING_STREAM: { IDirect3DVertexBuffer8 *vb; memcpy(&vb, a + 2, sizeof(vb)); dev->SetStreamSource(a[0], vb, a[1]); break; }
	case RING_IND: { IDirect3DIndexBuffer8 *ib; memcpy(&ib, a + 2, sizeof(ib)); dev->SetIndices(ib, a[0]); break; }
	case RING_DIP: dev->DrawIndexedPrimitive((D3DPRIMITIVETYPE)a[0], a[1], a[2], a[3], a[4]); break;
	case RING_CALL: {
		DX8Wrapper::DeviceCallFn fn; void *ctx;
		memcpy(&fn, a, sizeof(fn)); memcpy(&ctx, a + 2, sizeof(ctx));
		fn(dev, ctx);
		break;
	}
	case RING_VBCOPY:
	case RING_IBCOPY: {
		const RingStage *s = (const RingStage *)p;
		unsigned char *dst = nullptr;
		HRESULT hr = h->op == RING_VBCOPY
			? ((IDirect3DVertexBuffer8 *)s->buf)->Lock(s->offset, s->bytes, &dst, s->flags)
			: ((IDirect3DIndexBuffer8 *)s->buf)->Lock(s->offset, s->bytes, &dst, s->flags);
		if (SUCCEEDED(hr) && dst) {
			memcpy(dst, p + sizeof(RingStage), s->bytes);
			if (h->op == RING_VBCOPY) ((IDirect3DVertexBuffer8 *)s->buf)->Unlock();
			else ((IDirect3DIndexBuffer8 *)s->buf)->Unlock();
		}
		break;
	}
	default:
		fprintf(stderr, "[GXRING] FATAL: unknown op %u size %u\n", h->op, h->size);
		abort();
	}
	return h->size;
}

void Ring_Thread_Main(fenv_t env)
{
	fesetenv(&env);	// the FP environment is per thread; DXVK's fixed-function constants are computed under it
#if defined(__APPLE__)
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
	pthread_setname_np("gx-d3d8-ring");
#elif defined(_WIN32)
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
	size_t rd = 0;
	bool worked = false;
	while (!s_quit.load(std::memory_order_acquire)) {
		size_t pub = s_pub.load(std::memory_order_acquire);
		if (pub == rd) {
			// idle: right after a batch, spin ~100 us (the next batch of a flush is usually that close), then sleep
			if (worked && s_spinNs > 0) {
				const int64_t t0 = Ring_Now_Ns();
				int spins = 0;
				while ((pub = s_pub.load(std::memory_order_acquire)) == rd) {
					if ((++spins & 63) == 0) {
						if (Ring_Now_Ns() - t0 > s_spinNs) break;
						std::this_thread::yield();
					} else {
						Ring_Pause();
					}
				}
			}
			if (pub == rd) {
				worked = false;
				std::unique_lock<std::mutex> lk(s_wakeMutex);
				s_sleeping.store(true, std::memory_order_seq_cst);
				if (s_pub.load(std::memory_order_seq_cst) == rd && !s_quit.load(std::memory_order_acquire))
					s_wakeCv.wait_for(lk, std::chrono::milliseconds(50));	// the timeout only bounds a lost wakeup
				s_sleeping.store(false, std::memory_order_seq_cst);
				s_wakes.fetch_add(1, std::memory_order_relaxed);
				continue;
			}
		}
		worked = true;
		const int64_t b0 = Ring_Now_Ns();
		// read per batch: a device the main thread recreated was swapped while the ring was empty, and the
		// acquire of s_pub orders that write before this read
		IDirect3DDevice8 *dev = DX8Wrapper::D3DDevice_For_Ring();
		int n = 0;
		while (rd != pub) {
			rd += Ring_Execute(dev, s_ring + (rd & RING_MASK));
			if ((++n & 15) == 0) s_done.store(rd, std::memory_order_release);	// frees ring room early
			if (rd == pub) pub = s_pub.load(std::memory_order_acquire);
		}
		s_done.store(rd, std::memory_order_release);
		s_replayNs.fetch_add((uint64_t)(Ring_Now_Ns() - b0), std::memory_order_relaxed);
	}
}

size_t Ring_Done() { return DX8Ring::Mode == 1 ? s_done.load(std::memory_order_acquire) : s_doneInline; }

#if defined(_WIN32)
// Native Direct3D 8 (Windows' own d3d8.dll) wants D3DCREATE_MULTITHREADED before a second thread touches the device -
// its runtime also talks to the window-message thread - and this device is created without it. DXVK's d3d8.dll (next
// to the exe) has no such affinity. Unknown counts as native.
bool Ring_Native_D3D8()
{
	HMODULE m = GetModuleHandleA("d3d8.dll");
	char path[MAX_PATH] = { 0 };
	if (!m || !GetModuleFileNameA(m, path, MAX_PATH)) return true;
	for (char *c = path; *c; ++c) *c = (char)tolower((unsigned char)*c);
	return strstr(path, "\\system32\\") != nullptr || strstr(path, "\\syswow64\\") != nullptr;
}
#endif

// Main: wait until everything up to `upto` (published, or published now) has been executed.
void Ring_Wait(size_t upto)
{
	if (DX8Ring::Mode == 3) {
		IDirect3DDevice8 *dev = DX8Wrapper::D3DDevice_For_Ring();
		const int64_t b0 = Ring_Now_Ns();
		while (s_doneInline != upto) s_doneInline += Ring_Execute(dev, s_ring + (s_doneInline & RING_MASK));
		s_replayNs.fetch_add((uint64_t)(Ring_Now_Ns() - b0), std::memory_order_relaxed);
		return;
	}
	int spins = 0;
	while (s_done.load(std::memory_order_acquire) != upto) {
		if ((++spins & 255) == 0) std::this_thread::yield();
		else Ring_Pause();
	}
}

// Reserve n bytes (a multiple of 8) for one command, padding to the buffer end when it would wrap.
uint8_t *Ring_Alloc(uint32_t op, uint32_t n)
{
	const size_t idx = s_wr & RING_MASK;
	const size_t tail = RING_CAP - idx;
	const size_t need = (tail < n) ? n + tail : n;
	if (s_wr + need - Ring_Done() > RING_CAP) {
		if (s_openStages > 0) {
			// Ring_Stage kept RING_STAGE_SLACK free for this; only a stage holding megabytes of commands gets here
			fprintf(stderr, "[GXRING] FATAL: ring full inside a staged lock (%u bytes)\n", n);
			abort();
		}
		++s_winFull;
		DX8Ring::Drain();
	}
	if (tail < n) {
		RingHdr *pad = (RingHdr *)(s_ring + idx);
		pad->op = RING_PAD;
		pad->size = (uint32_t)tail;
		s_wr += tail;
	}
	uint8_t *p = s_ring + (s_wr & RING_MASK);
	((RingHdr *)p)->op = op;
	((RingHdr *)p)->size = n;
	s_wr += n;
	DX8Ring::Pending = true;
	++s_winCmds;
	s_winBytes += n;
	return p;
}

inline uint32_t *Ring_Args(uint32_t op, uint32_t argBytes)
{
	return (uint32_t *)(Ring_Alloc(op, (uint32_t)((sizeof(RingHdr) + argBytes + 7) & ~size_t(7))) + sizeof(RingHdr));
}

void *Ring_Stage(uint32_t op, void *buf, DWORD offset, DWORD size, DWORD flags)
{
	const uint32_t n = (uint32_t)((sizeof(RingStage) + size + 7) & ~size_t(7));
	if (s_openStages == 0) {
		// room for this copy, a nested one as big, and the slack, so nothing inside the stage has to wait
		const size_t want = (size_t)n * 2 + RING_STAGE_SLACK;
		if (s_wr + want - Ring_Done() > RING_CAP) {
			Ring_Publish();
			Ring_Wait(s_wr);
		}
		s_stageStart = s_wr;
	}
	RingStage *s = (RingStage *)Ring_Alloc(op, n);
	s->buf = buf;
	s->offset = offset;
	s->bytes = size;
	s->flags = flags;
	++s_openStages;
	s_winStageBytes += size;
	return (uint8_t *)s + sizeof(RingStage);
}
} // namespace

int DX8Ring::Mode = 0;
bool DX8Ring::Recording = false;
bool DX8Ring::Pending = false;

void DX8Ring::Init()
{
	// 0.9.61: on by default where it was measured - Windows on DXVK (Begin_Record refuses native d3d8) and macOS. iOS and
	// Linux have never run it, so they keep the direct path unless asked. GX_D3D8RING=0 = off, =3 = inline replay.
#if (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE) || !(defined(_WIN32) || defined(__APPLE__))
	const int def = 0;
#else
	const int def = 1;
#endif
	const char *e = getenv("GX_D3D8RING");
	Mode = e ? atoi(e) : def;
	s_stats = e != nullptr;
	if (Mode != 1 && Mode != 3) Mode = 0;
	if (Mode == 0) return;
	if (!s_ring) s_ring = (uint8_t *)malloc(RING_CAP);
	if (!s_ring) { Mode = 0; return; }
	if (const char *v = getenv("GX_D3D8RING_SPIN_US")) s_spinNs = (int64_t)atoi(v) * 1000;
	fprintf(stderr, "[GXRING] init mode=%d (%s) cap=%u MB spin_us=%lld\n", Mode,
		Mode == 1 ? "render thread replays the object flush" : "inline replay at the end of the object flush",
		(unsigned)(RING_CAP >> 20), (long long)(s_spinNs / 1000));
}

void DX8Ring::Shutdown()
{
	if (!s_thread) return;
	Drain();
	s_quit.store(true, std::memory_order_seq_cst);
	{
		std::lock_guard<std::mutex> g(s_wakeMutex);
		s_wakeCv.notify_one();
	}
	s_thread->join();
	delete s_thread;
	s_thread = nullptr;
	s_quit.store(false, std::memory_order_seq_cst);
	// a later Begin_Record starts a new thread reading from 0: start the ring over with it
	s_wr = s_lastPub = s_doneInline = s_stageStart = 0;
	s_pub.store(0, std::memory_order_seq_cst);
	s_done.store(0, std::memory_order_seq_cst);
	Pending = false;
}

void DX8Ring::Begin_Record()
{
	// suspended = a raw device pointer from this bracket may still be in use: a nested bracket must not re-arm recording
	// (End_Record clears it)
	if (Mode == 0 || Recording || s_suspended) return;
	if (Mode == 1 && !s_thread) {
#if defined(_WIN32)
		if (Ring_Native_D3D8()) {
			fprintf(stderr, "[GXRING] native d3d8.dll: the render thread needs DXVK - direct path\n");
			Mode = 0;
			return;
		}
#endif
		fenv_t env;
		fegetenv(&env);
		try {
			s_thread = new std::thread(Ring_Thread_Main, env);
		} catch (...) {
			// an optional optimisation must not end the game: no thread, the direct path from here on
			fprintf(stderr, "[GXRING] render thread could not start - direct path\n");
			Mode = 0;
			return;
		}
	}
	Recording = true;
	s_suspended = false;
	s_recStart = Ring_Now_Ns();
	s_midNs = 0;
}

void DX8Ring::Resume()
{
	if (!s_suspended) return;
	s_suspended = false;
	Recording = true;
	++s_winResumes;
}

void DX8Ring::End_Record()
{
	if (!Recording && !s_suspended) return;
	Recording = false;
	s_suspended = false;
	const int64_t t1 = Ring_Now_Ns();
	Drain();
	const int64_t t2 = Ring_Now_Ns();
	s_winRecNs += (uint64_t)((t1 - s_recStart) - s_midNs);
	s_winMidNs += (uint64_t)s_midNs;
	s_winDrainNs += (uint64_t)(t2 - t1);
	++s_winSpans;
}

void DX8Ring::Drain()
{
	if (!Pending) return;
	Ring_Publish();
	if (s_openStages > 0) {
		// a staged lock is still being written: run everything before it; the stage and what follows it stay
		// queued (the caller's direct call then lands before the stage's Lock/copy/Unlock, which no draw can see
		// before the matching unlock anyway)
		Ring_Wait(s_stageStart);
		return;
	}
	Ring_Wait(s_wr);
	Pending = false;
}

void DX8Ring::Fallback()
{
	// the hooks in texture code can run on the texture loader thread, which never owns the device
	if (Mode == 0 || ThreadClass::_Get_Current_Thread_ID() != DX8Wrapper::_Get_Main_Thread_ID()) return;
	if (Recording) {
		// whoever asked may keep the raw device pointer (renderStenciledPlayerColor does): record nothing more until
		// an audited Resume point, where no such pointer is alive - also when nothing is queued yet, since what the
		// caller records next would otherwise race its raw calls
		Recording = false;
		s_suspended = true;
		++s_winFallbacks;
		const int64_t t0 = Ring_Now_Ns();
		Drain();
		s_midNs += Ring_Now_Ns() - t0;
		return;
	}
	if (!Pending) return;
	{
		const int64_t t0 = Ring_Now_Ns();
		Drain();
		if (s_suspended) s_midNs += Ring_Now_Ns() - t0;
	}
}

void DX8Ring::Frame_Stats()
{
	if (Mode == 0) return;
	if (++s_winFrames < 120) return;
	const uint64_t rep = s_replayNs.load(std::memory_order_relaxed), wk = s_wakes.load(std::memory_order_relaxed);
	const double f = (double)s_winFrames;
	if (s_stats) fprintf(stderr, "[GXRING] mode=%d n=%u spans=%.1f rec_us=%.1f drain_us=%.1f mid_us=%.1f replay_us=%.1f cmds=%.0f kb=%.1f "
		"stage_kb=%.1f draws=%.0f fb=%.1f resume=%.1f full=%llu wakes=%.1f\n",
		Mode, s_winFrames, s_winSpans / f, s_winRecNs / f / 1000.0, s_winDrainNs / f / 1000.0, s_winMidNs / f / 1000.0,
		(rep - s_replayNsPrev) / f / 1000.0, s_winCmds / f, s_winBytes / f / 1024.0, s_winStageBytes / f / 1024.0,
		s_winDraws / f, s_winFallbacks / f, s_winResumes / f, (unsigned long long)s_winFull, (wk - s_wakesPrev) / f);
	s_replayNsPrev = rep;
	s_wakesPrev = wk;
	s_winRecNs = s_winDrainNs = s_winMidNs = s_winCmds = s_winBytes = s_winStageBytes = 0;
	s_winFallbacks = s_winFull = s_winSpans = s_winDraws = s_winResumes = 0;
	s_winFrames = 0;
}

void DX8Ring::Rec_Render_State(DWORD state, DWORD value) { uint32_t *a = Ring_Args(RING_RS, 8); a[0] = state; a[1] = value; }
void DX8Ring::Rec_Texture_Stage_State(DWORD stage, DWORD type, DWORD value)
{
	uint32_t *a = Ring_Args(RING_TSS, 12); a[0] = stage; a[1] = type; a[2] = value;
}
void DX8Ring::Rec_Texture(DWORD stage, IDirect3DBaseTexture8 *tex)
{
	uint32_t *a = Ring_Args(RING_TEX, 8 + sizeof(tex)); a[0] = stage; memcpy(a + 2, &tex, sizeof(tex));
}
void DX8Ring::Rec_Release(IUnknown *obj) { uint32_t *a = Ring_Args(RING_REL, sizeof(obj)); memcpy(a, &obj, sizeof(obj)); }
void DX8Ring::Rec_Call(DX8Wrapper::DeviceCallFn fn, void *ctx)
{
	static_assert(sizeof(fn) <= 8 && sizeof(ctx) <= 8, "RING_CALL packs two pointers into 16 bytes");
	uint32_t *a = Ring_Args(RING_CALL, 16); memcpy(a, &fn, sizeof(fn)); memcpy(a + 2, &ctx, sizeof(ctx));
}

// GeneralsX: WS-X hybrid spike: run fn on whichever thread owns the d3d8 device right now, in
// stream order - recorded into the ring inside the object-flush bracket, called directly otherwise (after a drain when
// commands are still pending). fn gets the device; ctx must stay valid until fn has run.
void DX8Wrapper::Device_Call(DeviceCallFn fn, void *ctx)
{
	if (DX8Ring::Recording)
		DX8Ring::Rec_Call(fn, ctx);
	else
		fn(_Get_D3D_Device8(), ctx);
}
void DX8Ring::Rec_Transform(DWORD type, const D3DMATRIX *m)
{
	uint32_t *a = Ring_Args(RING_XF, 8 + sizeof(D3DMATRIX)); a[0] = type; memcpy(a + 2, m, sizeof(D3DMATRIX));
}
void DX8Ring::Rec_Material(const D3DMATERIAL8 *mat) { memcpy(Ring_Args(RING_MAT, sizeof(*mat)), mat, sizeof(*mat)); }
void DX8Ring::Rec_Light(DWORD index, const D3DLIGHT8 *light)
{
	uint32_t *a = Ring_Args(RING_LIGHT, 8 + sizeof(*light)); a[0] = index; memcpy(a + 2, light, sizeof(*light));
}
void DX8Ring::Rec_Light_Enable(DWORD index, BOOL enable) { uint32_t *a = Ring_Args(RING_LENA, 8); a[0] = index; a[1] = enable ? 1 : 0; }
void DX8Ring::Rec_Vertex_Shader(DWORD handle) { Ring_Args(RING_VS, 4)[0] = handle; }
void DX8Ring::Rec_Pixel_Shader(DWORD handle) { Ring_Args(RING_PS, 4)[0] = handle; }
void DX8Ring::Rec_Vertex_Shader_Constant(DWORD reg, const void *data, DWORD count)
{
	uint32_t *a = Ring_Args(RING_VSC, 8 + count * 16); a[0] = reg; a[1] = count; memcpy(a + 2, data, count * 16);
}
void DX8Ring::Rec_Pixel_Shader_Constant(DWORD reg, const void *data, DWORD count)
{
	uint32_t *a = Ring_Args(RING_PSC, 8 + count * 16); a[0] = reg; a[1] = count; memcpy(a + 2, data, count * 16);
}
void DX8Ring::Rec_Clip_Plane(DWORD index, const float *plane)
{
	uint32_t *a = Ring_Args(RING_CLIP, 8 + 16); a[0] = index; memcpy(a + 2, plane, 16);
}
void DX8Ring::Rec_Stream_Source(DWORD stream, IDirect3DVertexBuffer8 *vb, DWORD stride)
{
	uint32_t *a = Ring_Args(RING_STREAM, 8 + sizeof(vb)); a[0] = stream; a[1] = stride; memcpy(a + 2, &vb, sizeof(vb));
}
void DX8Ring::Rec_Indices(IDirect3DIndexBuffer8 *ib, DWORD base)
{
	uint32_t *a = Ring_Args(RING_IND, 8 + sizeof(ib)); a[0] = base; memcpy(a + 2, &ib, sizeof(ib));
}
void DX8Ring::Rec_Draw_Indexed(DWORD type, DWORD min_index, DWORD num_vertices, DWORD start_index, DWORD prim_count)
{
	uint32_t *a = Ring_Args(RING_DIP, 20);
	a[0] = type; a[1] = min_index; a[2] = num_vertices; a[3] = start_index; a[4] = prim_count;
	++s_winDraws;
	if (s_wr - s_lastPub >= RING_PUBLISH_BYTES) Ring_Publish();
}
void *DX8Ring::Stage_VB(IDirect3DVertexBuffer8 *vb, DWORD offset, DWORD size, DWORD flags) { return Ring_Stage(RING_VBCOPY, vb, offset, size, flags); }
void *DX8Ring::Stage_IB(IDirect3DIndexBuffer8 *ib, DWORD offset, DWORD size, DWORD flags) { return Ring_Stage(RING_IBCOPY, ib, offset, size, flags); }
void DX8Ring::Stage_End()
{
	WWASSERT(s_openStages > 0);
	--s_openStages;
	if (s_openStages == 0) Ring_Publish();
}

bool DX8Wrapper::Init(void * hwnd, bool lite)
{
	WWASSERT(!IsInitted);

	// zero memory
	memset(Textures,0,sizeof(IDirect3DBaseTexture8*)*MAX_TEXTURE_STAGES);
	memset(RenderStates,0,sizeof(unsigned)*256);
	memset(TextureStageStates,0,sizeof(unsigned)*32*MAX_TEXTURE_STAGES);
	memset(Vertex_Shader_Constants,0,sizeof(Vector4)*MAX_VERTEX_SHADER_CONSTANTS);
	memset(Pixel_Shader_Constants,0,sizeof(Vector4)*MAX_PIXEL_SHADER_CONSTANTS);
	memset(&render_state,0,sizeof(RenderStateStruct));
	memset(Shadow_Map,0,sizeof(ZTextureClass*)*MAX_SHADOW_MAPS);

	/*
	** Initialize all variables!
	*/
	_Hwnd = (HWND)hwnd;
	_MainThreadID=ThreadClass::_Get_Current_Thread_ID();
	WWDEBUG_SAY(("DX8Wrapper main thread: 0x%x",_MainThreadID));
	DX8Ring::Init();
	CurRenderDevice = -1;
	ResolutionWidth = DEFAULT_RESOLUTION_WIDTH;
	ResolutionHeight = DEFAULT_RESOLUTION_HEIGHT;
	// Initialize Render2DClass Screen Resolution
	Render2DClass::Set_Screen_Resolution( RectClass( 0, 0, ResolutionWidth, ResolutionHeight ) );
	BitDepth = DEFAULT_BIT_DEPTH;
	IsWindowed = false;
	DX8Wrapper_IsWindowed = false;

	for (int light=0;light<4;++light) CurrentDX8LightEnables[light]=false;
	++LightStateSerial;	// GX_WRAPSKIP 8

	//old_vertex_shader; TODO
	//old_sr_shader;
	//current_shader;

	//world_identity;
	//CurrentFogColor;

	D3DInterface = nullptr;
	D3DDevice = nullptr;

	WWDEBUG_SAY(("Reset DX8Wrapper statistics"));
	Reset_Statistics();

	Invalidate_Cached_Render_States();

	if (!lite) {
		// GeneralsX: Platform-specific DLL/SO/DYLIB loading (Phase 5: macOS)
#ifdef _WIN32
		D3D8Lib = LoadLibrary("D3D8.DLL");
#elif defined(__APPLE__)
		fprintf(stderr, "DEBUG: DX8Wrapper::Init() - Loading libdxvk_d3d8.dylib (Apple)...\n");
		// iOS confines dlopen to the app bundle; bare names don't resolve there, so
		// load from the embedded Frameworks directory explicitly. macOS keeps the
		// bare name (resolved via DYLD_LIBRARY_PATH set by run.sh).
		#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
		// Resolve Frameworks/ from where THIS code actually lives, not from
		// @executable_path.
		//
		// @executable_path is the process's main executable. That is the app itself
		// when launched normally, but not when the app is hosted inside another
		// process — LiveContainer and similar containers dlopen the target binary,
		// so the main executable is the host and @executable_path points at the
		// host's bundle. DXVK was then never found and the game exited immediately
		// after launch. dladdr() on a symbol in our own image reports the path of
		// the binary that actually contains this code, which is correct in both
		// cases.
		{
			char frameworkPath[1024] = {0};
			Dl_info dlInfo;
			if (dladdr((const void *)&DX8Wrapper::Init, &dlInfo) != 0 && dlInfo.dli_fname != nullptr) {
				const char *slash = strrchr(dlInfo.dli_fname, '/');
				if (slash != nullptr) {
					const size_t dirLen = (size_t)(slash - dlInfo.dli_fname);
					if (dirLen < sizeof(frameworkPath) - 64) {
						memcpy(frameworkPath, dlInfo.dli_fname, dirLen);
						snprintf(frameworkPath + dirLen, sizeof(frameworkPath) - dirLen,
						         "/Frameworks/libdxvk_d3d8.0.dylib");
					}
				}
			}
			if (frameworkPath[0] != '\0') {
				fprintf(stderr, "DEBUG: DX8Wrapper::Init() - resolved DXVK path: %s\n", frameworkPath);
				D3D8Lib = LoadLibrary(frameworkPath);
			}
			// Fall back to the historical literal if resolution failed for any reason.
			if (D3D8Lib == nullptr) {
				D3D8Lib = LoadLibrary("@executable_path/Frameworks/libdxvk_d3d8.0.dylib");
			}
		}
		#else
		D3D8Lib = LoadLibrary("libdxvk_d3d8.dylib");
		#endif
		fprintf(stderr, "DEBUG: DX8Wrapper::Init() - LoadLibrary result: %p\n", (void*)D3D8Lib);
		if (D3D8Lib == nullptr) {
			const char* error = dlerror();
			fprintf(stderr, "ERROR: DX8Wrapper::Init() - dlerror(): %s\n", error ? error : "unknown");
		}
#else
		fprintf(stderr, "DEBUG: DX8Wrapper::Init() - Loading libdxvk_d3d8.so (Linux)...\n");
		D3D8Lib = LoadLibrary("libdxvk_d3d8.so");
		fprintf(stderr, "DEBUG: DX8Wrapper::Init() - LoadLibrary result: %p\n", (void*)D3D8Lib);
		if (D3D8Lib == nullptr) {
			const char* error = dlerror();
			fprintf(stderr, "ERROR: DX8Wrapper::Init() - dlerror(): %s\n", error ? error : "unknown");
		}
#endif

		if (D3D8Lib == nullptr) {
			fprintf(stderr, "ERROR: DX8Wrapper::Init() - Failed to load D3D8 library\n");
			return false;	// Return false at this point if init failed
		}

		fprintf(stderr, "DEBUG: DX8Wrapper::Init() - Getting Direct3DCreate8 function pointer...\n");
		Direct3DCreate8Ptr = (Direct3DCreate8Type) GetProcAddress(D3D8Lib, "Direct3DCreate8");
		if (Direct3DCreate8Ptr == nullptr) {
			fprintf(stderr, "ERROR: DX8Wrapper::Init() - Failed to get Direct3DCreate8 function\n");
			return false;
		}

		/*
		** Create the D3D interface object
		*/
		WWDEBUG_SAY(("Create Direct3D8"));
		fprintf(stderr, "DEBUG: DX8Wrapper::Init() - About to call Direct3DCreate8(D3D_SDK_VERSION=%d)\n", D3D_SDK_VERSION);
		fprintf(stderr, "DEBUG: DX8Wrapper::Init() - Window handle: %p\n", hwnd);
		{
			// TheSuperHackers @bugfix xezon 13/06/2025 Front load the system dbghelp.dll to prevent
			// the graphics driver from potentially loading the old game dbghelp.dll and then crashing the game process.
			DbgHelpGuard dbgHelpGuard;

			D3DInterface = Direct3DCreate8Ptr(D3D_SDK_VERSION);		// TODO: handle failure cases...
		}
		// GeneralsX: native-device session switch (Interface_Wrap_Hook in dx8wrapper.h)
		if (D3DInterface != nullptr && Interface_Wrap_Hook != nullptr) {
			IDirect3D8 *wrapped = Interface_Wrap_Hook(D3DInterface);
			if (wrapped != nullptr)
				D3DInterface = wrapped;
		}
		fprintf(stderr, "DEBUG: DX8Wrapper::Init() - Direct3DCreate8 returned: %p\n", (void*)D3DInterface);
		if (D3DInterface == nullptr) {
			fprintf(stderr, "ERROR: DX8Wrapper::Init() - Direct3DCreate8 returned NULL (DXVK failed to create D3D8 interface)\n");
			return(false);
		}
		IsInitted = true;

		/*
		** Enumerate the available devices
		*/
		WWDEBUG_SAY(("Enumerate devices"));
		Enumerate_Devices();
		WWDEBUG_SAY(("DX8Wrapper Init completed"));
	}

	return(true);
}

void DX8Wrapper::Shutdown()
{
	if (D3DDevice) {

		Set_Render_Target ((IDirect3DSurface8 *)nullptr);
		Release_Device();
	}
	DX8Ring::Shutdown();	// d3d8 ring spike: join the render thread once the device is gone

	if (D3DInterface) {
		D3DInterface->Release();
		D3DInterface=nullptr;

	}

	for (size_t i = 0; i < ARRAY_SIZE(Textures); ++i)
	{
		if (Textures[i])
		{
			Textures[i]->Release();
			Textures[i] = nullptr;
		}
	}

	// GeneralsX: Keep D3D8.DLL mapped until the process exits. GameEngine
	// destroys TheParticleSystemManager after TheGameClient (reverse init order), so its PointGroupClass
	// texture calls IDirect3DTexture8::Release after this Shutdown; with the DXVK D3D8.DLL beside the exe
	// FreeLibrary really unmapped it and every windowed Windows exit died with 0xC0000005 in
	// ~TextureBaseClass. The Mac/Linux FreeLibrary shim (CompatLib module_compat.cpp) was already a no-op.
	D3D8Lib = nullptr;

	_RenderDeviceNameTable.Clear();		 // note - Delete_All() resizes the vector, causing a reallocation.  Clear is better. jba.
	_RenderDeviceShortNameTable.Clear();
	_RenderDeviceDescriptionTable.Clear();

	DX8Caps::Shutdown();
	IsInitted = false;		// 010803 srj
}

void DX8Wrapper::Do_Onetime_Device_Dependent_Inits()
{
	// GeneralsX: A freshly created device has default (disabled) lights, and
	// Create_Device does not go through Invalidate_Cached_Render_States, so drop the light shadow here too.
	for (int light=0;light<4;++light) CurrentDX8LightValid[light]=false;
	++LightStateSerial;				// GX_WRAPSKIP 8
	Forget_Transforms();			// GX_WRAPSKIP: the same for the matrix and material shadows
	MaterialShadowValid = false;
	static const unsigned s_mask = Read_Wrap_Skip();	// read once, printed once; before the first device nothing renders
	WrapSkipMask = s_mask;

	/*
	** Set Global render states (some of which depend on caps)
	*/
	Compute_Caps(D3DFormat_To_WW3DFormat(DisplayFormat));

	// GeneralsX @diag /10/2026 One line per device creation: the formats Set_Render_Device chose and the caps they
	// produced, plus the adapter's mode list ends (the exclusive branch needs an EXACT asked=WxH match in that list). On the
	// iPad every texture format read 0 because that search failed and left DisplayFormat UNKNOWN (ios-caps-fix-1); this line
	// shows on any platform whether that happens. Reads only - nothing here feeds back into the device.
	{
		StringClass fmtDisplay(0,true);
		StringClass fmtBack(0,true);
		StringClass fmtZ(0,true);
		Get_Format_Name(DisplayFormat,&fmtDisplay);
		Get_Format_Name(_PresentParameters.BackBufferFormat,&fmtBack);
		Get_Format_Name(_PresentParameters.AutoDepthStencilFormat,&fmtZ);
		D3DDISPLAYMODE modeFirst;
		D3DDISPLAYMODE modeLast;
		::ZeroMemory(&modeFirst, sizeof(D3DDISPLAYMODE));
		::ZeroMemory(&modeLast, sizeof(D3DDISPLAYMODE));
		const UINT modeCount = D3DInterface ? D3DInterface->GetAdapterModeCount(D3DADAPTER_DEFAULT) : 0;
		if (modeCount > 0) {
			D3DInterface->EnumAdapterModes(D3DADAPTER_DEFAULT, 0, &modeFirst);
			D3DInterface->EnumAdapterModes(D3DADAPTER_DEFAULT, modeCount - 1, &modeLast);
		}
		const DX8Caps* caps = CurrentCaps;
		fprintf(stderr, "[GXCAPS] search=%d asked=%dx%d modes=%u first=%ux%u last=%ux%u display=%s backbuffer=%s z=%s stencil=%d"
			" tex A8R8G8B8=%d X8R8G8B8=%d R5G6B5=%d A1R5G5B5=%d DXT1=%d DXT3=%d DXT5=%d dxtc=%d rtt_A8R8G8B8=%d\n",
			GxModeSearch, GxModeAskedW, GxModeAskedH, modeCount,
			modeFirst.Width, modeFirst.Height, modeLast.Width, modeLast.Height,
			fmtDisplay.str(), fmtBack.str(), fmtZ.str(), Has_Stencil() ? 1 : 0,
			caps->Support_Texture_Format(WW3D_FORMAT_A8R8G8B8) ? 1 : 0,
			caps->Support_Texture_Format(WW3D_FORMAT_X8R8G8B8) ? 1 : 0,
			caps->Support_Texture_Format(WW3D_FORMAT_R5G6B5) ? 1 : 0,
			caps->Support_Texture_Format(WW3D_FORMAT_A1R5G5B5) ? 1 : 0,
			caps->Support_Texture_Format(WW3D_FORMAT_DXT1) ? 1 : 0,
			caps->Support_Texture_Format(WW3D_FORMAT_DXT3) ? 1 : 0,
			caps->Support_Texture_Format(WW3D_FORMAT_DXT5) ? 1 : 0,
			caps->Support_DXTC() ? 1 : 0,
			caps->Support_Render_To_Texture_Format(WW3D_FORMAT_A8R8G8B8) ? 1 : 0);
		fflush(stderr);
	}

   /*
	** Initialize any other subsystems inside of WW3D
	*/
	MissingTexture::_Init();
	TextureFilterClass::_Init_Filters(
		(TextureFilterClass::TextureFilterMode)WW3D::Get_Texture_Filter(),
		(TextureFilterClass::AnisotropicFilterMode)WW3D::Get_Anisotropy_Level()
	);
	TheDX8MeshRenderer.Init();
	SHD_INIT;
	BoxRenderObjClass::Init();
	VertexMaterialClass::Init();
	PointGroupClass::_Init(); // This needs the VertexMaterialClass to be initted
	ShatterSystem::Init();
	TextureLoader::Init();

	Set_Default_Global_Render_States();
}

inline DWORD F2DW(float f) { return std::bit_cast<unsigned>(f); }
void DX8Wrapper::Set_Default_Global_Render_States()
{
	DX8_THREAD_ASSERT();
	const D3DCAPS8 &caps = Get_Current_Caps()->Get_DX8_Caps();

	Set_DX8_Render_State(D3DRS_RANGEFOGENABLE, (caps.RasterCaps & D3DPRASTERCAPS_FOGRANGE) ? TRUE : FALSE);
	Set_DX8_Render_State(D3DRS_FOGTABLEMODE, D3DFOG_NONE);
	Set_DX8_Render_State(D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
	Set_DX8_Render_State(D3DRS_SPECULARMATERIALSOURCE, D3DMCS_MATERIAL);
	Set_DX8_Render_State(D3DRS_COLORVERTEX, TRUE);
	Set_DX8_Render_State(D3DRS_ZBIAS,0);
	Set_DX8_Texture_Stage_State(1, D3DTSS_BUMPENVLSCALE, F2DW(1.0f));
	Set_DX8_Texture_Stage_State(1, D3DTSS_BUMPENVLOFFSET, F2DW(0.0f));
	Set_DX8_Texture_Stage_State(0, D3DTSS_BUMPENVMAT00,F2DW(1.0f));
	Set_DX8_Texture_Stage_State(0, D3DTSS_BUMPENVMAT01,F2DW(0.0f));
	Set_DX8_Texture_Stage_State(0, D3DTSS_BUMPENVMAT10,F2DW(0.0f));
	Set_DX8_Texture_Stage_State(0, D3DTSS_BUMPENVMAT11,F2DW(1.0f));

//	Set_DX8_Render_State(D3DRS_CULLMODE, D3DCULL_CW);
	// Set dither mode here?
}

void DX8Wrapper::Invalidate_Cached_Render_States()
{
	GxLogBody gxBody(gxlog::INVALIDATE, true);	// GeneralsX: WS-G Task 5: the model ports the whole body
	DX8Ring::Drain();	// d3d8 ring spike: the device must be idle before a direct D3DDevice call
	render_state_changed=0;

	// GeneralsX: Forget the light shadow so the next Set_DX8_Light re-sends.
	// Covers Init, Reset_Device and the pillarbox end-of-frame resync.
	for (int light=0;light<4;++light) CurrentDX8LightValid[light]=false;
	++LightStateSerial;	// GX_WRAPSKIP 8

	int a;
	for (a=0;a<sizeof(RenderStates)/sizeof(unsigned);++a) {
		RenderStates[a]=0x12345678;
	}
	for (a=0;a<MAX_TEXTURE_STAGES;++a)
	{
		for (int b=0; b<32;b++)
		{
			TextureStageStates[a][b]=0x12345678;
		}
		//Need to explicitly set texture to null, otherwise app will not be able to
		//set it to null because of redundant state checker. MW
		if (_Get_D3D_Device8())
			_Get_D3D_Device8()->SetTexture(a,nullptr);
		if (Textures[a] != nullptr) {
			Textures[a]->Release();
		}
		Textures[a]=nullptr;
	}

	ShaderClass::Invalidate();

	//Need to explicitly set render_state texture pointers to null. MW
	Release_Render_State();

	// (gth) clear the matrix shadows too
	memset(&DX8Transforms, 0, sizeof(DX8Transforms));
	Forget_Transforms();
	MaterialShadowValid = false;
	s_cenLastVB = s_cenLastIB = nullptr;
	CensusLastFVF = ~0u;
}

void DX8Wrapper::Set_Transform_Dirty()
{
	memset(&DX8Transforms, 0, sizeof(DX8Transforms));
	Forget_Transforms();	// GX_WRAPSKIP 4: a caller that set the matrices behind the wrapper wants them re-sent
	render_state_changed |= (unsigned)(WORLD_CHANGED | VIEW_CHANGED);
}

void DX8Wrapper::Do_Onetime_Device_Dependent_Shutdowns()
{
	/*
	** Shutdown ww3d systems
	*/
	int i;
	for (i=0;i<MAX_VERTEX_STREAMS;++i) {
		if (render_state.vertex_buffers[i]) render_state.vertex_buffers[i]->Release_Engine_Ref();
		REF_PTR_RELEASE(render_state.vertex_buffers[i]);
	}
	if (render_state.index_buffer) render_state.index_buffer->Release_Engine_Ref();
	REF_PTR_RELEASE(render_state.index_buffer);
	REF_PTR_RELEASE(render_state.material);
	for (i=0;i<CurrentCaps->Get_Max_Textures_Per_Pass();++i) REF_PTR_RELEASE(render_state.Textures[i]);


	TextureLoader::Deinit();
	SortingRendererClass::Deinit();
	DynamicVBAccessClass::_Deinit();
	DynamicIBAccessClass::_Deinit();
	ShatterSystem::Shutdown();
	PointGroupClass::_Shutdown();
	VertexMaterialClass::Shutdown();
	BoxRenderObjClass::Shutdown();
	SHD_SHUTDOWN;
	TheDX8MeshRenderer.Shutdown();
	MissingTexture::_Deinit();

	delete CurrentCaps;
	CurrentCaps=nullptr;

}


bool DX8Wrapper::Create_Device()
{
	WWASSERT(D3DDevice==nullptr);	// for now, once you've created a device, you're stuck with it!

	D3DCAPS8 caps;
	if
	(
		FAILED
		(
			D3DInterface->GetDeviceCaps
			(
				CurRenderDevice,
				WW3D_DEVTYPE,
				&caps
			)
		)
	)
	{
		return false;
	}

	::ZeroMemory(&CurrentAdapterIdentifier, sizeof(D3DADAPTER_IDENTIFIER8));

	if
	(
		FAILED
		(
			D3DInterface->GetAdapterIdentifier
			(
				CurRenderDevice,
				D3DENUM_NO_WHQL_LEVEL,
				&CurrentAdapterIdentifier
			)
			)
	)
	{
		return false;
	}

	Vertex_Processing_Behavior=(caps.DevCaps&D3DDEVCAPS_HWTRANSFORMANDLIGHT) ?
		D3DCREATE_MIXED_VERTEXPROCESSING : D3DCREATE_SOFTWARE_VERTEXPROCESSING;

	// enable this when all 'get' dx calls are removed KJM
	/*if (caps.DevCaps&D3DDEVCAPS_PUREDEVICE)
	{
		Vertex_Processing_Behavior|=D3DCREATE_PUREDEVICE;
	}*/

#ifdef CREATE_DX8_MULTI_THREADED
	Vertex_Processing_Behavior|=D3DCREATE_MULTITHREADED;
	_DX8SingleThreaded=false;
#else
	_DX8SingleThreaded=true;
#endif

	if (DX8Wrapper_PreserveFPU)
		Vertex_Processing_Behavior |= D3DCREATE_FPU_PRESERVE;

#ifdef CREATE_DX8_FPU_PRESERVE
	Vertex_Processing_Behavior|=D3DCREATE_FPU_PRESERVE;
#endif

	// TheSuperHackers @bugfix xezon 13/06/2025 Front load the system dbghelp.dll to prevent
	// the graphics driver from potentially loading the old game dbghelp.dll and then crashing the game process.
	DbgHelpGuard dbgHelpGuard;

	HRESULT hr=D3DInterface->CreateDevice
	(
		CurRenderDevice,
		WW3D_DEVTYPE,
		_Hwnd,
		Vertex_Processing_Behavior,
		&_PresentParameters,
		&D3DDevice
	);

	if (FAILED(hr))
	{
		// The device selection may fail because the device lied that it supports 32 bit zbuffer with 16 bit
		// display. This happens at least on Voodoo2.

		if ((_PresentParameters.BackBufferFormat==D3DFMT_R5G6B5 ||
			_PresentParameters.BackBufferFormat==D3DFMT_X1R5G5B5 ||
			_PresentParameters.BackBufferFormat==D3DFMT_A1R5G5B5) &&
			(_PresentParameters.AutoDepthStencilFormat==D3DFMT_D32 ||
			_PresentParameters.AutoDepthStencilFormat==D3DFMT_D24S8 ||
			_PresentParameters.AutoDepthStencilFormat==D3DFMT_D24X8))
		{
			_PresentParameters.AutoDepthStencilFormat=D3DFMT_D16;
			hr = D3DInterface->CreateDevice
			(
				CurRenderDevice,
				WW3D_DEVTYPE,
				_Hwnd,
				Vertex_Processing_Behavior,
				&_PresentParameters,
				&D3DDevice
			);

			if (FAILED(hr))
			{
				return false;
			}
        }
		else
		{
				return false;
		}
	}

	dbgHelpGuard.deactivate();

	// GeneralsX: GXCAP: the recording proxy replaces the device before anything else sees it.
	if (Device_Wrap_Hook != nullptr)
		D3DDevice = Device_Wrap_Hook(D3DDevice, CurRenderDevice, WW3D_DEVTYPE, Vertex_Processing_Behavior, &_PresentParameters);

	/*
	** Initialize all subsystems
	*/
	Do_Onetime_Device_Dependent_Inits();
	return true;
}

bool DX8Wrapper::Reset_Device(bool reload_assets)
{
	DX8Ring::Drain();	// d3d8 ring spike: the device must be idle before a direct D3DDevice call
	if (Pre_Reset_Hook) Pre_Reset_Hook();	// WS-X: external work releases its device references
	WWDEBUG_SAY(("Resetting device."));
	DX8_THREAD_ASSERT();
	if ((IsInitted) && (D3DDevice != nullptr)) {
		// Release all non-MANAGED stuff
		WW3D::_Invalidate_Textures();

		for (unsigned i=0;i<MAX_VERTEX_STREAMS;++i)
		{
			Set_Vertex_Buffer (nullptr,i);
		}
		Set_Index_Buffer (nullptr, 0);
		if (m_pCleanupHook) {
			m_pCleanupHook->ReleaseResources();
		}
		DynamicVBAccessClass::_Deinit();
		DynamicIBAccessClass::_Deinit();
		DX8TextureManagerClass::Release_Textures();
		SHD_SHUTDOWN_SHADERS;

		// GeneralsX: Release the D3DPOOL_DEFAULT references this wrapper
		// still holds. Without this Reset() fails, and because nothing else ever drops them it
		// fails FOREVER - which is the permanent black screen after a monitor is unplugged or the
		// display configuration changes.
		//
		// Everything above releases the resources it OWNS. These four are references the wrapper
		// acquired on resources it does not own: Set_Render_Target() calls GetRenderTarget() and
		// GetDepthStencilSurface() to remember the backbuffer it is switching away from, and holds
		// the target it switched to. They are balanced by Set_Render_Target(nullptr) - which runs
		// from Pillarbox_End(), inside End_Render(). A lost device makes WW3D::Begin_Render bail
		// (ww3d.cpp), so End_Render never runs, so the balance never happens: W3DDisplay::draw
		// calls Pillarbox_Begin() BEFORE it tests Begin_Render, and its failure path only logs.
		// The offscreen texture, its surface and its depth buffer are D3DPOOL_DEFAULT too, and
		// this fork creates them, so they must go the same way.
		//
		// Released directly rather than through Set_Render_Target(nullptr): that path issues
		// SetRenderTarget through DX8CALL, and the device is by definition not in a state to
		// accept it here. The refcounts are what matter, and Reset() rebuilds the state anyway.
		if (CurrentRenderTarget) { CurrentRenderTarget->Release(); CurrentRenderTarget = nullptr; }
		if (CurrentDepthBuffer)  { CurrentDepthBuffer->Release();  CurrentDepthBuffer  = nullptr; }
		if (DefaultRenderTarget) { DefaultRenderTarget->Release(); DefaultRenderTarget = nullptr; }
		if (DefaultDepthBuffer)  { DefaultDepthBuffer->Release();  DefaultDepthBuffer  = nullptr; }

		Pillarbox_Cleanup();
		if (Default_Pool_Release_Hook) Default_Pool_Release_Hook();	// render-hud-seam-1: the world/HUD seam's targets

		// Reset frame count to reflect the flipping chain being reset by Reset()
		FrameCount = 0;

		memset(Vertex_Shader_Constants,0,sizeof(Vector4)*MAX_VERTEX_SHADER_CONSTANTS);
		memset(Pixel_Shader_Constants,0,sizeof(Vector4)*MAX_PIXEL_SHADER_CONSTANTS);

		HRESULT hr=_Get_D3D_Device8()->TestCooperativeLevel();
		if (hr != D3DERR_DEVICELOST )
		{	DX8CALL_HRES(Reset(&_PresentParameters),hr)
			Gx_Capture_Forget();	// WS-G: the device's material / projection / texture matrices are its defaults now, or unknown
			if (hr != D3D_OK) {
				Gx_Truth_Forget();	// WS-G
				return false;	//reset failed.
			}
		}
		else {
			Gx_Truth_Forget();	// WS-G
			return false;	//device is lost and can't be reset.
		}
		Gx_Truth_Reseed();	// WS-G: Reset restored DXVK's default state (no-op unless the L2 check tracks the device truth)

		if (reload_assets)
		{
			DX8TextureManagerClass::Recreate_Textures();
			if (m_pCleanupHook) {
				m_pCleanupHook->ReAcquireResources();
			}
		}
		Invalidate_Cached_Render_States();
		Set_Default_Global_Render_States();
		SHD_INIT_SHADERS;
		// GeneralsX: Re-create the pillarbox surfaces released above. Same
		// order and the same unconditional call Set_Device_Resolution already uses: cleanup, reset,
		// then setup only once the reset has actually worked. Unconditional is right because
		// Pillarbox_Setup decides for itself - it returns early when the backbuffer already matches
		// the game resolution - so this cannot create a pillarbox where one was not wanted.
		//
		// Deliberately NOT gated on "was it enabled before". That flag is cleared by the cleanup
		// above and is not restored when a reset FAILS, so a first attempt that returns false at the
		// cooperative-level check would leave the pillarbox permanently off on the attempt that
		// finally succeeds. The redundant Setup this costs Set_Device_Resolution is harmless -
		// Pillarbox_Setup begins with Pillarbox_Cleanup - and a resolution change is already the
		// expensive path.
		//
		// GeneralsX: DECISION: return deliberately discarded. Reset_Device's own
		// bool answers "was the device reset", and by the time we reach this line the reset genuinely did
		// succeed - the pillarbox is a consequence of the reset, not a part of it. Propagating is also
		// unimplementable as the return currently stands: false is what the healthy 1:1 exit returns, so
		// this function would begin reporting a failed device reset on every machine whose resolution
		// matches its display. Failure is reported by the ERROR line inside Pillarbox_Setup instead.
		Pillarbox_Setup(ResolutionWidth, ResolutionHeight);
		WWDEBUG_SAY(("Device reset completed"));
		return true;
	}
	WWDEBUG_SAY(("Device reset failed"));
	return false;
}

void DX8Wrapper::Release_Device()
{
	DX8Ring::Drain();	// d3d8 ring spike: the device must be idle before a direct D3DDevice call
	if (Pre_Release_Hook) Pre_Release_Hook();	// WS-X: external work drains and drops its device references (GxInterop::Shutdown)
	Pillarbox_Cleanup();
	if (Default_Pool_Release_Hook) Default_Pool_Release_Hook();	// render-hud-seam-1: the world/HUD seam's targets

	if (D3DDevice) {

		for (int a=0;a<MAX_TEXTURE_STAGES;++a)
		{	//release references to any textures that were used in last rendering call
			DX8CALL(SetTexture(a,nullptr));
		}

		DX8CALL(SetStreamSource(0, nullptr, 0));	//release reference count on last rendered vertex buffer
		DX8CALL(SetIndices(nullptr,0));	//release reference count on last rendered index buffer


		/*
		** Release the current vertex and index buffers
		*/
		for (unsigned i=0;i<MAX_VERTEX_STREAMS;++i)
		{
			if (render_state.vertex_buffers[i]) render_state.vertex_buffers[i]->Release_Engine_Ref();
			REF_PTR_RELEASE(render_state.vertex_buffers[i]);
		}
		if (render_state.index_buffer) render_state.index_buffer->Release_Engine_Ref();
		REF_PTR_RELEASE(render_state.index_buffer);

		/*
		** Shutdown all subsystems
		*/
		Do_Onetime_Device_Dependent_Shutdowns();

		/*
		** Release the device
		*/

		D3DDevice->Release();
		D3DDevice=nullptr;
	}
}

void DX8Wrapper::Enumerate_Devices()
{
	DX8_Assert();

	int adapter_count = D3DInterface->GetAdapterCount();
	for (int adapter_index=0; adapter_index<adapter_count; adapter_index++) {

		D3DADAPTER_IDENTIFIER8 id;
		::ZeroMemory(&id, sizeof(D3DADAPTER_IDENTIFIER8));
		HRESULT res = D3DInterface->GetAdapterIdentifier(adapter_index,D3DENUM_NO_WHQL_LEVEL,&id);

		if (res == D3D_OK) {

			/*
			** Set up the render device description
			** TODO: Fill in more fields of the render device description?  (need some lookup tables)
			*/
			RenderDeviceDescClass desc;
			desc.set_device_name(id.Description);
			desc.set_driver_name(id.Driver);

			char buf[64];
			sprintf(buf,"%d.%d.%d.%d", //"%04x.%04x.%04x.%04x",
				HIWORD(id.DriverVersion.HighPart),
				LOWORD(id.DriverVersion.HighPart),
				HIWORD(id.DriverVersion.LowPart),
				LOWORD(id.DriverVersion.LowPart));

			desc.set_driver_version(buf);

			D3DInterface->GetDeviceCaps(adapter_index,WW3D_DEVTYPE,&desc.Caps);
			D3DInterface->GetAdapterIdentifier(adapter_index,D3DENUM_NO_WHQL_LEVEL,&desc.AdapterIdentifier);

			DX8Caps dx8caps(D3DInterface,desc.Caps,WW3D_FORMAT_UNKNOWN,desc.AdapterIdentifier);

			/*
			** Enumerate the resolutions
			*/
			desc.reset_resolution_list();
			int mode_count = D3DInterface->GetAdapterModeCount(adapter_index);
			for (int mode_index=0; mode_index<mode_count; mode_index++) {
				D3DDISPLAYMODE d3dmode;
				::ZeroMemory(&d3dmode, sizeof(D3DDISPLAYMODE));
				HRESULT res = D3DInterface->EnumAdapterModes(adapter_index,mode_index,&d3dmode);

				if (res == D3D_OK) {
					int bits = 0;
					switch (d3dmode.Format)
					{
						case D3DFMT_R8G8B8:
						case D3DFMT_A8R8G8B8:
						case D3DFMT_X8R8G8B8:		bits = 32; break;

						case D3DFMT_R5G6B5:
						case D3DFMT_X1R5G5B5:		bits = 16; break;
					}

					// Some cards fail in certain modes, DX8Caps keeps list of those.
					if (!dx8caps.Is_Valid_Display_Format(d3dmode.Width,d3dmode.Height,D3DFormat_To_WW3DFormat(d3dmode.Format))) {
						bits=0;
					}

					/*
					** If we recognize the format, add it to the list
					** TODO: should we handle more formats?  will any cards report more than 24 or 16 bit?
					*/
					if (bits != 0) {
						desc.add_resolution(d3dmode.Width,d3dmode.Height,bits);
					}
				}
			}

			// IML: If the device has one or more valid resolutions add it to the device list.
			// NOTE: Testing has shown that there are drivers with zero resolutions.
			if (desc.Enumerate_Resolutions().Count() > 0) {

				/*
				** Set up the device name
				*/
				StringClass device_name(id.Description,true);
				_RenderDeviceNameTable.Add(device_name);
				_RenderDeviceShortNameTable.Add(device_name);	// for now, just add the same name to the "pretty name table"

				/*
				** Add the render device to our table
				*/
				_RenderDeviceDescriptionTable.Add(desc);
			}
		}
	}
}

bool DX8Wrapper::Set_Any_Render_Device()
{
	// Try fullscreen first
	int dev_number = 0;
	for (; dev_number < _RenderDeviceNameTable.Count(); dev_number++) {
		if (Set_Render_Device(dev_number,-1,-1,-1,0,false)) {
			return true;
		}
	}

	// Then windowed
	for (dev_number = 0; dev_number < _RenderDeviceNameTable.Count(); dev_number++) {
		if (Set_Render_Device(dev_number,-1,-1,-1,1,false)) {
			return true;
		}
	}

	return false;
}

bool DX8Wrapper::Set_Render_Device
(
	const char * dev_name,
	int width,
	int height,
	int bits,
	int windowed,
	bool resize_window
)
{
	for ( int dev_number = 0; dev_number < _RenderDeviceNameTable.Count(); dev_number++) {
		if ( strcmp( dev_name, _RenderDeviceNameTable[dev_number]) == 0) {
			return Set_Render_Device( dev_number, width, height, bits, windowed, resize_window );
		}

		if ( strcmp( dev_name, _RenderDeviceShortNameTable[dev_number]) == 0) {
			return Set_Render_Device( dev_number, width, height, bits, windowed, resize_window );
		}
	}
	return false;
}

void DX8Wrapper::Get_Format_Name(unsigned int format, StringClass *tex_format)
{
		*tex_format="Unknown";
		switch (format) {
		case D3DFMT_A8R8G8B8: *tex_format="D3DFMT_A8R8G8B8"; break;
		case D3DFMT_R8G8B8: *tex_format="D3DFMT_R8G8B8"; break;
		case D3DFMT_A4R4G4B4: *tex_format="D3DFMT_A4R4G4B4"; break;
		case D3DFMT_A1R5G5B5: *tex_format="D3DFMT_A1R5G5B5"; break;
		case D3DFMT_R5G6B5: *tex_format="D3DFMT_R5G6B5"; break;
		case D3DFMT_L8: *tex_format="D3DFMT_L8"; break;
		case D3DFMT_A8: *tex_format="D3DFMT_A8"; break;
		case D3DFMT_P8: *tex_format="D3DFMT_P8"; break;
		case D3DFMT_X8R8G8B8: *tex_format="D3DFMT_X8R8G8B8"; break;
		case D3DFMT_X1R5G5B5: *tex_format="D3DFMT_X1R5G5B5"; break;
		case D3DFMT_R3G3B2: *tex_format="D3DFMT_R3G3B2"; break;
		case D3DFMT_A8R3G3B2: *tex_format="D3DFMT_A8R3G3B2"; break;
		case D3DFMT_X4R4G4B4: *tex_format="D3DFMT_X4R4G4B4"; break;
		case D3DFMT_A8P8: *tex_format="D3DFMT_A8P8"; break;
		case D3DFMT_A8L8: *tex_format="D3DFMT_A8L8"; break;
		case D3DFMT_A4L4: *tex_format="D3DFMT_A4L4"; break;
		case D3DFMT_V8U8: *tex_format="D3DFMT_V8U8"; break;
		case D3DFMT_L6V5U5: *tex_format="D3DFMT_L6V5U5"; break;
		case D3DFMT_X8L8V8U8: *tex_format="D3DFMT_X8L8V8U8"; break;
		case D3DFMT_Q8W8V8U8: *tex_format="D3DFMT_Q8W8V8U8"; break;
		case D3DFMT_V16U16: *tex_format="D3DFMT_V16U16"; break;
		case D3DFMT_W11V11U10: *tex_format="D3DFMT_W11V11U10"; break;
		case D3DFMT_UYVY: *tex_format="D3DFMT_UYVY"; break;
		case D3DFMT_YUY2: *tex_format="D3DFMT_YUY2"; break;
		case D3DFMT_DXT1: *tex_format="D3DFMT_DXT1"; break;
		case D3DFMT_DXT2: *tex_format="D3DFMT_DXT2"; break;
		case D3DFMT_DXT3: *tex_format="D3DFMT_DXT3"; break;
		case D3DFMT_DXT4: *tex_format="D3DFMT_DXT4"; break;
		case D3DFMT_DXT5: *tex_format="D3DFMT_DXT5"; break;
		case D3DFMT_D16_LOCKABLE: *tex_format="D3DFMT_D16_LOCKABLE"; break;
		case D3DFMT_D32: *tex_format="D3DFMT_D32"; break;
		case D3DFMT_D15S1: *tex_format="D3DFMT_D15S1"; break;
		case D3DFMT_D24S8: *tex_format="D3DFMT_D24S8"; break;
		case D3DFMT_D16: *tex_format="D3DFMT_D16"; break;
		case D3DFMT_D24X8: *tex_format="D3DFMT_D24X8"; break;
		case D3DFMT_D24X4S4: *tex_format="D3DFMT_D24X4S4"; break;
		default:	break;
		}
}

void DX8Wrapper::Resize_And_Position_Window()
{
#ifdef _WIN32
	// GeneralsX: BORDERLESS: the window IS the monitor, whatever the game
	// resolution - the backbuffer follows the client size and the game is pillarboxed into it. The
	// monitor is the one the window is on, which WinMain created centred on the PRIMARY display
	// (initializeAppWindows, SM_CXSCREEN). NOT topmost, unlike exclusive: a borderless window must
	// let Alt+Tab and a second monitor work, which is the reason to want it.
	if (!IsWindowed && Borderless) {
		Borderless_Cover_Monitor(_Hwnd, true);
		return;
	}
#endif

	// Get the current dimensions of the 'render area' of the window
	RECT rect = { 0 };
	::GetClientRect (_Hwnd, &rect);

	// Is the window the correct size for this resolution?
	if ((rect.right-rect.left) != ResolutionWidth ||
			(rect.bottom-rect.top) != ResolutionHeight) {

		// Calculate what the main window's bounding rectangle should be to
		// accommodate this resolution
		rect.left = 0;
		rect.top = 0;
		rect.right = ResolutionWidth;
		rect.bottom = ResolutionHeight;
		DWORD dwstyle = ::GetWindowLong (_Hwnd, GWL_STYLE);
		AdjustWindowRect (&rect, dwstyle, FALSE);
		int width = rect.right-rect.left;
		int height = rect.bottom-rect.top;

		// Resize the window to fit this resolution
		if (!IsWindowed)
		{
			#ifdef _WIN32
			::SetWindowPos(_Hwnd, HWND_TOPMOST, 0, 0, width, height, 0);

			DEBUG_LOG(("Window resized to w:%d h:%d", width, height));
			#else
			// GeneralsX: On SDL platforms, fullscreen size is controlled by SDL display mode.
			// Forcing window size here can lock fullscreen transitions to the game resolution (for example 1024x768).
			DEBUG_LOG(("Skipping explicit fullscreen resize on SDL platform"));
			#endif
		}
		else
		{
#ifdef _WIN32
			// TheSuperHackers @feature helmutbuhler 14/04/2025
			// Center the window in the workarea of the monitor it is on.
			MONITORINFO mi = {sizeof(MONITORINFO)};
			GetMonitorInfo(MonitorFromWindow(_Hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
			int left = (mi.rcWork.left + mi.rcWork.right - width) / 2;
			int top  = (mi.rcWork.top + mi.rcWork.bottom - height) / 2;

			// TheSuperHackers @feature helmutbuhler 14/04/2025
			// Move the window to try fit it into the monitor area, if one of its dimensions is larger than the work area.
			// Otherwise align the window to the top left edges, if it is even larger than the monitor area.
			RECT rectClient;
			rectClient.left = left - rect.left;
			rectClient.top = top - rect.top;
			rectClient.right = rectClient.left + ResolutionWidth;
			rectClient.bottom = rectClient.top + ResolutionHeight;
			MoveRectIntoOtherRect(rectClient, mi.rcMonitor, &left, &top);

			::SetWindowPos (_Hwnd, nullptr, left, top, width, height, SWP_NOZORDER);

			DEBUG_LOG(("Window positioned to x:%d y:%d, resized to w:%d h:%d", left, top, width, height));
#else
			// GeneralsX: SDL3 window management on Linux (no Monitor API needed)
			::SetWindowPos (_Hwnd, nullptr, 0, 0, width, height, SWP_NOZORDER);

			DEBUG_LOG(("Window resized (Linux/SDL) to w:%d h:%d", width, height));
#endif
		}
	}
}

bool DX8Wrapper::Set_Render_Device(int dev, int width, int height, int bits, int windowed,
								   bool resize_window,bool reset_device, bool restore_assets)
{
	WWASSERT(IsInitted);
	WWASSERT(dev >= -1);
	WWASSERT(dev < _RenderDeviceNameTable.Count());

	/*
	** If user has never selected a render device, start out with device 0
	*/
	if ((CurRenderDevice == -1) && (dev == -1)) {
		CurRenderDevice = 0;
	} else if (dev != -1) {
		CurRenderDevice = dev;
	}

	/*
	** If user doesn't want to change res, set the res variables to match the
	** current resolution
	*/
	if (width != -1)		ResolutionWidth = width;
	if (height != -1)		ResolutionHeight = height;

	if (bits != -1)		BitDepth = bits;
	if (windowed != -1)	IsWindowed = (windowed != 0);
	DX8Wrapper_IsWindowed = IsWindowed;

	WWDEBUG_SAY(("Attempting Set_Render_Device: name: %s (%s:%s), width: %d, height: %d, windowed: %d",
		_RenderDeviceNameTable[CurRenderDevice].str(),_RenderDeviceDescriptionTable[CurRenderDevice].Get_Driver_Name(),
		_RenderDeviceDescriptionTable[CurRenderDevice].Get_Driver_Version(),ResolutionWidth,ResolutionHeight,(IsWindowed ? 1 : 0)));

	// PWG 4/13/2000 - changed so that if you say to resize the window it resizes
	// regardless of whether its windowed or not as OpenGL resizes its self around
	// the caption and edges of the window type you provide, so its important to
	// push the client area to be the size you really want.
	// if ( resize_window && windowed ) {
	if (resize_window) {
		Resize_And_Position_Window();
	}
	//must be either resetting existing device or creating a new one.
	WWASSERT(reset_device || D3DDevice == nullptr);

	/*
	** Initialize values for D3DPRESENT_PARAMETERS members.
	*/
	::ZeroMemory(&_PresentParameters, sizeof(D3DPRESENT_PARAMETERS));

	Resolve_Present_BackBuffer_Size(
		ResolutionWidth,
		ResolutionHeight,
		IsWindowed,
		_PresentParameters.BackBufferWidth,
		_PresentParameters.BackBufferHeight);
	_PresentParameters.BackBufferCount = IsWindowed ? 1 : 2;

	//I changed this to discard all the time (even when full-screen) since that the most efficient. 07-16-03 MW:
	_PresentParameters.SwapEffect = D3DSWAPEFFECT_DISCARD;//IsWindowed ? D3DSWAPEFFECT_DISCARD : D3DSWAPEFFECT_FLIP;		// Shouldn't this be D3DSWAPEFFECT_FLIP?
	_PresentParameters.hDeviceWindow = _Hwnd;
	#ifndef _WIN32
	// GeneralsX @bugfix xorza 14/04/2026 Force DXVK to use windowed presentation mode on Linux.
	// DXVK's SDL3 WSI calls SDL_SetWindowPosition during fullscreen entry which Wayland rejects.
	// SDL3 native fullscreen is applied separately after device creation (see W3DDisplay::init).
	_PresentParameters.Windowed = TRUE;
	#else
	// GeneralsX: Borderless presents windowed, like the SDL3 platforms above.
	_PresentParameters.Windowed = IsWindowed || Win32_Presents_Borderless();
	#endif

	// GeneralsX: Vsync for a windowed present - see Set_Windowed_VSync. DXVK's d3d8
	// front end accepts COPY_VSYNC with the two back buffers a non-windowed SDL3 device asks for (only plain
	// COPY is refused, d3d8_interface.cpp), and a windowed swapchain must keep the DEFAULT interval above.
	// Native D3D8 allows multisampling only with DISCARD; DXVK checks no such pairing (d3d9_interface.cpp
	// ValidatePresentationParameters), and its only other use of the swap effect is buffer rotation
	// (D3D9SwapChainEx::SwapWithFrontBuffer), where a windowed single-buffer COPY and DISCARD are the same
	// path - so AntiAliasing keeps working. Only macOS, which is always DXVK, asks for this.
	if (_PresentParameters.Windowed && WindowedVSync)
		_PresentParameters.SwapEffect = D3DSWAPEFFECT_COPY_VSYNC;

	_PresentParameters.EnableAutoDepthStencil = TRUE;				// Driver will attempt to match Z-buffer depth
	_PresentParameters.Flags=0;											// We're not going to lock the backbuffer

	_PresentParameters.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
	_PresentParameters.FullScreen_RefreshRateInHz = D3DPRESENT_RATE_DEFAULT;

	/*
	** Set up the buffer formats.  Several issues here:
	** - if in windowed mode, the backbuffer must use the current display format.
	** - the depth buffer must use
	*/
	// GeneralsX: Windows borderless is a windowed present, so it takes the
	// windowed branch's desktop format. The SDL3 platforms keep the exclusive branch they always had.
	if (IsWindowed || Win32_Presents_Borderless()) {

		GxModeSearch = -1;
		D3DDISPLAYMODE desktop_mode;
		::ZeroMemory(&desktop_mode, sizeof(D3DDISPLAYMODE));
		D3DInterface->GetAdapterDisplayMode( CurRenderDevice, &desktop_mode );

		DisplayFormat=_PresentParameters.BackBufferFormat = desktop_mode.Format;

		// In windowed mode, define the bitdepth from desktop mode (as it can't be changed)
		switch (_PresentParameters.BackBufferFormat) {
		case D3DFMT_X8R8G8B8:
		case D3DFMT_A8R8G8B8:
		case D3DFMT_R8G8B8: BitDepth=32; break;
		case D3DFMT_A4R4G4B4:
		case D3DFMT_A1R5G5B5:
		case D3DFMT_R5G6B5: BitDepth=16; break;
		case D3DFMT_L8:
		case D3DFMT_A8:
		case D3DFMT_P8: BitDepth=8; break;
		default:
			// Unknown backbuffer format probably means the device can't do windowed
			return false;
		}

		if (BitDepth==32 && D3DInterface->CheckDeviceType(0,D3DDEVTYPE_HAL,desktop_mode.Format,D3DFMT_A8R8G8B8, TRUE) == D3D_OK)
		{	//promote 32-bit modes to include destination alpha
			_PresentParameters.BackBufferFormat = D3DFMT_A8R8G8B8;
		}

		/*
		** Find a appropriate Z buffer
		*/
		if (!Find_Z_Mode(DisplayFormat,_PresentParameters.BackBufferFormat,&_PresentParameters.AutoDepthStencilFormat))
		{
			// If opening 32 bit mode failed, try 16 bit, even if the desktop happens to be 32 bit
			if (BitDepth==32) {
				BitDepth=16;
				_PresentParameters.BackBufferFormat=D3DFMT_R5G6B5;
				if (!Find_Z_Mode(_PresentParameters.BackBufferFormat,_PresentParameters.BackBufferFormat,&_PresentParameters.AutoDepthStencilFormat)) {
					_PresentParameters.AutoDepthStencilFormat=D3DFMT_UNKNOWN;
				}
			}
			else {
				_PresentParameters.AutoDepthStencilFormat=D3DFMT_UNKNOWN;
			}
		}

	} else {

		/*
		** Try to find a mode that matches the user's desired bit-depth.
		*/
		GxModeAskedW = ResolutionWidth;
		GxModeAskedH = ResolutionHeight;
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
		// A failed search writes nothing, so a format left by an earlier call (a device reset) would hide the failure from
		// the fallback below. ios-caps-fix-1.
		DisplayFormat = D3DFMT_UNKNOWN;
#endif
		GxModeSearch = Find_Color_And_Z_Mode(ResolutionWidth,ResolutionHeight,BitDepth,&DisplayFormat,
			&_PresentParameters.BackBufferFormat,&_PresentParameters.AutoDepthStencilFormat) ? 1 : 0;

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
		// GeneralsX: The iPad is always "fullscreen" here, and the exact-mode search above can never
		// match on it: the game asks for its PIXEL size, while DXVK's SDL3 WSI lists modes in SDL's POINTS (mode.w/h; the
		// iPad's density is 2). The search failed, its result was ignored, and DisplayFormat, the back buffer and the depth
		// format all stayed UNKNOWN - so Compute_Caps reported EVERY texture, render-target and depth format unsupported
		// (DXT decompressed to 32-bit, a 16-bit terrain atlas, no render targets) and the depth buffer defaulted to D32 with
		// no stencil. The SDL3 present is windowed on every non-Win32 platform (above), so the format really in effect is
		// the adapter's current display mode: take it, exactly as a successful search would have used a matching mode -
		// promote X8R8G8B8 to A8R8G8B8, then the same Z search (D24S8 first). iOS only: other platforms find their mode
		// or keep today's behaviour unchanged. ios-caps-fix-1.
		if (DisplayFormat == D3DFMT_UNKNOWN) {
			D3DDISPLAYMODE current_mode;
			::ZeroMemory(&current_mode, sizeof(D3DDISPLAYMODE));
			if (SUCCEEDED(D3DInterface->GetAdapterDisplayMode(CurRenderDevice, &current_mode))
				&& current_mode.Format != D3DFMT_UNKNOWN)
			{
				DisplayFormat = _PresentParameters.BackBufferFormat = current_mode.Format;
				if (BitDepth == 32 && DisplayFormat == D3DFMT_X8R8G8B8
					&& D3DInterface->CheckDeviceType(0, D3DDEVTYPE_HAL, DisplayFormat, D3DFMT_A8R8G8B8, TRUE) == D3D_OK)
				{
					_PresentParameters.BackBufferFormat = D3DFMT_A8R8G8B8;
				}
				if (!Find_Z_Mode(DisplayFormat, _PresentParameters.BackBufferFormat, &_PresentParameters.AutoDepthStencilFormat)) {
					_PresentParameters.AutoDepthStencilFormat = D3DFMT_UNKNOWN;
				}
				GxModeSearch = 2;
			}
		}
#endif
	}

	/*
	** Set default for depth stencil format if auto Z buffer failed.
	*/
	if (_PresentParameters.AutoDepthStencilFormat==D3DFMT_UNKNOWN) {
		if (BitDepth==32) {
			_PresentParameters.AutoDepthStencilFormat=D3DFMT_D32;
		}
		else {
			_PresentParameters.AutoDepthStencilFormat=D3DFMT_D16;
		}
	}

	/*
	** Check the devices support for the requested MSAA mode then setup the multi sample type
	*/
	// GeneralsX: Step DOWN to the highest level the GPU supports (8 -> 4 -> 2) instead
	// of dropping straight to none. Apple GPUs have no 8x through MoltenVK, so "MSAA 8x" there used to mean
	// no anti-aliasing at all while the launcher promised the card "may lower the level it gets".
	while (MultiSampleAntiAliasing > D3DMULTISAMPLE_NONE) {

		HRESULT hrBack = D3DInterface->CheckDeviceMultiSampleType(
			CurRenderDevice,
			D3DDEVTYPE_HAL,
			_PresentParameters.BackBufferFormat,
			IsWindowed || Win32_Presents_Borderless(),
			MultiSampleAntiAliasing
		);

		HRESULT hrDepth = D3DInterface->CheckDeviceMultiSampleType(
			CurRenderDevice,
			D3DDEVTYPE_HAL,
			_PresentParameters.AutoDepthStencilFormat,
			IsWindowed || Win32_Presents_Borderless(),
			MultiSampleAntiAliasing
		);

		if (SUCCEEDED(hrBack) && SUCCEEDED(hrDepth)) {
			break;
		}
		// External code needs to retrieve the configured MSAA mode after device creation
		WWDEBUG_SAY(("Requested MSAA Mode Not Supported"));
		MultiSampleAntiAliasing = MultiSampleAntiAliasing > D3DMULTISAMPLE_2_SAMPLES
			? (D3DMULTISAMPLE_TYPE)(MultiSampleAntiAliasing / 2) : D3DMULTISAMPLE_NONE;
	}

	_PresentParameters.MultiSampleType = MultiSampleAntiAliasing;

	/*
	** Time to actually create the device.
	*/
	StringClass displayFormat;
	StringClass backbufferFormat;

	Get_Format_Name(DisplayFormat,&displayFormat);
	Get_Format_Name(_PresentParameters.BackBufferFormat,&backbufferFormat);

	WWDEBUG_SAY(("Using Display/BackBuffer Formats: %s/%s",displayFormat.str(),backbufferFormat.str()));

	bool ret;

	if (reset_device)
	{
		WWDEBUG_SAY(("DX8Wrapper::Set_Render_Device is resetting the device."));
		ret = Reset_Device(restore_assets);	//reset device without restoring data - we're likely switching out of the app.
	}
	else
		ret = Create_Device();

	WWDEBUG_SAY(("Reset/Create_Device done, reset_device=%d, restore_assets=%d", reset_device, restore_assets));

	if (ret)
	{
		Render2DClass::Set_Screen_Resolution( RectClass( 0, 0, ResolutionWidth, ResolutionHeight ) );
		// GeneralsX: DECISION: return deliberately discarded. `ret` is the
		// Create_Device/Reset_Device result and stays that way; the device really was created.
		//
		// This is where the disagreement the ERROR line describes actually gets assembled: the line
		// directly above pins Render2DClass to the GAME resolution, and if Pillarbox_Setup then fails
		// nothing moves it back, so the scene goes to a window-sized backbuffer while Render2D still
		// measures in game units. Making `ret` false here would not repair that - it would abort a
		// device creation that succeeded, and would do so on every 1:1 machine because of the shared
		// false. See the contract note above Pillarbox_Setup.
		Pillarbox_Setup(ResolutionWidth, ResolutionHeight);
	}

	return ret;
}

bool DX8Wrapper::Set_Next_Render_Device()
{
	int new_dev = (CurRenderDevice + 1) % _RenderDeviceNameTable.Count();
	return Set_Render_Device(new_dev);
}

bool DX8Wrapper::Toggle_Windowed()
{
#ifdef WW3D_DX8
	// State OK?
	assert (IsInitted);
	if (IsInitted) {

		// Get information about the current render device's resolutions
		const RenderDeviceDescClass &render_device = Get_Render_Device_Desc ();
		const DynamicVectorClass<ResolutionDescClass> &resolutions = render_device.Enumerate_Resolutions ();

		// Loop through all the resolutions supported by the current device.
		// If we aren't currently running under one of these resolutions,
		// then we should probably		 to the closest resolution before
		// toggling the windowed state.
		int curr_res = -1;
		for (int res = 0;
		     (res < resolutions.Count ()) && (curr_res == -1);
			  res ++) {

			// Is this the resolution we are looking for?
			if ((resolutions[res].Width == ResolutionWidth) &&
				 (resolutions[res].Height == ResolutionHeight) &&
				 (resolutions[res].BitDepth == BitDepth)) {
				curr_res = res;
			}
		}

		if (curr_res == -1) {

			// We don't match any of the standard resolutions,
			// so set the first resolution and toggle the windowed state.
			return Set_Device_Resolution (resolutions[0].Width,
								 resolutions[0].Height,
								 resolutions[0].BitDepth,
								 !IsWindowed, true);
		} else {

			// Toggle the windowed state
			return Set_Device_Resolution (-1, -1, -1, !IsWindowed, true);
		}
	}
#endif //WW3D_DX8

	return false;
}

void DX8Wrapper::Set_Swap_Interval(int swap)
{
	switch (swap) {
		case 0: _PresentParameters.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE; break;
		case 1: _PresentParameters.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE ; break;
		case 2: _PresentParameters.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_TWO; break;
		case 3: _PresentParameters.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_THREE; break;
		default: _PresentParameters.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE ; break;
	}

	WWDEBUG_SAY(("DX8Wrapper::Set_Swap_Interval is resetting the device."));
	Reset_Device();
}

int DX8Wrapper::Get_Swap_Interval()
{
	return _PresentParameters.FullScreen_PresentationInterval;
}

bool DX8Wrapper::Has_Stencil()
{
	bool has_stencil = (_PresentParameters.AutoDepthStencilFormat == D3DFMT_D24S8 ||
						_PresentParameters.AutoDepthStencilFormat == D3DFMT_D24X4S4);
	return has_stencil;
}

int DX8Wrapper::Get_Render_Device_Count()
{
	return _RenderDeviceNameTable.Count();

}
int DX8Wrapper::Get_Render_Device()
{
	assert(IsInitted);
	return CurRenderDevice;
}

const RenderDeviceDescClass & DX8Wrapper::Get_Render_Device_Desc(int deviceidx)
{
	WWASSERT(IsInitted);

	if ((deviceidx == -1) && (CurRenderDevice == -1)) {
		CurRenderDevice = 0;
	}

	// if the device index is -1 then we want the current device
	if (deviceidx == -1) {
		WWASSERT(CurRenderDevice >= 0);
		WWASSERT(CurRenderDevice < _RenderDeviceNameTable.Count());
		return _RenderDeviceDescriptionTable[CurRenderDevice];
	}

	// We can only ask for multiple device information if the devices
	// have been detected.
	WWASSERT(deviceidx >= 0);
	WWASSERT(deviceidx < _RenderDeviceNameTable.Count());
	return _RenderDeviceDescriptionTable[deviceidx];
}

const char * DX8Wrapper::Get_Render_Device_Name(int device_index)
{
	device_index = device_index % _RenderDeviceShortNameTable.Count();
	return _RenderDeviceShortNameTable[device_index];
}

bool DX8Wrapper::Set_Device_Resolution(int width,int height,int bits,int windowed, bool resize_window)
{
	if (D3DDevice != nullptr) {

		if (width != -1) ResolutionWidth = width;
		if (height != -1) ResolutionHeight = height;

		if (resize_window)
		{
			Resize_And_Position_Window();
		}

		Resolve_Present_BackBuffer_Size(
			ResolutionWidth,
			ResolutionHeight,
			IsWindowed,
			_PresentParameters.BackBufferWidth,
			_PresentParameters.BackBufferHeight);

		// Release pillarbox RT before device reset
		Pillarbox_Cleanup();
#pragma message("TODO: support changing windowed status and changing the bit depth")
		WWDEBUG_SAY(("DX8Wrapper::Set_Device_Resolution is resetting the device."));
		bool ok = Reset_Device();
		// GeneralsX: DECISION: return deliberately discarded. `ok` is
		// Reset_Device's result and is what this function returns - it is NOT a check on
		// Pillarbox_Setup, which is easy to misread because the two share a line. Set_Device_Resolution's
		// contract is whether the resolution change took, and it did. The same shared-false objection as
		// the other three sites applies to propagating; see the contract note above Pillarbox_Setup.
		if (ok) Pillarbox_Setup(ResolutionWidth, ResolutionHeight);
		return ok;
	} else {
		return false;
	}
}

void DX8Wrapper::Get_Device_Resolution(int & set_w,int & set_h,int & set_bits,bool & set_windowed)
{
	WWASSERT(IsInitted);

	set_w = ResolutionWidth;
	set_h = ResolutionHeight;
	set_bits = BitDepth;
	set_windowed = IsWindowed;
}

void DX8Wrapper::Get_Render_Target_Resolution(int & set_w,int & set_h,int & set_bits,bool & set_windowed)
{
	WWASSERT(IsInitted);

	if (CurrentRenderTarget != nullptr) {
		D3DSURFACE_DESC info;
		CurrentRenderTarget->GetDesc (&info);

		set_w				= info.Width;
		set_h				= info.Height;
		set_bits			= BitDepth;		// should we get the actual bit depth of the target?
		set_windowed	= IsWindowed;	// this doesn't really make sense for render targets (shouldn't matter)...

	} else {
		Get_Device_Resolution (set_w, set_h, set_bits, set_windowed);
	}
}

bool DX8Wrapper::Registry_Save_Render_Device( const char * sub_key )
{
	int	width, height, depth;
	bool	windowed;
	Get_Device_Resolution(width, height, depth, windowed);
	return Registry_Save_Render_Device(sub_key, CurRenderDevice, ResolutionWidth, ResolutionHeight, BitDepth, IsWindowed, TextureBitDepth);
}

bool DX8Wrapper::Registry_Save_Render_Device( const char *sub_key, int device, int width, int height, int depth, bool windowed, int texture_depth)
{
	RegistryClass * registry = W3DNEW RegistryClass( sub_key );
	WWASSERT( registry );

	if ( !registry->Is_Valid() ) {
		delete registry;
		WWDEBUG_SAY(( "Error getting Registry" ));
		return false;
	}

	registry->Set_String( VALUE_NAME_RENDER_DEVICE_NAME,
		_RenderDeviceShortNameTable[device] );
	registry->Set_Int( VALUE_NAME_RENDER_DEVICE_WIDTH,	width );
	registry->Set_Int( VALUE_NAME_RENDER_DEVICE_HEIGHT, height );
	registry->Set_Int( VALUE_NAME_RENDER_DEVICE_DEPTH, depth );
	registry->Set_Int( VALUE_NAME_RENDER_DEVICE_WINDOWED, windowed );
	registry->Set_Int( VALUE_NAME_RENDER_DEVICE_TEXTURE_DEPTH, texture_depth );

	delete registry;
	return true;
}

bool DX8Wrapper::Registry_Load_Render_Device( const char * sub_key, bool resize_window )
{
	char	name[ 200 ];
	int	width,height,depth,windowed;

	if (	Registry_Load_Render_Device(	sub_key,
													name,
													sizeof(name),
													width,
													height,
													depth,
													windowed,
													TextureBitDepth) &&
			(*name != 0))
	{
		WWDEBUG_SAY(( "Device %s (%d X %d) %d bit windowed:%d", name,width,height,depth,windowed));

		if (TextureBitDepth==16 || TextureBitDepth==32) {
//			WWDEBUG_SAY(( "Texture depth %d", TextureBitDepth));
		} else {
			WWDEBUG_SAY(( "Invalid texture depth %d, switching to 16 bits", TextureBitDepth));
			TextureBitDepth=16;
		}

		if ( Set_Render_Device( name, width,height,depth,windowed, resize_window ) != true) {
			if (depth==16) depth=32;
			else depth=16;
			if ( Set_Render_Device( name, width,height,depth,windowed, resize_window ) == true) {
				return true;
			}
			if (depth==16) depth=32;
			else depth=16;
			// we'll test resolutions down, so if start is 640, increase to begin with...
			if (width==640) {
				width=1024;
				height=768;
			}
			for(;;) {
				if (width>2048) {
					width=2048;
					height=1536;
				}
				else if (width>1920) {
					width=1920;
					height=1440;
				}
				else if (width>1600) {
					width=1600;
					height=1200;
				}
				else if (width>1280) {
					width=1280;
					height=1024;
				}
				else if (width>1024) {
					width=1024;
					height=768;
				}
				else if (width>800) {
					width=800;
					height=600;
				}
				else if (width!=640) {
					width=640;
					height=480;
				}
				else {
					return Set_Any_Render_Device();
				}
				for (int i=0;i<2;++i) {
					if ( Set_Render_Device( name, width,height,depth,windowed, resize_window ) == true) {
						return true;
					}
					if (depth==16) depth=32;
					else depth=16;
				}
			}
		}

		return true;
	}

	WWDEBUG_SAY(( "Error getting Registry" ));

	return Set_Any_Render_Device();
}

bool DX8Wrapper::Registry_Load_Render_Device( const char * sub_key, char *device, int device_len, int &width, int &height, int &depth, int &windowed, int &texture_depth)
{
	RegistryClass registry( sub_key );

	if ( registry.Is_Valid() ) {
		registry.Get_String( VALUE_NAME_RENDER_DEVICE_NAME,
			device, device_len);

		width =		registry.Get_Int( VALUE_NAME_RENDER_DEVICE_WIDTH, -1 );
		height =		registry.Get_Int( VALUE_NAME_RENDER_DEVICE_HEIGHT, -1 );
		depth =		registry.Get_Int( VALUE_NAME_RENDER_DEVICE_DEPTH, -1 );
		windowed =	registry.Get_Int( VALUE_NAME_RENDER_DEVICE_WINDOWED, -1 );
		texture_depth = registry.Get_Int( VALUE_NAME_RENDER_DEVICE_TEXTURE_DEPTH, -1 );
		return true;
	}
	*device=0;
	width=-1;
	height=-1;
	depth=-1;
	windowed=-1;
	texture_depth=-1;
	return false;
}


bool DX8Wrapper::Find_Color_And_Z_Mode(int resx,int resy,int bitdepth,D3DFORMAT * set_colorbuffer,D3DFORMAT * set_backbuffer,D3DFORMAT * set_zmode)
{
	static D3DFORMAT _formats16[] =
	{
		D3DFMT_R5G6B5,
		D3DFMT_X1R5G5B5,
		D3DFMT_A1R5G5B5
	};

	static D3DFORMAT _formats32[] =
	{
		D3DFMT_A8R8G8B8,
		D3DFMT_X8R8G8B8,
		D3DFMT_R8G8B8,
	};

	/*
	** Select the table that we're going to use to search for a valid backbuffer format
	*/
	D3DFORMAT * format_table = nullptr;
	int format_count = 0;

	if (BitDepth == 16) {
		format_table = _formats16;
		format_count = sizeof(_formats16) / sizeof(D3DFORMAT);
	} else {
		format_table = _formats32;
		format_count = sizeof(_formats32) / sizeof(D3DFORMAT);
	}

	/*
	** now search for a valid format
	*/
	bool found = false;
	unsigned int mode = 0;

	int format_index=0;
	for (; format_index < format_count; format_index++) {
		found |= Find_Color_Mode(format_table[format_index],resx,resy,&mode);
		if (found) break;
	}

	if (!found) {
		return false;
	} else {
		*set_backbuffer=*set_colorbuffer = format_table[format_index];
	}

	if (bitdepth==32 && *set_colorbuffer == D3DFMT_X8R8G8B8 && D3DInterface->CheckDeviceType(0,D3DDEVTYPE_HAL,*set_colorbuffer,D3DFMT_A8R8G8B8, TRUE) == D3D_OK)
	{	//promote 32-bit modes to include destination alpha when supported
		*set_backbuffer = D3DFMT_A8R8G8B8;
	}

	/*
	** We found a backbuffer format, now find a zbuffer format
	*/
	return Find_Z_Mode(*set_colorbuffer,*set_backbuffer, set_zmode);
};


// find the resolution mode with at least resx,resy with the highest supported
// refresh rate
bool DX8Wrapper::Find_Color_Mode(D3DFORMAT colorbuffer, int resx, int resy, UINT *mode)
{
	UINT i,j,modemax;
	UINT rx,ry;
	D3DDISPLAYMODE dmode;
	::ZeroMemory(&dmode, sizeof(D3DDISPLAYMODE));

	rx=(unsigned int) resx;
	ry=(unsigned int) resy;

	bool found=false;

	modemax=D3DInterface->GetAdapterModeCount(D3DADAPTER_DEFAULT);

	i=0;

	while (i<modemax && !found)
	{
		D3DInterface->EnumAdapterModes(D3DADAPTER_DEFAULT, i, &dmode);
		if (dmode.Width==rx && dmode.Height==ry && dmode.Format==colorbuffer) {
			WWDEBUG_SAY(("Found valid color mode.  Width = %d Height = %d Format = %d",dmode.Width,dmode.Height,dmode.Format));
			found=true;
		}
		i++;
	}

	i--; // this is the first valid mode

	// no match
	if (!found) {
		WWDEBUG_SAY(("Failed to find a valid color mode"));
		return false;
	}

	// go to the highest refresh rate in this mode
	bool stillok=true;

	j=i;
	while (j<modemax && stillok)
	{
		D3DInterface->EnumAdapterModes(D3DADAPTER_DEFAULT, j, &dmode);
		if (dmode.Width==rx && dmode.Height==ry && dmode.Format==colorbuffer)
			stillok=true; else stillok=false;
		j++;
	}

	if (stillok==false) *mode=j-2;
	else *mode=i;

	return true;
}

// Helper function to find a Z buffer mode for the colorbuffer
// Will look for greatest Z precision
bool DX8Wrapper::Find_Z_Mode(D3DFORMAT colorbuffer,D3DFORMAT backbuffer, D3DFORMAT *zmode)
{
	//MW: Swapped the next 2 tests so that Stencil modes get tested first.
	if (Test_Z_Mode(colorbuffer,backbuffer,D3DFMT_D24S8))
	{
		*zmode=D3DFMT_D24S8;
		WWDEBUG_SAY(("Found zbuffer mode D3DFMT_D24S8"));
		return true;
	}

	if (Test_Z_Mode(colorbuffer,backbuffer,D3DFMT_D32))
	{
		*zmode=D3DFMT_D32;
		WWDEBUG_SAY(("Found zbuffer mode D3DFMT_D32"));
		return true;
	}

	if (Test_Z_Mode(colorbuffer,backbuffer,D3DFMT_D24X8))
	{
		*zmode=D3DFMT_D24X8;
		WWDEBUG_SAY(("Found zbuffer mode D3DFMT_D24X8"));
		return true;
	}

	if (Test_Z_Mode(colorbuffer,backbuffer,D3DFMT_D24X4S4))
	{
		*zmode=D3DFMT_D24X4S4;
		WWDEBUG_SAY(("Found zbuffer mode D3DFMT_D24X4S4"));
		return true;
	}

	if (Test_Z_Mode(colorbuffer,backbuffer,D3DFMT_D16))
	{
		*zmode=D3DFMT_D16;
		WWDEBUG_SAY(("Found zbuffer mode D3DFMT_D16"));
		return true;
	}

	if (Test_Z_Mode(colorbuffer,backbuffer,D3DFMT_D15S1))
	{
		*zmode=D3DFMT_D15S1;
		WWDEBUG_SAY(("Found zbuffer mode D3DFMT_D15S1"));
		return true;
	}

	// can't find a match
	WWDEBUG_SAY(("Failed to find a valid zbuffer mode"));
	return false;
}

bool DX8Wrapper::Test_Z_Mode(D3DFORMAT colorbuffer,D3DFORMAT backbuffer, D3DFORMAT zmode)
{
	// See if we have this mode first
	if (FAILED(D3DInterface->CheckDeviceFormat(D3DADAPTER_DEFAULT,WW3D_DEVTYPE,
		colorbuffer,D3DUSAGE_DEPTHSTENCIL,D3DRTYPE_SURFACE,zmode)))
	{
		WWDEBUG_SAY(("CheckDeviceFormat failed.  Colorbuffer format = %d  Zbufferformat = %d",colorbuffer,zmode));
		return false;
	}

	// Then see if it matches the color buffer
	if(FAILED(D3DInterface->CheckDepthStencilMatch(D3DADAPTER_DEFAULT, WW3D_DEVTYPE,
		colorbuffer,backbuffer,zmode)))
	{
		WWDEBUG_SAY(("CheckDepthStencilMatch failed.  Colorbuffer format = %d  Backbuffer format = %d Zbufferformat = %d",colorbuffer,backbuffer,zmode));
		return false;
	}
	return true;
}


void DX8Wrapper::Reset_Statistics()
{
	FrameStatistics = DX8FrameStatistics();
	LastFrameStatistics = DX8FrameStatistics();
}

void DX8Wrapper::Begin_Statistics()
{
	FrameStatistics = DX8FrameStatistics();
}

void DX8Wrapper::End_Statistics()
{
	LastFrameStatistics = FrameStatistics;
}

const DX8FrameStatistics& DX8Wrapper::Get_Last_Frame_Statistics()
{
	return LastFrameStatistics;
}

unsigned long DX8Wrapper::Get_FrameCount() {return FrameCount;}

void DX8_Assert()
{
	WWASSERT(DX8Wrapper::_Get_D3D8());
	DX8_THREAD_ASSERT();
}

void DX8Wrapper::Begin_Scene()
{
	DX8_THREAD_ASSERT();

#if ENABLE_EMBEDDED_BROWSER
	DX8WebBrowser::Update();
#endif

	DX8CALL(BeginScene());

	// GeneralsX: Embedded browser Windows-only
#ifdef _WIN32
	DX8WebBrowser::Update();
#endif
}

void DX8Wrapper::End_Scene(bool flip_frames)
{
	DX8_THREAD_ASSERT();

	// Pillarbox: restore backbuffer + blit offscreen, all within the current scene
	if (s_pillarboxActive && s_offscreenTex) {
		Pillarbox_End();
		// GeneralsX: Re-sync cached DX8 state after the pillarbox blit mutates global render state at the end of the frame.
		Invalidate_Cached_Render_States();
	}
	DX8CALL(EndScene());

	// GeneralsX: Embedded browser Windows-only
#ifdef _WIN32
	DX8WebBrowser::Render(0);
#endif

	if (flip_frames) {
		DX8_Assert();
		HRESULT hr;
		{
			WWPROFILE("DX8Device::Present()");
			if (GxPrePresentHook) GxPrePresentHook();	// GeneralsX: WS-G S1 seat: the digest bracket's frame end
			if (Present_Probe) Present_Probe(false);
			hr=_Get_D3D_Device8()->Present(nullptr, nullptr, nullptr, nullptr);
			++PresentCount;
			if (Present_Probe) Present_Probe(true);
		}
		DX8Ring::Frame_Stats();
		GxFlushProf::Frame();	// GX_FLUSHPROF: once per displayed frame (not per scene pass)

		DX8_RECORD_DX8_CALLS();

		if (SUCCEEDED(hr)) {
#ifdef EXTENDED_STATS
			if (stats.m_sleepTime) {
				::Sleep(stats.m_sleepTime);
			}
#endif
			IsDeviceLost=false;
			FrameCount++;
		}
		else {
			IsDeviceLost=true;
		}

		// If the device was lost we need to check for cooperative level and possibly reset the device
		if (hr==D3DERR_DEVICELOST) {
			hr=_Get_D3D_Device8()->TestCooperativeLevel();
			if (hr==D3DERR_DEVICENOTRESET) {
				WWDEBUG_SAY(("DX8Wrapper::End_Scene is resetting the device."));
				Reset_Device();
			}
			else {
				// Sleep it not active
				ThreadClass::Sleep_Ms(200);
			}
		}
		else {
			DX8_ErrorCode(hr);
		}
	}

	// Each frame, release all of the buffers and textures.
	Set_Vertex_Buffer(nullptr);
	Set_Index_Buffer(nullptr,0);
	for (int i=0;i<CurrentCaps->Get_Max_Textures_Per_Pass();++i) Set_Texture(i,nullptr);
	Set_Material(nullptr);
}


void DX8Wrapper::Flip_To_Primary()
{
	// If we are fullscreen and the current frame is odd then we need
	// to force a page flip to ensure that the first buffer in the flipping
	// chain is the one visible.
	if (!IsWindowed) {
		DX8_Assert();

		int numBuffers = (_PresentParameters.BackBufferCount + 1);
		int visibleBuffer = (FrameCount % numBuffers);
		int flipCount = ((numBuffers - visibleBuffer) % numBuffers);
		int resetAttempts = 0;

		while ((flipCount > 0) && (resetAttempts < 3)) {
			HRESULT hr = _Get_D3D_Device8()->TestCooperativeLevel();

			if (FAILED(hr)) {
				WWDEBUG_SAY(("TestCooperativeLevel Failed!"));

				if (D3DERR_DEVICELOST == hr) {
					IsDeviceLost=true;
					WWDEBUG_SAY(("DEVICELOST: Cannot flip to primary."));
					return;
				}
				IsDeviceLost=false;

				if (D3DERR_DEVICENOTRESET == hr) {
					WWDEBUG_SAY(("DEVICENOTRESET"));
					Reset_Device();
					resetAttempts++;
				}
			} else {
				WWDEBUG_SAY(("Flipping: %ld", FrameCount));
				hr = _Get_D3D_Device8()->Present(nullptr, nullptr, nullptr, nullptr);
				++PresentCount;

				if (SUCCEEDED(hr)) {
					IsDeviceLost=false;
					FrameCount++;
					WWDEBUG_SAY(("Flip to primary succeeded %ld", FrameCount));
				}
				else {
					IsDeviceLost=true;
				}
			}

			--flipCount;
		}
	}
}


//**********************************************************************************************
//! Clear current render device
/*! KM
/* 5/17/02 KM Fixed support for render to texture with depth/stencil buffers
*/
void DX8Wrapper::Clear(bool clear_color, bool clear_z_stencil, const Vector3 &color, float dest_alpha, float z, unsigned int stencil)
{
	DX8_THREAD_ASSERT();

	// If we try to clear a stencil buffer which is not there, the entire call will fail
	// KJM fixed this to get format from back buffer (incase render to texture is used)
	/*bool has_stencil = (	_PresentParameters.AutoDepthStencilFormat == D3DFMT_D15S1 ||
								_PresentParameters.AutoDepthStencilFormat == D3DFMT_D24S8 ||
								_PresentParameters.AutoDepthStencilFormat == D3DFMT_D24X4S4);*/
	bool has_stencil=false;
	IDirect3DSurface8* depthbuffer;

	_Get_D3D_Device8()->GetDepthStencilSurface(&depthbuffer);
	DX8_RECORD_DX8_CALLS();

	if (depthbuffer)
	{
		D3DSURFACE_DESC desc;
		depthbuffer->GetDesc(&desc);
		has_stencil=
		(
			desc.Format==D3DFMT_D15S1 ||
			desc.Format==D3DFMT_D24S8 ||
			desc.Format==D3DFMT_D24X4S4
		);

		// release ref
		depthbuffer->Release();
	}

	DWORD flags = 0;
	if (clear_color) flags |= D3DCLEAR_TARGET;
	if (clear_z_stencil) flags |= D3DCLEAR_ZBUFFER;
	if (clear_z_stencil && has_stencil) flags |= D3DCLEAR_STENCIL;
	if (flags)
	{
		DX8CALL(Clear(0, nullptr, flags, Convert_Color(color,dest_alpha), z, stencil));
	}
}

void DX8Wrapper::Set_Viewport(CONST D3DVIEWPORT8* pViewport)
{
	DX8_THREAD_ASSERT();
	if (GxViewportCaptureOn && pViewport) { GxViewportSent = *pViewport; GxViewportSentValid = true; }	// A35 motion bridge (dx8wrapper.h)
	DX8CALL(SetViewport(pViewport));
}

// ----------------------------------------------------------------------------
//
// Set vertex buffer. A reference to previous vertex buffer is released and
// this one is assigned the current vertex buffer. The DX8 vertex buffer will
// actually be set in Apply() which is called by Draw_Indexed_Triangles().
//
// ----------------------------------------------------------------------------

void DX8Wrapper::Set_Vertex_Buffer(const VertexBufferClass* vb, unsigned stream)
{
	if (GxLogOn && GxLogParent == 0) Gx_Log_Vertex_Buffer(gxlog::SET_VB, stream, 0, vb ? vb->Type() : BUFFER_TYPE_INVALID, vb);	// GeneralsX: WS-G Task 5
	render_state.vba_offset=0;
	render_state.vba_count=0;
	if (render_state.vertex_buffers[stream]) {
		render_state.vertex_buffers[stream]->Release_Engine_Ref();
	}
	REF_PTR_SET(render_state.vertex_buffers[stream],const_cast<VertexBufferClass*>(vb));
	if (vb) {
		vb->Add_Engine_Ref();
		render_state.vertex_buffer_types[stream]=vb->Type();
	}
	else {
		render_state.vertex_buffer_types[stream]=BUFFER_TYPE_INVALID;
	}
	render_state_changed|=VERTEX_BUFFER_CHANGED;
}

// ----------------------------------------------------------------------------
//
// Set index buffer. A reference to previous index buffer is released and
// this one is assigned the current index buffer. The DX8 index buffer will
// actually be set in Apply() which is called by Draw_Indexed_Triangles().
//
// ----------------------------------------------------------------------------

void DX8Wrapper::Set_Index_Buffer(const IndexBufferClass* ib,unsigned short index_base_offset)
{
	if (GxLogOn && GxLogParent == 0) Gx_Log_Index_Buffer(gxlog::SET_IB, index_base_offset, 0, ib ? ib->Type() : BUFFER_TYPE_INVALID, ib);	// GeneralsX: WS-G Task 5
	render_state.iba_offset=0;
	if (render_state.index_buffer) {
		render_state.index_buffer->Release_Engine_Ref();
	}
	REF_PTR_SET(render_state.index_buffer,const_cast<IndexBufferClass*>(ib));
	render_state.index_base_offset=index_base_offset;
	if (ib) {
		ib->Add_Engine_Ref();
		render_state.index_buffer_type=ib->Type();
	}
	else {
		render_state.index_buffer_type=BUFFER_TYPE_INVALID;
	}
	render_state_changed|=INDEX_BUFFER_CHANGED;
}

// ----------------------------------------------------------------------------
//
// Set vertex buffer using dynamic access object.
//
// ----------------------------------------------------------------------------

void DX8Wrapper::Set_Vertex_Buffer(const DynamicVBAccessClass& vba_)
{
	if (GxLogOn) {	// GeneralsX: WS-G Task 5: the plain setter below is this entry's port (raised parent)
		if (GxLogParent == 0) Gx_Log_Vertex_Buffer(gxlog::SET_VB_DYNAMIC, vba_.VertexBufferOffset, vba_.Get_Vertex_Count(), vba_.Get_Type(), vba_.VertexBuffer);
		++GxLogParent;
	}
	// Release all streams (only one stream allowed in the legacy pipeline)
	for (int i=1;i<MAX_VERTEX_STREAMS;++i) {
		DX8Wrapper::Set_Vertex_Buffer(nullptr, i);
	}

	if (render_state.vertex_buffers[0]) render_state.vertex_buffers[0]->Release_Engine_Ref();
	DynamicVBAccessClass& vba=const_cast<DynamicVBAccessClass&>(vba_);
	render_state.vertex_buffer_types[0]=vba.Get_Type();
	render_state.vba_offset=vba.VertexBufferOffset;
	render_state.vba_count=vba.Get_Vertex_Count();
	REF_PTR_SET(render_state.vertex_buffers[0],vba.VertexBuffer);
	render_state.vertex_buffers[0]->Add_Engine_Ref();
	render_state_changed|=VERTEX_BUFFER_CHANGED;
	render_state_changed|=INDEX_BUFFER_CHANGED;		// vba_offset changes so index buffer needs to be reset as well.
	if (GxLogOn) --GxLogParent;	// GeneralsX: WS-G Task 5
}

// ----------------------------------------------------------------------------
//
// Set index buffer using dynamic access object.
//
// ----------------------------------------------------------------------------

void DX8Wrapper::Set_Index_Buffer(const DynamicIBAccessClass& iba_,unsigned short index_base_offset)
{
	if (GxLogOn && GxLogParent == 0) Gx_Log_Index_Buffer(gxlog::SET_IB_DYNAMIC, iba_.IndexBufferOffset, index_base_offset, iba_.Get_Type(), iba_.IndexBuffer);	// GeneralsX: WS-G Task 5
	if (render_state.index_buffer) render_state.index_buffer->Release_Engine_Ref();

	DynamicIBAccessClass& iba=const_cast<DynamicIBAccessClass&>(iba_);
	render_state.index_base_offset=index_base_offset;
	render_state.index_buffer_type=iba.Get_Type();
	render_state.iba_offset=iba.IndexBufferOffset;
	REF_PTR_SET(render_state.index_buffer,iba.IndexBuffer);
	render_state.index_buffer->Add_Engine_Ref();
	render_state_changed|=INDEX_BUFFER_CHANGED;
}

// ----------------------------------------------------------------------------
//
// Private function for the special case of rendering polygons from sorting
// index and vertex buffers.
//
// ----------------------------------------------------------------------------

void DX8Wrapper::Draw_Sorting_IB_VB(
	unsigned primitive_type,
	unsigned short start_index,
	unsigned short polygon_count,
	unsigned short min_vertex_index,
	unsigned short vertex_count)
{
	WWASSERT(render_state.vertex_buffer_types[0]==BUFFER_TYPE_SORTING || render_state.vertex_buffer_types[0]==BUFFER_TYPE_DYNAMIC_SORTING);
	WWASSERT(render_state.index_buffer_type==BUFFER_TYPE_SORTING || render_state.index_buffer_type==BUFFER_TYPE_DYNAMIC_SORTING);

	// Fill dynamic vertex buffer with sorting vertex buffer vertices
	DynamicVBAccessClass dyn_vb_access(BUFFER_TYPE_DYNAMIC_DX8,dynamic_fvf_type,vertex_count);
	{
		DynamicVBAccessClass::WriteLockClass lock(&dyn_vb_access);
		VertexFormatXYZNDUV2* src = static_cast<SortingVertexBufferClass*>(render_state.vertex_buffers[0])->VertexBuffer;
		VertexFormatXYZNDUV2* dest= lock.Get_Formatted_Vertex_Array();
		src += render_state.vba_offset + render_state.index_base_offset + min_vertex_index;
		unsigned  size = dyn_vb_access.FVF_Info().Get_FVF_Size()*vertex_count/sizeof(unsigned);
		unsigned *dest_u =(unsigned*) dest;
		unsigned *src_u = (unsigned*) src;

		for (unsigned i=0;i<size;++i) {
			*dest_u++=*src_u++;
		}
	}

	DX8CALL(SetStreamSource(
		0,
		static_cast<DX8VertexBufferClass*>(dyn_vb_access.VertexBuffer)->Get_DX8_Vertex_Buffer(),
		dyn_vb_access.FVF_Info().Get_FVF_Size()));
	// If using FVF format VB, set the FVF as vertex shader (may not be needed here KM)
	unsigned fvf=dyn_vb_access.FVF_Info().Get_FVF();
	if (fvf!=0) {
		DX8CALL(SetVertexShader(fvf));
	}
	DX8_RECORD_VERTEX_BUFFER_CHANGE();
	if (CensusOn) {	// [GXCENSUS] this path binds directly, past Apply_Render_State_Changes (rcpu r4)
		const void *vb = static_cast<DX8VertexBufferClass*>(dyn_vb_access.VertexBuffer)->Get_DX8_Vertex_Buffer();
		++Census[DX8C_VB_DEV];
		if (vb == s_cenLastVB) ++Census[DX8C_VB_SAME];
		s_cenLastVB = vb;
		if (fvf!=0) {
			++Census[DX8C_FVF_DEV];
			if (CensusLastFVF == fvf) ++Census[DX8C_FVF_SAME];
			CensusLastFVF = fvf;
		}
	}

	unsigned index_count=0;
	switch (primitive_type) {
	case D3DPT_TRIANGLELIST: index_count=polygon_count*3; break;
	case D3DPT_TRIANGLESTRIP: index_count=polygon_count+2; break;
	case D3DPT_TRIANGLEFAN: index_count=polygon_count+2; break;
	default: WWASSERT(0); break; // Unsupported primitive type
	}

	// Fill dynamic index buffer with sorting index buffer vertices
	DynamicIBAccessClass dyn_ib_access(BUFFER_TYPE_DYNAMIC_DX8,index_count);
	{
		DynamicIBAccessClass::WriteLockClass lock(&dyn_ib_access);
		unsigned short* dest=lock.Get_Index_Array();
		unsigned short* src=nullptr;
		src=static_cast<SortingIndexBufferClass*>(render_state.index_buffer)->index_buffer;
		src+=render_state.iba_offset+start_index;

		for (unsigned short i=0;i<index_count;++i) {
			unsigned short index=*src++;
			index-=min_vertex_index;
			WWASSERT(index<vertex_count);
			*dest++=index;
		}
	}

	DX8CALL(SetIndices(
		static_cast<DX8IndexBufferClass*>(dyn_ib_access.IndexBuffer)->Get_DX8_Index_Buffer(),
		dyn_vb_access.VertexBufferOffset));
	DX8_RECORD_INDEX_BUFFER_CHANGE();
	if (CensusOn) {	// [GXCENSUS] see the stream above
		const void *ib = static_cast<DX8IndexBufferClass*>(dyn_ib_access.IndexBuffer)->Get_DX8_Index_Buffer();
		const unsigned base = dyn_vb_access.VertexBufferOffset;
		++Census[DX8C_IB_DEV];
		if (ib == s_cenLastIB && base == s_cenLastBase) ++Census[DX8C_IB_SAME];
		s_cenLastIB = ib;
		s_cenLastBase = base;
	}
	if (GxLogOn) Gx_Log_Draw_Sort(static_cast<DX8VertexBufferClass*>(dyn_vb_access.VertexBuffer)->Get_DX8_Vertex_Buffer(),	// GeneralsX: WS-G Task 5
		static_cast<DX8IndexBufferClass*>(dyn_ib_access.IndexBuffer)->Get_DX8_Index_Buffer(), dyn_vb_access.FVF_Info().Get_FVF_Size(), fvf,
		dyn_vb_access.VertexBufferOffset, dyn_ib_access.IndexBufferOffset);

	DX8_RECORD_DRAW_CALLS();
	if (GxDrawHook) GxDrawHook(D3DPT_TRIANGLELIST, 0, vertex_count, dyn_ib_access.IndexBufferOffset, polygon_count, true);	// WS-G
	DX8CALL(DrawIndexedPrimitive(
		D3DPT_TRIANGLELIST,
		0,		// start vertex
		vertex_count,
		dyn_ib_access.IndexBufferOffset,
		polygon_count));
	if (GxPostDrawHook) GxPostDrawHook();	// WS-G seat: the draw is recorded

	DX8_RECORD_RENDER(polygon_count,vertex_count,render_state.shader);
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

void DX8Wrapper::Draw(
	unsigned primitive_type,
	unsigned short start_index,
	unsigned short polygon_count,
	unsigned short min_vertex_index,
	unsigned short vertex_count)
{
	if (GxLogOn) Gx_Log_Draw(primitive_type, start_index, polygon_count, min_vertex_index, vertex_count);	// GeneralsX: WS-G Task 5: DRAW at GxLogParent 0
	if (DrawPolygonLowBoundLimit && DrawPolygonLowBoundLimit>=polygon_count) return;

	DX8_THREAD_ASSERT();
	SNAPSHOT_SAY(("DX8 - draw"));

	Apply_Render_State_Changes();

	// Debug feature to disable triangle drawing...
	if (!_Is_Triangle_Draw_Enabled()) return;

#ifdef MESH_RENDER_SNAPSHOT_ENABLED
	if (WW3D::Is_Snapshot_Activated()) {
		DWORD passes=0;		// GeneralsX: Use DWORD (32-bit) instead of unsigned long (platform-dependent)
		SNAPSHOT_SAY(("ValidateDevice:"));
		HRESULT res=D3DDevice->ValidateDevice(&passes);
		switch (res) {
		case D3D_OK:
			SNAPSHOT_SAY(("OK"));
			break;

		case D3DERR_CONFLICTINGTEXTUREFILTER:
			SNAPSHOT_SAY(("D3DERR_CONFLICTINGTEXTUREFILTER"));
			break;
		case D3DERR_CONFLICTINGTEXTUREPALETTE:
			SNAPSHOT_SAY(("D3DERR_CONFLICTINGTEXTUREPALETTE"));
			break;
		case D3DERR_DEVICELOST:
			SNAPSHOT_SAY(("D3DERR_DEVICELOST"));
			break;
		case D3DERR_TOOMANYOPERATIONS:
			SNAPSHOT_SAY(("D3DERR_TOOMANYOPERATIONS"));
			break;
		case D3DERR_UNSUPPORTEDALPHAARG:
			SNAPSHOT_SAY(("D3DERR_UNSUPPORTEDALPHAARG"));
			break;
		case D3DERR_UNSUPPORTEDALPHAOPERATION:
			SNAPSHOT_SAY(("D3DERR_UNSUPPORTEDALPHAOPERATION"));
			break;
		case D3DERR_UNSUPPORTEDCOLORARG:
			SNAPSHOT_SAY(("D3DERR_UNSUPPORTEDCOLORARG"));
			break;
		case D3DERR_UNSUPPORTEDCOLOROPERATION:
			SNAPSHOT_SAY(("D3DERR_UNSUPPORTEDCOLOROPERATION"));
			break;
		case D3DERR_UNSUPPORTEDFACTORVALUE:
			SNAPSHOT_SAY(("D3DERR_UNSUPPORTEDFACTORVALUE"));
			break;
		case D3DERR_UNSUPPORTEDTEXTUREFILTER:
			SNAPSHOT_SAY(("D3DERR_UNSUPPORTEDTEXTUREFILTER"));
			break;
		case D3DERR_WRONGTEXTUREFORMAT:
			SNAPSHOT_SAY(("D3DERR_WRONGTEXTUREFORMAT"));
			break;
		default:
			SNAPSHOT_SAY(("UNKNOWN Error"));
			break;
		}
	}
#endif	// MESH_RENDER_SNAPSHOT_ENABLED


	SNAPSHOT_SAY(("DX8 - draw %d polygons (%d vertices)",polygon_count,vertex_count));

	if (vertex_count<3) {
		min_vertex_index=0;
		switch (render_state.vertex_buffer_types[0]) {
		case BUFFER_TYPE_DX8:
		case BUFFER_TYPE_SORTING:
			vertex_count=render_state.vertex_buffers[0]->Get_Vertex_Count()-render_state.index_base_offset-render_state.vba_offset-min_vertex_index;
			break;
		case BUFFER_TYPE_DYNAMIC_DX8:
		case BUFFER_TYPE_DYNAMIC_SORTING:
			vertex_count=render_state.vba_count;
			break;
		}
	}

	switch (render_state.vertex_buffer_types[0]) {
	case BUFFER_TYPE_DX8:
	case BUFFER_TYPE_DYNAMIC_DX8:
		switch (render_state.index_buffer_type) {
		case BUFFER_TYPE_DX8:
		case BUFFER_TYPE_DYNAMIC_DX8:
			{
/*				if ((start_index+render_state.iba_offset+polygon_count*3) > render_state.index_buffer->Get_Index_Count())
				{	WWASSERT_PRINT(0,"OVERFLOWING INDEX BUFFER");
					///@todo: MUST FIND OUT WHY THIS HAPPENS WITH LOTS OF PARTICLES ON BIG FIGHT!  -MW
					break;
				}*/
				DX8_RECORD_RENDER(polygon_count,vertex_count,render_state.shader);
				DX8_RECORD_DRAW_CALLS();
				if (GxDrawHook) GxDrawHook(primitive_type, min_vertex_index, vertex_count, start_index+render_state.iba_offset,
					polygon_count, false);	// WS-G (dx8wrapper.h)
				if (DX8Ring::Recording) {
					DX8Ring::Rec_Draw_Indexed(primitive_type,min_vertex_index,vertex_count,
						start_index+render_state.iba_offset,polygon_count);
				} else {
				DX8CALL(DrawIndexedPrimitive(
					(D3DPRIMITIVETYPE)primitive_type,
					min_vertex_index,
					vertex_count,
					start_index+render_state.iba_offset,
					polygon_count));
				}
				if (GxPostDrawHook) GxPostDrawHook();	// WS-G seat: the draw is recorded (ring or device)
			}
			break;
		case BUFFER_TYPE_SORTING:
		case BUFFER_TYPE_DYNAMIC_SORTING:
			WWASSERT_PRINT(0,"VB and IB must of same type (sorting or dx8)");
			break;
		case BUFFER_TYPE_INVALID:
			WWASSERT(0);
			break;
		}
		break;
	case BUFFER_TYPE_SORTING:
	case BUFFER_TYPE_DYNAMIC_SORTING:
		switch (render_state.index_buffer_type) {
		case BUFFER_TYPE_DX8:
		case BUFFER_TYPE_DYNAMIC_DX8:
			WWASSERT_PRINT(0,"VB and IB must of same type (sorting or dx8)");
			break;
		case BUFFER_TYPE_SORTING:
		case BUFFER_TYPE_DYNAMIC_SORTING:
			Draw_Sorting_IB_VB(primitive_type,start_index,polygon_count,min_vertex_index,vertex_count);
			break;
		case BUFFER_TYPE_INVALID:
			WWASSERT(0);
			break;
		}
		break;
	case BUFFER_TYPE_INVALID:
		WWASSERT(0);
		break;
	}
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

void DX8Wrapper::Draw_Triangles(
	unsigned buffer_type,
	unsigned short start_index,
	unsigned short polygon_count,
	unsigned short min_vertex_index,
	unsigned short vertex_count)
{
	if (buffer_type==BUFFER_TYPE_SORTING || buffer_type==BUFFER_TYPE_DYNAMIC_SORTING) {
		SortingRendererClass::Insert_Triangles(start_index,polygon_count,min_vertex_index,vertex_count);
	}
	else {
		Draw(D3DPT_TRIANGLELIST,start_index,polygon_count,min_vertex_index,vertex_count);
	}
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

void DX8Wrapper::Draw_Triangles(
	unsigned short start_index,
	unsigned short polygon_count,
	unsigned short min_vertex_index,
	unsigned short vertex_count)
{
	Draw(D3DPT_TRIANGLELIST,start_index,polygon_count,min_vertex_index,vertex_count);
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

void DX8Wrapper::Draw_Strip(
	unsigned short start_index,
	unsigned short polygon_count,
	unsigned short min_vertex_index,
	unsigned short vertex_count)
{
	Draw(D3DPT_TRIANGLESTRIP,start_index,polygon_count,min_vertex_index,vertex_count);
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

void DX8Wrapper::Apply_Render_State_Changes()
{
	SNAPSHOT_SAY(("DX8Wrapper::Apply_Render_State_Changes()"));

	if (!render_state_changed) return;
	if (GxLogOn) Gx_Log_Apply(true);	// GeneralsX: WS-G Task 5: APPLY_BEGIN; the body's sends are NESTED
	if (render_state_changed&SHADER_CHANGED) {
		SNAPSHOT_SAY(("DX8 - apply shader"));
		DX8_CENSUS(DX8C_SHADER_APPLY);
		if (GxLogOn) GxLogBracket = gxlog::BR_SHADER;	// GeneralsX: WS-G Task 5
		render_state.shader.Apply();
	}

	unsigned mask=TEXTURE0_CHANGED;
	int i=0;
	for (;i<CurrentCaps->Get_Max_Textures_Per_Pass();++i,mask<<=1)
	{
		if (render_state_changed&mask)
		{
			SNAPSHOT_SAY(("DX8 - apply texture %d (%s)",i,render_state.Textures[i] ? render_state.Textures[i]->Get_Full_Path().str() : "null"));
			if (GxLogOn) { GxLogBracket = gxlog::BR_TEXTURE; GxLogStage = (uint8_t)i; }	// GeneralsX: WS-G Task 5

			if (render_state.Textures[i])
			{
				render_state.Textures[i]->Apply(i);
			}
			else
			{
				TextureBaseClass::Apply_Null(i);
			}
		}
	}

	if (render_state_changed&MATERIAL_CHANGED)
	{
		SNAPSHOT_SAY(("DX8 - apply material"));
		if (GxLogOn) GxLogBracket = gxlog::BR_MATERIAL;	// GeneralsX: WS-G Task 5
		VertexMaterialClass* material=const_cast<VertexMaterialClass*>(render_state.material);
		if (material)
		{
			material->Apply();
		}
		else VertexMaterialClass::Apply_Null();
	}

	if (render_state_changed&LIGHTS_CHANGED)
	{
		if (GxLogOn) GxLogBracket = gxlog::BR_LIGHTS;	// GeneralsX: WS-G Task 5
		unsigned mask=LIGHT0_CHANGED;
		for (unsigned index=0;index<4;++index,mask<<=1) {
			if (render_state_changed&mask) {
				SNAPSHOT_SAY(("DX8 - apply light %d",index));
				if (render_state.LightEnable[index]) {
#ifdef MESH_RENDER_SNAPSHOT_ENABLED
					if ( WW3D::Is_Snapshot_Activated() ) {
						D3DLIGHT8 * light = &(render_state.Lights[index]);
						static const char * _light_types[] = { "Unknown", "Point","Spot", "Directional" };
						WWASSERT((light->Type >= 0) && (light->Type <= 3));

						SNAPSHOT_SAY((" type = %s amb = %4.2f,%4.2f,%4.2f  diff = %4.2f,%4.2f,%4.2f spec = %4.2f, %4.2f, %4.2f",
							_light_types[light->Type],
							light->Ambient.r,light->Ambient.g,light->Ambient.b,
							light->Diffuse.r,light->Diffuse.g,light->Diffuse.b,
							light->Specular.r,light->Specular.g,light->Specular.b ));
						SNAPSHOT_SAY((" pos = %f, %f, %f  dir = %f, %f, %f",
							light->Position.x, light->Position.y, light->Position.z,
							light->Direction.x, light->Direction.y, light->Direction.z ));
					}
#endif

					Set_DX8_Light(index,&render_state.Lights[index]);
				}
				else {
					Set_DX8_Light(index,nullptr);
					SNAPSHOT_SAY((" clearing light to null"));
				}
			}
		}
	}

	if (GxLogOn) GxLogBracket = gxlog::BR_XFORM;	// GeneralsX: WS-G Task 5: the world / view sends
	if (render_state_changed&WORLD_CHANGED) {
		SNAPSHOT_SAY(("DX8 - apply world matrix"));
		// GeneralsX: pass Matrix4x4 directly (no D3DMATRIX conversion)
		_Set_DX8_Transform(D3DTS_WORLD, render_state.world);
	}
	if (render_state_changed&VIEW_CHANGED) {
		SNAPSHOT_SAY(("DX8 - apply view matrix"));
		// GeneralsX: pass Matrix4x4 directly (no D3DMATRIX conversion)
		_Set_DX8_Transform(D3DTS_VIEW, render_state.view);
	}
	if (GxLogOn) GxLogBracket = gxlog::BR_BINDINGS;	// GeneralsX: WS-G Task 5: the VB / IB blocks
	if (render_state_changed&VERTEX_BUFFER_CHANGED) {
		SNAPSHOT_SAY(("DX8 - apply vb change"));
		for (i=0;i<MAX_VERTEX_STREAMS;++i) {
			if (render_state.vertex_buffers[i]) {
				switch (render_state.vertex_buffer_types[i]) {//->Type()) {
				case BUFFER_TYPE_DX8:
				case BUFFER_TYPE_DYNAMIC_DX8:
					if (CensusOn && i == 0) {	// [GXCENSUS] stream 0 only (the others are null bindings or unused)
						const void *vb = static_cast<DX8VertexBufferClass*>(render_state.vertex_buffers[i])->Get_DX8_Vertex_Buffer();
						++Census[DX8C_VB_DEV];
						if (vb == s_cenLastVB) ++Census[DX8C_VB_SAME];
						s_cenLastVB = vb;
					}
					if (GxLogOn) Gx_Log_Ptr(gxlog::STREAM, i, render_state.vertex_buffers[i]->FVF_Info().Get_FVF_Size(),	// GeneralsX: WS-G Task 5
						static_cast<DX8VertexBufferClass*>(render_state.vertex_buffers[i])->Get_DX8_Vertex_Buffer(), gxlog::SENT);
					if (DX8Ring::Recording) {
						DX8Ring::Rec_Stream_Source(i,
							static_cast<DX8VertexBufferClass*>(render_state.vertex_buffers[i])->Get_DX8_Vertex_Buffer(),
							render_state.vertex_buffers[i]->FVF_Info().Get_FVF_Size());
					} else {
					DX8CALL(SetStreamSource(
						i,
						static_cast<DX8VertexBufferClass*>(render_state.vertex_buffers[i])->Get_DX8_Vertex_Buffer(),
						render_state.vertex_buffers[i]->FVF_Info().Get_FVF_Size()));
					}
					DX8_RECORD_VERTEX_BUFFER_CHANGE();
					{
						// If the VB format is FVF, set the FVF as a vertex shader
						unsigned fvf=render_state.vertex_buffers[i]->FVF_Info().Get_FVF();
						if (fvf!=0) {
							if (GxLogOn) Gx_Log(gxlog::VSHADER, fvf, 0, nullptr, 0, gxlog::SENT);	// GeneralsX: WS-G Task 5
							Set_Vertex_Shader(fvf);
						}
					}
					break;
				case BUFFER_TYPE_SORTING:
				case BUFFER_TYPE_DYNAMIC_SORTING:
					break;
				default:
					WWASSERT(0);
				}
			} else {
				if (CensusOn && i == 0) {	// [GXCENSUS] a null binding is a device call too, and breaks a "same" run
					++Census[DX8C_VB_DEV];
					if (s_cenLastVB == &s_cenLastVB) ++Census[DX8C_VB_SAME];
					s_cenLastVB = &s_cenLastVB;
				}
				if (GxLogOn) Gx_Log_Ptr(gxlog::STREAM, i, 0, nullptr, gxlog::SENT);	// GeneralsX: WS-G Task 5
				if (DX8Ring::Recording) { DX8Ring::Rec_Stream_Source(i,nullptr,0); }
				else { DX8CALL(SetStreamSource(i,nullptr,0)); }
				DX8_RECORD_VERTEX_BUFFER_CHANGE();
			}
		}
	}
	if (render_state_changed&INDEX_BUFFER_CHANGED) {
		SNAPSHOT_SAY(("DX8 - apply ib change"));
		if (render_state.index_buffer) {
			switch (render_state.index_buffer_type) {//->Type()) {
			case BUFFER_TYPE_DX8:
			case BUFFER_TYPE_DYNAMIC_DX8:
				if (CensusOn) {	// [GXCENSUS]
					const void *ib = static_cast<DX8IndexBufferClass*>(render_state.index_buffer)->Get_DX8_Index_Buffer();
					const unsigned base = render_state.index_base_offset+render_state.vba_offset;
					++Census[DX8C_IB_DEV];
					if (ib == s_cenLastIB && base == s_cenLastBase) ++Census[DX8C_IB_SAME];
					s_cenLastIB = ib;
					s_cenLastBase = base;
				}
				if (GxLogOn) Gx_Log_Ptr(gxlog::INDICES, 0, render_state.index_base_offset+render_state.vba_offset,	// GeneralsX: WS-G Task 5
					static_cast<DX8IndexBufferClass*>(render_state.index_buffer)->Get_DX8_Index_Buffer(), gxlog::SENT);
				if (DX8Ring::Recording) {
					DX8Ring::Rec_Indices(static_cast<DX8IndexBufferClass*>(render_state.index_buffer)->Get_DX8_Index_Buffer(),
						render_state.index_base_offset+render_state.vba_offset);
				} else {
				DX8CALL(SetIndices(
					static_cast<DX8IndexBufferClass*>(render_state.index_buffer)->Get_DX8_Index_Buffer(),
					render_state.index_base_offset+render_state.vba_offset));
				}
				DX8_RECORD_INDEX_BUFFER_CHANGE();
				break;
			case BUFFER_TYPE_SORTING:
			case BUFFER_TYPE_DYNAMIC_SORTING:
				break;
			default:
				WWASSERT(0);
			}
		}
		else {
			if (CensusOn) {	// [GXCENSUS] see the null stream above (counted whether the binding is recorded or sent)
				++Census[DX8C_IB_DEV];
				if (s_cenLastIB == &s_cenLastIB) ++Census[DX8C_IB_SAME];
				s_cenLastIB = &s_cenLastIB;
				s_cenLastBase = ~0u;
			}
			if (GxLogOn) Gx_Log_Ptr(gxlog::INDICES, 0, 0, nullptr, gxlog::SENT);	// GeneralsX: WS-G Task 5
			if (DX8Ring::Recording) { DX8Ring::Rec_Indices(nullptr,0); }
			else { DX8CALL(SetIndices(nullptr,0)); }	// braces: DX8CALL is several statements
			DX8_RECORD_INDEX_BUFFER_CHANGE();
		}
	}

	render_state_changed&=((unsigned)WORLD_IDENTITY|(unsigned)VIEW_IDENTITY);
	if (GxLogOn) Gx_Log_Apply(false);	// GeneralsX: WS-G Task 5: brackets reset, APPLY_END

	SNAPSHOT_SAY(("DX8Wrapper::Apply_Render_State_Changes() - finished"));
}

IDirect3DTexture8 * DX8Wrapper::_Create_DX8_Texture
(
	unsigned int width,
	unsigned int height,
	WW3DFormat format,
	MipCountType mip_level_count,
	D3DPOOL pool,
	bool rendertarget
)
{
	GxLogBody gxBody(gxlog::RESOURCE);	// GeneralsX: WS-G Task 5: a known device access (no RAW_ACCESS)
	DX8_THREAD_ASSERT();
	DX8_Assert();
	IDirect3DTexture8 *texture = nullptr;
	IDirect3DDevice8 *d3d_device = DX8Wrapper::_Get_D3D_Device8();

	// GeneralsX @bugfix fbraz 04/05/2026 Avoid null device calls during headless replay texture requests.
	if (d3d_device == nullptr)
	{
		return nullptr;
	}

	// Paletted textures not supported!
	WWASSERT(format!=D3DFMT_P8);

	// NOTE: If 'format' is not supported as a texture format, this function will find the closest
	// format that is supported and use that instead.

	// Render target may return NOTAVAILABLE, in
	// which case we return null.
	if (rendertarget) {
		unsigned ret=D3DXCreateTexture(
			d3d_device,
			width,
			height,
			mip_level_count,
			D3DUSAGE_RENDERTARGET,
			WW3DFormat_To_D3DFormat(format),
			pool,
			&texture);

		if (ret==D3DERR_NOTAVAILABLE) {
			Non_Fatal_Log_DX8_ErrorCode(ret,__FILE__,__LINE__);
			return nullptr;
		}

		// If ran out of texture ram, try invalidating some textures and mesh cache.
		if (ret==D3DERR_OUTOFVIDEOMEMORY) {
			WWDEBUG_SAY(("Error: Out of memory while creating render target. Trying to release assets..."));
			// Free all textures that haven't been used in the last 5 seconds
			TextureClass::Invalidate_Old_Unused_Textures(5000);

			// Invalidate the mesh cache
			WW3D::_Invalidate_Mesh_Cache();

			ret=D3DXCreateTexture(
				d3d_device,
				width,
				height,
				mip_level_count,
				D3DUSAGE_RENDERTARGET,
				WW3DFormat_To_D3DFormat(format),
				pool,
				&texture);

			if (SUCCEEDED(ret)) {
				WWDEBUG_SAY(("...Render target creation successful."));
			}
			else {
				WWDEBUG_SAY(("...Render target creation failed."));
			}
			if (ret==D3DERR_OUTOFVIDEOMEMORY) {
				Non_Fatal_Log_DX8_ErrorCode(ret,__FILE__,__LINE__);
				return nullptr;
			}
		}

		DX8_ErrorCode(ret);
		// Just return the texture, no reduction
		// allowed for render targets.
		return texture;
	}

	// We should never run out of video memory when allocating a non-rendertarget texture.
	// However, it seems to happen sometimes when there are a lot of textures in memory and so
	// if it happens we'll release assets and try again (anything is better than crashing).
	unsigned ret=D3DXCreateTexture(
		d3d_device,
		width,
		height,
		mip_level_count,
		0,
		WW3DFormat_To_D3DFormat(format),
		pool,
		&texture);

	// If ran out of texture ram, try invalidating some textures and mesh cache.
	if (ret==D3DERR_OUTOFVIDEOMEMORY) {
		WWDEBUG_SAY(("Error: Out of memory while creating texture. Trying to release assets..."));
		// Free all textures that haven't been used in the last 5 seconds
		TextureClass::Invalidate_Old_Unused_Textures(5000);

		// Invalidate the mesh cache
		WW3D::_Invalidate_Mesh_Cache();

		ret=D3DXCreateTexture(
			d3d_device,
			width,
			height,
			mip_level_count,
			0,
			WW3DFormat_To_D3DFormat(format),
			pool,
			&texture);
		if (SUCCEEDED(ret)) {
			WWDEBUG_SAY(("...Texture creation successful."));
		}
		else {
			StringClass format_name(0,true);
			Get_WW3D_Format_Name(format, format_name);
			WWDEBUG_SAY(("...Texture creation failed. (%d x %d, format: %s, mips: %d",width,height,format_name.str(),mip_level_count));
		}

	}
	DX8_ErrorCode(ret);

	// GeneralsX @bugfix felipebraz 10/06/2025 On non-Windows builds DX8_ErrorCode is a silent no-op
	// in non-debug builds (WWASSERT expands to nothing). Log failures to stderr so crashes can be
	// diagnosed without a debugger attached.
#if !defined(_WIN32)
	if (!SUCCEEDED(ret)) {
		StringClass format_name(0, true);
		Get_WW3D_Format_Name(static_cast<WW3DFormat>(format), format_name);
		fprintf(stderr, "[GeneralsX] _Create_DX8_Texture FAILED: hresult=0x%08X %dx%d fmt=%s mips=%d pool=%d rendertarget=%d\n",
			ret, (int)width, (int)height, format_name.str(), (int)mip_level_count, (int)pool, (int)rendertarget);
	}
#endif

	return texture;
}

IDirect3DTexture8 * DX8Wrapper::_Create_DX8_Texture
(
	const char *filename,
	MipCountType mip_level_count
)
{
	GxLogBody gxBody(gxlog::RESOURCE);	// GeneralsX: WS-G Task 5: a known device access (no RAW_ACCESS)
	DX8_THREAD_ASSERT();
	DX8_Assert();
	IDirect3DTexture8 *texture = nullptr;

	// NOTE: If the original image format is not supported as a texture format, it will
	// automatically be converted to an appropriate format.
	// NOTE: It is possible to get the size and format of the original image file from this
	// function as well, so if we later want to second-guess D3DX's format conversion decisions
	// we can do so after this function is called..
	unsigned result = D3DXCreateTextureFromFileExA(
		_Get_D3D_Device8(),
		filename,
		D3DX_DEFAULT,
		D3DX_DEFAULT,
		mip_level_count,//create_mipmaps ? 0 : 1,
		0,
		D3DFMT_UNKNOWN,
		D3DPOOL_MANAGED,
		D3DX_FILTER_BOX,
		D3DX_FILTER_BOX,
		0,
		nullptr,
		nullptr,
		&texture);

	if (result != D3D_OK) {
		return MissingTexture::_Get_Missing_Texture();
	}

	// Make sure texture wasn't paletted!
	D3DSURFACE_DESC desc;
	texture->GetLevelDesc(0,&desc);
	if (desc.Format==D3DFMT_P8) {
		texture->Release();
		return MissingTexture::_Get_Missing_Texture();
	}
	return texture;
}

IDirect3DTexture8 * DX8Wrapper::_Create_DX8_Texture
(
	IDirect3DSurface8 *surface,
	MipCountType mip_level_count
)
{
	GxLogBody gxBody(gxlog::RESOURCE);	// GeneralsX: WS-G Task 5: a known device access (no RAW_ACCESS)
	DX8_THREAD_ASSERT();
	DX8_Assert();
	IDirect3DTexture8 *texture = nullptr;

	D3DSURFACE_DESC surface_desc;
	::ZeroMemory(&surface_desc, sizeof(D3DSURFACE_DESC));
	surface->GetDesc(&surface_desc);

	// This function will create a texture with a different (but similar) format if the surface is
	// not in a supported texture format.
	WW3DFormat format=D3DFormat_To_WW3DFormat(surface_desc.Format);
	texture = _Create_DX8_Texture(surface_desc.Width, surface_desc.Height, format, mip_level_count);

	// Copy the surface to the texture
	IDirect3DSurface8 *tex_surface = nullptr;
	texture->GetSurfaceLevel(0, &tex_surface);
	DX8_ErrorCode(D3DXLoadSurfaceFromSurface(tex_surface, nullptr, nullptr, surface, nullptr, nullptr, D3DX_FILTER_BOX, 0));
	tex_surface->Release();

	// Create mipmaps if needed
	if (mip_level_count!=MIP_LEVELS_1)
	{
		DX8_ErrorCode(D3DXFilterTexture(texture, nullptr, 0, D3DX_FILTER_BOX));
	}

	return texture;

}

/*!
 * KJM create depth stencil texture
 */
IDirect3DTexture8 * DX8Wrapper::_Create_DX8_ZTexture
(
	unsigned int width,
	unsigned int height,
	WW3DZFormat zformat,
	MipCountType mip_level_count,
	D3DPOOL pool
)
{
	GxLogBody gxBody(gxlog::RESOURCE);	// GeneralsX: WS-G Task 5: a known device access (no RAW_ACCESS)
	DX8_THREAD_ASSERT();
	DX8_Assert();
	IDirect3DTexture8* texture = nullptr;

	D3DFORMAT zfmt=WW3DZFormat_To_D3DFormat(zformat);

	unsigned ret=DX8Wrapper::_Get_D3D_Device8()->CreateTexture
	(
		width,
		height,
		mip_level_count,
		D3DUSAGE_DEPTHSTENCIL,
		zfmt,
		pool,
		&texture
	);

	if (ret==D3DERR_NOTAVAILABLE)
	{
		Non_Fatal_Log_DX8_ErrorCode(ret,__FILE__,__LINE__);
		return nullptr;
	}

	// If ran out of texture ram, try invalidating some textures and mesh cache.
	if (ret==D3DERR_OUTOFVIDEOMEMORY)
	{
		WWDEBUG_SAY(("Error: Out of memory while creating render target. Trying to release assets..."));
		// Free all textures that haven't been used in the last 5 seconds
		TextureClass::Invalidate_Old_Unused_Textures(5000);

		// Invalidate the mesh cache
		WW3D::_Invalidate_Mesh_Cache();

		ret=DX8Wrapper::_Get_D3D_Device8()->CreateTexture
		(
			width,
			height,
			mip_level_count,
			D3DUSAGE_DEPTHSTENCIL,
			zfmt,
			pool,
			&texture
		);

		if (SUCCEEDED(ret))
		{
			WWDEBUG_SAY(("...Render target creation successful."));
		}
		else
		{
			WWDEBUG_SAY(("...Render target creation failed."));
		}
		if (ret==D3DERR_OUTOFVIDEOMEMORY)
		{
			Non_Fatal_Log_DX8_ErrorCode(ret,__FILE__,__LINE__);
			return nullptr;
		}
	}

	DX8_ErrorCode(ret);

	texture->AddRef(); // don't release this texture

	// Just return the texture, no reduction
	// allowed for render targets.

	return texture;
}

/*!
 * KJM create cube map texture
 */
IDirect3DCubeTexture8* DX8Wrapper::_Create_DX8_Cube_Texture
(
	unsigned int width,
	unsigned int height,
	WW3DFormat format,
	MipCountType mip_level_count,
	D3DPOOL pool,
	bool rendertarget
)
{
	GxLogBody gxBody(gxlog::RESOURCE);	// GeneralsX: WS-G Task 5: a known device access (no RAW_ACCESS)
	WWASSERT(width==height);
	DX8_THREAD_ASSERT();
	DX8_Assert();
	IDirect3DCubeTexture8* texture=nullptr;

	// Paletted textures not supported!
	WWASSERT(format!=D3DFMT_P8);

	// NOTE: If 'format' is not supported as a texture format, this function will find the closest
	// format that is supported and use that instead.

	// Render target may return NOTAVAILABLE, in
	// which case we return null.
	if (rendertarget)
	{
		unsigned ret=D3DXCreateCubeTexture
		(
			DX8Wrapper::_Get_D3D_Device8(),
			width,
			mip_level_count,
			D3DUSAGE_RENDERTARGET,
			WW3DFormat_To_D3DFormat(format),
			pool,
			&texture
		);

		if (ret==D3DERR_NOTAVAILABLE)
		{
			Non_Fatal_Log_DX8_ErrorCode(ret,__FILE__,__LINE__);
			return nullptr;
		}

		// If ran out of texture ram, try invalidating some textures and mesh cache.
		if (ret==D3DERR_OUTOFVIDEOMEMORY)
		{
			WWDEBUG_SAY(("Error: Out of memory while creating render target. Trying to release assets..."));
			// Free all textures that haven't been used in the last 5 seconds
			TextureClass::Invalidate_Old_Unused_Textures(5000);

			// Invalidate the mesh cache
			WW3D::_Invalidate_Mesh_Cache();

			ret=D3DXCreateCubeTexture
			(
				DX8Wrapper::_Get_D3D_Device8(),
				width,
				mip_level_count,
				D3DUSAGE_RENDERTARGET,
				WW3DFormat_To_D3DFormat(format),
				pool,
				&texture
			);

			if (SUCCEEDED(ret))
			{
				WWDEBUG_SAY(("...Render target creation successful."));
			}
			else
			{
				WWDEBUG_SAY(("...Render target creation failed."));
			}
			if (ret==D3DERR_OUTOFVIDEOMEMORY)
			{
				Non_Fatal_Log_DX8_ErrorCode(ret,__FILE__,__LINE__);
				return nullptr;
			}
		}

		DX8_ErrorCode(ret);
		// Just return the texture, no reduction
		// allowed for render targets.
		return texture;
	}

	// We should never run out of video memory when allocating a non-rendertarget texture.
	// However, it seems to happen sometimes when there are a lot of textures in memory and so
	// if it happens we'll release assets and try again (anything is better than crashing).
	unsigned ret=D3DXCreateCubeTexture
	(
		DX8Wrapper::_Get_D3D_Device8(),
		width,
		mip_level_count,
		0,
		WW3DFormat_To_D3DFormat(format),
		pool,
		&texture
	);

	// If ran out of texture ram, try invalidating some textures and mesh cache.
	if (ret==D3DERR_OUTOFVIDEOMEMORY)
	{
		WWDEBUG_SAY(("Error: Out of memory while creating texture. Trying to release assets..."));
		// Free all textures that haven't been used in the last 5 seconds
		TextureClass::Invalidate_Old_Unused_Textures(5000);

		// Invalidate the mesh cache
		WW3D::_Invalidate_Mesh_Cache();

		ret=D3DXCreateCubeTexture
		(
			DX8Wrapper::_Get_D3D_Device8(),
			width,
			mip_level_count,
			0,
			WW3DFormat_To_D3DFormat(format),
			pool,
			&texture
		);
		if (SUCCEEDED(ret))
		{
			WWDEBUG_SAY(("...Texture creation successful."));
		}
		else
		{
			StringClass format_name(0,true);
			Get_WW3D_Format_Name(format, format_name);
			WWDEBUG_SAY(("...Texture creation failed. (%d x %d, format: %s, mips: %d",width,height,format_name.str(),mip_level_count));
		}

	}
	DX8_ErrorCode(ret);

	return texture;
}

/*!
 * KJM create volume texture
 */
IDirect3DVolumeTexture8* DX8Wrapper::_Create_DX8_Volume_Texture
(
	unsigned int width,
	unsigned int height,
	unsigned int depth,
	WW3DFormat format,
	MipCountType mip_level_count,
	D3DPOOL pool
)
{
	GxLogBody gxBody(gxlog::RESOURCE);	// GeneralsX: WS-G Task 5: a known device access (no RAW_ACCESS)
	DX8_THREAD_ASSERT();
	DX8_Assert();
	IDirect3DVolumeTexture8* texture=nullptr;

	// Paletted textures not supported!
	WWASSERT(format!=D3DFMT_P8);

	// NOTE: If 'format' is not supported as a texture format, this function will find the closest
	// format that is supported and use that instead.


	// We should never run out of video memory when allocating a non-rendertarget texture.
	// However, it seems to happen sometimes when there are a lot of textures in memory and so
	// if it happens we'll release assets and try again (anything is better than crashing).
	unsigned ret=D3DXCreateVolumeTexture
	(
		DX8Wrapper::_Get_D3D_Device8(),
		width,
		height,
		depth,
		mip_level_count,
		0,
		WW3DFormat_To_D3DFormat(format),
		pool,
		&texture
	);

	// If ran out of texture ram, try invalidating some textures and mesh cache.
	if (ret==D3DERR_OUTOFVIDEOMEMORY)
	{
		WWDEBUG_SAY(("Error: Out of memory while creating texture. Trying to release assets..."));
		// Free all textures that haven't been used in the last 5 seconds
		TextureClass::Invalidate_Old_Unused_Textures(5000);

		// Invalidate the mesh cache
		WW3D::_Invalidate_Mesh_Cache();

		ret=D3DXCreateVolumeTexture
		(
			DX8Wrapper::_Get_D3D_Device8(),
			width,
			height,
			depth,
			mip_level_count,
			0,
			WW3DFormat_To_D3DFormat(format),
			pool,
			&texture
		);
		if (SUCCEEDED(ret))
		{
			WWDEBUG_SAY(("...Texture creation successful."));
		}
		else
		{
			StringClass format_name(0,true);
			Get_WW3D_Format_Name(format, format_name);
			WWDEBUG_SAY(("...Texture creation failed. (%d x %d, format: %s, mips: %d",width,height,format_name.str(),mip_level_count));
		}

	}
	DX8_ErrorCode(ret);

	return texture;
}


IDirect3DSurface8 * DX8Wrapper::_Create_DX8_Surface(unsigned int width, unsigned int height, WW3DFormat format)
{
	GxLogBody gxBody(gxlog::RESOURCE);	// GeneralsX: WS-G Task 5: a known device access (no RAW_ACCESS)
	DX8_THREAD_ASSERT();
	DX8_Assert();

	IDirect3DSurface8 *surface = nullptr;

	// Paletted surfaces not supported!
	WWASSERT(format!=D3DFMT_P8);

	DX8CALL(CreateImageSurface(width, height, WW3DFormat_To_D3DFormat(format), &surface));

	return surface;
}

IDirect3DSurface8 * DX8Wrapper::_Create_DX8_Surface(const char *filename_)
{
	GxLogBody gxBody(gxlog::RESOURCE);	// GeneralsX: WS-G Task 5: a known device access (no RAW_ACCESS)
	DX8_THREAD_ASSERT();
	DX8_Assert();

	// Note: Since there is no "D3DXCreateSurfaceFromFile" and no "GetSurfaceInfoFromFile" (the
	// latter is supposed to be added to D3DX in a future version), we create a texture from the
	// file (w/o mipmaps), check that its surface is equal to the original file data (which it
	// will not be if the file is not in a texture-supported format or size). If so, copy its
	// surface (we might be able to just get its surface and add a ref to it but I'm not sure so
	// I'm not going to risk it) and release the texture. If not, create a surface according to
	// the file data and use D3DXLoadSurfaceFromFile. This is a horrible hack, but it saves us
	// having to write file loaders. Will fix this when D3DX provides us with the right functions.
	// Create a surface the size of the file image data
	IDirect3DSurface8 *surface = nullptr;

	{

		file_auto_ptr myfile(_TheFileFactory,filename_);
		// If file not found, create a surface with missing texture in it

		if (!myfile->Is_Available()) {
			// If file not found, try the dds format
			// else create a surface with missing texture in it
			char compressed_name[200];
			strlcpy(compressed_name,filename_, sizeof(compressed_name));
			char *ext = strstr(compressed_name, ".");
			if ( ext && (strlen(ext)==4) &&
				  ( (ext[1] == 't') || (ext[1] == 'T') ) &&
				  ( (ext[2] == 'g') || (ext[2] == 'G') ) &&
				  ( (ext[3] == 'a') || (ext[3] == 'A') ) ) {
				ext[1]='d';
				ext[2]='d';
				ext[3]='s';
			}
			file_auto_ptr myfile2(_TheFileFactory,compressed_name);
			if (!myfile2->Is_Available())
				return MissingTexture::_Create_Missing_Surface();
		}
	}

	StringClass filename_string(filename_,true);
	surface=TextureLoader::Load_Surface_Immediate(
		filename_string,
		WW3D_FORMAT_UNKNOWN,
		true);
	return surface;
}


/***********************************************************************************************
 * DX8Wrapper::_Update_Texture -- Copies a texture from system memory to video memory          *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   4/26/2001  hy : Created.                                                                  *
 *=============================================================================================*/
void DX8Wrapper::_Update_Texture(TextureClass *system, TextureClass *video)
{
	WWASSERT(system);
	WWASSERT(video);
	WWASSERT(system->Get_Pool()==TextureClass::POOL_SYSTEMMEM);
	WWASSERT(video->Get_Pool()==TextureClass::POOL_DEFAULT);
	DX8CALL(UpdateTexture(system->Peek_D3D_Base_Texture(),video->Peek_D3D_Base_Texture()));
}

void DX8Wrapper::Compute_Caps(WW3DFormat display_format)
{
	DX8_THREAD_ASSERT();
	DX8_Assert();
	delete CurrentCaps;
	CurrentCaps=new DX8Caps(_Get_D3D8(),D3DDevice,display_format,Get_Current_Adapter_Identifier());
}


void DX8Wrapper::Set_Light(unsigned index, const D3DLIGHT8* light)
{
	if (GxLogOn && GxLogParent == 0) Gx_Log(gxlog::SET_LIGHT, index, light != nullptr, light, light ? sizeof(D3DLIGHT8) : 0, 0);	// GeneralsX: WS-G Task 5
	if (light) {
		render_state.Lights[index]=*light;
		render_state.LightEnable[index]=true;
	}
	else {
		render_state.LightEnable[index]=false;
	}
	render_state_changed|=(LIGHT0_CHANGED<<index);
	++LightStateSerial;	// GX_WRAPSKIP 8
}

void DX8Wrapper::Set_Light(unsigned index,const LightClass &light)
{
	D3DLIGHT8 dlight;
	Vector3 temp;
	memset(&dlight,0,sizeof(D3DLIGHT8));

	switch (light.Get_Type())
	{
	case LightClass::POINT:
		{
			dlight.Type=D3DLIGHT_POINT;
		}
		break;
	case LightClass::DIRECTIONAL:
		{
			dlight.Type=D3DLIGHT_DIRECTIONAL;
		}
		break;
	case LightClass::SPOT:
		{
			dlight.Type=D3DLIGHT_SPOT;
		}
		break;
	}

	light.Get_Diffuse(&temp);
	temp*=light.Get_Intensity();
	dlight.Diffuse.r=temp.X;
	dlight.Diffuse.g=temp.Y;
	dlight.Diffuse.b=temp.Z;
	dlight.Diffuse.a=1.0f;

	light.Get_Specular(&temp);
	temp*=light.Get_Intensity();
	dlight.Specular.r=temp.X;
	dlight.Specular.g=temp.Y;
	dlight.Specular.b=temp.Z;
	dlight.Specular.a=1.0f;

	light.Get_Ambient(&temp);
	temp*=light.Get_Intensity();
	dlight.Ambient.r=temp.X;
	dlight.Ambient.g=temp.Y;
	dlight.Ambient.b=temp.Z;
	dlight.Ambient.a=1.0f;

	temp=light.Get_Position();
	dlight.Position=*(D3DVECTOR*) &temp;

	light.Get_Spot_Direction(temp);
	dlight.Direction=*(D3DVECTOR*) &temp;

	dlight.Range=light.Get_Attenuation_Range();
	dlight.Falloff=light.Get_Spot_Exponent();
	dlight.Theta=light.Get_Spot_Angle();
	dlight.Phi=light.Get_Spot_Angle();

	// Inverse linear light 1/(1+D)
	double a,b;
	light.Get_Far_Attenuation_Range(a,b);
	dlight.Attenuation0=1.0f;
	if (fabs(a-b)<1e-5)
		// if the attenuation range is too small assume uniform with cutoff
		dlight.Attenuation1=0.0f;
	else
		// this will cause the light to drop to half intensity at the first far attenuation
		dlight.Attenuation1=(float) 1.0/a;
	dlight.Attenuation2=0.0f;

	Set_Light(index,&dlight);
}

//**********************************************************************************************
//! Set the light environment. This is a lighting model which used up to four
//! directional lights to produce the lighting.
/*! 5/27/02 KJM Added shader light environment support
*/
// [GXCENSUS] lenv / lenv_same (roadmap 2f): did this Set_Light_Environment build the same lights and ambient as the
// previous one? Read after the call from what it left in render_state (its D3DLIGHT8s are copied there, and the
// ambient render state), so the OFF path costs one test per call instead of one per light.
void DX8Wrapper::Census_Light_Environment(LightEnvironmentClass* light_env)
{
	static D3DLIGHT8 s_lights[4];
	static int s_count = -1;
	static unsigned s_color = 0;
	const int count = light_env ? light_env->Get_Light_Count() : -2;	// -2 = "no environment"
	const unsigned color = light_env ? RenderStates[D3DRS_AMBIENT] : 0u;
	bool same = (count == s_count && color == s_color);
	for (int l = 0; l < 4 && l < count; ++l) {
		same = same && memcmp(&s_lights[l], &render_state.Lights[l], sizeof(D3DLIGHT8)) == 0;
		s_lights[l] = render_state.Lights[l];
	}
	s_count = count;
	s_color = color;
	++Census[DX8C_LENV];
	if (same)
		++Census[DX8C_LENV_SAME];
}

// GeneralsX: GX_WRAPSKIP 8. Every mesh hands
// Set_Light_Environment its own copy of its object's lights, and the census measured ~99% of the ~12.4k calls a held
// frame rebuilding the same lights (Mini, G0B replay): per light a ZeroMemory, a fill, a 104-byte compare with the
// device's copy and a copy into render_state. The lights the call produces are a function of exactly the fields kept
// below (the count, and per light its direction, point flag, and the diffuse or the point fields), and what it then
// writes depends only on those lights and the wrapper's light state. So when the fields are bit-identical to the last
// full pass AND LightStateSerial has not moved since that pass ended, a full pass would write nothing: render_state
// already holds these lights, and the device holds them or a LIGHTn_CHANGED is still pending for them. Bits, not
// operator==: -0 and +0 (or two NaNs) differ in the D3DLIGHT8 bytes the full pass would compare, so they differ here.
struct GXLenvKey
{
	int		count;
	bool	point[LightEnvironmentClass::MAX_LIGHTS];
	Vector3	dir[LightEnvironmentClass::MAX_LIGHTS];
	Vector3	diff[LightEnvironmentClass::MAX_LIGHTS];	// non-point lights
	Vector3	pdiff[LightEnvironmentClass::MAX_LIGHTS];	// point lights: diffuse, ambient, centre, radii
	Vector3	pamb[LightEnvironmentClass::MAX_LIGHTS];
	Vector3	pcenter[LightEnvironmentClass::MAX_LIGHTS];
	float	irad[LightEnvironmentClass::MAX_LIGHTS];
	float	orad[LightEnvironmentClass::MAX_LIGHTS];
};
static GXLenvKey	s_lenvKey;
static bool			s_lenvKeyValid = false;
static unsigned		s_lenvSerial = 0;

static bool GX_Lenv_Same(const LightEnvironmentClass& e)
{
	const int n = e.Get_Light_Count();
	if (n != s_lenvKey.count) return false;
	for (int l = 0; l < n; ++l) {
		const bool point = e.isPointLight(l);
		if (point != s_lenvKey.point[l] || memcmp(&e.Get_Light_Direction(l), &s_lenvKey.dir[l], sizeof(Vector3)) != 0)
			return false;
		if (!point) {
			if (memcmp(&e.Get_Light_Diffuse(l), &s_lenvKey.diff[l], sizeof(Vector3)) != 0) return false;
			continue;
		}
		const float irad = e.getPointIrad(l), orad = e.getPointOrad(l);
		if (memcmp(&e.getPointDiffuse(l), &s_lenvKey.pdiff[l], sizeof(Vector3)) != 0
			|| memcmp(&e.getPointAmbient(l), &s_lenvKey.pamb[l], sizeof(Vector3)) != 0
			|| memcmp(&e.getPointCenter(l), &s_lenvKey.pcenter[l], sizeof(Vector3)) != 0
			|| memcmp(&irad, &s_lenvKey.irad[l], sizeof(float)) != 0
			|| memcmp(&orad, &s_lenvKey.orad[l], sizeof(float)) != 0)
			return false;
	}
	return true;
}

static void GX_Lenv_Remember(const LightEnvironmentClass& e)
{
	const int n = e.Get_Light_Count();
	s_lenvKey.count = n;
	for (int l = 0; l < n; ++l) {
		const bool point = e.isPointLight(l);
		s_lenvKey.point[l] = point;
		memcpy(&s_lenvKey.dir[l], &e.Get_Light_Direction(l), sizeof(Vector3));
		if (!point) {
			memcpy(&s_lenvKey.diff[l], &e.Get_Light_Diffuse(l), sizeof(Vector3));
			continue;
		}
		const float irad = e.getPointIrad(l), orad = e.getPointOrad(l);
		memcpy(&s_lenvKey.pdiff[l], &e.getPointDiffuse(l), sizeof(Vector3));
		memcpy(&s_lenvKey.pamb[l], &e.getPointAmbient(l), sizeof(Vector3));
		memcpy(&s_lenvKey.pcenter[l], &e.getPointCenter(l), sizeof(Vector3));
		memcpy(&s_lenvKey.irad[l], &irad, sizeof(float));
		memcpy(&s_lenvKey.orad[l], &orad, sizeof(float));
	}
}

void DX8Wrapper::Set_Light_Environment(LightEnvironmentClass* light_env)
{
	// Shader light environment support															*
	if (GxLogOn) Gx_Log_Light_Environment(light_env);	// GeneralsX: WS-G Task 5: SET_LIGHT_ENV at parent 0, raises it
	Light_Environment=light_env;

	if (light_env)
	{
		int light_count = light_env->Get_Light_Count();
		unsigned int color=Convert_Color(light_env->Get_Equivalent_Ambient(),0.0f);
		if (RenderStates[D3DRS_AMBIENT]!=color)
		{
			Set_DX8_Render_State(D3DRS_AMBIENT,color);
//buggy Radeon 9700 driver doesn't apply new ambient unless the material also changes.
#if 1
			render_state_changed|=MATERIAL_CHANGED;
#endif
		}

		// GX_WRAPSKIP 8 (see GXLenvKey): needs bit 1, whose skips the "a full pass writes nothing" argument relies on.
		const bool lenvSkip = (Wrap_Skip() & 9u) == 9u && light_count <= LightEnvironmentClass::MAX_LIGHTS;
		if (lenvSkip && s_lenvKeyValid && s_lenvSerial == LightStateSerial && GX_Lenv_Same(*light_env)) {
			DX8_CENSUS(DX8C_SKIP_LENV);
			if (CensusOn)
				Census_Light_Environment(light_env);
			if (GxLogOn) --GxLogParent;	// GeneralsX: WS-G Task 5
			return;
		}

		D3DLIGHT8 light;
		int l=0;
		for (;l<light_count;++l) {

			::ZeroMemory(&light, sizeof(D3DLIGHT8));

			light.Type=D3DLIGHT_DIRECTIONAL;
			(Vector3&)light.Diffuse=light_env->Get_Light_Diffuse(l);
			Vector3 dir=-light_env->Get_Light_Direction(l);
			light.Direction=(const D3DVECTOR&)(dir);

			// (gth) TODO: put specular into LightEnvironment?  Much work to be done on lights :-)'
			if (l==0) {
				light.Specular.r = light.Specular.g = light.Specular.b = 1.0f;
			}

			if (light_env->isPointLight(l)) {
				light.Type = D3DLIGHT_POINT;
				(Vector3&)light.Diffuse=light_env->getPointDiffuse(l);
				(Vector3&)light.Ambient=light_env->getPointAmbient(l);
				light.Position = (const D3DVECTOR&)light_env->getPointCenter(l);
				light.Range = light_env->getPointOrad(l);

				// Inverse linear light 1/(1+D)
				double a,b;
				b = light_env->getPointOrad(l);
				a = light_env->getPointIrad(l);

//(gth) CNC3 Generals code for the attenuation factors is causing the lights to over-brighten
//I'm changing the Attenuation0 parameter to 1.0 to avoid this problem.
#if 0
				light.Attenuation0=0.01f;
#else
				light.Attenuation0=1.0f;
#endif
				if (fabs(a-b)<1e-5)
					// if the attenuation range is too small assume uniform with cutoff
					light.Attenuation1=0.0f;
				else
					// this will cause the light to drop to half intensity at the first far attenuation
					light.Attenuation1=(float) 0.1/a;

				light.Attenuation2=8.0f/(b*b);
			}

			// GX_WRAPSKIP 1: the device already holds exactly this light, enabled, and nothing is pending for the slot:
			// re-flagging it only makes Apply_Render_State_Changes re-compare it (Set_DX8_Light's cache) per draw. The
			// deferred copy is still updated: the sorting renderer sets device lights directly, so render_state can hold
			// an older light than the device, and Insert_Triangles snapshots render_state (rcpu-wrapskip).
			if ((Wrap_Skip() & 1u) && l < 4 && CurrentDX8LightValid[l] && CurrentDX8LightEnables[l]
				&& render_state.LightEnable[l] && !(render_state_changed & (LIGHT0_CHANGED<<l))
				&& memcmp(&CurrentDX8Lights[l], &light, sizeof(D3DLIGHT8)) == 0) {
				render_state.Lights[l] = light;
				DX8_CENSUS(DX8C_SKIP_LIGHT);
				continue;
			}
			Set_Light(l,&light);
		}

		for (;l<4;++l) {
			if ((Wrap_Skip() & 1u) && !CurrentDX8LightEnables[l] && !render_state.LightEnable[l]
				&& !(render_state_changed & (LIGHT0_CHANGED<<l))) {
				DX8_CENSUS(DX8C_SKIP_LIGHT);
				continue;	// GX_WRAPSKIP 1: already disabled on the device
			}
			Set_Light(l,nullptr);
		}

		s_lenvKeyValid = lenvSkip;	// GX_WRAPSKIP 8: this full pass is the reference, with the serial it left
		if (lenvSkip) {
			GX_Lenv_Remember(*light_env);
			s_lenvSerial = LightStateSerial;
		}
	}
	if (CensusOn)	// one test when off: the census reads what this call left in render_state
		Census_Light_Environment(light_env);
	if (GxLogOn) --GxLogParent;	// GeneralsX: WS-G Task 5
/*	else {
		for (int l=0;l<4;++l) {
			Set_Light(l,nullptr);
		}
	}
*/
}

// GeneralsX: WS-G Task 5: the `=2` call log (dx8wrapper.h GxLogOn; gxcalllog.h; render-seam/rw1-G/
// ). Nothing here runs unless the check turned GxLogOn on. Matrices and lights go into the log as bytes.
static_assert(sizeof(D3DLIGHT8) == sizeof(gxlog::Light) && sizeof(D3DMATERIAL8) == sizeof(gxlog::Material)
	&& sizeof(Matrix4x4) == sizeof(gxlog::Mat4) && sizeof(Vector3) == 3 * sizeof(float), "the call log's byte copies");
bool DX8Wrapper::GxLogOn = false;
gxlog::Log *DX8Wrapper::GxLog = nullptr;
int DX8Wrapper::GxLogParent = 0;
int DX8Wrapper::GxLogScope = 0;
uint8_t DX8Wrapper::GxLogBracket = gxlog::BR_NONE;
uint8_t DX8Wrapper::GxLogStage = 0;
unsigned DX8Wrapper::GxLogEntry = 0;
int DX8Wrapper::GxLogDrawRec = -1;

void DX8Wrapper::Gx_Log(unsigned op, unsigned a, unsigned b, const void *payload, unsigned n, unsigned flags)
{
	if (GxLog == nullptr) return;
	const bool low = op >= gxlog::RS && op <= gxlog::XFORM_RAW;
	gxlog::Record r;
	memset(&r, 0, sizeof(r));
	r.op = (uint16_t)op;
	r.bracket = GxLogBracket;	// provenance inside an input bracket too: the replay skips it there
	r.stage = GxLogStage;
	r.flags = (uint8_t)(flags | ((low && GxLogParent > 0) ? gxlog::NESTED : 0));
	r.a = a;
	r.b = b;
	r.payload = payload ? GxLog->put(payload, n) : ~0u;
	r.entry = GxLogEntry;
	GxLog->rec.push_back(r);
}

void DX8Wrapper::Gx_Log_Ptr(unsigned op, unsigned a, unsigned b, const void *ptr, unsigned flags)
{
	const uint64_t id = (uint64_t)(uintptr_t)ptr;
	Gx_Log(op, a, b, &id, sizeof(id), flags);
}

void DX8Wrapper::Gx_Log_Raw_Access(const void *caller)
{
	Gx_Log_Ptr(gxlog::RAW_ACCESS, 0, 0, caller, 0);
}

// A buffer as Apply reads it: the D3D handle only for the DX8 types (the sorting ones bind nothing there).
static gxlog::VbDesc Gx_Vb_Desc(unsigned type, const VertexBufferClass *vb)
{
	gxlog::VbDesc d;
	memset(&d, 0, sizeof(d));
	d.type = type;
	d.obj = (uint64_t)(uintptr_t)vb;
	if (vb) {
		if (type == BUFFER_TYPE_DX8 || type == BUFFER_TYPE_DYNAMIC_DX8)
			d.d3d = (uint64_t)(uintptr_t)static_cast<DX8VertexBufferClass*>(const_cast<VertexBufferClass*>(vb))->Get_DX8_Vertex_Buffer();
		d.fvfSize = vb->FVF_Info().Get_FVF_Size();
		d.fvf = vb->FVF_Info().Get_FVF();
		d.vertexCount = vb->Get_Vertex_Count();
	}
	return d;
}

static gxlog::IbDesc Gx_Ib_Desc(unsigned type, const IndexBufferClass *ib)
{
	gxlog::IbDesc d;
	memset(&d, 0, sizeof(d));
	d.type = type;
	d.obj = (uint64_t)(uintptr_t)ib;
	if (ib && (type == BUFFER_TYPE_DX8 || type == BUFFER_TYPE_DYNAMIC_DX8))
		d.d3d = (uint64_t)(uintptr_t)static_cast<DX8IndexBufferClass*>(const_cast<IndexBufferClass*>(ib))->Get_DX8_Index_Buffer();
	return d;
}

static void Gx_Logical(const RenderStateStruct &s, gxlog::Logical &o)
{
	memset(&o, 0, sizeof(o));
	o.shaderBits = s.shader.Get_Bits();
	o.material = (uint64_t)(uintptr_t)s.material;
	for (int i = 0; i < gxlog::kStages; ++i) o.tex[i] = (uint64_t)(uintptr_t)s.Textures[i];
	static_assert(sizeof(o.lights) == sizeof(s.Lights) && sizeof(o.tex) / sizeof(o.tex[0]) == MAX_TEXTURE_STAGES, "Logical layout");
	memcpy(o.lights, s.Lights, sizeof(o.lights));
	for (int l = 0; l < gxlog::kLights; ++l) o.lightEnable[l] = s.LightEnable[l];
	memcpy(&o.world, &s.world, sizeof(o.world));
	memcpy(&o.view, &s.view, sizeof(o.view));
	for (int i = 0; i < 2; ++i) o.vb[i] = Gx_Vb_Desc(s.vertex_buffer_types[i], s.vertex_buffers[i]);
	o.ib = Gx_Ib_Desc(s.index_buffer_type, s.index_buffer);
	o.vbaOffset = s.vba_offset;
	o.vbaCount = s.vba_count;
	o.ibaOffset = s.iba_offset;
	o.indexBaseOffset = s.index_base_offset;
}

void DX8Wrapper::Gx_Log_Render_State(unsigned op, unsigned a, const RenderStateStruct &state)
{
	gxlog::Logical l;
	Gx_Logical(state, l);
	Gx_Log(op, a, 0, &l, sizeof(l), 0);
}

void DX8Wrapper::Gx_Log_Vertex_Buffer(unsigned op, unsigned a, unsigned b, unsigned type, const VertexBufferClass *vb)
{
	const gxlog::VbDesc d = Gx_Vb_Desc(type, vb);
	Gx_Log(op, a, b, &d, sizeof(d), 0);
}

void DX8Wrapper::Gx_Log_Index_Buffer(unsigned op, unsigned a, unsigned b, unsigned type, const IndexBufferClass *ib)
{
	const gxlog::IbDesc d = Gx_Ib_Desc(type, ib);
	Gx_Log(op, a, b, &d, sizeof(d), 0);
}

// SET_LIGHT_ENV at GxLogParent 0, read with Set_Light_Environment's own getters; then the body runs with the parent raised (its
// Set_Light / ambient send are the entry's port). The caller lowers it on each exit.
void DX8Wrapper::Gx_Log_Light_Environment(LightEnvironmentClass *light_env)
{
	if (GxLogParent == 0) {
		gxlog::Lenv e;
		memset(&e, 0, sizeof(e));
		if (light_env) {
			e.count = light_env->Get_Light_Count();
			e.ambientColor = Convert_Color(light_env->Get_Equivalent_Ambient(), 0.0f);
			for (int l = 0; l < e.count && l < gxlog::kLights; ++l) {
				gxlog::LenvLight &d = e.l[l];
				d.point = light_env->isPointLight(l);
				memcpy(d.dir, &light_env->Get_Light_Direction(l), sizeof(d.dir));
				memcpy(d.diff, &light_env->Get_Light_Diffuse(l), sizeof(d.diff));
				if (d.point) {
					memcpy(d.pdiff, &light_env->getPointDiffuse(l), sizeof(d.pdiff));
					memcpy(d.pamb, &light_env->getPointAmbient(l), sizeof(d.pamb));
					memcpy(d.pcenter, &light_env->getPointCenter(l), sizeof(d.pcenter));
					d.irad = light_env->getPointIrad(l);
					d.orad = light_env->getPointOrad(l);
				}
			}
		}
		Gx_Log(gxlog::SET_LIGHT_ENV, 0, light_env != nullptr, light_env ? &e : nullptr, light_env ? sizeof(e) : 0, 0);
	}
	++GxLogParent;
}

void DX8Wrapper::Gx_Log_Draw(unsigned primitive_type, unsigned start_index, unsigned polygon_count, unsigned min_vertex_index,
	unsigned vertex_count)
{
	GxLogDrawRec = -1;
	if (GxLogParent != 0 || GxLog == nullptr) return;
	gxlog::DrawRec d;
	memset(&d, 0, sizeof(d));
	d.args.prim = primitive_type;
	d.args.startIndex = start_index;
	d.args.polyCount = polygon_count;
	d.args.minIndex = min_vertex_index;
	d.args.vertexCount = vertex_count;
	d.lowBound = DrawPolygonLowBoundLimit;
	d.trianglesOn = _Is_Triangle_Draw_Enabled() ? 1 : 0;
	GxLogDrawRec = (int)GxLog->rec.size();
	Gx_Log(gxlog::OP_DRAW, 0, 0, &d, sizeof(d), 0);
}

// Draw_Sorting_IB_VB's dynamic buffers, written into the open DRAW record's payload in place.
void DX8Wrapper::Gx_Log_Draw_Sort(IDirect3DVertexBuffer8 *vb, IDirect3DIndexBuffer8 *ib, unsigned fvf_size, unsigned fvf,
	unsigned vb_offset, unsigned ib_offset)
{
	if (GxLog == nullptr || GxLogDrawRec < 0 || (size_t)GxLogDrawRec >= GxLog->rec.size()) return;
	const gxlog::Record &r = GxLog->rec[GxLogDrawRec];
	if (r.op != gxlog::OP_DRAW || r.payload == ~0u) return;
	gxlog::DrawRec *d = (gxlog::DrawRec *)(GxLog->blob.data() + r.payload);
	d->hasSort = 1;
	d->sb.vb = (uint64_t)(uintptr_t)vb;
	d->sb.ib = (uint64_t)(uintptr_t)ib;
	d->sb.fvfSize = fvf_size;
	d->sb.fvf = fvf;
	d->sb.vbOffset = vb_offset;
	d->sb.ibOffset = ib_offset;
}

void DX8Wrapper::Gx_Log_Apply(bool begin)
{
	if (begin) {
		Gx_Log(gxlog::APPLY_BEGIN, 0, 0, nullptr, 0, 0);
		++GxLogParent;
		return;
	}
	GxLogBracket = gxlog::BR_NONE;
	GxLogStage = 0;
	--GxLogParent;
	Gx_Log(gxlog::APPLY_END, 0, 0, nullptr, 0, 0);
}

void DX8Wrapper::Gx_Log_Body_Open(unsigned op, bool parent)
{
	if (op != 0 && (op >= gxlog::ENTRY || GxLogParent == 0)) Gx_Log(op, 0, 0, nullptr, 0, 0);	// READ / RESOURCE always
	++GxLogScope;
	if (parent) ++GxLogParent;
}

void DX8Wrapper::Gx_Log_Body_Close(bool parent)
{
	--GxLogScope;
	if (parent) --GxLogParent;
}

void DX8Wrapper::Gx_Log_Snapshot(gxlog::Snapshot &out)
{
	static_assert(sizeof(out.cacheRS) == sizeof(RenderStates) && sizeof(out.cacheTSS) == sizeof(TextureStageStates)
		&& sizeof(out.lights) == sizeof(CurrentDX8Lights) && sizeof(out.truthRS) == sizeof(GxTruthRS)
		&& sizeof(out.truthRSKnown) == sizeof(GxTruthRSKnown) && sizeof(out.truthTSS) == sizeof(GxTruthTSS)
		&& sizeof(out.truthTSSKnown) == sizeof(GxTruthTSSKnown) && sizeof(out.texmSent) == sizeof(GxTexmSent), "Snapshot layout");
	memset(&out, 0, sizeof(out));
	Gx_Logical(render_state, out.rs);
	out.changed = render_state_changed;
	memcpy(out.cacheRS, RenderStates, sizeof(out.cacheRS));
	memcpy(out.cacheTSS, TextureStageStates, sizeof(out.cacheTSS));
	for (int i = 0; i < gxlog::kStages; ++i) out.devTex[i] = (uint64_t)(uintptr_t)Textures[i];
	memcpy(out.lights, CurrentDX8Lights, sizeof(out.lights));
	for (int l = 0; l < gxlog::kLights; ++l) {
		out.lightOn[l] = CurrentDX8LightEnables[l];
		out.lightValid[l] = CurrentDX8LightValid[l];
	}
	out.lightSerial = LightStateSerial;
	out.lenvKey.count = s_lenvKey.count;	// ambientColor stays 0: the key holds none
	for (int l = 0; l < gxlog::kLights && l < LightEnvironmentClass::MAX_LIGHTS; ++l) {
		gxlog::LenvLight &d = out.lenvKey.l[l];
		d.point = s_lenvKey.point[l];
		memcpy(d.dir, &s_lenvKey.dir[l], sizeof(d.dir));
		memcpy(d.diff, &s_lenvKey.diff[l], sizeof(d.diff));
		memcpy(d.pdiff, &s_lenvKey.pdiff[l], sizeof(d.pdiff));
		memcpy(d.pamb, &s_lenvKey.pamb[l], sizeof(d.pamb));
		memcpy(d.pcenter, &s_lenvKey.pcenter[l], sizeof(d.pcenter));
		d.irad = s_lenvKey.irad[l];
		d.orad = s_lenvKey.orad[l];
	}
	out.lenvKeyValid = s_lenvKeyValid;
	out.lenvSerial = s_lenvSerial;
	memcpy(&out.xformWorld, &DX8Transforms[D3DTS_WORLD], sizeof(out.xformWorld));
	memcpy(&out.xformView, &DX8Transforms[D3DTS_VIEW], sizeof(out.xformView));
	out.xformShadowValid[0] = XformShadowValid[0];
	out.xformShadowValid[1] = XformShadowValid[1];
	memcpy(&out.matShadow, &MaterialShadow, sizeof(out.matShadow));
	out.matShadowValid = MaterialShadowValid;
	out.curShader = (uint32_t)ShaderClass::CurrentShader;	// DX8Wrapper is ShaderClass's friend
	out.shaderDirty = ShaderClass::ShaderDirty;
	out.vsCache = Vertex_Shader;
	out.wrapSkip = Wrap_Skip();
	const DX8Caps *caps = Get_Current_Caps();
	out.maxTexPerPass = caps->Get_Max_Textures_Per_Pass();
	out.truthOn = GxTruthOn;
	out.captureOn = GxCaptureOn;
	memcpy(out.truthRS, GxTruthRS, sizeof(out.truthRS));
	memcpy(out.truthRSKnown, GxTruthRSKnown, sizeof(out.truthRSKnown));
	memcpy(out.truthTSS, GxTruthTSS, sizeof(out.truthTSS));
	memcpy(out.truthTSSKnown, GxTruthTSSKnown, sizeof(out.truthTSSKnown));
	memcpy(&out.matSent, &GxMaterialSent, sizeof(out.matSent));
	out.matSentValid = GxMaterialSentValid;
	memcpy(&out.projSent, &GxProjSent, sizeof(out.projSent));
	out.projSentValid = GxProjSentValid;
	memcpy(out.texmSent, GxTexmSent, sizeof(out.texmSent));
	out.texmValid = GxTexmValid;
	memcpy(&out.viewSent, &GxViewSent, sizeof(out.viewSent));
	out.viewSentValid = GxViewSentValid;
	out.xformValid = GxXformValid;
	// ShaderClass::Apply's other inputs, read as it reads them (ZW/shader.cpp:409-1060). _PolygonCullMode is a file global there whose
	// only writer (Invert_Backface_Culling) stores CCW or CW, so the public getter gives its exact value.
	out.sg.fogAllowed = caps->Is_Fog_Allowed();
	out.sg.fogEnable = Get_Fog_Enable();
	out.sg.fogColor = FogColor;
	out.sg.polygonCullMode = ShaderClass::Is_Backface_Culling_Inverted() ? D3DCULL_CCW : D3DCULL_CW;
	out.sg.textureOpCaps = caps->Get_DX8_Caps().TextureOpCaps;
	out.sg.npatches = caps->Support_NPatches();
	out.sg.vendor = caps->Get_Vendor();
	out.sg.device = caps->Get_Device();
	out.sg.modAlphaAddClr = caps->Support_ModAlphaAddClr();
	out.sg.npatchesLevel = WW3D::Get_NPatches_Level();
}

IDirect3DSurface8 * DX8Wrapper::_Get_DX8_Front_Buffer()
{
	DX8_THREAD_ASSERT();
	D3DDISPLAYMODE mode;

	DX8CALL(GetDisplayMode(&mode));

	IDirect3DSurface8 * fb=nullptr;

	DX8CALL(CreateImageSurface(mode.Width,mode.Height,D3DFMT_A8R8G8B8,&fb));

	DX8CALL(GetFrontBuffer(fb));
	return fb;
}

SurfaceClass * DX8Wrapper::_Get_DX8_Back_Buffer(unsigned int num)
{
	DX8_THREAD_ASSERT();

	IDirect3DSurface8 * bb;
	SurfaceClass *surf=nullptr;
	DX8CALL(GetBackBuffer(num,D3DBACKBUFFER_TYPE_MONO,&bb));
	if (bb)
	{
		surf=NEW_REF(SurfaceClass,(bb));
		bb->Release();
	}

	return surf;
}


TextureClass *
DX8Wrapper::Create_Render_Target (int width, int height, WW3DFormat format)
{
	DX8_THREAD_ASSERT();
	DX8_Assert();
	DX8_RECORD_DX8_CALLS();

	// Use the current display format if format isn't specified
	if (format==WW3D_FORMAT_UNKNOWN) {
		D3DDISPLAYMODE mode;
		DX8CALL(GetDisplayMode(&mode));
		format=D3DFormat_To_WW3DFormat(mode.Format);
	}

	// If render target format isn't supported return null
	if (!Get_Current_Caps()->Support_Render_To_Texture_Format(format)) {
		WWDEBUG_SAY(("DX8Wrapper - Render target format is not supported"));
		return nullptr;
	}

	//
	//	Note: We're going to force the width and height to be powers of two and equal
	//
	const D3DCAPS8& dx8caps=Get_Current_Caps()->Get_DX8_Caps();
	float poweroftwosize = width;
	if (height > 0 && height < width) {
		poweroftwosize = height;
	}
	poweroftwosize = ::Find_POT (poweroftwosize);

	if (poweroftwosize>dx8caps.MaxTextureWidth) {
		poweroftwosize=dx8caps.MaxTextureWidth;
	}
	if (poweroftwosize>dx8caps.MaxTextureHeight) {
		poweroftwosize=dx8caps.MaxTextureHeight;
	}

	width = height = poweroftwosize;

	//
	//	Attempt to create the render target
	//
	TextureClass * tex = NEW_REF(TextureClass,(width,height,format,MIP_LEVELS_1,TextureClass::POOL_DEFAULT,true));

	// 3dfx drivers are lying in the CheckDeviceFormat call and claiming
	// that they support render targets!
	if (tex->Peek_D3D_Base_Texture() == nullptr)
	{
		WWDEBUG_SAY(("DX8Wrapper - Render target creation failed!"));
		REF_PTR_RELEASE(tex);
	}

	return tex;
}

//**********************************************************************************************
//! Create render target with associated depth stencil buffer
/*! KJM
*/
void DX8Wrapper::Create_Render_Target
(
	int width,
	int height,
	WW3DFormat format,
	WW3DZFormat zformat,
	TextureClass** target,
	ZTextureClass** depth_buffer
)
{
	DX8_THREAD_ASSERT();
	DX8_Assert();
	DX8_RECORD_DX8_CALLS();

	// Use the current display format if format isn't specified
	if (format==WW3D_FORMAT_UNKNOWN)
	{
		*target=nullptr;
		*depth_buffer=nullptr;
		return;
/*		D3DDISPLAYMODE mode;
		DX8CALL(GetDisplayMode(&mode));
		format=D3DFormat_To_WW3DFormat(mode.Format);*/
	}

	// If render target format isn't supported return null
	if (!Get_Current_Caps()->Support_Render_To_Texture_Format(format) ||
		 !Get_Current_Caps()->Support_Depth_Stencil_Format(zformat))
	{
		WWDEBUG_SAY(("DX8Wrapper - Render target with depth format is not supported"));
		return;
	}

	//	Note: We're going to force the width and height to be powers of two and equal
	const D3DCAPS8& dx8caps=Get_Current_Caps()->Get_DX8_Caps();
	float poweroftwosize = width;
	if (height > 0 && height < width)
	{
		poweroftwosize = height;
	}
	poweroftwosize = ::Find_POT (poweroftwosize);

	if (poweroftwosize>dx8caps.MaxTextureWidth)
	{
		poweroftwosize=dx8caps.MaxTextureWidth;
	}

	if (poweroftwosize>dx8caps.MaxTextureHeight)
	{
		poweroftwosize=dx8caps.MaxTextureHeight;
	}

	width = height = poweroftwosize;

	//	Attempt to create the render target
	TextureClass* tex=NEW_REF(TextureClass,(width,height,format,MIP_LEVELS_1,TextureClass::POOL_DEFAULT,true));

	// 3dfx drivers are lying in the CheckDeviceFormat call and claiming
	// that they support render targets!
	if (tex->Peek_D3D_Base_Texture() == nullptr)
	{
		WWDEBUG_SAY(("DX8Wrapper - Render target creation failed!"));
		REF_PTR_RELEASE(tex);
	}

	*target=tex;

	// attempt to create the depth stencil buffer
	*depth_buffer=NEW_REF
	(
		ZTextureClass,
		(
			width,
			height,
			zformat,
			MIP_LEVELS_1,
			TextureClass::POOL_DEFAULT
		)
	);
}

/*!
 * Set render target
 * KM Added optional custom z target
 */
void DX8Wrapper::Set_Render_Target_With_Z
(
	TextureClass* texture,
	ZTextureClass* ztexture
)
{
	WWASSERT(texture!=nullptr);
	IDirect3DSurface8 * d3d_surf = texture->Get_D3D_Surface_Level();
	WWASSERT(d3d_surf != nullptr);

	IDirect3DSurface8* d3d_zbuf=nullptr;
	if (ztexture!=nullptr)
	{

		d3d_zbuf=ztexture->Get_D3D_Surface_Level();
		WWASSERT(d3d_zbuf!=nullptr);
		Set_Render_Target(d3d_surf,d3d_zbuf);
		d3d_zbuf->Release();
	}
	else
	{
		Set_Render_Target(d3d_surf,true);
	}
	d3d_surf->Release();

	IsRenderToTexture = true;
}

void
DX8Wrapper::Set_Render_Target(IDirect3DSwapChain8 *swap_chain)
{
	DX8_THREAD_ASSERT();
	WWASSERT (swap_chain != nullptr);

	//
	//	Get the back buffer for the swap chain
	//
	LPDIRECT3DSURFACE8 render_target = nullptr;
	swap_chain->GetBackBuffer (0, D3DBACKBUFFER_TYPE_MONO, &render_target);

	//
	//	Set this back buffer as the render target
	//
	Set_Render_Target (render_target, true);

	//
	//	Release our hold on the back buffer
	//
	if (render_target != nullptr) {
		render_target->Release ();
		render_target = nullptr;
	}

	IsRenderToTexture = false;
}

void
DX8Wrapper::Set_Render_Target(IDirect3DSurface8 *render_target, bool use_default_depth_buffer)
{
	DX8_THREAD_ASSERT();
	DX8_Assert();

	//
	//	Should we restore the default render target set a new one?
	//
	if (render_target == nullptr || render_target == DefaultRenderTarget)
	{
		// If there is currently a custom render target, default must NOT be null.
		if (CurrentRenderTarget)
		{
			WWASSERT(DefaultRenderTarget!=nullptr);
		}

		//
		//	Restore the default render target
		//
		if (DefaultRenderTarget != nullptr)
		{
			DX8CALL(SetRenderTarget (DefaultRenderTarget, DefaultDepthBuffer));
			DefaultRenderTarget->Release ();
			DefaultRenderTarget = nullptr;
			if (DefaultDepthBuffer)
			{
				DefaultDepthBuffer->Release ();
				DefaultDepthBuffer = nullptr;
			}
		}

		//
		//	Release our hold on the "current" render target
		//
		if (CurrentRenderTarget != nullptr)
		{
			CurrentRenderTarget->Release ();
			CurrentRenderTarget = nullptr;
		}

		if (CurrentDepthBuffer!=nullptr)
		{
			CurrentDepthBuffer->Release();
			CurrentDepthBuffer=nullptr;
		}

	}
	else if (render_target != CurrentRenderTarget)
	{
		// GeneralsX: A custom-to-custom switch is legal: DefaultRenderTarget is only
		// captured while it is null, so it keeps the true backbuffer across nested switches. The assert that
		// stood here forbade exactly the nesting pillarbox -> script filter needs.

		//
		//	We'll need the depth buffer later...
		//
		if (DefaultDepthBuffer == nullptr)
		{
//		IDirect3DSurface8 *depth_buffer = nullptr;
			DX8CALL(GetDepthStencilSurface (&DefaultDepthBuffer));
		}

		//
		//	Get a pointer to the default render target (if necessary)
		//
		if (DefaultRenderTarget == nullptr)
		{
			DX8CALL(GetRenderTarget (&DefaultRenderTarget));
		}

		//
		//	Release our hold on the old "current" render target
		//
		if (CurrentRenderTarget != nullptr)
		{
			CurrentRenderTarget->Release ();
			CurrentRenderTarget = nullptr;
		}

		if (CurrentDepthBuffer!=nullptr)
		{
			CurrentDepthBuffer->Release();
			CurrentDepthBuffer=nullptr;
		}

		//
		//	Keep a copy of the current render target (for housekeeping)
		//
		CurrentRenderTarget = render_target;
		WWASSERT (CurrentRenderTarget != nullptr);
		if (CurrentRenderTarget != nullptr)
		{
			CurrentRenderTarget->AddRef ();

			//
			//	Switch render targets
			//
			if (use_default_depth_buffer)
			{
				DX8CALL(SetRenderTarget (CurrentRenderTarget, DefaultDepthBuffer));
			}
			else
			{
				DX8CALL(SetRenderTarget (CurrentRenderTarget, nullptr));
			}
		}
	}

	//
	//	Free our hold on the depth buffer
	//
//	if (depth_buffer != nullptr) {
//		depth_buffer->Release ();
//		depth_buffer = nullptr;
//	}

	IsRenderToTexture = false;
}


// GeneralsX: How Set_Render_Target_Pair reads the result of the switch below.
static bool s_quietRenderTargetFailure = false;
static HRESULT s_lastRenderTargetResult = D3D_OK;

//**********************************************************************************************
//! Set render target with depth stencil buffer
/*! KJM
*/
void DX8Wrapper::Set_Render_Target
(
	IDirect3DSurface8* render_target,
	IDirect3DSurface8* depth_buffer
)
{
	DX8_THREAD_ASSERT();
	DX8_Assert();
	DX8Ring::Drain();	// d3d8 ring spike: direct D3DDevice calls below

	//
	//	Should we restore the default render target set a new one?
	//
	if (render_target == nullptr || render_target == DefaultRenderTarget)
	{
		// If there is currently a custom render target, default must NOT be null.
		if (CurrentRenderTarget)
		{
			WWASSERT(DefaultRenderTarget!=nullptr);
		}

		//
		//	Restore the default render target
		//
		if (DefaultRenderTarget != nullptr)
		{
			DX8CALL(SetRenderTarget (DefaultRenderTarget, DefaultDepthBuffer));
			DefaultRenderTarget->Release ();
			DefaultRenderTarget = nullptr;
			if (DefaultDepthBuffer)
			{
				DefaultDepthBuffer->Release ();
				DefaultDepthBuffer = nullptr;
			}
		}

		//
		//	Release our hold on the "current" render target
		//
		if (CurrentRenderTarget != nullptr)
		{
			CurrentRenderTarget->Release ();
			CurrentRenderTarget = nullptr;
		}

		if (CurrentDepthBuffer!=nullptr)
		{
			CurrentDepthBuffer->Release();
			CurrentDepthBuffer=nullptr;
		}
	}
	else if (render_target != CurrentRenderTarget)
	{
		// GeneralsX: Custom-to-custom is legal here too; see the overload above.

		//
		//	We'll need the depth buffer later...
		//
		if (DefaultDepthBuffer == nullptr)
		{
//		IDirect3DSurface8 *depth_buffer = nullptr;
			DX8CALL(GetDepthStencilSurface (&DefaultDepthBuffer));
		}

		//
		//	Get a pointer to the default render target (if necessary)
		//
		if (DefaultRenderTarget == nullptr)
		{
			DX8CALL(GetRenderTarget (&DefaultRenderTarget));
		}

		//
		//	Release our hold on the old "current" render target
		//
		if (CurrentRenderTarget != nullptr)
		{
			CurrentRenderTarget->Release ();
			CurrentRenderTarget = nullptr;
		}

		if (CurrentDepthBuffer!=nullptr)
		{
			CurrentDepthBuffer->Release();
			CurrentDepthBuffer=nullptr;
		}

		//
		//	Keep a copy of the current render target (for housekeeping)
		//
		CurrentRenderTarget = render_target;
		CurrentDepthBuffer = depth_buffer;
		WWASSERT (CurrentRenderTarget != nullptr);
		if (CurrentRenderTarget != nullptr)
		{
			CurrentRenderTarget->AddRef ();
			// GeneralsX: A null depth is a legal binding (the device then has none).
			if (CurrentDepthBuffer != nullptr)
				CurrentDepthBuffer->AddRef();

			//
			//	Switch render targets
			//
			// GeneralsX: Set_Render_Target_Pair asks for this switch quietly and reads
			// the result: a pair the device refuses (TheSuperHackers' driver-forced MSAA depth) is an expected
			// failure there, which DX8CALL would turn into a debug assert.
			const HRESULT hr = D3DDevice->SetRenderTarget (CurrentRenderTarget, CurrentDepthBuffer);
			Increment_DX8_CallCount();
			s_lastRenderTargetResult = hr;
			if (!s_quietRenderTargetFailure)
				DX8_ErrorCode(hr);
		}
	}

	IsRenderToTexture=true;
}

// GeneralsX: See the declaration. The depth comes from the device because
// CurrentDepthBuffer is null after a (surf,bool) bind and after Set_Render_Target_With_Z(tex, nullptr).
void DX8Wrapper::Get_Render_Target_Pair(IDirect3DSurface8 *&rt, IDirect3DSurface8 *&ds)
{
	DX8Ring::Drain();	// d3d8 ring spike: the device must be idle before a direct D3DDevice call
	DX8_THREAD_ASSERT();
	rt = CurrentRenderTarget;
	if (rt != nullptr)
		rt->AddRef();
	ds = nullptr;
	if (FAILED(D3DDevice->GetDepthStencilSurface(&ds)))	// D3DERR_NOTFOUND = no depth bound
		ds = nullptr;
}

bool DX8Wrapper::Set_Render_Target_Pair(IDirect3DSurface8 *rt, IDirect3DSurface8 *ds)
{
	const bool renderToTexture = IsRenderToTexture;
	s_lastRenderTargetResult = D3D_OK;
	if (rt == nullptr)
		Set_Render_Target((IDirect3DSurface8 *)nullptr);
	else
	{
		s_quietRenderTargetFailure = true;
		Set_Render_Target(rt, ds);
		s_quietRenderTargetFailure = false;
	}
	IsRenderToTexture = renderToTexture;
	return SUCCEEDED(s_lastRenderTargetResult);
}


IDirect3DSwapChain8 *
DX8Wrapper::Create_Additional_Swap_Chain (HWND render_window)
{
	DX8_Assert();

	//
	//	Configure the presentation parameters for a windowed render target
	//
	D3DPRESENT_PARAMETERS params				= { 0 };
	params.BackBufferFormat						= _PresentParameters.BackBufferFormat;
	params.BackBufferCount						= 1;
	params.MultiSampleType						= D3DMULTISAMPLE_NONE;
	params.SwapEffect								= D3DSWAPEFFECT_COPY_VSYNC;
	params.hDeviceWindow							= render_window;
	params.Windowed								= TRUE;
	params.EnableAutoDepthStencil				= TRUE;
	params.AutoDepthStencilFormat				= _PresentParameters.AutoDepthStencilFormat;
	params.Flags									= 0;
	params.FullScreen_RefreshRateInHz		= D3DPRESENT_RATE_DEFAULT;
	params.FullScreen_PresentationInterval	= D3DPRESENT_INTERVAL_DEFAULT;

	//
	//	Create the swap chain
	//
	IDirect3DSwapChain8 *swap_chain = nullptr;
	DX8CALL(CreateAdditionalSwapChain(&params, &swap_chain));
	return swap_chain;
}

void DX8Wrapper::Flush_DX8_Resource_Manager(unsigned int bytes)
{
	DX8_Assert();
	DX8CALL(ResourceManagerDiscardBytes(bytes));
}

unsigned int DX8Wrapper::Get_Free_Texture_RAM()
{
	DX8_Assert();
	DX8_RECORD_DX8_CALLS();
	return DX8Wrapper::_Get_D3D_Device8()->GetAvailableTextureMem();
}

// Converts a linear gamma ramp to one that is controlled by:
// Gamma - controls the curvature of the middle of the curve
// Bright - controls the minimum value of the curve
// Contrast - controls the difference between the maximum and the minimum of the curve
void DX8Wrapper::Set_Gamma(float gamma,float bright,float contrast,bool calibrate,bool uselimit)
{
	gamma=Bound(gamma,0.6f,6.0f);
	bright=Bound(bright,-0.5f,0.5f);
	contrast=Bound(contrast,0.5f,2.0f);
	float oo_gamma=1.0f/gamma;

	DX8_Assert();
	DX8_RECORD_DX8_CALLS();

	DWORD flag=(calibrate?D3DSGR_CALIBRATE:D3DSGR_NO_CALIBRATION);

	D3DGAMMARAMP ramp;
	float			 limit;

	// IML: I'm not really sure what the intent of the 'limit' variable is. It does not produce useful results for my purposes.
	if (uselimit) {
		limit=(contrast-1)/2*contrast;
	} else {
		limit = 0.0f;
	}

	// HY - arrived at this equation after much trial and error.
	for (int i=0; i<256; i++) {
		float in,out;
		in=i/256.0f;
		float x=in-limit;
		x=Bound(x,0.0f,1.0f);
		x=powf(x,oo_gamma);
		out=contrast*x+bright;
		out=Bound(out,0.0f,1.0f);
		ramp.red[i]=(WORD) (out*65535);
		ramp.green[i]=(WORD) (out*65535);
		ramp.blue[i]=(WORD) (out*65535);
	}

	if (Get_Current_Caps()->Support_Gamma())	{
		DX8Wrapper::_Get_D3D_Device8()->SetGammaRamp(flag,&ramp);
	}
	// GeneralsX @build fbraz 10/02/2026 GDI gamma fallback Windows-only
	// Linux: Gamma adjustment via device only (no GDI fallback)
#ifdef _WIN32
	else {
		HWND hwnd = GetDesktopWindow();
		HDC hdc = GetDC(hwnd);
		if (hdc)
		{
			SetDeviceGammaRamp (hdc, &ramp);
			ReleaseDC (hwnd, hdc);
		}
	}
#endif
}

namespace wrapper
{
void D3DMatrixIdentity(D3DMATRIX* dxm)
{
	memset(dxm, 0, sizeof(*dxm));
	dxm->_11 = 1.0f;
	dxm->_22 = 1.0f;
	dxm->_33 = 1.0f;
	dxm->_44 = 1.0f;
}
} // namespace wrapper

void DX8Wrapper::Set_World_Identity()
{
	if (GxLogOn && GxLogParent == 0) Gx_Log(gxlog::SET_WORLD_IDENTITY, 0, 0, nullptr, 0, 0);	// GeneralsX: WS-G Task 5
	if (render_state_changed&(unsigned)WORLD_IDENTITY)
		return;
	// GeneralsX @build fbraz 10/02/2026 Use Matrix4x4 native method
	render_state.world.Make_Identity();
	render_state_changed|=(unsigned)WORLD_CHANGED|(unsigned)WORLD_IDENTITY;
}

void DX8Wrapper::Set_View_Identity()
{
	if (GxLogOn && GxLogParent == 0) Gx_Log(gxlog::SET_VIEW_IDENTITY, 0, 0, nullptr, 0, 0);	// GeneralsX: WS-G Task 5
	if (render_state_changed&(unsigned)VIEW_IDENTITY)
		return;
	// GeneralsX @build fbraz 10/02/2026 Use Matrix4x4 native method
	render_state.view.Make_Identity();
	render_state_changed|=(unsigned)VIEW_CHANGED|(unsigned)VIEW_IDENTITY;
}

//**********************************************************************************************
//! Resets render device to default state
/*!
*/
void DX8Wrapper::Apply_Default_State()
{
	SNAPSHOT_SAY(("DX8Wrapper::Apply_Default_State()"));

	// only set states used in game
	Set_DX8_Render_State(D3DRS_ZENABLE, TRUE);
//	Set_DX8_Render_State(D3DRS_FILLMODE, D3DFILL_SOLID);
	Set_DX8_Render_State(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
	//Set_DX8_Render_State(D3DRS_LINEPATTERN, 0);
	Set_DX8_Render_State(D3DRS_ZWRITEENABLE, TRUE);
	Set_DX8_Render_State(D3DRS_ALPHATESTENABLE, FALSE);
	//Set_DX8_Render_State(D3DRS_LASTPIXEL, FALSE);
	Set_DX8_Render_State(D3DRS_SRCBLEND, D3DBLEND_ONE);
	Set_DX8_Render_State(D3DRS_DESTBLEND, D3DBLEND_ZERO);
	Set_DX8_Render_State(D3DRS_CULLMODE, D3DCULL_CW);
	Set_DX8_Render_State(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	Set_DX8_Render_State(D3DRS_ALPHAREF, 0);
	Set_DX8_Render_State(D3DRS_ALPHAFUNC, D3DCMP_LESSEQUAL);
	Set_DX8_Render_State(D3DRS_DITHERENABLE, FALSE);
	Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE, FALSE);
	Set_DX8_Render_State(D3DRS_FOGENABLE, FALSE);
	Set_DX8_Render_State(D3DRS_SPECULARENABLE, FALSE);
//	Set_DX8_Render_State(D3DRS_ZVISIBLE, FALSE);
//	Set_DX8_Render_State(D3DRS_FOGCOLOR, 0);
//	Set_DX8_Render_State(D3DRS_FOGTABLEMODE, D3DFOG_NONE);
//	Set_DX8_Render_State(D3DRS_FOGSTART, 0);

//	Set_DX8_Render_State(D3DRS_FOGEND, WWMath::Float_As_Int(1.0f));
//	Set_DX8_Render_State(D3DRS_FOGDENSITY, WWMath::Float_As_Int(1.0f));

	//Set_DX8_Render_State(D3DRS_EDGEANTIALIAS, FALSE);
	Set_DX8_Render_State(D3DRS_ZBIAS, 0);
//	Set_DX8_Render_State(D3DRS_RANGEFOGENABLE, FALSE);
	Set_DX8_Render_State(D3DRS_STENCILENABLE, FALSE);
	Set_DX8_Render_State(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
	Set_DX8_Render_State(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
	Set_DX8_Render_State(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
	Set_DX8_Render_State(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
	Set_DX8_Render_State(D3DRS_STENCILREF, 0);
	Set_DX8_Render_State(D3DRS_STENCILMASK, 0xffffffff);
	Set_DX8_Render_State(D3DRS_STENCILWRITEMASK, 0xffffffff);
	Set_DX8_Render_State(D3DRS_TEXTUREFACTOR, 0);
/*	Set_DX8_Render_State(D3DRS_WRAP0, D3DWRAP_U| D3DWRAP_V);
	Set_DX8_Render_State(D3DRS_WRAP1, D3DWRAP_U| D3DWRAP_V);
	Set_DX8_Render_State(D3DRS_WRAP2, D3DWRAP_U| D3DWRAP_V);
	Set_DX8_Render_State(D3DRS_WRAP3, D3DWRAP_U| D3DWRAP_V);
	Set_DX8_Render_State(D3DRS_WRAP4, D3DWRAP_U| D3DWRAP_V);
	Set_DX8_Render_State(D3DRS_WRAP5, D3DWRAP_U| D3DWRAP_V);
	Set_DX8_Render_State(D3DRS_WRAP6, D3DWRAP_U| D3DWRAP_V);
	Set_DX8_Render_State(D3DRS_WRAP7, D3DWRAP_U| D3DWRAP_V);*/
	Set_DX8_Render_State(D3DRS_CLIPPING, TRUE);
	Set_DX8_Render_State(D3DRS_LIGHTING, FALSE);
	//Set_DX8_Render_State(D3DRS_AMBIENT, 0);
//	Set_DX8_Render_State(D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
	Set_DX8_Render_State(D3DRS_COLORVERTEX, TRUE);
/*	Set_DX8_Render_State(D3DRS_LOCALVIEWER, TRUE);
	Set_DX8_Render_State(D3DRS_NORMALIZENORMALS, FALSE);
	Set_DX8_Render_State(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
	Set_DX8_Render_State(D3DRS_SPECULARMATERIALSOURCE, D3DMCS_COLOR2);
	Set_DX8_Render_State(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
	Set_DX8_Render_State(D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
	Set_DX8_Render_State(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);*/
	//Set_DX8_Render_State(D3DRS_CLIPPLANEENABLE, 0);
	Set_DX8_Render_State(D3DRS_SOFTWAREVERTEXPROCESSING, FALSE);
	//Set_DX8_Render_State(D3DRS_POINTSIZE, 0x3f800000);
	//Set_DX8_Render_State(D3DRS_POINTSIZE_MIN, 0);
	//Set_DX8_Render_State(D3DRS_POINTSPRITEENABLE, FALSE);
	//Set_DX8_Render_State(D3DRS_POINTSCALEENABLE, FALSE);
	//Set_DX8_Render_State(D3DRS_POINTSCALE_A, 0);
	//Set_DX8_Render_State(D3DRS_POINTSCALE_B, 0);
	//Set_DX8_Render_State(D3DRS_POINTSCALE_C, 0);
	//Set_DX8_Render_State(D3DRS_MULTISAMPLEANTIALIAS, TRUE);
	//Set_DX8_Render_State(D3DRS_MULTISAMPLEMASK, 0xffffffff);
	//Set_DX8_Render_State(D3DRS_PATCHEDGESTYLE, D3DPATCHEDGE_DISCRETE);
	//Set_DX8_Render_State(D3DRS_PATCHSEGMENTS, 0x3f800000);
	//Set_DX8_Render_State(D3DRS_DEBUGMONITORTOKEN, D3DDMT_ENABLE);
	//Set_DX8_Render_State(D3DRS_POINTSIZE_MAX, Float_At_Int(64.0f));
	//Set_DX8_Render_State(D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);
	Set_DX8_Render_State(D3DRS_COLORWRITEENABLE, 0x0000000f);
	//Set_DX8_Render_State(D3DRS_TWEENFACTOR, 0);
	Set_DX8_Render_State(D3DRS_BLENDOP, D3DBLENDOP_ADD);
	//Set_DX8_Render_State(D3DRS_POSITIONORDER, D3DORDER_CUBIC);
	//Set_DX8_Render_State(D3DRS_NORMALORDER, D3DORDER_LINEAR);

	// disable TSS stages
	int i;
	for (i=0; i<CurrentCaps->Get_Max_Textures_Per_Pass(); i++)
	{
		Set_DX8_Texture_Stage_State(i, D3DTSS_COLOROP, D3DTOP_DISABLE);
		Set_DX8_Texture_Stage_State(i, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		Set_DX8_Texture_Stage_State(i, D3DTSS_COLORARG2, D3DTA_DIFFUSE);

		Set_DX8_Texture_Stage_State(i, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
		Set_DX8_Texture_Stage_State(i, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		Set_DX8_Texture_Stage_State(i, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

		/*Set_DX8_Texture_Stage_State(i, D3DTSS_BUMPENVMAT00, 0);
		Set_DX8_Texture_Stage_State(i, D3DTSS_BUMPENVMAT01, 0);
		Set_DX8_Texture_Stage_State(i, D3DTSS_BUMPENVMAT10, 0);
		Set_DX8_Texture_Stage_State(i, D3DTSS_BUMPENVMAT11, 0);
		Set_DX8_Texture_Stage_State(i, D3DTSS_BUMPENVLSCALE, 0);
		Set_DX8_Texture_Stage_State(i, D3DTSS_BUMPENVLOFFSET, 0);*/

		Set_DX8_Texture_Stage_State(i, D3DTSS_TEXCOORDINDEX, i);


		Set_DX8_Texture_Stage_State(i, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
		Set_DX8_Texture_Stage_State(i, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
		Set_DX8_Texture_Stage_State(i, D3DTSS_BORDERCOLOR, 0);
//		Set_DX8_Texture_Stage_State(i, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
//		Set_DX8_Texture_Stage_State(i, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
//		Set_DX8_Texture_Stage_State(i, D3DTSS_MIPFILTER, D3DTEXF_LINEAR);
//		Set_DX8_Texture_Stage_State(i, D3DTSS_MIPMAPLODBIAS, 0);
//		Set_DX8_Texture_Stage_State(i, D3DTSS_MAXMIPLEVEL, 0);
//		Set_DX8_Texture_Stage_State(i, D3DTSS_MAXANISOTROPY, 1);
		//Set_DX8_Texture_Stage_State(i, D3DTSS_ADDRESSW, D3DTADDRESS_WRAP);
		//Set_DX8_Texture_Stage_State(i, D3DTSS_COLORARG0, D3DTA_CURRENT);
		//Set_DX8_Texture_Stage_State(i, D3DTSS_ALPHAARG0, D3DTA_CURRENT);
		//Set_DX8_Texture_Stage_State(i, D3DTSS_RESULTARG, D3DTA_CURRENT);

		Set_DX8_Texture_Stage_State(i, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
		Set_Texture(i,nullptr);
	}

//	DX8Wrapper::Set_Material(nullptr);
	VertexMaterialClass::Apply_Null();

	for (unsigned index=0;index<4;++index) {
		SNAPSHOT_SAY(("Clearing light %d to null",index));
		Set_DX8_Light(index,nullptr);
	}

	// set up simple default TSS
	Vector4 vconst[MAX_VERTEX_SHADER_CONSTANTS];
	memset(vconst,0,sizeof(Vector4)*MAX_VERTEX_SHADER_CONSTANTS);
	Set_Vertex_Shader_Constant(0, vconst, MAX_VERTEX_SHADER_CONSTANTS);

	Vector4 pconst[MAX_PIXEL_SHADER_CONSTANTS];
	memset(pconst,0,sizeof(Vector4)*MAX_PIXEL_SHADER_CONSTANTS);
	Set_Pixel_Shader_Constant(0, pconst, MAX_PIXEL_SHADER_CONSTANTS);

	Set_Vertex_Shader(DX8_FVF_XYZNDUV2);
	Set_Pixel_Shader(0);

	ShaderClass::Invalidate();
}

const char* DX8Wrapper::Get_DX8_Render_State_Name(D3DRENDERSTATETYPE state)
{
	switch (state) {
	case D3DRS_ZENABLE                       : return "D3DRS_ZENABLE";
	case D3DRS_FILLMODE                      : return "D3DRS_FILLMODE";
	case D3DRS_SHADEMODE                     : return "D3DRS_SHADEMODE";
	case D3DRS_LINEPATTERN                   : return "D3DRS_LINEPATTERN";
	case D3DRS_ZWRITEENABLE                  : return "D3DRS_ZWRITEENABLE";
	case D3DRS_ALPHATESTENABLE               : return "D3DRS_ALPHATESTENABLE";
	case D3DRS_LASTPIXEL                     : return "D3DRS_LASTPIXEL";
	case D3DRS_SRCBLEND                      : return "D3DRS_SRCBLEND";
	case D3DRS_DESTBLEND                     : return "D3DRS_DESTBLEND";
	case D3DRS_CULLMODE                      : return "D3DRS_CULLMODE";
	case D3DRS_ZFUNC                         : return "D3DRS_ZFUNC";
	case D3DRS_ALPHAREF                      : return "D3DRS_ALPHAREF";
	case D3DRS_ALPHAFUNC                     : return "D3DRS_ALPHAFUNC";
	case D3DRS_DITHERENABLE                  : return "D3DRS_DITHERENABLE";
	case D3DRS_ALPHABLENDENABLE              : return "D3DRS_ALPHABLENDENABLE";
	case D3DRS_FOGENABLE                     : return "D3DRS_FOGENABLE";
	case D3DRS_SPECULARENABLE                : return "D3DRS_SPECULARENABLE";
	case D3DRS_ZVISIBLE                      : return "D3DRS_ZVISIBLE";
	case D3DRS_FOGCOLOR                      : return "D3DRS_FOGCOLOR";
	case D3DRS_FOGTABLEMODE                  : return "D3DRS_FOGTABLEMODE";
	case D3DRS_FOGSTART                      : return "D3DRS_FOGSTART";
	case D3DRS_FOGEND                        : return "D3DRS_FOGEND";
	case D3DRS_FOGDENSITY                    : return "D3DRS_FOGDENSITY";
	case D3DRS_EDGEANTIALIAS                 : return "D3DRS_EDGEANTIALIAS";
	case D3DRS_ZBIAS                         : return "D3DRS_ZBIAS";
	case D3DRS_RANGEFOGENABLE                : return "D3DRS_RANGEFOGENABLE";
	case D3DRS_STENCILENABLE                 : return "D3DRS_STENCILENABLE";
	case D3DRS_STENCILFAIL                   : return "D3DRS_STENCILFAIL";
	case D3DRS_STENCILZFAIL                  : return "D3DRS_STENCILZFAIL";
	case D3DRS_STENCILPASS                   : return "D3DRS_STENCILPASS";
	case D3DRS_STENCILFUNC                   : return "D3DRS_STENCILFUNC";
	case D3DRS_STENCILREF                    : return "D3DRS_STENCILREF";
	case D3DRS_STENCILMASK                   : return "D3DRS_STENCILMASK";
	case D3DRS_STENCILWRITEMASK              : return "D3DRS_STENCILWRITEMASK";
	case D3DRS_TEXTUREFACTOR                 : return "D3DRS_TEXTUREFACTOR";
	case D3DRS_WRAP0                         : return "D3DRS_WRAP0";
	case D3DRS_WRAP1                         : return "D3DRS_WRAP1";
	case D3DRS_WRAP2                         : return "D3DRS_WRAP2";
	case D3DRS_WRAP3                         : return "D3DRS_WRAP3";
	case D3DRS_WRAP4                         : return "D3DRS_WRAP4";
	case D3DRS_WRAP5                         : return "D3DRS_WRAP5";
	case D3DRS_WRAP6                         : return "D3DRS_WRAP6";
	case D3DRS_WRAP7                         : return "D3DRS_WRAP7";
	case D3DRS_CLIPPING                      : return "D3DRS_CLIPPING";
	case D3DRS_LIGHTING                      : return "D3DRS_LIGHTING";
	case D3DRS_AMBIENT                       : return "D3DRS_AMBIENT";
	case D3DRS_FOGVERTEXMODE                 : return "D3DRS_FOGVERTEXMODE";
	case D3DRS_COLORVERTEX                   : return "D3DRS_COLORVERTEX";
	case D3DRS_LOCALVIEWER                   : return "D3DRS_LOCALVIEWER";
	case D3DRS_NORMALIZENORMALS              : return "D3DRS_NORMALIZENORMALS";
	case D3DRS_DIFFUSEMATERIALSOURCE         : return "D3DRS_DIFFUSEMATERIALSOURCE";
	case D3DRS_SPECULARMATERIALSOURCE        : return "D3DRS_SPECULARMATERIALSOURCE";
	case D3DRS_AMBIENTMATERIALSOURCE         : return "D3DRS_AMBIENTMATERIALSOURCE";
	case D3DRS_EMISSIVEMATERIALSOURCE        : return "D3DRS_EMISSIVEMATERIALSOURCE";
	case D3DRS_VERTEXBLEND                   : return "D3DRS_VERTEXBLEND";
	case D3DRS_CLIPPLANEENABLE               : return "D3DRS_CLIPPLANEENABLE";
	case D3DRS_SOFTWAREVERTEXPROCESSING      : return "D3DRS_SOFTWAREVERTEXPROCESSING";
	case D3DRS_POINTSIZE                     : return "D3DRS_POINTSIZE";
	case D3DRS_POINTSIZE_MIN                 : return "D3DRS_POINTSIZE_MIN";
	case D3DRS_POINTSPRITEENABLE             : return "D3DRS_POINTSPRITEENABLE";
	case D3DRS_POINTSCALEENABLE              : return "D3DRS_POINTSCALEENABLE";
	case D3DRS_POINTSCALE_A                  : return "D3DRS_POINTSCALE_A";
	case D3DRS_POINTSCALE_B                  : return "D3DRS_POINTSCALE_B";
	case D3DRS_POINTSCALE_C                  : return "D3DRS_POINTSCALE_C";
	case D3DRS_MULTISAMPLEANTIALIAS          : return "D3DRS_MULTISAMPLEANTIALIAS";
	case D3DRS_MULTISAMPLEMASK               : return "D3DRS_MULTISAMPLEMASK";
	case D3DRS_PATCHEDGESTYLE                : return "D3DRS_PATCHEDGESTYLE";
	case D3DRS_PATCHSEGMENTS                 : return "D3DRS_PATCHSEGMENTS";
	case D3DRS_DEBUGMONITORTOKEN             : return "D3DRS_DEBUGMONITORTOKEN";
	case D3DRS_POINTSIZE_MAX                 : return "D3DRS_POINTSIZE_MAX";
	case D3DRS_INDEXEDVERTEXBLENDENABLE      : return "D3DRS_INDEXEDVERTEXBLENDENABLE";
	case D3DRS_COLORWRITEENABLE              : return "D3DRS_COLORWRITEENABLE";
	case D3DRS_TWEENFACTOR                   : return "D3DRS_TWEENFACTOR";
	case D3DRS_BLENDOP                       : return "D3DRS_BLENDOP";
//	case D3DRS_POSITIONORDER                 : return "D3DRS_POSITIONORDER";
//	case D3DRS_NORMALORDER                   : return "D3DRS_NORMALORDER";
	default											  : return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Texture_Stage_State_Name(D3DTEXTURESTAGESTATETYPE state)
{
	switch (state) {
	case D3DTSS_COLOROP                   : return "D3DTSS_COLOROP";
	case D3DTSS_COLORARG1                 : return "D3DTSS_COLORARG1";
	case D3DTSS_COLORARG2                 : return "D3DTSS_COLORARG2";
	case D3DTSS_ALPHAOP                   : return "D3DTSS_ALPHAOP";
	case D3DTSS_ALPHAARG1                 : return "D3DTSS_ALPHAARG1";
	case D3DTSS_ALPHAARG2                 : return "D3DTSS_ALPHAARG2";
	case D3DTSS_BUMPENVMAT00              : return "D3DTSS_BUMPENVMAT00";
	case D3DTSS_BUMPENVMAT01              : return "D3DTSS_BUMPENVMAT01";
	case D3DTSS_BUMPENVMAT10              : return "D3DTSS_BUMPENVMAT10";
	case D3DTSS_BUMPENVMAT11              : return "D3DTSS_BUMPENVMAT11";
	case D3DTSS_TEXCOORDINDEX             : return "D3DTSS_TEXCOORDINDEX";
	case D3DTSS_ADDRESSU                  : return "D3DTSS_ADDRESSU";
	case D3DTSS_ADDRESSV                  : return "D3DTSS_ADDRESSV";
	case D3DTSS_BORDERCOLOR               : return "D3DTSS_BORDERCOLOR";
	case D3DTSS_MAGFILTER                 : return "D3DTSS_MAGFILTER";
	case D3DTSS_MINFILTER                 : return "D3DTSS_MINFILTER";
	case D3DTSS_MIPFILTER                 : return "D3DTSS_MIPFILTER";
	case D3DTSS_MIPMAPLODBIAS             : return "D3DTSS_MIPMAPLODBIAS";
	case D3DTSS_MAXMIPLEVEL               : return "D3DTSS_MAXMIPLEVEL";
	case D3DTSS_MAXANISOTROPY             : return "D3DTSS_MAXANISOTROPY";
	case D3DTSS_BUMPENVLSCALE             : return "D3DTSS_BUMPENVLSCALE";
	case D3DTSS_BUMPENVLOFFSET            : return "D3DTSS_BUMPENVLOFFSET";
	case D3DTSS_TEXTURETRANSFORMFLAGS     : return "D3DTSS_TEXTURETRANSFORMFLAGS";
	case D3DTSS_ADDRESSW                  : return "D3DTSS_ADDRESSW";
	case D3DTSS_COLORARG0                 : return "D3DTSS_COLORARG0";
	case D3DTSS_ALPHAARG0                 : return "D3DTSS_ALPHAARG0";
	case D3DTSS_RESULTARG                 : return "D3DTSS_RESULTARG";
	default										  : return "UNKNOWN";
	}
}

void DX8Wrapper::Get_DX8_Render_State_Value_Name(StringClass& name, D3DRENDERSTATETYPE state, unsigned value)
{
	switch (state) {
	case D3DRS_ZENABLE:
		name=Get_DX8_ZBuffer_Type_Name(value);
		break;

	case D3DRS_FILLMODE:
		name=Get_DX8_Fill_Mode_Name(value);
		break;

	case D3DRS_SHADEMODE:
		name=Get_DX8_Shade_Mode_Name(value);
		break;

	case D3DRS_LINEPATTERN:
	case D3DRS_FOGCOLOR:
	case D3DRS_ALPHAREF:
	case D3DRS_STENCILMASK:
	case D3DRS_STENCILWRITEMASK:
	case D3DRS_TEXTUREFACTOR:
	case D3DRS_AMBIENT:
	case D3DRS_CLIPPLANEENABLE:
	case D3DRS_MULTISAMPLEMASK:
		name.Format("0x%x",value);
		break;

	case D3DRS_ZWRITEENABLE:
	case D3DRS_ALPHATESTENABLE:
	case D3DRS_LASTPIXEL:
	case D3DRS_DITHERENABLE:
	case D3DRS_ALPHABLENDENABLE:
	case D3DRS_FOGENABLE:
	case D3DRS_SPECULARENABLE:
	case D3DRS_STENCILENABLE:
	case D3DRS_RANGEFOGENABLE:
	case D3DRS_EDGEANTIALIAS:
	case D3DRS_CLIPPING:
	case D3DRS_LIGHTING:
	case D3DRS_COLORVERTEX:
	case D3DRS_LOCALVIEWER:
	case D3DRS_NORMALIZENORMALS:
	case D3DRS_SOFTWAREVERTEXPROCESSING:
	case D3DRS_POINTSPRITEENABLE:
	case D3DRS_POINTSCALEENABLE:
	case D3DRS_MULTISAMPLEANTIALIAS:
	case D3DRS_INDEXEDVERTEXBLENDENABLE:
		name=value ? "TRUE" : "FALSE";
		break;

	case D3DRS_SRCBLEND:
	case D3DRS_DESTBLEND:
		name=Get_DX8_Blend_Name(value);
		break;

	case D3DRS_CULLMODE:
		name=Get_DX8_Cull_Mode_Name(value);
		break;

	case D3DRS_ZFUNC:
	case D3DRS_ALPHAFUNC:
	case D3DRS_STENCILFUNC:
		name=Get_DX8_Cmp_Func_Name(value);
		break;

	case D3DRS_ZVISIBLE:
		name="NOTSUPPORTED";
		break;

	case D3DRS_FOGTABLEMODE:
	case D3DRS_FOGVERTEXMODE:
		name=Get_DX8_Fog_Mode_Name(value);
		break;

	case D3DRS_FOGSTART:
	case D3DRS_FOGEND:
	case D3DRS_FOGDENSITY:
	case D3DRS_POINTSIZE:
	case D3DRS_POINTSIZE_MIN:
	case D3DRS_POINTSCALE_A:
	case D3DRS_POINTSCALE_B:
	case D3DRS_POINTSCALE_C:
	case D3DRS_PATCHSEGMENTS:
	case D3DRS_POINTSIZE_MAX:
	case D3DRS_TWEENFACTOR:
		name.Format("%f",std::bit_cast<float>(value));
		break;

	case D3DRS_ZBIAS:
	case D3DRS_STENCILREF:
		name.Format("%d",value);
		break;

	case D3DRS_STENCILFAIL:
	case D3DRS_STENCILZFAIL:
	case D3DRS_STENCILPASS:
		name=Get_DX8_Stencil_Op_Name(value);
		break;

	case D3DRS_WRAP0:
	case D3DRS_WRAP1:
	case D3DRS_WRAP2:
	case D3DRS_WRAP3:
	case D3DRS_WRAP4:
	case D3DRS_WRAP5:
	case D3DRS_WRAP6:
	case D3DRS_WRAP7:
		name="0";
		if (value&D3DWRAP_U) name+="|D3DWRAP_U";
		if (value&D3DWRAP_V) name+="|D3DWRAP_V";
		if (value&D3DWRAP_W) name+="|D3DWRAP_W";
		break;

	case D3DRS_DIFFUSEMATERIALSOURCE:
	case D3DRS_SPECULARMATERIALSOURCE:
	case D3DRS_AMBIENTMATERIALSOURCE:
	case D3DRS_EMISSIVEMATERIALSOURCE:
		name=Get_DX8_Material_Source_Name(value);
		break;

	case D3DRS_VERTEXBLEND:
		name=Get_DX8_Vertex_Blend_Flag_Name(value);
		break;

	case D3DRS_PATCHEDGESTYLE:
		name=Get_DX8_Patch_Edge_Style_Name(value);
		break;

	case D3DRS_DEBUGMONITORTOKEN:
		name=Get_DX8_Debug_Monitor_Token_Name(value);
		break;

	case D3DRS_COLORWRITEENABLE:
		name="0";
		if (value&D3DCOLORWRITEENABLE_RED) name+="|D3DCOLORWRITEENABLE_RED";
		if (value&D3DCOLORWRITEENABLE_GREEN) name+="|D3DCOLORWRITEENABLE_GREEN";
		if (value&D3DCOLORWRITEENABLE_BLUE) name+="|D3DCOLORWRITEENABLE_BLUE";
		if (value&D3DCOLORWRITEENABLE_ALPHA) name+="|D3DCOLORWRITEENABLE_ALPHA";
		break;
	case D3DRS_BLENDOP:
		name=Get_DX8_Blend_Op_Name(value);
		break;
	default:
		name.Format("UNKNOWN (%d)",value);
		break;
	}
}

void DX8Wrapper::Get_DX8_Texture_Stage_State_Value_Name(StringClass& name, D3DTEXTURESTAGESTATETYPE state, unsigned value)
{
	switch (state) {
	case D3DTSS_COLOROP:
	case D3DTSS_ALPHAOP:
		name=Get_DX8_Texture_Op_Name(value);
		break;

	case D3DTSS_COLORARG0:
	case D3DTSS_COLORARG1:
	case D3DTSS_COLORARG2:
	case D3DTSS_ALPHAARG0:
	case D3DTSS_ALPHAARG1:
	case D3DTSS_ALPHAARG2:
	case D3DTSS_RESULTARG:
		name=Get_DX8_Texture_Arg_Name(value);
		break;

	case D3DTSS_ADDRESSU:
	case D3DTSS_ADDRESSV:
	case D3DTSS_ADDRESSW:
		name=Get_DX8_Texture_Address_Name(value);
		break;

	case D3DTSS_MAGFILTER:
	case D3DTSS_MINFILTER:
	case D3DTSS_MIPFILTER:
		name=Get_DX8_Texture_Filter_Name(value);
		break;

	case D3DTSS_TEXTURETRANSFORMFLAGS:
		name=Get_DX8_Texture_Transform_Flag_Name(value);
		break;

	// Floating point values
	case D3DTSS_MIPMAPLODBIAS:
	case D3DTSS_BUMPENVMAT00:
	case D3DTSS_BUMPENVMAT01:
	case D3DTSS_BUMPENVMAT10:
	case D3DTSS_BUMPENVMAT11:
	case D3DTSS_BUMPENVLSCALE:
	case D3DTSS_BUMPENVLOFFSET:
		name.Format("%f",std::bit_cast<float>(value));
		break;

	case D3DTSS_TEXCOORDINDEX:
		if ((value&0xffff0000)==D3DTSS_TCI_CAMERASPACENORMAL) {
			name.Format("D3DTSS_TCI_CAMERASPACENORMAL|%d",value&0xffff);
		}
		else if ((value&0xffff0000)==D3DTSS_TCI_CAMERASPACEPOSITION) {
			name.Format("D3DTSS_TCI_CAMERASPACEPOSITION|%d",value&0xffff);
		}
		else if ((value&0xffff0000)==D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR) {
			name.Format("D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR|%d",value&0xffff);
		}
		else {
			name.Format("%d",value);
		}
		break;

	// Integer value
	case D3DTSS_MAXMIPLEVEL:
	case D3DTSS_MAXANISOTROPY:
		name.Format("%d",value);
		break;
	// Hex values
	case D3DTSS_BORDERCOLOR:
		name.Format("0x%x",value);
		break;

	default:
		name.Format("UNKNOWN (%d)",value);
		break;
	}
}

const char* DX8Wrapper::Get_DX8_Texture_Op_Name(unsigned value)
{
	switch (value) {
	case D3DTOP_DISABLE                      : return "D3DTOP_DISABLE";
	case D3DTOP_SELECTARG1                   : return "D3DTOP_SELECTARG1";
	case D3DTOP_SELECTARG2                   : return "D3DTOP_SELECTARG2";
	case D3DTOP_MODULATE                     : return "D3DTOP_MODULATE";
	case D3DTOP_MODULATE2X                   : return "D3DTOP_MODULATE2X";
	case D3DTOP_MODULATE4X                   : return "D3DTOP_MODULATE4X";
	case D3DTOP_ADD                          : return "D3DTOP_ADD";
	case D3DTOP_ADDSIGNED                    : return "D3DTOP_ADDSIGNED";
	case D3DTOP_ADDSIGNED2X                  : return "D3DTOP_ADDSIGNED2X";
	case D3DTOP_SUBTRACT                     : return "D3DTOP_SUBTRACT";
	case D3DTOP_ADDSMOOTH                    : return "D3DTOP_ADDSMOOTH";
	case D3DTOP_BLENDDIFFUSEALPHA            : return "D3DTOP_BLENDDIFFUSEALPHA";
	case D3DTOP_BLENDTEXTUREALPHA            : return "D3DTOP_BLENDTEXTUREALPHA";
	case D3DTOP_BLENDFACTORALPHA             : return "D3DTOP_BLENDFACTORALPHA";
	case D3DTOP_BLENDTEXTUREALPHAPM          : return "D3DTOP_BLENDTEXTUREALPHAPM";
	case D3DTOP_BLENDCURRENTALPHA            : return "D3DTOP_BLENDCURRENTALPHA";
	case D3DTOP_PREMODULATE                  : return "D3DTOP_PREMODULATE";
	case D3DTOP_MODULATEALPHA_ADDCOLOR       : return "D3DTOP_MODULATEALPHA_ADDCOLOR";
	case D3DTOP_MODULATECOLOR_ADDALPHA       : return "D3DTOP_MODULATECOLOR_ADDALPHA";
	case D3DTOP_MODULATEINVALPHA_ADDCOLOR    : return "D3DTOP_MODULATEINVALPHA_ADDCOLOR";
	case D3DTOP_MODULATEINVCOLOR_ADDALPHA    : return "D3DTOP_MODULATEINVCOLOR_ADDALPHA";
	case D3DTOP_BUMPENVMAP                   : return "D3DTOP_BUMPENVMAP";
	case D3DTOP_BUMPENVMAPLUMINANCE          : return "D3DTOP_BUMPENVMAPLUMINANCE";
	case D3DTOP_DOTPRODUCT3                  : return "D3DTOP_DOTPRODUCT3";
	case D3DTOP_MULTIPLYADD                  : return "D3DTOP_MULTIPLYADD";
	case D3DTOP_LERP                         : return "D3DTOP_LERP";
	default										     : return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Texture_Arg_Name(unsigned value)
{
	switch (value) {
	case D3DTA_CURRENT			: return "D3DTA_CURRENT";
	case D3DTA_DIFFUSE			: return "D3DTA_DIFFUSE";
	case D3DTA_SELECTMASK		: return "D3DTA_SELECTMASK";
	case D3DTA_SPECULAR			: return "D3DTA_SPECULAR";
	case D3DTA_TEMP				: return "D3DTA_TEMP";
	case D3DTA_TEXTURE			: return "D3DTA_TEXTURE";
	case D3DTA_TFACTOR			: return "D3DTA_TFACTOR";
	case D3DTA_ALPHAREPLICATE	: return "D3DTA_ALPHAREPLICATE";
	case D3DTA_COMPLEMENT		: return "D3DTA_COMPLEMENT";
	default					      : return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Texture_Filter_Name(unsigned value)
{
	switch (value) {
	case D3DTEXF_NONE				: return "D3DTEXF_NONE";
	case D3DTEXF_POINT			: return "D3DTEXF_POINT";
	case D3DTEXF_LINEAR			: return "D3DTEXF_LINEAR";
	case D3DTEXF_ANISOTROPIC	: return "D3DTEXF_ANISOTROPIC";
	case D3DTEXF_FLATCUBIC		: return "D3DTEXF_FLATCUBIC";
	case D3DTEXF_GAUSSIANCUBIC	: return "D3DTEXF_GAUSSIANCUBIC";
	default					      : return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Texture_Address_Name(unsigned value)
{
	switch (value) {
	case D3DTADDRESS_WRAP		: return "D3DTADDRESS_WRAP";
	case D3DTADDRESS_MIRROR		: return "D3DTADDRESS_MIRROR";
	case D3DTADDRESS_CLAMP		: return "D3DTADDRESS_CLAMP";
	case D3DTADDRESS_BORDER		: return "D3DTADDRESS_BORDER";
	case D3DTADDRESS_MIRRORONCE: return "D3DTADDRESS_MIRRORONCE";
	default					      : return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Texture_Transform_Flag_Name(unsigned value)
{
	switch (value) {
	case D3DTTFF_DISABLE			: return "D3DTTFF_DISABLE";
	case D3DTTFF_COUNT1			: return "D3DTTFF_COUNT1";
	case D3DTTFF_COUNT2			: return "D3DTTFF_COUNT2";
	case D3DTTFF_COUNT3			: return "D3DTTFF_COUNT3";
	case D3DTTFF_COUNT4			: return "D3DTTFF_COUNT4";
	case D3DTTFF_PROJECTED		: return "D3DTTFF_PROJECTED";
	default					      : return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_ZBuffer_Type_Name(unsigned value)
{
	switch (value) {
	case D3DZB_FALSE				: return "D3DZB_FALSE";
	case D3DZB_TRUE				: return "D3DZB_TRUE";
	case D3DZB_USEW				: return "D3DZB_USEW";
	default					      : return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Fill_Mode_Name(unsigned value)
{
	switch (value) {
	case D3DFILL_POINT			: return "D3DFILL_POINT";
	case D3DFILL_WIREFRAME		: return "D3DFILL_WIREFRAME";
	case D3DFILL_SOLID			: return "D3DFILL_SOLID";
	default					      : return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Shade_Mode_Name(unsigned value)
{
	switch (value) {
	case D3DSHADE_FLAT			: return "D3DSHADE_FLAT";
	case D3DSHADE_GOURAUD		: return "D3DSHADE_GOURAUD";
	case D3DSHADE_PHONG			: return "D3DSHADE_PHONG";
	default							: return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Blend_Name(unsigned value)
{
	switch (value) {
	case D3DBLEND_ZERO                : return "D3DBLEND_ZERO";
	case D3DBLEND_ONE                 : return "D3DBLEND_ONE";
	case D3DBLEND_SRCCOLOR            : return "D3DBLEND_SRCCOLOR";
	case D3DBLEND_INVSRCCOLOR         : return "D3DBLEND_INVSRCCOLOR";
	case D3DBLEND_SRCALPHA            : return "D3DBLEND_SRCALPHA";
	case D3DBLEND_INVSRCALPHA         : return "D3DBLEND_INVSRCALPHA";
	case D3DBLEND_DESTALPHA           : return "D3DBLEND_DESTALPHA";
	case D3DBLEND_INVDESTALPHA        : return "D3DBLEND_INVDESTALPHA";
	case D3DBLEND_DESTCOLOR           : return "D3DBLEND_DESTCOLOR";
	case D3DBLEND_INVDESTCOLOR        : return "D3DBLEND_INVDESTCOLOR";
	case D3DBLEND_SRCALPHASAT         : return "D3DBLEND_SRCALPHASAT";
	case D3DBLEND_BOTHSRCALPHA        : return "D3DBLEND_BOTHSRCALPHA";
	case D3DBLEND_BOTHINVSRCALPHA     : return "D3DBLEND_BOTHINVSRCALPHA";
	default									 : return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Cull_Mode_Name(unsigned value)
{
	switch (value) {
	case D3DCULL_NONE				: return "D3DCULL_NONE";
	case D3DCULL_CW				: return "D3DCULL_CW";
	case D3DCULL_CCW				: return "D3DCULL_CCW";
	default							: return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Cmp_Func_Name(unsigned value)
{
	switch (value) {
	case D3DCMP_NEVER          : return "D3DCMP_NEVER";
	case D3DCMP_LESS           : return "D3DCMP_LESS";
	case D3DCMP_EQUAL          : return "D3DCMP_EQUAL";
	case D3DCMP_LESSEQUAL      : return "D3DCMP_LESSEQUAL";
	case D3DCMP_GREATER        : return "D3DCMP_GREATER";
	case D3DCMP_NOTEQUAL       : return "D3DCMP_NOTEQUAL";
	case D3DCMP_GREATEREQUAL   : return "D3DCMP_GREATEREQUAL";
	case D3DCMP_ALWAYS         : return "D3DCMP_ALWAYS";
	default							: return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Fog_Mode_Name(unsigned value)
{
	switch (value) {
	case D3DFOG_NONE				: return "D3DFOG_NONE";
	case D3DFOG_EXP				: return "D3DFOG_EXP";
	case D3DFOG_EXP2				: return "D3DFOG_EXP2";
	case D3DFOG_LINEAR			: return "D3DFOG_LINEAR";
	default							: return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Stencil_Op_Name(unsigned value)
{
	switch (value) {
	case D3DSTENCILOP_KEEP		: return "D3DSTENCILOP_KEEP";
	case D3DSTENCILOP_ZERO		: return "D3DSTENCILOP_ZERO";
	case D3DSTENCILOP_REPLACE	: return "D3DSTENCILOP_REPLACE";
	case D3DSTENCILOP_INCRSAT	: return "D3DSTENCILOP_INCRSAT";
	case D3DSTENCILOP_DECRSAT	: return "D3DSTENCILOP_DECRSAT";
	case D3DSTENCILOP_INVERT	: return "D3DSTENCILOP_INVERT";
	case D3DSTENCILOP_INCR		: return "D3DSTENCILOP_INCR";
	case D3DSTENCILOP_DECR		: return "D3DSTENCILOP_DECR";
	default							: return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Material_Source_Name(unsigned value)
{
	switch (value) {
	case D3DMCS_MATERIAL			: return "D3DMCS_MATERIAL";
	case D3DMCS_COLOR1			: return "D3DMCS_COLOR1";
	case D3DMCS_COLOR2			: return "D3DMCS_COLOR2";
	default							: return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Vertex_Blend_Flag_Name(unsigned value)
{
	switch (value) {
	case D3DVBF_DISABLE			: return "D3DVBF_DISABLE";
	case D3DVBF_1WEIGHTS			: return "D3DVBF_1WEIGHTS";
	case D3DVBF_2WEIGHTS			: return "D3DVBF_2WEIGHTS";
	case D3DVBF_3WEIGHTS			: return "D3DVBF_3WEIGHTS";
	case D3DVBF_TWEENING			: return "D3DVBF_TWEENING";
	case D3DVBF_0WEIGHTS			: return "D3DVBF_0WEIGHTS";
	default							: return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Patch_Edge_Style_Name(unsigned value)
{
	switch (value) {
	case D3DPATCHEDGE_DISCRETE	: return "D3DPATCHEDGE_DISCRETE";
   case D3DPATCHEDGE_CONTINUOUS:return "D3DPATCHEDGE_CONTINUOUS";
	default							: return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Debug_Monitor_Token_Name(unsigned value)
{
	switch (value) {
	case D3DDMT_ENABLE			: return "D3DDMT_ENABLE";
	case D3DDMT_DISABLE			: return "D3DDMT_DISABLE";
	default							: return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Blend_Op_Name(unsigned value)
{
	switch (value) {
	case D3DBLENDOP_ADD			: return "D3DBLENDOP_ADD";
	case D3DBLENDOP_SUBTRACT	: return "D3DBLENDOP_SUBTRACT";
	case D3DBLENDOP_REVSUBTRACT: return "D3DBLENDOP_REVSUBTRACT";
	case D3DBLENDOP_MIN			: return "D3DBLENDOP_MIN";
	case D3DBLENDOP_MAX			: return "D3DBLENDOP_MAX";
	default							: return "UNKNOWN";
	}
}


//============================================================================
// DX8Wrapper::getBackBufferFormat
//============================================================================

WW3DFormat	DX8Wrapper::getBackBufferFormat()
{
	return D3DFormat_To_WW3DFormat( _PresentParameters.BackBufferFormat );
}

// GeneralsX: TheSuperHackers' Renderer seam (Renderer.h) is defined by the DX8 backend, built
// here rather than listed in CMakeLists.txt: that recipe is digested whole (simsourcedigest_watcher.cmake), and a new
// source line there would move the SimID of a change that touches no simulation code.
#include "Backend/DX8Backend.cpp"
