// GeneralsX: LIVE WATER - the engine glue (the look and its arithmetic: GameClient/GXWaterLive.h).
//
// Included at the end of W3DWater.cpp, below every GX-SIMID region (and below GXWaterClearGlue.inl). Called from render code
// outside the SimID closure: WaterRenderObjClass's ReAcquireResources / ReleaseResources (device resources), load (the map's
// foam cells), update (the clock), drawTrapezoidWater (the standing water's coordinates and glints), Render (the foam pass),
// the destructor, and skyBoxMapReset (W3DDisplay::reset); W3DWaterTracks.cpp's flush asks gxWaterLiveWaveFrame. Every
// one of them only reads the map (heights, water polygons, lights) and writes render state, vertex buffers and textures of
// its own. W3DWater.h is digested whole, so nothing here is a class member: the state below is file-local.

#include "Common/OptionPreferences.h"
#include "Common/MapObject.h"
#include "GameLogic/PolygonTrigger.h"
#include "W3DDevice/GameClient/WorldHeightMap.h"
#include "W3DDevice/GameClient/W3DShroud.h"

#include <chrono>
#include <vector>

namespace
{
	Bool s_gxLiveOn = FALSE;						// the option, as read at the last map load
	Int s_gxLiveParts = kGxWaterLiveAll;
	Real s_gxLiveStrength = 1.0f;
	GxWaterLiveTune s_gxLiveTune = gxWaterLiveDefaultTune();
	const char *s_gxLiveSource = "default";
	double s_gxLiveClock = 0.0;						// game seconds since the map loaded (gxWaterLiveClockStep)
	Real s_gxSwellGain = 1.0f;						// the swell's one gain for this map (gxWaterLiveMeasureSwell)

	DWORD s_gxGlareShader = 0;						// ps.1.1 handles, made beside the retail three
	DWORD s_gxFoamShader = 0;
	TextureClass *s_gxFoamTable = nullptr;			// 64 x 64: depth x lapping phase -> foam opacity

	// One foam cell = one terrain cell within the foam's depth range: its corners in shoreline order (BaseHeightMap
	// updateShorelineTile) (i,j) (i+1,j) (i+1,j+1) (i,j+1) and the water level over it. The draw splits each of its two
	// triangles at the water line - the wet part flat on the water, the dry part on the ground - because a triangle whose
	// corners were each raised to the water separately floated above both between them.
	struct GxFoamCorner
	{
		float x, y;
		float ground;	// terrain height
		float phase;	// the coast phase here (gxWaterLiveCoastPhase), cycles
		float ux, uy;	// the uphill direction here (gxWaterLiveUphill): the way a surge carries the foam
		float shore;	// how much foam this shore holds: the bank factor x the surf factor (GXWaterLive.h THE BANK, THE SURF)
	};
	struct GxFoamCell
	{
		Short i, j;
		Bool flip;		// the terrain's own triangulation (WorldHeightMap::getQuickFlipState)
		Bool river;		// its water is a river polygon (skipped while the deformable grid replaces rivers)
		float level;	// the water's height over the cell
		GxFoamCorner c[4];
	};
	struct GxFoamRect
	{
		Int x0, y0, x1, y1;	// grid vertices, inclusive
	};
	Bool s_gxFoamStale = TRUE;						// rebuild everything at the next draw
	Bool s_gxFoamMapLoaded = FALSE;
}

// The containers, made on first use and never destroyed: a file-scope std::vector has a destructor, and its registration made
// this whole file's static initialisation run at start-up instead of being folded to constants by the compiler - which turned
// retail's zFillAlphaShader into a runtime-initialised global that WaterRenderObjClass::init's region READS, a SimID closure
// gap (simid-closure.py, 17/10, build f302e7220: "data from W3DWater.cpp -> zFillAlphaShader").
static std::vector<GxFoamCell> &gxFoamCells() { static std::vector<GxFoamCell> *v = new std::vector<GxFoamCell>(); return *v; }
static std::vector<Real> &gxFoamWaterLevels() { static std::vector<Real> *v = new std::vector<Real>(); return *v; }	// the water polygons' levels the cells were built for, in list order
static std::vector<GxFoamRect> &gxFoamDirty() { static std::vector<GxFoamRect> *v = new std::vector<GxFoamRect>(); return *v; }	// ground changed here since the scan (flushTerrainHeightChanges)
// THE SURF: the map's crests (x0, y0, x1, y1 per crest: its footprint, gxWaterLiveCrestFootprint), and per grid
// vertex the distance to the nearest of them, capped at the surf's far limit (built with the full scan; the crests never move).
static std::vector<float> &gxCrestLines() { static std::vector<float> *v = new std::vector<float>(); return *v; }
static std::vector<float> &gxSurfDistance() { static std::vector<float> *v = new std::vector<float>(); return *v; }

// The water polygons' levels, in list order: a script that raises or lowers a water area changes one, and the foam cells are
// rebuilt for the new shore line. Compared level by level (a weighted sum of them could cancel out). A handful
// of polygon triggers per map; cheap to read every frame.
static void gxWaterLiveWaterLevels(std::vector<Real> &out)
{
	out.clear();
	for (PolygonTrigger *p = PolygonTrigger::getFirstPolygonTrigger(); p; p = p->getNext())
		if (p->isWaterArea() && p->getNumPoints() >= 1)
			out.push_back((Real)p->getPoint(0)->z);
}

static Bool gxWaterLiveWaterMoved()
{
	static std::vector<Real> *s_now = new std::vector<Real>();
	gxWaterLiveWaterLevels(*s_now);
	return *s_now != gxFoamWaterLevels();
}

// Foam only within this many cells (10 world units each) of dry ground: a shallow sea is "foam depth" everywhere (TournamentB:
// 75,465 cells, the whole sea), and the shore is where foam belongs. Measured on the Mini 17/10.
static const Int kGxFoamShoreCells = 5;

// Milliseconds on the wall clock (gxRenderNowMs is the GX_SHOT capture clock under capture, which stands still here).
static UnsignedInt gxWaterLiveWallMs()
{
	return (UnsignedInt)std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Whether the water at (x, y) with level `lv` is a river polygon's. A WaveGuide's deformable grid replaces the rivers while it
// runs (WaterRenderObjClass m_disableRiver): their fixed polygon level is then not the surface.
static Bool gxWaterLiveRiverAt(Real x, Real y, Real lv)
{
	ICoord3D at;
	at.x = REAL_TO_INT_FLOOR(x + 0.5f);
	at.y = REAL_TO_INT_FLOOR(y + 0.5f);
	at.z = 0;
	for (PolygonTrigger *p = PolygonTrigger::getFirstPolygonTrigger(); p; p = p->getNext())
		if (p->isWaterArea() && p->isRiver() && p->getNumPoints() >= 1 && (Real)p->getPoint(0)->z == lv && p->pointInTrigger(at))
			return TRUE;
	return FALSE;
}

// Scans the cells whose lower-left vertex is in [i0, i1) x [j0, j1) (clamped to the map) and appends the foam ones.
static void gxWaterLiveScanFoamCells(Int i0, Int j0, Int i1, Int j1)
{
	WorldHeightMap *map = TheTerrainRenderObject->getMap();
	const Int border = map->getBorderSizeInline();
	const Int w = map->getXExtent();
	const Int h = map->getYExtent();
	const GxWaterLiveFlow flow = gxWaterLiveFlowDir(s_gxLiveTune.dir);
	i0 = (i0 < 0) ? 0 : i0;
	j0 = (j0 < 0) ? 0 : j0;
	i1 = (i1 > w - 1) ? w - 1 : i1;
	j1 = (j1 > h - 1) ? h - 1 : j1;
	if (i1 <= i0 || j1 <= j0)
		return;

	// The vertices read: the range, plus the shore margin around it (a cell's nearest dry ground may lie outside the range).
	const Int R = kGxFoamShoreCells;
	const Int ei0 = (i0 - R < 0) ? 0 : i0 - R, ej0 = (j0 - R < 0) ? 0 : j0 - R;
	const Int ei1 = (i1 + R > w - 1) ? w - 1 : i1 + R, ej1 = (j1 + R > h - 1) ? h - 1 : j1 + R;
	const Int vw = ei1 - ei0 + 1, vh = ej1 - ej0 + 1;
	// The water level at every vertex, once (4 corners per cell would ask 4 times), and whether the ground there is dry.
	std::vector<Real> level((size_t)vw * (size_t)vh);
	std::vector<UnsignedByte> dry((size_t)vw * (size_t)vh);
	for (Int j = ej0; j <= ej1; ++j)
		for (Int i = ei0; i <= ei1; ++i)
		{
			const size_t at = (size_t)(j - ej0) * vw + (i - ei0);
			level[at] = TheWaterRenderObj->getWaterHeight((i - border) * MAP_XY_FACTOR, (j - border) * MAP_XY_FACTOR);
			dry[at] = (level[at] <= INVALID_WATER_HEIGHT || MAP_HEIGHT_SCALE * map->getHeight(i, j) >= level[at]) ? 1 : 0;
		}
	// Dilate "dry" by R in both directions (separable max): nearShore[v] = some dry vertex within R cells of v.
	std::vector<UnsignedByte> rows((size_t)vw * (size_t)vh), nearShore((size_t)vw * (size_t)vh);
	for (Int y = 0; y < vh; ++y)
		for (Int x = 0; x < vw; ++x)
		{
			UnsignedByte m = 0;
			for (Int k = (x - R < 0 ? 0 : x - R); k <= (x + R > vw - 1 ? vw - 1 : x + R) && !m; ++k)
				m = dry[(size_t)y * vw + k];
			rows[(size_t)y * vw + x] = m;
		}
	for (Int y = 0; y < vh; ++y)
		for (Int x = 0; x < vw; ++x)
		{
			UnsignedByte m = 0;
			for (Int k = (y - R < 0 ? 0 : y - R); k <= (y + R > vh - 1 ? vh - 1 : y + R) && !m; ++k)
				m = rows[(size_t)k * vw + x];
			nearShore[(size_t)y * vw + x] = m;
		}

	static const Int kDi[4] = { 0, 1, 1, 0 };
	static const Int kDj[4] = { 0, 0, 1, 1 };
	for (Int j = j0; j < j1; ++j)
	{
		for (Int i = i0; i < i1; ++i)
		{
			if (!nearShore[(size_t)(j - ej0) * vw + (i - ei0)])
				continue;	// open water, far from any shore
			// Every corner inside a water area, as BaseHeightMap::updateShorelineTile requires of its shoreline tiles: a cell
			// the polygon only partly covers would carry the water's level past the polygon's edge, where no water is drawn
			// And one water surface: a cell between two water areas at different levels belongs to neither
			//
			Real lv = INVALID_WATER_HEIGHT, lo = 1e9f;
			Bool covered = TRUE;
			for (Int k = 0; k < 4; ++k)
			{
				const Real l = level[(size_t)(j + kDj[k] - ej0) * vw + (i + kDi[k] - ei0)];
				covered = covered && l > INVALID_WATER_HEIGHT;
				lv = (l > lv) ? l : lv;
				lo = (l < lo) ? l : lo;
			}
			if (!covered || lv - lo > 0.01f)
				continue;
			GxFoamCell cell;
			Real minD = 1e9f, maxD = -1e9f;
			for (Int k = 0; k < 4; ++k)
			{
				GxFoamCorner &c = cell.c[k];
				c.ground = MAP_HEIGHT_SCALE * map->getHeight(i + kDi[k], j + kDj[k]);
				const Real d = lv - c.ground;
				minD = (d < minD) ? d : minD;
				maxD = (d > maxD) ? d : maxD;
			}
			if (maxD <= kGxFoamDepthLo || minD >= kGxFoamDepthHi)
				continue;	// all of it high and dry above the highest surge, or all of it deeper than the incoming line
			cell.i = (Short)i;
			cell.j = (Short)j;
			cell.flip = map->getQuickFlipState(i, j);
			cell.river = gxWaterLiveRiverAt((i - border + 0.5f) * MAP_XY_FACTOR, (j - border + 0.5f) * MAP_XY_FACTOR, lv);
			cell.level = lv;
			float shoreMax = 0.0f;
			for (Int k = 0; k < 4; ++k)
			{
				GxFoamCorner &c = cell.c[k];
				const Int vx = i + kDi[k], vy = j + kDj[k];
				c.x = (vx - border) * MAP_XY_FACTOR;
				c.y = (vy - border) * MAP_XY_FACTOR;
				c.phase = gxWaterLiveCoastPhase(c.x, c.y, flow);
				// Central differences of the ground at this vertex (one-sided at a map edge): a vertex shared by two cells gets one
				// direction, so the carried pattern never tears along a cell edge.
				const Int xl = (vx > 0) ? vx - 1 : vx, xr = (vx < w - 1) ? vx + 1 : vx;
				const Int yd = (vy > 0) ? vy - 1 : vy, yu = (vy < h - 1) ? vy + 1 : vy;
				float hx = 0.0f, hy = 0.0f;
				const float dhx = MAP_HEIGHT_SCALE * (map->getHeight(xr, vy) - map->getHeight(xl, vy));
				const float dhy = MAP_HEIGHT_SCALE * (map->getHeight(vx, yu) - map->getHeight(vx, yd));
				gxWaterLiveUphill(dhx, (xr - xl) * MAP_XY_FACTOR, dhy, (yu - yd) * MAP_XY_FACTOR, &hx, &hy);
				c.ux = hx;
				c.uy = hy;
				// The steepest terrain triangle of the cells sharing this vertex (THE BANK, GXWaterLive.h).
				float slope = 0.0f;
				for (Int ci = vx - 1; ci <= vx; ++ci)
					for (Int cj = vy - 1; cj <= vy; ++cj)
						if (ci >= 0 && cj >= 0 && ci < w - 1 && cj < h - 1)
						{
							const float s = gxWaterLiveCellSlope(MAP_HEIGHT_SCALE * map->getHeight(ci, cj),
								MAP_HEIGHT_SCALE * map->getHeight(ci + 1, cj), MAP_HEIGHT_SCALE * map->getHeight(ci + 1, cj + 1),
								MAP_HEIGHT_SCALE * map->getHeight(ci, cj + 1), MAP_XY_FACTOR);
							slope = (s > slope || !(s == s)) ? s : slope;
						}
				const size_t sv = (size_t)vy * (size_t)w + (size_t)vx;
				const float surf = (sv < gxSurfDistance().size())
					? gxWaterLiveFoamSurf(gxSurfDistance()[sv], s_gxLiveTune.surf0, s_gxLiveTune.surf1) : 0.0f;
				c.shore = gxWaterLiveFoamBank(slope, s_gxLiveTune.bank0, s_gxLiveTune.bank1) * surf;
				shoreMax = (c.shore > shoreMax) ? c.shore : shoreMax;
			}
			if (!(shoreMax > 0.0f))
				continue;	// a cut bank, or no surf, all round: no foam anywhere in this cell (THE BANK, THE SURF)
			gxFoamCells().push_back(cell);
		}
	}
}

// THE SURF's distance field: every grid vertex's distance to the nearest crest line, capped at the far limit (a vertex beyond it
// takes no foam either way). Each crest touches only the vertices of its own box grown by that limit.
static void gxWaterLiveBuildSurf(WorldHeightMap *map)
{
	const Int w = map->getXExtent(), h = map->getYExtent(), border = map->getBorderSizeInline();
	const float farR = (s_gxLiveTune.surf1 > s_gxLiveTune.surf0) ? s_gxLiveTune.surf1 : s_gxLiveTune.surf0;
	std::vector<float> &field = gxSurfDistance();
	field.assign((size_t)w * (size_t)h, farR + 1.0f);
	const std::vector<float> &lines = gxCrestLines();
	for (size_t k = 0; k + 3 < lines.size(); k += 4)
	{
		const float ax = lines[k], ay = lines[k + 1], bx = lines[k + 2], by = lines[k + 3];
		const float lx = (ax < bx ? ax : bx) - farR, hx = (ax > bx ? ax : bx) + farR;
		const float ly = (ay < by ? ay : by) - farR, hy = (ay > by ? ay : by) + farR;
		Int i0 = (Int)floorf(lx / MAP_XY_FACTOR) + border, i1 = (Int)ceilf(hx / MAP_XY_FACTOR) + border;
		Int j0 = (Int)floorf(ly / MAP_XY_FACTOR) + border, j1 = (Int)ceilf(hy / MAP_XY_FACTOR) + border;
		i0 = (i0 < 0) ? 0 : i0;
		j0 = (j0 < 0) ? 0 : j0;
		i1 = (i1 > w - 1) ? w - 1 : i1;
		j1 = (j1 > h - 1) ? h - 1 : j1;
		for (Int j = j0; j <= j1; ++j)
			for (Int i = i0; i <= i1; ++i)
			{
				const float d = gxWaterLiveSegmentDistance((i - border) * MAP_XY_FACTOR, (j - border) * MAP_XY_FACTOR, ax, ay, bx, by);
				float &f = field[(size_t)j * (size_t)w + (size_t)i];
				f = (d < f) ? d : f;
			}
	}
}

void gxWaterLiveCrestsBegin()
{
	gxCrestLines().clear();
	s_gxFoamStale = TRUE;	// the surf moved: the next draw scans again
}

void gxWaterLiveCrest(float sx, float sy, float ex, float ey, float width)
{
	float f[4];
	gxWaterLiveCrestFootprint(sx, sy, ex, ey, width, f);
	if (!(f[0] == f[0] && f[1] == f[1] && f[2] == f[2] && f[3] == f[3]))
		return;	// a .wak comes with the map from the lobby host: a NaN record is no surf
	std::vector<float> &lines = gxCrestLines();
	lines.insert(lines.end(), f, f + 4);
}

static Bool gxWaterLiveFoamReady()
{
	return TheTerrainRenderObject != nullptr && TheTerrainRenderObject->getMap() != nullptr && TheWaterRenderObj != nullptr;
}

static void gxWaterLiveBuildFoamCells(const char *why)
{
	gxFoamCells().clear();
	gxFoamDirty().clear();
	s_gxFoamStale = FALSE;
	gxWaterLiveWaterLevels(gxFoamWaterLevels());
	if (!gxWaterLiveFoamReady() || gxFoamWaterLevels().empty())
		return;	// no water on this map: nothing to scan
	const UnsignedInt t0 = gxWaterLiveWallMs();
	WorldHeightMap *map = TheTerrainRenderObject->getMap();
	gxWaterLiveBuildSurf(map);
	gxWaterLiveScanFoamCells(0, 0, map->getXExtent() - 1, map->getYExtent() - 1);
	fprintf(stderr, "[GXWATER] live foam cells=%d grid=%dx%d crests=%d build_ms=%u why=%s\n", (int)gxFoamCells().size(),
		(int)map->getXExtent(), (int)map->getYExtent(), (int)(gxCrestLines().size() / 4), (unsigned)(gxWaterLiveWallMs() - t0), why);
}

// The ground changed (a structure's foundation, a script): the cells touching the changed vertices are scanned again - and
// every cell within the shore margin of them, because whether a cell is near dry ground depends on vertices up to
// kGxFoamShoreCells away. A changed vertex (x, y) belongs to the cells (x-1..x, y-1..y).
static void gxWaterLiveRescanDirty()
{
	if (gxFoamDirty().empty() || !gxWaterLiveFoamReady())
		return;
	const UnsignedInt t0 = gxWaterLiveWallMs();
	size_t before = gxFoamCells().size(), rects = gxFoamDirty().size();
	for (const GxFoamRect &r : gxFoamDirty())
	{
		const Int R = kGxFoamShoreCells;	// cells touching the rectangle plus the shore margin, inclusive
		const Int ci0 = r.x0 - 1 - R, cj0 = r.y0 - 1 - R, ci1 = r.x1 + R, cj1 = r.y1 + R;
		size_t kept = 0;
		for (size_t k = 0; k < gxFoamCells().size(); ++k)
		{
			const GxFoamCell &c = gxFoamCells()[k];
			if (c.i >= ci0 && c.i <= ci1 && c.j >= cj0 && c.j <= cj1)
				continue;
			if (kept != k)
				gxFoamCells()[kept] = c;
			++kept;
		}
		gxFoamCells().resize(kept);
		gxWaterLiveScanFoamCells(ci0, cj0, ci1 + 1, cj1 + 1);
	}
	gxFoamDirty().clear();
	fprintf(stderr, "[GXWATER] live foam rescan rects=%d cells=%d->%d ms=%u\n", (int)rects, (int)before,
		(int)gxFoamCells().size(), (unsigned)(gxWaterLiveWallMs() - t0));
}

void gxWaterLiveTerrainReloaded()
{
	if (s_gxFoamMapLoaded)
		s_gxFoamStale = TRUE;
}

void gxWaterLiveTerrainChanged(int x0, int y0, int x1, int y1)
{
	if (!s_gxFoamMapLoaded || !s_gxLiveOn || (s_gxLiveParts & kGxWaterLiveFoam) == 0)
		return;
	if (gxFoamDirty().size() >= 256)
	{	// a flood of edits: one full rescan is cheaper than many partial ones
		s_gxFoamStale = TRUE;
		gxFoamDirty().clear();
		return;
	}
	GxFoamRect r = { x0, y0, x1, y1 };
	gxFoamDirty().push_back(r);
}

// The foam table, written into the texture whenever it is (re)made.
static void gxWaterLiveFillFoamTable()
{
	if (s_gxFoamTable == nullptr)
		return;
	SurfaceClass *surf = s_gxFoamTable->Get_Surface_Level();
	if (surf == nullptr)
		return;
	int pitch = 0;
	UnsignedByte *bits = (UnsignedByte *)surf->Lock(&pitch);
	if (bits != nullptr)
	{
		for (Int j = 0; j < kGxFoamTableSize; ++j)
		{
			UnsignedInt *row = (UnsignedInt *)(bits + (size_t)j * (size_t)pitch);
			for (Int i = 0; i < kGxFoamTableSize; ++i)
			{
				const UnsignedInt a = (UnsignedInt)(gxWaterLiveFoamTexel(i, j) * 255.0f + 0.5f);
				row[i] = (a << 24) | 0x00ffffff;
			}
		}
		surf->Unlock();
	}
	REF_PTR_RELEASE(surf);
}

static void gxWaterLiveMakeFoamTable()
{
	if (s_gxFoamTable != nullptr)
		return;
	s_gxFoamTable = MSGNEW("TextureClass") TextureClass(kGxFoamTableSize, kGxFoamTableSize, WW3D_FORMAT_A8R8G8B8, MIP_LEVELS_1);
	s_gxFoamTable->Get_Filter().Set_U_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_CLAMP);	// depth: clamp at both ends
	s_gxFoamTable->Get_Filter().Set_V_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_REPEAT);	// phase: wraps every cycle
	s_gxFoamTable->Get_Filter().Set_Mag_Filter(TextureFilterClass::FILTER_TYPE_DEFAULT);
	s_gxFoamTable->Get_Filter().Set_Min_Filter(TextureFilterClass::FILTER_TYPE_DEFAULT);
	s_gxFoamTable->Get_Filter().Set_Mip_Mapping(TextureFilterClass::FILTER_TYPE_NONE);
	gxWaterLiveFillFoamTable();
}

// The swell's gain for the whole map: the worst standing-water patch's, over the same patches renderWater hands
// drawTrapezoidWater - enough vertices per wavelength (gxWaterLiveSurgeGain) and a seam between differently subdivided
// neighbours under kGxSwellSeamMax (gxWaterLiveSeamGain, at this map's surge). One value for every patch.
// The polygons' shapes do not change after load (scripts move levels, not corners).
static void gxWaterLiveMeasureSwell()
{
	const Real surge = gxWaterLiveUV(0.0, kGxWaterLiveMotion, s_gxLiveStrength, s_gxLiveTune).surge;
	Real sampling = 1.0f, seam = 1.0f, worst = 0.0f;
	Int patches = 0;
	for (PolygonTrigger *p = PolygonTrigger::getFirstPolygonTrigger(); p; p = p->getNext())
	{
		if (!p->isWaterArea() || p->isRiver() || p->getNumPoints() <= 2)
			continue;
		for (Int k = 1; k < p->getNumPoints() - 1; k += 2)
		{
			// renderWater's corners: pt3 = point 0, pt2 = point k, pt1 = point k+1, pt0 = point k+2 (or k+1 at an odd end)
			const ICoord3D *c3 = p->getPoint(0), *c2 = p->getPoint(k), *c1 = p->getPoint(k + 1);
			const ICoord3D *c0 = (k + 2 < p->getNumPoints()) ? p->getPoint(k + 2) : c1;
			const float pts[4][2] = { { (float)c0->x, (float)c0->y }, { (float)c1->x, (float)c1->y }, { (float)c2->x, (float)c2->y },
				{ (float)c3->x, (float)c3->y } };
			const float s = gxWaterLiveTrapezoidSpacing(pts);
			worst = (s > worst) ? s : worst;
			const Real g = gxWaterLiveSurgeGain(s), m = gxWaterLiveSeamGain(s, surge);
			sampling = (g < sampling) ? g : sampling;
			seam = (m < seam) ? m : seam;
			++patches;
		}
	}
	// A map with one patch has no shared edge: no seam to bound.
	const Real gain = sampling * ((patches > 1) ? seam : 1.0f);
	s_gxSwellGain = gain;
	fprintf(stderr, "[GXWATER] live swell gain=%.2f (sampling %.2f, seam %.2f%s) surge=%.2f worst_spacing=%.1f patches=%d\n", gain,
		sampling, seam, (patches > 1) ? "" : " unused: one patch", surge, worst, patches);
}

// Options.ini's GXWaterLive / GXWaterLiveStrength, then the A/B variables over them. Every map load.
static void gxWaterLiveReadSettings()
{
	OptionPreferences prefs;
	const OptionPreferences::const_iterator key = prefs.find("GXWaterLive");	// absent = ON from 0.9.66 (GXWaterLive.h)
	s_gxLiveOn = gxWaterLiveOptionOn((key != prefs.end()) ? key->second.str() : nullptr);
	s_gxLiveSource = (key != prefs.end()) ? "Options.ini" : "default";
	s_gxLiveStrength = gxWaterLiveClamp(prefs.getReal("GXWaterLiveStrength", 1.0f), 0.0f, 1.0f);
	const Int env = gxWaterLiveParseBool(getenv("GX_WATER_LIVE"));
	if (env >= 0)
	{
		s_gxLiveOn = (env == 1);
		s_gxLiveSource = "GX_WATER_LIVE";
	}
	s_gxLiveStrength = gxWaterLiveParseReal(getenv("GX_WATER_LIVE_STRENGTH"), s_gxLiveStrength, 0.0f, 1.0f);
	s_gxLiveParts = gxWaterLiveParseParts(getenv("GX_WATER_LIVE_PARTS"));
	s_gxLiveTune = gxWaterLiveDefaultTune();
	gxWaterLiveParseTune(getenv("GX_WATER_LIVE_TUNE"), &s_gxLiveTune);
}

static Bool gxWaterLivePart(Int part)
{
	return s_gxLiveOn && (s_gxLiveParts & part) != 0 && s_gxLiveStrength > 0.0f;
}

// ---------------------------------------------------------------------------------------------------------------------------
// Device resources (ReAcquireResources / ReleaseResources, and the map load that reads the option), beside the retail three.
// The two shaders exist ONLY while the option is on: off, the game creates exactly retail's bytecode. The native renderer picks
// its ports by bytecode (GXRender/Pipeline/GxCustomCatalog.h) and draws both: water:glare / water:foam, gx_cs_ps.frag cases 16 / 17
// (R-W1, E · water-live-native-port 21/10/2026). A change to either array (GameClient/GXWaterLive.h) needs the catalogue regenerated
// (scripts/render/ws-E/gen_catalog.py; gx_cs_test fails until it is): a native device REFUSES every draw with an unknown program.

static void gxWaterLiveRelease();

static void gxWaterLiveAcquire()
{
	if (!s_gxLiveOn || s_gxLiveStrength <= 0.0f || (s_gxLiveParts & (kGxWaterLiveGlare | kGxWaterLiveFoam)) == 0)
	{
		gxWaterLiveRelease();
		return;
	}
	if (W3DShaderManager::getChipset() < DC_GENERIC_PIXEL_SHADER_1_1)
		return;
	IDirect3DDevice8 *dev = DX8Wrapper::_Get_D3D_Device8();
	if (dev == nullptr)
		return;
	const Bool hadBoth = s_gxGlareShader != 0 && s_gxFoamShader != 0;
	if ((s_gxLiveParts & kGxWaterLiveGlare) && s_gxGlareShader == 0
		&& FAILED(dev->CreatePixelShader((const DWORD *)kGxWaterGlareShader, &s_gxGlareShader)))
		s_gxGlareShader = 0;
	if ((s_gxLiveParts & kGxWaterLiveFoam) && s_gxFoamShader == 0
		&& FAILED(dev->CreatePixelShader((const DWORD *)kGxWaterFoamShader, &s_gxFoamShader)))
		s_gxFoamShader = 0;
	if (!hadBoth)
		fprintf(stderr, "[GXWATER] live pixel shaders: glare handle=0x%x, foam handle=0x%x\n", (unsigned)s_gxGlareShader,
			(unsigned)s_gxFoamShader);
	// A procedural texture keeps no contents of its own across a device reset: write the table again.
	gxWaterLiveFillFoamTable();
}

static void gxWaterLiveRelease()
{
	IDirect3DDevice8 *dev = DX8Wrapper::_Get_D3D_Device8();
	if (dev != nullptr)
	{
		if (s_gxGlareShader)
			dev->DeletePixelShader(s_gxGlareShader);
		if (s_gxFoamShader)
			dev->DeletePixelShader(s_gxFoamShader);
	}
	s_gxGlareShader = 0;
	s_gxFoamShader = 0;
}

static void gxWaterLiveShutdown()
{
	REF_PTR_RELEASE(s_gxFoamTable);
	gxFoamDirty().clear();
	gxFoamCells().clear();
	std::vector<GxFoamCell>().swap(gxFoamCells());	// free the memory; the container itself stays
	s_gxFoamMapLoaded = FALSE;
}

// ---------------------------------------------------------------------------------------------------------------------------
// Map lifetime.

static void gxWaterLiveMapLoaded()
{
	if (TheGlobalData != nullptr && TheGlobalData->m_isWorldBuilder)
		return;
	gxWaterLiveReadSettings();
	gxWaterLiveAcquire();	// made now if the option turned on, released if it turned off
	s_gxLiveClock = 0.0;
	if (gxWaterLivePart(kGxWaterLiveMotion))
		gxWaterLiveMeasureSwell();
	s_gxFoamMapLoaded = TRUE;
	gxFoamCells().clear();
	gxFoamDirty().clear();
	s_gxFoamStale = TRUE;
	gxFoamWaterLevels().clear();
	if (gxWaterLivePart(kGxWaterLiveFoam))
	{
		gxWaterLiveMakeFoamTable();
		gxWaterLiveBuildFoamCells("map");
	}
	char parts[5] = { 0 };
	Int n = 0;
	if (s_gxLiveParts & kGxWaterLiveMotion) parts[n++] = 'm';
	if (s_gxLiveParts & kGxWaterLiveGlare) parts[n++] = 'g';
	if (s_gxLiveParts & kGxWaterLiveFoam) parts[n++] = 'f';
	if (s_gxLiveParts & kGxWaterLiveWaves) parts[n++] = 'w';
	fprintf(stderr, "[GXWATER] live %s parts=%s strength=%.2f tune=drift:%.2f,dir:%.0f,surge:%.2f,gate:%.2f,glint:%.2f,sheen:%.2f,"
		"foam:%.2f,lap:%.1f,wave:%.0f,bank0:%.2f,bank1:%.2f,surf0:%.0f,surf1:%.0f shaders=%d/%d src=%s\n", s_gxLiveOn ? "on" : "off",
		parts, s_gxLiveStrength, s_gxLiveTune.drift, s_gxLiveTune.dir, s_gxLiveTune.surge, s_gxLiveTune.gate, s_gxLiveTune.glint,
		s_gxLiveTune.sheen, s_gxLiveTune.foam, s_gxLiveTune.lap, s_gxLiveTune.wave, s_gxLiveTune.bank0, s_gxLiveTune.bank1,
		s_gxLiveTune.surf0, s_gxLiveTune.surf1, s_gxGlareShader != 0, s_gxFoamShader != 0, s_gxLiveSource);
}

static void gxWaterLiveMapReset()
{
	gxFoamCells().clear();
	gxFoamDirty().clear();
	s_gxFoamStale = TRUE;
	gxFoamWaterLevels().clear();
	s_gxFoamMapLoaded = FALSE;
}

// ---------------------------------------------------------------------------------------------------------------------------
// Standing water: the clock (update), the coordinates (drawTrapezoidWater) and the glints' pass.

// WaterRenderObjClass::update, once per client update (GameClient::update -> W3DTerrainVisual::update), including the
// iterations fast-forward and catch-up do not draw: `logicFrames` = gxRenderTimeScale(), that iteration's logic time, so the
// sum is game time however the iterations are drawn.
static void gxWaterLiveTick(Real logicFrames)
{
	const double prev = s_gxLiveClock;
	const double stepped = gxWaterLiveClockStep(prev, logicFrames, (float)LOGICFRAMES_PER_SECOND);
	// Then follow the game's own time (only READ here: the logic frame counter), so fast-forward and stalls keep the water in
	// step with the game (gxWaterLiveClockFollow).
	s_gxLiveClock = (TheGameLogic != nullptr) ? gxWaterLiveClockFollow(prev, stepped,
		(double)TheGameLogic->getFrame() / (double)LOGICFRAMES_PER_SECOND, 1.0 / (double)LOGICFRAMES_PER_SECOND) : stepped;
}

// This frame's coordinates (the swell at this map's one gain).
static GxWaterLiveUV gxWaterLiveFrameUV()
{
	const Int parts = (gxWaterLivePart(kGxWaterLiveMotion) ? kGxWaterLiveMotion : 0)
		| ((gxWaterLivePart(kGxWaterLiveGlare) && s_gxGlareShader != 0) ? kGxWaterLiveGlare : 0);
	GxWaterLiveUV uv = gxWaterLiveUV(s_gxLiveClock, parts, s_gxLiveStrength, s_gxLiveTune);
	uv.surge *= s_gxSwellGain;
	return uv;
}

// Over retail's vertex: the water tile carried by the flow (it REPLACES retail's slosh - wh1ter0se 18/10, "why is the water moving
// so randomly"), and the glints' own coordinates carried the same way. Called only while motion or glare is on, so the retail
// vertex is untouched otherwise.
static inline void gxWaterLiveVertex(VertexFormatXYZNDUV2 *vb, Real x, Real y, const GxWaterLiveUV &live)
{
	const GxWaterLiveVertexUV o = gxWaterLiveVertexUV(live, x, y);
	if (live.motion)
	{
		vb->u1 = o.u1;
		vb->v1 = o.v1;
	}
	if (live.glare)
	{
		vb->u2 = o.u2;
		vb->v2 = o.v2;
	}
}

// Camera-space texture generation on stage 2 for the twinkle bands: world position -> (along the flow / kGxWaterGateAlong
// - the bands' travel, across it / kGxWaterGateAcross), the same construction retail uses for its noise, rotated onto the flow.
static void gxWaterLiveStage2Gate(const GxWaterLiveUV &live)
{
	DX8Wrapper::Set_DX8_Texture_Stage_State(2, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
	DX8Wrapper::Set_DX8_Texture_Stage_State(2, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
	DX8Wrapper::Set_DX8_Texture_Stage_State(2, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
	DX8Wrapper::Set_DX8_Texture_Stage_State(2, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
	Matrix4x4 curView;
	DX8Wrapper::_Get_DX8_Transform(D3DTS_VIEW, curView);
	D3DXMATRIX inv;
	float det;
	D3DXMatrixInverse(&inv, &det, (D3DXMATRIX *)&curView);
	// Row vectors: [x y z 1] * g = (u, v): u = (x fx + y fy) / along - phase, v = (-x fy + y fx) / across.
	D3DXMATRIX g;
	D3DXMatrixScaling(&g, 1.0f, 1.0f, 1.0f);	// identity (this D3DX shim has no D3DXMatrixIdentity)
	g._11 = live.fx / kGxWaterGateAlong;
	g._21 = live.fy / kGxWaterGateAlong;
	g._41 = -live.gatePhase;
	g._12 = -live.fy / kGxWaterGateAcross;
	g._22 = live.fx / kGxWaterGateAcross;
	Matrix4x4 dest;
	*((D3DXMATRIX *)&dest) = inv * g;
	DX8Wrapper::_Set_DX8_Transform(D3DTS_TEXTURE2, dest);
}

static void gxWaterLiveLights(float amb[3], float dif[3])
{
	amb[0] = TheGlobalData->m_terrainAmbient[0].red;
	amb[1] = TheGlobalData->m_terrainAmbient[0].green;
	amb[2] = TheGlobalData->m_terrainAmbient[0].blue;
	dif[0] = TheGlobalData->m_terrainDiffuse[0].red;
	dif[1] = TheGlobalData->m_terrainDiffuse[0].green;
	dif[2] = TheGlobalData->m_terrainDiffuse[0].blue;
}

// The glints (kGxWaterGlareShader): drawTrapezoidWater's mesh again, right after retail's own draw and before it restores the
// blend, ADDED on top (ONE, ONE), so the water's clarity - retail's pass is blended by destination alpha - no longer fades
// them. Stage 1 reads the glint coordinates (u2/v2), stage 2 the twinkle bands (gxWaterLiveStage2Gate); the shroud (or white) is still on
// stage 3 from setupFlatWaterShader. Feathered water (a debug setting) gets them on its top layer only (renderWater).
static void gxWaterLiveDrawGlare(const GxWaterLiveUV &live, unsigned short tris, unsigned short verts)
{
	if (!live.glare || s_gxGlareShader == 0)
		return;
	IDirect3DDevice8 *dev = DX8Wrapper::_Get_D3D_Device8();
	DX8Wrapper::Set_DX8_Texture_Stage_State(1, D3DTSS_TEXCOORDINDEX, 1);
	gxWaterLiveStage2Gate(live);
	float amb[3], dif[3];
	gxWaterLiveLights(amb, dif);
	const unsigned c = gxWaterLiveLitColor(amb, dif, 1.0f, 1.0f);
	const float g = s_gxLiveTune.glint * s_gxLiveStrength;
	dev->SetPixelShaderConstant(1, D3DXVECTOR4(g * ((c >> 16) & 0xff) / 255.0f, g * ((c >> 8) & 0xff) / 255.0f,
		g * (c & 0xff) / 255.0f, 1.0f), 1);
	const float s = s_gxLiveTune.sheen * s_gxLiveStrength;
	dev->SetPixelShaderConstant(2, D3DXVECTOR4(s, s, s, 0.0f), 1);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND, D3DBLEND_ONE);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND, D3DBLEND_ONE);
	dev->SetPixelShader(s_gxGlareShader);
	DX8Wrapper::Draw_Triangles(0, tris, 0, verts);
	// drawTrapezoidWater unbinds the pixel shader and restores the blend right after, as it always did; stage 1's coordinate
	// set goes back to retail's here.
	DX8Wrapper::Set_DX8_Texture_Stage_State(1, D3DTSS_TEXCOORDINDEX, 0);
}

// ---------------------------------------------------------------------------------------------------------------------------
// The foam pass: after the water surface (Render), the visible foam cells, alpha blended, under the shroud.

enum
{
	kGxFoamMaxVertsPerCell = 18,	// 2 triangles, each split into at most 3
	kGxFoamBatchCells = 3000		// 54000 vertices per batch at most: inside an unsigned short index
};

struct GxFoamFrame
{
	unsigned rgb;	// the lit colour (alpha 0); a vertex's alpha is the strength x its shore factor
	Real alpha, phase0, noiseScale, nu, nv;
};

// One foam vertex: the table coordinates (depth, lapping phase) and the breakup's coordinates (Noise0000 on stage 1, the bubble
// specks on stage 2 - both read u2/v2), taken from the ground the surge has carried here: (x, y) minus the carry along the
// uphill direction (ux, uy), so the foam's pattern runs up the beach with each surge and back with its retreat.
static inline void gxWaterLiveFoamVertex(VertexFormatXYZNDUV2 *vb, Real x, Real y, Real z, Real depth, Real phase, Real ux,
	Real uy, Real shore, const GxFoamFrame &f)
{
	float ox = 0.0f, oy = 0.0f;
	gxWaterLiveFoamCarry(ux, uy, f.phase0 + phase, &ox, &oy);
	vb->x = x;
	vb->y = y;
	vb->z = z;
	vb->nx = 0.0f;
	vb->ny = 0.0f;
	vb->nz = 1.0f;
	vb->diffuse = f.rgb | ((unsigned)(gxWaterLiveClamp(f.alpha * shore, 0.0f, 1.0f) * 255.0f + 0.5f) << 24);
	vb->u1 = gxWaterLiveFoamU(depth);
	vb->v1 = f.phase0 + phase;
	vb->u2 = (x - ox) * f.noiseScale + f.nu;
	vb->v2 = (y - oy) * f.noiseScale + f.nv;
}

// One triangle (corners a, b, c of the cell) split at the water line (gxWaterLiveSplitTriangle): the wet part flat on the
// water, the dry part on the ground. Writes up to 9 vertices and returns how many.
static Int gxWaterLiveFoamTriangle(VertexFormatXYZNDUV2 *vb, const GxFoamCell &cell, Int a, Int b, Int c, const GxFoamFrame &f)
{
	GxFoamPt in[3], out[9];
	const Int idx[3] = { a, b, c };
	for (Int k = 0; k < 3; ++k)
	{
		const GxFoamCorner &q = cell.c[idx[k]];
		in[k].x = q.x;
		in[k].y = q.y;
		in[k].d = cell.level - q.ground;
		in[k].ph = q.phase;
		in[k].ux = q.ux;
		in[k].uy = q.uy;
		in[k].shore = q.shore;
	}
	int wetTris = 0;
	const Int n = gxWaterLiveSplitTriangle(in, out, &wetTris) * 3;
	for (Int k = 0; k < n; ++k)
	{
		const GxFoamPt &p = out[k];
		gxWaterLiveFoamVertex(vb + k, p.x, p.y, (k < wetTris * 3) ? cell.level : cell.level - p.d, p.d, p.ph, p.ux, p.uy, p.shore, f);
	}
	return n;
}

static void gxWaterLiveDrawFoam(Bool riversReplaced, TextureClass *noise, TextureClass *bubbles, TextureClass *white)
{
	if (!s_gxFoamMapLoaded || !gxWaterLivePart(kGxWaterLiveFoam) || s_gxFoamShader == 0 || s_gxFoamTable == nullptr)
		return;
	if (TheTerrainRenderObject == nullptr || TheTerrainRenderObject->getMap() == nullptr || noise == nullptr || bubbles == nullptr
		|| white == nullptr)
		return;
	if (s_gxFoamStale || gxWaterLiveWaterMoved())
		gxWaterLiveBuildFoamCells(s_gxFoamStale ? "ground" : "water level");
	gxWaterLiveRescanDirty();
	if (gxFoamCells().empty())
		return;

	WorldHeightMap *map = TheTerrainRenderObject->getMap();
	const Int x0 = map->getDrawOrgX(), y0 = map->getDrawOrgY();
	const Int x1 = x0 + map->getDrawWidth(), y1 = y0 + map->getDrawHeight();

	const double seconds = s_gxLiveClock;
	const GxWaterLiveFlow flow = gxWaterLiveFlowDir(s_gxLiveTune.dir);
	GxFoamFrame frame;
	frame.phase0 = gxWaterLiveWrapD(seconds / s_gxLiveTune.lap, 256.0);	// whole cycles: the table wraps each one
	frame.noiseScale = 1.0f / 30.0f;
	// a slow churn along the flow, so no two laps show the same pattern (the surge's carry is the motion that reads)
	frame.nu = gxWaterLiveWrapD(-seconds * 0.4 * flow.x * frame.noiseScale, 1.0);
	frame.nv = gxWaterLiveWrapD(-seconds * 0.4 * flow.y * frame.noiseScale, 1.0);
	float amb[3], dif[3];
	gxWaterLiveLights(amb, dif);
	frame.rgb = gxWaterLiveLitColor(amb, dif, 0.8f, 0.0f);
	frame.alpha = gxWaterLiveClamp(s_gxLiveTune.foam * s_gxLiveStrength, 0.0f, 1.0f);

	DX8Wrapper::Set_Transform(D3DTS_WORLD, Matrix3D(true));
	DX8Wrapper::Set_Texture(0, s_gxFoamTable);
	DX8Wrapper::Set_Texture(1, noise);
	DX8Wrapper::Set_Shader(ShaderClass::_PresetAlphaShader);
	VertexMaterialClass *vmat = VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
	DX8Wrapper::Set_Material(vmat);
	REF_PTR_RELEASE(vmat);
	DX8Wrapper::Apply_Render_State_Changes();

	// The shroud on stage 3, as setupFlatWaterShader does for the water itself.
	W3DShroud *shroud = TheTerrainRenderObject->getShroud();
	if (shroud)
	{
		W3DShaderManager::setTexture(0, shroud->getShroudTexture());
		W3DShaderManager::setShader(W3DShaderManager::ST_SHROUD_TEXTURE, 3);
		GxRawRS(DX8Wrapper::_Get_D3D_Device8(), D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	}
	else
	{
		if (!white->Is_Initialized())
			white->Init();
		DX8Wrapper::_Get_D3D_Device8()->SetTexture(3, white->Peek_D3D_Texture());
	}
	if (!bubbles->Is_Initialized())
		bubbles->Init();
	DX8Wrapper::_Get_D3D_Device8()->SetTexture(2, bubbles->Peek_D3D_Texture());
	// The specks read the same carried coordinates as the noise (u2/v2), untransformed, so they travel with the surge too.
	DX8Wrapper::Set_DX8_Texture_Stage_State(2, D3DTSS_TEXCOORDINDEX, 1);
	DX8Wrapper::Set_DX8_Texture_Stage_State(2, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	DX8Wrapper::Set_DX8_Texture_Stage_State(2, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
	DX8Wrapper::Set_DX8_Texture_Stage_State(2, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
	DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_TEXCOORDINDEX, 0);
	DX8Wrapper::Set_DX8_Texture_Stage_State(1, D3DTSS_TEXCOORDINDEX, 1);
	for (DWORD s = 0; s < 4; ++s)
	{
		GxRawTSS(DX8Wrapper::_Get_D3D_Device8(), s, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
		GxRawTSS(DX8Wrapper::_Get_D3D_Device8(), s, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
	}
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ZBIAS, 4);	// the beach half lies on the ground it was measured from
	DWORD cull;
	DX8Wrapper::_Get_D3D_Device8()->GetRenderState(D3DRS_CULLMODE, &cull);
	GxRawRS(DX8Wrapper::_Get_D3D_Device8(), D3DRS_CULLMODE, D3DCULL_NONE);
	DX8Wrapper::_Get_D3D_Device8()->SetPixelShaderConstant(1, D3DXVECTOR4(0.45f, 0.45f, 0.45f, 0.0f), 1);	// breakup scale
	DX8Wrapper::_Get_D3D_Device8()->SetPixelShaderConstant(2, D3DXVECTOR4(0.55f, 0.55f, 0.55f, 1.0f), 1);	// breakup floor
	DX8Wrapper::_Get_D3D_Device8()->SetPixelShader(s_gxFoamShader);

	size_t next = 0;
	const size_t total = gxFoamCells().size();
	while (next < total)
	{
		// Count this batch's visible cells first: the buffers are sized to the most they can take.
		size_t end = next;
		Int count = 0;
		for (; end < total && count < kGxFoamBatchCells; ++end)
		{
			const GxFoamCell &c = gxFoamCells()[end];
			if (c.i >= x0 && c.i < x1 && c.j >= y0 && c.j < y1 && !(riversReplaced && c.river))
				++count;
		}
		if (count > 0)
		{
			const Int room = count * kGxFoamMaxVertsPerCell;
			DynamicVBAccessClass vb_access(BUFFER_TYPE_DYNAMIC_DX8, dynamic_fvf_type, room);
			DynamicIBAccessClass ib_access(BUFFER_TYPE_DYNAMIC_DX8, room);
			Int verts = 0;
			{
				DynamicVBAccessClass::WriteLockClass lock(&vb_access);
				VertexFormatXYZNDUV2 *vb = lock.Get_Formatted_Vertex_Array();
				DynamicIBAccessClass::WriteLockClass lockib(&ib_access);
				UnsignedShort *ib = lockib.Get_Index_Array();
				if (vb == nullptr || ib == nullptr)
					break;
				for (size_t k = next; k < end; ++k)
				{
					const GxFoamCell &c = gxFoamCells()[k];
					if (!(c.i >= x0 && c.i < x1 && c.j >= y0 && c.j < y1 && !(riversReplaced && c.river)))
						continue;
					// The terrain's own two triangles (renderShoreLinesSorted's index order for each flip state).
					if (c.flip)
					{
						verts += gxWaterLiveFoamTriangle(vb + verts, c, 1, 3, 0, frame);
						verts += gxWaterLiveFoamTriangle(vb + verts, c, 1, 2, 3, frame);
					}
					else
					{
						verts += gxWaterLiveFoamTriangle(vb + verts, c, 0, 2, 3, frame);
						verts += gxWaterLiveFoamTriangle(vb + verts, c, 0, 1, 2, frame);
					}
				}
				for (Int k = 0; k < verts; ++k)
					ib[k] = (UnsignedShort)k;
			}
			if (verts >= 3)
			{
				DX8Wrapper::Set_Index_Buffer(ib_access, 0);
				DX8Wrapper::Set_Vertex_Buffer(vb_access);
				DX8Wrapper::Draw_Triangles(0, (unsigned short)(verts / 3), 0, (unsigned short)verts);
			}
		}
		next = end;
	}

	DX8Wrapper::_Get_D3D_Device8()->SetPixelShader(0);
	// Stage 2 back to camera-space generation, as the water's own passes leave it (setupFlatWaterShader, the glints).
	DX8Wrapper::Set_DX8_Texture_Stage_State(2, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
	DX8Wrapper::Set_DX8_Texture_Stage_State(2, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ZBIAS, 0);
	GxRawRS(DX8Wrapper::_Get_D3D_Device8(), D3DRS_CULLMODE, cull);
	DX8Wrapper::_Get_D3D_Device8()->SetTexture(2, nullptr);
	if (shroud)
	{	// as drawTrapezoidWater restores after its own shroud stage
		W3DShaderManager::resetShader(W3DShaderManager::ST_SHROUD_TEXTURE);
		DX8Wrapper::_Get_D3D_Device8()->SetTexture(3, nullptr);
		GxRawRS(DX8Wrapper::_Get_D3D_Device8(), D3DRS_ZFUNC, D3DCMP_EQUAL);
	}
	else
		DX8Wrapper::_Get_D3D_Device8()->SetTexture(3, nullptr);
}

// ---------------------------------------------------------------------------------------------------------------------------
// EA's wave crests (W3DWaterTracks.cpp flush asks once per frame).

void gxWaterLiveWaveFrame(GxWaterLiveWaveFrame *out)
{
	out->on = false;
	out->rgb = 0xffffff;
	out->alphaGain = 1;
	out->seconds = s_gxLiveClock;
	out->lap = s_gxLiveTune.lap;
	out->flow = gxWaterLiveFlowDir(s_gxLiveTune.dir);
	if (!gxWaterLivePart(kGxWaterLiveWaves) || TheGlobalData == nullptr)
		return;
	float amb[3], dif[3];
	gxWaterLiveLights(amb, dif);
	out->on = true;
	// Ambient + the sun, clamped: white by day like retail's crest, dim at night. (EA's own unused diffuseLight takes half the
	// sun, which on the Mini 17/10 turned the white crests grey - the wrong way for "wave water in white".)
	out->rgb = gxWaterLiveLitColor(amb, dif, 1.0f, 0.0f) & 0x00ffffff;
	const float w = s_gxLiveTune.wave * s_gxLiveStrength;
	out->alphaGain = (w >= 3.0f) ? 4 : ((w >= 1.5f) ? 2 : 1);
}
