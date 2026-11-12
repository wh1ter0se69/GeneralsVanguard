// A SHIPPED BUILD READS ONLY THE GX_* VARIABLES ON THIS LIST
//
// The engine has ~590 GX_* switches: A/B probes, traces, debug views, test hooks. Gating them one at a time does not converge (every
// pass found more), and several live in digested code that cannot change without moving the digest.
// This is the other way round, and it is fail-closed: main() scrubs the environment first, so every GX_* NOT named here is simply absent
// when the engine's lazy getenv() runs. A new probe is off in a shipped build until someone adds it here on purpose.
//
//   * GX_TEST_INSTRUMENTS builds keep everything (they never ship; A/B arms that need a probe off this list build with it).
//   * Only names starting GX_. GENERALSX_* and DXVK_* are not touched.
//   * Runs at the top of main() / WinMain(), in this non-digested header. Nearly every env read is lazy, after main; the few at static initialization that are not
//     listed are gated by GX_TEST_INSTRUMENTS at the read (dx8renderer.cpp), and gx-env-allowlist-check.py fails on a new one, so nothing digested changes.
//   * Says so once on stderr when it dropped anything: [GXENVDROP]. Names only, never values. A harness that silently measures something
//     else is worse than one that fails: scripts/test/lib-gxverdict.sh gxv_probe_locks turns the line into a FAIL.
//
// THE LIST is what scripts/test/ and the standing host actually set on release builds (scripts/test/gx-env-allowlist-check.py keeps that true
// and fails on a name set but not listed), minus what may never ship:
//   * GX_PATHTRACE ([GXPATH] + [GXLINEMEMO]), GX_TRACE_LO/HI, GX_DRAWDBG, GX_VICTORY_PROBE: off the list on purpose
//     (each prints or changes state a player must not reach). Their harnesses build with GX_TEST_INSTRUMENTS.
//   * Render and look knobs (GX_WATER_*, GX_SHADOWMAP*, GX_RENDER_*...) are off: same treatment as GX_NOSHROUD since 0.9.72, an arm that needs
//     one is a TI build.
//   * The AI gym and trainer channels (GX_OBS*, GX_ACT*, GX_GYM_*, GX_SKIRMISH, GX_STARTS) are off: an outside process driving or watching a
//     shipped game is a bot surface. Those runs build with GX_TEST_INSTRUMENTS, and their launchers refuse an engine that drops one.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <crt_externs.h>
#else
#include <unistd.h>
#endif

#ifdef GX_TEST_INSTRUMENTS
inline bool gxEnvAllowed(const char *) { return true; }
inline void gxScrubEnvironment() {}
#else
// Sorted by hand into groups; gx-env-allowlist-check.py compares the whole set.
inline const char *const *gxEnvAllowListTable(size_t &count)
{
	static const char *const kAllowed[] = {
		// the standing host (starthost.sh) and every harness run
		"GX_NET", "GX_CRC_EVERY_FRAME", "GX_IDENTITY_FILE", "GX_ACTIVITY", "GX_PACE", "GX_PACE_HEADLESS", "GX_TICKMS", "GX_USERDATA_DIR", "GX_CPUSETS",
		"GX_HEADLESS_LINGER_MS", "GX_STDERR_LOG", "GX_STDERR_PUMP", "GX_STDERR_STATS", "GX_DRAW", "GX_RETRY_MS", "GX_ROUTER_TRACE", "GX_RUNAHEAD_TRACE",
		"GX_PKT_TRACE", "GX_MATCH_RECORD", "GX_MATCH_STATS", "GX_RELAYLIST_BOOTSTRAP", "GX_RELAYLIST_LAUNCHER_CACHE",
		// replay playback
		"GX_REPLAY_XPLAT", "GX_REPLAY_ANY_COMMIT", "GX_REPLAY_SEEK", "GX_REPLAY_SEEK_EXIT", "GX_REPLAY_SEEK_HOLD",
		// match setup and scripted play; GX_CAM and GX_NOSHROUD stay listed so their refusals (the [GXCAMLOCK] / [GXSHROUDLOCK] lines) still fire
		"GX_TEAMS", "GX_HOST_FACTION", "GX_LIMIT_ARMIES", "GX_ORDERS", "GX_ORDERS_MIX", "GX_ORDERS_SEED", "GX_ORDERS_FROM", "GX_CAM", "GX_NOSHROUD",
		"GX_PIPS_ALL", "GX_SHOT", "GX_LOBBY_TEST", "GX_HP_TEMPLATE", "GX_AI_COLOURS", "GX_GROUPING_DIV", "GX_SEAT_MUTANT", "GX_L2_MUTANT", "GX_ALLOC_FOREIGN",
		"GX_CONTACT_SKIP_AB", "GX_CATCHUP", "GX_FAST_FORWARD", "GX_PARALLEL_WORKERS", "GX_POSEPUB",
		// the battlefield stress harness
		"GX_STRESS", "GX_STRESS_BUILDINGS", "GX_STRESS_BUILDING_TYPES", "GX_STRESS_EVERY", "GX_STRESS_FRAME", "GX_STRESS_ORDER", "GX_STRESS_PLACE", "GX_STRESS_RADIUS",
		"GX_STRESS_UNIT",
		// the test hooks that are not GX_TEST_INSTRUMENTS-only
		"GX_TEST_DROP_STARTS", "GX_TEST_FIRESW", "GX_TEST_LOAD_STALL_MS", "GX_TEST_MATCH_SAY", "GX_TEST_QUIT_AT", "GX_TEST_SAVE_AT_FRAME", "GX_TEST_SAVE_UNGATED",
		"GX_TEST_SAVE_WARNING_ANSWER", "GX_TEST_SELECT",
		// read by harness tooling, not by the engine: the port-isolation shim (scripts/test/gx-loopshim.c, on every socket call) and the tag lib-gxpeer finds its processes by
		"GX_SHIM_BIND_SHIFT", "GX_SHIM_RELAY_BASE", "GX_SHIM_DELAY_MS", "GX_BOT_TAG",
		// settings the harness picture and input runs set, and the Windows DXVK seed
		"GX_GFX", "GX_GFX_LOAD", "GX_GROUND", "GX_RENDERER", "GX_RSPLIT", "GX_FONT", "GX_UI", "GX_UI_SCALE", "GX_KEYS_TRACE", "GX_KEYTRACE", "GX_MAPDBG", "GX_DXVK_SEED_DIR",
	};
	count = sizeof(kAllowed) / sizeof(kAllowed[0]);
	return kAllowed;
}

inline bool gxEnvNameEquals(const char *a, const char *b)
{
#if defined(_WIN32)
	return _stricmp(a, b) == 0;		// environment names are case-insensitive on Windows, so getenv("GX_X") finds gx_x
#else
	return strcmp(a, b) == 0;
#endif
}

inline bool gxEnvAllowed(const char *name)
{
	size_t n = 0;
	const char *const *t = gxEnvAllowListTable(n);
	for (size_t i = 0; i < n; ++i) {
		if (gxEnvNameEquals(name, t[i])) {
			return true;
		}
	}
	return false;
}

inline void gxScrubEnvironment()
{
	std::vector<std::string> dropped;
#if defined(_WIN32)
	char *block = GetEnvironmentStringsA();		// the process block, not _environ: the CRT copy is not built yet in every path through WinMain
	if (block == nullptr) {
		return;
	}
	for (const char *p = block; *p != '\0'; p += strlen(p) + 1) {
		const char *eq = strchr(p, '=');
		if (eq != nullptr && eq != p && _strnicmp(p, "GX_", 3) == 0) {
			std::string name(p, static_cast<size_t>(eq - p));
			if (!gxEnvAllowed(name.c_str())) {
				dropped.push_back(name);
			}
		}
	}
	FreeEnvironmentStringsA(block);
#else
#if defined(__APPLE__)
	char **env = *_NSGetEnviron();
#else
	extern char **environ;
	char **env = environ;
#endif
	for (char **e = env; e != nullptr && *e != nullptr; ++e) {
		const char *eq = strchr(*e, '=');
		if (eq != nullptr && strncmp(*e, "GX_", 3) == 0) {
			std::string name(*e, static_cast<size_t>(eq - *e));
			if (!gxEnvAllowed(name.c_str())) {
				dropped.push_back(name);
			}
		}
	}
#endif
	if (dropped.empty()) {
		return;
	}
	std::string names;
	for (size_t i = 0; i < dropped.size(); ++i) {
#if defined(_WIN32)
		_putenv_s(dropped[i].c_str(), "");		// the CRT copy
		SetEnvironmentVariableA(dropped[i].c_str(), nullptr);	// the process block
#else
		unsetenv(dropped[i].c_str());
#endif
		names += ' ';
		names += dropped[i];
	}
	fprintf(stderr, "[GXENVDROP] ignored %d GX_* variable(s) not on the shipped allow-list (an arm that needs one builds with -DGX_TEST_INSTRUMENTS=ON):%s\n",
		static_cast<int>(dropped.size()), names.c_str());
}
#endif
