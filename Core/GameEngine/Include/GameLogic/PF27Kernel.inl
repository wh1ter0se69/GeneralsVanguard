// GeneralsX: PF27 - the new A* kernel for Pathfinder::internalFindPath.
// Design, contract and proof. Included at the END of
// AIPathfind.cpp because it uses that file's statics (the info pool gauges, s_gxMoveGen, the cost constants).
//
// WHAT IT IS. The same search as the legacy loop in internalFindPath + examineNeighboringCells +
// examineCellsCallback + checkChangeLayers, decision for decision, with the per-search state moved out of
// PathfindCellInfo (64 bytes, taken from and returned to a pooled free list per cell, linked into sorted lists)
// into a per-map array of small records stamped with a search generation, and the sorted open list replaced by
// PF27BucketQueue (same pop order, see that header). A search touches no PathfindCellInfo except on the final
// path (materialised so the unchanged buildActualPath can read it) and on obstacle cells (which keep an info
// in the legacy code too).
//
// SIDE EFFECTS (DESIGN 4.4) that the legacy loop leaves on shared state, each reproduced here:
//   * PathfindCell::m_blockedByAlly written on touched cells       -> kept in the record, written back at the end
//   * m_cumulativeCellsAllocated += cells released from the lists  -> += open + closed at the end
//   * info pool takes/free drive the growth guard                  -> emulated (m_virtualTakes)
//   * obstacle cells keep the info a search gave them              -> real allocateInfo on exactly those cells
//   * persistent infos keep the costs the search wrote             -> written back
//   * m_isTunneling                                                 -> the real member is driven
//   * s_gxMoveGen bump at search end                                -> cleanOpenAndClosedLists on empty lists
// The dual-run checker (GX_PF27=check) runs this kernel as a shadow, then the legacy loop, and compares the whole
// final search state; see compareWithLegacy.

#include "GameLogic/PF27BucketQueue.h"

#include <algorithm>

enum PF27Mode { PF27_LEGACY = 0, PF27_NEW = 1, PF27_CHECK = 2 };

static Int pf27ModeFromEnv()
{
	static Int s_mode = -1;
	if (s_mode < 0)
	{
		// GeneralsX: The new kernel is the DEFAULT: path-identical to the
		// legacy loop on Mac, Linux and Windows (fixes/pathfinding-2027 identity runs); GX_PF27=legacy keeps the old loop,
		// GX_PF27=check runs both and compares. Peers may differ in this choice - the paths do not. Builds whose retail
		// pathfinding switches are on still run legacy (PF27Kernel::prepare refuses the kernel there).
		const char *v = getenv("GX_PF27");
		s_mode = PF27_NEW;
		if (v != nullptr && strcmp(v, "legacy") == 0) s_mode = PF27_LEGACY;
		if (v != nullptr && strcmp(v, "check") == 0) s_mode = PF27_CHECK;
	}
	return s_mode;
}

// Checker counters, emitted as [GXPF27] next to [GXPATH] (see processPathfindQueue). Observation only.
static UnsignedInt s_pf27Searches = 0, s_pf27Ok = 0, s_pf27Bad = 0, s_pf27Reopen = 0, s_pf27Sat = 0,
	s_pf27Tunnel = 0, s_pf27LayerChange = 0, s_pf27OnLayer = 0, s_pf27XLayer = 0, s_pf27CapTakes = 0, s_pf27CapFloor = 0,
	s_pf27StartGoal = 0, s_pf27Fail = 0, s_pf27BadDumps = 0;
// Production-path fixture counters (GX_PF27=new): what the kernel's cleanup met. asked for named evidence
// that each boundary ran, not only "0 bad".
static UnsignedInt s_pf27PreRelease = 0, s_pf27KeptObstacle = 0, s_pf27KeptUnit = 0, s_pf27KeptAircraft = 0,
	s_pf27StaleBlocked = 0;

// End-to-end trace (GX_PF27_TRACE=1 per frame, =2 also per search): see PF27Kernel::traceEnd. Observation only.
static Int pf27TraceLevel()
{
	static Int s_level = -1;
	if (s_level < 0)
	{
		const char *v = getenv("GX_PF27_TRACE");
		s_level = (v != nullptr) ? atoi(v) : 0;
	}
	return s_level;
}
static UnsignedInt s_pf27TraceHash = 2166136261u, s_pf27TraceN = 0;
static inline UnsignedInt pf27Fnv(UnsignedInt h, UnsignedInt v)
{
	for (Int i = 0; i < 4; ++i)
	{
		h ^= (v >> (8 * i)) & 0xFFu;
		h *= 16777619u;
	}
	return h;
}
static inline UnsignedInt pf27FloatBits(Real r)
{
	UnsignedInt u;
	memcpy(&u, &r, sizeof(u));
	return u;
}

// Fail arms (never ship): a build with GX_PF27_MUTANTS defined reads GX_PF27_MUTANT=1..4. Each must be caught -
// 1 ties in another order (neighbours expanded before the line walk), 2 one blockedByAlly write-back skipped,
// 3 the listed count off by one, 4 one releasable listed info not released.
#if defined(GX_PF27_MUTANTS)
static Int pf27Mutant()
{
	static Int s_m = -1;
	if (s_m < 0)
	{
		const char *v = getenv("GX_PF27_MUTANT");
		s_m = (v != nullptr) ? atoi(v) : 0;
	}
	return s_m;
}
#else
static inline Int pf27Mutant() { return 0; }
#endif

/// [GXPF27] per queue pass in check/new mode. bad is never reset: one mismatch anywhere stays visible.
static void pf27ReportCounters()
{
	if (s_pf27Searches == 0 && s_pf27StartGoal == 0 && s_pf27Fail == 0 && s_pf27PreRelease == 0 && s_pf27KeptObstacle == 0 &&
	    s_pf27KeptUnit == 0 && s_pf27KeptAircraft == 0 && s_pf27StaleBlocked == 0 && s_pf27CapTakes == 0 && s_pf27CapFloor == 0)
		return;
	fprintf(stderr, "[GXPF27] f=%u n=%u ok=%u bad=%u reopen=%u sat=%u tunnel=%u layerchg=%u onlayer=%u xlayer=%u"
		" captakes=%u capfloor=%u startgoal=%u fail=%u prerel=%u keptobst=%u keptunit=%u keptair=%u staleblk=%u\n",
		TheGameLogic ? (unsigned)TheGameLogic->getFrame() : 0u, s_pf27Searches, s_pf27Ok, s_pf27Bad, s_pf27Reopen,
		s_pf27Sat, s_pf27Tunnel, s_pf27LayerChange, s_pf27OnLayer, s_pf27XLayer, s_pf27CapTakes, s_pf27CapFloor,
		s_pf27StartGoal, s_pf27Fail, s_pf27PreRelease, s_pf27KeptObstacle, s_pf27KeptUnit, s_pf27KeptAircraft,
		s_pf27StaleBlocked);
	fflush(stderr);
	s_pf27Searches = s_pf27Ok = s_pf27Reopen = s_pf27Sat = s_pf27Tunnel = 0;
	s_pf27LayerChange = s_pf27OnLayer = s_pf27XLayer = s_pf27CapTakes = s_pf27CapFloor = 0;
	s_pf27StartGoal = s_pf27Fail = s_pf27PreRelease = s_pf27KeptObstacle = s_pf27KeptUnit = s_pf27KeptAircraft = 0;
	s_pf27StaleBlocked = 0;
}

/// [GXPF27H] per queue pass; atEnd (Pathfinder::reset) flushes the last batch and prints the end marker that
/// pf27-trace-diff.py requires, so a log that stopped early can never compare as identical.
static void pf27TraceFlush(Bool atEnd)
{
	if (pf27TraceLevel() == 0)
		return;
	const unsigned f = TheGameLogic ? (unsigned)TheGameLogic->getFrame() : 0u;
	if (s_pf27TraceN > 0)
	{
		fprintf(stderr, "[GXPF27H] f=%u n=%u h=%08x\n", f, s_pf27TraceN, s_pf27TraceHash);
		s_pf27TraceN = 0;
		s_pf27TraceHash = 2166136261u;
	}
	if (atEnd)
		fprintf(stderr, "[GXPF27H] end f=%u\n", f);
	fflush(stderr);
}

class PF27Kernel
{
public:
	enum
	{
		F_OPEN = 0x01, F_CLOSED = 0x02, F_INFO = 0x04, F_BLOCKED = 0x08, F_WROTE_BLOCKED = 0x10,
		F_COSTSET = 0x20, F_LINE_KNOWN = 0x40, F_LINE_OK = 0x80
	};
	enum { F2_ORIG_BLOCKED = 0x01, F2_REAL_INFO = 0x02, F2_REAL_ALLOC = 0x04 };

	struct Rec
	{
		UnsignedInt stamp;	///< == m_gen: this record belongs to the current search
		UnsignedShort g;		///< costSoFar as legacy stores it (gxPathCost16)
		UnsignedShort f;		///< totalCost as legacy stores it
		Int parent;				///< dense index of the parent, -1 none
		UnsignedShort x, y;	///< the coordinates legacy reads for this cell (its info's m_pos)
		UnsignedByte flags, flags2, layer, pad;
	};

	explicit PF27Kernel(Pathfinder *pf) : m_pf(pf), m_recs(nullptr), m_touched(nullptr), m_traceCells(nullptr), m_cap(0),
		m_gen(0), m_groundBase(nullptr), m_groundCount(0), m_tracing(false), m_traceN(0)
	{
		for (Int i = 0; i <= LAYER_LAST; ++i) { m_layerBase[i] = nullptr; m_layerCount[i] = 0; m_layerOffset[i] = 0; }
	}
	~PF27Kernel() { release(); }

	void release()
	{
		delete[] m_recs;
		delete[] m_touched;
		delete[] m_traceCells;
		m_recs = nullptr;
		m_touched = nullptr;
		m_traceCells = nullptr;
		m_cap = 0;
		m_open.release();
	}

	/// Per search: the dense index table (ground block, then each layer block) and capacity. False = use legacy.
	Bool prepare()
	{
		Pathfinder *pf = m_pf;
		if (pf->m_blockOfMapCells == nullptr)
			return false;
		// The kernel reproduces the LIVE (fixed) pathfinder. In a retail-compatible build the retail arm marks the real
		// start info open in startPathfind and tears its lists down differently, so those builds run legacy.
		//
#if RETAIL_COMPATIBLE_PATHFINDING_ALLOCATION
		return false;
#endif
#if RETAIL_COMPATIBLE_PATHFINDING
		if (!s_useFixedPathfinding)
			return false;
#endif
		m_groundBase = pf->m_blockOfMapCells;
		m_groundCount = (pf->m_extent.hi.x + 1) * (pf->m_extent.hi.y + 1);
		Int total = m_groundCount;
		for (Int L = 0; L <= LAYER_LAST; ++L)
		{
			m_layerBase[L] = nullptr;
			m_layerCount[L] = 0;
			m_layerOffset[L] = total;
			if (L <= LAYER_GROUND)
				continue;
			PathfindLayer &lay = pf->m_layers[L];
			if (lay.m_blockOfMapCells != nullptr && lay.m_width > 0 && lay.m_height > 0)
			{
				m_layerBase[L] = lay.m_blockOfMapCells;
				m_layerCount[L] = lay.m_width * lay.m_height;
				total += m_layerCount[L];
			}
		}
		// One expansion takes at most 1 + 8 + one line walk (< 2 x the longer map side + 2) infos; the growth guard
		// stops a search with more than PATHFIND_SEARCH_FREE_RESERVE free, so a take can never fail inside the loop.
		// That is what lets this kernel emulate the pool instead of using it. A map too large for that: legacy.
		const Int side = (pf->m_extent.hi.x > pf->m_extent.hi.y ? pf->m_extent.hi.x : pf->m_extent.hi.y) + 1;
		if (1 + 8 + 2 * side + 2 >= (Int)PATHFIND_SEARCH_FREE_RESERVE)
			return false;
		if (total > m_cap)
		{
			release();
			m_cap = total;
			m_recs = MSGNEW("PF27Records") Rec[m_cap];
			memset(m_recs, 0, sizeof(Rec) * m_cap);
			m_touched = MSGNEW("PF27Touched") Int[m_cap];
			m_traceCells = MSGNEW("PF27Trace") Int[m_cap + 2];
			m_open.init(m_cap);
			m_gen = 0;
		}
		return true;
	}

	Int indexOf(const PathfindCell *c) const
	{
		const uintptr_t p = (uintptr_t)c;
		if (p >= (uintptr_t)m_groundBase && p < (uintptr_t)(m_groundBase + m_groundCount))
			return (Int)(c - m_groundBase);
		const Int L = c->getLayer();
		if (L > LAYER_GROUND && L <= LAYER_LAST && m_layerBase[L] != nullptr &&
		    p >= (uintptr_t)m_layerBase[L] && p < (uintptr_t)(m_layerBase[L] + m_layerCount[L]))
			return m_layerOffset[L] + (Int)(c - m_layerBase[L]);
		return -1;
	}

	PathfindCell *cellAt(Int idx) const
	{
		if (idx < m_groundCount)
			return m_groundBase + idx;
		const Int L = m_recs[idx].layer;
		return m_layerBase[L] + (idx - m_layerOffset[L]);
	}

	/// cellAt for an index whose record may belong to another search (the trace): finds the block by range.
	PathfindCell *cellAtAnySearch(Int idx) const
	{
		if (idx < m_groundCount)
			return m_groundBase + idx;
		for (Int L = LAYER_GROUND + 1; L <= LAYER_LAST; ++L)
			if (m_layerBase[L] != nullptr && idx >= m_layerOffset[L] && idx < m_layerOffset[L] + m_layerCount[L])
				return m_layerBase[L] + (idx - m_layerOffset[L]);
		return nullptr;
	}

	/// The Bresenham cell walk of Pathfinder::iterateCellsAlongLine (which now calls this), with the callback inlined:
	/// f(from, to, x, y) for every cell from start to end inclusive, two calls on a diagonal step, from == nullptr on
	/// the first; a non-zero return stops the walk and is returned; a cell off the map ends it with 0.
	template <typename F>
	static Int forEachCellAlongLine(Pathfinder *pf, const ICoord2D &start, const ICoord2D &end, PathfindLayerEnum layer, F f)
	{
		Int delta_x = abs(end.x - start.x);			// The difference between the x's
		Int delta_y = abs(end.y - start.y);			// The difference between the y's
		Int x = start.x;												// Start x off at the first pixel
		Int y = start.y;												// Start y off at the first pixel

		Int xinc1, xinc2;
		if (end.x >= start.x)								// The x-values are increasing
		{
			xinc1 = 1;
			xinc2 = 1;
		}
		else																// The x-values are decreasing
		{
			xinc1 = -1;
			xinc2 = -1;
		}

		Int yinc1, yinc2;
		if (end.y >= start.y)               // The y-values are increasing
		{
			yinc1 = 1;
			yinc2 = 1;
		}
		else																// The y-values are decreasing
		{
			yinc1 = -1;
			yinc2 = -1;
		}

		Int den, num, numadd, numpixels;
		if (delta_x >= delta_y)							// There is at least one x-value for every y-value
		{
			xinc1 = 0;												// Don't change the x when numerator >= denominator
			yinc2 = 0;												// Don't change the y for every iteration
			den = delta_x;
			num = delta_x / 2;
			numadd = delta_y;
			numpixels = delta_x;							// There are more x-values than y-values
		}
		else																// There is at least one y-value for every x-value
		{
			xinc2 = 0;												// Don't change the x for every iteration
			yinc1 = 0;												// Don't change the y when numerator >= denominator
			den = delta_y;
			num = delta_y / 2;
			numadd = delta_x;
			numpixels = delta_y;							// There are more y-values than x-values
		}

		PathfindCell* from = nullptr;
		for (Int curpixel = 0; curpixel <= numpixels; curpixel++)
		{
			PathfindCell* to = pf->getCell( layer, x, y );
			if (to==nullptr) return 0;

			Int ret = f(from, to, x, y);
			if (ret != 0)
				return ret;

			num += numadd;										// Increase the numerator by the top of the fraction
			if (num >= den)										// Check if numerator >= denominator
			{
				num -= den;											// Calculate the new numerator value
				x += xinc1;											// Change the x as appropriate
				y += yinc1;											// Change the y as appropriate
				from = to;
				to = pf->getCell( layer, x, y );
				if (to==nullptr) return 0;
				Int ret = f(from, to, x, y);
				if (ret != 0)
					return ret;
			}
			x += xinc2;												// Change the x as appropriate
			y += yinc2;												// Change the y as appropriate

			from = to;
		}

		return 0;
	}

#if defined(GX_TEST_INSTRUMENTS)
	// GeneralsX: TEST BUILDS ONLY: the probe's searches spend the frame's queue
	// budget and move m_cumulativeCellsAllocated, so a peer that set the env var alone would desync. A shipping build has no
	// probe at all and never reads GX_PF27_PROBE.
	/// GX_PF27_PROBE=K,F (test env, any GX_PF27 mode - it moves the simulation, identically in every mode since the kernel is
	/// path-identical, so it doubles as a timing workload): at frame F every unit with an AI and a
	/// ground locomotor asks K seeded goals spread over the whole map through findPath, so the checker sees many
	/// locomotor classes, far and unreachable goals, and the 20,000-take cap. Integer LCG seeded by frame and object.
	static void runProbe(Pathfinder *pf)
	{
		static Int s_k = -1;
		static UnsignedInt s_f = 0;
		if (s_k < 0)
		{
			s_k = 0;
			const char *v = getenv("GX_PF27_PROBE");
			if (v != nullptr)
			{
				s_k = atoi(v);
				const char *c = strchr(v, ',');
				s_f = (c != nullptr) ? (UnsignedInt)atoi(c + 1) : 0u;
			}
		}
		if (s_k <= 0 || TheGameLogic == nullptr || TheGameLogic->getFrame() != s_f)
			return;
		const Int w = pf->m_extent.hi.x - pf->m_extent.lo.x;
		const Int h = pf->m_extent.hi.y - pf->m_extent.lo.y;
		if (w <= 0 || h <= 0)
			return;
		UnsignedInt asked = 0, found = 0;
		for (Object *obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject())
		{
			AIUpdateInterface *ai = obj->getAIUpdateInterface();
			if (ai == nullptr || obj->isKindOf(KINDOF_AIRCRAFT) || obj->isEffectivelyDead())
				continue;
			const LocomotorSet &loco = ai->getLocomotorSet();
			if (loco.getValidSurfaces() == 0)
				continue;
			UnsignedInt rng = (s_f * 2654435761u) ^ ((UnsignedInt)obj->getID() * 40503u) ^ 0x9E3779B9u;
			for (Int i = 0; i < s_k; ++i)
			{
				rng = rng * 1664525u + 1013904223u;
				const Int cx = pf->m_extent.lo.x + (Int)((rng >> 8) % (UnsignedInt)w);
				rng = rng * 1664525u + 1013904223u;
				const Int cy = pf->m_extent.lo.y + (Int)((rng >> 8) % (UnsignedInt)h);
				Coord3D goal;
				goal.x = cx * PATHFIND_CELL_SIZE_F + PATHFIND_CELL_SIZE_F / 2;
				goal.y = cy * PATHFIND_CELL_SIZE_F + PATHFIND_CELL_SIZE_F / 2;
				goal.z = TheTerrainLogic->getGroundHeight(goal.x, goal.y);
				Coord3D from = *obj->getPosition();
				Path *path = pf->findPath(obj, loco, &from, &goal);
				++asked;
				if (path != nullptr)
				{
					++found;
					deleteInstance(path);
				}
			}
		}
		fprintf(stderr, "[GXPF27P] f=%u asked=%u found=%u\n", s_f, asked, found);
		fflush(stderr);
	}
#endif

	// ------------------------------------------------------------------------------------------------------------
	// End-to-end trace (GX_PF27_TRACE): every internalFindPath, whichever loop ran, hashed after its cleanup.
	// ------------------------------------------------------------------------------------------------------------

	Bool tracing() const { return m_tracing; }
	void traceBegin() { m_tracing = true; m_traceN = 0; }

	/// The legacy loop's exit, before its cleanup: every listed cell, plus the start and the goal.
	void traceCaptureLegacy(PathfindCell *start, PathfindCell *goal)
	{
		Pathfinder *pf = m_pf;
		for (Int pass = 0; pass < 2; ++pass)
		{
			PathfindCellList &list = pass == 0 ? pf->m_openList : pf->m_closedList;
			for (PathfindCell *c = list.getHead(); c != nullptr; c = c->getNextOpen())
				traceAdd(indexOf(c));
		}
		traceAdd(indexOf(start));
		traceAdd(indexOf(goal));
	}

	/// The kernel's exit: the same set from the records.
	void traceCaptureKernel(PathfindCell *start)
	{
		if (!m_tracing)
			return;
		for (Int i = 0; i < m_numTouched; ++i)
			if (m_recs[m_touched[i]].flags & (F_OPEN | F_CLOSED))
				traceAdd(m_touched[i]);
		traceAdd(indexOf(start));
		traceAdd(m_goalIdx);
	}

	/// After the search has cleaned up: inputs, the Path node by node, the captured cells' state, the globals.
	void traceEnd(const Object *obj, const Coord3D *from, const Coord3D *to, Path *path)
	{
		Pathfinder *pf = m_pf;
		m_tracing = false;
		UnsignedInt h = 2166136261u;
		h = pf27Fnv(h, obj ? (UnsignedInt)obj->getID() : 0u);
		h = pf27Fnv(h, pf27FloatBits(from->x)); h = pf27Fnv(h, pf27FloatBits(from->y)); h = pf27Fnv(h, pf27FloatBits(from->z));
		h = pf27Fnv(h, pf27FloatBits(to->x)); h = pf27Fnv(h, pf27FloatBits(to->y)); h = pf27Fnv(h, pf27FloatBits(to->z));
		UnsignedInt nodes = 0;
		if (path != nullptr)
		{
			h = pf27Fnv(h, 0x50415448u);
			h = pf27Fnv(h, (UnsignedInt)path->getBlockedByAlly());
			h = pf27Fnv(h, (UnsignedInt)path->m_isOptimized);
			h = pf27Fnv(h, (UnsignedInt)path->m_cpopValid);
			h = pf27Fnv(h, (UnsignedInt)path->m_cpopCountdown);
			h = pf27Fnv(h, path->m_cpopRecentStart == nullptr ? 0u : 1u);
			for (const PathNode *n = path->getFirstNode(); n != nullptr; n = n->getNext())
			{
				const Coord3D *pos = n->getPosition();
				h = pf27Fnv(h, pf27FloatBits(pos->x)); h = pf27Fnv(h, pf27FloatBits(pos->y)); h = pf27Fnv(h, pf27FloatBits(pos->z));
				h = pf27Fnv(h, (UnsignedInt)n->getLayer());
				h = pf27Fnv(h, (UnsignedInt)n->getCanOptimize());
				Coord2D dir;
				Real dist = 0;
				const PathNode *o = n->getNextOptimized(&dir, &dist);
				Int oi = -1;	// the optimized link as a position in the list, never an address
				Int k = 0;
				for (const PathNode *m = path->getFirstNode(); o != nullptr && m != nullptr; m = m->getNext(), ++k)
					if (m == o) { oi = k; break; }
				h = pf27Fnv(h, (UnsignedInt)oi);
				if (o != nullptr) { h = pf27Fnv(h, pf27FloatBits(dist)); h = pf27Fnv(h, pf27FloatBits(dir.x)); h = pf27Fnv(h, pf27FloatBits(dir.y)); }
				// the back links, as list positions: node i's previous must be node i-1
				Int pi = -2;
				Int k2 = 0;
				const PathNode *pv = n->getPrevious();
				if (pv == nullptr) pi = -1;
				for (const PathNode *m = path->getFirstNode(); pv != nullptr && m != nullptr; m = m->getNext(), ++k2)
					if (m == pv) { pi = k2; break; }
				h = pf27Fnv(h, (UnsignedInt)pi);
				if (n == path->m_pathTail) h = pf27Fnv(h, 0x5441494Cu ^ nodes);	// where the tail is
				++nodes;
			}
		}
		// the cells, in index order, each once
		std::sort(m_traceCells, m_traceCells + m_traceN);
		Int last = -1;
		for (Int i = 0; i < m_traceN; ++i)
		{
			const Int idx = m_traceCells[i];
			if (idx == last)
				continue;
			last = idx;
			PathfindCell *c = cellAtAnySearch(idx);
			if (c == nullptr)
				continue;
			h = pf27Fnv(h, (UnsignedInt)idx);
			h = pf27Fnv(h, (UnsignedInt)c->isBlockedByAlly());
			h = pf27Fnv(h, (UnsignedInt)c->hasInfo());
			if (c->hasInfo())
			{	// everything a later search or the game can read from a kept info
				const PathfindCellInfo *in = c->m_info;
				h = pf27Fnv(h, in->m_costSoFar);
				h = pf27Fnv(h, in->m_totalCost);
				h = pf27Fnv(h, in->m_pathParent == nullptr ? 0xFFFFFFFFu : (UnsignedInt)indexOf(in->m_pathParent->m_cell));
				h = pf27Fnv(h, ((UnsignedInt)in->m_open) | ((UnsignedInt)in->m_closed << 1) |
					((in->m_nextOpen != nullptr) ? 4u : 0u) | ((in->m_prevOpen != nullptr) ? 8u : 0u) |
					((in->m_cell == c) ? 16u : 0u) | ((UnsignedInt)in->m_isFree << 5));
				h = pf27Fnv(h, ((UnsignedInt)(UnsignedShort)in->m_pos.x) | ((UnsignedInt)(UnsignedShort)in->m_pos.y << 16));
				h = pf27Fnv(h, in->m_goalUnitID); h = pf27Fnv(h, in->m_posUnitID); h = pf27Fnv(h, in->m_goalAircraftID);
			}
		}
		h = pf27Fnv(h, (UnsignedInt)pf->m_isTunneling);
		h = pf27Fnv(h, (UnsignedInt)pf->m_cumulativeCellsAllocated);
		h = pf27Fnv(h, s_pathfindCellInfoFree);
		const PathfindZoneManager &zm = pf->m_zoneManager;
		if (zm.m_passableBlocks != nullptr)
		{
			const Int nb = zm.m_zoneBlockExtent.x * zm.m_zoneBlockExtent.y;
			for (Int i = 0; i < nb; ++i)
				h = pf27Fnv(h, (UnsignedInt)zm.m_passableBlocks[i]);
		}
		s_pf27TraceHash = pf27Fnv(s_pf27TraceHash, h);
		++s_pf27TraceN;
		if (pf27TraceLevel() >= 2)
			fprintf(stderr, "[GXPF27S] f=%u i=%u obj=%u nodes=%u cells=%d h=%08x\n",
				TheGameLogic ? (unsigned)TheGameLogic->getFrame() : 0u, s_pf27TraceN, obj ? (unsigned)obj->getID() : 0u,
				nodes, m_traceN, h);
	}

	// ------------------------------------------------------------------------------------------------------------
	// Search entry points
	// ------------------------------------------------------------------------------------------------------------

	/// GX_PF27=new: the whole search after legacy's prologue (startPathfind already done), exits included.
	Path *findInternal(Object *obj, const LocomotorSet &loco, const Coord3D *from, Bool isHuman, Bool centerInCell,
	                   Int radius, const ICoord2D &startCellNdx, PathfindCell *startCell, PathfindCell *goalCell)
	{
		Pathfinder *pf = m_pf;
		const Bool found = run(obj, loco, isHuman, centerInCell, radius, startCellNdx, startCell, goalCell, false);
		if (!m_ok)
		{	// A cell outside the dense index. prepare() and getCell make this impossible (every cell getCell returns
			// lies in the ground block or a layer block and carries its layer id); if it ever happens the shared
			// state is half-written, so stop loudly rather than return a quietly different answer.
			RELEASE_CRASH(("PF27: search left the dense cell index"));
			return nullptr;
		}
		pf->m_isTunneling = false;
		traceCaptureKernel(startCell);
		writeBack();
		Path *path = nullptr;
		if (found)
		{
			materialiseChain(m_goalIdx);
			path = pf->buildActualPath(obj, loco.getValidSurfaces(), from, goalCell, centerInCell, false);
		}
		// legacy: cleanOpenAndClosedLists(), then the goal's info (on success the goal was popped, so it is on no
		// list; on failure it was never reached). The start is on the closed list either way, unless it is the goal.
		finish();
		goalCell->releaseInfo();
		return path;
	}

	/// GX_PF27=check: the same search as a shadow - nothing outside this object is written. The caller saves and
	/// restores m_isTunneling around it and bumps s_gxMoveGen after it.
	void shadowInternal(Object *obj, const LocomotorSet &loco, Bool isHuman, Bool centerInCell, Int radius,
	                    const ICoord2D &startCellNdx, PathfindCell *startCell, PathfindCell *goalCell)
	{
		m_shadowFound = run(obj, loco, isHuman, centerInCell, radius, startCellNdx, startCell, goalCell, true);
		m_shadowTunneling = m_pf->m_isTunneling;
	}

	/// GX_PF27=check: called by the legacy loop at its exit, BEFORE it cleans its lists. Compares everything.
	void compareWithLegacy(Bool legacyFound, PathfindCell *goalCell, UnsignedInt takesAtStart, const Object *obj,
	                       const Coord3D *from, const Coord3D *to)
	{
		++s_pf27Searches;
		Pathfinder *pf = m_pf;
		const char *why = nullptr;
		Int whyIdx = -1;
		if (!m_ok) why = "kernel-not-ok";
		if (why == nullptr && legacyFound != m_shadowFound) why = "found";
		if (why == nullptr && pf->m_isTunneling != m_shadowTunneling) why = "tunneling";
		if (why == nullptr && (UnsignedInt)(s_pathfindCellInfoTakes - takesAtStart) != m_virtualTakes) why = "takes";
		UnsignedInt legacyListed = 0;
		for (Int pass = 0; pass < 2 && why == nullptr; ++pass)
		{
			PathfindCellList &list = pass == 0 ? pf->m_openList : pf->m_closedList;
			for (PathfindCell *c = list.getHead(); c != nullptr && why == nullptr; c = c->getNextOpen())
			{
				++legacyListed;
				const Int idx = indexOf(c);
				if (idx < 0) { why = "index"; break; }
				whyIdx = idx;
				const Rec &r = m_recs[idx];
				if (r.stamp != m_gen) { why = "untouched"; break; }
				const Bool open = pass == 0;
				if ((open && !(r.flags & F_OPEN)) || (!open && !(r.flags & F_CLOSED))) { why = "list"; break; }
				if (r.g != c->getCostSoFar() || r.f != c->getTotalCost()) { why = "cost"; break; }
				PathfindCell *lp = c->getParentCell();
				const Int lpi = lp ? indexOf(lp) : -1;
				if (lpi != r.parent) { why = "parent"; break; }
				if (r.x != c->getXIndex() || r.y != c->getYIndex()) { why = "coord"; break; }
			}
		}
		if (why == nullptr && legacyListed != (UnsignedInt)(m_open.size() + m_closedCount)) why = "count";
		if (why == nullptr && legacyFound)
		{	// the goal was popped: it is on neither list
			whyIdx = m_goalIdx;
			const Rec &r = m_recs[m_goalIdx];
			PathfindCell *lp = goalCell->getParentCell();
			if (r.g != goalCell->getCostSoFar() || r.f != goalCell->getTotalCost() || (lp ? indexOf(lp) : -1) != r.parent)
				why = "goal";
		}
		for (Int i = 0; i < m_numTouched && why == nullptr; ++i)
		{	// every cell either side could have written: the persistent bit must agree
			const Int idx = m_touched[i];
			const Rec &r = m_recs[idx];
			const Bool expect = (r.flags & F_WROTE_BLOCKED) ? (r.flags & F_BLOCKED) != 0 : (r.flags2 & F2_ORIG_BLOCKED) != 0;
			if ((cellAt(idx)->isBlockedByAlly() != 0) != expect) { why = "blockedByAlly"; whyIdx = idx; }
		}
		if (why == nullptr) { ++s_pf27Ok; return; }
		++s_pf27Bad;
		if (s_pf27BadDumps < 20)
		{
			++s_pf27BadDumps;
			fprintf(stderr, "[GXPF27] BAD why=%s f=%u obj=%u tmpl=%s from=%.2f,%.2f to=%.2f,%.2f found=%d/%d cell=%d,%d"
				" listed=%u/%d takes=%u/%u\n", why, TheGameLogic ? (unsigned)TheGameLogic->getFrame() : 0u,
				obj ? (unsigned)obj->getID() : 0u, obj ? obj->getTemplate()->getName().str() : "-",
				from->x, from->y, to->x, to->y, (Int)legacyFound, (Int)m_shadowFound,
				whyIdx >= 0 ? (Int)m_recs[whyIdx].x : -1, whyIdx >= 0 ? (Int)m_recs[whyIdx].y : -1,
				legacyListed, m_open.size() + m_closedCount,
				(UnsignedInt)(s_pathfindCellInfoTakes - takesAtStart), m_virtualTakes);
			fflush(stderr);
		}
	}

private:
	// ------------------------------------------------------------------------------------------------------------
	// The search
	// ------------------------------------------------------------------------------------------------------------

	Bool run(Object *obj, const LocomotorSet &loco, Bool isHuman, Bool centerInCell, Int radius,
	         const ICoord2D &startCellNdx, PathfindCell *startCell, PathfindCell *goalCell, Bool shadow)
	{
		++m_gen;
		if (m_gen == 0)
		{	// 2^32 searches: wipe the stamps once
			memset(m_recs, 0, sizeof(Rec) * m_cap);
			m_gen = 1;
		}
		m_open.clear();
		m_numTouched = 0;
		m_closedCount = 0;
		m_virtualTakes = 0;
		m_ok = true;
		m_shadow = shadow;
		m_obj = obj;
		m_loco = &loco;
		m_surfaces = loco.getValidSurfaces();
		m_isHuman = isHuman;
		m_centerInCell = centerInCell;
		m_radius = radius;
		m_startNdx = startCellNdx;
		m_goalCell = goalCell;
		m_takesAtStart = s_pathfindCellInfoTakes;	// legacy: infosAtSearchStart, read right before its loop
		m_freeAtStart = s_pathfindCellInfoFree;
		// Legacy computes both per expansion (examineNeighboringCells); they cannot change inside one search.
		m_isCrusher = obj ? obj->getCrusherLevel() > 0 : false;
		m_canPathThroughUnits = false;
		if (obj && obj->getAIUpdateInterface())
			m_canPathThroughUnits = obj->getAIUpdateInterface()->canPathThroughUnits();
		m_downhillOnly = loco.isDownhillOnly();

		// Start and goal hold real infos from the prologue (legacy allocated them before infosAtSearchStart).
		m_goalIdx = touchExisting(goalCell);
		const Int s = touchExisting(startCell);
		if (m_goalIdx < 0 || s < 0) { m_ok = false; return false; }
		if (s == m_goalIdx)
			++s_pf27StartGoal;
		{	// startPathfind (already run for real on the info): costSoFar 0, totalCost = costToGoal
			Rec &r = m_recs[s];
			r.g = 0;
			r.f = startCell->getTotalCost();
			r.parent = -1;
			r.flags |= F_COSTSET;
			pushOpen(s);
		}

		while (!m_open.empty())
		{
			const Int p = m_open.pop();
			m_recs[p].flags &= ~F_OPEN;
			if (p == m_goalIdx)
				return m_ok;
			m_recs[p].flags |= F_CLOSED;
			++m_closedCount;
			if (mayGrow())
			{
				checkChangeLayers(p);
				examine(p);
			}
			else
			{
				const UnsignedInt takes = (UnsignedInt)(s_pathfindCellInfoTakes - m_takesAtStart) + m_virtualTakes;
				if (takes >= MAX_INTERNAL_PATH_INFO_COUNT) ++s_pf27CapTakes; else ++s_pf27CapFloor;
			}
			if (!m_ok)
				return false;
		}
		++s_pf27Fail;
		return false;
	}

	Bool mayGrow() const
	{
		const UnsignedInt takes = (UnsignedInt)(s_pathfindCellInfoTakes - m_takesAtStart) + m_virtualTakes;
		const UnsignedInt freeNow = s_pathfindCellInfoFree - m_virtualTakes;
		return takes < MAX_INTERNAL_PATH_INFO_COUNT && freeNow > PATHFIND_SEARCH_FREE_RESERVE;
	}

	/// A cell met for the first time this search gets a fresh record. x/y are what legacy reads: the info's m_pos when
	/// the cell already has an info (persistent unit stamps, or the prologue's start/goal), else the coordinates the
	/// legacy code would have given allocateInfo.
	Int touch(PathfindCell *c, Int x, Int y)
	{
		const Int idx = indexOf(c);
		if (idx < 0) { m_ok = false; return -1; }
		Rec &r = m_recs[idx];
		if (r.stamp != m_gen)
		{
			r.stamp = m_gen;
			r.flags = 0;
			r.flags2 = c->isBlockedByAlly() ? F2_ORIG_BLOCKED : 0;
			r.layer = (UnsignedByte)c->getLayer();
			r.parent = -1;
			r.g = 0;
			r.f = 0;
			if (c->hasInfo())
			{
				r.flags |= F_INFO;
				r.flags2 |= F2_REAL_INFO;
				r.x = c->getXIndex();
				r.y = c->getYIndex();
			}
			else
			{
				r.x = (UnsignedShort)x;
				r.y = (UnsignedShort)y;
			}
			m_touched[m_numTouched++] = idx;
		}
		return idx;
	}

	Int touchExisting(PathfindCell *c)
	{
		return touch(c, c->hasInfo() ? c->getXIndex() : 0, c->hasInfo() ? c->getYIndex() : 0);
	}

	/// Legacy allocateInfo at this point. A cell without an info takes one from the pool: emulated, except an obstacle
	/// cell, whose info legacy never gives back (releaseInfo returns early for CELL_OBSTACLE) - that one is real.
	void allocate(Int idx, PathfindCell *c)
	{
		Rec &r = m_recs[idx];
		if (r.flags & F_INFO)
			return;
		r.flags |= F_INFO;
		if (!m_shadow && c->getType() == PathfindCell::CELL_OBSTACLE)
		{
			ICoord2D pos;
			pos.x = r.x;
			pos.y = r.y;
			c->allocateInfo(pos);	// counted by the real pool gauges
			r.flags2 |= F2_REAL_ALLOC | F2_REAL_INFO;
			return;
		}
		++m_virtualTakes;
	}

	void pushOpen(Int idx)
	{
		Rec &r = m_recs[idx];
		r.flags |= F_OPEN;
		if (r.f == 0xFFFFu && m_shadow) ++s_pf27Sat;
		m_open.push(idx, r.f);
	}

	void setBlocked(Int idx, Bool v)
	{
		Rec &r = m_recs[idx];
		r.flags |= F_WROTE_BLOCKED;
		if (v) r.flags |= F_BLOCKED; else r.flags &= ~F_BLOCKED;
	}

	/// PathfindCell::costToGoal on the coordinates legacy reads.
	UnsignedInt costToGoal(const Rec &r) const
	{
		const Rec &g = m_recs[m_goalIdx];
		Int dx = (Int)r.x - (Int)g.x;
		Int dy = (Int)r.y - (Int)g.y;
		if (dx < 0) dx = -dx;
		if (dy < 0) dy = -dy;
		if (dx > dy)
			return COST_ORTHOGONAL * dx + (COST_ORTHOGONAL * dy) / 2;
		return COST_ORTHOGONAL * dy + (COST_ORTHOGONAL * dx) / 2;
	}

	/// PathfindCell::costSoFar(parent) for the cell idx entered from parent p (turn penalty from p's parent).
	UnsignedInt costSoFar(Int idx, Int p, Bool pinched) const
	{
		const Rec &r = m_recs[idx];
		const Rec &pr = m_recs[p];
		ICoord2D prevDir;
		prevDir.x = (Int)pr.x - (Int)r.x;
		prevDir.y = (Int)pr.y - (Int)r.y;
		Int cost;
		if (prevDir.x == 0 || prevDir.y == 0)
			cost = pr.g + COST_ORTHOGONAL;
		else
			cost = pr.g + COST_DIAGONAL;
		if (pinched)
			cost += 1 * COST_DIAGONAL;
		Int numTurns = 0;
		if (pr.parent >= 0)
		{
			const Rec &pp = m_recs[pr.parent];
			ICoord2D dir;
			dir.x = (Int)pp.x - (Int)pr.x;
			dir.y = (Int)pp.y - (Int)pr.y;
			if (dir.x != prevDir.x || dir.y != prevDir.y)
			{
				const Int dot = dir.x * prevDir.x + dir.y * prevDir.y;
				if (dot > 0)
					numTurns = 4;
				else if (dot == 0)
					numTurns = 8;
				else
					numTurns = 16;
			}
		}
		return cost + numTurns;
	}

	/// Pathfinder::checkChangeLayers.
	void checkChangeLayers(Int p)
	{
		Pathfinder *pf = m_pf;
		PathfindCell *pc = cellAt(p);
		if (pc->getConnectLayer() == LAYER_INVALID)
			return;
		const Rec pr = m_recs[p];
		PathfindCell *nc = pf->getCell(pc->getConnectLayer(), pr.x, pr.y);
		if (nc == nullptr)
			return;
		const Int n = touch(nc, pr.x, pr.y);
		if (n < 0)
			return;
		Rec &r = m_recs[n];
		if (r.flags & (F_OPEN | F_CLOSED))
			return;
		allocate(n, nc);
		r.parent = p;
		gxNotePathCost(pr.g);
		r.g = pr.g;
		gxNotePathCost(pr.f);
		r.f = pr.f;
		r.flags |= F_COSTSET;
		if (m_shadow) ++s_pf27LayerChange;
		pushOpen(n);
	}

	/// The line-walk predicate of examineCellsCallback for `to` entered from a cell on layer fromLayer (everything
	/// up to and including the cliff test). Pure within one search; remembered per cell when fromLayer is the cell's
	/// own layer (the common case), recomputed otherwise.
	Bool lineValid(Int idx, PathfindCell *to, PathfindCell *from, Int x, Int y)
	{
		Rec &r = m_recs[idx];
		const Bool cacheable = (from->getLayer() == to->getLayer());
		if (cacheable && (r.flags & F_LINE_KNOWN))
			return (r.flags & F_LINE_OK) != 0;
		if (!cacheable && m_shadow)
			++s_pf27XLayer;
		Pathfinder *pf = m_pf;
		Bool ok = true;
		if (!pf->validMovementPosition(m_isCrusher, m_surfaces, to, from)) ok = false;
		else if (to->getLayer() == LAYER_GROUND && !pf->m_zoneManager.isPassable(x, y)) ok = false;
		else if (to->getPinched()) ok = false;
		else if (m_isHuman && (x < pf->m_logicalExtent.lo.x || y < pf->m_logicalExtent.lo.y ||
		                       x > pf->m_logicalExtent.hi.x || y > pf->m_logicalExtent.hi.y)) ok = false;
		else
		{
			TCheckMovementInfo info;
			info.cell.x = x;
			info.cell.y = y;
			info.layer = from->getLayer();
			info.centerInCell = m_centerInCell;
			info.radius = m_radius;
			info.considerTransient = false;
			info.acceptableSurfaces = m_surfaces;
			if (!pf->checkForMovementInSearch(m_obj, info)) ok = false;
			else if (info.enemyFixed) ok = false;
			else if (info.allyFixedCount) ok = false;
			else if (to->getType() == PathfindCell::CELL_CLIFF) ok = false;
		}
		if (cacheable)
		{
			r.flags |= F_LINE_KNOWN;
			if (ok) r.flags |= F_LINE_OK;
		}
		return ok;
	}

	/// examineCellsCallback for one step from -> to. Returns true to keep walking.
	Bool lineStep(Int fromIdx, PathfindCell *from, PathfindCell *to, Int x, Int y)
	{
		const Int idx = touch(to, x, y);
		if (idx < 0)
			return false;
		if (!lineValid(idx, to, from, x, y))
			return false;
		const UnsignedInt newCostSoFar = m_recs[fromIdx].g + 0.5f*COST_ORTHOGONAL;	// legacy expression, exact for g <= 65535
		allocate(idx, to);
		setBlocked(idx, false);
		Rec &r = m_recs[idx];
		if (r.flags & (F_OPEN | F_CLOSED))
		{
			if (r.g <= newCostSoFar)
				return true;	// keep going
		}
		const UnsignedInt costRemaining = costToGoal(r);
		gxNotePathCost(newCostSoFar);	// legacy's setters count the raw value (GX_PATHCOST_COUNT)
		r.g = gxPathCost16(newCostSoFar);
		r.parent = fromIdx;
		gxNotePathCost((UnsignedInt)r.g + costRemaining);
		r.f = gxPathCost16((UnsignedInt)r.g + costRemaining);
		r.flags |= F_COSTSET;
		if (r.flags & F_CLOSED)
		{
			r.flags &= ~F_CLOSED;
			--m_closedCount;
			if (m_shadow) ++s_pf27Reopen;
		}
		if (r.flags & F_OPEN)
		{
			m_open.remove(idx);
			r.flags &= ~F_OPEN;
		}
		pushOpen(idx);
		return true;
	}

	/// The line walk: iterateCellsAlongLine(parent, goal, parent's layer, examineCellsCallback) on the same Bresenham.
	void lineWalk(Int p, PathfindCell *pc)
	{
		const Rec &pr = m_recs[p];
		const Rec &gr = m_recs[m_goalIdx];
		ICoord2D start, end;
		start.x = pr.x;
		start.y = pr.y;
		end.x = gr.x;
		end.y = gr.y;
		forEachCellAlongLine(m_pf, start, end, pc->getLayer(),
			[this](PathfindCell *from, PathfindCell *to, Int x, Int y) -> Int
			{
				if (from == nullptr)
					return 0;	// legacy callback does nothing on the first cell
				const Int fromIdx = indexOf(from);
				if (fromIdx < 0) { m_ok = false; return 1; }
				return lineStep(fromIdx, from, to, x, y) ? 0 : 1;
			});
	}

	/// Pathfinder::examineNeighboringCells(..., NO_ATTACK).
	void examine(Int p)
	{
		Pathfinder *pf = m_pf;
		PathfindCell *pc = cellAt(p);
		const Bool walkLine = !pf->m_isTunneling && !m_downhillOnly && m_goalCell;
		if (walkLine && pf27Mutant() != 1)
			lineWalk(p, pc);
		if (!m_ok)
			return;
		examineNeighbours(p, pc);
		if (walkLine && pf27Mutant() == 1)
			lineWalk(p, pc);
	}

	void examineNeighbours(Int p, PathfindCell *pc)
	{
		Pathfinder *pf = m_pf;

		static const ICoord2D delta[] =
		{
			{ 1, 0 }, { 0, 1 }, { -1, 0 }, { 0, -1 },
			{ 1, 1 }, { -1, 1 }, { -1, -1 }, { 1, -1 }
		};
		const Int adjacent[5] = {0, 1, 2, 3, 0};
		Bool neighborFlags[8] = { 0 };
		const Rec pr = m_recs[p];
		const PathfindLayerEnum layer = pc->getLayer();
		if (layer != LAYER_GROUND && m_shadow) ++s_pf27OnLayer;

		for (Int i = 0; i < 8; i++)
		{
			neighborFlags[i] = false;
			const Int nx = (Int)pr.x + delta[i].x;
			const Int ny = (Int)pr.y + delta[i].y;
			PathfindCell *nc = pf->getCell(layer, nx, ny);
			if (nc == nullptr)
				continue;

			Bool notZonePassable = false;
			if ((nc->getLayer() == LAYER_GROUND) && !pf->m_zoneManager.isPassable(nx, ny))
				notZonePassable = true;

			if (m_isHuman && (nx < pf->m_logicalExtent.lo.x || nx > pf->m_logicalExtent.hi.x ||
			                  ny < pf->m_logicalExtent.lo.y || ny > pf->m_logicalExtent.hi.y))
				continue;

			const Int n = touch(nc, nx, ny);
			if (n < 0)
				return;
			if (m_recs[n].flags & (F_OPEN | F_CLOSED))
				continue;

			if (i >= 4)
			{
				if (!neighborFlags[adjacent[i-4]] && !neighborFlags[adjacent[i-3]])
					continue;
			}

			if (m_downhillOnly)
			{
				Coord3D fromPos;
				fromPos.x = pr.x * PATHFIND_CELL_SIZE_F;
				fromPos.y = pr.y * PATHFIND_CELL_SIZE_F;
				fromPos.z = TheTerrainLogic->getGroundHeight(fromPos.x, fromPos.y);
				Coord3D toPos;
				toPos.x = nx * PATHFIND_CELL_SIZE_F;
				toPos.y = ny * PATHFIND_CELL_SIZE_F;
				toPos.z = TheTerrainLogic->getGroundHeight(toPos.x, toPos.y);
				if (fromPos.z < toPos.z)
					continue;
			}

			Bool movementValid = pf->validMovementPosition(m_isCrusher, m_surfaces, nc, pc);
			Bool dozerHack = false;
			if (!movementValid && m_obj->isKindOf(KINDOF_DOZER) && nc->getType() == PathfindCell::CELL_OBSTACLE)
			{
				Object *obstacle = TheGameLogic->findObjectByID(nc->getObstacleID());
				if (obstacle && !(m_obj->getRelationship(obstacle) == ENEMIES))
				{
					movementValid = true;
					dozerHack = true;
				}
			}

			if (!movementValid && !pf->m_isTunneling)
				continue;

			if (!dozerHack)
				neighborFlags[i] = true;

			TCheckMovementInfo info;
			info.cell.x = nx;
			info.cell.y = ny;
			info.layer = layer;
			info.centerInCell = m_centerInCell;
			info.radius = m_radius;
			info.considerTransient = false;
			info.acceptableSurfaces = m_surfaces;
			Int dx = nx - m_startNdx.x;
			Int dy = ny - m_startNdx.y;
			if (dx < 0) dx = -dx;
			if (dy < 0) dy = -dy;
			if (!pf->checkForMovementInSearch(m_obj, info) || info.enemyFixed)
			{
				if (!pf->m_isTunneling)
					continue;
				movementValid = false;
			}

			if (movementValid && !nc->getPinched())
			{
				if (pf->m_isTunneling && m_shadow) ++s_pf27Tunnel;
				pf->m_isTunneling = false;
			}

			allocate(n, nc);

			Rec &r = m_recs[n];
			UnsignedInt newCostSoFar = costSoFar(n, p, nc->getPinched());
			if (info.allyMoving && dx < 10 && dy < 10)
				newCostSoFar += 3*COST_DIAGONAL;

			if (nc->getType() == PathfindCell::CELL_CLIFF && !nc->getPinched())
			{
				Coord3D fromPos;
				fromPos.x = pr.x * PATHFIND_CELL_SIZE_F;
				fromPos.y = pr.y * PATHFIND_CELL_SIZE_F;
				fromPos.z = TheTerrainLogic->getGroundHeight(fromPos.x, fromPos.y);
				Coord3D toPos;
				toPos.x = nx * PATHFIND_CELL_SIZE_F;
				toPos.y = ny * PATHFIND_CELL_SIZE_F;
				toPos.z = TheTerrainLogic->getGroundHeight(toPos.x, toPos.y);
				if (fabs(fromPos.z - toPos.z) < PATHFIND_CELL_SIZE_F)
					newCostSoFar += 7*COST_DIAGONAL;
			}
			else if (nc->getPinched())
			{
				newCostSoFar += COST_ORTHOGONAL;
			}

			setBlocked(n, false);
			if (info.allyFixedCount > 0)
			{
#if RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING
				newCostSoFar += 3*COST_DIAGONAL*info.allyFixedCount;	// legacy's own retail Generals variant
#else
				newCostSoFar += 3*COST_DIAGONAL;
#endif
				if (!m_canPathThroughUnits)
					setBlocked(n, true);
			}

			Int costRemaining = 0;
			if (m_goalCell)
				costRemaining = costToGoal(r);

			if (notZonePassable)
				newCostSoFar += 100*COST_ORTHOGONAL;
			if (nc->getType() == PathfindCell::CELL_OBSTACLE)
				newCostSoFar += 100*COST_ORTHOGONAL;
			if (pf->m_isTunneling)
			{
				if (!pf->validMovementPosition(m_isCrusher, m_surfaces, nc, pc))
					newCostSoFar += 10*COST_ORTHOGONAL;
			}

			gxNotePathCost(newCostSoFar);
			r.g = gxPathCost16(newCostSoFar);
			r.parent = p;
			if (pf->m_isTunneling)
				costRemaining = 0;
			gxNotePathCost((UnsignedInt)r.g + (UnsignedInt)costRemaining);
			r.f = gxPathCost16((UnsignedInt)r.g + (UnsignedInt)costRemaining);
			r.flags |= F_COSTSET;
			// legacy removes it from a list here if it is on one; the test above made sure it is on none
			pushOpen(n);
		}
	}

	// ------------------------------------------------------------------------------------------------------------
	// Exits
	// ------------------------------------------------------------------------------------------------------------

	/// The persistent effects legacy leaves on cells and on infos that outlive the search.
	void writeBack()
	{
		m_mutantDone = false;
		for (Int i = 0; i < m_numTouched; ++i)
		{
			const Int idx = m_touched[i];
			const Rec &r = m_recs[idx];
			PathfindCell *c = cellAt(idx);
			if (r.flags & F_WROTE_BLOCKED)
			{
				const Bool v = (r.flags & F_BLOCKED) != 0;
				if (pf27Mutant() == 2 && !m_mutantDone && (c->isBlockedByAlly() != 0) != v)
					m_mutantDone = true;
				else
					c->setBlockedByAlly(v);
			}
			if ((r.flags & F_COSTSET) && c->hasInfo())
			{
				c->setCostSoFar(r.g);
				c->setTotalCost(r.f);
			}
		}
	}

	/// Real infos on the goal -> start chain, linked, so the unchanged buildActualPath can read the path.
	void materialiseChain(Int goalIdx)
	{
		Int guard = m_numTouched + 1;
		for (Int idx = goalIdx; idx >= 0 && guard-- > 0; idx = m_recs[idx].parent)
		{
			PathfindCell *c = cellAt(idx);
			const Rec &r = m_recs[idx];
			ICoord2D pos;
			pos.x = r.x;
			pos.y = r.y;
			// Cannot fail: the chain is part of the listed set, which legacy held infos for at this moment, and the
			// kernel took none of them from the pool (virtual takes). Asserted rather than assumed.
			if (!c->allocateInfo(pos))
			{
				RELEASE_CRASH(("PF27: cell info pool exhausted while materialising a path"));
				return;
			}
			c->setCostSoFar(r.g);
			c->setTotalCost(r.f);
		}
		guard = m_numTouched + 1;
		for (Int idx = goalIdx; idx >= 0 && guard-- > 0; idx = m_recs[idx].parent)
		{
			const Int pi = m_recs[idx].parent;
			if (pi >= 0)
				cellAt(idx)->setParentCell(cellAt(pi));
			else
				cellAt(idx)->clearParentCell();
		}
	}

	/// legacy cleanOpenAndClosedLists: the memo generation, releaseInfo on every cell left on a list (which frees an
	/// info unless the cell is an obstacle, has unit flags or is an aircraft goal - so a releasable info that was
	/// already on a listed cell before the search, e.g. one findAttackPath left behind, is freed here exactly as
	/// legacy frees it), and the released-cell count the queue budget reads. The materialised chain is part of the
	/// listed set (all but the goal), so it goes back here too.
	void finish()
	{
		Pathfinder *pf = m_pf;
		pf->cleanOpenAndClosedLists();	// both legacy lists are empty: bumps s_gxMoveGen, adds 0
		for (Int i = 0; i < m_numTouched; ++i)
		{
			const Int idx = m_touched[i];
			if (!(m_recs[idx].flags & (F_OPEN | F_CLOSED)))
				continue;
			PathfindCell *c = cellAt(idx);
			const Rec &rr = m_recs[idx];
			if ((rr.flags2 & F2_ORIG_BLOCKED) && !(rr.flags & F_WROTE_BLOCKED))
				++s_pf27StaleBlocked;	// a listed cell nobody reset (a layer-change cell): its old bit must survive
			if (c->hasInfo())
			{
				if (c->getType() == PathfindCell::CELL_OBSTACLE) ++s_pf27KeptObstacle;
				else if (c->getFlags() != PathfindCell::NO_UNITS) ++s_pf27KeptUnit;
				else if (c->isAircraftGoal()) ++s_pf27KeptAircraft;
				else if ((rr.flags2 & F2_REAL_INFO) && !(rr.flags2 & F2_REAL_ALLOC) && idx != m_goalIdx &&
				         !onChain(idx)) ++s_pf27PreRelease;	// a releasable info that was there before the search
				if (pf27Mutant() == 4 && !m_mutantDone && c->getType() != PathfindCell::CELL_OBSTACLE &&
				    c->getFlags() == PathfindCell::NO_UNITS && !c->isAircraftGoal())
				{
					m_mutantDone = true;	// a releasable info left behind
					continue;
				}
				c->releaseInfo();
			}
		}
		pf->m_cumulativeCellsAllocated += (Int)(m_open.size() + m_closedCount) + (pf27Mutant() == 3 ? 1 : 0);
	}

	Bool onChain(Int idx) const
	{
		Int guard = m_numTouched + 1;
		for (Int i = m_goalIdx; i >= 0 && guard-- > 0; i = m_recs[i].parent)
			if (i == idx)
				return true;
		return false;
	}

	void traceAdd(Int idx)
	{
		if (idx >= 0 && m_traceN < m_cap + 2)
			m_traceCells[m_traceN++] = idx;
	}

	Pathfinder *m_pf;
	Rec *m_recs;
	Int *m_touched;
	Int *m_traceCells;
	Int m_cap;
	UnsignedInt m_gen;
	PF27BucketQueue m_open;

	PathfindCell *m_groundBase;
	Int m_groundCount;
	PathfindCell *m_layerBase[LAYER_LAST + 1];
	Int m_layerCount[LAYER_LAST + 1];
	Int m_layerOffset[LAYER_LAST + 1];

	// the current search
	Int m_numTouched;
	Int m_closedCount;
	UnsignedInt m_virtualTakes;
	UnsignedInt m_takesAtStart;
	UnsignedInt m_freeAtStart;
	Bool m_ok;
	Bool m_shadow;
	Bool m_shadowFound;
	Bool m_shadowTunneling;
	Object *m_obj;
	const LocomotorSet *m_loco;
	LocomotorSurfaceTypeMask m_surfaces;
	Bool m_isHuman;
	Bool m_centerInCell;
	Int m_radius;
	ICoord2D m_startNdx;
	PathfindCell *m_goalCell;
	Int m_goalIdx;
	Bool m_isCrusher;
	Bool m_canPathThroughUnits;
	Bool m_downhillOnly;
	Bool m_tracing;
	Int m_traceN;
	Bool m_mutantDone;
};
