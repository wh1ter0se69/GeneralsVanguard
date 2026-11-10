// GeneralsX: LIVE WATER, a client-only look option on top of clearer water (GXWaterClear.h).
// "Clearer water looks nice just needs obvious animation and white glare to make it look like water and like wave water in
// white". It is ON by default from 0.9.66 (gxWaterLiveOptionOn): Options.ini GXWaterLive = no turns it off, GXWaterLiveStrength
// (0..1, default 1) scales it;
// GX_WATER_LIVE=0/1, GX_WATER_LIVE_PARTS=<letters of mgfw> and GX_WATER_LIVE_TUNE=<name:value,...> override them for A/B arms.
//
// WHAT RETAIL ALREADY HAS, AND WHAT THIS PUSHES (today's classic renderer; DXVK on every platform):
//   motion (m)  standing water is a 150-unit tiling of TWWater01 whose coordinates wobble by 0.02 of a tile (3 units): a
//               slosh back and forth, its u and v swings on different periods, so its direction wanders. Live water
//               REPLACES that slosh with one flow per map (THE FLOW below): the tile drifts steadily along it, and a long
//               swell travels along it, surging the surface forward - never backwards (drawTrapezoidWater).
//               the first version, which amplified the slosh over the clear water's still sea floor: "why is the water
//               moving so randomly fix it".
//   glare  (g)  the retail ps.1.1 water shader already ADDS WaterSurfaceBubbles x Noise0000 - white specks where the noise is
//               bright - but at x1, sub-pixel and nearly still. Live water uses its own ps.1.1 variant (the same four
//               textures), ADDED on top in a second draw: the specks squared and x4, riding the surface (its drift and swell),
//               lit where a band of the noise - stretched across the flow and travelling along it a little faster than the
//               water - passes under them, so they twinkle in bands that move with the water; plus a faint sheen from those
//               bands; both tinted by the sun (setupFlatWaterShader).
//   foam   (f)  retail has no shoreline foam. Live water draws one more pass over the cells within a few units of the water
//               line: a 64x64 table (water depth x lapping phase) of white foam that surges up the beach fast and slips back
//               slowly - a swash line at the water's edge, not a second wave (EA's own crests are the waves; an incoming
//               foam line of its own drew pale stripes across the shallows between them - Mini 18/10); broken up by Noise0000
//               and the bubble specks, which
//               are carried up the beach with each surge and back with its retreat; the lapping follows the same swell, so a
//               beach surges when the swell reaches it; lit by the map's light, under the shroud like the water
//               (W3DWater.cpp Render, after the water surface).
//   waves  (w)  EA's own breaking waves (the map's .wak file, wave256.tga - 25 stock maps, TournamentB among them) are drawn
//               pure white at the texture's thin alpha: faint lines. Live water lights them like EA's own unused code meant to
//               (W3DWaterTracks.cpp flush computes diffuseLight and never uses it) and doubles their alpha. And it grows them in
//               retail breaks the whole coast in one frame as a full-white line;
//               live, each crest breaks when the swell reaches it, riding in from offshore and building smoothly (EA'S CRESTS).
//
// RENDER-ONLY. Every value here feeds texture coordinates, pixel-shader constants or vertex colours of render passes; nothing
// the simulation reads is written, and there is no random number anywhere (a GameLogicRandomValue call from render code would
// desync). This header is pure logic with no engine include and sits in Core/GameEngine/Include/GameClient, outside the SimID
// digest; never include it from Common/, GameLogic/ or GameNetwork/. The engine glue is
// W3DDevice/GameClient/Water/GXWaterLiveGlue.inl, outside every GX-SIMID region. scripts/test/look-water-live-test.sh checks
// the functions below, with mutant arms.

#pragma once

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------------------------------------------------------
// The parts, as a bit mask (GX_WATER_LIVE_PARTS letters).
enum
{
	kGxWaterLiveMotion = 1,
	kGxWaterLiveGlare = 2,
	kGxWaterLiveFoam = 4,
	kGxWaterLiveWaves = 8,
	kGxWaterLiveAll = 15
};

// The foam's bank limits (THE BANK, below): full foam where the ground climbs out of the water at most this steeply (rise per
// world unit), none where it climbs at least this steeply. Here because the tune's defaults below take them.
static constexpr float kGxFoamBankGentle = 0.6f;
static constexpr float kGxFoamBankSteep = 0.9f;
// The foam's surf limits (THE SURF, below): full foam within this many world units of one of the map's own wave crests, none
// beyond the second.
static constexpr float kGxFoamSurfNear = 40.0f;
static constexpr float kGxFoamSurfFar = 80.0f;

// ---------------------------------------------------------------------------------------------------------------------------
// The look at strength 1, every value overridable per run by GX_WATER_LIVE_TUNE (name:value pairs, names as below).
struct GxWaterLiveTune
{
	float drift;	// the surface's steady drift along the flow, world units per second (retail: 0)
	float dir;		// the flow's compass angle, degrees from +x toward +y (225: retail's own noise scroll, toward -x -y)
	float surge;	// the swell's surge of the surface along the flow, world units (capped so the water never runs back)
	float gate;		// how fast the glints' twinkle bands travel along the flow, world units per second
	float glint;	// glint brightness: the specks x4 x this (retail: x1, untinted, so about 0.25 here)
	float sheen;	// the twinkle bands' own faint sheen (retail: 0)
	float foam;		// foam opacity at full surge
	float lap;		// the swell's period = the lapping period, seconds
	float wave;		// EA wave crest alpha gain: 1 retail, 2 doubled, 4 quadrupled (snapped to the nearest of the three)
	float bank0;	// the foam fades out where the bank climbs out of the water faster than this (rise per world unit) ...
	float bank1;	// ... and is gone where it climbs faster than this (THE BANK, below)
	float surf0;	// the foam is full within this many world units of one of the map's wave crests ...
	float surf1;	// ... and gone beyond this (THE SURF, below)
};

// constexpr: the glue's file-scope copy must be CONSTANT-initialised. A dynamic initialiser in W3DWater.cpp's file scope
// keeps the whole file's static initialisation at run time, and retail's zFillAlphaShader with it - a global that
// WaterRenderObjClass::init's GX-SIMID region reads, which simid-closure.py then reports as a gap (17/10).
constexpr GxWaterLiveTune gxWaterLiveDefaultTune()
{
	return GxWaterLiveTune{ 3.6f, 225.0f, 2.0f, 9.0f, 0.85f, 0.10f, 1.0f, 7.0f, 2.0f, kGxFoamBankGentle, kGxFoamBankSteep, kGxFoamSurfNear,
		kGxFoamSurfFar };
}

// What each knob accepts (a value outside is clamped to the range).
struct GxWaterLiveTuneRange
{
	const char *name;
	size_t offset;
	float lo;
	float hi;
};

static const GxWaterLiveTuneRange kGxWaterLiveTuneRanges[] = {
	{ "drift", offsetof(GxWaterLiveTune, drift), 0.0f, 20.0f },
	{ "dir", offsetof(GxWaterLiveTune, dir), 0.0f, 360.0f },
	{ "surge", offsetof(GxWaterLiveTune, surge), 0.0f, 10.0f },
	{ "gate", offsetof(GxWaterLiveTune, gate), 0.0f, 40.0f },
	{ "glint", offsetof(GxWaterLiveTune, glint), 0.0f, 1.0f },
	{ "sheen", offsetof(GxWaterLiveTune, sheen), 0.0f, 1.0f },
	{ "foam", offsetof(GxWaterLiveTune, foam), 0.0f, 1.0f },
	{ "lap", offsetof(GxWaterLiveTune, lap), 2.0f, 30.0f },
	{ "wave", offsetof(GxWaterLiveTune, wave), 1.0f, 4.0f },
	{ "bank0", offsetof(GxWaterLiveTune, bank0), 0.0f, 8.0f },
	{ "bank1", offsetof(GxWaterLiveTune, bank1), 0.0f, 8.0f },
	{ "surf0", offsetof(GxWaterLiveTune, surf0), 0.0f, 2000.0f },
	{ "surf1", offsetof(GxWaterLiveTune, surf1), 0.0f, 2000.0f },
};

inline float gxWaterLiveClamp(float v, float lo, float hi)
{
	if (!(v == v))
		return lo;	// NaN
	return v < lo ? lo : (v > hi ? hi : v);
}

// The Options.ini key. GeneralsX: ON BY DEFAULT from 0.9.66 (for wh1ter0se, who chose this water
// from the look-water-3 pictures, clearer and live water on together): an Options.ini without GXWaterLive draws live water, and
// GXWaterLive = no turns it off.
static const bool kGxWaterLiveDefaultOn = true;

// The key's value read exactly as UserPreferences::getBool reads it (UserPreferences.cpp:193): missing or empty ->
// kGxWaterLiveDefaultOn; otherwise, ignoring case, "1", "t", "true", "y", "yes" or "ok" is on and anything else ("no" too) off.
inline bool gxWaterLiveOptionOn(const char *value)
{
	if (value == nullptr || value[0] == '\0')
		return kGxWaterLiveDefaultOn;
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

// "0"/"1"/"no"/"yes"/"off"/"on"/"false"/"true" -> 0/1; anything else (or null) -> -1 (unset: the Options.ini key decides).
inline int gxWaterLiveParseBool(const char *s)
{
	if (s == nullptr)
		return -1;
	static const char *const kOff[] = { "0", "no", "off", "false" };
	static const char *const kOn[] = { "1", "yes", "on", "true" };
	for (const char *w : kOff)
		if (strcmp(s, w) == 0)
			return 0;
	for (const char *w : kOn)
		if (strcmp(s, w) == 0)
			return 1;
	return -1;
}

// A strength in [0, 1]; anything unparsable keeps `fallback`.
inline float gxWaterLiveParseReal(const char *s, float fallback, float lo, float hi)
{
	if (s == nullptr || *s == '\0')
		return fallback;
	char *end = nullptr;
	const double v = strtod(s, &end);
	if (end == s || *end != '\0' || !(v == v))
		return fallback;
	return gxWaterLiveClamp((float)v, lo, hi);
}

// GX_WATER_LIVE_PARTS: letters m g f w in any order; anything else in the string -> all parts (the safe reading of a typo is
// "everything on", which a picture shows at once). Null or empty -> all.
inline int gxWaterLiveParseParts(const char *s)
{
	if (s == nullptr || *s == '\0')
		return kGxWaterLiveAll;
	int mask = 0;
	for (const char *p = s; *p != '\0'; ++p)
	{
		switch (*p)
		{
			case 'm': mask |= kGxWaterLiveMotion; break;
			case 'g': mask |= kGxWaterLiveGlare; break;
			case 'f': mask |= kGxWaterLiveFoam; break;
			case 'w': mask |= kGxWaterLiveWaves; break;
			default: return kGxWaterLiveAll;
		}
	}
	return mask;
}

// GX_WATER_LIVE_TUNE "glint:0.7,lap:6": each known name sets its knob (clamped); unknown names and malformed pairs are
// skipped. Returns how many pairs were applied.
inline int gxWaterLiveParseTune(const char *s, GxWaterLiveTune *t)
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
				for (const GxWaterLiveTuneRange &r : kGxWaterLiveTuneRanges)
				{
					if (strcmp(pair, r.name) != 0)
						continue;
					char *end = nullptr;
					const double v = strtod(colon + 1, &end);
					if (end != colon + 1 && *end == '\0' && v == v)
					{
						*(float *)((char *)t + r.offset) = gxWaterLiveClamp((float)v, r.lo, r.hi);
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
// TIME. The glue keeps its own clock: game seconds in a double, stepped once per CLIENT update (WaterRenderObjClass::update,
// from GameClient::update - including the iterations fast-forward and catch-up do not draw) by that iteration's logic time:
// gxRenderTimeScale() logic frames (the pacer's elapsed time x the logic rate, at most one tick; the GX_SHOT capture clock
// under capture; 0 while paused) over the logic rate - then pulled toward the game's own time (gxWaterLiveClockFollow). So
// the water moves the same at 30, 60 or 240 rendered frames a second and on 30 Hz and 60 Hz builds; a double, so it still
// resolves a microsecond after days (m_riverVOrigin, the float retail steps, rounds each frame's step by ~10% after a few hours
// at a high frame rate).
inline double gxWaterLiveClockStep(double clock, float logicFrames, float logicFramesPerSecond)
{
	if (!(logicFrames > 0.0f) || !(logicFramesPerSecond > 0.0f))
		return clock;	// paused, or nothing to step by (NaN included)
	return clock + (double)logicFrames / (double)logicFramesPerSecond;
}

// The pacer's step is an animation time step, not the ticks the game actually ran: under manual fast-forward a cheap iteration
// counts a fraction of a tick while the game still runs a whole one, and in a network stall it keeps counting while the game
// waits. So after each step the clock is pulled toward the game's own time (logic frame / logic rate): left
// alone within half a tick (normal pacing - smooth), eased in by a tenth of the gap beyond that, held within three ticks - and
// never moved backwards (water that waits is still water; water that runs back is what wh1ter0se saw). A zero step (paused,
// frozen) holds the clock: only a jump past the three-tick window (a loaded save) moves it then.
inline double gxWaterLiveClockFollow(double prev, double stepped, double gameSeconds, double tickSeconds)
{
	double c = stepped;
	const double err = gameSeconds - c, dead = 0.5 * tickSeconds, hard = 3.0 * tickSeconds;
	if (err > hard)
		c = gameSeconds - hard;
	else if (!(stepped > prev))
		c = prev;	// no step this update: no settling either
	else if (err < -hard)
		c = gameSeconds + hard;
	else if (err > dead)
		c += 0.1 * (err - dead);
	else if (err < -dead)
		c += 0.1 * (err + dead);
	return (c > prev) ? c : prev;
}

// A periodic value for texture translation that never grows without bound: v mod period, in [0, period).
inline float gxWaterLiveWrap(float v, float period)
{
	if (period <= 0.0f)
		return 0.0f;
	const float w = v - period * floorf(v / period);
	return (w >= period) ? 0.0f : w;
}

// The same in double, for the clock's products (seconds x speed grows without bound; the wrap is taken before the float).
inline float gxWaterLiveWrapD(double v, double period)
{
	if (!(period > 0.0))
		return 0.0f;
	double w = v - period * floor(v / period);
	if (!(w >= 0.0) || w >= period)
		w = 0.0;
	const float f = (float)w;
	return (f >= (float)period) ? 0.0f : f;
}

// ---------------------------------------------------------------------------------------------------------------------------
// THE FLOW. One direction for the whole map, which every moving part follows: the surface's drift, the swell, the glints'
// twinkle bands and the swell that sets when each beach laps. Its default is the direction retail's own standing water
// already scrolls its noise (setupFlatWaterShader translates the noise texgen by (+t, +t): the pattern moves toward -x -y),
// so retail's pass, still drawn under the live one, moves the same way.
struct GxWaterLiveFlow
{
	float x, y;	// a unit vector in world x/y
};

inline GxWaterLiveFlow gxWaterLiveFlowDir(float degrees)
{
	const float r = gxWaterLiveWrap(degrees, 360.0f) * (3.14159265f / 180.0f);
	GxWaterLiveFlow f;
	f.x = cosf(r);
	f.y = sinf(r);
	return f;
}

// ---------------------------------------------------------------------------------------------------------------------------
// MOTION + GLARE coordinates for one standing-water draw, from the time and the knobs. Retail's own values when off.
static constexpr float kGxWaterTileUnits = 150.0f;		// drawTrapezoidWater's waterFactor: one TWWater01 tile, world units
static constexpr float kGxWaterGlintUnits = 110.0f;		// one WaterSurfaceBubbles tile for the glints (Mini 17/10: 190 made soft blobs)
static constexpr float kGxWaterGateAlong = 44.0f;		// the twinkle bands: one Noise0000 tile along the flow (retail: 16 square)
static constexpr float kGxWaterGateAcross = 132.0f;		// ...and across it: the noise's blobs drawn out into bands across the flow
static constexpr float kGxSwellLength = 420.0f;			// the swell's wavelength along the flow, world units (period = lap)
static constexpr float kGxSurgeMaxShare = 0.8f;			// the surge's fastest backward swing, as a share of the drift's speed

struct GxWaterLiveUV
{
	bool motion;		// drift + swell REPLACE retail's slosh on u1/v1, and carry the glints' u2/v2 the same way
	float fx, fy;		// the flow (unit)
	float du, dv;		// the steady drift, in water tiles (wrapped)
	float surge;		// the swell's surge along the flow, world units (0: none) - see gxWaterLiveSurgeAt
	float swellK;		// its wavenumber along the flow, radians per world unit
	float swellPhase;	// its phase now, radians in [0, 2pi)
	bool glare;			// glint coordinates in u2/v2 + the twinkle-band texgen below
	float glintScale;	// u2 = x * glintScale + gu (+ the surge, as the surface)
	float gu, gv;		// the glints' drift, in glint tiles (wrapped): the surface's drift
	float gatePhase;	// the twinkle bands' travel along the flow, in noise tiles (wrapped); tile kGxWaterGateAlong x Across
};

inline GxWaterLiveUV gxWaterLiveUV(double seconds, int parts, float strength, const GxWaterLiveTune &t)
{
	GxWaterLiveUV uv;
	memset(&uv, 0, sizeof(uv));
	uv.glintScale = 1.0f / kGxWaterGlintUnits;
	const GxWaterLiveFlow f = gxWaterLiveFlowDir(t.dir);
	uv.fx = f.x;
	uv.fy = f.y;
	if (strength <= 0.0f)
		return uv;
	const double lap = (t.lap > 0.0f) ? (double)t.lap : 7.0;
	if (parts & kGxWaterLiveMotion)
	{
		// The pattern moves +flow, so the coordinates move -flow: u = x / tile - drift * t * fx / tile.
		const double d = (double)t.drift * (double)strength * seconds;
		uv.motion = true;
		uv.du = gxWaterLiveWrapD(-d * f.x / kGxWaterTileUnits, 1.0);
		uv.dv = gxWaterLiveWrapD(-d * f.y / kGxWaterTileUnits, 1.0);
		// The swell: s(a, t) = surge * sin(k a - w t + pi) on top of the drift. Its speed swings by surge * w around the
		// drift's, so the surge is capped at kGxSurgeMaxShare of the drift's speed: the water speeds up and slows down as
		// each swell passes but never runs backwards (the slosh it replaces did, both ways, every ~2 s). The + pi puts the
		// fastest forward surge on the swell's crest, which reaches a beach as the lapping starts (gxWaterLiveCoastPhase).
		const float w = (float)(2.0 * 3.14159265358979 / lap);
		const float cap = kGxSurgeMaxShare * t.drift * strength / w;
		const float want = t.surge * strength;
		uv.surge = (want < cap) ? want : cap;
		uv.swellK = 2.0f * 3.14159265f / kGxSwellLength;
		uv.swellPhase = 2.0f * 3.14159265f * gxWaterLiveWrapD(seconds / lap + 0.5, 1.0);
	}
	if (parts & kGxWaterLiveGlare)
	{
		// The specks ride the surface (its drift; the vertex adds its surge); the noise that lights them is drawn out
		// across the flow into bands and travels along it at `gate` - faster than the water, so each speck lights as a band
		// passes it, and the twinkle sweeps the same way the water goes.
		uv.glare = true;
		if (uv.motion)
		{
			const double d = (double)t.drift * (double)strength * seconds;
			uv.gu = gxWaterLiveWrapD(-d * f.x / kGxWaterGlintUnits, 1.0);
			uv.gv = gxWaterLiveWrapD(-d * f.y / kGxWaterGlintUnits, 1.0);
		}
		uv.gatePhase = gxWaterLiveWrapD((double)t.gate * seconds / kGxWaterGateAlong, 1.0);
	}
	return uv;
}

// The swell's surge at `along` (= x * fx + y * fy, world units), world units along the flow; 0 when motion is off. The glue
// scales uv.surge by gxWaterLiveSurgeGain for each water area's mesh.
inline float gxWaterLiveSurgeAt(const GxWaterLiveUV &uv, float along)
{
	if (!uv.motion || !(uv.surge > 0.0f))
		return 0.0f;
	return uv.surge * sinf(uv.swellK * along - uv.swellPhase);
}

// drawTrapezoidWater samples the swell only at its vertices. At 6 or more vertices per wavelength the swell is drawn whole;
// under 4 it would alias into a shimmer that moves with nothing, so it fades out between. The glue takes ONE gain for the whole
// map - the worst patch's (gxWaterLiveTrapezoidSpacing over every standing-water patch at load) - because a gain per patch
// gives two neighbouring patches different coordinates on their shared edge: a seam that moves. A map with
// one enormous sea polygon then just drifts.
inline float gxWaterLiveSurgeGain(float cellUnits)
{
	if (!(cellUnits > 0.0f))
		return 1.0f;
	return gxWaterLiveClamp((kGxSwellLength / cellUnits - 4.0f) * 0.5f, 0.0f, 1.0f);
}

// Two neighbouring patches subdivided differently sample the swell at different points along their shared edge and each
// interpolates linearly between its own samples, so their coordinates there can differ (a T-junction). Between
// samples `spacing` apart the line misses a sine of amplitude `surge` by at most surge x (1 - cos(pi x spacing / wavelength)),
// and the seam is at most the two sides' misses added. This gain keeps that seam under kGxSwellSeamMax world units - under a
// pixel at the game's camera (retail's own slosh, 80-unit wavelength on ~40-unit vertices, leaves seams of up to its whole
// 3-unit amplitude). The glue multiplies it into the map's swell gain.
static constexpr float kGxSwellSeamMax = 0.5f;

inline float gxWaterLiveSeamGain(float spacing, float surge)
{
	if (!(spacing > 0.0f) || !(surge > 0.0f))
		return 1.0f;
	const float miss = surge * (1.0f - cosf(3.14159265f * gxWaterLiveClamp(spacing / kGxSwellLength, 0.0f, 1.0f)));
	if (!(miss > 0.0f))
		return 1.0f;
	return gxWaterLiveClamp(kGxSwellSeamMax / (2.0f * miss), 0.0f, 1.0f);
}

// The largest vertex spacing drawTrapezoidWater gives one 4-sided water patch, corners (x, y) in the order renderWater passes
// them (p0 origin, p1 along u, p2 the far corner, p3 along v). Its subdivision counts come from the SUM of opposite edges
// ((Int)(sum / 80), clamped to 1..50: 8 * MAP_XY_FACTOR), but a tapered or triangular patch (renderWater makes one from an odd
// corner) spaces its vertices along the LONGER edge - so the bound is the longer edge over the count, per axis.
inline float gxWaterLiveTrapezoidSpacing(const float pts[4][2])
{
	const float u1 = hypotf(pts[1][0] - pts[0][0], pts[1][1] - pts[0][1]);
	const float u2 = hypotf(pts[2][0] - pts[3][0], pts[2][1] - pts[3][1]);
	const float v1 = hypotf(pts[3][0] - pts[0][0], pts[3][1] - pts[0][1]);
	const float v2 = hypotf(pts[2][0] - pts[1][0], pts[2][1] - pts[1][1]);
	int uc = (int)((u1 + u2) / 80.0f), vc = (int)((v1 + v2) / 80.0f);
	uc = (uc < 1) ? 1 : ((uc > 50) ? 50 : uc);
	vc = (vc < 1) ? 1 : ((vc > 50) ? 50 : vc);
	const float su = ((u1 > u2) ? u1 : u2) / (float)uc;
	const float sv = ((v1 > v2) ? v1 : v2) / (float)vc;
	return (su > sv) ? su : sv;
}

// One standing-water vertex's live coordinates: the water tile (u1/v1) and the glints (u2/v2), both carried by the drift
// and the surge. drawTrapezoidWater writes them over retail's when motion / glare are on.
struct GxWaterLiveVertexUV
{
	float u1, v1, u2, v2;
};

inline GxWaterLiveVertexUV gxWaterLiveVertexUV(const GxWaterLiveUV &uv, float x, float y)
{
	const float s = gxWaterLiveSurgeAt(uv, x * uv.fx + y * uv.fy);
	GxWaterLiveVertexUV o;
	o.u1 = x / kGxWaterTileUnits + uv.du - uv.fx * s / kGxWaterTileUnits;
	o.v1 = y / kGxWaterTileUnits + uv.dv - uv.fy * s / kGxWaterTileUnits;
	o.u2 = (x - uv.fx * s) * uv.glintScale + uv.gu;
	o.v2 = (y - uv.fy * s) * uv.glintScale + uv.gv;
	return o;
}

// ---------------------------------------------------------------------------------------------------------------------------
// LIGHT. One colour per pass from the map's terrain lights (TheGlobalData m_terrainAmbient/m_terrainDiffuse [0], 0..1 each):
// ambient + diffuse * sunShare, each channel clamped to [0, 1] - so night maps get dim blue foam, never a glow, and a bright
// map never wraps a channel past 255 into its neighbour (EA's unused wave colour would have: it adds without a clamp).
inline unsigned gxWaterLiveLitColor(const float ambient[3], const float diffuse[3], float sunShare, float alpha)
{
	unsigned c = 0;
	for (int i = 0; i < 3; ++i)
	{
		const float v = gxWaterLiveClamp(ambient[i] + diffuse[i] * sunShare, 0.0f, 1.0f);
		c |= (unsigned)(v * 255.0f + 0.5f) << (16 - 8 * i);	// r, g, b
	}
	const float a = gxWaterLiveClamp(alpha, 0.0f, 1.0f);
	return c | ((unsigned)(a * 255.0f + 0.5f) << 24);
}

// ---------------------------------------------------------------------------------------------------------------------------
// FOAM. Water depth d (world units of height: the water level minus the ground; negative on the beach) and the lapping
// phase (cycles, any real; the fraction is used) -> foam opacity in [0, 1]. One cycle: the surge runs up the beach in the
// first kGxFoamRise of it (ease-out: fast), then slips back over the rest (ease-in-out: slow), a band of foam behind its front
// that never reaches deeper than ~0.7 height units: a swash line at the water's edge. (Until 18/10 the table also sent an
// incoming foam line in from 3.5 deep each cycle and trailed the band to 2.2 deep: on a gentle beach under the clear water
// those drew wide pale stripes across the shallows between EA's own crests - a second wave rhythm that read as random.)
static constexpr float kGxFoamDepthLo = -0.6f;	// table u = 0: 0.6 units up the beach (above the highest surge)
static constexpr float kGxFoamDepthHi = 1.5f;	// table u = 1: deeper than the band ever reaches
static constexpr float kGxFoamRise = 0.22f;		// fraction of a cycle the surge takes to run up
static constexpr float kGxFoamClimb = 0.3f;		// how far up the beach (height units) the surge front reaches (Mini 17/10:
												// 1.2 ran far up a gentle beach and read as bleached sand, not foam)
static constexpr int kGxFoamTableSize = 64;

inline float gxWaterLiveSmooth(float e0, float e1, float x)
{
	const float t = gxWaterLiveClamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

inline float gxWaterLiveSurge(float phase)
{
	const float ph = phase - floorf(phase);
	if (ph < kGxFoamRise)
	{
		const float u = 1.0f - ph / kGxFoamRise;
		return 1.0f - u * u;	// ease-out
	}
	const float u = (ph - kGxFoamRise) / (1.0f - kGxFoamRise);
	return 1.0f - u * u * (3.0f - 2.0f * u);	// ease-in-out
}

inline float gxWaterLiveFoamAlpha(float depth, float phase)
{
	const float ph = phase - floorf(phase);
	const float s = gxWaterLiveSurge(ph);
	const float front = -kGxFoamClimb * s;
	float edge = gxWaterLiveSmooth(front - 0.1f, front + 0.15f, depth)
		* (1.0f - gxWaterLiveSmooth(front + 0.2f, front + 0.2f + 0.4f + 0.4f * s, depth));
	edge *= 0.8f + 0.2f * s;
	return gxWaterLiveClamp(edge, 0.0f, 1.0f);
}

// The table's texel (i = depth column, j = phase row) -> its opacity; texel centres.
inline float gxWaterLiveFoamTexel(int i, int j)
{
	const float u = ((float)i + 0.5f) / (float)kGxFoamTableSize;
	const float v = ((float)j + 0.5f) / (float)kGxFoamTableSize;
	return gxWaterLiveFoamAlpha(kGxFoamDepthLo + u * (kGxFoamDepthHi - kGxFoamDepthLo), v);
}

// The table coordinate for a depth (clamped addressing does the rest).
inline float gxWaterLiveFoamU(float depth)
{
	return (depth - kGxFoamDepthLo) / (kGxFoamDepthHi - kGxFoamDepthLo);
}

// The lapping phase offset at world (x, y): the swell's own (the surface's swell, gxWaterLiveUV, reaches a point at
// along / kGxSwellLength cycles after the origin, travelling along the flow), so a beach surges when the swell gets there -
// a coast square to the flow laps all at once, one along it is swept by the break at the swell's speed - plus a gentle
// irregularity so no two coves match exactly. The phase row is v = seconds / lap + this, never wrapped per vertex (the texture
// wraps), so a cell never interpolates across a wrap; its gradient stays under 0.003 cycles per world unit, so one 10-unit
// cell interpolates smoothly.
inline float gxWaterLiveCoastPhase(float x, float y, const GxWaterLiveFlow &f)
{
	return -(x * f.x + y * f.y) / kGxSwellLength + 0.04f * sinf(0.0063f * x + 1.3f) * cosf(0.0049f * y);
}

// The foam's pattern is carried up the beach by each surge and back by its retreat: at a vertex it is offset along the uphill
// direction (gxWaterLiveUphill) by kGxFoamCarry x the surge. A foam vertex's noise coordinates are taken at (x, y) minus this.
static constexpr float kGxFoamCarry = 4.0f;	// world units at full surge (the front itself climbs 0.3 height units: a few units)

inline void gxWaterLiveFoamCarry(float ux, float uy, float phase, float *ox, float *oy)
{
	const float c = kGxFoamCarry * gxWaterLiveSurge(phase);
	*ox = ux * c;
	*oy = uy * c;
}

// The uphill direction at a grid vertex from the ground's rise across it: dhx over spanX world units along x (the right
// neighbour's height minus the left's), dhy over spanY along y. A unit vector toward higher ground, faded toward zero where
// the ground is flatter than 1 in 20 - there is no reliable direction there, and a flat bank carries no foam anywhere in
// particular. A zero span (a one-vertex-wide map) is flat on that axis.
inline void gxWaterLiveUphill(float dhx, float spanX, float dhy, float spanY, float *ux, float *uy)
{
	*ux = 0.0f;
	*uy = 0.0f;
	const float gx = (spanX > 0.0f) ? dhx / spanX : 0.0f;
	const float gy = (spanY > 0.0f) ? dhy / spanY : 0.0f;
	const float m = sqrtf(gx * gx + gy * gy);
	if (!(m > 1e-6f))
		return;	// flat (or NaN)
	const float fade = gxWaterLiveClamp(m / 0.05f, 0.0f, 1.0f);
	*ux = gx / m * fade;
	*uy = gy / m * fade;
}

// ---------------------------------------------------------------------------------------------------------------------------
// THE BANK. GeneralsX:
// - a thin cream outline drawn along every shore, stair-stepped in close-up. The foam's band is a
// band of DEPTH (about -0.4 to 0.7 height units at full surge, gxWaterLiveFoamAlpha), so on the ground it is that depth over
// the bank's slope wide. On TournamentB's beaches (median slope 0.35 at the water line) that is a few world units of swash; on
// the cut banks of ponds and rivers (median 0.7 to 0.9: Mountain Guns, Killing Fields, Winding River, Tournament Lake) it is
// about one unit - a hairline, lit cream by a warm sun, that traces the water line triangle by triangle, so the terrain grid's
// steps show in it. A swash zone needs a beach: the foam fades out where the ground climbs out of the water faster than
// `gentle`, and is gone at `steep`, per vertex and smoothly (the factor is the foam vertex's alpha, interpolated over the
// triangle). A degenerate pair (steep <= gentle) is a hard step at `gentle`; NaN reads as steep (no foam where the ground is
// unknown).
//
// The slope at a vertex is the steepest terrain triangle around it: of the (up to four) cells that share the vertex, each
// cell's steepest triangle under EITHER diagonal (gxWaterLiveCellSlope). Not the central difference gxWaterLiveUphill reads
// for the carry's direction: over two cells it halves a one-cell cliff (a bank that jumps 7.5 between neighbours reads 0.375,
// not 0.75) and cancels to flat at a pit, so a stepped bank kept its line. The steepest face is what
// sets the narrowest band the vertex's alpha reaches; either diagonal, so the measure needs no flip state and a map tool can
// reproduce it from the heights alone.
inline float gxWaterLiveCellSlope(float h00, float h10, float h11, float h01, float cell)
{
	if (!(cell > 0.0f))
		return 0.0f;
	// The four triangles the two diagonals make, each as (rise along x, rise along y) over one cell.
	const float faces[4][2] = { { h10 - h00, h11 - h10 }, { h11 - h01, h01 - h00 }, { h10 - h00, h01 - h00 }, { h11 - h01, h11 - h10 } };
	float worst = 0.0f;
	for (const float *f : faces)
	{
		const float s = sqrtf(f[0] * f[0] + f[1] * f[1]);
		worst = (s > worst || !(s == s)) ? s : worst;	// a NaN height stays NaN: no foam there (gxWaterLiveFoamBank)
	}
	return worst / cell;
}

inline float gxWaterLiveFoamBank(float slope, float gentle, float steep)
{
	if (!(slope == slope))
		return 0.0f;
	if (!(steep > gentle))
		return (slope <= gentle) ? 1.0f : 0.0f;
	return 1.0f - gxWaterLiveSmooth(gentle, steep, slope);
}

// ---------------------------------------------------------------------------------------------------------------------------
// THE SURF. GeneralsX: The bank alone leaves the line on a moderate grass
// bank (Mini 22/10, Mountain Guns: the stretch at slope under 0.5 kept a continuous cream line at every limit swept): the band
// is narrow by design - a swash line, about 0.5 to 0.85 height units of depth - and on dark grass a narrow pale band IS a drawn
// outline, while on TournamentB's pale sand it reads as the swash wh1ter0se approved. Swash comes from waves breaking: the foam is
// drawn only where the map's own breaking waves are - EA's crests, the map's .wak (W3DWaterTracks loadTracks hands each
// record to the glue) - full within `nearR` world units of a crest's footprint, none beyond `farR`. The footprint is where the
// crest breaks on the beach: its record's start point (WaterTracksObj::init: the start is the wave's furthest reach), across
// the wave's travel by the type's final width, centred (render: the front's origin minus half the width along m_perpDir). The
// record's end point is only a DIRECTION - init normalises end - start - so a long one is the same wave as a unit one and must
// give the same footprint (the raw start-end pair as a shoreline segment gated foam far from the breaker). A pond or a river with no surf
// (every stock map without a .wak, and the ponds and rivers of most with one) shows retail's soft water edge again. TournamentB
// (921 crests, ~20 units apart round the coast): 87% of its foam cells lie within 30 units of a crest, 91% within 60 (measured
// 22/10 from the map and its .wak). A degenerate pair is a hard step at `nearR`; NaN reads as far.
inline float gxWaterLiveSegmentDistance(float px, float py, float ax, float ay, float bx, float by)
{
	const float vx = bx - ax, vy = by - ay;
	const float ll = vx * vx + vy * vy;
	float t = (ll > 0.0f) ? ((px - ax) * vx + (py - ay) * vy) / ll : 0.0f;
	t = (t < 0.0f) ? 0.0f : ((t > 1.0f) ? 1.0f : t);
	const float dx = px - (ax + t * vx), dy = py - (ay + t * vy);
	return sqrtf(dx * dx + dy * dy);
}

// A crest record (start, end: the travel direction) and its type's final width -> its footprint (x0, y0, x1, y1). A zero or NaN
// direction is a point at the start.
inline void gxWaterLiveCrestFootprint(float sx, float sy, float ex, float ey, float width, float out[4])
{
	const float dx = ex - sx, dy = ey - sy;
	const float len = sqrtf(dx * dx + dy * dy);
	float px = 0.0f, py = 0.0f;
	if (len > 0.0f && width > 0.0f)
	{
		px = dy / len * width * 0.5f;	// the travel direction turned a quarter: along the beach
		py = -dx / len * width * 0.5f;
	}
	out[0] = sx - px;
	out[1] = sy - py;
	out[2] = sx + px;
	out[3] = sy + py;
}

inline float gxWaterLiveFoamSurf(float dist, float nearR, float farR)
{
	if (!(dist == dist))
		return 0.0f;
	if (!(farR > nearR))
		return (dist <= nearR) ? 1.0f : 0.0f;
	return 1.0f - gxWaterLiveSmooth(nearR, farR, dist);
}

// ---------------------------------------------------------------------------------------------------------------------------
// THE WATER-LINE SPLIT. A foam triangle's corners: position, depth d (water level minus ground; linear over the triangle)
// coast phase and uphill direction. The part with d >= 0 is drawn flat on the water, the part with d <= 0 on the ground; the cut runs where
// d = 0 (raising each corner to the water separately made the foam float between them). Writes the wet part's
// triangles, then the dry part's, as vertex triples into out (at most 3 triangles, 9 vertices); *wetTris = how many of them
// are wet. A triangle lying exactly on the water line (every d = 0) is ONE wet triangle, never one of each (
// drawn twice it blended twice as opaque). Returns the triangle count.
struct GxFoamPt
{
	float x, y, d, ph;
	float ux, uy;	// the uphill direction (gxWaterLiveUphill), interpolated like the rest
	float shore;	// how much foam this shore holds: the bank factor x the surf factor (THE BANK, THE SURF), interpolated too
};

inline int gxWaterLiveSplitTriangle(const GxFoamPt in[3], GxFoamPt out[9], int *wetTris)
{
	GxFoamPt wet[4], dry[4];
	int nw = 0, nd = 0;
	bool anyPos = false, anyNeg = false;
	for (int k = 0; k < 3; ++k)
	{
		const GxFoamPt &p = in[k];
		const GxFoamPt &n = in[(k + 1) % 3];
		anyPos = anyPos || p.d > 0.0f;
		anyNeg = anyNeg || p.d < 0.0f;
		if (p.d >= 0.0f)
			wet[nw++] = p;
		if (p.d <= 0.0f)
			dry[nd++] = p;
		if ((p.d > 0.0f && n.d < 0.0f) || (p.d < 0.0f && n.d > 0.0f))
		{
			const float t = p.d / (p.d - n.d);
			GxFoamPt x;
			x.x = p.x + t * (n.x - p.x);
			x.y = p.y + t * (n.y - p.y);
			x.d = 0.0f;
			x.ph = p.ph + t * (n.ph - p.ph);
			x.ux = p.ux + t * (n.ux - p.ux);
			x.uy = p.uy + t * (n.uy - p.uy);
			x.shore = p.shore + t * (n.shore - p.shore);
			wet[nw++] = x;
			dry[nd++] = x;
		}
	}
	if (!anyNeg)
		nd = 0;	// nothing below the water line: no dry part (the coplanar triangle is drawn once, as wet)
	else if (!anyPos)
		nw = 0;	// nothing above it: no wet part
	int tris = 0;
	for (int k = 1; k + 1 < nw; ++k, ++tris)
	{
		out[tris * 3 + 0] = wet[0];
		out[tris * 3 + 1] = wet[k];
		out[tris * 3 + 2] = wet[k + 1];
	}
	*wetTris = tris;
	for (int k = 1; k + 1 < nd; ++k, ++tris)
	{
		out[tris * 3 + 0] = dry[0];
		out[tris * 3 + 1] = dry[k];
		out[tris * 3 + 2] = dry[k + 1];
	}
	return tris;
}

// ---------------------------------------------------------------------------------------------------------------------------
// EA'S CRESTS, GROWN IN. GeneralsX:
// Retail (W3DWaterTracks.cpp) gives every crest of a map ONE
// clock (SYNC_WAVES: each bindTrack resets them all) and rides a crest in as a sliver: a Pond crest (TournamentB's 921 are all
// Pond) is 28 x 0.01 wide and 0.18 of that deep - 0.05 world units - while it approaches, and its alpha is already 1 the moment
// it reaches the beach and starts to stretch. So the whole coast drew a sharp full-white line out of nothing in one frame
// (Mini 22/10, frame 855: nothing at 850, the line at 855). With live water's waves part each crest instead
//   - keeps the swell's time: it breaks when the live swell reaches its break point - the lapping foam's own phase there
//     (gxWaterLiveCoastPhase), shifted by a gentle irregularity so a coast square to the flow, which the swell reaches all at
//     once, still breaks in sections - once per lap at each place (whole laps when its life is longer than one);
//   - forms OFFSHORE: it rides in at its retail speed for at least kGxCrestApproachMs as a crest with body, from nothing,
//     building with a smoothstep to kGxCrestBreakAlpha at the break, and whitens to full only through the first part of its
//     surge - no step anywhere;
//   - then surges, stops and fades on retail's own kinematics (the look wh1ter0se chose), its fade eased instead of linear.
// All of it in milliseconds of the live clock (game seconds, a double: gxWaterLiveClockStep), not retail's integer per-draw sum.

static constexpr float kGxCrestApproachMs = 1500.0f;	// the shortest ride in from offshore (retail Pond: 389 ms, 7 units)
static constexpr float kGxCrestBody = 0.2f;				// the crest's depth while riding in, a share of its surge (retail Pond: 0.003)
static constexpr float kGxCrestWidth0 = 0.6f;			// its first width, a share of the final (retail Pond: 0.01, a point)
static constexpr float kGxCrestBreakAlpha = 0.4f;		// its alpha as it breaks: still forming (retail: 1)
static constexpr float kGxCrestWhiten = 0.45f;			// it is fully white this share of the way to rest
static constexpr int kGxCrestMaxLaps = 8;

// One crest's retail motion, as WaterTracksObj::init computes it from waveTypeInfo (times in ms, lengths in world units).
struct GxCrestRetail
{
	float v0;			// its approach speed, world units per ms (m_initialVelocity)
	float frontAcc;		// the front's deceleration once it breaks, units per ms^2, negative (m_frontSlowDownAcc)
	float surge;		// how far the front runs up past the break before it stops (m_waveFinalHeight)
	float body;			// retail's depth while approaching (m_waveInitialHeight)
	float width0;		// retail's first width as a share of the final (m_waveInitialWidth / m_waveFinalWidth)
	float reachMs;		// retail's approach time (m_timeToReachBeach)
	float stopMs;		// from the break to rest (m_timeToStop)
	float compressMs;	// the back keeps coming this long after the front stops (m_timeToCompress)
	float fadeMs;		// the fade after rest (m_fadeMs)
	bool paired;		// its type draws two crests per place (waveTypeInfo m_secondWaveTimeOffset != 0)
	bool second;		// and this is the second of the two (m_initTimeOffset != 0; ignored unless paired)
};

// Where a crest is, measured along its travel from its BREAK point (where retail's front starts to surge): front and tail
// edges (negative: still offshore), width as a share of the final width, alpha 0..1.
struct GxCrestShape
{
	float front;
	float tail;
	float width;
	float alpha;
};

inline float gxWaterLiveCrestApproachMs(const GxCrestRetail &r)
{
	return (r.reachMs > kGxCrestApproachMs) ? r.reachMs : kGxCrestApproachMs;
}

// The crest at tau ms after its break (negative: riding in).
inline GxCrestShape gxWaterLiveCrestShape(const GxCrestRetail &r, float tau)
{
	GxCrestShape c = { 0.0f, 0.0f, 1.0f, 0.0f };
	const float ta = gxWaterLiveCrestApproachMs(r);
	const float body = (r.body > kGxCrestBody * r.surge) ? r.body : kGxCrestBody * r.surge;
	if (tau < 0.0f)
	{
		const float s = gxWaterLiveSmooth(-ta, 0.0f, tau);
		const float w0 = (r.width0 > kGxCrestWidth0) ? r.width0 : kGxCrestWidth0;
		c.front = r.v0 * tau;
		c.tail = c.front - body;
		c.width = w0 + (1.0f - w0) * s;
		c.alpha = kGxCrestBreakAlpha * s;
		return c;
	}
	// retail's surge, its back accelerating from `body` behind the break so it reaches the break as the front stops
	const float backAcc = (r.stopMs > 0.0f) ? 2.0f * body / (r.stopMs * r.stopMs) : 0.0f;
	c.front = r.v0 * tau + 0.5f * r.frontAcc * tau * tau;
	const float tc = r.stopMs + r.compressMs;
	if (tau > tc)
		c.tail = -body + 0.5f * backAcc * tc * tc + 0.5f * r.frontAcc * (tau - tc) * (tau - tc);
	else
		c.tail = -body + 0.5f * backAcc * tau * tau;
	if (c.tail > c.front)
		c.tail = c.front;	// never turned inside out (retail's back can pass its retreating front after the fade)
	const float tw = kGxCrestWhiten * r.stopMs;
	if (tau < tw)
		c.alpha = kGxCrestBreakAlpha + (1.0f - kGxCrestBreakAlpha) * gxWaterLiveSmooth(0.0f, tw, tau);
	else if (tau < r.stopMs)
		c.alpha = 1.0f;
	else
		c.alpha = 1.0f - gxWaterLiveSmooth(r.stopMs, r.stopMs + r.fadeMs, tau);
	return c;
}

// How many laps one break of this crest takes: one per lap at each place, but whole laps until its life (ride in, surge to
// rest, fade) fits; a pair gets an even count, so its second crest - half a period later - also breaks on the swell.
inline int gxWaterLiveCrestLaps(const GxCrestRetail &r, float lapSeconds)
{
	const float lap = (lapSeconds > 0.0f) ? lapSeconds * 1000.0f : 7000.0f;
	const float life = gxWaterLiveCrestApproachMs(r) + r.stopMs + r.fadeMs;
	int k = (int)ceilf(life / lap - 1e-4f);
	if (k < 1)
		k = 1;
	if (r.paired && (k & 1))
		++k;
	return (k > kGxCrestMaxLaps) ? kGxCrestMaxLaps : k;
}

// A gentle irregularity of when a coast breaks, in laps: sections a few hundred units long break up to ~0.08 lap early or
// late. Its gradient stays under ~0.0015 laps per world unit, so crests 20 units apart (TournamentB's spacing) break within
// ~0.03 lap (0.2 s) of each other and the line stays whole while it peels.
inline float gxWaterLiveCrestIrregular(float x, float y)
{
	return 0.05f * sinf(0.015f * x + 0.009f * y + 0.7f) + 0.035f * sinf(-0.007f * x + 0.014f * y + 2.1f);
}

// Milliseconds since this crest's last break (negative: the next one is close enough that it is riding in), at `seconds` of
// live time, for a crest breaking at (x, y). A break comes when seconds / lap + coast phase + irregularity is a whole number of
// laps (the second of a pair: half a period later), so it breaks as the swell - and the lapping foam - reaches it.
inline float gxWaterLiveCrestTau(const GxCrestRetail &r, double seconds, float lapSeconds, const GxWaterLiveFlow &f, float x, float y)
{
	const double lap = (lapSeconds > 0.0f) ? (double)lapSeconds : 7.0;
	const int k = gxWaterLiveCrestLaps(r, (float)lap);
	double c = (seconds / lap + (double)gxWaterLiveCoastPhase(x, y, f) + (double)gxWaterLiveCrestIrregular(x, y)) / (double)k
		+ ((r.paired && r.second) ? 0.5 : 0.0);
	c -= floor(c);
	const double period = lap * (double)k * 1000.0;
	double tau = c * period;
	if (tau >= period - (double)gxWaterLiveCrestApproachMs(r))
		tau -= period;
	return (float)tau;
}

inline GxCrestShape gxWaterLiveCrestAt(const GxCrestRetail &r, double seconds, float lapSeconds, const GxWaterLiveFlow &f, float x,
	float y)
{
	return gxWaterLiveCrestShape(r, gxWaterLiveCrestTau(r, seconds, lapSeconds, f, x, y));
}

// ---------------------------------------------------------------------------------------------------------------------------
// THE TWO PIXEL SHADERS, ps.1.1 bytecode written by hand in the token format the retail three use (W3DWater.cpp
// ReAcquireResources): DXVK's d3d8 front end takes the same tokens on every platform. Token fields (D3D8 shader format):
//   instruction  opcode in bits 0-15 (0x42 tex, 0x04 mad, 0x05 mul, 0x02 add), 0x40000000 = co-issued with the previous one
//   destination  0x80000000 | type << 28 (0 temp r, 3 texture t) | shift << 24 (2 = _x4) | mask << 16 (7 rgb, 8 a, f all) | n
//   source       0x80000000 | type << 28 (0 r, 1 input v, 2 const c, 3 t) | swizzle << 16 (0xe4 rgba, 0xaa bbbb) | n
// The source text is beside each array; scripts/test/look-water-live-test.cpp decodes the tokens back to that text.

// Standing water's glints, a SECOND draw of the same mesh ADDED on top (blend ONE, ONE) after retail's own pass (stages as
// setupFlatWaterShader leaves them: t1 WaterSurfaceBubbles, t2 Noise0000, t3 shroud or white; c1 glint colour, c2 sheen).
// Added, not mixed into retail's pass: that pass is blended by the clear-water opacity (destination alpha), which faded the
// glints with the water's clarity - on the Mini 17/10 they ended up FAINTER than retail's own sparkles. A glint is a
// reflection on the surface; it does not care how clear the water under it is.
//   ps.1.1
//   tex t1 / tex t2 / tex t3
//   mul r1, t1, t2            ; specks where the noise is bright (retail adds this, x1, into its own pass)
//   mul_x4 r1, r1, r1         ; squared then x4: narrow bright peaks - sparkle, not soft blobs (Mini 17/10)
//   mad r1.rgb, t2, c2, r1    ; + the noise's own moving sheen
//   mul r1.rgb, r1, c1        ; x the sun's colour x strength
//   mul r0.rgb, r1, t3        ; under the shroud
//   +mov r0.a, c1             ; (alpha unused by ONE, ONE)
static const unsigned kGxWaterGlareShader[] = {
	0xffff0101,
	0x00000042, 0xb00f0001, 0x00000042, 0xb00f0002, 0x00000042, 0xb00f0003,
	0x00000005, 0x800f0001, 0xb0e40001, 0xb0e40002,
	0x00000005, 0x820f0001, 0x80e40001, 0x80e40001,
	0x00000004, 0x80070001, 0xb0e40002, 0xa0e40002, 0x80e40001,
	0x00000005, 0x80070001, 0x80e40001, 0xa0e40001,
	0x00000005, 0x80070000, 0x80e40001, 0xb0e40003,
	0x40000001, 0x80080000, 0xa0e40001,
	0x0000ffff
};

// Shoreline foam (stages: t0 the foam table, t1 Noise0000, t2 WaterSurfaceBubbles, t3 shroud or white; c1/c2 the breakup
// scale and floor; v0 = the lit colour, its alpha the strength):
//   ps.1.1
//   tex t0 / tex t1 / tex t2 / tex t3
//   mad r1, t1, c1, c2        ; breakup = noise x c1 + c2 (patchy, never fully off)
//   add r1, r1, t2            ; + bubble specks
//   mul r0.rgb, v0, t3        ; foam colour: the map's light under the shroud
//   +mul r0.a, t0, r1.b       ; foam amount: the table (depth x phase) x the breakup
//   mul r0.a, r0, v0          ; x strength
static const unsigned kGxWaterFoamShader[] = {
	0xffff0101,
	0x00000042, 0xb00f0000, 0x00000042, 0xb00f0001, 0x00000042, 0xb00f0002, 0x00000042, 0xb00f0003,
	0x00000004, 0x800f0001, 0xb0e40001, 0xa0e40001, 0xa0e40002,
	0x00000002, 0x800f0001, 0x80e40001, 0xb0e40002,
	0x00000005, 0x80070000, 0x90e40000, 0xb0e40003,
	0x40000005, 0x80080000, 0xb0e40000, 0x80aa0001,
	0x00000005, 0x80080000, 0x80e40000, 0x90e40000,
	0x0000ffff
};

// ---------------------------------------------------------------------------------------------------------------------------
// The engine glue (GXWaterLiveGlue.inl, compiled into W3DWater.cpp). Plain types only, so this header stays engine-free.

// W3DWaterTracks.cpp flush asks once per frame: whether live water draws the crests (lit, their alpha gained, grown in on the
// swell's time - EA'S CRESTS above), the colour to use (0xAARRGGBB, alpha ignored), the crest alpha gain (1, 2 or 4), and the
// live clock, lap and flow the crests keep time with.
struct GxWaterLiveWaveFrame
{
	bool on;
	unsigned rgb;
	int alphaGain;
	double seconds;
	float lap;
	GxWaterLiveFlow flow;
};
void gxWaterLiveWaveFrame(GxWaterLiveWaveFrame *out);

// W3DWaterTracks loadTracks, at every map load: the map's crests are about to be read (none if it has no .wak) ...
void gxWaterLiveCrestsBegin();
// ... and one crest record: the .wak's start and end points (the end only sets the direction) and its type's final width (THE SURF).
void gxWaterLiveCrest(float sx, float sy, float ex, float ey, float width);

// BaseHeightMapRenderObjClass::flushTerrainHeightChanges reports each rectangle of grid vertices (inclusive) whose ground
// moved (a structure's foundation, a script): the foam cells there are scanned again at the next draw.
void gxWaterLiveTerrainChanged(int x0, int y0, int x1, int y1);

// BaseHeightMapRenderObjClass::staticLightingChanged: the whole terrain is rebuilt - a loaded save's heights restored
// (W3DTerrainVisual::xfer writes them without dirty rectangles), a new time of day, a new map: every foam cell is scanned
// again at the next draw.
void gxWaterLiveTerrainReloaded();
