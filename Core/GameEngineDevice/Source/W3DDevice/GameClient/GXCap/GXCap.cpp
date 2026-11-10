// GeneralsX: GXCAP game glue (GXCap.h; recorder core GXCapRecorder.cpp).
// GXCAP=<frame>[,...] (1-16 ascending), GXCAP_SPAN=<updates> (1-16, default 3), GXCAP_DIR=<dir> (default ./gxcap).
// A window opens at the first in-match draw whose logic frame reaches the next GXCAP frame. It closes at the first draw
// entry after GXCAP_SPAN updates AND, when GX_SHOT is set, after a screenshot was recorded inside it (DXVK rotates two
// windowed back buffers plus the front one, so a shot taken right after Present reads the image rendered two updates
// earlier: put GX_SHOT at least two frames after the GXCAP frame) - or after 16 updates. Refused with GX_RENDER_HYBRID
// (L1 capture is CLASSIC-only). Unset: Install returns after one getenv; Armed stays false.
#include "W3DDevice/GameClient/GXCap.h"

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32

namespace GxCap
{
bool Armed = false;
void Install(const char *)
{
	if (getenv("GXCAP")) { fprintf(stderr, "[GXCAP] GXCAP ignored: the recorder is not built on Windows yet\n"); fflush(stderr); }
}
void Draw_Begin(unsigned, bool) {}
void Shot_Begin(unsigned, bool, const char *) {}
void Shot_End(bool) {}
}

#else

#include "WW3D2/dx8wrapper.h"
#include "GXCapRecorder.h"

#include <string>
#include <vector>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace
{
std::vector<unsigned> s_frames;
size_t s_next = 0;
unsigned s_span = 3;
unsigned s_updates = 0;
bool s_shotArmed = false;
const unsigned MAX_UPDATES = 16;

IDirect3DDevice8 *Wrap_Hook(IDirect3DDevice8 *device, UINT adapter, D3DDEVTYPE type, DWORD behavior, const D3DPRESENT_PARAMETERS *pp)
{
	IDirect3DDevice8 *w = gxcap::Rec_Wrap_Device(device, adapter, type, behavior, pp);
	GxCap::Armed = w != device;
	return w;
}

bool Parse_Frames(const char *p, std::vector<unsigned> &out, const char *&why)
{
	while (*p != 0) {
		if (*p < '0' || *p > '9') { why = "not a comma-separated list of decimal frames"; return false; }
		unsigned long long v = 0;
		while (*p >= '0' && *p <= '9') {
			v = v * 10 + unsigned(*p - '0');
			if (v > 0xFFFFFFFFull) { why = "frame too large"; return false; }
			++p;
		}
		if (!out.empty() && v <= out.back()) { why = "frames must ascend"; return false; }
		out.push_back(unsigned(v));
		if (*p == ',') {
			++p;
			if (*p == 0) { why = "trailing comma"; return false; }
		} else if (*p != 0) {
			why = "not a comma-separated list of decimal frames";
			return false;
		}
	}
	if (out.empty() || out.size() > 16) { why = "1-16 frames"; return false; }
	return true;
}
}

namespace GxCap
{
bool Armed = false;

void Install(const char *buildSha)
{
	const char *env = getenv("GXCAP");
	if (env == nullptr)
		return;
	const char *why = nullptr;
	const char *hybrid = getenv("GX_RENDER_HYBRID");
	if (hybrid != nullptr && *hybrid != 0 && *hybrid != '0')
		why = "GX_RENDER_HYBRID is set: the capture is CLASSIC-only";
	else if (!Parse_Frames(env, s_frames, why))
		;
	else if (const char *s = getenv("GXCAP_SPAN")) {
		const int v = atoi(s);
		if (v < 1 || v > int(MAX_UPDATES))
			why = "GXCAP_SPAN must be 1-16";
		else
			s_span = unsigned(v);
	}
	const char *dir = getenv("GXCAP_DIR");
	std::string def = "gxcap";
#if defined(__APPLE__) && TARGET_OS_IPHONE
	// iPadOS runs from inside the read-only app bundle (SDL3Main changes to <bundle>/GameData): default to the app's
	// Documents directory; GXCAP_DIR still overrides
	if (const char *home = getenv("HOME"))
		def = std::string(home) + "/Documents/gxcap";
#endif
	if (why == nullptr && !gxcap::Rec_Configure((dir != nullptr && *dir != 0) ? dir : def, buildSha != nullptr ? buildSha : ""))
		why = "cannot create GXCAP_DIR";
	if (why != nullptr) {
		s_frames.clear();
		fprintf(stderr, "[GXCAP] GXCAP='%s' refused (%s) - recorder off\n", env, why);
		fflush(stderr);
		return;
	}
	s_shotArmed = getenv("GX_SHOT") != nullptr;
	DX8Wrapper::Device_Wrap_Hook = &Wrap_Hook;
	fprintf(stderr, "[GXCAP] armed: %zu frame(s) from GXCAP='%s', span %u%s, dir %s\n", s_frames.size(), env, s_span,
		s_shotArmed ? " (+ until a GX_SHOT inside the window)" : "", (dir != nullptr && *dir != 0) ? dir : "gxcap");
	fflush(stderr);
}

void Draw_Begin(unsigned logicFrame, bool inMatch)
{
	if (!gxcap::Rec_Has_Device()) {
		Armed = false;
		return;
	}
	if (gxcap::Rec_Is_Open()) {
		++s_updates;
		const bool done = s_updates >= s_span && (!s_shotArmed || gxcap::Rec_Shot_Seen());
		if (done || s_updates >= MAX_UPDATES) {
			DX8Wrapper::_Get_D3D_Device8();	// the d3d8 ring drains (its render thread) before the recorder takes its lock
			gxcap::Rec_Close(false);
		}
	}
	if (!gxcap::Rec_Is_Open() && s_next < s_frames.size() && inMatch && logicFrame >= s_frames[s_next]) {
		DX8Wrapper::_Get_D3D_Device8();
		gxcap::Rec_Open(s_frames[s_next], logicFrame, s_span);
		s_updates = 0;
		++s_next;
	}
}

void Shot_Begin(unsigned logicFrame, bool requested, const char *name)
{
	if (Armed)
		gxcap::Rec_Shot_Begin(logicFrame, requested, name);
}

void Shot_End(bool completed)
{
	if (Armed)
		gxcap::Rec_Shot_End(completed);
}
}

#endif
