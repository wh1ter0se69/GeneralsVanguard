// GeneralsX @feature Claude 28/09/2026 THE CLASSIC/REMASTERED SWITCH (graphics P3-3). See GXGraphicsSettings.h.

#include "PreRTS.h"

#include "GameClient/GXGraphicsSettings.h"

#include "Common/GlobalData.h"
#include "Common/OptionPreferences.h"
#include "Common/UserPreferences.h"
#include "GameClient/ClientInstance.h"
#include "GameClient/GXRendererKey.h"

#include <stdio.h>
#include <stdlib.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>	// TARGET_OS_IPHONE exists only after this, and only inside __APPLE__
#endif

namespace
{
	Bool s_loaded = FALSE;
	Bool s_remastered = FALSE;
	GXGraphics::RemasteredChangedFn s_onChanged = nullptr;
	Int s_upscaler = GXGraphics::UPSCALER_OFF;
	Int s_upscalerAsked = GXGraphics::UPSCALER_OFF;
	Int s_scalePercent = 100;
	Int s_sharpness = 20;
	Int s_frameGen = 0;
	Int s_reflex = 0;
	Bool s_shadowMap = FALSE;          // S7 GXShadowMap
	Int s_shadowMapStepPin = -1;      // S7 GX_SHADOWMAP_STEP (TEST-ONLY)
	Bool s_alphaToCoverage = FALSE;   // B · a2c-1 GXAlphaToCoverage

	const char *const s_upscalerNames[GXGraphics::UPSCALER_COUNT] =
		{ "off", "bilinear", "fsr1", "fsr3", "dlss", "metalfx", "metalfx-temporal", "xess" };

	/// The file's value, unless the env twin is set (TEST-ONLY: harness arms). `src` says which won, for the log.
	AsciiString keyValue(const UserPreferences &prefs, const char *key, const char *env, const char *def, const char **src)
	{
		const char *e = getenv(env);
		if (e != nullptr && *e != '\0')
		{
			*src = "env";
			return AsciiString(e);
		}
		*src = prefs.find(key) != prefs.end() ? "file" : "default";
		return prefs.getAsciiString(key, def);
	}

	Int clampInt(const AsciiString &v, Int lo, Int hi, Int def)
	{
		if (v.isEmpty())
			return def;
		char *end = nullptr;
		const long n = strtol(v.str(), &end, 10);
		if (end == v.str() || *end != '\0')
			return def;
		return n < lo ? lo : (n > hi ? hi : (Int)n);
	}

	void loadUpscaler(const UserPreferences &prefs)
	{
		const char *srcU, *srcQ, *srcS, *srcH, *srcF;
		const AsciiString u = keyValue(prefs, "GXUpscaler", "GX_UPSCALER", "off", &srcU);
		s_upscaler = GXGraphics::UPSCALER_OFF;
		Bool known = FALSE;
		for (Int i = 0; i < GXGraphics::UPSCALER_COUNT; ++i)
			if (u.compareNoCase(s_upscalerNames[i]) == 0) { s_upscaler = i; known = TRUE; }

		// GXUpscaleQuality's standard per-axis ratios (FSR / DLSS): native 1.0, quality 1/1.5, balanced 1/1.7, performance 1/2.0,
		// ultra 1/3.0; custom = GXRenderScale. A temporal backend may map the preset to its own optimal size instead.
		const AsciiString q = keyValue(prefs, "GXUpscaleQuality", "GX_UPSCALE_QUALITY", "custom", &srcQ);
		const AsciiString sc = keyValue(prefs, "GXRenderScale", "GX_RENDER_SCALE", "67", &srcS);
		if (q.compareNoCase("native") == 0) s_scalePercent = 100;
		else if (q.compareNoCase("quality") == 0) s_scalePercent = 67;
		else if (q.compareNoCase("balanced") == 0) s_scalePercent = 59;
		else if (q.compareNoCase("performance") == 0) s_scalePercent = 50;
		else if (q.compareNoCase("ultra") == 0) s_scalePercent = 33;
		else s_scalePercent = clampInt(sc, 33, 100, 67);

		// U2: resolve ONCE, before anything is sized - every reader of upscaler() / renderScalePercent() gets the resolved world
#if defined(GX_STREAMLINE)
		const Bool streamlineBuild = TRUE;
#else
		const Bool streamlineBuild = FALSE;
#endif
		s_upscalerAsked = s_upscaler;
		const char *why = "";
		s_upscaler = GXGraphics::resolveUpscaler(s_upscalerAsked, streamlineBuild, &why);
		if (s_upscaler == GXGraphics::UPSCALER_OFF)
			s_scalePercent = 100;
		fprintf(stderr, "[GXUPSCALE] resolve requested=%s resolved=%s reason=%s\n", s_upscalerNames[s_upscalerAsked],
			s_upscalerNames[s_upscaler], *why != '\0' ? why : (s_upscalerAsked == s_upscaler ? "supported" : "off"));

		s_sharpness = clampInt(keyValue(prefs, "GXUpscaleSharpness", "GX_UPSCALE_SHARPNESS", "20", &srcH), 0, 100, 20);
		s_frameGen = clampInt(keyValue(prefs, "GXFrameGen", "GX_FRAMEGEN", "0", &srcF), 0, 4, 0);
		fprintf(stderr, "[GXUPSCALE] upscaler=%s (%s%s) quality=%s (%s) scale=%d%% (%s) sharpness=%d (%s) framegen=%d (%s)\n",
			s_upscalerNames[s_upscaler], srcU, known ? "" : ", unknown value - off", q.str(), srcQ, (int)s_scalePercent, srcS,
			(int)s_sharpness, srcH, (int)s_frameGen, srcF);
		const char *srcR = "default";
		s_reflex = clampInt(keyValue(prefs, "GXReflex", "GX_REFLEX", "0", &srcR), 0, 2, 0);
		fprintf(stderr, "[GXREFLEX] reflex=%d (%s)\n", (int)s_reflex, srcR);
	}

	// S7 (D · look-shadowmaps, PREREG A18): GXShadowMap off|on + the TEST-ONLY step pin
	void loadShadowMap(const UserPreferences &prefs)
	{
		const char *src = "default";
		const AsciiString v = keyValue(prefs, "GXShadowMap", "GX_SHADOWMAP", "off", &src);
		s_shadowMap = v.compareNoCase("on") == 0 || v.compareNoCase("1") == 0 || v.compareNoCase("yes") == 0;
		const Bool known = s_shadowMap || v.compareNoCase("off") == 0 || v.compareNoCase("0") == 0 || v.compareNoCase("no") == 0;
		const char *pin = getenv("GX_SHADOWMAP_STEP");
		s_shadowMapStepPin = pin != nullptr ? clampInt(AsciiString(pin), 0, 3, -1) : -1;
		fprintf(stderr, "[GXSHADOWMAP] key=%s (%s%s) step_pin=%d\n", s_shadowMap ? "on" : "off", src, known ? "" : ", unknown value - off",
			(int)s_shadowMapStepPin);
	}

	// B · a2c-1 (PREREG A108): GXAlphaToCoverage yes|no - the spellings GXShadowMap takes
	void loadAlphaToCoverage(const UserPreferences &prefs)
	{
		const char *src = "default";
		const AsciiString v = keyValue(prefs, "GXAlphaToCoverage", "GX_A2C", "yes", &src);
		s_alphaToCoverage = v.compareNoCase("yes") == 0 || v.compareNoCase("on") == 0 || v.compareNoCase("1") == 0;
		const Bool known = s_alphaToCoverage || v.compareNoCase("no") == 0 || v.compareNoCase("off") == 0 || v.compareNoCase("0") == 0;
		fprintf(stderr, "[GXA2C] key=%s (%s%s)\n", s_alphaToCoverage ? "yes" : "no", src, known ? "" : ", unknown value - no");
	}

	/// Options_InstanceNN.ini's naming (OptionPreferences::loadFromIniFile), so two local instances - the harness's
	/// multi-client runs - can each hold their own mode.
	AsciiString fileName()
	{
		AsciiString name = "GXGraphics.ini";
		if (rts::ClientInstance::getInstanceId() > 1u)
			name.format("GXGraphics_Instance%.2u.ini", rts::ClientInstance::getInstanceId());
		return name;
	}

	void loadOnce()
	{
		if (s_loaded)
			return;
		s_loaded = TRUE;
		UserPreferences prefs;
		prefs.load(fileName());
		s_remastered = prefs.getBool("GXRemastered", GXGraphics::platformDefault());
		fprintf(stderr, "[GXREMASTER] %s = %d (%s)\n", fileName().str(), (int)s_remastered,
			prefs.find("GXRemastered") != prefs.end() ? "from the file" : "code default");
		loadUpscaler(prefs);
		loadShadowMap(prefs);
		loadAlphaToCoverage(prefs);
	}
}

// wh1ter0se, 28/09/2026 (graphics DECISIONS_P3 #2): OFF everywhere until he approves the REMASTERED look; then ON for
// Windows, macOS and Linux, and OFF on iPad until its GPU cost is measured. One constant per platform. Every existing
// iPad takes whatever its line says: iOS seeds no GXGraphics.ini and has no launcher.
Bool GXGraphics::platformDefault()
{
#if defined(_WIN32)
	return FALSE;	// Windows
#elif defined(__APPLE__) && TARGET_OS_IPHONE
	return FALSE;	// iPad
#elif defined(__APPLE__)
	return FALSE;	// macOS
#else
	return FALSE;	// Linux
#endif
}

Bool GXGraphics::isRemastered()
{
	loadOnce();
	return s_remastered;
}

void GXGraphics::setRemastered(Bool on)
{
	loadOnce();
	if (on == s_remastered)
		return;
	s_remastered = on;

	// Load first so a key someone else put in the file survives; write whatever load found. load() names the file
	// before it opens it, so a missing file still writes - never gate the write on load's answer.
	UserPreferences prefs;
	prefs.load(fileName());
	prefs.setBool("GXRemastered", on);
	const Bool written = prefs.write();
	fprintf(stderr, "[GXREMASTER] set %d%s\n", (int)on, written ? "" : " - GXGraphics.ini NOT written, this session only");

	if (s_onChanged != nullptr)
		s_onChanged(on);
}

void GXGraphics::setOnRemasteredChanged(RemasteredChangedFn fn)
{
	s_onChanged = fn;
}

Int GXGraphics::upscaler() { loadOnce(); return s_upscaler; }
Int GXGraphics::requestedUpscaler() { loadOnce(); return s_upscalerAsked; }
void GXGraphics::demoteUpscaler(const char *why)
{
	loadOnce();
	if (s_upscaler == UPSCALER_OFF)
		return;
	fprintf(stderr, "[GXUPSCALE] demote requested=%s resolved=%s -> off reason=%s\n", s_upscalerNames[s_upscalerAsked],
		s_upscalerNames[s_upscaler], why != nullptr ? why : "run-time failure");
	s_upscaler = UPSCALER_OFF;
	s_scalePercent = 100;
}
const char *GXGraphics::upscalerName(Int u) { return (u >= 0 && u < UPSCALER_COUNT) ? s_upscalerNames[u] : "?"; }
Int GXGraphics::renderScalePercent() { loadOnce(); return s_scalePercent; }
Int GXGraphics::upscaleSharpness() { loadOnce(); return s_sharpness; }
Int GXGraphics::frameGen() { loadOnce(); return s_frameGen; }
Int GXGraphics::reflex() { loadOnce(); return s_reflex; }
Bool GXGraphics::shadowMap() { loadOnce(); return s_shadowMap; }
Int GXGraphics::shadowMapStepPin() { loadOnce(); return s_shadowMapStepPin; }
Bool GXGraphics::alphaToCoverage() { loadOnce(); return s_alphaToCoverage; }
unsigned int GXGraphics::lookFeatureMask()
{
	return (shadowMap() ? (unsigned int)LOOK_SHADOWMAP : 0u) | (alphaToCoverage() ? (unsigned int)LOOK_A2C : 0u);
}
Bool GXGraphics::upscalerIsTemporal(Int u)
{
	return u == UPSCALER_FSR3 || u == UPSCALER_DLSS || u == UPSCALER_METALFX_TEMPORAL || u == UPSCALER_XESS;
}

// GeneralsX @feature D · render-line-0967 21/10/2026 THE RENDERER SWITCH (GXRendererKey.h). Once per process, so SDL3Main (the loader
// choice, before SDL loads Vulkan), GxNative::Install (the device hook) and the REMASTERED ground's veto (TerrainTex.cpp) can never
// disagree. Options.ini needs the user-data path, which CommandLine::parseCommandLineForStartup sets up with TheGlobalData: a call
// before that answers from GX_RENDERER alone and decides nothing (no reader asks that early today).
Bool GXGraphics::nativeRendererRequested()
{
	static Bool s_decided = FALSE;
	static Bool s_native = FALSE;
	if (s_decided)
		return s_native;
	// GX_RENDERER - TEST-ONLY harness override of Options.ini GXRenderer: "native" = the native renderer, any other value = classic.
	const char *env = getenv("GX_RENDERER");
	if (TheGlobalData == nullptr)
		return gxRendererDecide(env, nullptr).pick == GXRENDERER_NATIVE ? TRUE : FALSE;
	OptionPreferences prefs;
	const OptionPreferences::const_iterator key = prefs.find("GXRenderer");
	const char *keyValue = key != prefs.end() ? key->second.str() : nullptr;
	const GxRendererDecision d = gxRendererDecide(env, keyValue);
	s_native = d.pick == GXRENDERER_NATIVE ? TRUE : FALSE;
	s_decided = TRUE;
	const char *badValue = d.source == GXRENDERER_SRC_ENV ? env : keyValue;
	if (d.bad)
		fprintf(stderr, "[GXRENDERER] renderer=%s (%s, unknown value \"%s\" - classic)\n", gxRendererName(d.pick),
			gxRendererSourceName(d.source), badValue != nullptr ? badValue : "");
	else
		fprintf(stderr, "[GXRENDERER] renderer=%s (%s)\n", gxRendererName(d.pick), gxRendererSourceName(d.source));
	return s_native;
}
