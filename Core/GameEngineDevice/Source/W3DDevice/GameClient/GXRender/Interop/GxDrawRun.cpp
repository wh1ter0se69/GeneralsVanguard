/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
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

// GeneralsX @feature Claude 11/10/2026 WS-X2 - see W3DDevice/GameClient/GxDrawRun.h and render-seam/DESIGN_hybrid.md §13.
//
// Threads: Available / Draw_Run run on the main thread and only queue work; DX8Wrapper::Device_Call runs Run_On_Device on the
// thread that owns the d3d8 device at that point of the stream (the d3d8 ring's consumer inside the object-flush bracket, the
// main thread otherwise). A run's bytes are written once by the main thread into this frame's arena and then only read.
#include "W3DDevice/GameClient/GxDrawRun.h"
#include "W3DDevice/GameClient/GxTestEnv.h"

#include "GxVulkan.h"
#include "WW3D2/dx8renderer.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/ww3d.h"
#include "GameLogic/GameLogic.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#if GX_HAVE_VULKAN
#include "GxDxvkExternal.h"
static_assert(sizeof(GxDrawRun::Task) == sizeof(DxvkGxTask) && offsetof(GxDrawRun::Task, primCount) == offsetof(DxvkGxTask, primCount)
	&& offsetof(GxDrawRun::Task, baseVertex) == offsetof(DxvkGxTask, baseVertex), "GxDrawRun::Task must be the fork's DxvkGxTask");
static_assert(sizeof(GxDrawRun::LightSet) == sizeof(DxvkGxLightSet) && offsetof(GxDrawRun::LightSet, lights) == offsetof(DxvkGxLightSet, lights),
	"GxDrawRun::LightSet must be the fork's DxvkGxLightSet");
#endif

namespace GxDrawRun
{
namespace
{

const unsigned DXVK_GX_RUN_VALIDATE_ONLY_GAME = 0x8000u;	// = the fork's DXVK_GX_RUN_VALIDATE_ONLY (asserted below)
#if GX_HAVE_VULKAN
static_assert(DXVK_GX_RUN_VALIDATE_ONLY_GAME == DXVK_GX_RUN_VALIDATE_ONLY, "VALIDATE_ONLY differs from the fork's");
#endif

enum { INIT_NONE, INIT_QUEUED, INIT_READY, INIT_FAILED };
std::atomic<int> s_init(INIT_NONE);
UINT s_flags = 0;				// DXVK_GX_RUN_* for every call (GX_DRAWRUN_FAST / _CHECK / _MUTANT), parsed before the init is queued
bool s_mutantRefuse = false;	// GX_DRAWRUN_MUTANT=refuse: every call fails the fork's validation - the fallback must draw the same picture
int s_mutantRun0 = 0;			// GX_DRAWRUN_MUTANT=zbias|stencilref: C7's depth / stencil fail arms - the frame's FIRST run draws with
								// D3DRS_ZBIAS 4 / STENCILREF + 1 (restored right after): arm B's depth / stencil readback must differ
bool s_digest = false;			// GX_DRAWRUN_DIGEST=1: Draw_Run brackets every run for the fork's C2b input digest
int s_test = 0;					// GX_HYBRID_DRAWRUN_TEST
bool s_testSetupOk = false;		// device thread: this frame's Test_Setup succeeded (written and read on the device thread only)

// the gate's in-run toggle - see Tog_Account below
struct Moments { double n, sum, sumsq; void add(double v) { n += 1; sum += v; sumsq += v * v; } };
int s_togK = 0, s_togDrop = 3, s_togWhat = 0;
bool s_togNull = false;
unsigned s_togFrame = ~0u, s_togPeriods = 0, s_winLogic0 = 0, s_winFrame0 = 0;
unsigned long long s_togT0 = 0, s_gaps = 0, s_dropped = 0, s_lastOffPeriod = ~0ull;
double s_lastOffMean = 0.0;
unsigned s_lastOffLogic = 0;	// the logic frame when the pair's OFF period closed (drt.py: both halves inside the frozen block)
Moments s_period, s_on, s_off, s_pairs;
unsigned long long s_keptOn = 0, s_keptOff = 0;
// per window, main thread: what was queued in each state
unsigned long long s_wRunsOn = 0, s_wRunsOff = 0, s_wTasksOn = 0, s_wTasksOff = 0, s_wVOn = 0, s_wVOff = 0, s_wFramesOn = 0;
bool s_rowPending = false;
char s_rowHead[640];
unsigned long long s_rowRunsOn, s_rowRunsOff, s_rowTasksOn, s_rowTasksOff, s_rowVOn, s_rowVOff, s_rowFramesOn;
unsigned long long s_pubFbOn = 0, s_pubFbOff = 0, s_pubVRef = 0, s_pubNsOn = 0, s_pubNsOff = 0;
unsigned long long Now_Ns();

#if GX_HAVE_VULKAN
IDirect3DDevice8 *s_dev8 = nullptr;			// the device the extension came from (not AddRef'd: DX8Wrapper owns it)
GxIDxvkGxExternal *s_ext = nullptr;			// not AddRef'd: a member of DXVK's d3d9 device, alive as long as s_dev8
#endif

// Main thread: counters of what was asked; device thread: what ran how.
int s_mutantTruth = 0;
unsigned long long s_runs = 0, s_tasks = 0, s_badSets = 0;	// s_badSets: tasks naming a light set the run lacks (a seat bug; review r14 P3-6)
std::atomic<unsigned long long> s_devRuns(0), s_devTasks(0), s_fbRuns(0), s_fbTasks(0);
std::mutex s_statsLock;
unsigned long long s_fork[8] = {};			// the fork's GetRunStats, refreshed by the device thread
unsigned s_lastPrint = 0;

// Two per-frame arenas used alternately (DESIGN_hybrid §13.4): a run's bytes are overwritten one whole frame after the bracket that
// consumed them ended (End_Record drains the ring; outside the bracket Device_Call runs inline).
struct Arena
{
	std::vector<std::unique_ptr<unsigned char[]> > blocks;
	std::vector<size_t> sizes;
	size_t block = 0, used = 0;
	void Reset() { block = 0; used = 0; }
	void *Take(size_t bytes)
	{
		bytes = (bytes + 15) & ~(size_t)15;
		while (block < blocks.size() && used + bytes > sizes[block])
		{
			++block;
			used = 0;
		}
		if (block == blocks.size())
		{
			const size_t size = bytes > (size_t)262144 ? bytes : (size_t)262144;
			blocks.push_back(std::unique_ptr<unsigned char[]>(new unsigned char[size]));
			sizes.push_back(size);
			used = 0;
		}
		void *p = blocks[block].get() + used;
		used += bytes;
		return p;
	}
};
Arena s_arena[2];
unsigned long long s_arenaFrame = ~0ull;
unsigned s_runInFrame = 0;		// the next run's index in this frame (the digest's tag)

void *Arena_Take(size_t bytes)
{
	const unsigned long long pc = DX8Wrapper::Get_Present_Count();
	if (pc != s_arenaFrame)
	{
		s_arenaFrame = pc;
		s_arena[pc & 1u].Reset();
		s_runInFrame = 0;
	}
	return s_arena[pc & 1u].Take(bytes);
}

struct RunHeader
{
	D3DPRIMITIVETYPE type;
	unsigned taskCount;
	unsigned setCount;
	unsigned runIndex;	// in its frame: the digest tag's low half
	unsigned flags;		// DXVK_GX_RUN_VALIDATE_ONLY for the (a) toggle's added calls
	unsigned onPeriod;	// the toggle state the main thread queued it in (device-side counters per state)
	// then Task[taskCount], LightSet[setCount]
};

// The fallback (DESIGN_hybrid §13.4) and the test's =1 arm: the same per-task calls through the d3d8 device, in the classic per-task
// order. The d3d8 base vertex is restored at the end, as DrawRun leaves it untouched.
void Issue_Classic(IDirect3DDevice8 *dev, D3DPRIMITIVETYPE type, const Task *tasks, unsigned n, const LightSet *sets, unsigned m)
{
	IDirect3DIndexBuffer8 *ib = nullptr;
	UINT base0 = 0;
	if (FAILED(dev->GetIndices(&ib, &base0)))
		base0 = 0;
	UINT base = base0;
	unsigned last = ~0u;
	for (unsigned i = 0; i < n; ++i)
	{
		const Task &t = tasks[i];
		if (t.lightSet != last && t.lightSet < m)
		{
			const LightSet &s = sets[t.lightSet];
			for (DWORD l = 0; l < 4; ++l)
			{
				if (s.enableMask & (1u << l))
					dev->SetLight(l, &s.lights[l]);	// GX-WRAPSKIP-OK: seat run: the seat's CARRY-OUT re-syncs the wrapper after it (GX_DRAWRUN_TRUTH checks)
				dev->LightEnable(l, (s.enableMask >> l) & 1u);	// GX-WRAPSKIP-OK: seat run: the seat's CARRY-OUT re-syncs the wrapper after it (GX_DRAWRUN_TRUTH checks)
			}
			last = t.lightSet;
		}
		dev->SetRenderState(D3DRS_AMBIENT, t.ambient);
		dev->SetTransform(D3DTS_WORLD, &t.world);	// GX-WRAPSKIP-OK: seat run: the seat's CARRY-OUT re-syncs the wrapper after it (GX_DRAWRUN_TRUTH checks)
		if (t.normalizeNormals)
			dev->SetRenderState(D3DRS_NORMALIZENORMALS, TRUE);
		if ((UINT)t.baseVertex != base)
		{
			dev->SetIndices(ib, (UINT)t.baseVertex);
			base = (UINT)t.baseVertex;
		}
		dev->DrawIndexedPrimitive(type, t.minIndex, t.numVertices, t.startIndex, t.primCount);
		// [GXGFX] dc=: counted by the caller - the S1 seat calls DX8Wrapper::Record_Direct_Draw per task at submission (DrawRun and this fallback alike)
		if (t.normalizeNormals)
			dev->SetRenderState(D3DRS_NORMALIZENORMALS, FALSE);
	}
	if (base != base0)
		dev->SetIndices(ib, base0);
	if (ib != nullptr)
		ib->Release();
}

void Parse_Options()
{
	const char *t = GxTestEnv("GX_HYBRID_DRAWRUN_TEST");
	s_test = t != nullptr ? atoi(t) : 0;
	if (s_test < 1 || s_test > 3)
		s_test = 0;
	const char *f = getenv("GX_DRAWRUN_FAST");
	const char *c = GxTestEnv("GX_DRAWRUN_CHECK");
	const char *mu = GxTestEnv("GX_DRAWRUN_MUTANT");
	s_flags = 0;
#if GX_HAVE_VULKAN
	if (f != nullptr && atoi(f) != 0)
		s_flags |= DXVK_GX_RUN_FAST;
	if (c != nullptr && atoi(c) != 0)
		s_flags |= DXVK_GX_RUN_CHECK;
	if (mu != nullptr && strcmp(mu, "nokey") == 0)
		s_flags |= DXVK_GX_RUN_MUTANT_NOKEY;
	if (mu != nullptr && strcmp(mu, "noamb") == 0)
		s_flags |= DXVK_GX_RUN_MUTANT_NOAMB;
#else
	(void)f;
	(void)c;
#endif
	s_mutantRefuse = mu != nullptr && strcmp(mu, "refuse") == 0;
	// the synthetic test's Check_Truth fail arms: the wrapper belief of a seat that skipped its carry-out (PREPARE's = the first
	// task's values) / that moved its base vertex without the setter (the last task's base vertex, never sent to the device)
	s_mutantTruth = mu != nullptr && strcmp(mu, "nocarry") == 0 ? 1 : mu != nullptr && strcmp(mu, "nocarryib") == 0 ? 2
		: mu != nullptr && strcmp(mu, "wrongtask") == 0 ? 3 : mu != nullptr && strcmp(mu, "nntrue") == 0 ? 4 : 0;
	s_mutantRun0 = mu != nullptr && strcmp(mu, "zbias") == 0 ? 1 : mu != nullptr && strcmp(mu, "stencilref") == 0 ? 2 : 0;
	const char *tk = GxTestEnv("GX_DRAWRUN_TOGGLE");
	const char *td = GxTestEnv("GX_DRAWRUN_TOGGLE_DROP");
	const char *tn = GxTestEnv("GX_DRAWRUN_TOGGLE_NULL");
	const char *tw = GxTestEnv("GX_DRAWRUN_TOGGLE_WHAT");
	s_togK = tk != nullptr ? atoi(tk) : 0;
	s_togK = s_togK < 0 ? 0 : s_togK;
	s_togDrop = td != nullptr ? atoi(td) : 3;
	s_togNull = tn != nullptr && atoi(tn) != 0;
	s_togWhat = tw != nullptr && strcmp(tw, "overhead") == 0 ? 1 : 0;

	const char *dg = GxTestEnv("GX_DRAWRUN_DIGEST");
	// =1: Draw_Run brackets each run itself (the synthetic test); =2: the S1 seat brackets each run's OWN draws in both toggle states
	// (open before PREPARE, closed after the carry-out's Apply / the classic last task's epilogue; never nested, never early - Codex
	// rw1x-design-r17 #2/#4) and Draw_Run does not bracket. The carry-out is Check_Truth's (the oracle), not the digest's.
	s_digest = s_test != 0 || (dg != nullptr && atoi(dg) == 1);
}

#if GX_HAVE_VULKAN
// Device thread: the extension of THIS device (a re-created device answers anew).
GxIDxvkGxExternal *Ext_For(IDirect3DDevice8 *dev)
{
	if (dev != s_dev8)
	{
		s_dev8 = dev;
		s_ext = nullptr;
		GxIDxvkGxExternal *ext = nullptr;
		if (SUCCEEDED(dev->QueryInterface(GX_IID_IDxvkGxExternal, (void **)&ext)) && ext != nullptr)
		{
			s_ext = ext;
			ext->Release();	// a member of DXVK's d3d9 device: it lives exactly as long as dev, which DX8Wrapper holds
		}
	}
	return s_ext;
}
#endif

// DX8Wrapper::Pre_Release_Hook (main thread, ring drained, before the device is released): the cached device and extension
// pointers die with it - a device re-created at the same address must be asked again (Codex rw1x-design-r13 #3). Chained: the
// hook it replaced (GxInterop's, the test's) still runs.
void (*s_prevRelease)() = nullptr;
bool s_releaseChained = false;
void Print_Stats(unsigned frame, bool final = false);

#if GX_HAVE_VULKAN
// The fork's own counters into s_fork: from the device thread every 256 runs, and once more for the final record
void Refresh_Fork_Stats(GxIDxvkGxExternal *ext)
{
	DxvkGxRunStats st;
	if (FAILED(ext->GetRunStats(&st)))
		return;
	std::lock_guard<std::mutex> g(s_statsLock);
	const unsigned long long v[8] = { st.calls, st.refused, st.tasks, st.lightSetsApplied, st.fastTasks, st.checkedTasks,
		st.checkFailed, st.fastKeyTasks };
	memcpy(s_fork, v, sizeof(v));
}
#endif

void Release_Device_State()
{
	// the ring is drained here, so every queued run and Truth check has executed: the FINAL counters, marked final=1 - an analyzer
	// judges these, never a periodic snapshot a later mismatch could postdate (Codex rw1x-design-r17 #3). The fork's counters are
	// re-read now (the device thread refreshes them only every 256 runs), and the record is printed for every armed mode, the
	// classic-only test arm and a Truth-only run included (Codex seat-c9 on bd4d71d9f)
	if (s_init.load() == INIT_READY || s_test != 0 || Truth_On())
	{
#if GX_HAVE_VULKAN
		if (s_ext != nullptr)
			Refresh_Fork_Stats(s_ext);
#endif
		Print_Stats(WW3D::Get_Frame_Count(), true);
	}
#if GX_HAVE_VULKAN
	s_dev8 = nullptr;
	s_ext = nullptr;
#endif
	s_init.store(INIT_NONE);
	if (s_prevRelease != nullptr)
		s_prevRelease();
}

void Chain_Release_Hook()
{
	if (s_releaseChained)
		return;
	s_releaseChained = true;
	s_prevRelease = DX8Wrapper::Pre_Release_Hook;
	DX8Wrapper::Pre_Release_Hook = &Release_Device_State;
}

void Device_Init(IDirect3DDevice8 *dev, void *)
{
#if GX_HAVE_VULKAN
	const bool ok = Ext_For(dev) != nullptr;
#else
	const bool ok = false;
	(void)dev;
#endif
	fprintf(stderr, "[GXDRUN] init %s flags=0x%x fast=%d check=%d mutant=%s test=%d\n",
		ok ? "ready: the fork answers IDxvkGxExternal 703eff94 (DrawRun)"
			: "unavailable: no DrawRun on this device (an unpatched DXVK, the d3d8 batcher on, or built without DXVK's headers) - classic",
		s_flags, (s_flags & 1u) ? 1 : 0, (s_flags & 2u) ? 1 : 0,
		(s_flags & 0x100u) ? "nokey" : (s_flags & 0x200u) ? "noamb" : s_mutantRefuse ? "refuse" : s_mutantRun0 == 1 ? "zbias"
			: s_mutantRun0 == 2 ? "stencilref" : "none", s_test);
	s_init.store(ok ? INIT_READY : INIT_FAILED, std::memory_order_release);
}

// Device thread: the digest bracket (the fork prints the CLOSE's tag; a no-op unless DXVK_GX_INPUT_DIGEST is armed).
void Mark_On_Device(IDirect3DDevice8 *dev, bool begin, unsigned long long tag)
{
#if GX_HAVE_VULKAN
	if (GxIDxvkGxExternal *ext = Ext_For(dev))
		ext->DigestMark(begin ? 1u : 0u, tag);
#else
	(void)dev; (void)begin; (void)tag;
#endif
}

struct MarkPacket { unsigned long long tag; int begin; };
void Mark_Call(IDirect3DDevice8 *dev, void *ctx)
{
	const MarkPacket *m = (const MarkPacket *)ctx;
	Mark_On_Device(dev, m->begin != 0, m->tag);
}

// GX_DRAWRUN_TRUTH (Check_Truth): the wrapper's belief vs the d3d8 device's answers, on the device thread in stream order
std::atomic<unsigned long long> s_truthChecked[2], s_truthMismatch[2];	// [0] OFF (classic, tag path 0), [1] ON (S1)
std::atomic<unsigned long long> s_truthUncreated[2];	// light slots d3d9 had never created (GetLightEnable D3DERR_INVALIDCALL), read as disabled
void Truth_Call(IDirect3DDevice8 *dev, void *ctx)
{
	const Truth &t = *(const Truth *)ctx;
	const char *bad = nullptr;
	IDirect3DIndexBuffer8 *ib = nullptr;
	UINT base = 0;
	D3DMATRIX w;
	DWORD amb = 0, nn = 0;
	BOOL on[4] = {};
	D3DLIGHT8 li[4];
	memset(li, 0, sizeof(li));
	// the device's executed state (d3d8 keeps no copy of world / lights / render states: these are d3d9's values); each Get* names
	// itself and its HRESULT when it fails (seat smoke S33/M33, 11/10)
	static char failed[96];
	HRESULT hr = S_OK;
	if (FAILED(hr = dev->GetIndices(&ib, &base)))
		bad = "GetIndices";
	else if (FAILED(hr = dev->GetTransform(D3DTS_WORLD, &w)))
		bad = "GetTransform(WORLD)";
	else if (FAILED(hr = dev->GetRenderState(D3DRS_AMBIENT, &amb)))
		bad = "GetRenderState(AMBIENT)";
	else if (FAILED(hr = dev->GetRenderState(D3DRS_NORMALIZENORMALS, &nn)))
		bad = "GetRenderState(NORMALIZENORMALS)";
	const int stc = (t.tag >> 32) != 0 ? 1 : 0;
	for (DWORD l = 0; bad == nullptr && l < 4; ++l)
	{
		// d3d9 answers D3DERR_INVALIDCALL for a light index never created (d3d9_device.cpp GetLightEnable / GetLight: the slot is
		// empty), and LightEnable creates a default light - so a never-created slot cannot be enabled: it reads as disabled
		hr = dev->GetLightEnable(l, &on[l]);
		if (hr == D3DERR_INVALIDCALL)
		{
			on[l] = FALSE;
			++s_truthUncreated[stc];	// counted, so a run shows how often the rule applied (the seat's G3 OFF A/A: every check before the first DrawRun)
		}
		else if (FAILED(hr))
			bad = "GetLightEnable";
		else if (on[l] && FAILED(hr = dev->GetLight(l, &li[l])))
			bad = "GetLight";
	}
	if (bad != nullptr)
	{
		snprintf(failed, sizeof(failed), "device %s failed (hr=0x%08lx)", bad, (unsigned long)hr);
		bad = failed;
	}
	if (ib != nullptr)
		ib->Release();	// only its identity is compared
	UINT mask = 0;
	for (DWORD l = 0; l < 4; ++l)
		mask |= on[l] ? (1u << l) : 0u;
	auto lightsDiffer = [&](UINT m, const D3DLIGHT8 *ls) {
		if ((m & 15u) != mask)
			return true;
		for (DWORD l = 0; l < 4; ++l)
			if (on[l] && memcmp(&li[l], &ls[l], sizeof(D3DLIGHT8)) != 0)
				return true;
		return false;
	};
	// coherence: the wrapper's belief == the device (an index buffer of null included)
	if (bad == nullptr && (ib != t.ib || base != t.baseVertex))
		bad = "wrapper vs device: index buffer / base vertex";
	if (bad == nullptr && memcmp(&w, &t.world, sizeof(w)) != 0)
		bad = "wrapper vs device: world";
	if (bad == nullptr && amb != t.ambient)
		bad = "wrapper vs device: ambient";
	if (bad == nullptr && (nn != 0) != (t.normalizeNormals != 0))
		bad = "wrapper vs device: normalizenormals";
	if (bad == nullptr && lightsDiffer(t.lightEnableMask, t.lights))
		bad = "wrapper vs device: lights";
	if (bad == nullptr && t.wrapperDirty != 0)
		bad = "wrapper has pending changes after the Apply";
	// the oracle: the classic final state of the run, from the claim (Codex rw1x-design-r16 #1)
	if (bad == nullptr && t.hasOracle)
	{
		if (ib != t.runIb || base != (UINT)t.last.baseVertex)
			bad = "device vs classic final state: index buffer / base vertex";
		else if (memcmp(&w, &t.last.world, sizeof(w)) != 0)
			bad = "device vs classic final state: world";
		else if (amb != t.last.ambient)
			bad = "device vs classic final state: ambient";
		else if (nn != 0)
			bad = "device vs classic final state: normalizenormals (classic leaves FALSE after every admitted run - Codex r17 #1)";
		else if (lightsDiffer(t.lastSet.enableMask, t.lastSet.lights))
			bad = "device vs classic final state: lights";
	}
	const int st = (t.tag >> 32) != 0 ? 1 : 0;
	++s_truthChecked[st];
	if (bad != nullptr && s_truthMismatch[st]++ < 20)
		fprintf(stderr, "[GXDTRUTH] f=%u tag=%llx oracle=%u mismatch: %s (wrapper base %u, device base %u)\n", WW3D::Get_Frame_Count(),
			t.tag, t.hasOracle, bad, t.baseVertex, base);
}

std::atomic<unsigned long long> s_devFbOn(0), s_devFbOff(0), s_devVRefused(0);
// the device counters as of a toggle row's boundary: snapshotted by a Device_Call queued when the row closes on the main thread, so
// they cover exactly the work queued before that boundary (Codex rw1x2-code-r1 #2)
std::atomic<unsigned long long> s_snapFbOn(0), s_snapFbOff(0), s_snapVRef(0), s_snapNsOn(0), s_snapNsOff(0);
std::atomic<bool> s_snapDone(false);
std::atomic<unsigned long long> s_devNsOn(0), s_devNsOff(0);	// device-thread time inside Run_On_Device, per toggle state

void Snap_Device_Counters(IDirect3DDevice8 *, void *)
{
	s_snapFbOn.store(s_devFbOn.load());
	s_snapFbOff.store(s_devFbOff.load());
	s_snapVRef.store(s_devVRefused.load());
	s_snapNsOn.store(s_devNsOn.load());
	s_snapNsOff.store(s_devNsOff.load());
	s_snapDone.store(true, std::memory_order_release);
}

void Run_On_Device_Body(IDirect3DDevice8 *dev, const struct RunHeader *h);

// The device thread's cost of a run (the ring consumer on macOS / Windows, MAIN at PROJ_SHADOW or with the ring off): the (a)
// clause's DXVK-bound projection reads it per call.
void Run_On_Device(IDirect3DDevice8 *dev, void *ctx)
{
	const RunHeader *h = (const RunHeader *)ctx;
	const unsigned long long t0 = s_togK > 0 ? Now_Ns() : 0;
	Run_On_Device_Body(dev, h);
	if (s_togK > 0)
		(h->onPeriod ? s_devNsOn : s_devNsOff) += Now_Ns() - t0;
}

void Run_On_Device_Body(IDirect3DDevice8 *dev, const RunHeader *h)
{
	// test mode: every run is the synthetic test's, and it may draw only on the state its own setup established - a failed setup
	// (buffers, state block) must not let the run draw with the game's bindings and nothing to restore them (Codex rw1x2-code-r2)
	if (s_test != 0 && !s_testSetupOk)
	{
		static bool s_said = false;
		if (!s_said)
		{
			s_said = true;
			fprintf(stderr, "[GXDRUN] synthetic test run skipped: its device setup failed\n");
		}
		return;
	}
	const Task *tasks = (const Task *)(h + 1);
	const LightSet *sets = (const LightSet *)(tasks + h->taskCount);
	if (h->flags & DXVK_GX_RUN_VALIDATE_ONLY_GAME)
	{
		// the (a) toggle's added call: transport + the fork's entry and validation, nothing drawn, never a fallback
#if GX_HAVE_VULKAN
		GxIDxvkGxExternal *ext = Ext_For(dev);
		if (ext == nullptr || FAILED(ext->DrawRun(h->type, h->taskCount, (const DxvkGxTask *)tasks, h->setCount,
				(const DxvkGxLightSet *)sets, s_flags | DXVK_GX_RUN_VALIDATE_ONLY)))
#endif
			++s_devVRefused;
		return;
	}
	bool drawn = false;
	DWORD mutantOld = 0;
	const D3DRENDERSTATETYPE mutantRs = s_mutantRun0 == 1 ? D3DRS_ZBIAS : D3DRS_STENCILREF;
	if (s_mutantRun0 && h->runIndex == 0 && SUCCEEDED(dev->GetRenderState(mutantRs, &mutantOld)))
		dev->SetRenderState(mutantRs, s_mutantRun0 == 1 ? 4u : mutantOld + 1u);
	else
		mutantOld = ~0u;
	if (s_digest)
		Mark_On_Device(dev, true, 0);
#if GX_HAVE_VULKAN
	if (GxIDxvkGxExternal *ext = Ext_For(dev))
	{
		drawn = SUCCEEDED(ext->DrawRun(h->type, h->taskCount, (const DxvkGxTask *)tasks, s_mutantRefuse ? 0u : h->setCount,
			(const DxvkGxLightSet *)sets, s_flags));
		if ((s_devRuns.load(std::memory_order_relaxed) & 255u) == 0)
			Refresh_Fork_Stats(ext);
	}
#endif
	if (drawn)
	{
		++s_devRuns;
		s_devTasks += h->taskCount;
	}
	else
	{
		Issue_Classic(dev, h->type, tasks, h->taskCount, sets, h->setCount);
		++s_fbRuns;
		s_fbTasks += h->taskCount;
		++(h->onPeriod ? s_devFbOn : s_devFbOff);
	}
	if (s_digest)
		Mark_On_Device(dev, false, ((unsigned long long)(drawn ? 1u : 2u) << 32) | h->runIndex);
	if (mutantOld != ~0u)
		dev->SetRenderState(mutantRs, mutantOld);
}

void Print_Stats(unsigned frame, bool final)
{
	unsigned long long fork[8];
	{
		std::lock_guard<std::mutex> g(s_statsLock);
		memcpy(fork, s_fork, sizeof(fork));
	}
	fprintf(stderr, "[GXDRUN] f=%u runs=%llu tasks=%llu drawrun_runs=%llu drawrun_tasks=%llu fallback_runs=%llu fallback_tasks=%llu"
		" fork_calls=%llu fork_refused=%llu fork_tasks=%llu fork_lightsets=%llu fork_fast=%llu fork_checked=%llu fork_check_failed=%llu fork_fast_key=%llu"
		" bad_sets=%llu truth_checked_off=%llu truth_mismatch_off=%llu truth_checked_on=%llu truth_mismatch_on=%llu"
		" truth_uncreated_off=%llu truth_uncreated_on=%llu%s\n",
		frame, s_runs, s_tasks, s_devRuns.load(), s_devTasks.load(), s_fbRuns.load(), s_fbTasks.load(),
		fork[0], fork[1], fork[2], fork[3], fork[4], fork[5], fork[6], fork[7], s_badSets, s_truthChecked[0].load(), s_truthMismatch[0].load(),
		s_truthChecked[1].load(), s_truthMismatch[1].load(), s_truthUncreated[0].load(), s_truthUncreated[1].load(), final ? " final=1" : "");
}

// ---- the gate's in-run toggle (render-seam/rw1x/PREREG_gate.md; DESIGN_hybrid §13.6) ----
// GX_DRAWRUN_TOGGLE=k: render frames in periods of k, OFF / ON alternating (odd periods ON); the first GX_DRAWRUN_TOGGLE_DROP (3)
// frames of every period are dropped (frames in flight). GX_DRAWRUN_TOGGLE_WHAT:
//   s1 (default) - ON: S1_On() is true, WS-G replaces its claimed runs with Draw_Run; OFF: classic. Clause (d).
//   overhead     - S1 replaces the claimed runs in BOTH states; ON: every real run is preceded by one validate-only copy of
//                  ITSELF (same tasks and light sets) through the production transport + the fork's entry and validation,
//                  nothing drawn (Codex rw1x-design-r13 #2). Clause (a): per call = diff / validate-only calls per ON frame.
// GX_DRAWRUN_TOGGLE_NULL=1: the bookkeeping with BOTH states OFF (the null arm). Lines: [GXDRUNP] per pair (an OFF period and the ON
// period after it: diff = ON mean - OFF mean of the kept frames' periods) and [GXDRUNR] per 20 periods (rows close on an ON period,
// are published when the next OFF period closes, so the device thread has drained their work) - rw1x2/drt.py reads both.

unsigned long long Now_Ns()
{
	return (unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool Tog_On(unsigned frame)
{
	return s_togK > 0 && !s_togNull && ((frame / (unsigned)s_togK) & 1u) != 0;
}

void Tog_Account(unsigned prev, unsigned frame, double ms)
{
	const unsigned long long period = prev / (unsigned)s_togK;
	if ((int)(prev % (unsigned)s_togK) >= s_togDrop)
	{
		s_period.add(ms);
		((period & 1) ? s_on : s_off).add(ms);
		++((period & 1) ? s_keptOn : s_keptOff);
	}
	else
		++s_dropped;
	if (frame / (unsigned)s_togK == period)
		return;
	const unsigned logic = TheGameLogic != nullptr ? TheGameLogic->getFrame() : 0;
	if (s_period.n > 0)
	{
		const double mean = s_period.sum / s_period.n;
		if ((period & 1) == 0)
		{
			s_lastOffMean = mean;
			s_lastOffPeriod = period;
			s_lastOffLogic = logic;
		}
		else if (s_lastOffPeriod == period - 1)
		{
			s_pairs.add(mean - s_lastOffMean);
			fprintf(stderr, "[GXDRUNP] p=%llu diff_ms=%.5f on_ms=%.5f off_ms=%.5f logic=%u logic_off=%u\n", period, mean - s_lastOffMean,
				mean, s_lastOffMean, logic, s_lastOffLogic);
		}
	}
	s_period = Moments();
	if (s_rowPending && (period & 1) == 0)
	{
		s_rowPending = false;
		// the snapshot the row's boundary queued has executed by now (End_Record drains the ring every frame); if it somehow has
		// not, the row says so instead of publishing shifted counters
		const bool snapped = s_snapDone.load(std::memory_order_acquire);
		const unsigned long long fbOn = s_snapFbOn.load(), fbOff = s_snapFbOff.load(), vRef = s_snapVRef.load();
		const unsigned long long nsOn = s_snapNsOn.load(), nsOff = s_snapNsOff.load();
		if (!snapped)
			fprintf(stderr, "[GXDRUN] toggle row's device snapshot had not executed at publication - its device counters are invalid\n");
		fprintf(stderr, "%s runs_on=%llu runs_off=%llu tasks_on=%llu tasks_off=%llu vcalls_on=%llu vcalls_off=%llu fallback_on=%llu"
			" fallback_off=%llu vrefused=%llu on_frames=%llu dev_us_on=%.1f dev_us_off=%.1f\n", s_rowHead, s_rowRunsOn, s_rowRunsOff,
			s_rowTasksOn, s_rowTasksOff, s_rowVOn, s_rowVOff, fbOn - s_pubFbOn, fbOff - s_pubFbOff, vRef - s_pubVRef, s_rowFramesOn,
			(double)(nsOn - s_pubNsOn) / 1000.0, (double)(nsOff - s_pubNsOff) / 1000.0);
		s_pubFbOn = fbOn;
		s_pubFbOff = fbOff;
		s_pubVRef = vRef;
		s_pubNsOn = nsOn;
		s_pubNsOff = nsOff;
	}
	if (++s_togPeriods < 20 || (period & 1) == 0)
		return;
	s_togPeriods = 0;
	snprintf(s_rowHead, sizeof(s_rowHead), "[GXDRUNR] k=%d drop=%d null=%d what=%s n=1 logic=%u..%u frames=%u..%u off_n=%.0f off_ms=%.5f"
		" on_n=%.0f on_ms=%.5f pairs=%.0f diff_ms=%.5f gaps=%llu dropped=%llu", s_togK, s_togDrop, (int)s_togNull,
		s_togWhat ? "overhead" : "s1", s_winLogic0, logic, s_winFrame0, prev, s_off.n, s_off.n > 0 ? s_off.sum / s_off.n : 0.0,
		s_on.n, s_on.n > 0 ? s_on.sum / s_on.n : 0.0, s_pairs.n, s_pairs.n > 0 ? s_pairs.sum / s_pairs.n : 0.0, s_gaps, s_dropped);
	s_snapDone.store(false, std::memory_order_relaxed);
	DX8Wrapper::Device_Call(&Snap_Device_Counters, nullptr);	// in stream order: after every run queued before this boundary
	s_rowRunsOn = s_wRunsOn; s_rowRunsOff = s_wRunsOff; s_rowTasksOn = s_wTasksOn; s_rowTasksOff = s_wTasksOff;
	s_rowVOn = s_wVOn; s_rowVOff = s_wVOff; s_rowFramesOn = s_wFramesOn;
	s_rowPending = true;
	s_on = s_off = s_pairs = Moments();
	s_wRunsOn = s_wRunsOff = s_wTasksOn = s_wTasksOff = s_wVOn = s_wVOff = s_wFramesOn = 0;
	s_gaps = s_dropped = 0;
	s_winLogic0 = logic;
	s_winFrame0 = frame;
}

// Main thread, at most once per render frame: closes the previous frame's period (its length = between the two calls).
void Tog_Frame()
{
	if (s_togK <= 0)
		return;
	const unsigned frame = WW3D::Get_Frame_Count();
	if (frame == s_togFrame)
		return;
	const unsigned long long t = Now_Ns();
	if (s_togFrame != ~0u)
	{
		if (frame == s_togFrame + 1)
			Tog_Account(s_togFrame, frame, (double)(t - s_togT0) / 1.0e6);
		else
			++s_gaps;	// a frame without a call: no period for it (drt.py refuses a block with gaps)
	}
	s_togFrame = frame;
	s_togT0 = t;
	if (Tog_On(frame))
		++s_wFramesOn;
}

void Queue_Run(D3DPRIMITIVETYPE type, const Task *tasks, unsigned taskCount, const LightSet *sets, unsigned setCount, unsigned flags);

// ---- GX_HYBRID_DRAWRUN_TEST: the synthetic identity test (DESIGN_hybrid §13.7) ----
// One run of 12 lit cubes per frame at fixed screen positions (own VIEW = identity and PROJECTION, restored by a state block): three
// light sets (incl. a disabled middle slot and a point light), per-task ambient, scaled tasks with the NORMALIZENORMALS bracket, two
// base vertices and two start indices. =1 issues the calls through the d3d8 device (Issue_Classic), =2 through Draw_Run (the
// production transport). The two arms' pictures must be identical; GX_DRAWRUN_MUTANT=noamb must make =2 differ.
struct TestVertex { float x, y, z, nx, ny, nz; DWORD diffuse; };
const int TEST_TASKS = 12;
Task s_testTasks[TEST_TASKS];
LightSet s_testSets[3];
struct TestState { DWORD block; IDirect3DVertexBuffer8 *vb; IDirect3DIndexBuffer8 *ib; bool ok; } s_ts = { 0, nullptr, nullptr, false };
std::atomic<IDirect3DIndexBuffer8 *> s_tsIbPublished(nullptr);	// s_ts.ib once Test_Setup (device thread) created it - the main thread's
																	// Truth reads only this (Codex rw1x-design-r16 #4)
unsigned s_testFrame = ~0u;

void Make_Test_Data()
{
	for (int i = 0; i < TEST_TASKS; ++i)
	{
		Task &t = s_testTasks[i];
		memset(&t, 0, sizeof(t));
		const bool scaled = i == 3 || i == 4 || i == 9;
		const float s = scaled ? 0.24f : 0.15f;
		float a = 0.35f * (float)i;	// a per-task rotation about Y, then X
		const float cy = cosf(a), sy = sinf(a), cx = cosf(a * 0.7f), sx = sinf(a * 0.7f);
		// row-vector D3D convention: world = S * Ry * Rx * T
		const float r[3][3] = { { cy, sy * sx, -sy * cx }, { 0.0f, cx, sx }, { sy, -cy * sx, cy * cx } };
		float *m = &t.world._11;
		for (int row = 0; row < 3; ++row)
			for (int col = 0; col < 3; ++col)
				m[row * 4 + col] = s * r[row][col];
		m[12] = ((float)(i % 4) - 1.5f) * 0.6f;
		m[13] = ((float)(i / 4) - 1.0f) * 0.5f;
		m[14] = 2.5f + 0.1f * (float)(i % 3);
		m[15] = 1.0f;
		static const UINT sets[TEST_TASKS] = { 0, 0, 1, 2, 1, 1, 0, 2, 2, 1, 0, 0 };
		t.lightSet = sets[i];
		t.ambient = 0xFF000000u | (0x101010u * (DWORD)(2 + i));
		t.normalizeNormals = scaled ? 1u : 0u;
		t.baseVertex = (i & 1) ? 24 : 0;	// the second vertex copy has its own colours
		t.minIndex = 0;
		t.numVertices = 24;
		t.startIndex = (i & 2) ? 36 : 0;	// the second index copy is identical
		t.primCount = 12;
	}
	memset(s_testSets, 0, sizeof(s_testSets));
	D3DLIGHT8 &w = s_testSets[0].lights[0];
	w.Type = D3DLIGHT_DIRECTIONAL;
	w.Diffuse.r = w.Diffuse.g = w.Diffuse.b = 0.9f;
	w.Direction.x = 0.3f; w.Direction.y = -0.5f; w.Direction.z = 0.8f;
	s_testSets[0].enableMask = 1u;
	D3DLIGHT8 &rd = s_testSets[1].lights[0];
	rd.Type = D3DLIGHT_DIRECTIONAL;
	rd.Diffuse.r = 1.0f;
	rd.Direction.x = 1.0f; rd.Direction.z = 0.5f;
	D3DLIGHT8 &bl = s_testSets[1].lights[1];
	bl.Type = D3DLIGHT_DIRECTIONAL;
	bl.Diffuse.b = 1.0f;
	bl.Direction.y = -1.0f; bl.Direction.z = 0.5f;
	s_testSets[1].enableMask = 3u;
	D3DLIGHT8 &pt = s_testSets[2].lights[0];
	pt.Type = D3DLIGHT_POINT;
	pt.Diffuse.g = 1.0f;
	pt.Position.z = 1.5f;
	pt.Range = 5.0f;
	pt.Attenuation0 = 1.0f;
	s_testSets[2].lights[1] = w;	// a disabled middle slot: its value must not be read
	D3DLIGHT8 &w2 = s_testSets[2].lights[2];
	w2 = w;
	w2.Direction.x = -0.6f;
	s_testSets[2].enableMask = 5u;
}

void Test_Setup(IDirect3DDevice8 *dev, void *)
{
	s_ts.ok = false;
	s_testSetupOk = false;
	if (s_ts.vb == nullptr || s_ts.ib == nullptr)
	{
		static const float n[6][3] = { { 0, 0, -1 }, { 0, 0, 1 }, { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 } };
		TestVertex v[48];
		WORD idx[72];
		for (int copy = 0; copy < 2; ++copy)
			for (int f = 0; f < 6; ++f)
			{
				// the face's two tangent axes
				const int ax = n[f][0] != 0 ? 1 : 0, ay = n[f][2] != 0 ? 1 : 2;
				for (int k = 0; k < 4; ++k)
				{
					float p[3] = { n[f][0], n[f][1], n[f][2] };
					p[ax] = (k & 1) ? 1.0f : -1.0f;
					p[ay] = (k & 2) ? 1.0f : -1.0f;
					TestVertex &tv = v[copy * 24 + f * 4 + k];
					tv.x = p[0]; tv.y = p[1]; tv.z = p[2];
					tv.nx = n[f][0]; tv.ny = n[f][1]; tv.nz = n[f][2];
					tv.diffuse = copy == 0 ? 0xFFE0E0E0u : 0xFFF0C060u;
				}
			}
		for (int f = 0; f < 6; ++f)
		{
			const WORD b = (WORD)(f * 4);
			const WORD q[6] = { b, (WORD)(b + 1), (WORD)(b + 2), (WORD)(b + 2), (WORD)(b + 1), (WORD)(b + 3) };
			memcpy(&idx[f * 6], q, sizeof(q));
			memcpy(&idx[36 + f * 6], q, sizeof(q));
		}
		// all or nothing: both buffers created AND filled, else neither is kept and the frame's setup fails (Codex rw1x2-code-r3)
		BYTE *pv = nullptr, *pi = nullptr;
		bool ok = SUCCEEDED(dev->CreateVertexBuffer(sizeof(v), D3DUSAGE_WRITEONLY, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE,
				D3DPOOL_MANAGED, &s_ts.vb))
			&& SUCCEEDED(dev->CreateIndexBuffer(sizeof(idx), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &s_ts.ib));
		if (ok && (ok = SUCCEEDED(s_ts.vb->Lock(0, sizeof(v), &pv, 0))))
		{
			memcpy(pv, v, sizeof(v));
			s_ts.vb->Unlock();
		}
		if (ok && (ok = SUCCEEDED(s_ts.ib->Lock(0, sizeof(idx), &pi, 0))))
		{
			memcpy(pi, idx, sizeof(idx));
			s_ts.ib->Unlock();
		}
		if (!ok)
		{
			if (s_ts.vb != nullptr)
				s_ts.vb->Release();
			if (s_ts.ib != nullptr)
				s_ts.ib->Release();
			s_ts.vb = nullptr;
			s_ts.ib = nullptr;
			return;
		}
	}
	if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &s_ts.block)))	// GX-WRAPSKIP-OK: test bracket: Test_Setup's D3DSBT_ALL block, applied by Test_Restore
		return;
	D3DMATRIX id, proj;
	memset(&id, 0, sizeof(id));
	id._11 = id._22 = id._33 = id._44 = 1.0f;
	memset(&proj, 0, sizeof(proj));
	const float zn = 1.0f, zf = 1000.0f, yScale = 1.0f / tanf(0.5236f), xScale = yScale * 9.0f / 16.0f;
	proj._11 = xScale; proj._22 = yScale; proj._33 = zf / (zf - zn); proj._34 = 1.0f; proj._43 = -zn * zf / (zf - zn);
	dev->SetTransform(D3DTS_VIEW, &id);	// GX-WRAPSKIP-OK: test bracket: Test_Setup's D3DSBT_ALL block, applied by Test_Restore
	dev->SetTransform(D3DTS_PROJECTION, &proj);
	dev->SetVertexShader(D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE);
	dev->SetPixelShader(0);
	dev->SetStreamSource(0, s_ts.vb, sizeof(TestVertex));
	dev->SetIndices(s_ts.ib, 0);
	// ALL eight stages are the test's own: DXVK uploads every stage's texture matrix into the FF VS constants whether the stage is used
	// or not, and the game's (UV-animated) matrices left over from its own draws made every frame's digest unique (Mini M3d4: only
	// VSFixedFunction moved)
	for (DWORD st = 0; st < 8; ++st)
	{
		dev->SetTexture(st, nullptr);
		dev->SetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + st), &id);
		dev->SetTextureStageState(st, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
		dev->SetTextureStageState(st, D3DTSS_TEXCOORDINDEX, st);
		if (st > 0)
		{
			dev->SetTextureStageState(st, D3DTSS_COLOROP, D3DTOP_DISABLE);
			dev->SetTextureStageState(st, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
		}
	}
	dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
	dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
	D3DMATERIAL8 mat;
	memset(&mat, 0, sizeof(mat));
	mat.Diffuse.r = mat.Diffuse.g = mat.Diffuse.b = mat.Diffuse.a = 1.0f;
	mat.Ambient.r = mat.Ambient.g = mat.Ambient.b = mat.Ambient.a = 1.0f;
	dev->SetMaterial(&mat);	// GX-WRAPSKIP-OK: test bracket: Test_Setup's D3DSBT_ALL block, applied by Test_Restore
	const DWORD rs[][2] = {
		{ D3DRS_ZENABLE, TRUE }, { D3DRS_ZFUNC, D3DCMP_LESSEQUAL }, { D3DRS_ZWRITEENABLE, TRUE }, { D3DRS_LIGHTING, TRUE },
		{ D3DRS_COLORVERTEX, TRUE }, { D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1 }, { D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL },
		{ D3DRS_SPECULARMATERIALSOURCE, D3DMCS_MATERIAL }, { D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL },
		{ D3DRS_SPECULARENABLE, FALSE }, { D3DRS_NORMALIZENORMALS, FALSE }, { D3DRS_LOCALVIEWER, FALSE },
		{ D3DRS_CULLMODE, D3DCULL_NONE }, { D3DRS_ALPHABLENDENABLE, FALSE }, { D3DRS_ALPHATESTENABLE, FALSE },
		// the cubes write stencil (0x5A where they pass), so C7's stencil readback and its STENCILREF fail arm see them
		{ D3DRS_STENCILENABLE, TRUE }, { D3DRS_STENCILFUNC, D3DCMP_ALWAYS }, { D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE },
		{ D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP }, { D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP }, { D3DRS_STENCILREF, 0x5A },
		{ D3DRS_STENCILMASK, 0xFF }, { D3DRS_STENCILWRITEMASK, 0xFF }, { D3DRS_FOGENABLE, FALSE }, { D3DRS_COLORWRITEENABLE, 0xF }, { D3DRS_FILLMODE, D3DFILL_SOLID },
		{ D3DRS_ZBIAS, 0 }, { D3DRS_CLIPPLANEENABLE, 0 }, { D3DRS_VERTEXBLEND, D3DVBF_DISABLE },
		// everything DXVK pushes or uploads for this draw is the test's own, so the digest of a bracket depends on the test alone
		// (fog colour/scale/end/density, the alpha reference and point sizes are DXVK push constants; the texture factor is FF PS data)
		{ D3DRS_FOGCOLOR, 0 }, { D3DRS_FOGSTART, 0 }, { D3DRS_FOGEND, 0x3F800000 }, { D3DRS_FOGDENSITY, 0x3F800000 },
		{ D3DRS_FOGTABLEMODE, D3DFOG_NONE }, { D3DRS_FOGVERTEXMODE, D3DFOG_NONE }, { D3DRS_RANGEFOGENABLE, FALSE },
		{ D3DRS_ALPHAREF, 0 }, { D3DRS_ALPHAFUNC, D3DCMP_ALWAYS }, { D3DRS_POINTSIZE, 0x3F800000 }, { D3DRS_POINTSIZE_MIN, 0x3F800000 },
		{ D3DRS_POINTSCALEENABLE, FALSE }, { D3DRS_TEXTUREFACTOR, 0xFFFFFFFF }, { D3DRS_DITHERENABLE, FALSE },
		{ D3DRS_TWEENFACTOR, 0 } };	// the FF VS constants carry the tween factor too
	for (size_t i = 0; i < sizeof(rs) / sizeof(rs[0]); ++i)
		dev->SetRenderState((D3DRENDERSTATETYPE)rs[i][0], rs[i][1]);
	s_ts.ok = true;
	s_testSetupOk = true;
	s_tsIbPublished.store(s_ts.ib, std::memory_order_release);
}

// The test's model of the seat's CARRY-OUT on the device: DrawRun and its fallback leave the base vertex at PREPARE's (0); the
// carry-out's Apply moves it to the last task's. ctx: 0 = the carry-out, 1 = none (mutant nocarry / nocarryib), 2 = the PENULTIMATE
// task applied coherently instead (mutant wrongtask: world, ambient, lights and base vertex of task n-2).
void Test_Carry(IDirect3DDevice8 *dev, void *ctx)
{
	const intptr_t how = (intptr_t)ctx;
	if (!s_ts.ok || how == 1)
		return;
	if (how == 3)	// mutant nntrue: the correct carry-out, then NORMALIZENORMALS left TRUE (coherently - the belief says TRUE too)
		dev->SetRenderState(D3DRS_NORMALIZENORMALS, TRUE);
	const Task &t = s_testTasks[how == 2 ? TEST_TASKS - 2 : TEST_TASKS - 1];
	if (how == 2)
	{
		const LightSet &ls = s_testSets[t.lightSet];
		for (DWORD l = 0; l < 4; ++l)
		{
			if (ls.enableMask & (1u << l))
				dev->SetLight(l, &ls.lights[l]);	// GX-WRAPSKIP-OK: test bracket: Test_Setup's D3DSBT_ALL block, applied by Test_Restore
			dev->LightEnable(l, (ls.enableMask >> l) & 1u);	// GX-WRAPSKIP-OK: test bracket: Test_Setup's D3DSBT_ALL block, applied by Test_Restore
		}
		dev->SetTransform(D3DTS_WORLD, &t.world);	// GX-WRAPSKIP-OK: test bracket: Test_Setup's D3DSBT_ALL block, applied by Test_Restore
		dev->SetRenderState(D3DRS_AMBIENT, t.ambient);
	}
	dev->SetIndices(s_ts.ib, (UINT)t.baseVertex);
}

void Test_Classic(IDirect3DDevice8 *dev, void *)
{
	if (!s_ts.ok)
		return;
	Mark_On_Device(dev, true, 0);
	Issue_Classic(dev, D3DPT_TRIANGLELIST, s_testTasks, TEST_TASKS, s_testSets, 3);
	Mark_On_Device(dev, false, 0);	// path 0 = classic, run 0
}

void Test_Restore(IDirect3DDevice8 *dev, void *)
{
	if (s_ts.block != 0)
	{
		dev->ApplyStateBlock(s_ts.block);	// GX-WRAPSKIP-OK: test bracket: Test_Setup's D3DSBT_ALL block, applied by Test_Restore
		dev->DeleteStateBlock(s_ts.block);
		s_ts.block = 0;
	}
}

// DX8Wrapper::Pre_Release_Hook (main thread, ring drained, before the device is released): the test's buffers would otherwise keep
// DXVK's device alive past the globals its threads read.
void (*s_prevTestRelease)() = nullptr;
void Test_Release()
{
	if (s_ts.vb != nullptr)
		s_ts.vb->Release();
	if (s_ts.ib != nullptr)
		s_ts.ib->Release();
	s_ts.vb = nullptr;
	s_ts.ib = nullptr;
	if (s_prevTestRelease != nullptr)
		s_prevTestRelease();
}

// DX8MeshRendererClass::GxBoundary (installed only by the test, only when nothing else holds it): the frame's first boundary of an
// object flush inside the ring's bracket (not PROJ_SHADOW) - so =2 exercises the ring transport.
void Test_Boundary(int, unsigned long long)
{
	if (GxFlushSite == GXFS_UNSET || GxFlushSite == GXFS_PROJ_SHADOW)
		return;
	const unsigned frame = WW3D::Get_Frame_Count();
	if (frame == s_testFrame)
		return;
	const int mode = s_test == 3 ? ((frame & 1u) ? 2 : 1) : s_test;
	if (s_test >= 2 && !Available())
		return;
	s_testFrame = frame;
	DX8Wrapper::Device_Call(&Test_Setup, nullptr);
	if (mode == 2)
		Draw_Run(D3DPT_TRIANGLELIST, s_testTasks, TEST_TASKS, s_testSets, 3);
	else
		DX8Wrapper::Device_Call(&Test_Classic, nullptr);
	IDirect3DIndexBuffer8 *const tib = s_tsIbPublished.load(std::memory_order_acquire);
	if (Truth_On() && tib != nullptr)
	{
		// The seat's carry-out, modelled: both paths leave the LAST task's world / ambient / lights and NORMALIZENORMALS FALSE; the
		// carry-out moves the base vertex to the last task's. The wrapper belief is what a seat's caches would hold: the mutants are
		// a seat that skipped the carry-out (belief = PREPARE's = the first task's values, no device call), one that moved the base
		// vertex without the setter (belief = the last base vertex, never sent) and one that applied the PENULTIMATE task coherently
		// (device and belief agree - only the oracle, the run's own last task, sees it)
		const Task &last = s_testTasks[TEST_TASKS - 1], &first = s_testTasks[0], &pen = s_testTasks[TEST_TASKS - 2];
		DX8Wrapper::Device_Call(&Test_Carry, (void *)(intptr_t)(s_mutantTruth == 1 || s_mutantTruth == 2 ? 1 : s_mutantTruth == 3 ? 2
			: s_mutantTruth == 4 ? 3 : 0));
		const Task &w = s_mutantTruth == 1 ? first : s_mutantTruth == 3 ? pen : last;
		Truth t;
		memset(&t, 0, sizeof(t));
		t.ib = tib;
		t.baseVertex = s_mutantTruth == 1 ? 0u : (UINT)w.baseVertex;
		t.world = w.world;
		t.ambient = w.ambient;
		t.normalizeNormals = s_mutantTruth == 4 ? TRUE : FALSE;
		t.lightEnableMask = s_testSets[w.lightSet].enableMask;
		memcpy(t.lights, s_testSets[w.lightSet].lights, sizeof(t.lights));
		t.tag = (unsigned long long)(mode == 2 ? 1u : 0u) << 32;
		t.hasOracle = 1;
		t.runIb = tib;
		t.last = last;
		t.lastSet = s_testSets[last.lightSet];
		t.anyScaled = 0;
		for (int i = 0; i < TEST_TASKS; ++i)
			t.anyScaled |= s_testTasks[i].normalizeNormals;
		Check_Truth(t);
	}
	DX8Wrapper::Device_Call(&Test_Restore, nullptr);
	if (frame - s_lastPrint >= 300)
	{
		s_lastPrint = frame;
		fprintf(stderr, "[GXDRUNT] f=%u mode=%d tasks=%d\n", frame, s_test, TEST_TASKS);
		Print_Stats(frame);
	}
}

} // namespace

bool Enabled()
{
	static int s_on = -1;
	if (s_on < 0)
	{
		const char *e = getenv("GX_L2_DRAWRUN");
		const char *t = GxTestEnv("GX_HYBRID_DRAWRUN_TEST");
		const char *w = GxTestEnv("GX_DRAWRUN_TOGGLE_WHAT");
		s_on = (e != nullptr && atoi(e) != 0) || (t != nullptr && atoi(t) >= 2) || (w != nullptr && strcmp(w, "overhead") == 0) ? 1 : 0;
	}
	return s_on != 0;
}

bool Available()
{
	if (!Enabled())
		return false;
	int st = s_init.load(std::memory_order_acquire);
	if (st == INIT_NONE)
	{
		Parse_Options();
		s_init.store(INIT_QUEUED);
		DX8Wrapper::Device_Call(&Device_Init, nullptr);	// in stream order; it may already have run (outside the bracket it runs inline)
		st = s_init.load(std::memory_order_acquire);
	}
	return st == INIT_READY;
}

void Draw_Run(D3DPRIMITIVETYPE type, const Task *tasks, unsigned taskCount, const LightSet *sets, unsigned setCount)
{
	if (taskCount == 0)
		return;
	Queue_Run(type, tasks, taskCount, sets, setCount, 0);
	if (s_test == 0)
	{
		const unsigned frame = WW3D::Get_Frame_Count();
		if (frame - s_lastPrint >= 300)
		{
			s_lastPrint = frame;
			Print_Stats(frame);
		}
	}
}

namespace
{
void Queue_Run(D3DPRIMITIVETYPE type, const Task *tasks, unsigned taskCount, const LightSet *sets, unsigned setCount, unsigned flags)
{
	const size_t bytes = sizeof(RunHeader) + sizeof(Task) * taskCount + sizeof(LightSet) * setCount;
	RunHeader *h = (RunHeader *)Arena_Take(bytes);
	const bool on = Tog_On(WW3D::Get_Frame_Count());
	h->type = type;
	h->taskCount = taskCount;
	h->setCount = setCount;
	h->runIndex = flags ? ~0u : s_runInFrame++;
	h->flags = flags ? DXVK_GX_RUN_VALIDATE_ONLY_GAME : 0u;
	h->onPeriod = on ? 1u : 0u;
	memcpy(h + 1, tasks, sizeof(Task) * taskCount);
	if (setCount)
		memcpy((Task *)(h + 1) + taskCount, sets, sizeof(LightSet) * setCount);
	if (flags)
		++(on ? s_wVOn : s_wVOff);
	else
	{
		// overhead mode: the run's own validate-only copy goes first, in ON periods only (it draws nothing)
		if (s_togWhat == 1 && on)
			Queue_Run(type, tasks, taskCount, sets, setCount, DXVK_GX_RUN_VALIDATE_ONLY_GAME);
		++s_runs;
		s_tasks += taskCount;
		for (unsigned i = 0; i < taskCount; ++i)
			s_badSets += tasks[i].lightSet >= setCount;	// the fork refuses such a run and the fallback skips its lights: counted, never silent
		++(on ? s_wRunsOn : s_wRunsOff);
		(on ? s_wTasksOn : s_wTasksOff) += taskCount;
	}
	DX8Wrapper::Device_Call(&Run_On_Device, h);
}
} // namespace

bool S1_On()
{
	if (!Available())
		return false;
	Tog_Frame();
	// the overhead arm replaces in both states (it toggles only the validate-only copies)
	return s_togK <= 0 || s_togWhat == 1 || Tog_On(WW3D::Get_Frame_Count());
}

bool Truth_On()
{
	static int s_on = -1;
	if (s_on < 0)
	{
		const char *e = GxTestEnv("GX_DRAWRUN_TRUTH");
		s_on = e != nullptr && atoi(e) == 1 ? 1 : 0;
	}
	return s_on == 1;
}

void Check_Truth(const Truth &t)
{
	if (!Truth_On())
		return;
	Truth *c = (Truth *)Arena_Take(sizeof(Truth));
	memcpy(c, &t, sizeof(Truth));
	DX8Wrapper::Device_Call(&Truth_Call, c);
}

void Digest_Mark(bool begin, unsigned long long tag)
{
	MarkPacket *m = (MarkPacket *)Arena_Take(sizeof(MarkPacket));
	// bits 40-63 carry the render frame (mod 2^24) for every caller, so an analyzer proves coverage per (frame, run index) instead of
	// inferring frames from run index 0 (Codex rw1x-design-r18 #1); the caller's path (bits 32-39) and run index (0-31) are kept
	m->tag = (tag & 0xFFFFFFFFFFull) | ((unsigned long long)(WW3D::Get_Frame_Count() & 0xFFFFFFu) << 40);
	m->begin = begin ? 1 : 0;
	DX8Wrapper::Device_Call(&Mark_Call, m);
}

void Install()
{
	Parse_Options();
	if (s_togK > 0)
		fprintf(stderr, "[GXDRUN] toggle armed: k=%d drop=%d null=%d what=%s\n", s_togK, s_togDrop, (int)s_togNull,
			s_togWhat ? "overhead" : "s1");
	if (Enabled() || s_test != 0 || Truth_On())
		Chain_Release_Hook();	// the final=1 record comes from its hook
	if (s_test == 0)
		return;
	Make_Test_Data();
	if (DX8MeshRendererClass::GxBoundary != nullptr)
	{
		fprintf(stderr, "[GXDRUN] GX_HYBRID_DRAWRUN_TEST refused: the flush boundary hook is taken (GX_RENDER_HYBRID set?) - run it alone\n");
		s_test = 0;
		return;
	}
	DX8MeshRendererClass::GxBoundary = &Test_Boundary;
	s_prevTestRelease = DX8Wrapper::Pre_Release_Hook;
	DX8Wrapper::Pre_Release_Hook = &Test_Release;
	fprintf(stderr, "[GXDRUN] GX_HYBRID_DRAWRUN_TEST=%d installed: %d tasks per frame, %s\n", s_test, TEST_TASKS,
		s_test == 3 ? "alternating: even frames through the d3d8 device, odd frames through Draw_Run"
		: s_test == 2 ? "through Draw_Run (the fork's DrawRun, its fallback on refusal)" : "through the d3d8 device");
}

} // namespace GxDrawRun
