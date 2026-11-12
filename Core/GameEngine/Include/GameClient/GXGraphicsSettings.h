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

// THE CLASSIC/REMASTERED SWITCH: its home and two controls (the Options checkbox and the SHIFT_CTRL+KEY_R chord).
// `GXRemastered = 1|0` is in <user data>/GXGraphics.ini, its own file: the Options, Extras and Debug screens
// hold a whole Options.ini map while open and would revert a flip. Read via the plain UserPreferences map, never
// a typed accessor or GlobalData field (sourceID digest). RENDER-ONLY: the simulation must not read it.

#pragma once

#include "Common/GameDefines.h"

// THE ONE SWITCH for everything a player can see or press: the Options
// checkbox and the chord. 0 until a real REMASTERED look is drawn, then 1. While it is 0 the Options menu
// keeps the retail box hidden, the chord is neither read nor caught (the key does exactly what it did before), and the
// engine binary names no GXRemasteredKey, so the launcher does not show that row. GXGraphics.ini and the hook stay live.
#define GX_REMASTERED_UI 0

namespace GXGraphics
{
	/// The session's value: GXGraphics.ini on first call, the code default when the file does not say.
	Bool isRemastered();

	/// Sets the session's value, persists it (load, set, write - never a stale map) and calls the hook. A no-op when
	/// the value does not change, so an Options Accept that leaves the box alone writes no file.
	void setRemastered(Bool on);

	/// What a player who never chose gets, and what the Options menu's Defaults button selects.
	Bool platformDefault();

	/// The post chain registers here to hear a change; one listener, nullptr to clear.
	typedef void (*RemasteredChangedFn)(Bool on);
	void setOnRemasteredChanged(RemasteredChangedFn fn);

	// THE UPSCALER KEYS. Same file, read once with the rest; each key has a TEST-ONLY env twin
	// (GX_UPSCALER, GX_UPSCALE_QUALITY, GX_RENDER_SCALE, GX_UPSCALE_SHARPNESS, GX_FRAMEGEN) that wins over the file. Render-only, like
	// GXRemastered: nothing the simulation reads may depend on them. Upscaler values are GxUpscaleKind + 1 (GxWorldPass.h).
	enum Upscaler { UPSCALER_OFF = 0, UPSCALER_BILINEAR, UPSCALER_FSR1, UPSCALER_FSR3, UPSCALER_DLSS, UPSCALER_METALFX,
		UPSCALER_METALFX_TEMPORAL, UPSCALER_XESS, UPSCALER_COUNT };
	Int upscaler();						///< GXUpscaler; OFF = the world draws at 100% with no seam (today's device calls)
	const char *upscalerName(Int u);	///< the key's own spelling ("off", "fsr1", ...)
	Int renderScalePercent();			///< world size per axis, 33-100: GXUpscaleQuality's ratio, or GXRenderScale when custom
	Int upscaleSharpness();				///< GXUpscaleSharpness 0-100
	Int frameGen();						///< GXFrameGen 0-4
	// GXReflex (env twin GX_REFLEX, TEST-ONLY): NVIDIA Reflex 0 off / 1 on / 2 on + boost,
	// default 0 (keys table). Frame generation needs Reflex on: GXFrameGen > 0 runs it at 1 at least.
	Int reflex();						///< GXReflex 0-2
	// upscaler() returns the RESOLVED value - a
	// backend this build really has, else OFF for the whole chain (scale 100%, msaa as configured, no motion pass). Real backends:
	// bilinear and FSR 1 everywhere; DLSS only in a GX_STREAMLINE build. fsr3, xess and both MetalFX kinds have no device backend yet.
	// Pure and inline so the test exe needs nothing else; `reason` (may be null) is set to why a request was changed.
	inline Int resolveUpscaler(Int requested, Bool streamlineBuild, const char **reason)
	{
		const char *none = nullptr;
		if (reason == nullptr) reason = &none;
		*reason = "";
		if (requested <= UPSCALER_OFF || requested >= UPSCALER_COUNT)
			return UPSCALER_OFF;
		if (requested == UPSCALER_BILINEAR || requested == UPSCALER_FSR1)
			return requested;
		if (requested == UPSCALER_DLSS && streamlineBuild)
			return requested;
		*reason = requested == UPSCALER_DLSS ? "no Streamline in this build" : "no device backend for this kind in this build";
		return UPSCALER_OFF;
	}
	Int requestedUpscaler();			///< the key as written (before resolveUpscaler), for logs
	void demoteUpscaler(const char *why);	///< a run-time backend failure: resolved -> OFF at scale 100, logged once; the next frame draws native
	Bool upscalerIsTemporal(Int u);	///< fsr3, dlss, metalfx-temporal, xess: 1x world targets, sampleable depth
	// GPU sun shadow map: GXShadowMap = off | on (default OFF), env twin GX_SHADOWMAP (TEST-ONLY), read ONCE with the rest: the native device
	// picks its look feature mask at creation (a change needs a restart). Classic ignores it (one log line). GX_SHADOWMAP_STEP
	// (TEST-ONLY) pins the runtime step 0..3; -1 = not pinned (the governor's, or 0 until the governor lands on this base).
	Bool shadowMap();
	Int shadowMapStepPin();
	// alpha-to-coverage under the existing MSAA:
	// GXAlphaToCoverage = yes | no (default YES), env twin GX_A2C (TEST-ONLY), read ONCE with the rest. The native device acts on it only
	// when the session is multisampled at creation (MSAA 2x and above); classic ignores it.
	Bool alphaToCoverage();
	enum LookFeature { LOOK_SHADOWMAP = 1, LOOK_A2C = 2 };
	unsigned int lookFeatureMask();	///< the native device's look mask (LOOK_SHADOWMAP = shadowMap(), LOOK_A2C = alphaToCoverage()); plain unsigned: gx_upscaler_resolve_test
	                               	///< compiles this header against a stub GameDefines.h with Int / Bool only
	// THE RENDERER SWITCH (GXRendererKey.h): Options.ini GXRenderer = classic | native
	// (default classic), env twin GX_RENDERER (TEST-ONLY) wins. NOT in GXGraphics.ini: the file the graphics keys live in, and the
	// in-game screens only read it here (no menu writes it). Resolved ONCE per process and logged once ([GXRENDERER]); the first
	// reader is SDL3Main before SDL loads Vulkan. The ASK: the session can still go classic at device creation.
	Bool nativeRendererRequested();
}
