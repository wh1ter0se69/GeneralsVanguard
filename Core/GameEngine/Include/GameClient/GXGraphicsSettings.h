// GeneralsX @feature Claude 28/09/2026 THE CLASSIC/REMASTERED SWITCH (graphics P3-3). Its home and its two controls,
// with nothing behind it yet: the render path reads nothing here until the post chain (P3-4) registers the hook.
//
// The value lives in <user data>/GXGraphics.ini as `GXRemastered = 1|0` - its own file, not Options.ini, because the
// Options, Extras and Debug screens each hold a whole Options.ini map while open and their next write would revert a
// flip made meanwhile. Read and written through the plain UserPreferences map, never a typed OptionPreferences accessor,
// a GlobalData field or GameLOD: those are inside the sourceID digest, and GlobalData is written back to Options.ini by
// the Options Accept. Not in dataID (fopen, never INI::load). This file and GXGraphicsSettings.cpp are GameClient code,
// outside the digest, so the switch ships per platform.
//
// Controls: the Options menu's retail Send Delay checkbox, relabelled "Remastered" and applied on Accept
// (OptionsMenu.cpp), and a chord caught in front of the meta map, SHIFT_CTRL+KEY_R unless CommandButtonHotKeys.ini's
// GXRemasteredKey names another (MetaEvent.cpp). Neither creates a GameMessage, so nothing reaches the network or a
// replay. The value is RENDER-ONLY and must stay so: a headless peer builds a real W3DDisplay, so nothing the
// simulation reads may ever depend on it.

#pragma once

#include "Common/GameDefines.h"

// GeneralsX @feature Claude 28/09/2026 THE ONE SWITCH for everything a player can see or press (lead, 28/09): the Options
// checkbox and the chord. 0 until P3-4 draws a real REMASTERED look; P3-4 sets it to 1. While it is 0 the Options menu
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

	/// P3-4's post chain registers here to hear a change; one listener, nullptr to clear.
	typedef void (*RemasteredChangedFn)(Bool on);
	void setOnRemasteredChanged(RemasteredChangedFn fn);

	// GeneralsX @feature D · render-hud-seam-1 13/10/2026 THE UPSCALER KEYS (modernize/render-dlss/DLSS_INTEGRATION.md "Settings
	// keys", agreed with dlss-streamline-1 + fsr-metalfx-1). Same file, read once with the rest; each key has a TEST-ONLY env twin
	// (GX_UPSCALER, GX_UPSCALE_QUALITY, GX_RENDER_SCALE, GX_UPSCALE_SHARPNESS, GX_FRAMEGEN) that wins over the file. Render-only, like
	// GXRemastered: nothing the simulation reads may depend on them. Upscaler values are GxUpscaleKind + 1 (GxWorldPass.h).
	enum Upscaler { UPSCALER_OFF = 0, UPSCALER_BILINEAR, UPSCALER_FSR1, UPSCALER_FSR3, UPSCALER_DLSS, UPSCALER_METALFX,
		UPSCALER_METALFX_TEMPORAL, UPSCALER_XESS, UPSCALER_COUNT };
	Int upscaler();						///< GXUpscaler; OFF = the world draws at 100% with no seam (today's device calls)
	const char *upscalerName(Int u);	///< the key's own spelling ("off", "fsr1", ...)
	Int renderScalePercent();			///< world size per axis, 33-100: GXUpscaleQuality's ratio, or GXRenderScale when custom
	Int upscaleSharpness();				///< GXUpscaleSharpness 0-100
	Int frameGen();						///< GXFrameGen 0-4
	// GeneralsX @feature B · dlss-fg-1 14/10/2026 GXReflex (env twin GX_REFLEX, TEST-ONLY): NVIDIA Reflex 0 off / 1 on / 2 on + boost,
	// default 0 (DLSS_INTEGRATION.md keys table). Frame generation needs Reflex on: GXFrameGen > 0 runs it at 1 at least.
	Int reflex();						///< GXReflex 0-2
	// GeneralsX @feature B · mac-upscaler-u2 18/10/2026 (U2, DESIGN_mac-upscaler-u2.md): upscaler() returns the RESOLVED value - a
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
	Bool upscalerIsTemporal(Int u);	///< fsr3, dlss, metalfx-temporal, xess: 1x world targets, sampleable depth (render-motion-1)
	// GeneralsX @feature D · look-shadowmaps 17/10/2026 S7 GPU sun shadow map (modernize/look-director/d2/PREREG_look-shadowmaps.md
	// A18): GXShadowMap = off | on (default OFF), env twin GX_SHADOWMAP (TEST-ONLY), read ONCE with the rest: the native device
	// picks its look feature mask at creation (a change needs a restart). Classic ignores it (one log line). GX_SHADOWMAP_STEP
	// (TEST-ONLY) pins the runtime step 0..3; -1 = not pinned (the governor's, or 0 until the governor lands on this base).
	Bool shadowMap();
	Int shadowMapStepPin();
	// GeneralsX @feature B · a2c-1 20/10/2026 alpha-to-coverage under the existing MSAA (modernize/render-dlss/SCOPE_a2c.md, PREREG A108):
	// GXAlphaToCoverage = yes | no (default YES, wh1ter0se 21/10/2026), env twin GX_A2C (TEST-ONLY), read ONCE with the rest. The native device acts on it only
	// when the session is multisampled at creation (MSAA 2x and above); classic ignores it.
	Bool alphaToCoverage();
	enum LookFeature { LOOK_SHADOWMAP = 1, LOOK_A2C = 2 };
	unsigned int lookFeatureMask();	///< the native device's look mask (LOOK_SHADOWMAP = shadowMap(), LOOK_A2C = alphaToCoverage()); plain unsigned: gx_upscaler_resolve_test
	                               	///< compiles this header against a stub GameDefines.h with Int / Bool only
	// GeneralsX @feature D · render-line-0967 21/10/2026 THE RENDERER SWITCH (GXRendererKey.h): Options.ini GXRenderer = classic | native
	// (default classic), env twin GX_RENDERER (TEST-ONLY) wins. NOT in GXGraphics.ini: the file wh1ter0se's graphics keys live in, and the
	// in-game screens only read it here (no menu writes it). Resolved ONCE per process and logged once ([GXRENDERER]); the first
	// reader is SDL3Main before SDL loads Vulkan. The ASK: the session can still go classic at device creation.
	Bool nativeRendererRequested();
}
