// GeneralsX: the look governor (S14, PLAN_look; wh1ter0se 17/10: "if someone's PC struggles in a big
// fight we lower stuff to compensate, then raise it back").
//
// WHAT. A per-peer, render-only controller. Every look feature registers a few quality STEPS (step 0 = full quality). Once
// per window the governor reads what the last frames cost and, when the frame time is over the player's target, lowers ONE
// step of the knob that relieves the measured limit and costs the least to the eye; when there is headroom again it raises
// the last lowered knob back, slowly, and backs off a knob whose raise put the frame time straight back over. Nothing
// flickers: a change needs a streak of windows, a minimum hold since the last change, and a raise that fails doubles that
// knob's wait.
//
// THE LIMIT, measured not guessed. Per drawn frame: the interval since the previous drawn frame, the busy part of it (the
// interval minus the frame limiter's hold, FramePacer::getTotalHoldNs), the main thread's CPU inside the draw and the wall
// time it waits inside Present ([GXGFX]'s `pres`: the whole downstream - DXVK's translation and submit threads, MoltenVK's
// encode, the GPU). Over budget with a large Present wait = the downstream (GPU side) limits; with a small one = the main
// thread (draw CPU and the logic tick it shares the loop with) limits. GPU-side knobs are cut for the first, CPU-side for the
// second; a knob that relieves both is eligible for either.
//
// RENDER-ONLY. This header is pure logic with no engine include. The knobs it drives are read at render-side sites only
// (ParticleSys's count limit, the heat-smudge flag, the terrain cloud pass, the stencil-shadow screen-size rule), all of which
// retail already lets differ per peer (each player's own Options detail), and none of them writes TheGlobalData: the user's
// settings, which the Options screen saves, are never touched. Core/GameEngine/Include/GameClient is outside the SimID digest.
// It must stay that way: never include this from Common/, GameLogic/ or GameNetwork/.
//
// OFF COSTS NOTHING. g_gxGovEff.active is false unless the governor (or a forced test step) is on, and every read-site helper
// returns the caller's own value then.

#pragma once

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <algorithm>

// ---------------------------------------------------------------------------------------------------------------------------
// The knob registry. A knob's class says which limit its steps relieve. impact[s] is the visual cost of moving TO step s
// (s >= 1): the governor always takes the cheapest next step among the eligible knobs. Native-renderer knobs (render scale,
// shadow-map size, post passes, soft particles) join this table when those features land; a feature without steps is not ready.

enum GxGovClass { GXGOV_GPU = 1, GXGOV_CPU = 2, GXGOV_BOTH = 3 };

enum GxGovKnob
{
	GXK_SHADOW,		// stencil shadow volumes: the screen-size rule (W3DVolumetricShadow.cpp) - fewer rebuilt, uploaded and drawn
	GXK_PART,		// particle count limit, as a share of the player's own MaxParticleCount (ParticleSys.cpp)
	GXK_HEAT,		// heat-distortion smudges (ParticleSys.cpp / W3DParticleSys.cpp): a screen copy plus distorted quads
	GXK_CLOUD,		// the terrain's scrolling cloud-shadow pass (BaseHeightMap::useCloud)
	GXK_FXLIGHT,	// S9 classic light pulses (W3DDisplay::createLightPulse): revived small pulses + a larger reach, while GXFxLight is on
	GXK_GROUND,		// the REMASTERED ground (TerrainTex.cpp, Options.ini GXGround; GameClient/GXGroundSteps.h g_gxGroundGovStep): 1 = its
					// second pass without the detail grain and the macro variation, 2 = the stock ground
	GXK_N
};

enum { GXGOV_MAX_STEPS = 4 };

struct GxGovKnobDef
{
	const char *name;
	int cls;					// GxGovClass mask
	int steps;					// including step 0
	int impact[ GXGOV_MAX_STEPS ];	// impact[0] unused
};

// Lowering order this table gives (ties go to the earlier knob): CPU-bound shadow 1, particles 1, fxlight 1, shadow 2, particles 2,
// fxlight 2, shadow 3, particles 3; GPU-bound shadow 1, particles 1, heat, fxlight 1, shadow 2, particles 2, cloud, fxlight 2, shadow 3,
// particles 3, then ground 2 (the stock ground: it takes back the whole look wh1ter0se approved 14/10) last of all; ground 1 comes after
// fxlight 1 on GPU-bound. fxlight and ground move only while the player has them on (setAvailable).
static const GxGovKnobDef kGxGovKnobs[ GXK_N ] = {
	{ "shadow", GXGOV_BOTH, 4, { 0, 1, 4, 7 } },
	{ "part",   GXGOV_BOTH, 4, { 0, 2, 5, 8 } },
	{ "heat",   GXGOV_GPU,  2, { 0, 3, 0, 0 } },
	{ "cloud",  GXGOV_GPU,  2, { 0, 6, 0, 0 } },
	{ "fxlight", GXGOV_BOTH, 3, { 0, 3, 6, 0 } },	// CPU: the per-vertex terrain relight; GPU: per-object light count
	{ "ground", GXGOV_GPU,  3, { 0, 3, 9, 0 } },	// 1: the grain shows only at the closest zoom, the macro tint is +-10%; 2: last resort
};

// Step values. Step 0 is today's behaviour exactly.
static const float kGxGovShadowMinPx[ 4 ]   = { 1.5f, 3.0f, 6.0f, 12.0f };	// SHADOW_MIN_SCREEN_RADIUS_PX is 1.5
static const float kGxGovShadowStalePx[ 4 ] = { 0.5f, 1.0f, 1.5f, 2.0f };	// SHADOW_STALE_PX is 0.5
static const int   kGxGovPartPct[ 4 ]       = { 100, 70, 50, 35 };

// What the read sites see. Written only by the governor glue (W3DDisplay::draw, after a frame commits).
struct GxGovEffective
{
	bool  active;
	int   step[ GXK_N ];
};

inline GxGovEffective g_gxGovEff = { false, {} };

// Read-site helpers: the caller's own value when the governor is off or at step 0.
inline float gxGovShadowMinPx( float stock )
{
	return ( g_gxGovEff.active && g_gxGovEff.step[ GXK_SHADOW ] > 0 ) ? kGxGovShadowMinPx[ g_gxGovEff.step[ GXK_SHADOW ] ] : stock;
}

inline float gxGovShadowStalePx( float stock )
{
	return ( g_gxGovEff.active && g_gxGovEff.step[ GXK_SHADOW ] > 0 ) ? kGxGovShadowStalePx[ g_gxGovEff.step[ GXK_SHADOW ] ] : stock;
}

// 0 stays 0 (the player turned particles off); any positive limit stays at least 1.
inline int gxGovParticleLimit( int userLimit )
{
	if( !g_gxGovEff.active || g_gxGovEff.step[ GXK_PART ] <= 0 || userLimit <= 0 )
		return userLimit;
	const long long l = (long long)userLimit * kGxGovPartPct[ g_gxGovEff.step[ GXK_PART ] ] / 100;
	return l < 1 ? 1 : (int)l;
}

inline bool gxGovHeatEffects( bool user )
{
	return user && !( g_gxGovEff.active && g_gxGovEff.step[ GXK_HEAT ] > 0 );
}

inline bool gxGovCloudMap( bool user )
{
	return user && !( g_gxGovEff.active && g_gxGovEff.step[ GXK_CLOUD ] > 0 );
}

// S9 classic light pulses: 0 = the feature as configured, 1 = no revived small pulses, 2 = stock pulses (feature off).
inline int gxGovFxLightStep()
{
	return g_gxGovEff.active ? g_gxGovEff.step[ GXK_FXLIGHT ] : 0;
}

// ---------------------------------------------------------------------------------------------------------------------------
// The controller.

struct GxGovSample
{
	int64_t intervalNs;		// since the previous drawn frame
	int64_t busyNs;			// intervalNs minus the frame limiter's hold in it
	int64_t drawCpuNs;		// main-thread CPU inside W3DDisplay::draw
	int64_t presNs;			// main-thread wall inside Present
};

struct GxGovConfig
{
	int targetFps;			// the frame rate to hold
	int windowMs;			// one decision per window
	int lowerWindows;		// over-budget windows in a row before a step down
	int raiseWindows;		// headroom windows in a row before a step up
	int holdLowerMs;		// since the last change, before another step down
	int holdRaiseMs;		// since the last change, before a step up
	int overPct;			// p90 busy above target * overPct/100 = over budget
	int underPct;			// p90 busy below target * underPct/100 = headroom (or, while the target is held, p90 work)
	int holdPct;			// p90 interval at or under target * holdPct/100 = the target is held
	int hitchMs;			// a window with a frame this long is a hitch (shader compile, autosave): no decision
	int probeMs;			// a raise followed by an over-budget window this soon is undone
	int backoffMs;			// first wait before a failed knob may be raised again; doubles per failure
	int backoffMaxMs;
	int gpuPresPct;			// Present wait >= this share of busy = the downstream limits
};

inline GxGovConfig gxGovDefaultConfig( int targetFps )
{
	GxGovConfig c;
	c.targetFps = targetFps;
	c.windowMs = 500;
	c.lowerWindows = 2;
	c.raiseWindows = 6;
	c.holdLowerMs = 1500;
	c.holdRaiseMs = 4000;
	c.overPct = 108;
	c.underPct = 80;
	c.holdPct = 102;
	c.hitchMs = 250;
	c.probeMs = 2500;
	c.backoffMs = 8000;
	c.backoffMaxMs = 120000;
	c.gpuPresPct = 25;
	return c;
}

// GeneralsX: wh1ter0se 17/10 ("yes all green" to the look set): the governor is ON by default. Options.ini
// GXLookGovernor = no turns it off; an absent key reads as this value (W3DDisplay.cpp gxGovInitOnce, its only reader).
static const bool kGxGovKeyDefault = true;

// The frame rate the governor holds. wh1ter0se 17/10 (~18:3x, replacing "the player's frame cap, clamped 60-144" the same day, when a
// 480 cap on a 500 Hz panel would have aimed at 144 and cut every big fight to the floor): "aim for 60 or whatever the server's tickrate".
// So: the game's LOGIC tick rate - tickFps = LOGICFRAMES_PER_SECOND (60 under GX_LOGIC_60HZ, 30 otherwise) - or, in a network game, the
// match's agreed rate when it is lower (netFps = TheNetwork->getFrameRate(), the source FramePacer uses; <= 0 = no network game). Frames
// above the logic rate show no new game state, and a lockstep match cannot run faster than its agreed rate anyway. Never above what the
// display presents (refreshHz, 0 = unknown): a target over the refresh reads every vsync wait as overload. At least
// 30. cfg = Options.ini GXLookGovernorFps or GX_GOVERNOR_FPS (0 = this rule; a configured value is used as given).
inline int gxGovTargetFps( int cfg, int tickFps, int netFps, int refreshHz )
{
	if( cfg > 0 )
		return cfg;
	int t = tickFps;
	if( netFps > 0 && netFps < t )
		t = netFps;
	if( refreshHz > 0 && refreshHz < t )
		t = refreshHz;
	if( t < 30 )
		t = 30;
	return t;
}

// A GX_SHOT capture (the capture clock, gxShotCaptureOn in GameClient.h) compares one logic frame across runs byte for byte, and the
// governor steps from wall-clock timings: two runs under different machine load could draw that frame at different steps (
// ls1017-r1 P2, now that the governor is on by default). While a capture runs the governor holds full quality, unless GX_GOVERNOR names
// it (an arm that tests the governor) or GX_GOVERNOR_FORCE pins its steps (the render-only proofs).
inline bool gxGovHoldsForCapture( bool captureOn, bool envNamesGovernor, bool forced )
{
	return captureOn && !envNamesGovernor && !forced;
}

enum GxGovEvent { GXGOV_EV_NONE, GXGOV_EV_LOWER, GXGOV_EV_RAISE, GXGOV_EV_UNDO, GXGOV_EV_FLOOR };

struct GxGovWindowStats
{
	uint32_t n;
	int64_t  p90IntervalNs, p90BusyNs, p90WorkNs, maxIntervalNs;	// work = busy minus the Present wait
	int64_t  meanBusyNs, meanDrawCpuNs, meanPresNs;
	int      cls;			// GxGovClass of an over-budget window, 0 otherwise
	bool     over, under, hitch;
};

struct GxGovChange
{
	GxGovEvent ev;
	int knob, from, to;
};

class GxLookGovernor
{
public:
	enum { MAX_WINDOW_FRAMES = 1024, STACK_MAX = 32 };

	GxLookGovernor() { configure( gxGovDefaultConfig( 60 ) ); reset( 0 ); }

	void configure( const GxGovConfig &c ) { m_cfg = c; }

	// A knob whose feature the player has off (shadows off, particles 0, heat off, no cloud map, GXFxLight off) saves nothing when
	// stepped: the governor skips it instead of spending a hold period on it. Default: every knob available.
	void setAvailable( int knob, bool on ) { m_avail[ knob ] = on; }
	const GxGovConfig &config() const { return m_cfg; }

	// A new match (or the governor switched on): full quality, no history.
	void reset( int64_t nowNs )
	{
		memset( m_step, 0, sizeof( m_step ) );
		m_stackN = 0;
		for( int k = 0; k < GXK_N; ++k )
			m_avail[ k ] = true;
		for( int k = 0; k < GXK_N; ++k )
		{
			m_backoffMs[ k ] = 0;
			m_raiseAllowedNs[ k ] = 0;
			m_raisedOkSinceNs[ k ] = 0;
		}
		m_lastChangeNs = nowNs;
		m_overStreak = m_underStreak = m_hitchStreak = 0;
		m_probeKnob = -1;
		m_probeNs = 0;
		m_floorLogged = false;
		beginWindow( nowNs );
		m_last = GxGovWindowStats();
		memset( &m_last, 0, sizeof( m_last ) );
		m_change.ev = GXGOV_EV_NONE;
		m_change.knob = m_change.from = m_change.to = -1;
		m_windows = 0;
	}

	// One drawn frame. Returns true when a window closed (stats() and change() then describe it).
	bool addFrame( int64_t nowNs, const GxGovSample &s )
	{
		if( m_n < MAX_WINDOW_FRAMES )
		{
			m_interval[ m_n ] = s.intervalNs;
			m_busy[ m_n ] = s.busyNs;
			m_work[ m_n ] = s.busyNs > s.presNs ? s.busyNs - s.presNs : 0;
			++m_n;
		}
		m_sumBusy += s.busyNs;
		m_sumDrawCpu += s.drawCpuNs;
		m_sumPres += s.presNs;
		++m_frames;
		if( s.intervalNs > m_maxInterval )
			m_maxInterval = s.intervalNs;
		if( nowNs - m_windowStartNs < (int64_t)m_cfg.windowMs * 1000000 )
			return false;
		evaluate( nowNs );
		beginWindow( nowNs );
		return true;
	}

	int step( int knob ) const { return m_step[ knob ]; }
	const GxGovWindowStats &stats() const { return m_last; }
	const GxGovChange &change() const { return m_change; }
	uint32_t windows() const { return m_windows; }
	int stackDepth() const { return m_stackN; }

	static int64_t percentile( int64_t *v, uint32_t n, int pct )
	{
		if( n == 0 )
			return 0;
		uint32_t k = (uint32_t)( ( (uint64_t)n * (uint64_t)pct ) / 100 );
		if( k >= n )
			k = n - 1;
		std::nth_element( v, v + k, v + n );
		return v[ k ];
	}

private:
	void beginWindow( int64_t nowNs )
	{
		m_windowStartNs = nowNs;
		m_n = 0;
		m_frames = 0;
		m_sumBusy = m_sumDrawCpu = m_sumPres = 0;
		m_maxInterval = 0;
	}

	int64_t targetNs() const { return m_cfg.targetFps > 0 ? 1000000000LL / m_cfg.targetFps : 16666667LL; }

	// The cheapest next step among the knobs this class may cut; -1 when every one is at its last step.
	int pickLower( int cls ) const
	{
		int best = -1, bestImpact = 1 << 30;
		for( int k = 0; k < GXK_N; ++k )
		{
			const GxGovKnobDef &d = kGxGovKnobs[ k ];
			if( !m_avail[ k ] || ( d.cls & cls ) == 0 || m_step[ k ] + 1 >= d.steps )
				continue;
			const int imp = d.impact[ m_step[ k ] + 1 ];
			if( imp < bestImpact )
			{
				bestImpact = imp;
				best = k;
			}
		}
		return best;
	}

	void lower( int knob, int64_t nowNs, GxGovEvent ev )
	{
		m_change.ev = ev;
		m_change.knob = knob;
		m_change.from = m_step[ knob ];
		m_change.to = ++m_step[ knob ];
		if( m_stackN < STACK_MAX )
			m_stack[ m_stackN++ ] = knob;
		m_lastChangeNs = nowNs;
		m_raisedOkSinceNs[ knob ] = 0;
	}

	void evaluate( int64_t nowNs )
	{
		++m_windows;
		m_change.ev = GXGOV_EV_NONE;
		m_change.knob = m_change.from = m_change.to = -1;
		GxGovWindowStats &w = m_last;
		memset( &w, 0, sizeof( w ) );
		w.n = m_frames;
		if( m_frames == 0 )
			return;
		const uint32_t n = m_n;
		w.maxIntervalNs = m_maxInterval;
		w.p90IntervalNs = percentile( m_interval, n, 90 );
		w.p90BusyNs = percentile( m_busy, n, 90 );
		w.p90WorkNs = percentile( m_work, n, 90 );
		w.meanBusyNs = m_sumBusy / (int64_t)m_frames;
		w.meanDrawCpuNs = m_sumDrawCpu / (int64_t)m_frames;
		w.meanPresNs = m_sumPres / (int64_t)m_frames;
		const int64_t T = targetNs();
		w.hitch = m_maxInterval > (int64_t)m_cfg.hitchMs * 1000000;
		w.over = w.p90BusyNs * 100 > T * m_cfg.overPct && w.p90IntervalNs * 100 > T * m_cfg.overPct;
		// Headroom. Busy time is the plain reading. But when Present supplies the pacing (vsync on the swapchain), the display wait sits
		// inside busy and busy never drops under the target however cheap drawing gets: so while the target is
		// held, the work outside Present counts too. That cannot tell a vsync wait from a GPU-bound one; the raise is a probe either way,
		// and a raise that puts the frame back over is undone at once and waits longer next time.
		w.under = w.p90BusyNs * 100 < T * m_cfg.underPct
			|| ( w.p90IntervalNs * 100 <= T * m_cfg.holdPct && w.p90WorkNs * 100 < T * m_cfg.underPct );
		if( w.over )
			w.cls = ( w.meanPresNs * 100 >= w.meanBusyNs * m_cfg.gpuPresPct ) ? GXGOV_GPU : GXGOV_CPU;

		// A raise that survived the probe window and then held for a minute forgets its backoff.
		for( int k = 0; k < GXK_N; ++k )
			if( m_raisedOkSinceNs[ k ] != 0 && nowNs - m_raisedOkSinceNs[ k ] >= 60000000000LL )
			{
				m_backoffMs[ k ] = 0;
				m_raisedOkSinceNs[ k ] = 0;
			}
		if( m_probeKnob >= 0 && nowNs - m_probeNs > (int64_t)m_cfg.probeMs * 1000000 )
		{
			m_raisedOkSinceNs[ m_probeKnob ] = nowNs;
			m_probeKnob = -1;
		}

		// One hitch window (a shader compile, an autosave) decides nothing; a run of them is a real slowdown.
		if( w.hitch )
		{
			if( ++m_hitchStreak < 3 )
			{
				m_underStreak = 0;
				return;
			}
		}
		else
			m_hitchStreak = 0;

		if( w.over )
		{
			m_underStreak = 0;
			++m_overStreak;
			// The knob just raised put us straight back over: undo it at once and make it wait longer next time.
			if( m_probeKnob >= 0 )
			{
				const int k = m_probeKnob;
				m_probeKnob = -1;
				if( m_step[ k ] + 1 < kGxGovKnobs[ k ].steps )
				{
					m_backoffMs[ k ] = m_backoffMs[ k ] == 0 ? m_cfg.backoffMs : std::min( m_backoffMs[ k ] * 2, m_cfg.backoffMaxMs );
					m_raiseAllowedNs[ k ] = nowNs + (int64_t)m_backoffMs[ k ] * 1000000;
					lower( k, nowNs, GXGOV_EV_UNDO );
					m_overStreak = 0;
					return;
				}
			}
			if( m_overStreak >= m_cfg.lowerWindows && nowNs - m_lastChangeNs >= (int64_t)m_cfg.holdLowerMs * 1000000 )
			{
				const int k = pickLower( w.cls );
				if( k >= 0 )
				{
					lower( k, nowNs, GXGOV_EV_LOWER );
					m_overStreak = 0;
					m_floorLogged = false;
				}
				else if( !m_floorLogged )
				{
					m_change.ev = GXGOV_EV_FLOOR;
					m_floorLogged = true;
				}
			}
			return;
		}
		m_overStreak = 0;
		if( !w.under )
		{
			m_underStreak = 0;
			return;
		}
		++m_underStreak;
		if( m_stackN == 0 || m_underStreak < m_cfg.raiseWindows || nowNs - m_lastChangeNs < (int64_t)m_cfg.holdRaiseMs * 1000000 )
			return;
		const int k = m_stack[ m_stackN - 1 ];
		if( nowNs < m_raiseAllowedNs[ k ] )
			return;
		--m_stackN;
		m_change.ev = GXGOV_EV_RAISE;
		m_change.knob = k;
		m_change.from = m_step[ k ];
		m_change.to = --m_step[ k ];
		m_lastChangeNs = nowNs;
		m_underStreak = 0;
		m_probeKnob = k;
		m_probeNs = nowNs;
		m_floorLogged = false;
	}

	GxGovConfig m_cfg;
	int      m_step[ GXK_N ];
	bool     m_avail[ GXK_N ];
	int      m_stack[ STACK_MAX ];
	int      m_stackN;
	int      m_backoffMs[ GXK_N ];
	int64_t  m_raiseAllowedNs[ GXK_N ];
	int64_t  m_raisedOkSinceNs[ GXK_N ];
	int64_t  m_lastChangeNs;
	int      m_overStreak, m_underStreak, m_hitchStreak;
	int      m_probeKnob;
	int64_t  m_probeNs;
	bool     m_floorLogged;
	uint32_t m_windows;
	// window in progress
	int64_t  m_windowStartNs;
	int64_t  m_interval[ MAX_WINDOW_FRAMES ];
	int64_t  m_busy[ MAX_WINDOW_FRAMES ];
	int64_t  m_work[ MAX_WINDOW_FRAMES ];
	uint32_t m_n, m_frames;
	int64_t  m_sumBusy, m_sumDrawCpu, m_sumPres, m_maxInterval;
	GxGovWindowStats m_last;
	GxGovChange m_change;
};

// GX_GOVERNOR_FORCE="shadow:2,part:1,heat:1,cloud:1,fxlight:2,ground:1" - fixed steps for proofs (pictures, replay CRC); the controller stays off.
// Returns the number of knobs set; unknown names and out-of-range steps are refused (return -1) so a typo is never a silent pass.
inline int gxGovParseForce( const char *spec, int outStep[ GXK_N ] )
{
	for( int k = 0; k < GXK_N; ++k )
		outStep[ k ] = 0;
	if( spec == nullptr || *spec == '\0' )
		return 0;
	int set = 0;
	const char *p = spec;
	while( *p )
	{
		const char *colon = strchr( p, ':' );
		if( colon == nullptr )
			return -1;
		const size_t len = (size_t)( colon - p );
		int knob = -1;
		for( int k = 0; k < GXK_N; ++k )
			if( strlen( kGxGovKnobs[ k ].name ) == len && strncmp( kGxGovKnobs[ k ].name, p, len ) == 0 )
				knob = k;
		if( knob < 0 )
			return -1;
		char *end = nullptr;
		const long v = strtol( colon + 1, &end, 10 );
		if( end == colon + 1 || v < 0 || v >= kGxGovKnobs[ knob ].steps )
			return -1;
		outStep[ knob ] = (int)v;
		++set;
		p = end;
		if( *p == ',' )
			++p;
		else if( *p != '\0' )
			return -1;
	}
	return set;
}

// "shadow=1 part=0 heat=0 cloud=0" into buf.
inline void gxGovFormatSteps( const int step[ GXK_N ], char *buf, size_t cap )
{
	size_t used = 0;
	if( cap > 0 )
		buf[ 0 ] = '\0';
	for( int k = 0; k < GXK_N && used < cap; ++k )
	{
		const int w = snprintf( buf + used, cap - used, "%s%s=%d", k ? " " : "", kGxGovKnobs[ k ].name, step[ k ] );
		if( w < 0 )
			break;
		used += (size_t)w;
	}
}
