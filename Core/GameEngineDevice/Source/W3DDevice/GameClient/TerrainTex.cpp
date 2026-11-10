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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// FILE: TerrainTex.cpp ////////////////////////////////////////////////
//-----------------------------------------------------------------------------
//
//                       Westwood Studios Pacific.
//
//                       Confidential Information
//                Copyright (C) 2001 - All Rights Reserved
//
//-----------------------------------------------------------------------------
//
// Project:   RTS3
//
// File name: TerrainTex.cpp
//
// Created:   John Ahlquist, April 2001
//
// Desc:      TextureClass overrides to perform custom texturing for the terrain.
//
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//         Includes
//-----------------------------------------------------------------------------
#include <stdlib.h>

#include "W3DDevice/GameClient/TerrainTex.h"
#include "W3DDevice/GameClient/WorldHeightMap.h"
#include "W3DDevice/GameClient/TileData.h"
#include "Common/GlobalData.h"
#include "GameClient/GameClient.h"
#include "WW3D2/dx8caps.h"
#include "WW3D2/dx8wrapper.h"
#include "d3dx8tex.h"

#include <chrono>
#include <math.h>
#include <string.h>
#include <vector>
#include "d3dx8math.h"
#include "Common/GameLOD.h"
#include "Common/OptionPreferences.h"
#include "GameClient/GXGroundSteps.h"
#include "GameClient/GXGroundPrograms.h"	// the ground's three ps.1.1 programs (R-G1: shared with the native catalogue's test)
#include "GameLogic/GameLogic.h"
#include "W3DDevice/GameClient/W3DShaderManager.h"

//=============================================================================
// GeneralsX: THE REMASTERED GROUND, slice 1 (productised from
// test/look-ground-2 4fe7f0db9, itself grown from per-pixel relief; wh1ter0se 14/10 "new ground looks
// better"). The switch, its A/B override, the vetoes and the governor steps are GameClient/GXGroundSteps.h: Options.ini
// GXGround = yes (absent = kGxGroundKeyDefault: ON since wh1ter0se 17/10), GX_GROUND = off | "shade,blend,detail,macro,noise". OFF = today's ground byte for byte: nothing
// is built, nothing is drawn, no device call differs. Client-only: this file is outside the SimID digest
// (resources/gitinfo/simdigest_exclusions.txt), and nothing here is read by the simulation.
// The 2003 ground read as flat wallpaper for four reasons: one light value per
// 10 m vertex, soft 10 m linear fades between terrain types, two blurry grey multiply layers (cloud x macro noise, the "dirty
// smudge"), and the same tiles repeating every 40-200 m. When on, this draws the same 2003 tiles differently, in two ps.1.1
// passes over the same vertex buffers:
//  BASE (replaces the retail terrain*.pso for the HeightMap base pass): t0/t1 = the colour atlas at UV0/UV1 as before, t2/t3 = a
//    SHADE atlas at UV0/UV1 laid out exactly like the colour atlas. The two tiles are blended by a' = sat(s*(k*(h1-h0) + a - 0.5)
//    + 0.5) instead of the vertex alpha a: the taller texel wins near the cell edge, so grass tufts stand over dirt and the 10 m
//    smear becomes a crisp, irregular edge (s = 3: a third-of-a-cell band; the blend heights carry 5 m lumps, so the edge wanders
//    off the 10 m cell polygons - SW1 at s = 4 with 1 m lumps still traced them). |k*(h1-h0)| <= 0.65*0.498 = 0.324 < 0.5 - 0.5/s
//    = 0.333 keeps a' = a exactly at a = 0 and a = 1, so a cell that blends nothing never shows its (arbitrary) second tile. The colour is then multiplied
//    by light = sat(vertex light x shade factor), the shade lerped by the same a'. The saturate is the engine's own rule: doTheLight
//    clamps every vertex's light at 1.0 per channel, so a relief-lit facet gets at most full light, as a sun-facing slope does
//    today, and colour x light never exceeds 1.0 in the 8-bit framebuffer before MULT darkens it.
//  MULT (one extra pass, framebuffer *= 2*src): the stock cloud map and macro noise with the stock texture matrices (the noise's
//    depth softened, the preset's 5th value; 100 = stock), times a world-projected high-frequency DETAIL grain whose mips fade to
//    neutral (it shows only close up, no shimmer at the normal camera), times a world-projected low-frequency MACRO variation
//    (+-10% brightness, +-4% warm/cool) that breaks the 40-200 m tile repeats. Governor step 1 draws MULT_LITE instead: the cloud
//    and the softened noise only (framebuffer *= src), the same product with the two layers at neutral.
//  SHADE atlas: baked on the CPU from the colour atlas's own level-0 texels per texture class (wrapping inside the class like
//    the tile art does). Height = luminance blurred at 1, 3 and 8 texels; normals from it; the map's own terrain lights (ambient 0
//    + every global light, as BaseHeightMap::doTheLight lights the vertices) per texel, with a short soft self-shadow march toward
//    each light and a cavity term, as a factor RELATIVE to flat ground, normalised to mean 1 per class (same average brightness).
//    Alpha = the class's height at 3 and 8 texels of blur, normalised to [0.25, 0.75], for the blend above.
// WHEN IT BAKES (~350 ms of CPU on a 2048x1024 atlas, measured on wh1ter0se's PC by look-ground-2): at MAP LOAD, through GXGround_Prepare
// from HeightMapRenderObjClass::staticLightingChanged - W3DTerrainLogic::loadMap's setTimeOfDay reaches it once the map's lights
// are final (TerrainVisual::load built the atlas just before) - and again only when a time-of-day change relights the terrain.
// The first base pass bakes only if neither ran (after a device reset dropped the atlas), and says so in its log line. A failed
// bake latches the stock path for that atlas and device: no retry per frame.
// Test hooks (read once): GX_GROUND2 / GX_GROUND3 = more presets; GX_GROUND_AT="<frame>:<mode>,..." switches mode in-run from that
// logic frame on (0 = stock, 1-3 = preset; default 1) for one-run tuning sweeps; GX_GROUND_BAKE="depth,shadow,cavity" (texels x10,
// percent, x100; default 120,80,160); GX_GROUND_FLIP=1 bakes the lights from the opposite side (fail arm).
// UV axes (WorldHeightMap::getUVForTileIndex + HeightMap updateVB): +u = world +X, +v (atlas rows down) = world -Y.
// Known limits (later slices): cells with a 3rd texture (renderExtraBlendTiles, drawn after) show it
// stock where its alpha reaches 1, and roads stay stock; cliff cells whose UVs AdjustCliffTextures remaps along the slope are
// shaded with the flat-tile axes; the detail and macro layers are projected straight down, so they stretch on steep cliffs; the
// native renderer has no port (GXGroundSteps.h vetoes it).
// Ownership as in the relief prototype: the shade atlas belongs to ONE colour atlas (the TerrainTextureClass
// whose texels it was baked from) on ONE device; every device object is dropped in W3DShaderManager::shutdown (device alive) and
// rebuilt from the kept luminance copy.
//=============================================================================
struct GXGroundPreset { Bool valid; Real shade, blend, detail, macro, noise; int pct[5]; };
struct GXGroundClassRect { Int x, y, size; };
struct GXGroundLight { float u, v, z, d; };	// toward the light in atlas axes (+u = +X, +v = -Y), diffuse luminance
enum { GXG_MAX_LIGHTS = 3 };
static const float GXG_SHADE_ENC = 0.5f;	// shade atlas RGB = factor * this (factor <= 1.9); neutral (factor 1) = this
// What Options.ini GXGround = yes (or absent) draws: look-ground-2's NEW, the frames wh1ter0se approved as the direction (14/10, pairs 1 and 3).
static const int kGxgKeyPreset[5] = { 100, 100, 100, 100, 50 };

static std::vector<UnsignedByte> s_gxgLum;		// level-0 luminance of the owner atlas, width*height
static Int s_gxgW = 0, s_gxgH = 0;
static std::vector<GXGroundClassRect> s_gxgClasses;
static const void *s_gxgOwner = nullptr;			// the colour atlas s_gxgLum was read from
static IDirect3DTexture8 *s_gxgShade = nullptr;		// shade atlas (RGB = factor/2, A = blend height), on s_gxgDev
static IDirect3DTexture8 *s_gxgDetail = nullptr;
static IDirect3DTexture8 *s_gxgMacro = nullptr;
static IDirect3DTexture8 *s_gxgWhite = nullptr;
static DWORD s_gxgPSBase = 0, s_gxgPSMult = 0, s_gxgPSMultLite = 0;
static IDirect3DDevice8 *s_gxgDev = nullptr;		// the device every object above lives on
static Bool s_gxgDevFailed = false;					// creation failed on s_gxgDev: stock path until the next shutdown
static float s_gxgBakedLights[16];					// the light set the shade atlas was baked for (re-bake when it changes)
static Bool s_gxgBaseActive = false;				// the base pass was replaced this Render: the MULT pass must follow
static Bool s_gxgMultSet = false;					// MultBegin changed device state: MultEnd must restore it
static Bool s_gxgBakeFailed = false;				// the shade atlas could not be created for this atlas/device: stock path, no
													// per-frame retry; cleared by a new atlas or a shutdown
static Int s_gxgWhy = GXGROUND_OFF_KEY;				// this map's decision (GXGroundSteps.h), taken when its atlas is built
static Bool s_gxgOn = false;						// that decision is on AND the atlas is one this can bake from
static GXGroundPreset s_gxgP[3];					// 1 = this map's (GX_GROUND list or the key's values); 2, 3 = GX_GROUND2/3
static Int s_gxgAtN = 0;							// GX_GROUND_AT entries (0 = no in-run switching, no per-mode log line)
static Int s_gxgLastMode = -1;
static Int s_gxgLastStep = GXGROUND_STEP_FULL;		// the governor step the last base pass saw (a change is logged)
static Int s_gxgFrameStep = GXGROUND_STEP_FULL;		// the step GXGround_Base drew with: this Render's MultBegin draws the same

static Int gxgEnvInt(const char *name, Int dflt, Int lo, Int hi)
{
	const char *v = getenv(name);
	if (v == nullptr || *v == 0) return dflt;
	Int n = atoi(v);
	return n < lo ? lo : (n > hi ? hi : n);
}

static Bool gxgFlip() { static const Bool s = gxgEnvInt("GX_GROUND_FLIP", 0, 0, 1) != 0; return s; }

static UnsignedInt gxgFrame() { return TheGameLogic != nullptr ? TheGameLogic->getFrame() : 0u; }

// A preset from 1-5 percent values (GXGroundSteps.h gxGroundParseInts), each clamped to 0-100; the missing ones are 100.
static void gxgSetPreset(GXGroundPreset &p, const int *pct, int n)
{
	for (int j = 0; j < 5; j++) {
		const int v = j < n ? pct[j] : 100;
		p.pct[j] = v < 0 ? 0 : (v > 100 ? 100 : v);
	}
	p.shade = p.pct[0] / 100.0f; p.blend = p.pct[1] / 100.0f; p.detail = p.pct[2] / 100.0f;
	p.macro = p.pct[3] / 100.0f; p.noise = p.pct[4] / 100.0f;
	p.valid = true;
}

// GX_GROUND2 / GX_GROUND3: GX_GROUND_AT's modes 2 and 3 (test hooks, read once). Mode 1 is this map's own preset.
static void gxgReadTestPresets()
{
	static Bool s_read = false;
	if (s_read) return;
	s_read = true;
	const char *names[2] = { "GX_GROUND2", "GX_GROUND3" };
	for (Int i = 0; i < 2; i++) {
		const char *e = getenv(names[i]);
		int v[5];
		const int n = gxGroundParseInts(e, v, 5);
		s_gxgP[i + 1].valid = false;
		if (n > 0) {
			gxgSetPreset(s_gxgP[i + 1], v, n);
			const int *q = s_gxgP[i + 1].pct;
			fprintf(stderr, "[GXGROUND] preset %d (%s): shade=%d blend=%d detail=%d macro=%d noise=%d\n", i + 2, names[i], q[0], q[1], q[2], q[3], q[4]);
			fflush(stderr);
		} else if (e != nullptr && *e != 0) {
			fprintf(stderr, "[GXGROUND] %s='%s' refused - want \"shade,blend,detail,macro,noise\" in percent\n", names[i], e);
			fflush(stderr);
		}
	}
}

static const GXGroundPreset *gxgPreset(Int mode)
{
	return (mode >= 1 && mode <= 3 && s_gxgP[mode - 1].valid) ? &s_gxgP[mode - 1] : nullptr;
}

// GX_GROUND_AT: the mode for the logic frame being drawn (1 when unset)
static Int gxgMode()
{
	static UnsignedInt s_at[16];
	static Int s_m[16];
	static Int s_n = -1;
	if (s_n < 0) {
		s_n = 0;
		const char *p = getenv("GX_GROUND_AT");
		while (p != nullptr && *p != 0 && s_n < 16) {
			unsigned f = 0;
			int m = 0;
			if (sscanf(p, "%u:%d", &f, &m) != 2 || m < 0 || m > 3 || (s_n > 0 && f <= s_at[s_n - 1])) {
				fprintf(stderr, "[GXGROUND] GX_GROUND_AT rejected at '%s' - want <frame>:<0-3>[,...], ascending frames\n", p);
				s_n = 0;
				break;
			}
			s_at[s_n] = f;
			s_m[s_n] = m;
			s_n++;
			p = strchr(p, ',');
			if (p != nullptr) p++;
		}
		s_gxgAtN = s_n;
	}
	Int mode = 1;
	if (s_n > 0 && TheGameLogic != nullptr) {
		const UnsignedInt frame = TheGameLogic->getFrame();
		for (Int i = 0; i < s_n; i++) if (frame >= s_at[i]) mode = s_m[i];
	}
	return mode;
}

// This frame's preset; nullptr = the stock path (the ground is off for this map, or GX_GROUND_AT says mode 0).
static const GXGroundPreset *gxgActive()
{
	if (!s_gxgOn) return nullptr;
	const Int mode = gxgMode();
	if (mode != s_gxgLastMode) {
		if (s_gxgAtN > 0) {
			fprintf(stderr, "[GXGROUND] f=%u mode %d\n", (unsigned)gxgFrame(), mode);
			fflush(stderr);
		}
		s_gxgLastMode = mode;
	}
	return mode == 0 ? nullptr : gxgPreset(mode);
}

// The look governor's step for this frame (GXGroundSteps.h: g_gxGroundGovStep, written after a frame commits), clamped; each
// change is logged once. Only read while the ground is on for this map.
static Int gxGroundGovStep()
{
	const Int step = gxGroundClampStep(g_gxGroundGovStep);
	if (step != s_gxgLastStep) {
		fprintf(stderr, "[GXGROUND] f=%u governor step %d->%d (%s)\n", (unsigned)gxgFrame(), (int)s_gxgLastStep, (int)step, gxGroundStepName(step));
		fflush(stderr);
		s_gxgLastStep = step;
	}
	return step;
}

// This map's switch (GXGroundSteps.h gxGroundDecide), taken when its atlas is built; logs one line. Options.ini is read only
// when GX_GROUND leaves the decision to the key, and never on a -headless peer (off there either way).
static void gxgDecideForMap()
{
	gxgReadTestPresets();
	const char *env = getenv("GX_GROUND");
	const int envState = gxGroundEnvState(env);
	const Bool headless = TheGlobalData != nullptr && TheGlobalData->m_headless;
	if (envState == GXGROUND_ENV_BAD) {
		fprintf(stderr, "[GXGROUND] GX_GROUND='%s' refused - want off or \"shade,blend,detail,macro,noise\" in percent; Options.ini decides\n", env);
		fflush(stderr);
	}
	const Bool keyMatters = (envState == GXGROUND_ENV_NONE || envState == GXGROUND_ENV_BAD) && !headless;
	const Bool key = keyMatters && OptionPreferences().getBool("GXGround", kGxGroundKeyDefault ? TRUE : FALSE);
	// Either renderer: the native one draws the ground's three programs from its catalogue (R-G1, GXGroundSteps.h gxGroundDecide).
	s_gxgWhy = gxGroundDecide(headless != FALSE, envState, key != FALSE);
	s_gxgP[0].valid = false;
	if (s_gxgWhy == GXGROUND_ON_ENV) {
		int v[5];
		const int n = gxGroundParseInts(env, v, 5);
		gxgSetPreset(s_gxgP[0], v, n);
	} else if (s_gxgWhy == GXGROUND_ON_KEY) {
		gxgSetPreset(s_gxgP[0], kGxgKeyPreset, 5);
	}
}

static void gxgReleaseShade()
{
	if (s_gxgShade != nullptr) {
		s_gxgShade->Release();
		s_gxgShade = nullptr;
	}
}

static void gxgForgetAtlas()
{
	gxgReleaseShade();
	s_gxgLum.clear();
	s_gxgClasses.clear();
	s_gxgW = s_gxgH = 0;
	s_gxgOwner = nullptr;
	s_gxgBakeFailed = false;
	g_gxGroundEngaged = s_gxgOn && !s_gxgBakeFailed && !s_gxgDevFailed;	// GXGroundSteps.h: the governor's availability
	s_gxgOn = false;
	g_gxGroundEngaged = s_gxgOn && !s_gxgBakeFailed && !s_gxgDevFailed;	// GXGroundSteps.h: the governor's availability
}

static UnsignedByte gxgU8(Real x)
{
	Int b = (Int)floorf(x * 255.0f + 0.5f);
	return (UnsignedByte)(b < 0 ? 0 : (b > 255 ? 255 : b));
}

// Wrapping box blur of an n x n block, radius r < n (separable running sums).
static void gxgBox(const std::vector<float> &src, std::vector<float> &dst, Int n, Int r)
{
	auto wrap = [n](Int i) { return ((i % n) + n) % n; };
	std::vector<float> tmp((size_t)n * n);
	const float inv = 1.0f / (float)(2 * r + 1);
	for (Int y = 0; y < n; y++) {
		const float *s = &src[(size_t)y * n];
		float acc = 0;
		for (Int k = -r; k <= r; k++) acc += s[wrap(k)];
		for (Int x = 0; x < n; x++) {
			tmp[(size_t)y * n + x] = acc * inv;
			acc += s[wrap(x + r + 1)] - s[wrap(x - r)];
		}
	}
	dst.assign((size_t)n * n, 0.0f);
	for (Int x = 0; x < n; x++) {
		float acc = 0;
		for (Int k = -r; k <= r; k++) acc += tmp[(size_t)wrap(k) * n + x];
		for (Int y = 0; y < n; y++) {
			dst[(size_t)y * n + x] = acc * inv;
			acc += tmp[(size_t)wrap(y + r + 1) * n + x] - tmp[(size_t)wrap(y - r) * n + x];
		}
	}
}

// The light set the vertices are lit with now (doTheLight): ambient 0 + every global light. key = what a re-bake compares.
static Int gxgLights(GXGroundLight *L, float *amb, float *key)
{
	auto lumOf = [](const RGBColor &c) { return 0.299f * c.red + 0.587f * c.green + 0.114f * c.blue; };
	*amb = lumOf(TheGlobalData->m_terrainAmbient[0]);
	Int n = TheGlobalData->m_numGlobalLights;
	n = n < 0 ? 0 : (n > GXG_MAX_LIGHTS ? GXG_MAX_LIGHTS : n);
	Int k = 0;
	for (Int i = 0; i < 16; i++) key[i] = 0.0f;
	for (Int i = 0; i < n; i++) {
		const Coord3D &p = TheGlobalData->m_terrainLightPos[i];
		float x = -p.x, y = -p.y, z = -p.z;
		const float len = sqrtf(x * x + y * y + z * z);
		if (len < 1e-4f) continue;
		x /= len; y /= len; z /= len;
		if (gxgFlip()) { x = -x; y = -y; }
		L[k].u = x; L[k].v = -y; L[k].z = z; L[k].d = lumOf(TheGlobalData->m_terrainDiffuse[i]);
		key[k * 4 + 0] = x; key[k * 4 + 1] = y; key[k * 4 + 2] = z; key[k * 4 + 3] = L[k].d;
		k++;
	}
	key[15] = *amb;
	return k;
}

// Bakes one class block (n x n at (x0, y0) in the atlas) into the locked shade atlas (B G R A rows).
static void gxgBakeClass(Int x0, Int y0, Int n, UnsignedByte *rgba, size_t pitch, const GXGroundLight *L, Int nl, float amb,
	float depth, float shadow, float cavity)
{
	const size_t nn = (size_t)n * n;
	std::vector<float> lum(nn), h1, h2, h3, Z(nn), F(nn);
	for (Int y = 0; y < n; y++)
		for (Int x = 0; x < n; x++)
			lum[(size_t)y * n + x] = s_gxgLum[(size_t)(y0 + y) * s_gxgW + (x0 + x)] * (1.0f / 255.0f);
	gxgBox(lum, h1, n, 1);
	gxgBox(lum, h2, n, 3);
	gxgBox(lum, h3, n, 8);
	double mean = 0, mean2 = 0;
	std::vector<float> hbl(nn);	// the blend height: 3- and 8-texel lumps (~1 m and ~5 m)
	for (size_t i = 0; i < nn; i++) {
		Z[i] = (1.0f * h1[i] + 0.6f * h2[i] + 0.35f * h3[i]) * (1.0f / 1.95f);
		hbl[i] = 0.5f * (h2[i] + h3[i]);
		mean += Z[i];
		mean2 += hbl[i];
	}
	mean /= (double)nn;
	mean2 /= (double)nn;
	double var2 = 0;
	for (size_t i = 0; i < nn; i++) {
		Z[i] = (float)((Z[i] - mean) * depth);	// texel units
		var2 += (hbl[i] - mean2) * (hbl[i] - mean2);
	}
	const float std2 = (float)sqrt(var2 / (double)nn) + 1e-4f;
	auto at = [n](const std::vector<float> &a, Int x, Int y) -> float { return a[(size_t)(((y % n) + n) % n) * n + (((x % n) + n) % n)]; };
	float den = amb;	// flat ground under the same lights
	for (Int l = 0; l < nl; l++) den += L[l].d * (L[l].z > 0 ? L[l].z : 0);
	if (den < 1e-3f) den = 1e-3f;
	double sumF = 0;
	for (Int y = 0; y < n; y++) {
		for (Int x = 0; x < n; x++) {
			const float gu = (at(Z, x + 1, y) - at(Z, x - 1, y)) * 0.5f, gv = (at(Z, x, y + 1) - at(Z, x, y - 1)) * 0.5f;
			const float inv = 1.0f / sqrtf(gu * gu + gv * gv + 1.0f);
			const float nu = -gu * inv, nv = -gv * inv, nz = inv;
			const float z0 = at(Z, x, y);
			float num = amb;
			for (Int l = 0; l < nl; l++) {
				float ndl = nu * L[l].u + nv * L[l].v + nz * L[l].z;
				if (ndl <= 0) continue;
				if (ndl > 1) ndl = 1;
				float occ = 0;
				const float hz = sqrtf(L[l].u * L[l].u + L[l].v * L[l].v);
				if (shadow > 0 && hz > 1e-3f && L[l].z > 0.02f) {
					const float du = L[l].u / hz, dv = L[l].v / hz, tanE = L[l].z / hz;
					for (Int s = 1; s <= 20 && occ < 1.0f; s++) {
						const float o = (at(Z, x + (Int)floorf(s * du + 0.5f), y + (Int)floorf(s * dv + 0.5f)) - (z0 + s * tanE)) * (1.0f / 1.5f);
						if (o > occ) occ = o > 1.0f ? 1.0f : o;
					}
				}
				num += L[l].d * ndl * (1.0f - shadow * occ);
			}
			float c = (h3[(size_t)y * n + x] - h1[(size_t)y * n + x]) * cavity;
			c = c < 0 ? 0 : (c > 0.35f ? 0.35f : c);
			const float f = (num / den) * (1.0f - c);
			F[(size_t)y * n + x] = f;
			sumF += f;
		}
	}
	// Mean 1 per class (today's average brightness), clamp, mean 1 again (applied when encoding).
	float m = (float)(sumF / (double)nn);
	if (m < 1e-3f) m = 1e-3f;
	sumF = 0;
	for (size_t i = 0; i < nn; i++) {
		const float f = F[i] / m;
		F[i] = f < 0.25f ? 0.25f : (f > 1.9f ? 1.9f : f);
		sumF += F[i];
	}
	m = (float)(sumF / (double)nn);
	for (Int y = 0; y < n; y++) {
		UnsignedByte *o = rgba + (size_t)(y0 + y) * pitch + (size_t)x0 * 4;
		for (Int x = 0; x < n; x++, o += 4) {
			const size_t i = (size_t)y * n + x;
			float f = F[i] / m;
			f = f > 1.99f ? 1.99f : f;	// the encoding holds factors up to 2.0
			const UnsignedByte s = gxgU8(f * GXG_SHADE_ENC);
			float hb = 0.5f + 0.22f * (hbl[i] - (float)mean2) / std2;
			hb = hb < 0.25f ? 0.25f : (hb > 0.75f ? 0.75f : hb);
			o[0] = s; o[1] = s; o[2] = s; o[3] = gxgU8(hb);
		}
	}
}

// (Re)bakes the shade atlas on device for the given lights. False = stock path. `at` (prepare = map load or a relight, draw =
// the first base pass) and `why` go into the log line with the time it took.
static Bool gxgBake(IDirect3DDevice8 *device, const GXGroundLight *L, Int nl, float amb, const char *at, const char *why)
{
	gxgReleaseShade();
	if (s_gxgLum.empty()) return false;
	const auto t0 = std::chrono::steady_clock::now();
	IDirect3DTexture8 *tex = nullptr;
	if (FAILED(D3DXCreateTexture(device, s_gxgW, s_gxgH, (UINT)MIP_LEVELS_3, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex)) || tex == nullptr) {
		fprintf(stderr, "[GXGROUND] shade atlas %dx%d: create failed, stock path\n", s_gxgW, s_gxgH);
		fflush(stderr);
		return false;
	}
	D3DSURFACE_DESC desc;
	D3DLOCKED_RECT lr;
	if (FAILED(tex->GetLevelDesc(0, &desc)) || desc.Format != D3DFMT_A8R8G8B8 || (Int)desc.Width != s_gxgW || (Int)desc.Height != s_gxgH ||
		FAILED(tex->LockRect(0, &lr, nullptr, 0)) || lr.pBits == nullptr) {
		tex->Release();
		fprintf(stderr, "[GXGROUND] shade atlas %dx%d: unexpected surface, stock path\n", s_gxgW, s_gxgH);
		fflush(stderr);
		return false;
	}
	UnsignedByte *base = (UnsignedByte *)lr.pBits;
	const size_t pitch = (size_t)lr.Pitch;
	const UnsignedByte nS = gxgU8(GXG_SHADE_ENC);	// neutral: factor 1, blend height 0.5
	for (Int y = 0; y < s_gxgH; y++) {
		UnsignedByte *o = base + (size_t)y * pitch;
		for (Int x = 0; x < s_gxgW; x++, o += 4) { o[0] = nS; o[1] = nS; o[2] = nS; o[3] = 0x80; }
	}
	int b[3] = { 120, 80, 160 };
	if (const char *e = getenv("GX_GROUND_BAKE")) {
		int t[3];
		const int n = gxGroundParseInts(e, t, 3);
		for (int i = 0; i < n; i++) b[i] = t[i];
		static Bool s_refusedLogged = false;
		if (n == 0 && *e != 0 && !s_refusedLogged) {
			s_refusedLogged = true;
			fprintf(stderr, "[GXGROUND] GX_GROUND_BAKE='%s' refused - want \"depth,shadow,cavity\"; defaults 120,80,160\n", e);
			fflush(stderr);
		}
	}
	const float depth = (b[0] < 0 ? 0 : (b[0] > 1000 ? 1000 : b[0])) / 10.0f;
	const float shadow = (b[1] < 0 ? 0 : (b[1] > 100 ? 100 : b[1])) / 100.0f;
	const float cavity = (b[2] < 0 ? 0 : (b[2] > 1000 ? 1000 : b[2])) / 100.0f;
	Int baked = 0;
	for (const GXGroundClassRect &r : s_gxgClasses) {
		const Int w = r.size;
		if (r.x < 4 || r.y < 4 || w < 32 || r.x + w + 4 > s_gxgW || r.y + w + 4 > s_gxgH) continue;	// the blocks update() fills
		gxgBakeClass(r.x, r.y, w, base, pitch, L, nl, amb, depth, shadow, cavity);
		// 4-texel gutters from the class's own wrapped interior, as update() does for the colours
		for (Int j = 0; j < w; j++) {
			UnsignedByte *p = base + (size_t)(r.y + j) * pitch + (size_t)r.x * 4;
			memcpy(p - 4 * 4, p + (w - 4) * 4, 4 * 4);
			memcpy(p + w * 4, p, 4 * 4);
		}
		for (Int j = 0; j < 4; j++) {
			UnsignedByte *above = base + (size_t)(r.y - j - 1) * pitch + (size_t)(r.x - 4) * 4;
			memcpy(above, above + (size_t)w * pitch, (size_t)(w + 8) * 4);
			UnsignedByte *below = base + (size_t)(r.y + j) * pitch + (size_t)(r.x - 4) * 4;
			memcpy(below + (size_t)w * pitch, below, (size_t)(w + 8) * 4);
		}
		baked++;
	}
	tex->UnlockRect(0);
	D3DXFilterTexture(tex, nullptr, 0, D3DX_FILTER_BOX);
	s_gxgShade = tex;
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	fprintf(stderr, "[GXGROUND] shade atlas %dx%d baked f=%u at=%s why=%s classes=%d lights=%d amb=%.3f sun=(%.3f,%.3f,%.3f) depth=%.1f shadow=%.2f cavity=%.2f flip=%d ms=%.1f\n",
		s_gxgW, s_gxgH, (unsigned)gxgFrame(), at, why, baked, nl, amb, nl > 0 ? L[0].u : 0.0f, nl > 0 ? -L[0].v : 0.0f, nl > 0 ? L[0].z : 0.0f,
		depth, shadow, cavity, gxgFlip() ? 1 : 0, ms);
	fflush(stderr);
	return true;
}

// The shade atlas for the lights the vertices are lit with now (doTheLight's set): kept while those lights are the ones it was
// baked for, baked when it is missing or they changed. FALSE = the stock path; a failed bake latches it until a new atlas or a
// device shutdown, so nothing retries per frame.
static Bool gxgEnsureShade(IDirect3DDevice8 *device, const char *at)
{
	if (s_gxgBakeFailed) return FALSE;
	GXGroundLight L[GXG_MAX_LIGHTS];
	float amb = 0, key[16];
	const Int nl = gxgLights(L, &amb, key);
	if (s_gxgShade != nullptr && memcmp(key, s_gxgBakedLights, sizeof(key)) == 0) return TRUE;
	if (!gxgBake(device, L, nl, amb, at, s_gxgShade == nullptr ? "new" : "lights-changed")) {
		s_gxgBakeFailed = true;
		g_gxGroundEngaged = s_gxgOn && !s_gxgBakeFailed && !s_gxgDevFailed;	// GXGroundSteps.h: the governor's availability
		return FALSE;
	}
	memcpy(s_gxgBakedLights, key, sizeof(key));
	return TRUE;
}

// Periodic value noise on an n x n texture, `cells` lattice cells per side at the first octave, smoothstep interpolation;
// normalised to [-1, 1] (+-2.5 standard deviations).
static float gxgHash(Int x, Int y, Int seed)
{
	UnsignedInt h = (UnsignedInt)x * 374761393u + (UnsignedInt)y * 668265263u + (UnsignedInt)seed * 2246822519u;
	h = (h ^ (h >> 13)) * 1274126177u;
	h ^= h >> 16;
	return (h & 0xffffff) * (1.0f / 16777215.0f);
}
static void gxgFbm(std::vector<float> &out, Int n, Int cells, Int octaves, float gain, Int seed)
{
	out.assign((size_t)n * n, 0.0f);
	float amp = 1.0f, tot = 0.0f;
	for (Int o = 0; o < octaves && cells <= n; o++, cells *= 2, amp *= gain) {
		for (Int y = 0; y < n; y++) {
			const float fy = (float)y * cells / n;
			const Int y0 = (Int)fy;
			float ty = fy - y0;
			ty = ty * ty * (3 - 2 * ty);
			for (Int x = 0; x < n; x++) {
				const float fx = (float)x * cells / n;
				const Int x0 = (Int)fx;
				float tx = fx - x0;
				tx = tx * tx * (3 - 2 * tx);
				const Int xa = x0 % cells, xb = (x0 + 1) % cells, ya = y0 % cells, yb = (y0 + 1) % cells;
				const float a = gxgHash(xa, ya, seed + o) * (1 - tx) + gxgHash(xb, ya, seed + o) * tx;
				const float b = gxgHash(xa, yb, seed + o) * (1 - tx) + gxgHash(xb, yb, seed + o) * tx;
				out[(size_t)y * n + x] += amp * (a * (1 - ty) + b * ty);
			}
		}
		tot += amp;
	}
	double mean = 0, var = 0;
	for (float &v : out) { v /= tot; mean += v; }
	mean /= (double)out.size();
	for (float v : out) var += (v - mean) * (v - mean);
	const float sd = (float)sqrt(var / (double)out.size()) + 1e-6f;
	for (float &v : out) {
		v = (float)((v - mean) / sd);
		v = (v < -2.5f ? -2.5f : (v > 2.5f ? 2.5f : v)) / 2.5f;
	}
}

// A full-mip A8R8G8B8 texture from R, G, B planes in [0, 1] at level 0; each lower level is the box average of the one above,
// pulled toward 0.5 by fade[level] (1 = keep; levels past nfade are 0.5).
static IDirect3DTexture8 *gxgMakeTex(IDirect3DDevice8 *device, Int n, const std::vector<float> *ch, const float *fade, Int nfade)
{
	IDirect3DTexture8 *tex = nullptr;
	if (FAILED(D3DXCreateTexture(device, n, n, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex)) || tex == nullptr) return nullptr;
	std::vector<float> cur[3] = { ch[0], ch[1], ch[2] };
	Int size = n;
	const Int levels = (Int)tex->GetLevelCount();
	for (Int lev = 0; lev < levels; lev++) {
		D3DSURFACE_DESC desc;
		D3DLOCKED_RECT lr;
		if (FAILED(tex->GetLevelDesc(lev, &desc)) || desc.Format != D3DFMT_A8R8G8B8 || (Int)desc.Width != size || (Int)desc.Height != size) {
			tex->Release();
			return nullptr;
		}
		if (FAILED(tex->LockRect(lev, &lr, nullptr, 0)) || lr.pBits == nullptr) {
			tex->Release();
			return nullptr;
		}
		const float f = lev < nfade ? fade[lev] : 0.0f;
		for (Int y = 0; y < size; y++) {
			UnsignedByte *o = (UnsignedByte *)lr.pBits + (size_t)y * lr.Pitch;
			for (Int x = 0; x < size; x++, o += 4) {
				const size_t i = (size_t)y * size + x;
				o[2] = gxgU8(0.5f + (cur[0][i] - 0.5f) * f);
				o[1] = gxgU8(0.5f + (cur[1][i] - 0.5f) * f);
				o[0] = gxgU8(0.5f + (cur[2][i] - 0.5f) * f);
				o[3] = 0xff;
			}
		}
		tex->UnlockRect(lev);
		if (size > 1) {
			const Int h = size / 2;
			for (Int c = 0; c < 3; c++) {
				std::vector<float> nx((size_t)h * h);
				for (Int y = 0; y < h; y++)
					for (Int x = 0; x < h; x++)
						nx[(size_t)y * h + x] = 0.25f * (cur[c][(size_t)(2 * y) * size + 2 * x] + cur[c][(size_t)(2 * y) * size + 2 * x + 1] +
							cur[c][(size_t)(2 * y + 1) * size + 2 * x] + cur[c][(size_t)(2 * y + 1) * size + 2 * x + 1]);
				cur[c].swap(nx);
			}
			size = h;
		}
	}
	return tex;
}

// The three ps.1.1 programs (BASE, MULT, MULT_LITE) and their source text: GameClient/GXGroundPrograms.h.

// GX-ground device objects: the three pixel shaders, the detail / macro textures and a 1x1 white. False = stock path. Built at
// map load (GXGround_Prepare), so a governor step change mid-match never creates anything.
static Bool gxgEnsureDevice(IDirect3DDevice8 *device)
{
	if (s_gxgDev != device) {
		// A different device: the old objects died with theirs (W3DShaderManager::shutdown drops them while it is alive).
		s_gxgPSBase = s_gxgPSMult = s_gxgPSMultLite = 0;
		s_gxgShade = s_gxgDetail = s_gxgMacro = s_gxgWhite = nullptr;
		s_gxgDev = device;
		s_gxgDevFailed = false;
		g_gxGroundEngaged = s_gxgOn && !s_gxgBakeFailed && !s_gxgDevFailed;	// GXGroundSteps.h: the governor's availability
		s_gxgBakeFailed = false;
		g_gxGroundEngaged = s_gxgOn && !s_gxgBakeFailed && !s_gxgDevFailed;	// GXGroundSteps.h: the governor's availability
	}
	if (s_gxgDevFailed) return false;
	if (s_gxgPSBase != 0 && s_gxgPSMult != 0 && s_gxgPSMultLite != 0 && s_gxgDetail != nullptr && s_gxgMacro != nullptr && s_gxgWhite != nullptr) return true;
	s_gxgDevFailed = true;	// until everything below exists
	g_gxGroundEngaged = s_gxgOn && !s_gxgBakeFailed && !s_gxgDevFailed;	// GXGroundSteps.h: the governor's availability
	if (s_gxgPSBase == 0 && FAILED(device->CreatePixelShader((const DWORD *)kGxGroundBasePs, &s_gxgPSBase))) {
		s_gxgPSBase = 0;
		fprintf(stderr, "[GXGROUND] CreatePixelShader(base) failed: stock path\n");
		fflush(stderr);
		return false;
	}
	if (s_gxgPSMult == 0 && FAILED(device->CreatePixelShader((const DWORD *)kGxGroundMultPs, &s_gxgPSMult))) {
		s_gxgPSMult = 0;
		fprintf(stderr, "[GXGROUND] CreatePixelShader(mult) failed: stock path\n");
		fflush(stderr);
		return false;
	}
	if (s_gxgPSMultLite == 0 && FAILED(device->CreatePixelShader((const DWORD *)kGxGroundMultLitePs, &s_gxgPSMultLite))) {
		s_gxgPSMultLite = 0;
		fprintf(stderr, "[GXGROUND] CreatePixelShader(mult-lite) failed: stock path\n");
		fflush(stderr);
		return false;
	}
	const auto t0 = std::chrono::steady_clock::now();
	if (s_gxgDetail == nullptr) {
		// Grain, 256 px per 32 world units (2.5 texels per atlas texel); mips fade to neutral, so it shows only close up.
		std::vector<float> g;
		gxgFbm(g, 256, 16, 5, 0.62f, 7);
		for (float &v : g) v = 0.5f + 0.5f * 0.20f * v;	// x(1 +- 20%) at full strength
		const std::vector<float> ch[3] = { g, g, g };
		const float fade[] = { 1.0f, 0.85f, 0.6f, 0.35f, 0.15f };
		s_gxgDetail = gxgMakeTex(device, 256, ch, fade, 5);
	}
	if (s_gxgMacro == nullptr) {
		// Macro variation, 256 px per 1600 world units: x(1 +- 10%) brightness, +-4% warm (dry) / cool (lush).
		std::vector<float> v, h;
		gxgFbm(v, 256, 4, 4, 0.55f, 11);
		gxgFbm(h, 256, 3, 3, 0.5f, 12);
		std::vector<float> ch[3];
		for (Int c = 0; c < 3; c++) ch[c].resize(v.size());
		for (size_t i = 0; i < v.size(); i++) {
			const float b = 1.0f + 0.10f * v[i];
			ch[0][i] = 0.5f * b * (1.0f + 0.04f * h[i]);
			ch[1][i] = 0.5f * b * (1.0f - 0.012f * h[i]);
			ch[2][i] = 0.5f * b * (1.0f - 0.04f * h[i]);
		}
		const float fade[] = { 1, 1, 1, 1, 1, 1, 1, 1, 1 };
		s_gxgMacro = gxgMakeTex(device, 256, ch, fade, 9);
	}
	if (s_gxgWhite == nullptr) {
		if (SUCCEEDED(D3DXCreateTexture(device, 1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &s_gxgWhite)) && s_gxgWhite != nullptr) {
			D3DLOCKED_RECT lr;
			if (SUCCEEDED(s_gxgWhite->LockRect(0, &lr, nullptr, 0)) && lr.pBits != nullptr) {
				*(DWORD *)lr.pBits = 0xffffffff;
				s_gxgWhite->UnlockRect(0);
			} else {
				s_gxgWhite->Release();
				s_gxgWhite = nullptr;
			}
		} else {
			s_gxgWhite = nullptr;
		}
	}
	if (s_gxgDetail == nullptr || s_gxgMacro == nullptr || s_gxgWhite == nullptr) {
		fprintf(stderr, "[GXGROUND] detail/macro/white textures failed: stock path\n");
		fflush(stderr);
		return false;
	}
	fprintf(stderr, "[GXGROUND] device objects ready (3 ps.1.1, detail 256 full mips, macro 256) ms=%.1f\n",
		std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
	fflush(stderr);
	s_gxgDevFailed = false;
	g_gxGroundEngaged = s_gxgOn && !s_gxgBakeFailed && !s_gxgDevFailed;	// GXGroundSteps.h: the governor's availability
	return true;
}

// GeneralsX: Map load (HeightMapRenderObjClass::staticLightingChanged): everything the ground draws with
// is built here, while the loading screen is up - the device objects and the shade atlas for the map's final lights (the ~350 ms
// look-ground-2 used to spend in the first frame). Called again on a time-of-day change, it re-bakes only if the lights moved.
// The same gates as the base pass, so nothing is built that the base pass would not use. FALSE = the stock path for now.
Bool GXGround_Prepare(TextureClass *atlas)
{
	if (!s_gxgOn || atlas == nullptr || TheGlobalData == nullptr) return FALSE;
	if (s_gxgOwner != (const void *)atlas || atlas->Peek_D3D_Texture() == nullptr || s_gxgLum.empty()) return FALSE;
	if (W3DShaderManager::getChipset() < DC_GENERIC_PIXEL_SHADER_1_1) return FALSE;	// the base pass replaces only the pixel-shader one
	IDirect3DDevice8 *device = DX8Wrapper::_Get_D3D_Device8();
	if (device == nullptr || !gxgEnsureDevice(device)) return FALSE;
	return gxgEnsureShade(device, "prepare");
}

Bool GXGround_Base(TextureClass *atlas)
{
	s_gxgBaseActive = false;
	const GXGroundPreset *p = gxgActive();
	if (p == nullptr || atlas == nullptr || TheGlobalData == nullptr) return FALSE;
	if (W3DShaderManager::getChipset() < DC_GENERIC_PIXEL_SHADER_1_1) return FALSE;	// the caller's pass is the retail pixel-shader one
	g_gxGroundBasePath = true;	// GXGroundSteps.h: this pass is one the ground replaces (read by the governor glue, at any step)
	// The governor's last step is the stock ground: the retail set() above stands, and every object and the atlas stay built.
	const Int step = gxGroundGovStep();
	if (step >= GXGROUND_STEP_STOCK) return FALSE;
	IDirect3DDevice8 *device = DX8Wrapper::_Get_D3D_Device8();
	if (device == nullptr || s_gxgOwner != (const void *)atlas || atlas->Peek_D3D_Texture() == nullptr || s_gxgLum.empty()) return FALSE;
	if (!gxgEnsureDevice(device)) return FALSE;
	// Baked at map load by GXGround_Prepare; this bakes (and logs at=draw) only after a device reset dropped the atlas, or for a
	// light change that did not pass staticLightingChanged.
	if (!gxgEnsureShade(device, "draw")) return FALSE;
	const DWORD lod = atlas->Peek_D3D_Texture()->GetLOD();	// follow the colour atlas's texture reduction (options slider)
	if (s_gxgShade->GetLOD() != lod) s_gxgShade->SetLOD(lod);
	// The retail set() just bound t0/t1 = the colour atlas at UV0/UV1 with the options' filters; stages 2/3 become the shade atlas.
	if (FAILED(device->SetPixelShader(s_gxgPSBase))) {
		s_gxgDevFailed = true;	// stock path until the next shutdown; the retail set()'s state is untouched
		g_gxGroundEngaged = s_gxgOn && !s_gxgBakeFailed && !s_gxgDevFailed;	// GXGroundSteps.h: the governor's availability
		return FALSE;
	}
	device->SetTexture(2, s_gxgShade);
	device->SetTexture(3, s_gxgShade);
	for (Int stage = 2; stage < 4; stage++) {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_TEXCOORDINDEX, stage - 2);
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MIPFILTER, D3DTEXF_LINEAR);
	}
	const float s = 1.0f + 2.0f * p->blend, k = 0.65f * p->blend;	// |k*dh| <= 0.65*0.498*blend < 0.5 - 0.5/s for blend in [0, 1]
	const float c0[4] = { k, k, k, k }, c1[4] = { s / 4, s / 4, s / 4, s / 4 }, c2[4] = { 0.125f, 0.125f, 0.125f, 0.125f };
	const float c3[4] = { p->shade, p->shade, p->shade, p->shade }, c4[4] = { GXG_SHADE_ENC, GXG_SHADE_ENC, GXG_SHADE_ENC, GXG_SHADE_ENC };
	device->SetPixelShaderConstant(0, c0, 1);
	device->SetPixelShaderConstant(1, c1, 1);
	device->SetPixelShaderConstant(2, c2, 1);
	device->SetPixelShaderConstant(3, c3, 1);
	device->SetPixelShaderConstant(4, c4, 1);
	s_gxgFrameStep = step;
	s_gxgBaseActive = true;
	return TRUE;
}

Bool GXGround_MultBegin(TextureClass *cloud, TextureClass *noise)
{
	const Bool base = s_gxgBaseActive;
	s_gxgBaseActive = false;
	s_gxgMultSet = false;
	if (!base) return FALSE;	// only after a replaced base pass: that one left cloud and noise out
	const GXGroundPreset *p = gxgActive();
	IDirect3DDevice8 *device = DX8Wrapper::_Get_D3D_Device8();
	// The step the base pass drew with (the governor writes its step between frames, never inside one).
	const Bool lite = s_gxgFrameStep >= GXGROUND_STEP_NO_LAYERS;
	const DWORD ps = lite ? s_gxgPSMultLite : s_gxgPSMult;
	if (p == nullptr || device == nullptr || device != s_gxgDev || ps == 0) return FALSE;
	DX8Wrapper::Apply_Render_State_Changes();
	Matrix4x4 m[4];
	GXGround_TerrainNoiseMatrices((float *)&m[0], (float *)&m[1]);	// the retail cloud / noise projections
	Matrix4x4 view;
	DX8Wrapper::_Get_DX8_Transform(D3DTS_VIEW, view);
	D3DXMATRIX inv, sc;
	float det;
	D3DXMatrixInverse(&inv, &det, (D3DXMATRIX *)&view);
	D3DXMatrixScaling(&sc, 1.0f / 32.0f, 1.0f / 32.0f, 1.0f);
	*(D3DXMATRIX *)&m[2] = inv * sc;	// detail: one repeat per 32 world units
	D3DXMatrixScaling(&sc, 1.0f / 1600.0f, 1.0f / 1600.0f, 1.0f);
	*(D3DXMATRIX *)&m[3] = inv * sc;	// macro: one repeat per 1600 world units
	IDirect3DBaseTexture8 *t[4] = {
		(cloud != nullptr && cloud->Peek_D3D_Texture() != nullptr) ? (IDirect3DBaseTexture8 *)cloud->Peek_D3D_Texture() : (IDirect3DBaseTexture8 *)s_gxgWhite,
		(noise != nullptr && noise->Peek_D3D_Texture() != nullptr) ? (IDirect3DBaseTexture8 *)noise->Peek_D3D_Texture() : (IDirect3DBaseTexture8 *)s_gxgWhite,
		s_gxgDetail, s_gxgMacro };
	s_gxgMultSet = true;
	// One stage setup for both programs: MULT_LITE simply never samples t2/t3, so MultEnd restores the same state either way.
	for (Int stage = 0; stage < 4; stage++) {
		device->SetTexture(stage, t[stage]);
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
		DX8Wrapper::_Set_DX8_Transform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + stage), m[stage]);
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
		// cloud LINEAR/LINEAR and noise MIN POINT / MAG LINEAR, as the retail set() samples them
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MINFILTER, stage == 1 ? D3DTEXF_POINT : D3DTEXF_LINEAR);
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MIPFILTER, D3DTEXF_LINEAR);
	}
	// MULT: dest = src*dest + dest*src = 2*src*dest. MULT_LITE: dest = src*dest. Depth already written by the base pass from the
	// same vertices.
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE, TRUE);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND, D3DBLEND_DESTCOLOR);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND, lite ? D3DBLEND_ZERO : D3DBLEND_SRCCOLOR);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHATESTENABLE, FALSE);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ZWRITEENABLE, FALSE);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	if (FAILED(device->SetPixelShader(ps))) {
		s_gxgDevFailed = true;
		g_gxGroundEngaged = s_gxgOn && !s_gxgBakeFailed && !s_gxgDevFailed;	// GXGroundSteps.h: the governor's availability
		GXGround_MultEnd();
		return FALSE;
	}
	// c0, c1 serve both programs; c2-c4 only MULT's detail and macro layers.
	const float c0[4] = { p->noise, p->noise, p->noise, p->noise }, c1[4] = { 1, 1, 1, 1 };
	const float c2[4] = { p->detail, p->detail, p->detail, p->detail }, c3[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
	const float c4[4] = { p->macro, p->macro, p->macro, p->macro };
	device->SetPixelShaderConstant(0, c0, 1);
	device->SetPixelShaderConstant(1, c1, 1);
	device->SetPixelShaderConstant(2, c2, 1);
	device->SetPixelShaderConstant(3, c3, 1);
	device->SetPixelShaderConstant(4, c4, 1);
	return TRUE;
}

void GXGround_MultEnd()
{
	if (!s_gxgMultSet) return;
	IDirect3DDevice8 *device = DX8Wrapper::_Get_D3D_Device8();
	if (device == nullptr) return;
	s_gxgMultSet = false;
	device->SetPixelShader(0);
	for (Int stage = 0; stage < 4; stage++) {
		device->SetTexture(stage, nullptr);
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_PASSTHRU | stage);
	}
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE, FALSE);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND, D3DBLEND_ONE);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND, D3DBLEND_ZERO);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ZWRITEENABLE, TRUE);
	ShaderClass::Invalidate();
	DX8Wrapper::Invalidate_Cached_Render_States();
}

// Called from W3DShaderManager::shutdown(), which the engine runs while the device is still alive, before a reset
// (BaseHeightMapRenderObjClass::ReleaseResources) and at display shutdown - the same place the terrain's own .pso handles go.
// Every device object goes; the next base pass rebuilds them and re-bakes the shade atlas from the kept luminance (logged at=draw).
void GXGround_ReleaseDeviceObjects()
{
	// Nothing was ever built on a device (the ground is off): no device call at all, as before the ground existed.
	IDirect3DDevice8 *device = s_gxgDev != nullptr ? DX8Wrapper::_Get_D3D_Device8() : nullptr;
	if (device != nullptr && device == s_gxgDev) {
		if (s_gxgPSBase != 0) device->DeletePixelShader(s_gxgPSBase);
		if (s_gxgPSMult != 0) device->DeletePixelShader(s_gxgPSMult);
		if (s_gxgPSMultLite != 0) device->DeletePixelShader(s_gxgPSMultLite);
		gxgReleaseShade();
		if (s_gxgDetail != nullptr) s_gxgDetail->Release();
		if (s_gxgMacro != nullptr) s_gxgMacro->Release();
		if (s_gxgWhite != nullptr) s_gxgWhite->Release();
	}
	s_gxgPSBase = s_gxgPSMult = s_gxgPSMultLite = 0;
	s_gxgShade = s_gxgDetail = s_gxgMacro = s_gxgWhite = nullptr;
	s_gxgDev = nullptr;
	s_gxgDevFailed = false;
	g_gxGroundEngaged = s_gxgOn && !s_gxgBakeFailed && !s_gxgDevFailed;	// GXGroundSteps.h: the governor's availability
	s_gxgBakeFailed = false;
	g_gxGroundEngaged = s_gxgOn && !s_gxgBakeFailed && !s_gxgDevFailed;	// GXGroundSteps.h: the governor's availability
	s_gxgBaseActive = false;
	s_gxgMultSet = false;
}

// update(), at map load: this map's switch (one [GXGROUND] line), then - when on - the luminance of the colour atlas just written
// (32-bit B G R A rows; bits = nullptr for the 16-bit atlas, which the ground cannot read) and its class blocks, for the bakes.
static void gxgKeepAtlas(const UnsignedByte *bits, Int width, Int height, Int pitch, const void *owner, const std::vector<GXGroundClassRect> &classes)
{
	gxgForgetAtlas();
	gxgDecideForMap();
	s_gxgOn = false;
	g_gxGroundEngaged = s_gxgOn && !s_gxgBakeFailed && !s_gxgDevFailed;	// GXGroundSteps.h: the governor's availability
	switch (s_gxgWhy) {
	case GXGROUND_OFF_KEY:		fprintf(stderr, "[GXGROUND] mode off (Options.ini GXGround=no)\n"); break;
	case GXGROUND_OFF_ENV:		fprintf(stderr, "[GXGROUND] mode off (GX_GROUND=off)\n"); break;
	case GXGROUND_OFF_HEADLESS:	fprintf(stderr, "[GXGROUND] off: headless\n"); break;
	default: break;
	}
	fflush(stderr);
	if (!gxGroundIsOn(s_gxgWhy)) return;
	if (bits == nullptr || width < 16 || height < 16) {
		fprintf(stderr, "[GXGROUND] off: the terrain atlas is %dx%d %s, the ground needs a 32-bit one\n", width, height, bits == nullptr ? "16-bit" : "32-bit");
		fflush(stderr);
		return;
	}
	s_gxgLum.resize((size_t)width * height);
	for (Int y = 0; y < height; y++) {
		const UnsignedByte *p = bits + (size_t)y * pitch;
		for (Int x = 0; x < width; x++, p += 4) {
			s_gxgLum[(size_t)y * width + x] = (UnsignedByte)((114 * p[0] + 587 * p[1] + 299 * p[2] + 500) / 1000);
		}
	}
	s_gxgW = width;
	s_gxgH = height;
	s_gxgClasses = classes;
	s_gxgOwner = owner;
	s_gxgOn = true;
	g_gxGroundEngaged = s_gxgOn && !s_gxgBakeFailed && !s_gxgDevFailed;	// GXGroundSteps.h: the governor's availability
	s_gxgLastMode = -1;
	const int *q = s_gxgP[0].pct;
	fprintf(stderr, "[GXGROUND] mode on (%s) shade=%d blend=%d detail=%d macro=%d noise=%d atlas=%dx%d classes=%d; shade bake at map load\n",
		s_gxgWhy == GXGROUND_ON_ENV ? "GX_GROUND" : "Options.ini GXGround (yes, or absent: the default)", q[0], q[1], q[2], q[3], q[4], width, height, (int)classes.size());
	fflush(stderr);
}

/******************************************************************************
						TerrainTextureClass
******************************************************************************/
//-----------------------------------------------------------------------------
//         Public Functions
//-----------------------------------------------------------------------------

//=============================================================================
// TerrainTextureClass::TerrainTextureClass
//=============================================================================
/** Constructor. Calls parent constructor to create a 32 bit per pixel D3D
texture of the desired height and mip level. */
//=============================================================================
// GeneralsX: The tile atlas was A1R5G5B5, so every map's 8-bit-per-channel ground art
// was stored at 5 bits per channel (visible as banding on smooth snow and sand). A8R8G8B8 costs 2x the atlas
// bytes: 2048 x height x 4 with 3 mips, e.g. 10.5 MiB instead of 5.25 at height 1024 (DXVK keeps a second,
// staging copy of a MANAGED texture, so resident memory grows by twice the difference). Load-time only.
//
// Built exactly as before (16-bit), then upgraded: the 32-bit atlas is created with a call that cannot assert, and
// replaces the 16-bit one only when the device supports the format, the allocation succeeds and the surface really is
// 32-bit. Anything else keeps the atlas that always fit - never a substituted format update() cannot write, and never
// a missing texture or a debug assertion on a map too big for the doubled atlas.
TerrainTextureClass::TerrainTextureClass(int height) :
	TextureClass(TEXTURE_WIDTH, height,
		WW3D_FORMAT_A1R5G5B5, MIP_LEVELS_3 )
{
	const DX8Caps *caps = DX8Wrapper::Get_Current_Caps();
	IDirect3DDevice8 *device = DX8Wrapper::_Get_D3D_Device8();
	if (device == nullptr || caps == nullptr || !caps->Support_Texture_Format(WW3D_FORMAT_A8R8G8B8)) {
		return;
	}
	IDirect3DTexture8 *tex = nullptr;
	if (FAILED(D3DXCreateTexture(device, TEXTURE_WIDTH, height, (UINT)MIP_LEVELS_3, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex)) || tex == nullptr) {
		return;
	}
	D3DSURFACE_DESC desc;
	if (SUCCEEDED(tex->GetLevelDesc(0, &desc)) && desc.Format == D3DFMT_A8R8G8B8) {
		Set_D3D_Base_Texture(tex);
	}
	tex->Release();
}

//=============================================================================
// TerrainTextureClass::TerrainTextureClass
//=============================================================================
/** Constructor. Calls parent constructor to create a 16 bit per pixel D3D
texture of the desired height and mip level. */
//=============================================================================
TerrainTextureClass::TerrainTextureClass(int height, int width) :
	TextureClass(width, height,
		WW3D_FORMAT_A1R5G5B5, MIP_LEVELS_ALL )
{
}


//=============================================================================
// TerrainTextureClass::update
//=============================================================================
/** Sets the tile bitmap data into the texture.  The tiles are placed with 4
	pixel borders around them, so that when the tiles are scaled and bilinearly
	interpolated, you don't get seams between the tiles.  */
//=============================================================================
// GeneralsX: the REMASTERED ground's shade atlas and
// luminance copy live and die with the colour atlas they were read from (see gxgKeepAtlas).
TerrainTextureClass::~TerrainTextureClass()
{
	if (s_gxgOwner == (const void *)this) {
		gxgForgetAtlas();
	}
}

int TerrainTextureClass::update(WorldHeightMap *htMap)
{
	// D3DTexture is our texture;
	// GeneralsX: drop the REMASTERED ground's copy first -
	// every early return leaves none, so a map never draws another map's shade (on the relief prototype).
	gxgForgetAtlas();

	IDirect3DSurface8 *surface_level;
	D3DSURFACE_DESC surface_desc;
	D3DLOCKED_RECT locked_rect;
	DX8_ErrorCode(Peek_D3D_Texture()->GetSurfaceLevel(0, &surface_level));
	DX8_ErrorCode(surface_level->GetDesc(&surface_desc));
	if (surface_desc.Width < TEXTURE_WIDTH) {
		surface_level->Release();
		return 0;
	}

	DX8_ErrorCode(surface_level->LockRect(&locked_rect, nullptr, 0));

	Int tilePixelExtent = TILE_PIXEL_EXTENT;
	Int tilesPerRow = surface_desc.Width/(2*TILE_PIXEL_EXTENT+TILE_OFFSET);
	tilesPerRow *= 2;
//	Int numRows = surface_desc.Height/(tilePixelExtent+TILE_OFFSET);
#ifdef RTS_DEBUG
	//DEBUG_ASSERTCRASH(tilesPerRow*numRows >= htMap->m_numBitmapTiles, ("Too many tiles."));
	DEBUG_ASSERTCRASH((Int)surface_desc.Width >= tilePixelExtent*tilesPerRow, ("Bitmap too small."));
#endif
	const Bool is32Bit = (surface_desc.Format == D3DFMT_A8R8G8B8 || surface_desc.Format == D3DFMT_X8R8G8B8);
	if (surface_desc.Format == D3DFMT_A1R5G5B5 || is32Bit) {
#if 0
		UnsignedInt cellX, cellY;
		for (cellX = 0; cellX < surface_desc.Width; cellX++) {
			for (cellY = 0; cellY < surface_desc.Height; cellY++) {
				UnsignedByte *pBGR = ((UnsignedByte *)locked_rect.pBits)+(cellY*surface_desc.Width+cellX)*2;
				*((Short*)pBGR) = (((255-2*cellY)>>3)<<10) + ((4*cellX)>>4);
			}
		}
#endif
		Int tileNdx;
		Int pixelBytes = is32Bit ? 4 : 2;
		for (tileNdx=0; tileNdx < htMap->m_numBitmapTiles; tileNdx++) {
			TileData *pTile = htMap->getSourceTile(tileNdx);
			if (!pTile) continue;
			ICoord2D position = pTile->m_tileLocationInTexture;
			if (position.x<=0) continue; // all real tile offsets start at 2.  jba.

			Int i,j;
			for (j=0; j<tilePixelExtent; j++) {
				UnsignedByte *pBGR = pTile->getRGBDataForWidth(tilePixelExtent);
				pBGR += (tilePixelExtent-1-j)*TILE_BYTES_PER_PIXEL*tilePixelExtent; // invert to match.
				Int row = position.y+j;
				UnsignedByte *pBGRX = ((UnsignedByte*)locked_rect.pBits) +
							(row)*surface_desc.Width*pixelBytes;

				Int column = position.x;
				pBGRX += column*pixelBytes;
				for (i=0; i<tilePixelExtent; i++) {
					if (is32Bit) {
						// Alpha is forced opaque, exactly as the 16-bit pack forces its A bit (0x8000): TileData keeps the
						// source TGA's own alpha, most 32-bit stock terrain TGAs carry alpha 0, and the blend pass (which
						// shares this texture) multiplies by texture alpha - a straight copy makes those blend edges vanish.
						pBGRX[0] = pBGR[0];
						pBGRX[1] = pBGR[1];
						pBGRX[2] = pBGR[2];
						pBGRX[3] = 0xff;
					} else {
						*((Short*)pBGRX) = 0x8000 + ((pBGR[2]>>3)<<10) + ((pBGR[1]>>3)<<5) + (pBGR[0]>>3);
					}
					pBGRX +=pixelBytes;
					pBGR +=TILE_BYTES_PER_PIXEL;
				}
			}
		}
		// Now draw the 4 pixel border around each tile class.
		Int texClass;
		for (texClass=0; texClass<htMap->m_numTextureClasses; texClass++) {
			Int width = htMap->m_textureClasses[texClass].width;
			ICoord2D origin = htMap->m_textureClasses[texClass].positionInTexture;
			if (origin.x<=0) continue;
			width *= TILE_PIXEL_EXTENT;
			// Duplicate 4 columns of pixels before and after.
			Int j;
			for (j=0; j<width; j++) {
				Int row = origin.y+j;
				UnsignedByte *pBGRX = ((UnsignedByte*)locked_rect.pBits) +
							(row)*surface_desc.Width*pixelBytes;

				Int column = origin.x;
				pBGRX += column*pixelBytes;
				// copy before
				memcpy(pBGRX-(4)*pixelBytes, pBGRX+(width-4)*pixelBytes, 4*pixelBytes);
				// copy after
				memcpy(pBGRX+(width*pixelBytes), pBGRX, 4*pixelBytes);
			}

			// Duplicate 4 rows of pixels before and after.
			for (j=0; j<4; j++) {
				// copy before.
				Int row = origin.y-j-1;
				UnsignedByte *pBGRX = ((UnsignedByte*)locked_rect.pBits) +
							(row)*surface_desc.Width*pixelBytes;
				UnsignedByte *target = pBGRX+(origin.x-4)*pixelBytes;
				memcpy(target, target+width*surface_desc.Width*pixelBytes, (width+8)*pixelBytes);
				// copy after.
				row = origin.y+j;
				pBGRX = ((UnsignedByte*)locked_rect.pBits) +
							(row)*surface_desc.Width*pixelBytes;
				target = pBGRX+(origin.x-4)*pixelBytes;
				memcpy(target+width*surface_desc.Width*pixelBytes, target, (width+8)*pixelBytes);
			}

		}

	}

	// GeneralsX: [GXATLAS], with GX_ATLAS set: read the level-0 tiles back and compare them
	// with their source art, once per atlas build (map load). maxerr is the largest per-channel difference (0 when the
	// atlas holds the art exactly, up to 7 at 5 bits per channel); translucent counts texels whose alpha is not opaque,
	// which must be 0 because the blend pass multiplies by it.
	static const Bool s_atlasTrace = (getenv("GX_ATLAS") != nullptr);
	if (s_atlasTrace && (surface_desc.Format == D3DFMT_A1R5G5B5 || is32Bit)) {
		const Int pixelBytes = is32Bit ? 4 : 2;
		Int texels = 0, errTexels = 0, maxErr = 0, translucent = 0;
		for (Int tileNdx = 0; tileNdx < htMap->m_numBitmapTiles; tileNdx++) {
			TileData *pTile = htMap->getSourceTile(tileNdx);
			if (!pTile || pTile->m_tileLocationInTexture.x <= 0) continue;
			const ICoord2D position = pTile->m_tileLocationInTexture;
			for (Int j = 0; j < tilePixelExtent; j++) {
				const UnsignedByte *src = pTile->getRGBDataForWidth(tilePixelExtent) + (tilePixelExtent-1-j)*TILE_BYTES_PER_PIXEL*tilePixelExtent;
				const UnsignedByte *dst = ((UnsignedByte*)locked_rect.pBits) + ((position.y+j)*surface_desc.Width + position.x)*pixelBytes;
				for (Int i = 0; i < tilePixelExtent; i++, src += TILE_BYTES_PER_PIXEL, dst += pixelBytes) {
					Int bgr[3];
					if (is32Bit) {
						bgr[0] = dst[0]; bgr[1] = dst[1]; bgr[2] = dst[2];
						if (dst[3] != 0xff) translucent++;
					} else {
						const UnsignedShort v = *(const UnsignedShort *)dst;
						const Int b5 = v & 31, g5 = (v >> 5) & 31, r5 = (v >> 10) & 31;
						bgr[0] = (b5 << 3) | (b5 >> 2); bgr[1] = (g5 << 3) | (g5 >> 2); bgr[2] = (r5 << 3) | (r5 >> 2);
						if (!(v & 0x8000)) translucent++;
					}
					Int e = 0;
					for (Int c = 0; c < 3; c++) {
						const Int d = bgr[c] > src[c] ? bgr[c] - src[c] : src[c] - bgr[c];
						if (d > e) e = d;
					}
					if (e > maxErr) maxErr = e;
					if (e) errTexels++;
					texels++;
				}
			}
		}
		fprintf(stderr, "[GXATLAS] %ux%u %s tiles=%d texels=%d maxerr=%d errtexels=%d translucent=%d\n",
			(unsigned)surface_desc.Width, (unsigned)surface_desc.Height, is32Bit ? "32-bit" : "A1R5G5B5",
			htMap->m_numBitmapTiles, texels, maxErr, errTexels, translucent);
		fflush(stderr);
	}
	// GeneralsX: this map's REMASTERED ground switch, and -
	// when on - the level-0 texels its shade atlas is baked from (at map load, once the map's lights are final: GXGround_Prepare).
	// The 16-bit atlas has no 8-bit texels to read: the ground stays off there and says so.
	{
		std::vector<GXGroundClassRect> classes;
		if (is32Bit) {
			for (Int texClass = 0; texClass < htMap->m_numTextureClasses; texClass++) {
				const ICoord2D origin = htMap->m_textureClasses[texClass].positionInTexture;
				if (origin.x <= 0) continue;
				classes.push_back({ origin.x, origin.y, htMap->m_textureClasses[texClass].width * TILE_PIXEL_EXTENT });
			}
		}
		gxgKeepAtlas(is32Bit ? (const UnsignedByte*)locked_rect.pBits : nullptr, (Int)surface_desc.Width, (Int)surface_desc.Height,
			(Int)locked_rect.Pitch, this, classes);
	}
	surface_level->UnlockRect();
	surface_level->Release();
	DX8_ErrorCode(D3DXFilterTexture(Peek_D3D_Texture(), nullptr, 0, D3DX_FILTER_BOX));
	if (WW3D::Get_Texture_Reduction()) {
		Peek_D3D_Texture()->SetLOD(WW3D::Get_Texture_Reduction());
	}
	return(surface_desc.Height);
}

#if 0 // old version.
//=============================================================================
// TerrainTextureClass::update
//=============================================================================
/** Sets the tile bitmap data into the texture.  The tiles are placed with 4
	pixel borders around them, so that when the tiles are scaled and bilinearly
	interpolated, you don't get seams between the tiles.  */
//=============================================================================
int TerrainTextureClass::update(WorldHeightMap *htMap)
{
	// D3DTexture is our texture;

	IDirect3DSurface8 *surface_level;
	D3DSURFACE_DESC surface_desc;
	D3DLOCKED_RECT locked_rect;
	DX8_ErrorCode(D3DTexture->GetSurfaceLevel(0, &surface_level));
	DX8_ErrorCode(surface_level->GetDesc(&surface_desc));
	if (surface_desc.Width < TEXTURE_WIDTH) {
		surface_level->Release();
		if (surface_desc.Width == 256) {
			return update256(htMap);
		}
		return false;
	}

	DX8_ErrorCode(surface_level->LockRect(&locked_rect, nullptr, 0));

	Int tilePixelExtent = TILE_PIXEL_EXTENT;
	Int tilesPerRow = surface_desc.Width/(2*TILE_PIXEL_EXTENT+TILE_OFFSET);
	tilesPerRow *= 2;
	Int numRows = surface_desc.Height/(tilePixelExtent+TILE_OFFSET);
#ifdef RTS_DEBUG
	assert(tilesPerRow*numRows >= htMap->m_numBitmapTiles);
	assert((Int)surface_desc.Width >= tilePixelExtent*tilesPerRow);
#endif
	if (surface_desc.Format == D3DFMT_A1R5G5B5) {
		Int cellX, cellY;
#if 0
		for (cellX = 0; cellX < surface_desc.Width; cellX++) {
			for (cellY = 0; cellY < surface_desc.Height; cellY++) {
				UnsignedByte *pBGR = ((UnsignedByte *)locked_rect.pBits)+(cellY*surface_desc.Width+cellX)*2;
				*((Short*)pBGR) = (((255-2*cellY)>>3)<<10) + ((4*cellX)>>4);
			}
		}
#endif
		Int pixelBytes = 2;
		for (cellY = 0; cellY < numRows; cellY++) {
			for (cellX = 0; cellX < tilesPerRow; cellX++) {
				Int tileNdx = cellX/2 + (tilesPerRow/2)*(cellY/2);
				tileNdx *=4;
				if (cellX&1) tileNdx++;
				if (!(cellY&1)) tileNdx += 2;
#define ADD_EXTRA_TILES 1
#if ADD_EXTRA_TILES // Fills in an extra 2 columns and 1 row of tiles if there is room.
				if (!htMap->getSourceTile(tileNdx) && htMap->getSourceTile(tileNdx-4)) {
					tileNdx -= 4;
				}
				if (!htMap->getSourceTile(tileNdx) && htMap->getSourceTile(tileNdx-8)) {
					tileNdx -= 8;
				}
				if (!htMap->getSourceTile(tileNdx) && htMap->getSourceTile(tileNdx-2*tilesPerRow)) {
					tileNdx -= 2*tilesPerRow;
				}
#endif
				if (htMap->getSourceTile(tileNdx)) {
					Int i,j;
					for (j=0; j<tilePixelExtent; j++) {
						UnsignedByte *pBGR = htMap->getSourceTile(tileNdx)->getRGBDataForWidth(tilePixelExtent);
						pBGR += (tilePixelExtent-1-j)*TILE_BYTES_PER_PIXEL*tilePixelExtent; // invert to match.
						Int row = cellY*tilePixelExtent+j;
						row += TILE_OFFSET/2;
						row += TILE_OFFSET*(cellY/2);
						UnsignedByte *pBGRX = ((UnsignedByte*)locked_rect.pBits) +
									(row)*surface_desc.Width*pixelBytes;

						Int column = cellX*tilePixelExtent;
						column += TILE_OFFSET*(cellX/2);
						pBGRX += column*pixelBytes;
						pBGRX += (TILE_OFFSET/2)*pixelBytes;
						for (i=0; i<tilePixelExtent; i++) {
							*((Short*)pBGRX) = 0x8000 + ((pBGR[2]>>3)<<10) + ((pBGR[1]>>3)<<5) + (pBGR[0]>>3);
							pBGRX +=pixelBytes;
							pBGR +=TILE_BYTES_PER_PIXEL;
						}
					}

				}
			}

		}


		for (cellY = 0; cellY < numRows; cellY++) {
			for (cellX = 0; cellX < tilesPerRow; cellX++) {
			// Duplicate 4 rows of pixels before and after.
				Int j;
				for (j=0; j<tilePixelExtent; j++) {
					Int row = cellY*tilePixelExtent+j;
					row += TILE_OFFSET/2;
					row += TILE_OFFSET*(cellY/2);
					UnsignedByte *pBGRX = ((UnsignedByte*)locked_rect.pBits) +
								(row)*surface_desc.Width*pixelBytes;

					Int column = cellX*tilePixelExtent;
					column += TILE_OFFSET*(cellX/2);
					pBGRX += column*pixelBytes;
					if (cellX&1) {
						Int bytesToCopy = 4*pixelBytes;
						if (cellX == tilesPerRow-1) {
							// last cell, so fill to the end of the texture.
							Int usedWidth = tilesPerRow*tilePixelExtent + TILE_OFFSET*(tilesPerRow/2);
							usedWidth *= pixelBytes;
							bytesToCopy += surface_desc.Width*pixelBytes - usedWidth;
						}
						// Copy after.
						pBGRX -= tilePixelExtent*pixelBytes;
						pBGRX += (TILE_OFFSET/2)*pixelBytes;
						memcpy(pBGRX+(2*tilePixelExtent*pixelBytes), pBGRX, bytesToCopy);
					} else {
						// copy before
						memcpy(pBGRX, pBGRX+(2*tilePixelExtent)*pixelBytes, 4*pixelBytes);
					}
				}
			}
		}
 		for (cellY = 0; cellY < numRows; cellY++) {
			Int rowBytes = surface_desc.Width*pixelBytes;
			Int row = cellY*tilePixelExtent;
			row += TILE_OFFSET/2;
			row += TILE_OFFSET*(cellY/2);
			if ( (cellY&1) == 0) {
				UnsignedByte *pBGRX = ((UnsignedByte*)locked_rect.pBits) +
							(row)*rowBytes;
				row += 2*tilePixelExtent;
				UnsignedByte *pBase = ((UnsignedByte*)locked_rect.pBits) +
							(row)*rowBytes;
				memcpy(pBGRX-4*rowBytes, pBase-4*rowBytes, 4*rowBytes);
			} else {
				UnsignedByte *pBase = ((UnsignedByte*)locked_rect.pBits) +
							(row-tilePixelExtent)*rowBytes;
				row += tilePixelExtent;
				UnsignedByte *pBGRX = ((UnsignedByte*)locked_rect.pBits) +
							(row)*rowBytes;
				memcpy(pBGRX, pBase, 4*rowBytes);
			}
		}

	}
	surface_level->UnlockRect();
	surface_level->Release();
	DX8_ErrorCode(D3DXFilterTexture(D3DTexture, nullptr, 0, D3DX_FILTER_BOX));
	return(surface_desc.Height);
}
#endif

//=============================================================================
// TerrainTextureClass::setLOD
//=============================================================================
/** Sets the lod of the texture to be loaded into the video card.  */
//=============================================================================
void TerrainTextureClass::setLOD(Int LOD)
{
	if (Peek_D3D_Texture()) Peek_D3D_Texture()->SetLOD(LOD);
}
//=============================================================================
// TerrainTextureClass::update
//=============================================================================
/** Sets the tile bitmap data into the texture.  The tiles are placed with 4
	pixel borders around them, so that when the tiles are scaled and bilinearly
	interpolated, you don't get seams between the tiles.  */
//=============================================================================
Bool TerrainTextureClass::updateFlat(WorldHeightMap *htMap, Int xCell, Int yCell, Int cellWidth, Int pixelsPerCell)
{
	// D3DTexture is our texture;

	IDirect3DSurface8 *surface_level;
	D3DSURFACE_DESC surface_desc;
	D3DLOCKED_RECT locked_rect;
	DX8_ErrorCode(Peek_D3D_Texture()->GetSurfaceLevel(0, &surface_level));
	DX8_ErrorCode(surface_level->GetDesc(&surface_desc));
	DEBUG_ASSERTCRASH((Int)surface_desc.Width == cellWidth*pixelsPerCell, ("Bitmap too small."));
	DEBUG_ASSERTCRASH((Int)surface_desc.Height == cellWidth*pixelsPerCell, ("Bitmap too small."));
	if (surface_desc.Width != cellWidth*pixelsPerCell) {
		return false;
	}

	DX8_ErrorCode(surface_level->LockRect(&locked_rect, nullptr, 0));


	if (surface_desc.Format == D3DFMT_A1R5G5B5) {

		Int pixelBytes = 2;
		Int cellX, cellY;
#if 0
		UnsignedInt X, Y;
		for (X = 0; X < surface_desc.Width; X++) {
			for (Y = 0; Y < surface_desc.Height; Y++) {
				UnsignedByte *pBGR = ((UnsignedByte *)locked_rect.pBits)+(Y*surface_desc.Width+X)*pixelBytes;
				*((Short*)pBGR) = (((255-2*Y)>>3)<<10) + ((2*X)>>4);
			}
		}
#endif
		for (cellX = 0; cellX < cellWidth; cellX++) {
			for (cellY = 0; cellY < cellWidth; cellY++) {
				UnsignedByte *pBGRX_data = ((UnsignedByte*)locked_rect.pBits);
				UnsignedByte *pBGR = htMap->getPointerToTileData(xCell+cellX, yCell+cellY, pixelsPerCell);
				if (pBGR == nullptr) continue; // past end of defined terrain. [3/24/2003]
				Int k, l;
				for (k=pixelsPerCell-1; k>=0; k--) {
					UnsignedByte *pBGRX = pBGRX_data + (pixelsPerCell*(cellWidth-cellY-1)+k)*surface_desc.Width*pixelBytes +
						cellX*pixelsPerCell*pixelBytes;
					for (l=0; l<pixelsPerCell; l++) {
						*((Short*)pBGRX) = 0x8000 + ((pBGR[2]>>3)<<10) + ((pBGR[1]>>3)<<5) + (pBGR[0]>>3);
						pBGRX +=pixelBytes;
						pBGR +=TILE_BYTES_PER_PIXEL;
					}
				}
			}
		}
	}

	surface_level->UnlockRect();
	surface_level->Release();
	DX8_ErrorCode(D3DXFilterTexture(Peek_D3D_Texture(), nullptr, 0, D3DX_FILTER_BOX));
	return(surface_desc.Height);
}

//=============================================================================
// TerrainTextureClass::Apply
//=============================================================================
/** Sets the texture as the current D3D texture, and does some custom setup
(standard D3D setup, but beyond the scope of W3D).  */
//=============================================================================
void TerrainTextureClass::Apply(unsigned int stage)
{
	// Do the base apply.
	TextureClass::Apply(stage);
#if 0 // obsolete [4/1/2003]
	if (TheGlobalData && (TheGlobalData->m_bilinearTerrainTex || TheGlobalData->m_trilinearTerrainTex)) {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
	} else {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MINFILTER, D3DTEXF_POINT);
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MAGFILTER, D3DTEXF_POINT);
	}
	if (TheGlobalData && TheGlobalData->m_trilinearTerrainTex) {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MIPFILTER, D3DTEXF_LINEAR);
	} else {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MIPFILTER, D3DTEXF_POINT);
	}
	// Now setup the texture pipeline.
	if (stage==0) {
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP,   D3DTOP_MODULATE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );

		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_DISABLE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXCOORDINDEX, 0 );
		DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE,false);

	}
#endif
}

/******************************************************************************
						AlphaTerrainTextureClass
******************************************************************************/
//-----------------------------------------------------------------------------
//         Public Functions
//-----------------------------------------------------------------------------

//=============================================================================
// AlphaTerrainTextureClass::AlphaTerrainTextureClass
//=============================================================================
/** Constructor. Calls parent constructor to creat a throw away 8x8 texture,
then uses the base texture's D3D texture. This way the base tiles pass, drawn
using TerrainTextureClass shares the same texture with the blended edges pass,
saving lots of texture memory, and preventing seams between blended tiles. */
//=============================================================================
AlphaTerrainTextureClass::AlphaTerrainTextureClass( TextureClass *pBaseTex ):
	TextureClass(8, 8,
		WW3D_FORMAT_A1R5G5B5, MIP_LEVELS_1 )
{
	// Attach the base texture's d3d texture.
	IDirect3DTexture8 * d3d_tex = pBaseTex->Peek_D3D_Texture();
	Set_D3D_Base_Texture(d3d_tex);
}


//=============================================================================
// AlphaTerrainTextureClass::Apply
//=============================================================================
/** Sets the texture as the current D3D texture, and does some custom setup.
This may be applied in either single pass, as the second texture in the pipe,
or multipass.  If stage==0, we are doing multipass and we set up the pipe
for a single texture.  If stage==1, then we are doing a single pass, and we
set up the pipe so that we blend onto the base texture in stage 0.
(standard D3D setup, but beyond the scope of W3D). */
//=============================================================================
void AlphaTerrainTextureClass::Apply(unsigned int stage)
{
	// Do the base apply.
	TextureClass::Apply(stage);

	// Set the bilinear or trilinear filtering.
	if (TheGlobalData && (TheGlobalData->m_bilinearTerrainTex || TheGlobalData->m_trilinearTerrainTex)) {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
	} else {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MINFILTER, D3DTEXF_POINT);
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MAGFILTER, D3DTEXF_POINT);
	}
	if (TheGlobalData && TheGlobalData->m_trilinearTerrainTex) {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MIPFILTER, D3DTEXF_LINEAR);
	} else {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MIPFILTER, D3DTEXF_POINT);
	}
	// Since we are using multiple distinct tiles, the textures doesn't wrap, so clamp it.
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
	// Now setup the texture pipeline.
	if (stage==0) {
		// Modulate the diffuse color with the texture as lighting comes from diffuse.
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP,   D3DTOP_MODULATE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_MODULATE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXCOORDINDEX, 1 );
		// Blend the result using the alpha. (came from diffuse mod texture)
		DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE,true);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
		// Disable stage 2.
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_DISABLE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );
	}	else if (stage==1) {

		if (TheGlobalData && !TheGlobalData->m_multiPassTerrain)
		{
			///@todo: Remove 8-Stage Nvidia hack after drivers are fixed.
			//This method is a backdoor specific to Nvidia based cards.  It will fail on
			//other hardware.  Allows single pass blend of 2 textures and post modulate diffuse.
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXCOORDINDEX, 0);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_MODULATE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP, D3DTOP_ADD);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_TEXCOORDINDEX, 1);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG1, D3DTA_DIFFUSE | D3DTA_COMPLEMENT | D3DTA_ALPHAREPLICATE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_ADD);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAARG1, D3DTA_TFACTOR | D3DTA_COMPLEMENT);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);

			DX8Wrapper::Set_DX8_Texture(2, nullptr);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_COLOROP, D3DTOP_MODULATE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_TEXCOORDINDEX, 2);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_COLORARG1, D3DTA_TEXTURE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_COLORARG2, D3DTA_TEXTURE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_ALPHAOP,   D3DTOP_MODULATE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);

			DX8Wrapper::Set_DX8_Texture(3, nullptr);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 3, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 3, D3DTSS_TEXCOORDINDEX, 3);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 3, D3DTSS_COLORARG1, D3DTA_DIFFUSE | 0 | D3DTA_ALPHAREPLICATE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 3, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 3, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 3, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 3, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);

			DX8Wrapper::Set_DX8_Texture(4, nullptr);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 4, D3DTSS_COLOROP, D3DTOP_MODULATE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 4, D3DTSS_TEXCOORDINDEX, 4);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 4, D3DTSS_COLORARG1, D3DTA_CURRENT);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 4, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 4, D3DTSS_ALPHAOP,   D3DTOP_MODULATE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 4, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 4, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

			DX8Wrapper::Set_DX8_Texture(5, nullptr);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 5, D3DTSS_COLOROP, D3DTOP_ADD);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 5, D3DTSS_TEXCOORDINDEX, 5);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 5, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 5, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 5, D3DTSS_ALPHAOP,   D3DTOP_ADD);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 5, D3DTSS_ALPHAARG1, D3DTA_TFACTOR | D3DTA_COMPLEMENT);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 5, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);

			DX8Wrapper::Set_DX8_Texture(6, nullptr);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 6, D3DTSS_COLOROP, D3DTOP_MODULATE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 6, D3DTSS_TEXCOORDINDEX, 6);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 6, D3DTSS_COLORARG1, D3DTA_TFACTOR);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 6, D3DTSS_COLORARG2, D3DTA_TFACTOR);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 6, D3DTSS_ALPHAOP,   D3DTOP_MODULATE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 6, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 6, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);

			DX8Wrapper::Set_DX8_Texture(7, nullptr);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 7, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 7, D3DTSS_TEXCOORDINDEX, 7);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 7, D3DTSS_COLORARG1, D3DTA_TFACTOR);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 7, D3DTSS_COLORARG2, D3DTA_TFACTOR);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 7, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 7, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 7, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
		}
		else
		{
  			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1 );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1 );

			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG1, D3DTA_TEXTURE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_MODULATE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG2, D3DTA_CURRENT );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1 );
		}
	}
}


/******************************************************************************
						LightMapTerrainTextureClass
******************************************************************************/
//-----------------------------------------------------------------------------
//         Public Functions
//-----------------------------------------------------------------------------

//=============================================================================
// LightMapTerrainTextureClass::LightMapTerrainTextureClass
//=============================================================================
/** Constructor. Calls parent constructor to load the .tga texture. */
//=============================================================================
LightMapTerrainTextureClass::LightMapTerrainTextureClass(AsciiString name, MipCountType mipLevelCount) :
TextureClass(name.isEmpty()?"TSNoiseUrb.tga":name.str(),name.isEmpty()?"TSNoiseUrb.tga":name.str(), mipLevelCount )
{
	Get_Filter().Set_Min_Filter(TextureFilterClass::FILTER_TYPE_BEST);
	Get_Filter().Set_Mag_Filter(TextureFilterClass::FILTER_TYPE_BEST);
	Get_Filter().Set_U_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_REPEAT);
	Get_Filter().Set_V_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_REPEAT);
}

#define STRETCH_FACTOR ((float)(1/(63.0*MAP_XY_FACTOR/2))) /* covers 63/2 tiles */

//=============================================================================
// LightMapTerrainTextureClass::Apply
//=============================================================================
/** Sets the texture as the current D3D texture, and does some custom setup.
The LightMapTerrainTextureClass may be applied by itself, or with the
CloudMapTerrainTextureClass.  This may be applied in either single pass,
as the second texture in the pipe,
or multipass.  If stage==0, we are doing multipass and we set up the pipe
for a single texture.  If stage==1, then we are doing a single pass, and we
set up the pipe so that we blend onto the cloud map texture in stage 0.
Also, texture is mapped using the x/y coordinates of the map, saving us
yet another set of uv coordinates.
(standard D3D setup, but beyond the scope of W3D). */
//=============================================================================
void LightMapTerrainTextureClass::Apply(unsigned int stage)
{
	TextureClass::Apply(stage);
#if 0 // obsolete [4/1/2003]
	// Do the base apply.
	/* previous setup */
	if (TheGlobalData && TheGlobalData->m_trilinearTerrainTex) {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MIPFILTER, D3DTEXF_LINEAR);
	} else {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MIPFILTER, D3DTEXF_POINT);
	}

	DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MINFILTER, D3DTEXF_POINT);
	DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);

	// Disable 3rd stage just in case.
	DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_COLOROP,   D3DTOP_DISABLE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );

	// Now setup the texture pipeline.
	DX8Wrapper::Set_DX8_Texture_Stage_State( stage, D3DTSS_COLORARG1, D3DTA_TEXTURE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( stage, D3DTSS_COLORARG2, D3DTA_CURRENT );
	if (stage == 0) {
		DX8Wrapper::Set_DX8_Texture_Stage_State( stage, D3DTSS_COLOROP,   D3DTOP_SELECTARG1 );
		//Disable second stage
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_DISABLE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );
	} else {
		DX8Wrapper::Set_DX8_Texture_Stage_State( stage, D3DTSS_COLOROP,   D3DTOP_MODULATE );
	}
	DX8Wrapper::Set_DX8_Texture_Stage_State( stage, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );
	DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
	// Two output coordinates are used.
	DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);


	DX8Wrapper::Set_DX8_Texture_Stage_State( stage, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
	DX8Wrapper::Set_DX8_Texture_Stage_State( stage, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);

	D3DXMATRIX curView;
	DX8Wrapper::_Get_DX8_Transform(D3DTS_VIEW, curView);

	D3DXMATRIX inv;
	float det;
	D3DXMatrixInverse(&inv, &det, &curView);

	D3DXMATRIX scale;
	D3DXMatrixScaling(&scale, STRETCH_FACTOR, STRETCH_FACTOR,1);
	inv *=scale;
	if (stage==0) {
		DX8Wrapper::_Set_DX8_Transform(D3DTS_TEXTURE0, inv);
	}	if (stage==1) {
		DX8Wrapper::_Set_DX8_Transform(D3DTS_TEXTURE1, inv);
	}


	if (stage==0) {
		DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE,true);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND,D3DBLEND_DESTCOLOR);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND,D3DBLEND_ZERO);
	}
#endif
}









/******************************************************************************
						AlphaEdgeTextureClass
******************************************************************************/
//-----------------------------------------------------------------------------
//         Public Functions
//-----------------------------------------------------------------------------

/**
* AlphaEdgeTextureClass - Generates the alpha edge blending for terrain.
*
*/
AlphaEdgeTextureClass::AlphaEdgeTextureClass( int height, MipCountType mipLevelCount) :
//	TextureClass("EdgingTemplate.tga","EdgingTemplate.tga", mipLevelCount )
	TextureClass(TEXTURE_WIDTH, height, WW3D_FORMAT_A8R8G8B8, mipLevelCount )
{

}

int AlphaEdgeTextureClass::update256(WorldHeightMap *htMap)
{
	return 1;
}

int AlphaEdgeTextureClass::update(WorldHeightMap *htMap)
{
	// D3DTexture is our texture;

	IDirect3DSurface8 *surface_level;
	D3DSURFACE_DESC surface_desc;
	D3DLOCKED_RECT locked_rect;
	DX8_ErrorCode(Peek_D3D_Texture()->GetSurfaceLevel(0, &surface_level));
	DX8_ErrorCode(surface_level->LockRect(&locked_rect, nullptr, 0));
	DX8_ErrorCode(surface_level->GetDesc(&surface_desc));

	Int tilePixelExtent = TILE_PIXEL_EXTENT; // blend tiles are 1/4 tiles.
//	Int tilesPerRow = surface_desc.Width / (tilePixelExtent+8);

//	Int numRows = surface_desc.Height/(tilePixelExtent+8);

	if (surface_desc.Format == D3DFMT_A8R8G8B8) {
#if 1
#if 1
		Int cellX, cellY;
		for (cellX = 0; (UnsignedInt)cellX < surface_desc.Width; cellX++) {
			for (cellY = 0; cellY < surface_desc.Height; cellY++) {
				UnsignedByte *pBGR = ((UnsignedByte *)locked_rect.pBits)+(cellY*surface_desc.Width+cellX)*4;
				pBGR[2] = 255-cellY/2;
				pBGR[0] = cellX/2;
				pBGR[3] = cellX/2;  // alpha.
				pBGR[3] = 128;  // alpha.
			}
		}
#endif
#if 1
		Int tileNdx;
		Int pixelBytes = 4;
		for (tileNdx=0; tileNdx < htMap->m_numEdgeTiles; tileNdx++) {
			TileData *pTile = htMap->getEdgeTile(tileNdx);
			if (!pTile) continue;
			ICoord2D position = pTile->m_tileLocationInTexture;
			if (position.x<=0) continue; // all real edge offsets start at 4.  jba.
			Int i,j;
			Int column = position.x;
			for (j=0; j<tilePixelExtent; j++) {
				Int row = position.y+j;
				UnsignedByte *pBGR = htMap->getEdgeTile(tileNdx)->getRGBDataForWidth(tilePixelExtent);
				pBGR += (tilePixelExtent-1-j)*TILE_BYTES_PER_PIXEL*tilePixelExtent; // invert to match.
				UnsignedByte *pBGRX = ((UnsignedByte*)locked_rect.pBits) +
							(row)*surface_desc.Width*pixelBytes;
				pBGRX += column*pixelBytes;

				for (i=0; i<tilePixelExtent; i++) {
					pBGRX[0] = pBGR[0];  //r
					pBGRX[1] = pBGR[1];	//g
					pBGRX[2] = pBGR[2];	//b
					if (pBGR[0]==0 && pBGR[1]==0 && pBGR[2]==0) {
						pBGRX[3] = 0x80;
					} else if (pBGR[0]==0xff && pBGR[1]==0xff && pBGR[2]==0xff) {
						pBGRX[3] = 0x00;
					}	else {
						pBGRX[3] = 0xff;
					}

					pBGRX += pixelBytes;
					pBGR += TILE_BYTES_PER_PIXEL;
				}
			}
		}
#endif
#endif
	}
	surface_level->UnlockRect();
	surface_level->Release();
	DX8_ErrorCode(D3DXFilterTexture(Peek_D3D_Texture(), nullptr, 0, D3DX_FILTER_BOX));
	return(surface_desc.Height);
}

void AlphaEdgeTextureClass::Apply(unsigned int stage)
{
	// Do the base apply.
	TextureClass::Apply(stage);
#if 0 // obsolete [4/1/2003]

	if (TheGlobalData && (TheGlobalData->m_bilinearTerrainTex || TheGlobalData->m_trilinearTerrainTex)) {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
	} else {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MINFILTER, D3DTEXF_POINT);
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MAGFILTER, D3DTEXF_POINT);
	}
	if (TheGlobalData && TheGlobalData->m_trilinearTerrainTex) {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MIPFILTER, D3DTEXF_LINEAR);
	} else {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MIPFILTER, D3DTEXF_POINT);
	}

	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
	// Now setup the texture pipeline.
	if (stage==0) {

		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP,   D3DTOP_MODULATE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAARG1,   D3DTA_TEXTURE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1 );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXCOORDINDEX, 1 );
		DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE,true);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);

		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_DISABLE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );

	} else if (stage==1) {
		// Drawing texture through the mask.
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAARG1,   D3DTA_CURRENT );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1 );

		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG1, D3DTA_CURRENT );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG2, D3DTA_TEXTURE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_SELECTARG1 );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAARG1,   D3DTA_CURRENT );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAARG2,   D3DTA_TEXTURE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG2 );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_TEXCOORDINDEX, 1 );
		DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE,true);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND,D3DBLEND_ONE);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND,D3DBLEND_ZERO);

	}
#endif
}


/******************************************************************************
						CloudMapTerrainTextureClass
******************************************************************************/
//-----------------------------------------------------------------------------
//         Public Functions
//-----------------------------------------------------------------------------

//=============================================================================
// CloudMapTerrainTextureClass::CloudMapTerrainTextureClass
//=============================================================================
/** Constructor. Calls parent constructor to load the .tga texture, and sets
up the "sliding" parameters for the clouds to slide over the terrain. */
//=============================================================================
//@todo - Allow adjustment of the cloud slide rate, and lose the hard coded "cloudmap.tga"
CloudMapTerrainTextureClass::CloudMapTerrainTextureClass(MipCountType mipLevelCount) :
	TextureClass("TSCloudMed.tga","TSCloudMed.tga", mipLevelCount )
{
	Get_Filter().Set_Mip_Mapping( TextureFilterClass::FILTER_TYPE_FAST );
	m_xSlidePerSecond = -0.02f;
	m_ySlidePerSecond =  1.50f * m_xSlidePerSecond;
	m_curTick = 0;
	m_xOffset = 0;
	m_yOffset = 0;

}

//=============================================================================
// CloudMapTerrainTextureClass::Apply
//=============================================================================
/** Sets the texture as the current D3D texture, and does some custom setup.
The CloudMapTerrainTextureClass may be applied by itself, or with the
LightMapTerrainTexture.  This may be applied in either single pass,
as the first texture in the pipe with LightMapTerrainTextureClass as the
second stage of the pape, or multipass.  We setup for stage 0, assuming that
we are the only texture, as LightMapTerrainTexture will adjust for multitexture
if it is applied to stage 1.
Also, texture is mapped using the x/y coordinates of the map, saving us
yet another set of uv coordinates.
(standard D3D setup, but beyond the scope of W3D). */
//=============================================================================
void CloudMapTerrainTextureClass::Apply(unsigned int stage)
{


	// Do the base apply.
	TextureClass::Apply(stage);
#if 0   // obsolete
	/* previous setup */
	if (TheGlobalData && TheGlobalData->m_trilinearTerrainTex) {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MIPFILTER, D3DTEXF_LINEAR);
	} else {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MIPFILTER, D3DTEXF_POINT);
	}

	DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
	DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);

	// Now setup the texture pipeline.
	DX8Wrapper::Set_DX8_Texture_Stage_State(stage,  D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
	// Two output coordinates are used.
	DX8Wrapper::Set_DX8_Texture_Stage_State(stage,  D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);


	DX8Wrapper::Set_DX8_Texture_Stage_State( stage,  D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
	DX8Wrapper::Set_DX8_Texture_Stage_State( stage,  D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);

	D3DXMATRIX curView;
	DX8Wrapper::_Get_DX8_Transform(D3DTS_VIEW, curView);

	D3DXMATRIX inv;
	float det;
	D3DXMatrixInverse(&inv, &det, &curView);

	D3DXMATRIX scale;
	D3DXMatrixScaling(&scale, STRETCH_FACTOR, STRETCH_FACTOR,1);
	inv *=scale;
	D3DXMATRIX offset;

	Int delta = m_curTick;
	// GeneralsX: GX_SHOT capture clock (GameClient.cpp): GetTickCount() without GX_SHOT.
	m_curTick = (Int)gxRenderTickMs();
	delta = m_curTick-delta;
	m_xOffset += m_xSlidePerSecond*delta/1000;
	m_yOffset += m_ySlidePerSecond*delta/1000;

	if (m_xOffset > 1) m_xOffset -= 1;
	if (m_yOffset > 1) m_yOffset -= 1;
	if (m_xOffset < -1) m_xOffset += 1;
	if (m_yOffset < -1) m_yOffset += 1;


	D3DXMatrixTranslation(&offset, m_xOffset, m_yOffset,0);

	inv *= offset;

	if (stage==0) {
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1 );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );

		DX8Wrapper::_Set_DX8_Transform(D3DTS_TEXTURE0, inv);

		// Disable 3rd stage just in case.
		DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_COLOROP,   D3DTOP_DISABLE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );

		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_DISABLE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );

		DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE,true);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND,D3DBLEND_DESTCOLOR);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND,D3DBLEND_ZERO);
	}	else if (stage==1) {
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG1, D3DTA_TEXTURE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG2, D3DTA_CURRENT );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_MODULATE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAARG1, D3DTA_CURRENT );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1 );

		DX8Wrapper::_Set_DX8_Transform(D3DTS_TEXTURE1, inv);
	}
#endif
}

//=============================================================================
// CloudMapTerrainTextureClass::restore
//=============================================================================
/** Cleans up any custom settings to the texturing pipeline that may not be
understood by w3d. */
//=============================================================================
void CloudMapTerrainTextureClass::restore()
{
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP,   D3DTOP_MODULATE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );

	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXCOORDINDEX, 0 );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);

	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG1, D3DTA_TEXTURE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG2, D3DTA_DIFFUSE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_MODULATE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );

	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_TEXCOORDINDEX, 0 );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE,false);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);


	if (TheGlobalData && !TheGlobalData->m_multiPassTerrain)
	{
		///@todo: Remove 8-Stage Nvidia hack after drivers are fixed.
		//This method is a backdoor specific to Nvidia based cards.  It will fail on
		//other hardware.  Allows single pass blend of 2 textures and post modulate diffuse.
		Int i;
		for (i=0; i<8; i++) {
			DX8Wrapper::Set_DX8_Texture_Stage_State( i, D3DTSS_COLOROP, D3DTOP_DISABLE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( i, D3DTSS_TEXCOORDINDEX, i);
			DX8Wrapper::Set_DX8_Texture_Stage_State( i, D3DTSS_COLORARG1, D3DTA_TEXTURE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( i, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( i, D3DTSS_ALPHAOP,   D3DTOP_DISABLE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( i, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( i, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

			DX8Wrapper::Set_DX8_Texture(i, nullptr);
		}
	}
}

/******************************************************************************
						ScorchTextureClass
******************************************************************************/
//-----------------------------------------------------------------------------
//         Public Functions
//-----------------------------------------------------------------------------

//=============================================================================
// ScorchTextureClass::ScorchTextureClass
//=============================================================================
/** Constructor. Calls parent constructor to load the .tga texture. */
//=============================================================================
/// @todo - get "EXScorch01.tga" from not hard coded location.
ScorchTextureClass::ScorchTextureClass(MipCountType mipLevelCount) :
	TextureClass("EXScorch01.tga","EXScorch01.tga", mipLevelCount )
// Hack to disable texture reduction.
//	TextureClass("EXScorch01.tga","EXScorch01.tga", mipLevelCount,WW3D_FORMAT_UNKNOWN,true,false)
{
}

//=============================================================================
// ScorchTextureClass::Apply
//=============================================================================
/** Sets the texture as the current D3D texture, and does some custom setup.
The ScorchTextureClass is applied by iteself, as it's mesh is a subset of the
terrain mesh.
(standard D3D setup, but beyond the scope of W3D). */
//=============================================================================
void ScorchTextureClass::Apply(unsigned int stage)
{
	// Do the base apply.
	TextureClass::Apply(stage);
	// Setup bilinear or trilinear filtering as specified in global data.
	if (TheGlobalData && (TheGlobalData->m_bilinearTerrainTex || TheGlobalData->m_trilinearTerrainTex)) {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
	} else {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MINFILTER, D3DTEXF_POINT);
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MAGFILTER, D3DTEXF_POINT);
	}
	if (TheGlobalData && TheGlobalData->m_trilinearTerrainTex) {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MIPFILTER, D3DTEXF_LINEAR);
	} else {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MIPFILTER, D3DTEXF_POINT);
	}

	DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
	// Now setup the texture pipeline.

	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP,   D3DTOP_MODULATE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1 );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXCOORDINDEX, 0 );
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE,true);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);

	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_DISABLE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );
}


