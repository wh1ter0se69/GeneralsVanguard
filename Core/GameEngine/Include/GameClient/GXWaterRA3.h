// GeneralsX: RA3 WATER, a client-only look option on top of clearer and live water (GXWaterClear.h,
// GXWaterLive.h).
// DEFAULT ON for every player since wh1ter0se's yes, 04/11/2026: GXWaterRA3 = no turns it off; GX_WATER_RA3=0/1 overrides the key
// and GX_WATER_RA3_TUNE=<name:value,...> tunes it per run (A/B arms, picture pieces).
//
// WHAT RA3 DOES, AND WHAT WE DO INSTEAD. EA's own Ocean.fx draws the water OPAQUE
// over a refraction copy of the scene, with two scrolling normal maps, a planar reflection and a cube map; its turquoise
// shallows are the painted sea floor seen through that copy. Our classic renderer (D3D8 through DXVK) has no mid-frame copy,
// but retail Generals already has something better for the shallows: real depth. Every terrain tile near a shore is drawn a
// second time into DESTINATION ALPHA through a 256-texel LUT indexed by t = depth / (D * M) (depth = water height - terrain
// height; BaseHeightMap::updateShorelineTile), and the water blends SRC=DESTALPHA, DEST=INVDESTALPHA. Stock D=3 / M=1 make
// that a 3-unit sliver - the hard edge in wh1ter0se's picture. This option uses the same tiles and the same depth three ways:
//
//   opacity   the dest-alpha LUT becomes a Beer-Lambert ramp, M (1 - e^(-shape u)) / (1 - e^(-shape)), u = depth / fade end,
//             and the fade ends `reach` times further out than the map's own (stock: 7.2 units instead of 3). Deep water caps at
//             `opacity` (0.9), so the sea floor reads through the shallows and is all but hidden further out.
//   tint      the same shore tiles are drawn ONCE MORE, late in the terrain render (after 3-way blends, roads, props and
//             bridges, before the terrain's shroud pass), fixed-function, RGB only, blended SRC=DESTCOLOR, DEST=SRCALPHA:
//             seabed' = seabed x (src.rgb + src.a). A second 256-texel LUT holds that multiplier per depth, from 1 at the
//             waterline to (tintR, tintG, tintB) - red absorbed, cyan up to 2x - so sand under a few units of water reads
//             turquoise. A multiplier, never an addition: a dark sea floor stays dark, a shrouded one stays black.
//   colour    the water pass's vertex colour becomes `deep`, lit by the map's own light (ambient + sun, clamped), over a
//             generated smooth surface texture instead of TWWater01's speckle (flat = 1).
//
// Composite, per pixel: deep x light x surface x a(u) + seabed x m(u) x (1 - a(u)). No new shader, no new render target, no
// new texture fetch on the water surface; the native renderer (opt-in) takes it as it is (fixed function and data only).
//
// RENDER-ONLY. Every value here feeds a LUT texel, a vertex colour or a texture; the TransparentWater override it supplies is
// read only by render code (GXWaterClear.h says which), and there is no random number - the surface texture comes from a
// fixed integer hash. This header is pure logic with no engine include and sits in Core/GameEngine/Include/GameClient,
// outside the SimID digest; never include it from Common/, GameLogic/ or GameNetwork/. The engine glue is
// W3DDevice/GameClient/Water/GXWaterRA3Glue.inl, outside every GX-SIMID region. scripts/test/look-water-ra3-test.sh checks
// the functions below, with mutant arms.

#pragma once

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------------------------------------------------------
// The look at its defaults, every value overridable per run by GX_WATER_RA3_TUNE (name:value pairs, names as below).
struct GxWaterRA3Tune
{
	float reach;		// the shore fade ends this many times further out than the map's own, in D*M*M terms (stock 3 -> 7.2:
						// Generals seas are shallow - measured 24/10 from the map files: TournamentB a flat 5 units, the
						// shell map median 2.9, Mountain Guns 8.8; a first 8x reach left the whole sea half transparent)
	float opacity;		// deep-water opacity cap (Water.ini stock 1.0; clearer water 0.6); never raised above the map's own
	float shape;		// the opacity ramp's Beer-Lambert steepness: 0 = linear (retail), 2.5 = ~85% of the cap half way out
	float tintR;		// the sea floor's colour multiplier at the end of the band, per channel, 0..2 (red absorbed ...
	float tintG;
	float tintB;		// ... green and blue lifted: sand reads turquoise)
	float tintShape;	// how fast the multiplier reaches its end value across the band (the same ramp)
	float deepR;		// the water's own colour where it is opaque, before the map's light: teal, green ~ blue, little red
						//
	float deepG;
	float deepB;
	float flat;			// >= 0.5: our smooth surface texture; below: the map's own (TWWater01)
	// PHASE 2, THE MOTION:
	float glint;		// the sun's sheen and glints over the moving normals: strength x the sun's colour (0 = none)
	float tiltMin;		// the sun's half vector is held between these angles from straight up, degrees: a sun reflected
	float tiltMax;		// straight at the camera would wash the sea white, one far off would never touch a ripple
	float sky;			// the sky's reflection, x linear Fresnel (1 - V.N, RA3's own) per vertex (0 = none)
	float skyR;			// the sky's colour before the map's light
	float skyG;
	float skyB;
	float scale;		// world units one tiling of the normal map covers (RA3: x0.0025, 400 of its units)
	float speed;		// both tilings' common drift along live water's flow, world units a second
	float div;			// and each one's own drift, + for tiling A, - for B (RA3's octave divergence): they counter-scroll
	float bump;			// the normal map's RMS slope (one tiling): how rough the ripples are
	float cells;		// the normal map's coarsest noise lattice, cells across one tiling (then 2x and 4x): its ripple size
	// PHASE 3, RIVERS (25/10; wh1ter0se approved phase 2's motion as it is, so nothing above changes):
	float river;		// >= 0.5: rivers take the deep colour, the smooth surface and the sheen too; below: retail rivers (phase 2)
	float riverOpacity;	// the river surface's alpha (retail TWWater01's is 194/255 everywhere; phase 3 kept that 0.76, phase 3b 0.90:
						// less of the bed - snow or sand already tinted cyan by the tint pass - shows through)
	float riverSpeed;	// the river tilings' common drift DOWNSTREAM, world units a second (retail's texture flows ~6-12 a second);
						// each tiling's own +-div is standing water's
	// PHASE 3b, DEEPER RIVERS (25/10; wh1ter0se on phase 3's pictures: "the light blue is too light blue ngl"). River-only:
	// the lakes and the sea (approved) read neither field.
	float riverShade;	// x the river's lit deep colour (1 = the lakes' colour, phase 3); its alpha byte (retail's sparkle weight) kept
	float riverSky;		// x `sky` for the river sheen only (the light-blue sky reflection; 1 = phase 3, 0 = the glints alone)
};

// constexpr: the glue's file-scope copy must be CONSTANT-initialised (a dynamic initialiser in W3DWater.cpp's file scope
// once made retail's zFillAlphaShader a SimID closure gap - GXWaterLive.h, 17/10).
constexpr GxWaterRA3Tune gxWaterRA3DefaultTune()
{
	return GxWaterRA3Tune{ 2.4f, 0.9f, 2.5f, 0.5f, 1.2f, 1.5f, 3.0f, 0.07f, 0.41f, 0.48f, 1.0f,
		0.60f, 40.0f, 50.0f, 0.20f, 0.55f, 0.72f, 0.88f, 180.0f, 2.0f, 6.0f, 0.40f, 16.0f,
		1.0f, 0.90f, 8.0f,	// phase 3 (rivers): on with the option, retail's flow; the opacity phase 3b's (phase 3: retail's 0.76)
		0.70f, 0.50f };	// phase 3b: V2 of wh1ter0se's three (pics4); phase 3's look = riverOpacity:0.76,riverShade:1,riverSky:1	// scale/bump/cells: the Mini
		// tune piece q2 (25/10, TournamentB beach + main menu): 260/0.30/8 left soft milky blotches on the close beach camera; a finer,
		// rougher map in smaller tiles reads as ripples moving across the whole sea	// phase 2: an offline preview of the shader
		// (modernize/water-ra3/research/preview): a half vector held 18-34 deg from straight up lit nearly every ripple (+0.16 on
		// average, the teal washed out); 40-50 deg at 0.6 leaves sparse moving highlights on the teal (+0.07 on average)
}

struct GxWaterRA3TuneRange
{
	const char *name;
	size_t offset;
	float lo;
	float hi;
};

static const GxWaterRA3TuneRange kGxWaterRA3TuneRanges[] = {
	{ "reach", offsetof(GxWaterRA3Tune, reach), 1.0f, 30.0f },
	{ "opacity", offsetof(GxWaterRA3Tune, opacity), 0.3f, 1.0f },
	{ "shape", offsetof(GxWaterRA3Tune, shape), 0.0f, 8.0f },
	{ "tintR", offsetof(GxWaterRA3Tune, tintR), 0.0f, 2.0f },
	{ "tintG", offsetof(GxWaterRA3Tune, tintG), 0.0f, 2.0f },
	{ "tintB", offsetof(GxWaterRA3Tune, tintB), 0.0f, 2.0f },
	{ "tintShape", offsetof(GxWaterRA3Tune, tintShape), 0.0f, 8.0f },
	{ "deepR", offsetof(GxWaterRA3Tune, deepR), 0.0f, 1.0f },
	{ "deepG", offsetof(GxWaterRA3Tune, deepG), 0.0f, 1.0f },
	{ "deepB", offsetof(GxWaterRA3Tune, deepB), 0.0f, 1.0f },
	{ "flat", offsetof(GxWaterRA3Tune, flat), 0.0f, 1.0f },
	{ "glint", offsetof(GxWaterRA3Tune, glint), 0.0f, 1.0f },
	{ "tiltMin", offsetof(GxWaterRA3Tune, tiltMin), 0.0f, 60.0f },
	{ "tiltMax", offsetof(GxWaterRA3Tune, tiltMax), 0.0f, 80.0f },
	{ "sky", offsetof(GxWaterRA3Tune, sky), 0.0f, 1.0f },
	{ "skyR", offsetof(GxWaterRA3Tune, skyR), 0.0f, 1.0f },
	{ "skyG", offsetof(GxWaterRA3Tune, skyG), 0.0f, 1.0f },
	{ "skyB", offsetof(GxWaterRA3Tune, skyB), 0.0f, 1.0f },
	{ "scale", offsetof(GxWaterRA3Tune, scale), 40.0f, 2000.0f },
	{ "speed", offsetof(GxWaterRA3Tune, speed), -40.0f, 40.0f },
	{ "div", offsetof(GxWaterRA3Tune, div), 0.0f, 40.0f },
	{ "bump", offsetof(GxWaterRA3Tune, bump), 0.0f, 1.0f },
	{ "cells", offsetof(GxWaterRA3Tune, cells), 4.0f, 32.0f },
	{ "river", offsetof(GxWaterRA3Tune, river), 0.0f, 1.0f },
	{ "riverOpacity", offsetof(GxWaterRA3Tune, riverOpacity), 0.3f, 1.0f },
	{ "riverSpeed", offsetof(GxWaterRA3Tune, riverSpeed), -40.0f, 40.0f },
	{ "riverShade", offsetof(GxWaterRA3Tune, riverShade), 0.2f, 1.5f },
	{ "riverSky", offsetof(GxWaterRA3Tune, riverSky), 0.0f, 1.0f },
};

inline float gxWaterRA3Clamp(float v, float lo, float hi)
{
	if (!(v == v))
		return lo;
	return v < lo ? lo : (v > hi ? hi : v);
}

inline bool gxWaterRA3Finite(float v)
{
	return v == v && v - v == 0.0f;	// false for NaN and both infinities
}

// ---------------------------------------------------------------------------------------------------------------------------
// The Options.ini key and the A/B variables.

// DEFAULT ON: wh1ter0se's yes, 04/11/2026 ("RA3 water ON by default in 0.9.70? yes").
static const bool kGxWaterRA3DefaultOn = true;

// The key's value read exactly as UserPreferences::getBool reads it (UserPreferences.cpp:193): missing or empty ->
// kGxWaterRA3DefaultOn; otherwise, ignoring case, "1", "t", "true", "y", "yes" or "ok" is on and anything else off.
inline bool gxWaterRA3OptionOn(const char *value)
{
	if (value == nullptr || value[0] == '\0')
		return kGxWaterRA3DefaultOn;
	static const char *const kTrue[] = { "1", "t", "true", "y", "yes", "ok" };
	for (const char *w : kTrue)
	{
		size_t i = 0;
		for (; w[i] != '\0'; ++i)
		{
			char c = value[i];
			if (c >= 'A' && c <= 'Z')
				c = (char)(c - 'A' + 'a');
			if (c != w[i])
				break;
		}
		if (w[i] == '\0' && value[i] == '\0')
			return true;
	}
	return false;
}

// GX_WATER_RA3: 1 (or y/t) on, 0 (or n/f) off, -1 when unset or unreadable (the Options.ini key then decides).
inline int gxWaterRA3ParseBool(const char *s)
{
	if (s == nullptr)
		return -1;
	while (*s == ' ' || *s == '\t')
		++s;
	switch (*s)
	{
		case '0': case 'n': case 'N': case 'f': case 'F':
			return 0;
		case '1': case 'y': case 'Y': case 't': case 'T':
			return 1;
		default:
			return -1;
	}
}

// GX_WATER_RA3_TUNE=reach:12,deepB:0.7: each known name with a whole number after the colon sets that field, clamped to its
// range; anything else is skipped. Returns how many pairs applied.
inline int gxWaterRA3ParseTune(const char *s, GxWaterRA3Tune *t)
{
	if (s == nullptr || t == nullptr)
		return 0;
	int applied = 0;
	const char *p = s;
	while (*p != '\0')
	{
		const char *comma = strchr(p, ',');
		const size_t len = comma ? (size_t)(comma - p) : strlen(p);
		char pair[48];
		if (len > 0 && len < sizeof(pair))
		{
			memcpy(pair, p, len);
			pair[len] = '\0';
			char *colon = strchr(pair, ':');
			if (colon != nullptr)
			{
				*colon = '\0';
				for (const GxWaterRA3TuneRange &r : kGxWaterRA3TuneRanges)
				{
					if (strcmp(pair, r.name) != 0)
						continue;
					char *end = nullptr;
					const double v = strtod(colon + 1, &end);
					if (end != colon + 1 && *end == '\0' && v == v)
					{
						*(float *)((char *)t + r.offset) = gxWaterRA3Clamp((float)v, r.lo, r.hi);
						++applied;
					}
					break;
				}
			}
		}
		if (comma == nullptr)
			break;
		p = comma + 1;
	}
	return applied;
}

// ---------------------------------------------------------------------------------------------------------------------------
// THE OVERRIDE. The two TransparentWater values RA3 water puts on TheWaterTransparency's chain (through clearer water's own
// override node, GXWaterClearGlue.inl), from the values it overrides (the map's own, or Water.ini's). The cap falls to
// `opacity` (never rises: a map already clearer keeps its own), and the fade - which ends at depth D * M * M, where the LUT
// reaches the cap (GXWaterClear.h) - ends `reach` times further out. A map that turned the soft edge off (depth 0) and any
// degenerate or non-finite input come back unchanged.
struct GxWaterRA3Values
{
	float depth;		// TransparentWaterDepth
	float minOpacity;	// TransparentWaterMinOpacity
};

inline GxWaterRA3Values gxWaterRA3Apply(float depth, float minOpacity, const GxWaterRA3Tune &t)
{
	GxWaterRA3Values out = { depth, minOpacity };
	if (!gxWaterRA3Finite(depth) || !gxWaterRA3Finite(minOpacity) || depth <= 0.0f || minOpacity <= 0.0f)
		return out;
	const float cap = gxWaterRA3Clamp(t.opacity, 0.3f, 1.0f);
	const float reach = gxWaterRA3Clamp(t.reach, 1.0f, 30.0f);
	float m = minOpacity;
	if (cap < m)
		m = cap;
	const float fadeEnd = depth * minOpacity * minOpacity * reach;
	out.minOpacity = m;
	out.depth = fadeEnd / (m * m);
	return out;
}

// ---------------------------------------------------------------------------------------------------------------------------
// THE LUTS. Both are 256 x 1 A8R8G8B8, read by the shore tiles' texture coordinate t = depth / (D * M) in [0, 1], clamped;
// texel x holds t = x / 255 (retail's own mapping, initDestAlphaLUT). The band is u = t / cap = depth / fade end; past u = 1
// both hold their end value, so the tiles beyond the band (not redrawn: the frame's cleared alpha, the untinted floor) meet
// them with no step.

// 0 at u = 0, 1 at u >= 1; the normalised Beer-Lambert ramp of steepness k, linear when k is ~0.
inline float gxWaterRA3Ramp(float u, float k)
{
	if (!(u > 0.0f))
		return 0.0f;
	if (u >= 1.0f)
		return 1.0f;
	if (!(k >= 1e-3f))
		return u;
	return (1.0f - expf(-k * u)) / (1.0f - expf(-k));
}

inline unsigned gxWaterRA3Byte(float v)
{
	return (unsigned)(gxWaterRA3Clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// The opacity LUT (the retail m_destAlphaTexture's contents): alpha = cap x ramp(u), RGB white as retail fills it (the pass
// that reads it writes alpha only). shape 0 is retail's own min(x, 255 x cap) to within one step.
inline void gxWaterRA3FillOpacityLUT(unsigned out[256], float cap, float shape)
{
	cap = gxWaterRA3Clamp(cap, 0.01f, 1.0f);
	shape = gxWaterRA3Clamp(shape, 0.0f, 8.0f);
	for (int x = 0; x < 256; ++x)
	{
		const float u = ((float)x / 255.0f) / cap;
		out[x] = (gxWaterRA3Byte(cap * gxWaterRA3Ramp(u, shape)) << 24) | 0x00ffffffu;
	}
}

// The tint LUT: the sea floor's multiplier m(u) = 1 + (tint - 1) x ramp(u, tintShape) per channel, in [0, 2], encoded for the
// blend SRC=DESTCOLOR, DEST=SRCALPHA, which gives dst x (src.rgb + src.a): src.a carries the common part (<= 1), src.rgb the
// rest. A spread over 1 between channels cannot be held; the largest channel then clamps.
// Past the band's end (t from cap to 1) the multiplier goes back to 1, linearly: t = 1 is depth D * M, where
// updateShorelineTile stops taking tiles, and the tiles beyond get no tint pass - a held end value would leave a tile-shaped
// step in the sea floor there. The floor weighs only 1 - cap under that stretch. An opaque cap (1) has no
// such stretch: the band end is the cutoff, and the floor beyond is not seen.
inline void gxWaterRA3FillTintLUT(unsigned out[256], float cap, const GxWaterRA3Tune &t)
{
	cap = gxWaterRA3Clamp(cap, 0.01f, 1.0f);
	const float k = gxWaterRA3Clamp(t.tintShape, 0.0f, 8.0f);
	const float end[3] = { gxWaterRA3Clamp(t.tintR, 0.0f, 2.0f), gxWaterRA3Clamp(t.tintG, 0.0f, 2.0f), gxWaterRA3Clamp(t.tintB, 0.0f, 2.0f) };
	for (int x = 0; x < 256; ++x)
	{
		const float tx = (float)x / 255.0f;
		float g = gxWaterRA3Ramp(tx / cap, k);
		if (tx > cap && cap < 1.0f)
			g = gxWaterRA3Clamp(1.0f - (tx - cap) / (1.0f - cap), 0.0f, 1.0f);
		float m[3];
		float lo = 2.0f, hi = 0.0f;
		for (int c = 0; c < 3; ++c)
		{
			m[c] = 1.0f + (end[c] - 1.0f) * g;
			lo = m[c] < lo ? m[c] : lo;
			hi = m[c] > hi ? m[c] : hi;
		}
		float a = lo < 1.0f ? lo : 1.0f;
		if (hi - a > 1.0f)
			a = hi - 1.0f < 1.0f ? hi - 1.0f : 1.0f;
		const unsigned a8 = gxWaterRA3Byte(a);
		const float aq = (float)a8 / 255.0f;	// the byte the blend will read, so the rgb part makes up its rounding
		out[x] = (a8 << 24) | (gxWaterRA3Byte(m[0] - aq) << 16) | (gxWaterRA3Byte(m[1] - aq) << 8) | gxWaterRA3Byte(m[2] - aq);
	}
}

// ---------------------------------------------------------------------------------------------------------------------------
// THE WATER'S COLOUR. The deep colour lit by the map: light = ambient + diffuse per channel, clamped to [0, 1] (the terrain's
// first light, as live water's glints and foam read it) - so a night map gets dark blue water, never a glow. The vertex
// colour's alpha byte is kept from the retail value (`alphaFrom`; the water's blend reads destination alpha, not this).
inline unsigned gxWaterRA3DeepDiffuse(const float ambient[3], const float diffuse[3], const GxWaterRA3Tune &t, unsigned alphaFrom)
{
	const float deep[3] = { t.deepR, t.deepG, t.deepB };
	unsigned c = 0;
	for (int i = 0; i < 3; ++i)
	{
		const float light = gxWaterRA3Clamp(ambient[i] + diffuse[i], 0.0f, 1.0f);
		c |= gxWaterRA3Byte(gxWaterRA3Clamp(deep[i], 0.0f, 1.0f) * light) << (16 - 8 * i);
	}
	return c | (alphaFrom & 0xff000000u);
}

// ---------------------------------------------------------------------------------------------------------------------------
// THE SURFACE. A size x size grey texture (size a multiple of 12) in [0.80, 1.0], mean ~0.9: two octaves of value noise
// whose lattices (4 and 12 cells across) wrap, so it tiles with no seam - a soft variation where TWWater01 has a
// high-contrast speckle. A fixed integer hash, no random number. Alpha 255 for standing water (its blend reads destination
// alpha); the rivers' copy takes `alpha` = riverOpacity, because retail's river pass blends by the texture's own alpha.
inline unsigned gxWaterRA3Hash(unsigned x, unsigned y, unsigned seed)
{
	unsigned h = x * 0x8da6b343u ^ y * 0xd8163841u ^ seed * 0xcb1ab31fu;
	h ^= h >> 13;
	h *= 0x5bd1e995u;
	h ^= h >> 15;
	return h;
}

// Value noise at (x, y) in lattice cells, the lattice wrapping every `period` cells.
inline float gxWaterRA3ValueNoise(float x, float y, int period, unsigned seed)
{
	const int x0 = (int)floorf(x), y0 = (int)floorf(y);
	float fx = x - (float)x0, fy = y - (float)y0;
	fx = fx * fx * (3.0f - 2.0f * fx);
	fy = fy * fy * (3.0f - 2.0f * fy);
	const unsigned xa = (unsigned)(((x0 % period) + period) % period), xb = (unsigned)((((x0 + 1) % period) + period) % period);
	const unsigned ya = (unsigned)(((y0 % period) + period) % period), yb = (unsigned)((((y0 + 1) % period) + period) % period);
	const float v00 = (float)(gxWaterRA3Hash(xa, ya, seed) & 0xffffu) / 65535.0f;
	const float v10 = (float)(gxWaterRA3Hash(xb, ya, seed) & 0xffffu) / 65535.0f;
	const float v01 = (float)(gxWaterRA3Hash(xa, yb, seed) & 0xffffu) / 65535.0f;
	const float v11 = (float)(gxWaterRA3Hash(xb, yb, seed) & 0xffffu) / 65535.0f;
	const float top = v00 + (v10 - v00) * fx, bottom = v01 + (v11 - v01) * fx;
	return top + (bottom - top) * fy;
}

inline void gxWaterRA3FillSurface(unsigned *out, int size, float alpha = 1.0f)
{
	const unsigned a8 = gxWaterRA3Byte(alpha) << 24;
	for (int y = 0; y < size; ++y)
		for (int x = 0; x < size; ++x)
		{
			const float fx = (float)x / (float)size, fy = (float)y / (float)size;
			const float n = 0.6f * gxWaterRA3ValueNoise(fx * 4.0f, fy * 4.0f, 4, 17u) + 0.4f * gxWaterRA3ValueNoise(fx * 12.0f, fy * 12.0f, 12, 29u);
			const unsigned g = gxWaterRA3Byte(0.80f + 0.20f * n);
			out[y * size + x] = a8 | (g << 16) | (g << 8) | g;
		}
}

// ---------------------------------------------------------------------------------------------------------------------------
// PHASE 2: THE MOTION. RA3's ocean moves because two tilings of one normal map scroll against each other (Ocean.fx: world XY
// x0.0025, one rotated 55 degrees and one by its transpose, scrolled at the octave speed +- its divergence) and the sky and the sun
// are reflected through them. Ours, one ADDED pass over the standing water (blend ONE, ONE, after the water and live water's
// glints), drawn from its own vertex buffer over the same grid:
//   u1/v1, u2/v2  the two tilings' coordinates (gxWaterRA3Tilings): world XY rotated +-55 degrees about live water's flow, each
//                 drifting along its own axis - so the pattern evolves instead of sliding;
//   diffuse       the sky's colour x the map's light x linear Fresnel 1 - V.N per vertex (gxWaterRA3Fresnel / SkyDiffuse): the
//                 water far from the camera, seen at a grazing angle, takes the sky's colour as RA3's does;
//   the program   kGxWaterRA3SheenShader: the two normals averaged, dotted with the sun's half vector (c0), ^8, x the sun's colour x
//                 strength (c1), + the sky, x the shroud - a moving sheen and glints where the ripples face the sun.
// A ps.1.1 has no per-pixel view vector, so the half vector is the camera's (one per frame), and it is held between tiltMin and
// tiltMax from straight up: a sun reflected straight at the camera would wash the sea white, one far off would never touch a ripple.
// All additive, so phase 1's colours stay underneath; glint 0 + sky 0 skips the pass (phase 1 exactly).

struct GxWaterRA3Vec
{
	float x, y, z;
};

inline GxWaterRA3Vec gxWaterRA3Unit(GxWaterRA3Vec v, GxWaterRA3Vec fallback)
{
	const float len = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
	if (!(len > 1e-6f) || !gxWaterRA3Finite(len))
		return fallback;
	return GxWaterRA3Vec{ v.x / len, v.y / len, v.z / len };
}

// The sun's half vector for the sheen: normalize(toSun + toEye), its angle from +z held in [tiltMin, tiltMax] degrees, the azimuth
// kept (degenerate: the eye's, then +x). Always finite and unit.
inline GxWaterRA3Vec gxWaterRA3HalfVector(GxWaterRA3Vec toSun, GxWaterRA3Vec toEye, float tiltMinDeg, float tiltMaxDeg)
{
	const GxWaterRA3Vec up = { 0.0f, 0.0f, 1.0f };
	const GxWaterRA3Vec l = gxWaterRA3Unit(toSun, up), e = gxWaterRA3Unit(toEye, up);
	const GxWaterRA3Vec h = gxWaterRA3Unit(GxWaterRA3Vec{ l.x + e.x, l.y + e.y, l.z + e.z }, up);
	float ax = h.x, ay = h.y, r = sqrtf(ax * ax + ay * ay);
	if (!(r > 1e-4f))
	{
		ax = e.x;
		ay = e.y;
		r = sqrtf(ax * ax + ay * ay);
		if (!(r > 1e-4f))
		{
			ax = 1.0f;
			ay = 0.0f;
			r = 1.0f;
		}
	}
	ax /= r;
	ay /= r;
	const float lo = gxWaterRA3Clamp(tiltMinDeg, 0.0f, 89.0f);
	const float hi = gxWaterRA3Clamp(tiltMaxDeg, lo, 89.0f);
	const float tilt = gxWaterRA3Clamp(acosf(gxWaterRA3Clamp(h.z, -1.0f, 1.0f)) * 57.2957795f, lo, hi) / 57.2957795f;
	return GxWaterRA3Vec{ ax * sinf(tilt), ay * sinf(tilt), cosf(tilt) };
}

// Linear Fresnel at a water point (the water's normal is +z): 1 - V.z, V the unit vector to the eye, clamped to [0, 1].
inline float gxWaterRA3Fresnel(GxWaterRA3Vec eye, float px, float py, float pz)
{
	const GxWaterRA3Vec v = { eye.x - px, eye.y - py, eye.z - pz };
	const float len = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
	if (!(len > 1e-6f) || !gxWaterRA3Finite(len))
		return 1.0f;
	return 1.0f - gxWaterRA3Clamp(v.z / len, 0.0f, 1.0f);
}

// The sheen pass's vertex colour: the sky's colour x the map's light (ambient + diffuse, clamped, as the deep colour) x Fresnel x
// strength; alpha 0xFF (the pass is ONE, ONE and writes no alpha).
inline unsigned gxWaterRA3SkyDiffuse(const float ambient[3], const float diffuse[3], const GxWaterRA3Tune &t, float fresnel)
{
	const float sky[3] = { t.skyR, t.skyG, t.skyB };
	const float k = gxWaterRA3Clamp(fresnel, 0.0f, 1.0f) * gxWaterRA3Clamp(t.sky, 0.0f, 1.0f);
	unsigned c = 0xff000000u;
	for (int i = 0; i < 3; ++i)
	{
		const float lit = gxWaterRA3Clamp(ambient[i] + diffuse[i], 0.0f, 1.0f);	// the map's light, as the deep colour's
		c |= gxWaterRA3Byte(gxWaterRA3Clamp(sky[i], 0.0f, 1.0f) * lit * k) << (16 - 8 * i);
	}
	return c;
}

// c1: the sun's own colour (the terrain's first light) x the sheen's strength, each in [0, 1] (a ps.1.1 constant is clamped there).
inline void gxWaterRA3SunConst(const float diffuse[3], const GxWaterRA3Tune &t, float out[4])
{
	const float g = gxWaterRA3Clamp(t.glint, 0.0f, 1.0f);
	for (int i = 0; i < 3; ++i)
		out[i] = gxWaterRA3Clamp(diffuse[i] * g, 0.0f, 1.0f);
	out[3] = 1.0f;
}

inline bool gxWaterRA3SheenOn(const GxWaterRA3Tune &t)
{
	return t.glint > 0.001f || t.sky > 0.001f;
}

// The two tilings: u = cu x + su y + ou, v = -su x + cu y + ov (world x, y). A is the flow turned +55 degrees, B -55; A drifts
// along its own u axis at speed + div world units a second, B at speed - div; offsets wrapped to [0, 1) from a double clock, so an
// hour into a match they are as exact as at its start. No flow (live water off): the map's +x.
struct GxWaterRA3Tiling
{
	float cu, su, ou, ov;
};

inline void gxWaterRA3Tilings(double seconds, float flowX, float flowY, const GxWaterRA3Tune &t, GxWaterRA3Tiling out[2])
{
	const float base = (flowX * flowX + flowY * flowY > 1e-12f) ? atan2f(flowY, flowX) : 0.0f;
	const float scale = gxWaterRA3Clamp(t.scale, 40.0f, 2000.0f);
	for (int k = 0; k < 2; ++k)
	{
		const float ang = base + (k == 0 ? 55.0f : -55.0f) / 57.2957795f;
		out[k].cu = cosf(ang) / scale;
		out[k].su = sinf(ang) / scale;
		const double drift = (double)(k == 0 ? t.speed + t.div : t.speed - t.div) * seconds / (double)scale;
		double ou = -drift;
		ou -= floor(ou);
		out[k].ou = (float)ou;
		if (out[k].ou >= 1.0f)
			out[k].ou = 0.0f;
		out[k].ov = (k == 0) ? 0.0f : 0.5f;	// B starts half a tile over: the two never line up at the map's origin
	}
}

inline void gxWaterRA3TilingUV(const GxWaterRA3Tiling &k, float x, float y, float *u, float *v)
{
	*u = k.cu * x + k.su * y + k.ou;
	*v = -k.su * x + k.cu * y + k.ov;
}

// The normal map: size x size A8R8G8B8, tileable. Height = three octaves of the surface's value noise (lattices `cells`, 2x and
// 4x that across, amplitudes 1, 1/2, 1/4, all wrapping), its gradient by central differences that wrap, scaled so the RMS slope
// is `bump`; normal = normalize(-sx, -sy, 1), stored n x 0.5 + 0.5 (R = x, G = y, B = z), alpha 255.
inline void gxWaterRA3FillNormalMap(unsigned *out, int size, float bump, int cells = 8)
{
	bump = gxWaterRA3Clamp(bump, 0.0f, 1.0f);
	cells = cells < 4 ? 4 : (cells > 32 ? 32 : cells);
	float *h = new float[(size_t)size * (size_t)size];
	for (int y = 0; y < size; ++y)
		for (int x = 0; x < size; ++x)
		{
			const float fx = (float)x / (float)size, fy = (float)y / (float)size;
			float v = 0.0f, amp = 1.0f;
			for (int p = cells; p <= 4 * cells; p *= 2)
			{
				v += amp * gxWaterRA3ValueNoise(fx * (float)p, fy * (float)p, p, 41u + (unsigned)p);
				amp *= 0.5f;
			}
			h[y * size + x] = v;
		}
	double s2 = 0.0;
	for (int y = 0; y < size; ++y)
		for (int x = 0; x < size; ++x)
		{
			const float gx = 0.5f * (h[y * size + (x + 1) % size] - h[y * size + (x + size - 1) % size]);
			const float gy = 0.5f * (h[((y + 1) % size) * size + x] - h[((y + size - 1) % size) * size + x]);
			s2 += (double)(gx * gx + gy * gy);
		}
	const double rms = sqrt(s2 / ((double)size * (double)size));
	const float k = (rms > 1e-12) ? (float)((double)bump / rms) : 0.0f;
	for (int y = 0; y < size; ++y)
		for (int x = 0; x < size; ++x)
		{
			const float sx = k * 0.5f * (h[y * size + (x + 1) % size] - h[y * size + (x + size - 1) % size]);
			const float sy = k * 0.5f * (h[((y + 1) % size) * size + x] - h[((y + size - 1) % size) * size + x]);
			const float inv = 1.0f / sqrtf(1.0f + sx * sx + sy * sy);
			const unsigned r = gxWaterRA3Byte(-sx * inv * 0.5f + 0.5f), g = gxWaterRA3Byte(-sy * inv * 0.5f + 0.5f);
			const unsigned b = gxWaterRA3Byte(inv * 0.5f + 0.5f);
			out[y * size + x] = 0xff000000u | (r << 16) | (g << 8) | b;
		}
	delete [] h;
}

// THE SHEEN PROGRAM, ps.1.1 bytecode written by hand in the token format of GXWaterLive.h's two (see the field table there;
// here also: opcode 0x08 dp3, source modifier 4 << 24 = _bx2 (x 2 - 1), destination shift 0xf << 24 = _d2, 1 << 20 = _sat).
// Stages: t0 / t1 the normal map by tiling A (u1/v1) / B (u2/v2), t3 the shroud (or white) as setupFlatWaterShader left it;
// c0 the sun's half vector (unit, so inside the [-1, 1] a ps.1.1 constant is clamped to), c1 the sun's colour x strength, v0 the
// sky x Fresnel. scripts/test/look-water-ra3-test.cpp decodes the tokens back to this text. The native renderer ports it by its
// BYTES (GXRender/Pipeline/GxCustomCatalog: water:ra3, gx_cs_ps.frag case 21): a change here needs the catalogue regenerated
// (scripts/render/ws-E/gen_catalog.py) or a native device refuses the pass.
//   ps.1.1
//   tex t0                      ; the normal map, tiling A
//   tex t1                      ; the normal map, tiling B
//   tex t3                      ; the shroud
//   add_d2 r1, t0_bx2, t1_bx2   ; the two tilings' average normal
//   dp3_sat r0, r1, c0          ; n.h (the water's normal is +z: tangent space = world space)
//   mul r0, r0, r0              ; ^2
//   mul r0, r0, r0              ; ^4
//   mul r0, r0, r0              ; ^8
//   mad r0.rgb, r0, c1, v0      ; x the sun's colour x strength, + the sky x Fresnel
//   mul r0.rgb, r0, t3          ; under the shroud
static const unsigned kGxWaterRA3SheenShader[] = {
	0xffff0101,
	0x00000042, 0xb00f0000, 0x00000042, 0xb00f0001, 0x00000042, 0xb00f0003,
	0x00000002, 0x8f0f0001, 0xb4e40000, 0xb4e40001,
	0x00000008, 0x801f0000, 0x80e40001, 0xa0e40000,
	0x00000005, 0x800f0000, 0x80e40000, 0x80e40000,
	0x00000005, 0x800f0000, 0x80e40000, 0x80e40000,
	0x00000005, 0x800f0000, 0x80e40000, 0x80e40000,
	0x00000004, 0x80070000, 0x80e40000, 0xa0e40001, 0x90e40000,
	0x00000005, 0x80070000, 0x80e40000, 0xb0e40003,
	0x0000ffff
};

// ---------------------------------------------------------------------------------------------------------------------------
// PHASE 3: RIVERS. A river polygon (drawRiverWater) is a strip of rows, each row a pair of bank points: a = the riverStart+1+i point
// (retail's u2 = 1), b = the riverStart-i point (u2 = 0). Retail draws it with TWWater01 (alpha 194) x the vertex colour and fades both
// banks out through TWAlphaEdge's alpha on stage 3 by u2: 0 at a bank, a near-linear ramp to 1 at 0.16 in, 1 to 0.84, back to 0
// (measured from the shipped DXT5, 25/10). With the option on, the river pass takes the deep colour and a river copy of the smooth
// surface (alpha riverOpacity), and the river gets the sheen too: one ADDED pass from its own buffers over EXACTLY retail's triangles
// - per row pair (a_i, b_i, b_i+1) and (a_i, b_i+1, a_i+1), the quad's diagonal a_i - b_i+1 - each cut along the lines where u2 = 0.16
// and 0.84, so the bank fade (linear in u2 inside each piece) rides the vertex alpha exactly and every sheen triangle lies on a retail
// triangle: the sheen covers what the river covers, no more, on a bent or non-planar quad too (an independent retriangulation did
// not: /r1L P2). The fade is multiplied by the shroud at the vertex (retail takes a river's shroud per vertex too,
// getRiverVertexDiffuse). The tilings are taken in the river's own frame - s = the distance along the centreline, t = the offset
// across, both in world units - with the flow along +s: downstream is the way retail's texture scrolls (toward a higher row index),
// and the ripples bend with the river.

static const float kGxWaterRA3RiverCols[4] = { 0.0f, 0.16f, 0.84f, 1.0f };	// u2 at each row's 4 vertices, b (0) to a (1)
static const float kGxWaterRA3RiverFade[4] = { 0.0f, 1.0f, 1.0f, 0.0f };	// TWAlphaEdge's profile there (linear between them)

struct GxWaterRA3RiverVertex
{
	float x, y, z;	// on retail's river triangles (the bank points' own heights, interpolated along the triangles' edges)
	float s, t;		// the river frame, world units: along the centreline from row 0, across from the centre (+ toward a)
	float fade;		// the bank fade
};

inline bool gxWaterRA3RiverOn(const GxWaterRA3Tune &t)
{
	return t.river >= 0.5f;
}

// Per row 4 vertices (u2 = 0, 0.16, 0.84, 1 on b - a), per row pair 2 more on the diagonal a_i - b_i+1 (u2 = 0.16, 0.84), and 10
// triangles a row pair: 6 rows - 2 vertices and 30 (rows - 1) indices, in 16-bit buffers (retail's own pass: 2 and 6 a row).
inline int gxWaterRA3RiverVertexCount(int rows) { return 6 * rows - 2; }
inline int gxWaterRA3RiverIndexCount(int rows) { return 30 * (rows - 1); }
inline bool gxWaterRA3RiverFits(int rows)
{
	return rows >= 2 && rows <= 65535 / 6 && rows - 1 <= 65535 / 30;
}

inline void gxWaterRA3RiverLerp(const GxWaterRA3RiverVertex &p, const GxWaterRA3RiverVertex &q, float k, GxWaterRA3RiverVertex *out)
{	// p x (1 - k) + q x k, written so k = 0 / 1 give p / q exactly
	out->x = q.x * k + p.x * (1.0f - k);
	out->y = q.y * k + p.y * (1.0f - k);
	out->z = q.z * k + p.z * (1.0f - k);
	out->s = q.s * k + p.s * (1.0f - k);
	out->t = q.t * k + p.t * (1.0f - k);
}

// gxWaterRA3RiverVertexCount(rows) vertices, or 0 when the rows do not fit: row i's at 4i .. 4i+3 (b to a; the bank columns are b and
// a exactly), then the diagonal's two of row pair i at 4 rows + 2i (u2 = 0.16) and 4 rows + 2i + 1 (u2 = 0.84).
inline int gxWaterRA3RiverMesh(const float (*a)[3], const float (*b)[3], int rows, GxWaterRA3RiverVertex *out)
{
	if (a == nullptr || b == nullptr || out == nullptr || !gxWaterRA3RiverFits(rows))
		return 0;
	double s = 0.0;
	float pcx = 0.0f, pcy = 0.0f;
	for (int i = 0; i < rows; ++i)
	{
		const float cx = 0.5f * (a[i][0] + b[i][0]), cy = 0.5f * (a[i][1] + b[i][1]);
		if (i > 0)
			s += sqrt((double)(cx - pcx) * (double)(cx - pcx) + (double)(cy - pcy) * (double)(cy - pcy));
		pcx = cx;
		pcy = cy;
		const float dx = a[i][0] - b[i][0], dy = a[i][1] - b[i][1];
		const float width = sqrtf(dx * dx + dy * dy);
		GxWaterRA3RiverVertex bank[2];	// b, a
		for (int e = 0; e < 2; ++e)
		{
			const float *pt = e ? a[i] : b[i];
			bank[e].x = pt[0];
			bank[e].y = pt[1];
			bank[e].z = pt[2];
			bank[e].s = (float)s;
			bank[e].t = (e ? 0.5f : -0.5f) * width;
		}
		for (int c = 0; c < 4; ++c)
		{
			gxWaterRA3RiverLerp(bank[0], bank[1], kGxWaterRA3RiverCols[c], &out[4 * i + c]);
			out[4 * i + c].fade = kGxWaterRA3RiverFade[c];
		}
	}
	for (int i = 0; i + 1 < rows; ++i)
		for (int c = 1; c <= 2; ++c)	// on the diagonal from b_i+1 (u2 = 0) to a_i (u2 = 1)
		{
			GxWaterRA3RiverVertex &v = out[4 * rows + 2 * i + (c - 1)];
			gxWaterRA3RiverLerp(out[4 * (i + 1)], out[4 * i + 3], kGxWaterRA3RiverCols[c], &v);
			v.fade = kGxWaterRA3RiverFade[c];
		}
	return gxWaterRA3RiverVertexCount(rows);
}

// Retail's two triangles of each row pair, each cut into its three u2 bands (a band of a triangle is a triangle or a convex quad):
// (a_i, b_i, b_i+1) -> [0, .16] quad b_i b_i+1 Q1 P1, [.16, .84] quad P1 Q1 Q2 P2, [.84, 1] triangle P2 Q2 a_i;
// (a_i, b_i+1, a_i+1) -> [0, .16] triangle b_i+1 R1 Q1, [.16, .84] quad Q1 R1 R2 Q2, [.84, 1] quad Q2 R2 a_i+1 a_i
// (P = row i's inner vertices, R = row i+1's, Q = the diagonal's). gxWaterRA3RiverIndexCount(rows) indices, or 0 when they do not fit.
inline int gxWaterRA3RiverIndices(int rows, unsigned short *out)
{
	if (out == nullptr || !gxWaterRA3RiverFits(rows))
		return 0;
	int n = 0;
	for (int i = 0; i + 1 < rows; ++i)
	{
		const int b0 = 4 * i, p1 = b0 + 1, p2 = b0 + 2, a0 = b0 + 3;
		const int b1 = 4 * i + 4, r1 = b1 + 1, r2 = b1 + 2, a1 = b1 + 3;
		const int q1 = 4 * rows + 2 * i, q2 = q1 + 1;
		const int tri[10][3] = {
			{ b0, b1, q1 }, { b0, q1, p1 },	// (a_i, b_i, b_i+1): u2 0 - .16
			{ p1, q1, q2 }, { p1, q2, p2 },	//                  .16 - .84
			{ p2, q2, a0 },					//                  .84 - 1
			{ b1, r1, q1 },					// (a_i, b_i+1, a_i+1): u2 0 - .16
			{ q1, r1, r2 }, { q1, r2, q2 },	//                  .16 - .84
			{ q2, r2, a1 }, { q2, a1, a0 } };	//                  .84 - 1
		for (const int *tr : tri)
			for (int k = 0; k < 3; ++k)
				out[n++] = (unsigned short)tr[k];
	}
	return n;
}

// The river pass's vertex colour: the sky x Fresnel (gxWaterRA3SkyDiffuse) with alpha = the bank fade x the shroud there (0..1).
inline unsigned gxWaterRA3RiverDiffuse(unsigned sky, float fade, float shroud)
{
	return (sky & 0x00ffffffu) | (gxWaterRA3Byte(gxWaterRA3Clamp(fade, 0.0f, 1.0f) * gxWaterRA3Clamp(shroud, 0.0f, 1.0f)) << 24);
}

// PHASE 3b: DEEPER RIVERS. Phase 3's rivers read pale turquoise on a bright bed (Lone Eagle's river measured (49,128,151) against the
// approved sea's (31,71,80)): the lakes' deep colour at 0.76 opacity, the snow bed - already tinted cyan by the tint pass - showing
// through the rest, and the light-blue sky reflection added on top. Three river-only levers: riverOpacity (less bed), riverShade (the
// river's lit deep colour x riverShade) and riverSky (the river sheen's sky x riverSky). Every value 1 (and riverOpacity 0.76) is
// phase 3 exactly.

// drawRiverWater's vertex colour: the lit deep colour's rgb x riverShade, rounded and clamped per byte (1 returns it bit for bit);
// the alpha byte kept - in retail's riverShader it only weighs the added sparkles (v0.a), never the river's opacity.
inline unsigned gxWaterRA3RiverShade(unsigned deep, const GxWaterRA3Tune &t)
{
	const float s = gxWaterRA3Clamp(t.riverShade, 0.2f, 1.5f);
	unsigned c = deep & 0xff000000u;
	for (int i = 0; i < 3; ++i)
	{
		const float v = (float)((deep >> (16 - 8 * i)) & 0xffu) * s + 0.5f;
		c |= (v >= 255.0f ? 255u : (unsigned)v) << (16 - 8 * i);
	}
	return c;
}

// The tune the river sheen pass draws with: standing water's, but drifting at riverSpeed and reflecting sky x riverSky (a copy: the
// standing pass never sees either change).
inline GxWaterRA3Tune gxWaterRA3RiverSheenTune(const GxWaterRA3Tune &t)
{
	GxWaterRA3Tune r = t;
	r.speed = t.riverSpeed;
	r.sky = t.sky * gxWaterRA3Clamp(t.riverSky, 0.0f, 1.0f);
	return r;
}

// THE RIVER SHEEN PROGRAM: the sheen program without the shroud texture - the last instruction multiplies by v0.a, the bank fade x
// the shroud per vertex (the replicated-alpha swizzle 0xff, as retail's riverShader reads v0.a). Native: water:ra3river,
// gx_cs_ps.frag case 22 (a change here needs the catalogue regenerated, as the sheen program's does).
//   ps.1.1
//   tex t0                      ; the normal map, tiling A (u1/v1)
//   tex t1                      ; the normal map, tiling B (u2/v2)
//   add_d2 r1, t0_bx2, t1_bx2   ; the two tilings' average normal
//   dp3_sat r0, r1, c0          ; n.h
//   mul r0, r0, r0              ; ^2
//   mul r0, r0, r0              ; ^4
//   mul r0, r0, r0              ; ^8
//   mad r0.rgb, r0, c1, v0      ; x the sun's colour x strength, + the sky x Fresnel
//   mul r0.rgb, r0, v0.a        ; x the bank fade x the shroud
static const unsigned kGxWaterRA3RiverSheenShader[] = {
	0xffff0101, 0x00000042, 0xb00f0000, 0x00000042, 0xb00f0001,
	0x00000002, 0x8f0f0001, 0xb4e40000, 0xb4e40001,	// add_d2
	0x00000008, 0x801f0000, 0x80e40001, 0xa0e40000,	// dp3_sat
	0x00000005, 0x800f0000, 0x80e40000, 0x80e40000,	// ^2
	0x00000005, 0x800f0000, 0x80e40000, 0x80e40000,	// ^4
	0x00000005, 0x800f0000, 0x80e40000, 0x80e40000,	// ^8
	0x00000004, 0x80070000, 0x80e40000, 0xa0e40001, 0x90e40000,	// mad
	0x00000005, 0x80070000, 0x80e40000, 0x90ff0000,	// x v0.a
	0x0000ffff
};

// ---------------------------------------------------------------------------------------------------------------------------
// The engine entry points (GXWaterRA3Glue.inl, compiled into W3DWater.cpp). Plain types, so this header stays engine-free.

// WaterRenderObjClass::load, BEFORE clearer water's own map load (which asks gxWaterRA3OverrideValues): re-reads Options.ini
// and the variables, refills the tint LUT.
void gxWaterRA3MapLoaded();
// skyBoxMapReset (the map's teardown).
void gxWaterRA3MapReset();
// A runtime change (a menu): applies inside a loaded map until it ends; not persisted.
void gxWaterRA3Set(bool on);
// On, inside a loaded map (not WorldBuilder).
bool gxWaterRA3Active();
// Clearer water's apply: when RA3 water is active, the override values it wants from the chain's own (depth, minOpacity).
bool gxWaterRA3OverrideValues(float depth, float minOpacity, float *outDepth, float *outMinOpacity);
// BaseHeightMap::initDestAlphaLUT: fills the 256 opacity texels (A8R8G8B8, x = 0..255) when active; false = retail's fill.
bool gxWaterRA3FillLUT(unsigned *texels256, float cap);
// Changes whenever the opacity LUT's shape would (on/off, shape): BaseHeightMap::setShoreLineDetail refills on a change.
unsigned gxWaterRA3LutKey();
// The terrain renders (HeightMap / FlatHeightMap Render, late): true = the tint state is set and renderShoreLines must draw
// the shore tiles once more under it (it then skips its own state set-up), then call gxWaterRA3TintEnd.
bool gxWaterRA3TintBegin();
void gxWaterRA3TintEnd();
// Between TintBegin and TintEnd (renderShoreLines / renderShoreLinesSorted read it).
bool gxWaterRA3TintPassOn();
